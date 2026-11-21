// The sky's occlusion. See sky_occlusion.h.

#include "sky_occlusion.h"

#include <SDL3/SDL.h>

#include "math3d.h"

// Each direction's depth map is at most this big (texels per side)
static const int LARGEST_VIEW = 8192;

const float sky_occlusion_layer_heights[SKY_OCCLUSION_LAYERS] = { 2.0f, 6.0f, 15.0f, 35.0f };
const float SKY_OCCLUSION_BOUNCE_FARTHEST = 400.0f;

typedef struct
{
	float light_view_projection[16];
	float direction[4];
	int32_t map_size;
	float cell_size;
	float lift;
	int32_t bounce_size;
	float depth_range;
	float padding[3];
} bounce_uniforms_t;

typedef struct
{
	int32_t bounce_size;
	float farthest;
	float padding[2];
} bounce_finish_uniforms_t;

typedef struct
{
	float light_view_projection[16];
	float direction[4];
	float layer_heights[4];
	int32_t map_size;
	float cell_size;
	float lift;
	int32_t layer_size;
} layers_uniforms_t;

typedef struct
{
	float light_view_projection[16];
	float direction[4];
	int32_t map_size;
	float cell_size;
	float lift;
	float padding;
} accumulate_uniforms_t;

// (SDL_GPU's Vulkan binds 4 KB of a uniform block at most: what is
// longer reads nothing, or garbage)
typedef struct
{
	int32_t width;
	int32_t height;
	int32_t padding[2];
} finish_uniforms_t;

static void release_texture(SDL_GPUTexture **texture)
{
	if (*texture != NULL && gpu.device != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, *texture);
	}
	*texture = NULL;
}

bool sky_occlusion_create(sky_occlusion_t *occlusion)
{
	gpu_compute_layout_t accumulate_layout = { 2, 0, 0, 1, 256, 0, 2 };
	gpu_compute_layout_t finish_layout = { 0, 0, 0, 1, 256, 0, 3 };
	gpu_compute_layout_t layers_layout = { 2, 0, 0, 1, 256, 0, 2 };
	gpu_compute_layout_t bounce_layout = { 2, 0, 0, 1, 256, 0, 1 };
	gpu_compute_layout_t bounce_finish_layout = { 0, 0, 0, 1, 256, 0, 2 };

	SDL_zerop(occlusion);
	if (gpu.device == NULL)
	{
		occlusion->failed = true;
		return false;
	}
	if (!SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTURETYPE_2D,
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Sky occlusion: the GPU cannot write R8 textures from compute shaders; none");
		occlusion->failed = true;
		return false;
	}
	occlusion->accumulate_pipeline = gpu_load_compute("sky_occlusion_accumulate.comp", &accumulate_layout);
	occlusion->finish_pipeline = gpu_load_compute("sky_occlusion_finish.comp", &finish_layout);
	occlusion->finish_layers_pipeline = gpu_load_compute("sky_occlusion_finish_layers.comp", &finish_layout);
	occlusion->layers_pipeline = gpu_load_compute("sky_occlusion_layers.comp", &layers_layout);
	occlusion->bounce_pipeline = gpu_load_compute("sky_occlusion_bounce.comp", &bounce_layout);
	occlusion->bounce_finish_pipeline = gpu_load_compute("sky_occlusion_bounce_finish.comp", &bounce_finish_layout);
	occlusion->failed = occlusion->accumulate_pipeline == NULL || occlusion->finish_pipeline == NULL || occlusion->finish_layers_pipeline == NULL
		|| occlusion->layers_pipeline == NULL
		|| occlusion->bounce_pipeline == NULL || occlusion->bounce_finish_pipeline == NULL;
	return !occlusion->failed;
}

static void stop_working(sky_occlusion_t *occlusion)
{
	release_texture(&occlusion->sum_texture);
	release_texture(&occlusion->weight_texture);
	release_texture(&occlusion->next_texture);
	release_texture(&occlusion->layer_sum_texture);
	release_texture(&occlusion->layer_weight_texture);
	release_texture(&occlusion->next_layers_texture);
	release_texture(&occlusion->bounce_sums_texture);
	release_texture(&occlusion->next_bounce_texture);
	shadow_map_destroy(&occlusion->view);
	occlusion->working = false;
}

void sky_occlusion_destroy(sky_occlusion_t *occlusion)
{
	stop_working(occlusion);
	SDL_free(occlusion->cells_cpu);
	SDL_free(occlusion->cell_sums);
	occlusion->cells_cpu = NULL;
	occlusion->cell_sums = NULL;
	release_texture(&occlusion->texture);
	release_texture(&occlusion->layers_texture);
	release_texture(&occlusion->bounce_texture);
	if (gpu.device != NULL)
	{
		if (occlusion->layers_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, occlusion->layers_pipeline);
		}
		if (occlusion->bounce_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, occlusion->bounce_pipeline);
		}
		if (occlusion->bounce_finish_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, occlusion->bounce_finish_pipeline);
		}
		if (occlusion->accumulate_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, occlusion->accumulate_pipeline);
		}
		if (occlusion->finish_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, occlusion->finish_pipeline);
		}
		if (occlusion->finish_layers_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, occlusion->finish_layers_pipeline);
		}
	}
	SDL_zerop(occlusion);
}

void sky_occlusion_forget(sky_occlusion_t *occlusion, bool request)
{
	stop_working(occlusion);
	SDL_free(occlusion->cells_cpu);
	SDL_free(occlusion->cell_sums);
	occlusion->cells_cpu = NULL;
	occlusion->cell_sums = NULL;
	occlusion->cpu_pending = false;
	release_texture(&occlusion->texture);
	release_texture(&occlusion->layers_texture);
	release_texture(&occlusion->bounce_texture);
	occlusion->ready = false;
	occlusion->map_size = 0;
	occlusion->layer_size = 0;
	occlusion->bounce_size = 0;
	occlusion->given_up = false;
	occlusion->requested = request && !occlusion->failed;
}

// Spread evenly over the sky (equal solid angles), from the zenith down to
// lowest degrees above the horizon: heights evenly between, turned by the
// golden angle one after the other
static void make_directions(sky_occlusion_t *occlusion, int count, float lowest_degrees)
{
	float lowest_sine = SDL_sinf(SDL_clamp(lowest_degrees, 0.0f, 80.0f) * SDL_PI_F / 180.0f);
	const float golden_angle = 2.39996323f;

	occlusion->direction_count = SDL_clamp(count, 4, SKY_OCCLUSION_MOST_DIRECTIONS);
	for (int k = 0; k < occlusion->direction_count; k++)
	{
		float up = 1.0f - (1.0f - lowest_sine) * ((float)k + 0.5f) / (float)occlusion->direction_count;
		float across = SDL_sqrtf(SDL_max(1.0f - up * up, 0.0f));
		float angle = golden_angle * (float)k;

		occlusion->directions[k][0] = across * SDL_cosf(angle);
		occlusion->directions[k][1] = up;
		occlusion->directions[k][2] = across * SDL_sinf(angle);
		occlusion->directions[k][3] = 0.0f;
	}
}

static bool start_working(sky_occlusion_t *occlusion, const terrain_t *terrain, const render_settings_t *settings)
{
	float extent = (float)terrain->size * terrain->cell_size_meters;
	int view_size = clamp_int((int)SDL_ceilf(extent / SDL_max(settings->sky_occlusion_texel_meters, 0.05f)), 256, LARGEST_VIEW);

	stop_working(occlusion);
	occlusion->sum_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, (uint32_t)terrain->size,
		(uint32_t)terrain->size, 1, 1, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
	occlusion->weight_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, (uint32_t)terrain->size,
		(uint32_t)terrain->size, 1, 1, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
	occlusion->next_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8_UNORM, (uint32_t)terrain->size,
		(uint32_t)terrain->size, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
	{
		uint32_t layer_size = (uint32_t)map_file_sky_layer_size(terrain->size, terrain->cell_size_meters);
		uint32_t rows = layer_size * SKY_OCCLUSION_LAYERS;

		occlusion->layer_sum_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, layer_size, rows, 1, 1,
			SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
		occlusion->layer_weight_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, layer_size, rows, 1, 1,
			SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
		occlusion->next_layers_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, layer_size, layer_size, 1, 1,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
	}
	{
		uint32_t bounce_size = (uint32_t)map_file_sky_bounce_size(terrain->size, terrain->cell_size_meters);

		occlusion->bounce_sums_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, bounce_size, bounce_size * 5u,
			1, 1, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
		occlusion->next_bounce_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, bounce_size, bounce_size,
			1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
	}
	if (occlusion->sum_texture == NULL || occlusion->weight_texture == NULL || occlusion->next_texture == NULL
		|| occlusion->layer_sum_texture == NULL || occlusion->layer_weight_texture == NULL || occlusion->next_layers_texture == NULL
		|| occlusion->bounce_sums_texture == NULL || occlusion->next_bounce_texture == NULL
		|| !shadow_map_create(&occlusion->view, view_size)
		|| occlusion->view.size <= 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Sky occlusion: no room on the GPU for working it out");
		stop_working(occlusion);
		return false;
	}
	make_directions(occlusion, settings->sky_occlusion_directions, settings->sky_occlusion_lowest_degrees);
	occlusion->directions_done = 0;
	occlusion->working = true;
	occlusion->start_counter = SDL_GetPerformanceCounter();
	occlusion->steps_since_progress = 0;
	SDL_Log("Sky occlusion: working it out for a %dx%d map, %d directions, depth maps of %d texels (%.2f m each)", terrain->size,
		terrain->size, occlusion->direction_count, view_size, (double)(extent / (float)view_size));
	return true;
}

// One direction: its depth map, then every cell's share
static void do_direction(sky_occlusion_t *occlusion, const terrain_t *terrain, objects_t *objects, int prepared_pass,
	SDL_GPUTexture *height_texture, SDL_GPUBuffer *chunk_vertex_buffer, SDL_GPUBuffer *chunk_index_buffer, int chunk_index_count)
{
	const float *direction = occlusion->directions[occlusion->directions_done];
	vec3_t towards = vec3_make(direction[0], direction[1], direction[2]);
	SDL_GPURenderPass *pass;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUComputePass *compute;
	SDL_GPUStorageTextureReadWriteBinding targets[2];
	SDL_GPUTextureSamplerBinding samplers[2];
	accumulate_uniforms_t uniforms;
	float extent = (float)terrain->size * terrain->cell_size_meters;

	shadow_map_update_projection(&occlusion->view, terrain, towards);
	// (every object of the far shadow map, whatever the direction: set up
	// once a frame, drawn into each)
	objects->prepared_pass = prepared_pass;
	pass = shadow_map_begin(&occlusion->view);
	if (pass == NULL)
	{
		return;
	}
	shadow_map_render_terrain(&occlusion->view, pass, terrain, chunk_vertex_buffer, chunk_index_buffer, chunk_index_count, height_texture, false);
	objects_render_shadow(objects, pass);
	shadow_map_end(&occlusion->view, pass);

	command_buffer = gpu_commands();
	SDL_zero(targets);
	targets[0].texture = occlusion->sum_texture;
	targets[1].texture = occlusion->weight_texture;
	compute = SDL_BeginGPUComputePass(command_buffer, targets, 2, NULL, 0);
	if (compute == NULL)
	{
		return;
	}
	samplers[0].texture = height_texture;
	samplers[0].sampler = gpu.nearest_clamp;
	samplers[1].texture = occlusion->view.depth_texture;
	samplers[1].sampler = gpu.nearest_clamp;
	SDL_zero(uniforms);
	SDL_memcpy(uniforms.light_view_projection, occlusion->view.light_view_projection.m, sizeof(uniforms.light_view_projection));
	SDL_memcpy(uniforms.direction, direction, sizeof(float) * 3);
	uniforms.direction[3] = occlusion->directions_done == 0 ? 1.0f : 0.0f;
	uniforms.map_size = terrain->size;
	uniforms.cell_size = terrain->cell_size_meters;
	// (off the ground by a texel and a half of the depth map: no acne)
	uniforms.lift = 1.5f * extent / (float)occlusion->view.size + 0.05f;
	SDL_BindGPUComputePipeline(compute, occlusion->accumulate_pipeline);
	SDL_BindGPUComputeSamplers(compute, 0, samplers, 2);
	SDL_PushGPUComputeUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
	SDL_DispatchGPUCompute(compute, (uint32_t)((terrain->size + 255) / 256), (uint32_t)terrain->size, 1);
	SDL_EndGPUComputePass(compute);

	// The layers in the air, from the same depth map
	{
		int layer_size = map_file_sky_layer_size(terrain->size, terrain->cell_size_meters);
		layers_uniforms_t layers;

		targets[0].texture = occlusion->layer_sum_texture;
		targets[1].texture = occlusion->layer_weight_texture;
		compute = SDL_BeginGPUComputePass(command_buffer, targets, 2, NULL, 0);
		if (compute != NULL)
		{
			SDL_zero(layers);
			SDL_memcpy(layers.light_view_projection, uniforms.light_view_projection, sizeof(layers.light_view_projection));
			SDL_memcpy(layers.direction, uniforms.direction, sizeof(layers.direction));
			SDL_memcpy(layers.layer_heights, sky_occlusion_layer_heights, sizeof(layers.layer_heights));
			layers.map_size = terrain->size;
			layers.cell_size = terrain->cell_size_meters;
			layers.lift = uniforms.lift;
			layers.layer_size = layer_size;
			SDL_BindGPUComputePipeline(compute, occlusion->layers_pipeline);
			SDL_BindGPUComputeSamplers(compute, 0, samplers, 2);
			SDL_PushGPUComputeUniformData(command_buffer, 0, &layers, sizeof(layers));
			SDL_DispatchGPUCompute(compute, (uint32_t)((layer_size + 255) / 256), (uint32_t)(layer_size * SKY_OCCLUSION_LAYERS), 1);
			SDL_EndGPUComputePass(compute);
		}
	}
	// What blocks the sky, which way and how far (the light it sends back)
	{
		int bounce_size = map_file_sky_bounce_size(terrain->size, terrain->cell_size_meters);
		bounce_uniforms_t bounce;

		SDL_zero(targets);
		targets[0].texture = occlusion->bounce_sums_texture;
		compute = SDL_BeginGPUComputePass(command_buffer, targets, 1, NULL, 0);
		if (compute != NULL)
		{
			SDL_zero(bounce);
			SDL_memcpy(bounce.light_view_projection, uniforms.light_view_projection, sizeof(bounce.light_view_projection));
			SDL_memcpy(bounce.direction, uniforms.direction, sizeof(bounce.direction));
			bounce.map_size = terrain->size;
			bounce.cell_size = terrain->cell_size_meters;
			bounce.lift = uniforms.lift;
			bounce.bounce_size = bounce_size;
			bounce.depth_range = occlusion->view.depth_far - occlusion->view.depth_near;
			SDL_BindGPUComputePipeline(compute, occlusion->bounce_pipeline);
			SDL_BindGPUComputeSamplers(compute, 0, samplers, 2);
			SDL_PushGPUComputeUniformData(command_buffer, 0, &bounce, sizeof(bounce));
			SDL_DispatchGPUCompute(compute, (uint32_t)((bounce_size + 255) / 256), (uint32_t)bounce_size, 1);
			SDL_EndGPUComputePass(compute);
		}
	}
	occlusion->directions_done++;
}

// sum / weight into result (width x height)
static bool divide(SDL_GPUCommandBuffer *command_buffer, SDL_GPUComputePipeline *pipeline, SDL_GPUTexture *sum, SDL_GPUTexture *weight,
	SDL_GPUTexture *result, int width, int height)
{
	SDL_GPUStorageTextureReadWriteBinding targets[3];
	SDL_GPUComputePass *compute;
	finish_uniforms_t uniforms;

	SDL_zero(targets);
	targets[0].texture = sum;
	targets[1].texture = weight;
	targets[2].texture = result;
	compute = SDL_BeginGPUComputePass(command_buffer, targets, 3, NULL, 0);
	if (compute == NULL)
	{
		return false;
	}
	SDL_zero(uniforms);
	uniforms.width = width;
	uniforms.height = height;
	SDL_BindGPUComputePipeline(compute, pipeline);
	SDL_PushGPUComputeUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
	SDL_DispatchGPUCompute(compute, (uint32_t)((width + 255) / 256), (uint32_t)height, 1);
	SDL_EndGPUComputePass(compute);
	return true;
}

static void finish(sky_occlusion_t *occlusion, const terrain_t *terrain, SDL_GPUTexture *height_texture)
{
	SDL_GPUCommandBuffer *command_buffer = gpu_commands();
	int layer_size = map_file_sky_layer_size(terrain->size, terrain->cell_size_meters);
	int bounce_size = map_file_sky_bounce_size(terrain->size, terrain->cell_size_meters);
	bool bounced = false;

	(void)height_texture;
	{
		SDL_GPUStorageTextureReadWriteBinding targets[2];
		SDL_GPUComputePass *compute;
		bounce_finish_uniforms_t uniforms;

		SDL_zero(targets);
		targets[0].texture = occlusion->bounce_sums_texture;
		targets[1].texture = occlusion->next_bounce_texture;
		compute = SDL_BeginGPUComputePass(command_buffer, targets, 2, NULL, 0);
		if (compute != NULL)
		{
			SDL_zero(uniforms);
			uniforms.bounce_size = bounce_size;
			uniforms.farthest = SKY_OCCLUSION_BOUNCE_FARTHEST;
			SDL_BindGPUComputePipeline(compute, occlusion->bounce_finish_pipeline);
			SDL_PushGPUComputeUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
			SDL_DispatchGPUCompute(compute, (uint32_t)((bounce_size + 255) / 256), (uint32_t)bounce_size, 1);
			SDL_EndGPUComputePass(compute);
			bounced = true;
		}
	}
	if (bounced && divide(command_buffer, occlusion->finish_pipeline, occlusion->sum_texture, occlusion->weight_texture, occlusion->next_texture,
			terrain->size, terrain->size)
		&& divide(command_buffer, occlusion->finish_layers_pipeline, occlusion->layer_sum_texture, occlusion->layer_weight_texture,
			occlusion->next_layers_texture, layer_size, layer_size))
	{
		// The new ones in (the old drawn until now)
		release_texture(&occlusion->texture);
		release_texture(&occlusion->layers_texture);
		occlusion->texture = occlusion->next_texture;
		occlusion->next_texture = NULL;
		occlusion->layers_texture = occlusion->next_layers_texture;
		occlusion->next_layers_texture = NULL;
		occlusion->layer_size = layer_size;
		release_texture(&occlusion->bounce_texture);
		occlusion->bounce_texture = occlusion->next_bounce_texture;
		occlusion->next_bounce_texture = NULL;
		occlusion->bounce_size = bounce_size;
		occlusion->map_size = terrain->size;
		occlusion->ready = true;
		occlusion->log_pending = true;
		occlusion->cpu_pending = true;
		SDL_Log("Sky occlusion: worked out in %.1f s (%d directions)",
			(double)(SDL_GetPerformanceCounter() - occlusion->start_counter) / (double)SDL_GetPerformanceFrequency(), occlusion->direction_count);
	}
	stop_working(occlusion);
}

// Its progress logged every this many frames it works
#define SKY_OCCLUSION_PROGRESS_FRAMES 1000

bool sky_occlusion_step(sky_occlusion_t *occlusion, const terrain_t *terrain, objects_t *objects, const render_settings_t *settings,
	SDL_GPUTexture *height_texture, SDL_GPUBuffer *chunk_vertex_buffer, SDL_GPUBuffer *chunk_index_buffer, int chunk_index_count)
{
	int per_frame = SDL_clamp(settings->sky_occlusion_directions_per_frame, 1, 32);
	int prepared_pass;

	if (occlusion->failed || height_texture == NULL || chunk_vertex_buffer == NULL || gpu.command_buffer == NULL)
	{
		return false;
	}
	if (occlusion->requested)
	{
		occlusion->requested = false;
		if (!start_working(occlusion, terrain, settings))
		{
			occlusion->given_up = true;
			return false;
		}
	}
	if (!occlusion->working)
	{
		return false;
	}
	objects_prepare_sky_occlusion(objects, &occlusion->view, settings->sky_occlusion_least_height);
	prepared_pass = objects->prepared_pass;
	for (int i = 0; i < per_frame && occlusion->directions_done < occlusion->direction_count; i++)
	{
		do_direction(occlusion, terrain, objects, prepared_pass, height_texture, chunk_vertex_buffer, chunk_index_buffer, chunk_index_count);
	}
	objects->prepared_pass = -1;
	// (how far it has come, every SKY_OCCLUSION_PROGRESS_FRAMES frames: a
	// big map's takes minutes)
	if (++occlusion->steps_since_progress >= SKY_OCCLUSION_PROGRESS_FRAMES && occlusion->directions_done < occlusion->direction_count)
	{
		double seconds = (double)(SDL_GetPerformanceCounter() - occlusion->start_counter) / (double)SDL_GetPerformanceFrequency();
		double share = (double)occlusion->directions_done / (double)SDL_max(occlusion->direction_count, 1);

		occlusion->steps_since_progress = 0;
		SDL_Log("Sky occlusion: %d of %d directions (%.0f%%), %.0f s so far, about %.0f s left", occlusion->directions_done,
			occlusion->direction_count, 100.0 * share, seconds, share > 0.0 ? seconds * (1.0 - share) / share : 0.0);
	}
	if (occlusion->directions_done >= occlusion->direction_count)
	{
		occlusion->steps_since_progress = 0;
		finish(occlusion, terrain, height_texture);
	}
	return true;
}

// An R8 (texel_bytes 1) or RGBA8 (4) texture, SAMPLER and
// COMPUTE_STORAGE_WRITE, with these bytes
static SDL_GPUTexture *upload_bytes(int width, int height, int texel_bytes, const uint8_t *bytes)
{
	SDL_GPUTexture *texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D,
		texel_bytes == 4 ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8_UNORM, (uint32_t)width, (uint32_t)height, 1, 1,
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);

	if (texture != NULL)
	{
		gpu_upload_texture(texture, 0, 0, 0, 0, (uint32_t)width, (uint32_t)height, bytes, (size_t)width * (size_t)height * (size_t)texel_bytes);
	}
	return texture;
}

// The cells' summed-area table (from cells_cpu; none without it)
static void make_cell_sums(sky_occlusion_t *occlusion)
{
	int size = occlusion->map_size;
	int stride = size + 1;

	SDL_free(occlusion->cell_sums);
	occlusion->cell_sums = NULL;
	if (occlusion->cells_cpu == NULL || size <= 0)
	{
		return;
	}
	occlusion->cell_sums = SDL_calloc((size_t)stride * (size_t)stride, sizeof(uint32_t));
	if (occlusion->cell_sums == NULL)
	{
		return;
	}
	for (int z = 0; z < size; z++)
	{
		uint32_t row = 0;

		for (int x = 0; x < size; x++)
		{
			row += occlusion->cells_cpu[z * size + x];
			occlusion->cell_sums[(z + 1) * stride + x + 1] = occlusion->cell_sums[z * stride + x + 1] + row;
		}
	}
}

bool sky_occlusion_upload(sky_occlusion_t *occlusion, int map_size, const uint8_t *cells, int layer_size, const uint8_t *layers, int bounce_size,
	const uint8_t *bounce)
{
	if (occlusion->failed || cells == NULL || layers == NULL || bounce == NULL || map_size <= 0 || layer_size <= 0 || bounce_size <= 0)
	{
		return false;
	}
	stop_working(occlusion);
	release_texture(&occlusion->texture);
	release_texture(&occlusion->layers_texture);
	release_texture(&occlusion->bounce_texture);
	occlusion->texture = upload_bytes(map_size, map_size, 1, cells);
	{
		// (one under the other, as the map file has them, into one texel)
		size_t layer_count = (size_t)layer_size * (size_t)layer_size;
		uint8_t *packed = SDL_malloc(layer_count * SKY_OCCLUSION_LAYERS);

		if (packed != NULL)
		{
			for (size_t i = 0; i < layer_count; i++)
			{
				for (int k = 0; k < SKY_OCCLUSION_LAYERS; k++)
				{
					packed[i * SKY_OCCLUSION_LAYERS + (size_t)k] = layers[(size_t)k * layer_count + i];
				}
			}
			occlusion->layers_texture = upload_bytes(layer_size, layer_size, 4, packed);
			SDL_free(packed);
		}
	}
	occlusion->bounce_texture = upload_bytes(bounce_size, bounce_size, 4, bounce);
	if (occlusion->texture == NULL || occlusion->layers_texture == NULL || occlusion->bounce_texture == NULL)
	{
		release_texture(&occlusion->texture);
		release_texture(&occlusion->layers_texture);
		release_texture(&occlusion->bounce_texture);
		return false;
	}
	occlusion->map_size = map_size;
	occlusion->layer_size = layer_size;
	occlusion->bounce_size = bounce_size;
	occlusion->ready = true;
	occlusion->requested = false;
	// (and on the CPU: what the game asks)
	SDL_free(occlusion->cells_cpu);
	occlusion->cells_cpu = SDL_malloc((size_t)map_size * (size_t)map_size);
	if (occlusion->cells_cpu != NULL)
	{
		SDL_memcpy(occlusion->cells_cpu, cells, (size_t)map_size * (size_t)map_size);
	}
	occlusion->cpu_pending = false;
	make_cell_sums(occlusion);
	return true;
}

// An R8 (texel_bytes 1) or RGBA8 (4) texture's bytes (SDL_free them),
// waiting for the GPU; NULL if it fails
static uint8_t *download_bytes(SDL_GPUTexture *texture, int width, int height, int texel_bytes)
{
	size_t size = (size_t)width * (size_t)height * (size_t)texel_bytes;
	SDL_GPUTransferBuffer *download;
	SDL_GPUTransferBufferCreateInfo info;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUCopyPass *copy_pass;
	SDL_GPUTextureRegion source;
	SDL_GPUTextureTransferInfo destination;
	SDL_GPUFence *fence = NULL;
	uint8_t *bytes = NULL;

	if (texture == NULL || size == 0 || gpu.device == NULL)
	{
		return NULL;
	}
	SDL_zero(info);
	info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	info.size = (uint32_t)size;
	download = SDL_CreateGPUTransferBuffer(gpu.device, &info);
	if (download == NULL)
	{
		return NULL;
	}
	gpu_flush_uploads();
	command_buffer = gpu_commands();
	copy_pass = command_buffer != NULL ? SDL_BeginGPUCopyPass(command_buffer) : NULL;
	if (copy_pass != NULL)
	{
		SDL_zero(source);
		source.texture = texture;
		source.w = (uint32_t)width;
		source.h = (uint32_t)height;
		source.d = 1;
		SDL_zero(destination);
		destination.transfer_buffer = download;
		SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
		SDL_EndGPUCopyPass(copy_pass);
		gpu_submit_work(&fence);
	}
	if (fence != NULL)
	{
		void *mapped;

		SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
		SDL_ReleaseGPUFence(gpu.device, fence);
		mapped = SDL_MapGPUTransferBuffer(gpu.device, download, false);
		bytes = mapped != NULL ? SDL_malloc(size) : NULL;
		if (bytes != NULL)
		{
			SDL_memcpy(bytes, mapped, size);
		}
		if (mapped != NULL)
		{
			SDL_UnmapGPUTransferBuffer(gpu.device, download);
		}
	}
	SDL_ReleaseGPUTransferBuffer(gpu.device, download);
	return bytes;
}

uint8_t *sky_occlusion_download(sky_occlusion_t *occlusion, uint8_t **layers_out, uint8_t **bounce_out)
{
	uint8_t *cells;

	if (layers_out != NULL)
	{
		*layers_out = NULL;
	}
	if (bounce_out != NULL)
	{
		*bounce_out = NULL;
	}
	if (!occlusion->ready || occlusion->map_size <= 0)
	{
		return NULL;
	}
	cells = download_bytes(occlusion->texture, occlusion->map_size, occlusion->map_size, 1);
	if (cells != NULL && layers_out != NULL && occlusion->layer_size > 0)
	{
		uint8_t *packed = download_bytes(occlusion->layers_texture, occlusion->layer_size, occlusion->layer_size, 4);
		size_t layer_count = (size_t)occlusion->layer_size * (size_t)occlusion->layer_size;

		// (back one under the other, as the map file keeps them)
		*layers_out = packed != NULL ? SDL_malloc(layer_count * SKY_OCCLUSION_LAYERS) : NULL;
		if (*layers_out != NULL)
		{
			for (size_t i = 0; i < layer_count; i++)
			{
				for (int k = 0; k < SKY_OCCLUSION_LAYERS; k++)
				{
					(*layers_out)[(size_t)k * layer_count + i] = packed[i * SKY_OCCLUSION_LAYERS + (size_t)k];
				}
			}
		}
		SDL_free(packed);
	}
	if (cells != NULL && bounce_out != NULL && occlusion->bounce_size > 0)
	{
		*bounce_out = download_bytes(occlusion->bounce_texture, occlusion->bounce_size, occlusion->bounce_size, 4);
	}
	return cells;
}

void sky_occlusion_log(sky_occlusion_t *occlusion)
{
	uint8_t *cells;
	uint8_t *layers = NULL;
	size_t count;
	uint64_t histogram[10] = { 0 };
	uint64_t sum = 0;
	int least = 255;
	double layer_means[SKY_OCCLUSION_LAYERS] = { 0 };

	if (!occlusion->log_pending)
	{
		return;
	}
	occlusion->log_pending = false;
	cells = sky_occlusion_download(occlusion, &layers, NULL);
	if (cells == NULL)
	{
		SDL_free(layers);
		return;
	}
	count = (size_t)occlusion->map_size * (size_t)occlusion->map_size;
	for (size_t i = 0; i < count; i++)
	{
		sum += cells[i];
		least = SDL_min(least, (int)cells[i]);
		histogram[SDL_min(cells[i] * 10 / 255, 9)]++;
	}
	if (layers != NULL)
	{
		size_t layer_count = (size_t)occlusion->layer_size * (size_t)occlusion->layer_size;

		for (int k = 0; k < SKY_OCCLUSION_LAYERS; k++)
		{
			uint64_t layer_sum = 0;

			for (size_t i = 0; i < layer_count; i++)
			{
				layer_sum += layers[(size_t)k * layer_count + i];
			}
			layer_means[k] = (double)layer_sum / (double)layer_count / 255.0;
		}
	}
	SDL_Log("Sky occlusion: the cells see %.2f of the sky on average, at least %.2f; by tenths: %.0f%% %.0f%% %.0f%% %.0f%% %.0f%% %.0f%% %.0f%% %.0f%% %.0f%% %.0f%%",
		(double)sum / (double)count / 255.0, (double)least / 255.0,
		100.0 * (double)histogram[0] / (double)count, 100.0 * (double)histogram[1] / (double)count, 100.0 * (double)histogram[2] / (double)count,
		100.0 * (double)histogram[3] / (double)count, 100.0 * (double)histogram[4] / (double)count, 100.0 * (double)histogram[5] / (double)count,
		100.0 * (double)histogram[6] / (double)count, 100.0 * (double)histogram[7] / (double)count, 100.0 * (double)histogram[8] / (double)count,
		100.0 * (double)histogram[9] / (double)count);
	SDL_Log("Sky occlusion: in the air (%dx%d a layer), %.0f m up %.2f, %.0f m %.2f, %.0f m %.2f, %.0f m %.2f on average", occlusion->layer_size,
		occlusion->layer_size, (double)sky_occlusion_layer_heights[0], layer_means[0], (double)sky_occlusion_layer_heights[1], layer_means[1],
		(double)sky_occlusion_layer_heights[2], layer_means[2], (double)sky_occlusion_layer_heights[3], layer_means[3]);
	SDL_free(cells);
	SDL_free(layers);
}

void sky_occlusion_between_frames(sky_occlusion_t *occlusion)
{
	if (occlusion->cpu_pending)
	{
		occlusion->cpu_pending = false;
		SDL_free(occlusion->cells_cpu);
		occlusion->cells_cpu = sky_occlusion_download(occlusion, NULL, NULL);
		make_cell_sums(occlusion);
	}
	sky_occlusion_log(occlusion);
}

float sky_occlusion_open_at(const sky_occlusion_t *occlusion, float cell_size_meters, float x, float z)
{
	const uint8_t *cells = occlusion->cells_cpu;
	int size = occlusion->map_size;
	float u;
	float v;
	int cell_x;
	int cell_z;
	int next_x;
	int next_z;
	float fraction_x;
	float fraction_z;
	float top;
	float bottom;

	if (cells == NULL || size <= 0 || cell_size_meters <= 0.0f)
	{
		return 1.0f;
	}
	// (a cell's value is at its corner, as the terrain's heights are)
	u = SDL_clamp(x / cell_size_meters, 0.0f, (float)(size - 1));
	v = SDL_clamp(z / cell_size_meters, 0.0f, (float)(size - 1));
	cell_x = (int)u;
	cell_z = (int)v;
	next_x = SDL_min(cell_x + 1, size - 1);
	next_z = SDL_min(cell_z + 1, size - 1);
	fraction_x = u - (float)cell_x;
	fraction_z = v - (float)cell_z;
	top = (float)cells[cell_z * size + cell_x] + ((float)cells[cell_z * size + next_x] - (float)cells[cell_z * size + cell_x]) * fraction_x;
	bottom = (float)cells[next_z * size + cell_x] + ((float)cells[next_z * size + next_x] - (float)cells[next_z * size + cell_x]) * fraction_x;
	return (top + (bottom - top) * fraction_z) / 255.0f;
}

float sky_occlusion_open_around(const sky_occlusion_t *occlusion, float cell_size_meters, float x, float z, float radius_meters)
{
	int size = occlusion->map_size;
	int stride = size + 1;
	int first_x;
	int first_z;
	int last_x;
	int last_z;
	const uint32_t *sums = occlusion->cell_sums;
	uint32_t total;
	int count;

	if (sums == NULL || size <= 0 || cell_size_meters <= 0.0f)
	{
		return sky_occlusion_open_at(occlusion, cell_size_meters, x, z);
	}
	first_x = clamp_int((int)SDL_floorf((x - radius_meters) / cell_size_meters + 0.5f), 0, size - 1);
	first_z = clamp_int((int)SDL_floorf((z - radius_meters) / cell_size_meters + 0.5f), 0, size - 1);
	last_x = clamp_int((int)SDL_floorf((x + radius_meters) / cell_size_meters + 0.5f), 0, size - 1);
	last_z = clamp_int((int)SDL_floorf((z + radius_meters) / cell_size_meters + 0.5f), 0, size - 1);
	// (the cells first..last both ways: four corners of the table)
	total = sums[(last_z + 1) * stride + last_x + 1] - sums[first_z * stride + last_x + 1] - sums[(last_z + 1) * stride + first_x]
		+ sums[first_z * stride + first_x];
	count = (last_x - first_x + 1) * (last_z - first_z + 1);
	return (float)total / (255.0f * (float)count);
}
