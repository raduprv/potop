// The sky. See sky_renderer.h.

#include "sky_renderer.h"

#include <SDL3/SDL.h>

#include "file_io.h"
#include "stb_image.h"

// The moon's map (see sky.frag): rgb its color, a its height; a gray one
// if the file is not there
#define MOON_MAP "textures/moon.png"

static SDL_GPUTexture *load_moon(void)
{
	size_t size = 0;
	void *data = file_io_load_data_file(MOON_MAP, &size);
	int width = 0;
	int height = 0;
	int channels = 0;
	uint8_t *pixels = data != NULL ? stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4) : NULL;
	static uint8_t gray[4] = { 150, 150, 150, 128 };
	SDL_GPUTexture *texture;

	SDL_free(data);
	if (pixels == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Sky: no %s, the moon is plain", MOON_MAP);
		width = 1;
		height = 1;
	}
	texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (uint32_t)width, (uint32_t)height, 1,
		gpu_mip_count((uint32_t)width, (uint32_t)height), SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	if (texture != NULL)
	{
		gpu_upload_texture(texture, 0, 0, 0, 0, (uint32_t)width, (uint32_t)height, pixels != NULL ? pixels : gray,
			(size_t)width * (size_t)height * 4);
		gpu_generate_mipmaps(texture);
	}
	if (pixels != NULL)
	{
		stbi_image_free(pixels);
	}
	return texture;
}

bool sky_renderer_create(sky_renderer_t *renderer)
{
	gpu_pipeline_info_t pipeline;

	SDL_zerop(renderer);
	SDL_zero(pipeline);
	pipeline.vertex_shader = gpu_load_shader("sky.vert", 0, 0, 1);
	pipeline.fragment_shader = gpu_load_shader("sky.frag", 3, 0, 1);
	renderer->moon_texture = load_moon();
	// Drawn at the far plane: the depth test passes only where nothing else
	// was drawn, and it writes no depth
	pipeline.cull = SDL_GPU_CULLMODE_NONE;
	pipeline.depth_test = true;
	pipeline.depth_write = false;
	pipeline.multisampled = true;
	renderer->pipeline = gpu_make_pipeline(&pipeline, "sky");
	SDL_ReleaseGPUShader(gpu.device, pipeline.vertex_shader);
	SDL_ReleaseGPUShader(gpu.device, pipeline.fragment_shader);
	return renderer->pipeline != NULL;
}

void sky_renderer_destroy(sky_renderer_t *renderer)
{
	if (renderer->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, renderer->pipeline);
	}
	if (renderer->moon_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, renderer->moon_texture);
	}
	SDL_zerop(renderer);
}

float sky_horizon_drop(const camera_t *camera, const terrain_t *terrain, const render_settings_t *settings)
{
	int chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;
	double ground_sum = 0.0;
	float typical_ground;
	float height;
	float extent = (float)terrain->size * terrain->cell_size_meters;
	// The fog leaves 5% of the land at sqrt(3) / density (it goes as
	// exp(-(distance * density)^2)); without fog, the map's size
	float haze_distance = settings->fog_density > 0.0f ? SDL_min(1.732f / settings->fog_density, extent) : extent;

	for (int chunk = 0; chunk < chunk_count; chunk++)
	{
		ground_sum += (double)terrain->chunks[chunk].minimum_height_meters;
	}
	typical_ground = chunk_count > 0 ? (float)(ground_sum / (double)chunk_count) : 0.0f;
	height = SDL_max(camera->eye_position.y - typical_ground, 0.0f);
	return SDL_min(height / SDL_sqrtf(height * height + haze_distance * haze_distance), 0.5f);
}

void sky_renderer_draw(sky_renderer_t *renderer, SDL_GPURenderPass *pass, SDL_GPUTexture *variation_texture, SDL_GPUTexture *dome_texture)
{
	SDL_GPUTextureSamplerBinding samplers[3] =
	{
		{ variation_texture, gpu.linear_repeat },
		{ dome_texture != NULL ? dome_texture : variation_texture, gpu.linear_clamp },
		{ renderer->moon_texture != NULL ? renderer->moon_texture : variation_texture, gpu.linear_repeat },
	};

	if (pass == NULL || renderer->pipeline == NULL)
	{
		return;
	}
	SDL_BindGPUGraphicsPipeline(pass, renderer->pipeline);
	SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}
