// The depth pyramid. See hiz.h.

#include "hiz.h"

#include <SDL3/SDL.h>

static void release_levels(hiz_t *hiz)
{
	if (hiz->first_level != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, hiz->first_level);
		hiz->first_level = NULL;
	}
	hiz->width = 0;
	hiz->height = 0;
	hiz->level_count = 0;
}

bool hiz_create(hiz_t *hiz)
{
	// No samplers; level 0 as a storage image, the pyramid buffer read and written
	gpu_compute_layout_t layout = { 0, 0, 1, 1, 64, 1, 0 };
	gpu_pipeline_info_t info;
	SDL_GPUShader *vertex;
	SDL_GPUShader *fragment;

	SDL_zerop(hiz);
	if (gpu.device == NULL)
	{
		return false;
	}
	hiz->build_pipeline = gpu_load_compute("hiz_build.comp", &layout);
	vertex = gpu_load_shader("post.vert", 0, 0, 0);
	fragment = gpu_load_shader("hiz_first.frag", 1, 0, 1);
	if (vertex != NULL && fragment != NULL)
	{
		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.cull = SDL_GPU_CULLMODE_NONE;
		info.color_format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
		info.no_depth = true;
		hiz->first_pipeline = gpu_make_pipeline(&info, "hiz first level");
	}
	if (vertex != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, vertex);
	}
	if (fragment != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, fragment);
	}
	// A small buffer from the start: the culling always has one to bind
	hiz->buffer_capacity = 256;
	hiz->buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE
			| SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
		hiz->buffer_capacity);
	return hiz->build_pipeline != NULL && hiz->first_pipeline != NULL && hiz->buffer != NULL;
}

void hiz_destroy(hiz_t *hiz)
{
	if (gpu.device != NULL)
	{
		release_levels(hiz);
		if (hiz->buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, hiz->buffer);
		}
		if (hiz->build_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, hiz->build_pipeline);
		}
		if (hiz->first_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, hiz->first_pipeline);
		}
	}
	SDL_zerop(hiz);
}

// Floats in all the levels of a pyramid whose level 0 is width x height
static uint32_t pyramid_texels(uint32_t width, uint32_t height, uint32_t level_count)
{
	uint32_t total = 0;

	for (uint32_t i = 0; i < level_count; i++)
	{
		total += SDL_max(width >> i, 1u) * SDL_max(height >> i, 1u);
	}
	return total;
}

static bool resize(hiz_t *hiz, uint32_t width, uint32_t height)
{
	uint32_t level_count = SDL_min(gpu_mip_count(width, height), (uint32_t)HIZ_MAX_LEVELS);
	uint32_t bytes = pyramid_texels(width, height, level_count) * (uint32_t)sizeof(float);

	release_levels(hiz);
	hiz->first_level = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, width, height, 1, 1,
		SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ);
	if (hiz->first_level == NULL)
	{
		return false;
	}
	if (bytes > hiz->buffer_capacity)
	{
		SDL_GPUBuffer *buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE
			| SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, bytes);

		if (buffer == NULL)
		{
			release_levels(hiz);
			return false;
		}
		SDL_ReleaseGPUBuffer(gpu.device, hiz->buffer);
		hiz->buffer = buffer;
		hiz->buffer_capacity = bytes;
	}
	hiz->width = width;
	hiz->height = height;
	hiz->level_count = level_count;
	return true;
}

void hiz_build(hiz_t *hiz, SDL_GPUTexture *depth_texture, uint32_t width, uint32_t height, const float view_projection[16])
{
	uint32_t base_width = SDL_max((width + 1) / 2, 1u);
	uint32_t base_height = SDL_max((height + 1) / 2, 1u);
	SDL_GPUCommandBuffer *command_buffer = gpu.command_buffer;
	hiz->ready = false;
	if (hiz->build_pipeline == NULL || hiz->first_pipeline == NULL || depth_texture == NULL || command_buffer == NULL)
	{
		return;
	}
	if ((hiz->width != base_width || hiz->height != base_height || hiz->first_level == NULL) && !resize(hiz, base_width, base_height))
	{
		return;
	}
	// Level 0: the farthest of each 2 x 2 of the scene's depth (a fragment
	// shader: it samples the depth safely)
	{
		SDL_GPUColorTargetInfo target;
		SDL_GPURenderPass *pass;
		SDL_GPUTextureSamplerBinding sampler = { depth_texture, gpu.nearest_clamp };
		int32_t uniforms[4] = { (int32_t)width, (int32_t)height, (int32_t)base_width, (int32_t)base_height };

		SDL_zero(target);
		target.texture = hiz->first_level;
		target.load_op = SDL_GPU_LOADOP_DONT_CARE;
		target.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass(command_buffer, &target, 1, NULL);
		if (pass == NULL)
		{
			return;
		}
		SDL_BindGPUGraphicsPipeline(pass, hiz->first_pipeline);
		SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
		SDL_PushGPUFragmentUniformData(command_buffer, 0, uniforms, sizeof(uniforms));
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}
	// Level 0 into the buffer (and level 1 from the image), then two levels
	// at a time from the last one made, a compute pass each (so that each
	// reads what the one before wrote; the passes are what cost)
	{
		int32_t table[HIZ_MAX_LEVELS][4];
		uint32_t level = 0;

		hiz_level_table(hiz, table);
		while (level < hiz->level_count)
		{
			// Mode 0: level 0 (and 1); mode 1: levels level and level + 1 from level - 1
			bool from_image = level == 0;
			uint32_t source = from_image ? 0 : level - 1;
			bool has_second = level + 1 < hiz->level_count;
			SDL_GPUStorageBufferReadWriteBinding destination;
			SDL_GPUComputePass *pass;
			int32_t uniforms[12] = { table[source][1], table[source][2], table[level][1], table[level][2],
				has_second ? table[level + 1][1] : 0, has_second ? table[level + 1][2] : 0, from_image ? 0 : 1, 0,
				table[source][0], table[level][0], has_second ? table[level + 1][0] : 0, 0 };

			SDL_zero(destination);
			destination.buffer = hiz->buffer;
			pass = SDL_BeginGPUComputePass(command_buffer, NULL, 0, &destination, 1);
			if (pass == NULL)
			{
				return;
			}
			SDL_BindGPUComputePipeline(pass, hiz->build_pipeline);
			SDL_BindGPUComputeStorageTextures(pass, 0, &hiz->first_level, 1);
			SDL_PushGPUComputeUniformData(command_buffer, 0, uniforms, sizeof(uniforms));
			SDL_DispatchGPUCompute(pass, ((uint32_t)table[level][1] * (uint32_t)table[level][2] + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(pass);
			level += has_second ? 2 : 1;
		}
	}
	SDL_memcpy(hiz->view_projection, view_projection, sizeof(hiz->view_projection));
	hiz->ready = true;
}

void hiz_level_table(const hiz_t *hiz, int32_t table[HIZ_MAX_LEVELS][4])
{
	uint32_t offset = 0;

	SDL_memset(table, 0, sizeof(int32_t) * HIZ_MAX_LEVELS * 4);
	for (uint32_t i = 0; i < hiz->level_count && i < HIZ_MAX_LEVELS; i++)
	{
		uint32_t level_width = SDL_max(hiz->width >> i, 1u);
		uint32_t level_height = SDL_max(hiz->height >> i, 1u);

		table[i][0] = (int32_t)offset;
		table[i][1] = (int32_t)level_width;
		table[i][2] = (int32_t)level_height;
		offset += level_width * level_height;
	}
}

SDL_GPUBuffer *hiz_buffer(const hiz_t *hiz)
{
	return hiz->buffer;
}
