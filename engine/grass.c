// Grass, blades made on the GPU every frame. See grass.h.

#include "grass.h"

#include <SDL3/SDL.h>

#include "lights.h"
#include "math3d.h"

// grass.comp's modes, and its blades
#define GRASS_CLEAR 1
#define GRASS_SCATTER 2
#define GRASS_ARGUMENTS 3
#define GRASS_BLADE_FLOATS 8
#define GRASS_THREADS 256
#define GRASS_MAXIMUM_GROUPS 65535u
// The rings: at most this many (each twice as far as the one before)
#define GRASS_MAXIMUM_LEVELS 10
// Out to here (of the distance, within these bounds) every blade of the
// fine grid shows; farther, they thin out with the square of the distance
#define GRASS_FULL_SHARE 0.25f
#define GRASS_FULL_MINIMUM_METERS 6.0f
#define GRASS_FULL_MAXIMUM_METERS 20.0f
// The blades a frame may write at most, whatever the settings ask
#define GRASS_MAXIMUM_BLADES (4u * 1024u * 1024u)
// Blades bow this far (radians) in a gale
#define GRASS_GALE_BOW 0.9f

// grass.comp's block (std140)
typedef struct
{
	float planes[24];
	float eye[4];
	float grid[4];
	float grid_origin[4];
	float map[4];
	float slopes[4];
	float terrain[4];
	float blade[4];
	int32_t control[4];
	float material_patches[MATERIAL_COUNT][4];
	float view_projection[16];             // the camera's the pyramid was made with (depth 0..1)
	float hiz_size[4];                     // level 0's width and height, the level count, 1 if it is there
} grass_scatter_uniforms_t;

SDL_COMPILE_TIME_ASSERT(grass_scatter_uniforms_size, sizeof(grass_scatter_uniforms_t) == 224 + 16 * MATERIAL_COUNT + 80);

// grass.vert's block (std140, vertex slot 1)
typedef struct
{
	float wind[4];
	float gusts[4];
	float look[4];
	float tint[4];
	float variation[4];
	float coarse[4];
} grass_draw_uniforms_t;

bool grass_create(grass_t *grass)
{
	gpu_compute_layout_t layout;
	gpu_pipeline_info_t info;
	static const SDL_GPUVertexBufferDescription buffers[1] =
	{
		{ 0, sizeof(float) * GRASS_BLADE_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
	};
	static const SDL_GPUVertexAttribute attributes[2] =
	{
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
	};

	SDL_zerop(grass);
	SDL_zero(layout);
	layout.samplers = 9;
	layout.readonly_storage_buffers = 1;
	layout.readwrite_storage_buffers = 2;
	layout.uniform_buffers = 1;
	layout.threads_x = GRASS_THREADS;
	grass->scatter_pipeline = gpu_load_compute("grass.comp", &layout);
	grass->map_scatter_pipeline = gpu_load_compute("grass_map.comp", &layout);
	SDL_zero(info);
	info.vertex_shader = gpu_load_shader("grass.vert", 7, 0, 2);
	info.fragment_shader = gpu_load_shader("grass.frag", 4, LIGHTS_STORAGE_BUFFERS, 1);
	info.vertex_buffers = buffers;
	info.vertex_buffer_count = 1;
	info.vertex_attributes = attributes;
	info.vertex_attribute_count = 2;
	info.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
	info.cull = SDL_GPU_CULLMODE_NONE;
	info.depth_test = true;
	info.depth_write = true;
	if (info.vertex_shader != NULL && info.fragment_shader != NULL)
	{
		info.multisampled = true;
		grass->pipeline = gpu_make_pipeline(&info, "grass");
	}
	if (info.vertex_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.vertex_shader);
	}
	if (info.fragment_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.fragment_shader);
	}
	grass->count_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
		sizeof(uint32_t) * 8);
	grass->argument_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDIRECT, sizeof(uint32_t) * 4);
	grass->no_occlusion_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, sizeof(float) * 4);
	grass->failed = grass->scatter_pipeline == NULL || grass->pipeline == NULL || grass->count_buffer == NULL || grass->argument_buffer == NULL
		|| grass->no_occlusion_buffer == NULL;
	if (grass->failed)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Grass: its shaders or buffers are not there; no grass");
	}
	return !grass->failed;
}

void grass_destroy(grass_t *grass)
{
	SDL_GPUBuffer *buffers[4] = { grass->blade_buffer, grass->count_buffer, grass->argument_buffer, grass->no_occlusion_buffer };

	for (int i = 0; i < 4; i++)
	{
		if (buffers[i] != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, buffers[i]);
		}
	}
	if (grass->map_scatter_pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(gpu.device, grass->map_scatter_pipeline);
	}
	if (grass->scatter_pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(gpu.device, grass->scatter_pipeline);
	}
	if (grass->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, grass->pipeline);
	}
	SDL_zerop(grass);
}

// Out to where every blade of the fine grid shows
static float full_radius_of(float distance)
{
	return SDL_clamp(distance * GRASS_FULL_SHARE, GRASS_FULL_MINIMUM_METERS, GRASS_FULL_MAXIMUM_METERS);
}

static void dispatch(grass_t *grass, SDL_GPUCommandBuffer *command_buffer, const grass_world_t *world, grass_scatter_uniforms_t *uniforms,
	int mode, uint32_t threads)
{
	SDL_GPUStorageBufferReadWriteBinding read_write[2];
	SDL_GPUTextureSamplerBinding samplers[9] =
	{
		{ world->height_texture, gpu.nearest_clamp },
		{ world->water_texture, gpu.nearest_clamp },
		{ world->flow_texture, gpu.nearest_clamp },
		{ world->soil_weight_texture, gpu.linear_clamp },
		{ world->normal_texture, gpu.linear_clamp },
		{ world->surface_texture, gpu.linear_clamp },
		{ world->variation_texture, gpu.linear_repeat },
		{ world->material_texture, gpu.nearest_clamp },
		{ world->overlay_texture != NULL ? world->overlay_texture : world->material_texture, gpu.nearest_clamp },
	};
	SDL_GPUComputePass *compute;
	uint32_t groups = (threads + GRASS_THREADS - 1) / GRASS_THREADS;

	SDL_zeroa(read_write);
	read_write[0].buffer = grass->blade_buffer;
	read_write[1].buffer = grass->count_buffer;
	compute = SDL_BeginGPUComputePass(command_buffer, NULL, 0, read_write, 2);
	if (compute == NULL)
	{
		return;
	}
	SDL_BindGPUComputePipeline(compute, world->material_map && grass->map_scatter_pipeline != NULL ? grass->map_scatter_pipeline
		: grass->scatter_pipeline);
	SDL_BindGPUComputeSamplers(compute, 0, samplers, 9);
	{
		SDL_GPUBuffer *pyramid = world->occlusion != NULL && uniforms->hiz_size[3] > 0.5f ? hiz_buffer(world->occlusion) : grass->no_occlusion_buffer;

		SDL_BindGPUComputeStorageBuffers(compute, 0, &pyramid, 1);
	}
	uniforms->control[0] = mode;
	SDL_PushGPUComputeUniformData(command_buffer, 0, uniforms, sizeof(*uniforms));
	SDL_DispatchGPUCompute(compute, SDL_clamp(groups, 1u, GRASS_MAXIMUM_GROUPS), 1, 1);
	SDL_EndGPUComputePass(compute);
}

void grass_prepare(grass_t *grass, const camera_t *camera, const render_settings_t *render, const objects_settings_t *settings,
	const grass_world_t *world)
{
	float distance = SDL_clamp(settings->grass_distance_meters, 5.0f, 500.0f);
	float density = SDL_clamp(settings->grass_density, 1.0f, 2000.0f);
	float spacing = 1.0f / SDL_sqrtf(density);
	float full_radius = full_radius_of(distance);
	grass_scatter_uniforms_t uniforms;
	frustum_t frustum = frustum_from_matrix(camera->view_projection);
	SDL_GPUCommandBuffer *command_buffer;
	float estimate;
	uint32_t capacity;
	int level_count;

	grass->prepared = false;
	if (grass->failed || !settings->grass || world->height_texture == NULL || world->water_texture == NULL || world->flow_texture == NULL
		|| world->soil_weight_texture == NULL || world->normal_texture == NULL || world->surface_texture == NULL
		|| world->variation_texture == NULL || world->material_color_array == NULL)
	{
		return;
	}
	// The blades out to the distance, at most (before the view and the
	// ground leave most out): the full disc near, then thinning with the
	// square of the distance, its integral a logarithm
	estimate = density * SDL_PI_F * full_radius * full_radius * (1.0f + 2.0f * SDL_logf(SDL_max(distance / full_radius, 1.0f)));
	capacity = (uint32_t)SDL_min(estimate + 4096.0f, (float)GRASS_MAXIMUM_BLADES);
	if (grass->blade_buffer == NULL || grass->blade_capacity < capacity)
	{
		uint32_t wanted = SDL_min(capacity + capacity / 2, GRASS_MAXIMUM_BLADES);

		if (grass->blade_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, grass->blade_buffer);
		}
		grass->blade_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
			wanted * (uint32_t)sizeof(float) * GRASS_BLADE_FLOATS);
		grass->blade_capacity = grass->blade_buffer != NULL ? wanted : 0;
		if (grass->blade_buffer == NULL)
		{
			return;
		}
	}
	grass->blade_estimate = (uint32_t)estimate;

	SDL_zero(uniforms);
	for (int i = 0; i < 6; i++)
	{
		uniforms.planes[4 * i] = frustum.planes[i].a;
		uniforms.planes[4 * i + 1] = frustum.planes[i].b;
		uniforms.planes[4 * i + 2] = frustum.planes[i].c;
		uniforms.planes[4 * i + 3] = frustum.planes[i].d;
	}
	uniforms.eye[0] = camera->eye_position.x;
	uniforms.eye[1] = camera->eye_position.y;
	uniforms.eye[2] = camera->eye_position.z;
	uniforms.eye[3] = distance;
	uniforms.grid_origin[2] = spacing;
	uniforms.grid_origin[3] = (float)grass->blade_capacity;
	uniforms.map[0] = (float)world->map_size;
	uniforms.map[1] = world->cell_size_meters;
	uniforms.map[2] = world->soil_blend_cells;
	SDL_memcpy(uniforms.material_patches, world->material_patches, sizeof(uniforms.material_patches));
	uniforms.control[1] = world->material_map ? 1 : 0;
	uniforms.control[2] = world->material_map && world->overlay_texture != NULL ? 1 : 0;
	uniforms.slopes[0] = render->grass_slope_start;
	uniforms.slopes[1] = SDL_max(render->grass_slope_end, render->grass_slope_start + 0.01f);
	uniforms.slopes[2] = render->cliff_slope_start;
	uniforms.slopes[3] = SDL_max(render->cliff_slope_end, render->cliff_slope_start + 0.01f);
	uniforms.terrain[0] = render->snow_height_fraction;
	uniforms.terrain[1] = render->snow_aspect_shift;
	uniforms.terrain[2] = world->max_height_meters;
	uniforms.terrain[3] = 1.0f / SDL_max(render->variation_tiling_meters, 1.0f);
	uniforms.blade[0] = SDL_clamp(settings->grass_height_meters, 0.02f, 3.0f);
	uniforms.blade[1] = SDL_clamp(settings->grass_width_meters, 0.001f, 0.2f);
	uniforms.blade[2] = world->sun_direction[0];
	uniforms.blade[3] = world->sun_direction[2];
	if (world->occlusion != NULL && world->occlusion->ready && hiz_buffer(world->occlusion) != NULL)
	{
		gpu_clip_matrix(world->occlusion->view_projection, uniforms.view_projection);
		uniforms.hiz_size[0] = (float)world->occlusion->width;
		uniforms.hiz_size[1] = (float)world->occlusion->height;
		uniforms.hiz_size[2] = (float)world->occlusion->level_count;
		uniforms.hiz_size[3] = 1.0f;
	}

	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return;
	}
	dispatch(grass, command_buffer, world, &uniforms, GRASS_CLEAR, 1);
	// Ring by ring: level 0 out to the full radius on the fine grid, level
	// L >= 1 from full_radius * 2^(L-1) to twice that, on a grid 2^(L-1)
	// times as coarse
	level_count = 1;
	while (level_count < GRASS_MAXIMUM_LEVELS && full_radius * SDL_powf(2.0f, (float)level_count - 1.0f) < distance)
	{
		level_count++;
	}
	grass->candidate_count = 0;
	for (int level = 0; level < level_count; level++)
	{
		float level_spacing = level == 0 ? spacing : spacing * SDL_powf(2.0f, (float)level - 1.0f);
		float outer = SDL_min(level == 0 ? full_radius : full_radius * SDL_powf(2.0f, (float)level), distance);
		uint32_t side = (uint32_t)SDL_ceilf(2.0f * outer / level_spacing) + 1;
		uint32_t threads;

		// A level too big to look at in one go (a huge density far out):
		// what fits
		side = SDL_min(side, (uint32_t)SDL_sqrtf((float)GRASS_MAXIMUM_GROUPS * (float)GRASS_THREADS));
		threads = side * side;
		uniforms.grid[0] = level_spacing;
		uniforms.grid[1] = (float)level;
		uniforms.grid[2] = (float)side;
		uniforms.grid[3] = full_radius;
		uniforms.grid_origin[0] = camera->eye_position.x - outer;
		uniforms.grid_origin[1] = camera->eye_position.z - outer;
		dispatch(grass, command_buffer, world, &uniforms, GRASS_SCATTER, threads);
		grass->candidate_count += threads;
	}
	dispatch(grass, command_buffer, world, &uniforms, GRASS_ARGUMENTS, 1);
	{
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command_buffer);
		SDL_GPUBufferLocation source = { grass->count_buffer, sizeof(uint32_t) };
		SDL_GPUBufferLocation destination = { grass->argument_buffer, 0 };

		if (copy == NULL)
		{
			return;
		}
		SDL_CopyGPUBufferToBuffer(copy, &source, &destination, sizeof(uint32_t) * 4, false);
		SDL_EndGPUCopyPass(copy);
	}
	grass->prepared = true;
}

void grass_draw(grass_t *grass, SDL_GPURenderPass *pass, const render_settings_t *render, const objects_settings_t *settings,
	const grass_world_t *world, const shadow_map_t *shadow, const shadow_map_t *near_shadow)
{
	grass_draw_uniforms_t uniforms;

	if (!grass->prepared || pass == NULL || !settings->grass)
	{
		return;
	}
	SDL_zero(uniforms);
	if (world->wind_uniform != NULL)
	{
		SDL_memcpy(uniforms.wind, world->wind_uniform, sizeof(float) * 4);
		uniforms.gusts[0] = world->wind_uniform[4];
		uniforms.gusts[1] = world->wind_uniform[5];
	}
	uniforms.gusts[2] = GRASS_GALE_BOW;
	uniforms.gusts[3] = render->grass_gust_sheen;
	uniforms.look[0] = (float)world->grass_layer;
	uniforms.look[1] = world->grass_macro_repeats_per_meter;
	uniforms.look[2] = (float)world->map_size;
	uniforms.look[3] = world->cell_size_meters;
	uniforms.tint[0] = settings->grass_tint[0];
	uniforms.tint[1] = settings->grass_tint[1];
	uniforms.tint[2] = settings->grass_tint[2];
	uniforms.tint[3] = SDL_clamp(settings->grass_dry_share, 0.0f, 1.0f);
	uniforms.variation[0] = 1.0f / SDL_max(render->variation_tiling_meters, 1.0f);
	uniforms.variation[1] = render->variation_strength;
	uniforms.variation[2] = render->variation_hue_strength;
	uniforms.coarse[0] = SDL_max(settings->grass_coarse_meters, 0.0f);
	uniforms.coarse[1] = SDL_clamp(settings->grass_coarse_width, 0.5f, 3.0f);
	SDL_BindGPUGraphicsPipeline(pass, grass->pipeline);
	{
		SDL_GPUTextureSamplerBinding vertex_samplers[7] =
		{
			{ world->material_color_array, gpu.linear_repeat },
			{ world->surface_texture, gpu.linear_clamp },
			{ world->variation_texture, gpu.linear_repeat },
			// The sky's occlusion, its layers, the bounce texture, the far
			// shadow map (none: anything stands in, not read: scene
			// sky_occlusion.y 0)
			{ world->sky_occlusion_texture != NULL ? world->sky_occlusion_texture : world->variation_texture, gpu.linear_clamp },
			{ world->sky_layers_texture != NULL ? world->sky_layers_texture : world->variation_texture, gpu.linear_clamp },
			{ world->sky_bounce_texture != NULL ? world->sky_bounce_texture : world->variation_texture, gpu.linear_clamp },
			{ shadow->depth_texture, gpu.shadow_compare },
		};
		SDL_GPUTextureSamplerBinding fragment_samplers[2] =
		{
			{ world->variation_texture, gpu.linear_repeat },
			{ world->cloud_shadow_texture != NULL ? world->cloud_shadow_texture : world->variation_texture, gpu.linear_repeat },
		};
		SDL_GPUBufferBinding blades = { grass->blade_buffer, 0 };

		SDL_BindGPUVertexSamplers(pass, 0, vertex_samplers, 7);
		shadow_map_bind_for_shading(pass, shadow, near_shadow);
		SDL_BindGPUFragmentSamplers(pass, 2, fragment_samplers, 2);
		lights_bind(pass);
		SDL_BindGPUVertexBuffers(pass, 0, &blades, 1);
	}
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
	SDL_DrawGPUPrimitivesIndirect(pass, grass->argument_buffer, 0, 1);
}
