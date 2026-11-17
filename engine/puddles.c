// Puddles: the scene they mirror (puddles.h)

#include "puddles.h"

#include "gpu.h"

// The pass's own block
typedef struct
{
	float depth_to_distance[4];    // x, y: the copy's depth to the distance along the view, y / (depth - x); z, w: 1 / its size
	float flags[4];                // x: 1 if the water's depth is bound
} puddles_uniforms_t;

static void create(puddles_t *puddles)
{
	gpu_pipeline_info_t info;

	puddles->tried = true;
	SDL_zero(info);
	// (the clouds' full screen triangle)
	info.vertex_shader = gpu_load_shader("clouds.vert", 0, 0, 1);
	info.fragment_shader = gpu_load_shader("puddles.frag", 8, 0, 2);
	info.cull = SDL_GPU_CULLMODE_NONE;
	info.no_depth = true;
	info.blend = GPU_BLEND_ALPHA;
	info.color_format = 0;
	if (info.vertex_shader != NULL && info.fragment_shader != NULL)
	{
		puddles->pipeline = gpu_make_pipeline(&info, "puddles");
	}
	if (info.vertex_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.vertex_shader);
	}
	if (info.fragment_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.fragment_shader);
	}
	if (puddles->pipeline == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Puddles: their pipeline was refused (%s); they mirror the sky alone", SDL_GetError());
	}
}

void puddles_destroy(puddles_t *puddles)
{
	if (puddles->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, puddles->pipeline);
	}
	SDL_zerop(puddles);
}

void puddles_draw(puddles_t *puddles, SDL_GPUTexture *color_texture, int width, int height, SDL_GPUTexture *scene_color,
	SDL_GPUTexture *scene_depth, const scene_uniforms_t *scene, const camera_t *camera, const terrain_renderer_t *terrain_renderer,
	SDL_GPUTexture *water_depth_texture)
{
	SDL_GPUColorTargetInfo color;
	SDL_GPURenderPass *pass;
	puddles_uniforms_t uniforms;
	SDL_GPUTexture *variation = terrain_renderer->materials.variation_texture;

	if (!puddles->tried)
	{
		create(puddles);
	}
	if (puddles->pipeline == NULL || gpu.command_buffer == NULL || color_texture == NULL || scene_color == NULL || scene_depth == NULL
		|| terrain_renderer->height_texture == NULL || terrain_renderer->normal_texture == NULL || terrain_renderer->surface_texture == NULL
		|| variation == NULL || width <= 0 || height <= 0)
	{
		return;
	}
	// (OpenGL's perspective, its depth taken to 0..1: depth = x + y / distance)
	SDL_zero(uniforms);
	uniforms.depth_to_distance[0] = 0.5f - 0.5f * camera->projection.m[10];
	uniforms.depth_to_distance[1] = 0.5f * camera->projection.m[14];
	uniforms.depth_to_distance[2] = 1.0f / (float)width;
	uniforms.depth_to_distance[3] = 1.0f / (float)height;
	uniforms.flags[0] = water_depth_texture != NULL ? 1.0f : 0.0f;
	{
		// (the stand-ins for what is not there are never read: the scene's
		// sky_occlusion.y, the flag above)
		SDL_GPUTextureSamplerBinding textures[8] =
		{
			{ scene_color, gpu.linear_clamp },
			{ scene_depth, gpu.nearest_clamp },
			{ terrain_renderer->height_texture, gpu.nearest_clamp },
			{ water_depth_texture != NULL ? water_depth_texture : terrain_renderer->height_texture, gpu.nearest_clamp },
			{ terrain_renderer->sky_occlusion_texture != NULL ? terrain_renderer->sky_occlusion_texture : variation, gpu.linear_clamp },
			{ terrain_renderer->normal_texture, gpu.linear_clamp },
			{ terrain_renderer->surface_texture, gpu.linear_clamp },
			{ variation, gpu.linear_repeat },
		};
		SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };

		SDL_zero(color);
		color.texture = color_texture;
		color.load_op = SDL_GPU_LOADOP_LOAD;
		color.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass(gpu.command_buffer, &color, 1, NULL);
		if (pass == NULL)
		{
			return;
		}
		SDL_SetGPUViewport(pass, &viewport);
		SDL_BindGPUGraphicsPipeline(pass, puddles->pipeline);
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
		SDL_BindGPUFragmentSamplers(pass, 0, textures, 8);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}
}
