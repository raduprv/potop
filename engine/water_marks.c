// What the water leaves on things. See water_marks.h.

#include "water_marks.h"

#include <SDL3/SDL.h>

// It runs at most this often (seconds of game time between runs)
static const float MARKS_STEP_SECONDS = 0.25f;
// After the last water went, all of it is gone in this long
// (water_marks.comp's longest fade, and some): then it rests
static const float MARKS_LINGER_SECONDS = 2500.0f;

// water_marks.comp's block (std140)
typedef struct
{
	int32_t map_size;
	int32_t marks_size;
	float seconds;
	float clock;
} marks_uniforms_t;

bool water_marks_create(water_marks_t *marks)
{
	gpu_compute_layout_t layout;

	SDL_zerop(marks);
	SDL_zero(layout);
	layout.samplers = 1;
	layout.uniform_buffers = 1;
	layout.threads_x = 256;
	layout.readwrite_storage_textures = 1;
	marks->pipeline = gpu_load_compute("water_marks.comp", &layout);
	if (marks->pipeline == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Water marks: no water_marks.comp; the water leaves nothing on things");
		return false;
	}
	return true;
}

void water_marks_destroy(water_marks_t *marks)
{
	if (marks->pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(gpu.device, marks->pipeline);
	}
	if (marks->texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, marks->texture);
	}
	SDL_zerop(marks);
}

void water_marks_clear(water_marks_t *marks)
{
	marks->cleared = false;
}

void water_marks_update(water_marks_t *marks, SDL_GPUTexture *water_texture, int map_size, float seconds, bool water_present)
{
	marks_uniforms_t uniforms;
	SDL_GPUStorageTextureReadWriteBinding target;
	SDL_GPUTextureSamplerBinding sampler;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUComputePass *pass;
	int size = (map_size + 1) / 2;
	static float clock = 0.0f;

	if (marks->pipeline == NULL || water_texture == NULL || map_size <= 0)
	{
		return;
	}
	if (marks->size != size)
	{
		if (marks->texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, marks->texture);
		}
		marks->texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (uint32_t)size, (uint32_t)size, 1, 1,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
		marks->size = marks->texture != NULL ? size : 0;
		marks->map_size = map_size;
		marks->cleared = false;
		if (marks->texture == NULL)
		{
			return;
		}
	}
	// A new map (or a new texture): nothing on it
	if (!marks->cleared)
	{
		uint8_t *zeros = SDL_calloc((size_t)size * (size_t)size, 4);

		if (zeros == NULL)
		{
			return;
		}
		gpu_upload_texture(marks->texture, 0, 0, 0, 0, (uint32_t)size, (uint32_t)size, zeros, (size_t)size * (size_t)size * 4);
		SDL_free(zeros);
		marks->cleared = true;
		marks->resting = !water_present;
		marks->dry_seconds = MARKS_LINGER_SECONDS;
	}
	// No water, and none for long: no work at all
	if (water_present)
	{
		marks->dry_seconds = 0.0f;
		marks->resting = false;
	}
	else
	{
		marks->dry_seconds += SDL_max(seconds, 0.0f);
		marks->resting = marks->dry_seconds > MARKS_LINGER_SECONDS;
	}
	if (marks->resting)
	{
		return;
	}
	marks->pending_seconds += SDL_max(seconds, 0.0f);
	if (marks->pending_seconds < MARKS_STEP_SECONDS)
	{
		return;
	}

	SDL_zero(uniforms);
	uniforms.map_size = map_size;
	uniforms.marks_size = size;
	uniforms.seconds = SDL_min(marks->pending_seconds, 5.0f);
	clock = SDL_fmodf(clock + 0.618f, 1.0f);
	uniforms.clock = clock;
	marks->pending_seconds = 0.0f;

	gpu_flush_uploads();
	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return;
	}
	SDL_zero(target);
	target.texture = marks->texture;
	pass = SDL_BeginGPUComputePass(command_buffer, &target, 1, NULL, 0);
	if (pass == NULL)
	{
		return;
	}
	sampler.texture = water_texture;
	sampler.sampler = gpu.nearest_clamp;
	SDL_BindGPUComputePipeline(pass, marks->pipeline);
	SDL_BindGPUComputeSamplers(pass, 0, &sampler, 1);
	SDL_PushGPUComputeUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
	SDL_DispatchGPUCompute(pass, (uint32_t)(((int64_t)size * size + 255) / 256), 1, 1);
	SDL_EndGPUComputePass(pass);
}

void water_marks_release(water_marks_t *marks)
{
	if (marks->texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, marks->texture);
		marks->texture = NULL;
	}
	marks->size = 0;
	marks->cleared = false;
	marks->resting = true;
}
