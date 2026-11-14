// The ground and water textures the objects' shaders read. See
// scene_uniforms.h (scene_ground_set). Apart from scene_uniforms.c: the
// tools' previews link this alone.

#include "scene_uniforms.h"

#include "gpu.h"

static SDL_GPUTexture *ground_height_texture;
static SDL_GPUTexture *ground_water_texture;
static SDL_GPUTexture *ground_marks_texture;
static SDL_GPUTexture *marks_stand_in;       // 1x1 RGBA8, zero
static SDL_GPUTexture *ground_stand_in;      // 1x1 R32F, zero
static SDL_GPUTexture *ground_sky_occlusion;
static SDL_GPUTexture *ground_sky_layers;
static SDL_GPUTexture *ground_sky_bounce;
static SDL_GPUTexture *sky_stand_in;         // 1x1 R8, white (not read: scene sky_occlusion.y 0)

void scene_ground_set(SDL_GPUTexture *height_texture, SDL_GPUTexture *water_texture, SDL_GPUTexture *marks_texture)
{
	ground_height_texture = height_texture;
	ground_water_texture = water_texture;
	ground_marks_texture = marks_texture;
}

void scene_ground_set_sky_occlusion(SDL_GPUTexture *sky_occlusion_texture, SDL_GPUTexture *layers_texture, SDL_GPUTexture *bounce_texture)
{
	ground_sky_occlusion = sky_occlusion_texture;
	ground_sky_layers = layers_texture;
	ground_sky_bounce = bounce_texture;
}

static SDL_GPUTexture *sky_texture(SDL_GPUTexture *texture)
{
	if (texture != NULL)
	{
		return texture;
	}
	if (sky_stand_in == NULL)
	{
		uint8_t white = 255;

		sky_stand_in = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 1, 1, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (sky_stand_in != NULL)
		{
			gpu_upload_texture(sky_stand_in, 0, 0, 0, 0, 1, 1, &white, 1);
		}
	}
	return sky_stand_in;
}

bool scene_ground_marks_ready(void)
{
	return scene_ground_ready() && ground_marks_texture != NULL;
}

bool scene_ground_ready(void)
{
	return ground_height_texture != NULL && ground_water_texture != NULL;
}

void scene_ground_bind(SDL_GPURenderPass *pass, uint32_t first)
{
	SDL_GPUTextureSamplerBinding bindings[6];

	if (ground_stand_in == NULL)
	{
		float zero = 0.0f;

		ground_stand_in = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 1, 1, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (ground_stand_in != NULL)
		{
			gpu_upload_texture(ground_stand_in, 0, 0, 0, 0, 1, 1, &zero, sizeof(zero));
		}
	}
	bindings[0].texture = scene_ground_ready() ? ground_height_texture : ground_stand_in;
	bindings[0].sampler = gpu.nearest_clamp;
	bindings[1].texture = scene_ground_ready() ? ground_water_texture : ground_stand_in;
	bindings[1].sampler = gpu.nearest_clamp;
	if (marks_stand_in == NULL)
	{
		uint8_t zero[4] = { 0, 0, 0, 0 };

		marks_stand_in = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (marks_stand_in != NULL)
		{
			gpu_upload_texture(marks_stand_in, 0, 0, 0, 0, 1, 1, zero, sizeof(zero));
		}
	}
	bindings[2].texture = scene_ground_marks_ready() ? ground_marks_texture : marks_stand_in;
	bindings[2].sampler = gpu.nearest_clamp;
	bindings[3].texture = sky_texture(ground_sky_occlusion);
	bindings[3].sampler = gpu.linear_clamp;
	bindings[4].texture = sky_texture(ground_sky_layers);
	bindings[4].sampler = gpu.linear_clamp;
	bindings[5].texture = sky_texture(ground_sky_bounce);
	bindings[5].sampler = gpu.linear_clamp;
	if (bindings[0].texture != NULL && bindings[2].texture != NULL && bindings[3].texture != NULL)
	{
		SDL_BindGPUFragmentSamplers(pass, first, bindings, 6);
	}
}

void scene_ground_bind_sky(SDL_GPURenderPass *pass, uint32_t first)
{
	SDL_GPUTextureSamplerBinding bindings[4];

	bindings[0].texture = scene_ground_ready() ? ground_height_texture : ground_stand_in;
	bindings[0].sampler = gpu.nearest_clamp;
	bindings[1].texture = sky_texture(ground_sky_occlusion);
	bindings[1].sampler = gpu.linear_clamp;
	bindings[2].texture = sky_texture(ground_sky_layers);
	bindings[2].sampler = gpu.linear_clamp;
	bindings[3].texture = sky_texture(ground_sky_bounce);
	bindings[3].sampler = gpu.linear_clamp;
	if (bindings[0].texture != NULL && bindings[1].texture != NULL)
	{
		SDL_BindGPUFragmentSamplers(pass, first, bindings, 4);
	}
}

void scene_ground_bind_sky_vertex(SDL_GPURenderPass *pass, uint32_t first)
{
	SDL_GPUTextureSamplerBinding bindings[2];

	bindings[0].texture = sky_texture(ground_sky_occlusion);
	bindings[0].sampler = gpu.linear_clamp;
	bindings[1].texture = sky_texture(ground_sky_layers);
	bindings[1].sampler = gpu.linear_clamp;
	if (bindings[0].texture != NULL && bindings[1].texture != NULL)
	{
		SDL_BindGPUVertexSamplers(pass, first, bindings, 2);
	}
}

void scene_ground_bind_sky_compute(SDL_GPUComputePass *compute, uint32_t first)
{
	SDL_GPUTextureSamplerBinding bindings[3];

	if (ground_stand_in == NULL)
	{
		float zero = 0.0f;

		ground_stand_in = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 1, 1, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (ground_stand_in != NULL)
		{
			gpu_upload_texture(ground_stand_in, 0, 0, 0, 0, 1, 1, &zero, sizeof(zero));
		}
	}
	bindings[0].texture = scene_ground_ready() ? ground_height_texture : ground_stand_in;
	bindings[0].sampler = gpu.nearest_clamp;
	bindings[1].texture = sky_texture(ground_sky_occlusion);
	bindings[1].sampler = gpu.linear_clamp;
	bindings[2].texture = sky_texture(ground_sky_layers);
	bindings[2].sampler = gpu.linear_clamp;
	if (bindings[0].texture != NULL && bindings[1].texture != NULL && bindings[2].texture != NULL)
	{
		SDL_BindGPUComputeSamplers(compute, first, bindings, 3);
	}
}

void scene_ground_bind_sky_vertex4(SDL_GPURenderPass *pass, uint32_t first, SDL_GPUTexture *far_shadow)
{
	SDL_GPUTextureSamplerBinding bindings[4];

	bindings[0].texture = sky_texture(ground_sky_occlusion);
	bindings[0].sampler = gpu.linear_clamp;
	bindings[1].texture = sky_texture(ground_sky_layers);
	bindings[1].sampler = gpu.linear_clamp;
	bindings[2].texture = sky_texture(ground_sky_bounce);
	bindings[2].sampler = gpu.linear_clamp;
	bindings[3].texture = far_shadow;
	bindings[3].sampler = gpu.shadow_compare;
	if (bindings[0].texture != NULL && bindings[1].texture != NULL && bindings[2].texture != NULL && far_shadow != NULL)
	{
		SDL_BindGPUVertexSamplers(pass, first, bindings, 4);
	}
}
