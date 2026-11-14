// The ice on the water. See water_ice.h.

#include "water_ice.h"

#include <SDL3/SDL.h>

// It runs at most this often (seconds of game time between runs)
static const float ICE_STEP_SECONDS = 0.25f;
// Once the winter is over, all of it has melted in this long (water_ice.comp's
// ICE_MELT, for its thickest, and some): then one last pass clears what is
// left, and it rests
static const float ICE_LINGER_SECONDS = 1800.0f;

// water_ice.comp's block (std140)
typedef struct
{
	int32_t map_size;
	int32_t reset;
	float seconds;
	float winter;
	float snowfall;
	float clock;
	float frozen;
	float padding;
} ice_uniforms_t;

bool water_ice_create(water_ice_t *ice)
{
	gpu_compute_layout_t layout;

	SDL_zerop(ice);
	SDL_zero(layout);
	layout.samplers = 3;
	layout.uniform_buffers = 1;
	layout.threads_x = 256;
	layout.readwrite_storage_textures = 1;
	ice->pipeline = gpu_load_compute("water_ice.comp", &layout);
	if (ice->pipeline == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Ice: no water_ice.comp; the water never freezes");
		return false;
	}
	return true;
}

void water_ice_release(water_ice_t *ice)
{
	if (ice->texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, ice->texture);
		ice->texture = NULL;
	}
	ice->size = 0;
	ice->cleared = false;
	ice->resting = false;
	ice->idle_seconds = 0.0f;
}

void water_ice_destroy(water_ice_t *ice)
{
	water_ice_release(ice);
	if (ice->pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(gpu.device, ice->pipeline);
	}
	SDL_zerop(ice);
}

void water_ice_clear(water_ice_t *ice)
{
	ice->cleared = false;
}

SDL_GPUTexture *water_ice_texture(const water_ice_t *ice)
{
	return ice->texture != NULL && ice->cleared && !ice->resting ? ice->texture : NULL;
}

void water_ice_update(water_ice_t *ice, const water_ice_inputs_t *inputs)
{
	ice_uniforms_t uniforms;
	SDL_GPUStorageTextureReadWriteBinding target;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUComputePass *pass;
	bool reset = inputs->reset;
	static float clock = 0.0f;

	if (ice->pipeline == NULL || inputs->water_texture == NULL || inputs->flow_texture == NULL || inputs->height_texture == NULL
		|| inputs->map_size <= 0)
	{
		return;
	}
	if (ice->size != inputs->map_size)
	{
		if (ice->texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, ice->texture);
		}
		ice->texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, (uint32_t)inputs->map_size,
			(uint32_t)inputs->map_size, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
		ice->size = ice->texture != NULL ? inputs->map_size : 0;
		ice->cleared = false;
		if (ice->texture == NULL)
		{
			return;
		}
	}
	if (!ice->cleared)
	{
		reset = true;
	}
	// No winter: once all of it has melted, no work at all
	if (inputs->winter > 0.0f || reset)
	{
		ice->idle_seconds = 0.0f;
		ice->resting = false;
	}
	else
	{
		ice->idle_seconds += SDL_max(inputs->seconds, 0.0f);
		if (ice->resting)
		{
			return;
		}
		if (ice->idle_seconds > ICE_LINGER_SECONDS)
		{
			// (the season's own: none)
			reset = true;
			ice->resting = true;
		}
	}
	ice->pending_seconds += SDL_max(inputs->seconds, 0.0f);
	if (!reset && ice->pending_seconds < ICE_STEP_SECONDS)
	{
		return;
	}
	SDL_zero(uniforms);
	uniforms.map_size = inputs->map_size;
	uniforms.reset = reset ? 1 : 0;
	// (a long pause, the game sped up: no more than a few seconds at once)
	uniforms.seconds = SDL_min(ice->pending_seconds, 5.0f);
	uniforms.winter = SDL_clamp(inputs->winter, 0.0f, 1.0f);
	uniforms.snowfall = SDL_clamp(inputs->snowfall, 0.0f, 1.0f);
	clock = SDL_fmodf(clock + 0.618034f, 64.0f);
	uniforms.clock = clock;
	uniforms.frozen = inputs->frozen ? 1.0f : 0.0f;
	ice->pending_seconds = 0.0f;

	gpu_flush_uploads();
	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return;
	}
	SDL_zero(target);
	target.texture = ice->texture;
	pass = SDL_BeginGPUComputePass(command_buffer, &target, 1, NULL, 0);
	if (pass == NULL)
	{
		return;
	}
	{
		SDL_GPUTextureSamplerBinding samplers[3] =
		{
			{ inputs->water_texture, gpu.nearest_clamp },
			{ inputs->flow_texture, gpu.nearest_clamp },
			{ inputs->height_texture, gpu.nearest_clamp },
		};

		SDL_BindGPUComputePipeline(pass, ice->pipeline);
		SDL_BindGPUComputeSamplers(pass, 0, samplers, 3);
	}
	SDL_PushGPUComputeUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
	SDL_DispatchGPUCompute(pass, (uint32_t)(((int64_t)inputs->map_size * inputs->map_size + 255) / 256), 1, 1);
	SDL_EndGPUComputePass(pass);
	ice->cleared = true;
	ice->pass_count++;
	ice->reset_count += uniforms.reset;
}
