// What settles on the ground. See ground_overlay.h.

#include "ground_overlay.h"

#include <SDL3/SDL.h>

// It runs at most this often (seconds of game time between runs)
static const float OVERLAY_STEP_SECONDS = 0.25f;
// Once nothing falls any more, all of it is gone in this long (the snow's
// melting, ground_overlay.comp's SNOW_MELT_SECONDS, and some): then one
// last pass clears what is left, and it rests
static const float OVERLAY_LINGER_SECONDS = 300.0f;

// ground_overlay.comp's block (std140)
typedef struct
{
	int32_t map_size;
	int32_t reset;
	float seconds;
	float snowfall;
	float winter;
	float bloom;
	float sun_height;
	int32_t no_water;               // no water texture: none read
	float sun[4];                  // x, z towards it; cell size (m); the snow on things (a reset's)
	float sky[4];                  // x: 1 no sky texture (all of it seen)
} overlay_uniforms_t;

bool ground_overlay_create(ground_overlay_t *overlay)
{
	gpu_compute_layout_t layout;

	SDL_zerop(overlay);
	SDL_zero(layout);
	layout.samplers = 4;
	layout.uniform_buffers = 1;
	layout.threads_x = 256;
	layout.readwrite_storage_textures = 1;
	overlay->pipeline = gpu_load_compute("ground_overlay.comp", &layout);
	if (overlay->pipeline == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Ground overlay: no ground_overlay.comp; no snow on the ground but the snow line's");
		return false;
	}
	return true;
}

void ground_overlay_destroy(ground_overlay_t *overlay)
{
	if (overlay->pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(gpu.device, overlay->pipeline);
	}
	if (overlay->texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, overlay->texture);
	}
	SDL_zerop(overlay);
}

void ground_overlay_update(ground_overlay_t *overlay, const ground_overlay_inputs_t *inputs)
{
	overlay_uniforms_t uniforms;
	SDL_GPUStorageTextureReadWriteBinding target;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUComputePass *pass;
	bool reset = inputs->reset;

	if (overlay->pipeline == NULL || inputs->material_texture == NULL || inputs->normal_texture == NULL || inputs->map_size <= 0)
	{
		return;
	}
	if (overlay->size != inputs->map_size)
	{
		if (overlay->texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, overlay->texture);
		}
		overlay->texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, (uint32_t)inputs->map_size,
			(uint32_t)inputs->map_size, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
		overlay->size = overlay->texture != NULL ? inputs->map_size : 0;
		overlay->cleared = false;
		if (overlay->texture == NULL)
		{
			return;
		}
	}
	if (!overlay->cleared)
	{
		reset = true;
	}
	// Nothing to do (summer, no snow left): no work at all
	if (inputs->snowfall > 0.0f || inputs->winter > 0.0f || inputs->bloom > 0.0f || reset)
	{
		overlay->idle_seconds = 0.0f;
		overlay->resting = false;
	}
	else
	{
		overlay->idle_seconds += SDL_max(inputs->seconds, 0.0f);
		if (overlay->resting)
		{
			return;
		}
		if (overlay->idle_seconds > OVERLAY_LINGER_SECONDS)
		{
			// (the season's own: none of either)
			reset = true;
			overlay->resting = true;
		}
	}
	overlay->pending_seconds += SDL_max(inputs->seconds, 0.0f);
	if (!reset && overlay->pending_seconds < OVERLAY_STEP_SECONDS)
	{
		return;
	}

	SDL_zero(uniforms);
	uniforms.map_size = inputs->map_size;
	uniforms.reset = reset ? 1 : 0;
	// (a long pause, the game sped up: no more than a few seconds at once)
	uniforms.seconds = SDL_min(overlay->pending_seconds, 5.0f);
	uniforms.snowfall = SDL_clamp(inputs->snowfall, 0.0f, 1.0f);
	uniforms.winter = SDL_clamp(inputs->winter, 0.0f, 1.0f);
	uniforms.bloom = SDL_clamp(inputs->bloom, 0.0f, 1.0f);
	uniforms.sun_height = inputs->sun_height;
	uniforms.sun[0] = inputs->sun_x;
	uniforms.sun[1] = inputs->sun_z;
	uniforms.sun[2] = inputs->cell_size_meters;
	uniforms.sun[3] = SDL_clamp(inputs->snow_amount, 0.0f, 1.0f);
	overlay->pending_seconds = 0.0f;

	gpu_flush_uploads();
	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return;
	}
	SDL_zero(target);
	target.texture = overlay->texture;
	pass = SDL_BeginGPUComputePass(command_buffer, &target, 1, NULL, 0);
	if (pass == NULL)
	{
		return;
	}
	{
		SDL_GPUTextureSamplerBinding samplers[4] =
		{
			{ inputs->material_texture, gpu.nearest_clamp },
			{ inputs->normal_texture, gpu.nearest_clamp },
			// (no water: the material map stands in, never deep)
			{ inputs->water_texture != NULL ? inputs->water_texture : inputs->material_texture, gpu.nearest_clamp },
			// (no sky's occlusion yet: the material map stands in, not read)
			{ inputs->sky_texture != NULL ? inputs->sky_texture : inputs->material_texture, gpu.nearest_clamp },
		};

		SDL_BindGPUComputePipeline(pass, overlay->pipeline);
		SDL_BindGPUComputeSamplers(pass, 0, samplers, 4);
	}
	if (inputs->water_texture == NULL)
	{
		uniforms.no_water = 1;
	}
	uniforms.sky[0] = inputs->sky_texture == NULL ? 1.0f : 0.0f;
	SDL_PushGPUComputeUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
	SDL_DispatchGPUCompute(pass, (uint32_t)(((int64_t)inputs->map_size * inputs->map_size + 255) / 256), 1, 1);
	SDL_EndGPUComputePass(pass);
	overlay->cleared = true;
	overlay->pass_count++;
	overlay->reset_count += uniforms.reset;
}
