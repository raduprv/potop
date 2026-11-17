// Water rendering. See water_renderer.h.

#include "lights.h"
#include "water_renderer.h"
#include "profile.h"
#include "clouds.h"
#include "water_gpu.h"

#include <SDL3/SDL.h>

// water_block in data/shaders/water.vert and water.frag (std140)
typedef struct
{
	int32_t chunk_origin[2];
	int32_t map_size;
	float cell_size;
	float thin_highlight[4];
	float shallow_color[3];
	float clarity_meters;
	float deep_color[3];
	float opacity_per_meter;
	float mud_color[3];
	float foam_speed;
	int32_t tile_overlay;
	float step_blend;
	int32_t reflections;           // 1: the scene's copy is bound (screen space reflections)
	int32_t dome;                  // 0: no volumetric clouds to mirror, 1: their dome of the whole sky, 2: of its upper half
	// The scene's depth (0..1) back to the distance along the view:
	// distance = y / (depth - x); z, w: 1 / the copy's size
	float depth_to_distance[4];
	float ice[4];                  // x 1: the ice is bound (water_ice.h); y: the ripples' choppiness; z: the ripples in colors (Debug)
	// The land and the trees mirrored out to x meters from the eye (the
	// scope's zoom farther), fading out over the last y; beyond, the sky
	// alone (no march: far water is a few pixels of shimmer)
	float reflection_range[4];
} water_uniforms_t;

// OpenGL's perspective (math3d.h), its depth taken to 0..1
// (gpu_clip_matrix): depth = x + y / distance; z, w: 1 / the copy's size
static void depth_to_distance(const water_renderer_t *renderer, const camera_t *camera, float out[4])
{
	out[0] = 0.5f - 0.5f * camera->projection.m[10];
	out[1] = 0.5f * camera->projection.m[14];
	out[2] = 1.0f / (float)SDL_max(renderer->scene_width, 1);
	out[3] = 1.0f / (float)SDL_max(renderer->scene_height, 1);
}

static float srgb_to_linear(float value)
{
	return SDL_powf(value, 2.2f);
}

// Bits: 1 depth test, 2 depth write, 4 cull back faces, 8 wireframe, 16
// the reflections apart (two more color targets)
static SDL_GPUGraphicsPipeline *water_pipeline(water_renderer_t *renderer, int bits)
{
	// The chunk grid's vertices, and each drawn chunk's origin (per instance)
	static const SDL_GPUVertexBufferDescription chunk_buffers[2] =
	{
		{ 0, sizeof(float) * 2, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
		{ 1, sizeof(int32_t) * 2, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
	};
	static const SDL_GPUVertexAttribute chunk_attributes[2] =
	{
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
		{ 1, 1, SDL_GPU_VERTEXELEMENTFORMAT_INT2, 0 },
	};
	SDL_GPUShader *vertex;
	SDL_GPUShader *fragment;
	gpu_pipeline_info_t info;

	if (renderer->pipelines[bits] != NULL || renderer->pipeline_failed[bits])
	{
		return renderer->pipelines[bits];
	}
	vertex = gpu_load_shader("water.vert", 3, 0, 2);
	fragment = gpu_load_shader((bits & 16) != 0 ? "water_split.frag" : "water.frag", 12, LIGHTS_STORAGE_BUFFERS, 2);
	if (vertex != NULL && fragment != NULL)
	{
		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = chunk_buffers;
		info.vertex_buffer_count = 2;
		info.vertex_attributes = chunk_attributes;
		info.vertex_attribute_count = 2;
		info.depth_test = (bits & 1) != 0;
		info.depth_write = (bits & 2) != 0;
		info.cull = (bits & 4) != 0 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
		info.fill = (bits & 8) != 0 ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
		info.blend = GPU_BLEND_ALPHA;
		if ((bits & 16) != 0)
		{
			info.extra_color_target_count = 2;
			info.extra_color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		}
		renderer->pipelines[bits] = gpu_make_pipeline(&info, (bits & 16) != 0 ? "water, reflections apart" : "water");
	}
	SDL_ReleaseGPUShader(gpu.device, vertex);
	SDL_ReleaseGPUShader(gpu.device, fragment);
	renderer->pipeline_failed[bits] = renderer->pipelines[bits] == NULL;
	return renderer->pipelines[bits];
}

// The ripples' tile: this many texels a side, and the slopes' range stored
// (in units of their spread: SNORM 1 is this many)
#define RIPPLE_SIZE 256
#define RIPPLE_SLOPE_RANGE 4.0f

static uint32_t ripple_hash(uint32_t index, uint32_t seed)
{
	uint32_t mixed = index * 1597334673u ^ seed * 2654435761u;

	mixed ^= mixed >> 15;
	mixed *= 2246822519u;
	mixed ^= mixed >> 13;
	return mixed;
}

static float ripple_random(uint32_t index, uint32_t seed)
{
	return (float)(ripple_hash(index, seed) >> 8) / 16777216.0f;
}
// The small ripples: a height of many waves, each its own way, length and
// phase (whole numbers of them across the tile, so it repeats), from 6 to
// 32 a tile (fewer than 6: rounded to a whole number, their ways were few,
// the diagonals among them, and the tile got a grain), turned by the golden
// angle one from the next (every way about as much), the shorter lower
// and less steep; each a little sharpened at its crest
// (exp(0.6 (sin - 1)): narrower crests, wider troughs, as choppy water;
// sharper, every crest caught the glint as a hairline). Value noise showed its
// lattice (diamonds); waves have none. Its slopes (in heights a tile)
// scaled so their spread is 1, with the levels of detail averaged here
// (SNORM cannot be blitted everywhere)
static SDL_GPUTexture *make_ripple_texture(void)
{
	enum { WAVE_COUNT = 96 };
	float wave_x[WAVE_COUNT];
	float wave_y[WAVE_COUNT];
	float wave_phase[WAVE_COUNT];
	float wave_height[WAVE_COUNT];
	uint32_t levels = gpu_mip_count(RIPPLE_SIZE, RIPPLE_SIZE);
	float *height = SDL_malloc(sizeof(float) * RIPPLE_SIZE * RIPPLE_SIZE);
	float *slope = SDL_malloc(sizeof(float) * 4 * RIPPLE_SIZE * RIPPLE_SIZE);
	int16_t *texels = SDL_malloc(sizeof(int16_t) * 4 * RIPPLE_SIZE * RIPPLE_SIZE);
	SDL_GPUTexture *texture = NULL;
	double squared_sum = 0.0;
	float scale;

	if (height == NULL || slope == NULL || texels == NULL)
	{
		goto done;
	}
	texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_SNORM, RIPPLE_SIZE, RIPPLE_SIZE, 1, levels,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
	if (texture == NULL)
	{
		goto done;
	}
	for (int i = 0; i < WAVE_COUNT; i++)
	{
		float waves = 6.0f * SDL_powf(5.333f, (float)i / (WAVE_COUNT - 1));
		float angle = 2.39996323f * (float)i + 0.6f * ripple_random((uint32_t)i, 1u);
		float across_x = SDL_roundf(waves * SDL_cosf(angle));
		float across_y = SDL_roundf(waves * SDL_sinf(angle));

		if (across_x == 0.0f && across_y == 0.0f)
		{
			across_x = 1.0f;
		}
		wave_x[i] = 6.2831853f * across_x;
		wave_y[i] = 6.2831853f * across_y;
		wave_phase[i] = 6.2831853f * ripple_random((uint32_t)i, 2u);
		wave_height[i] = SDL_powf(SDL_sqrtf(across_x * across_x + across_y * across_y), -1.6f);
	}
	for (int y = 0; y < RIPPLE_SIZE; y++)
	{
		for (int x = 0; x < RIPPLE_SIZE; x++)
		{
			float u = (float)x / RIPPLE_SIZE;
			float v = (float)y / RIPPLE_SIZE;
			float sum = 0.0f;

			for (int i = 0; i < WAVE_COUNT; i++)
			{
				sum += wave_height[i] * SDL_expf(0.6f * (SDL_sinf(wave_x[i] * u + wave_y[i] * v + wave_phase[i]) - 1.0f));
			}
			height[y * RIPPLE_SIZE + x] = sum;
		}
	}
	for (int y = 0; y < RIPPLE_SIZE; y++)
	{
		for (int x = 0; x < RIPPLE_SIZE; x++)
		{
			int left = (x + RIPPLE_SIZE - 1) % RIPPLE_SIZE;
			int right = (x + 1) % RIPPLE_SIZE;
			int back = (y + RIPPLE_SIZE - 1) % RIPPLE_SIZE;
			int front = (y + 1) % RIPPLE_SIZE;
			float slope_x = (height[y * RIPPLE_SIZE + right] - height[y * RIPPLE_SIZE + left]) * 0.5f * RIPPLE_SIZE;
			float slope_y = (height[front * RIPPLE_SIZE + x] - height[back * RIPPLE_SIZE + x]) * 0.5f * RIPPLE_SIZE;

			slope[(y * RIPPLE_SIZE + x) * 4] = slope_x;
			slope[(y * RIPPLE_SIZE + x) * 4 + 1] = slope_y;
			squared_sum += (double)(slope_x * slope_x + slope_y * slope_y);
		}
	}
	scale = 1.0f / (float)SDL_sqrt(squared_sum / (RIPPLE_SIZE * RIPPLE_SIZE) + 1e-12);
	// Next to each slope its square: averaged down the levels, how much
	// the slopes under a texel of a far level differ (the glint widened by
	// it in the shader, instead of glittering)
	for (int i = 0; i < RIPPLE_SIZE * RIPPLE_SIZE; i++)
	{
		slope[i * 4] *= scale;
		slope[i * 4 + 1] *= scale;
		slope[i * 4 + 2] = slope[i * 4] * slope[i * 4] / RIPPLE_SLOPE_RANGE;
		slope[i * 4 + 3] = slope[i * 4 + 1] * slope[i * 4 + 1] / RIPPLE_SLOPE_RANGE;
	}
	// Each level from the one above, two by two averaged, in place
	for (uint32_t level = 0, size = RIPPLE_SIZE; level < levels; level++, size = SDL_max(size / 2, 1u))
	{
		if (level > 0)
		{
			uint32_t above = size * 2;

			for (uint32_t y = 0; y < size; y++)
			{
				for (uint32_t x = 0; x < size; x++)
				{
					for (int channel = 0; channel < 4; channel++)
					{
						slope[(y * size + x) * 4 + channel] = 0.25f * (slope[((2 * y) * above + 2 * x) * 4 + channel]
							+ slope[((2 * y) * above + 2 * x + 1) * 4 + channel] + slope[((2 * y + 1) * above + 2 * x) * 4 + channel]
							+ slope[((2 * y + 1) * above + 2 * x + 1) * 4 + channel]);
					}
				}
			}
		}
		for (uint32_t i = 0; i < 4 * size * size; i++)
		{
			texels[i] = (int16_t)SDL_lroundf(SDL_clamp(slope[i] / RIPPLE_SLOPE_RANGE, -1.0f, 1.0f) * 32767.0f);
		}
		gpu_upload_texture(texture, level, 0, 0, 0, size, size, texels, sizeof(int16_t) * 4 * size * size);
	}

done:
	SDL_free(height);
	SDL_free(slope);
	SDL_free(texels);
	return texture;
}

bool water_renderer_create(water_renderer_t *renderer)
{
	SDL_zerop(renderer);
	renderer->ripple_texture = make_ripple_texture();
	if (renderer->ripple_texture == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot make the water's ripples: %s", SDL_GetError());
		return false;
	}
	// The usual pipeline now, so a broken shader shows at the start
	return water_pipeline(renderer, 1 | 4) != NULL;
}

static void release_capture(water_renderer_t *renderer)
{
	if (renderer->scene_color != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, renderer->scene_color);
	}
	if (renderer->scene_depth != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, renderer->scene_depth);
	}
	renderer->scene_color = NULL;
	renderer->scene_depth = NULL;
	renderer->scene_width = 0;
	renderer->scene_height = 0;
}

static void release_split_textures(water_renderer_t *renderer)
{
	SDL_GPUTexture **textures[4] = { &renderer->ray_texture, &renderer->sky_texture, &renderer->seen_texture, &renderer->distance_texture };

	for (int i = 0; i < 4; i++)
	{
		if (*textures[i] != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, *textures[i]);
		}
		*textures[i] = NULL;
	}
	renderer->split_width = 0;
	renderer->split_height = 0;
}

void water_renderer_destroy(water_renderer_t *renderer)
{
	if (renderer->instance_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, renderer->instance_buffer);
	}
	SDL_free(renderer->instance_staging);
	renderer->instance_buffer = NULL;
	renderer->instance_staging = NULL;
	renderer->instance_capacity = 0;
	release_capture(renderer);
	release_split_textures(renderer);
	for (int i = 0; i < 32; i++)
	{
		if (renderer->pipelines[i] != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, renderer->pipelines[i]);
		}
	}
	if (renderer->ripple_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, renderer->ripple_texture);
	}
	if (renderer->march_pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(gpu.device, renderer->march_pipeline);
	}
	if (renderer->apply_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, renderer->apply_pipeline);
	}
	SDL_zerop(renderer);
}

// A chunk's mesh reaches one cell into the next chunk to the right and to
// the front, so it must be drawn if any of those tiles hold water
static bool chunk_has_water(const water_t *water, int chunk_x, int chunk_y)
{
	int last = water->tiles_per_side - 1;
	int next_x = SDL_min(chunk_x + 1, last);
	int next_y = SDL_min(chunk_y + 1, last);

	return water->tile_state[chunk_y * water->tiles_per_side + chunk_x] != WATER_TILE_DRY
		|| water->tile_state[chunk_y * water->tiles_per_side + next_x] != WATER_TILE_DRY
		|| water->tile_state[next_y * water->tiles_per_side + chunk_x] != WATER_TILE_DRY
		|| water->tile_state[next_y * water->tiles_per_side + next_x] != WATER_TILE_DRY;
}

// The box a chunk's water fills: false if it holds none or is out of view
static bool chunk_seen(const water_t *water, const terrain_t *terrain, const frustum_t *frustum, int chunk_x, int chunk_y)
{
	const terrain_chunk_t *chunk = &terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x];
	float chunk_extent = (float)TERRAIN_CHUNK_SIZE * terrain->cell_size_meters;
	int32_t tile = chunk_y * water->tiles_per_side + chunk_x;
	vec3_t box_minimum;
	vec3_t box_maximum;

	if (!chunk_has_water(water, chunk_x, chunk_y))
	{
		return false;
	}
	box_minimum = vec3_make((float)chunk_x * chunk_extent, chunk->minimum_height_meters, (float)chunk_y * chunk_extent);
	box_maximum = vec3_make(box_minimum.x + chunk_extent + terrain->cell_size_meters,
		chunk->maximum_height_meters + (float)water->tile_max_depth[tile] * TERRAIN_METERS_PER_HEIGHT_UNIT + 1.0f,
		box_minimum.z + chunk_extent + terrain->cell_size_meters);
	return frustum_contains_box(frustum, box_minimum, box_maximum);
}

bool water_renderer_visible(const water_t *water, const terrain_t *terrain, const camera_t *camera)
{
	frustum_t frustum = frustum_from_matrix(camera->view_projection);

	if (water_gpu_depth_texture(water) == NULL)
	{
		return false;
	}
	for (int chunk_y = 0; chunk_y < terrain->chunks_per_side; chunk_y++)
	{
		for (int chunk_x = 0; chunk_x < terrain->chunks_per_side; chunk_x++)
		{
			if (chunk_seen(water, terrain, &frustum, chunk_x, chunk_y))
			{
				return true;
			}
		}
	}
	return false;
}

bool water_renderer_capture(water_renderer_t *renderer, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, int width, int height)
{
	SDL_GPUCopyPass *copy;
	SDL_GPUTextureLocation source;
	SDL_GPUTextureLocation destination;

	renderer->captured = false;
	if (gpu.command_buffer == NULL || color_texture == NULL || depth_texture == NULL || width <= 0 || height <= 0)
	{
		return false;
	}
	if (renderer->scene_width != width || renderer->scene_height != height || renderer->scene_color == NULL)
	{
		release_capture(renderer);
		renderer->scene_color = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, GPU_SCENE_COLOR_FORMAT, (uint32_t)width, (uint32_t)height, 1, 1,
			SDL_GPU_TEXTUREUSAGE_SAMPLER);
		renderer->scene_depth = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.depth_format, (uint32_t)width, (uint32_t)height, 1, 1,
			SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (renderer->scene_color == NULL || renderer->scene_depth == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot make the water's %d x %d copy of the scene: %s", width, height, SDL_GetError());
			release_capture(renderer);
			return false;
		}
		renderer->scene_width = width;
		renderer->scene_height = height;
	}
	copy = SDL_BeginGPUCopyPass(gpu.command_buffer);
	if (copy == NULL)
	{
		return false;
	}
	SDL_zero(source);
	SDL_zero(destination);
	source.texture = color_texture;
	destination.texture = renderer->scene_color;
	SDL_CopyGPUTextureToTexture(copy, &source, &destination, (uint32_t)width, (uint32_t)height, 1, false);
	source.texture = depth_texture;
	destination.texture = renderer->scene_depth;
	SDL_CopyGPUTextureToTexture(copy, &source, &destination, (uint32_t)width, (uint32_t)height, 1, false);
	SDL_EndGPUCopyPass(copy);
	renderer->captured = true;
	return true;
}

void water_renderer_prepare(water_renderer_t *renderer, const water_t *water, const terrain_t *terrain, const camera_t *camera)
{
	frustum_t frustum = frustum_from_matrix(camera->view_projection);
	int chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;

	renderer->instance_count = 0;
	if (water_gpu_depth_texture(water) == NULL)
	{
		return;
	}
	if (renderer->instance_capacity < chunk_count)
	{
		uint32_t bytes = (uint32_t)(sizeof(int32_t) * 2 * (size_t)chunk_count);
		int32_t *staging = SDL_realloc(renderer->instance_staging, bytes);

		if (staging == NULL)
		{
			return;
		}
		renderer->instance_staging = staging;
		if (renderer->instance_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, renderer->instance_buffer);
		}
		renderer->instance_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, bytes);
		renderer->instance_capacity = renderer->instance_buffer != NULL ? chunk_count : 0;
		if (renderer->instance_buffer == NULL)
		{
			return;
		}
	}
	// The water's own culling: the terrain's visible chunks are boxed by the
	// ground alone, and water stands above it (looking up from under a
	// lake, the ground is out of view while its surface is not)
	for (int chunk_y = 0; chunk_y < terrain->chunks_per_side; chunk_y++)
	{
		for (int chunk_x = 0; chunk_x < terrain->chunks_per_side; chunk_x++)
		{
			if (!chunk_seen(water, terrain, &frustum, chunk_x, chunk_y))
			{
				continue;
			}
			renderer->instance_staging[2 * renderer->instance_count] = chunk_x * TERRAIN_CHUNK_SIZE;
			renderer->instance_staging[2 * renderer->instance_count + 1] = chunk_y * TERRAIN_CHUNK_SIZE;
			renderer->instance_count++;
		}
	}
	if (renderer->instance_count > 0)
	{
		gpu_upload_buffer(renderer->instance_buffer, 0, renderer->instance_staging, sizeof(int32_t) * 2 * (size_t)renderer->instance_count);
	}
}

void water_renderer_draw(water_renderer_t *renderer, SDL_GPURenderPass *pass, const water_t *water, const terrain_t *terrain,
	const terrain_renderer_t *terrain_renderer, const camera_t *camera, const render_settings_t *settings,
	const shadow_map_t *shadow, const shadow_map_t *near_shadow, const clouds_t *clouds)
{
	water_uniforms_t uniforms;
	SDL_GPUGraphicsPipeline *pipeline;
	SDL_GPUBufferBinding vertex_binding = { terrain_renderer->vertex_buffer, 0 };
	SDL_GPUBufferBinding index_binding = { terrain_renderer->index_buffer, 0 };
	SDL_GPUTextureSamplerBinding vertex_textures[3] =
	{
		{ terrain_renderer->height_texture, gpu.nearest_clamp },
		{ water_gpu_depth_texture(water), gpu.nearest_clamp },
		{ water_gpu_flow_texture(water), gpu.nearest_clamp },
	};
	SDL_GPUTexture *cloud_shadows = clouds_shadow_texture();
	SDL_GPUTextureSamplerBinding fragment_textures[3] =
	{
		{ terrain_renderer->materials.variation_texture, gpu.linear_repeat },
		{ terrain_renderer->tile_texture != NULL ? terrain_renderer->tile_texture : terrain_renderer->empty_integer_texture, gpu.nearest_clamp },
		{ cloud_shadows != NULL ? cloud_shadows : terrain_renderer->materials.variation_texture, gpu.linear_repeat },
	};
	bool reflections = renderer->captured && settings->water_reflections;
	SDL_GPUTextureSamplerBinding reflection_textures[3] =
	{
		{ reflections ? renderer->scene_color : terrain_renderer->materials.variation_texture, gpu.linear_clamp },
		{ reflections ? renderer->scene_depth : terrain_renderer->materials.variation_texture, gpu.nearest_clamp },
		{ clouds != NULL && clouds->dome_texture != NULL ? clouds->dome_texture : terrain_renderer->materials.variation_texture,
			gpu.linear_clamp },
	};
	int bits = 0;

	renderer->captured = false;
	renderer->drawn_chunk_count = 0;
	if (water_gpu_depth_texture(water) == NULL || terrain_renderer->height_texture == NULL)
	{
		return;
	}
	// Transparent surface over the already drawn terrain: test depth so
	// hills hide the water behind them, but do not write it (each of these
	// can be switched in the settings, for debugging). From under water the
	// surface above is seen from its back.
	bits |= settings->water_depth_test ? 1 : 0;
	bits |= settings->water_depth_write ? 2 : 0;
	bits |= settings->water_cull_back_faces && !camera->underwater ? 4 : 0;
	bits |= terrain_renderer->wireframe ? 8 : 0;
	bits |= renderer->drawing_split ? 16 : 0;
	pipeline = water_pipeline(renderer, bits);
	if (pipeline == NULL)
	{
		pipeline = water_pipeline(renderer, bits & ~8);
	}
	if (pipeline == NULL)
	{
		return;
	}

	SDL_zero(uniforms);
	uniforms.map_size = terrain->size;
	uniforms.cell_size = terrain->cell_size_meters;
	uniforms.thin_highlight[0] = renderer->highlight_thin ? 1.0f : 0.0f;
	uniforms.thin_highlight[1] = renderer->film_meters;
	uniforms.thin_highlight[2] = renderer->thin_meters;
	uniforms.thin_highlight[3] = renderer->moving_speed;
	for (int i = 0; i < 3; i++)
	{
		uniforms.shallow_color[i] = srgb_to_linear(settings->water_shallow_color[i]);
		uniforms.deep_color[i] = srgb_to_linear(settings->water_deep_color[i]);
	}
	uniforms.clarity_meters = SDL_max(settings->water_clarity_meters, 0.01f);
	uniforms.opacity_per_meter = settings->water_opacity_per_meter;
	uniforms.foam_speed = SDL_max(settings->foam_speed_meters_per_second, 0.01f);
	// Muddy water: a fixed brown, deliberately not a setting yet
	uniforms.mud_color[0] = srgb_to_linear(0.42f);
	uniforms.mud_color[1] = srgb_to_linear(0.32f);
	uniforms.mud_color[2] = srgb_to_linear(0.20f);
	uniforms.tile_overlay = terrain_renderer->tile_overlay ? 1 : 0;
	uniforms.step_blend = SDL_clamp(renderer->step_blend, 0.0f, 1.0f);
	uniforms.reflections = reflections ? 1 : 0;
	{
		float reach = settings->water_reflection_meters > 0.0f ? settings->water_reflection_meters * SDL_max(camera->zoom, 1.0f) : 1e9f;

		uniforms.reflection_range[0] = reach;
		uniforms.reflection_range[1] = 0.2f * reach;
	}
	uniforms.reflection_range[2] = reflections ? SDL_clamp(settings->water_refraction, 0.0f, 3.0f) : 0.0f;
	uniforms.dome = settings->water_reflections && clouds != NULL && clouds->dome_texture != NULL ? (clouds->dome_half ? 2 : 1) : 0;
	// OpenGL's perspective (math3d.h), its depth taken to 0..1
	// (gpu_clip_matrix): depth = x + y / distance
	depth_to_distance(renderer, camera, uniforms.depth_to_distance);

	SDL_BindGPUGraphicsPipeline(pass, pipeline);
	lights_bind(pass);
	SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
	SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
	SDL_BindGPUVertexSamplers(pass, 0, vertex_textures, 3);
	shadow_map_bind_for_shading(pass, shadow, near_shadow);
	SDL_BindGPUFragmentSamplers(pass, 2, fragment_textures, 3);
	SDL_BindGPUFragmentSamplers(pass, 5, reflection_textures, 3);
	{
		// (none: anything stands in, not read)
		SDL_GPUTextureSamplerBinding ice = { renderer->ice_texture != NULL ? renderer->ice_texture : terrain_renderer->materials.variation_texture,
			gpu.linear_clamp };

		SDL_BindGPUFragmentSamplers(pass, 8, &ice, 1);
		uniforms.ice[0] = renderer->ice_texture != NULL ? 1.0f : 0.0f;
	}
	{
		// The sky's occlusion (none: anything stands in, not read: scene
		// sky_occlusion.y 0)
		SDL_GPUTextureSamplerBinding sky[2] =
		{
			{ terrain_renderer->sky_occlusion_texture != NULL ? terrain_renderer->sky_occlusion_texture : terrain_renderer->materials.variation_texture,
				gpu.linear_clamp },
			{ terrain_renderer->sky_layers_texture != NULL ? terrain_renderer->sky_layers_texture : terrain_renderer->materials.variation_texture,
				gpu.linear_clamp },
		};

		SDL_BindGPUFragmentSamplers(pass, 9, sky, 2);
	}
	{
		SDL_GPUTextureSamplerBinding ripples = { renderer->ripple_texture, gpu.linear_repeat };

		SDL_BindGPUFragmentSamplers(pass, 11, &ripples, 1);
		uniforms.ice[1] = SDL_clamp(settings->water_choppiness, 0.0f, 4.0f);
		uniforms.ice[2] = (float)settings->debug_water_ripples;
	}
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));

	// The chunks with water in view (water_renderer_prepare), all in one draw
	if (renderer->instance_count > 0 && renderer->instance_buffer != NULL)
	{
		SDL_GPUBufferBinding instance_binding = { renderer->instance_buffer, 0 };

		uniforms.chunk_origin[0] = 0;
		uniforms.chunk_origin[1] = 0;
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
		SDL_BindGPUVertexBuffers(pass, 1, &instance_binding, 1);
		SDL_DrawGPUIndexedPrimitives(pass, (uint32_t)terrain_renderer->index_count, (uint32_t)renderer->instance_count, 0, 0, 0);
		renderer->drawn_chunk_count += renderer->instance_count;
		frame_profile_count(FRAME_WATER_CHUNKS_DRAWN, renderer->instance_count);
	}
}

// The split's textures at this size, and its shaders (once)
static bool make_split(water_renderer_t *renderer, int width, int height)
{
	if (renderer->split_failed)
	{
		return false;
	}
	if (renderer->march_pipeline == NULL)
	{
		gpu_compute_layout_t layout;
		gpu_pipeline_info_t info;

		SDL_zero(layout);
		layout.samplers = 4;
		layout.readwrite_storage_textures = 2;
		layout.uniform_buffers = 2;
		layout.threads_x = 64;
		renderer->march_pipeline = gpu_load_compute("water_reflection.comp", &layout);
		SDL_zero(info);
		// (the clouds' full screen triangle)
		info.vertex_shader = gpu_load_shader("clouds.vert", 0, 0, 1);
		info.fragment_shader = gpu_load_shader("water_reflection_apply.frag", 4, 0, 1);
		info.cull = SDL_GPU_CULLMODE_NONE;
		info.no_depth = true;
		info.blend = GPU_BLEND_ADD;
		if (info.vertex_shader != NULL && info.fragment_shader != NULL)
		{
			renderer->apply_pipeline = gpu_make_pipeline(&info, "water reflections");
		}
		if (info.vertex_shader != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, info.vertex_shader);
		}
		if (info.fragment_shader != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, info.fragment_shader);
		}
		if (renderer->march_pipeline == NULL || renderer->apply_pipeline == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Water: no reflections at half size (their shaders did not load); every pixel marches");
			renderer->split_failed = true;
			return false;
		}
	}
	if (renderer->split_width != width || renderer->split_height != height || renderer->ray_texture == NULL)
	{
		uint32_t half_width = (uint32_t)SDL_max((width + 1) / 2, 1);
		uint32_t half_height = (uint32_t)SDL_max((height + 1) / 2, 1);

		release_split_textures(renderer);
		renderer->ray_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, (uint32_t)width,
			(uint32_t)height, 1, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		renderer->sky_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, (uint32_t)width,
			(uint32_t)height, 1, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		renderer->seen_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, half_width, half_height,
			1, 1, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		renderer->distance_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, half_width, half_height,
			1, 1, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (renderer->ray_texture == NULL || renderer->sky_texture == NULL || renderer->seen_texture == NULL || renderer->distance_texture == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Water: cannot make the reflections' %d x %d textures: %s", width, height, SDL_GetError());
			release_split_textures(renderer);
			return false;
		}
		renderer->split_width = width;
		renderer->split_height = height;
	}
	return true;
}

bool water_renderer_draw_split(water_renderer_t *renderer, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, int width,
	int height, const scene_uniforms_t *scene, const water_t *water, const terrain_t *terrain,
	const terrain_renderer_t *terrain_renderer, const camera_t *camera, const render_settings_t *settings,
	const shadow_map_t *shadow, const shadow_map_t *near_shadow, const clouds_t *clouds)
{
	SDL_GPUColorTargetInfo colors[3];
	SDL_GPUDepthStencilTargetInfo depth;
	SDL_GPURenderPass *pass;
	float uniforms[4];

	if (gpu.command_buffer == NULL || !renderer->captured || !settings->water_reflections || !make_split(renderer, width, height))
	{
		return false;
	}
	// The water, its rays and its skies beside its color
	SDL_zeroa(colors);
	colors[0].texture = color_texture;
	colors[0].load_op = SDL_GPU_LOADOP_LOAD;
	colors[0].store_op = SDL_GPU_STOREOP_STORE;
	colors[1].texture = renderer->ray_texture;
	colors[1].load_op = SDL_GPU_LOADOP_CLEAR;
	colors[1].store_op = SDL_GPU_STOREOP_STORE;
	colors[2].texture = renderer->sky_texture;
	colors[2].load_op = SDL_GPU_LOADOP_CLEAR;
	colors[2].store_op = SDL_GPU_STOREOP_STORE;
	SDL_zero(depth);
	depth.texture = depth_texture;
	depth.load_op = SDL_GPU_LOADOP_LOAD;
	depth.store_op = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	pass = SDL_BeginGPURenderPass(gpu.command_buffer, colors, 3, &depth);
	if (pass == NULL)
	{
		return false;
	}
	{
		SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };

		SDL_SetGPUViewport(pass, &viewport);
	}
	scene_uniforms_push(scene);
	depth_to_distance(renderer, camera, uniforms);
	renderer->drawing_split = true;
	water_renderer_draw(renderer, pass, water, terrain, terrain_renderer, camera, settings, shadow, near_shadow, clouds);
	renderer->drawing_split = false;
	SDL_EndGPURenderPass(pass);

	// The march, at half size
	{
		SDL_GPUStorageTextureReadWriteBinding targets[2];
		SDL_GPUTextureSamplerBinding samplers[4] =
		{
			{ renderer->ray_texture, gpu.nearest_clamp },
			{ renderer->sky_texture, gpu.nearest_clamp },
			{ renderer->scene_color, gpu.linear_clamp },
			{ renderer->scene_depth, gpu.nearest_clamp },
		};
		uint32_t texels = (uint32_t)(((width + 1) / 2) * ((height + 1) / 2));
		SDL_GPUComputePass *compute;

		SDL_zeroa(targets);
		targets[0].texture = renderer->seen_texture;
		targets[1].texture = renderer->distance_texture;
		compute = SDL_BeginGPUComputePass(gpu.command_buffer, targets, 2, NULL, 0);
		if (compute == NULL)
		{
			return true;
		}
		SDL_BindGPUComputePipeline(compute, renderer->march_pipeline);
		SDL_BindGPUComputeSamplers(compute, 0, samplers, 4);
		SDL_PushGPUComputeUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUComputeUniformData(gpu.command_buffer, 1, uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(compute, (texels + 63) / 64, 1, 1);
		SDL_EndGPUComputePass(compute);
	}

	// What it found, in the sky's place
	{
		SDL_GPUColorTargetInfo color;
		SDL_GPUTextureSamplerBinding samplers[4] =
		{
			{ renderer->ray_texture, gpu.nearest_clamp },
			{ renderer->sky_texture, gpu.nearest_clamp },
			{ renderer->seen_texture, gpu.nearest_clamp },
			{ renderer->distance_texture, gpu.nearest_clamp },
		};

		SDL_zero(color);
		color.texture = color_texture;
		color.load_op = SDL_GPU_LOADOP_LOAD;
		color.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass(gpu.command_buffer, &color, 1, NULL);
		if (pass == NULL)
		{
			return true;
		}
		scene_uniforms_push(scene);
		SDL_BindGPUGraphicsPipeline(pass, renderer->apply_pipeline);
		SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}
	return true;
}
