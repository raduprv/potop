// Offscreen scene target. See render_target.h.

#include "render_target.h"

#include <SDL3/SDL.h>

static void release_texture(SDL_GPUTexture **texture)
{
	if (*texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, *texture);
		*texture = NULL;
	}
}

static void release(render_target_t *target)
{
	release_texture(&target->color_texture);
	release_texture(&target->depth_texture);
	release_texture(&target->multisampled_color_texture);
	release_texture(&target->multisampled_depth_value_texture);
	release_texture(&target->multisampled_depth_texture);
	release_texture(&target->depth_value_texture);
	target->width = 0;
	target->height = 0;
	target->samples = 0;
}

// A multisampled render target (gpu_create_texture makes plain ones only)
static SDL_GPUTexture *create_multisampled(SDL_GPUTextureFormat format, int width, int height, SDL_GPUTextureUsageFlags usage)
{
	SDL_GPUTextureCreateInfo info;

	SDL_zero(info);
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = format;
	info.usage = usage;
	info.width = (uint32_t)width;
	info.height = (uint32_t)height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = gpu.scene_sample_count;
	return SDL_CreateGPUTexture(gpu.device, &info);
}

// Its samples a pixel: the device's for the world's scene, else 1
static uint32_t wanted_samples(const render_target_t *target)
{
	return target->world_scene ? gpu.scene_samples : 1u;
}

static bool create(render_target_t *target, int width, int height)
{
	bool made;

	release(target);
	target->color_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, GPU_SCENE_COLOR_FORMAT, (uint32_t)width, (uint32_t)height, 1, 1,
		SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
	target->depth_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.depth_format, (uint32_t)width, (uint32_t)height, 1, 1,
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
	made = target->color_texture != NULL && target->depth_texture != NULL;
	if (made && wanted_samples(target) > 1)
	{
		target->multisampled_color_texture = create_multisampled(GPU_SCENE_COLOR_FORMAT, width, height, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		target->multisampled_depth_value_texture = create_multisampled(GPU_SCENE_DEPTH_VALUE_FORMAT, width, height,
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		target->multisampled_depth_texture = create_multisampled(gpu.depth_format, width, height, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
		target->depth_value_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, GPU_SCENE_DEPTH_VALUE_FORMAT, (uint32_t)width,
			(uint32_t)height, 1, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		made = target->multisampled_color_texture != NULL && target->multisampled_depth_value_texture != NULL
			&& target->multisampled_depth_texture != NULL && target->depth_value_texture != NULL;
	}
	if (!made)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot make the scene's %d x %d target: %s", width, height, SDL_GetError());
		release(target);
		return false;
	}
	target->width = width;
	target->height = height;
	target->samples = wanted_samples(target);
	SDL_Log("Rendering the scene at %d x %d (%u samples a pixel)", width, height, target->samples);
	return true;
}

// The pass that writes the resolved depth values into the plain depth:
// one triangle over it all, always passing, no color
static void create_depth_copy(render_target_t *target)
{
	gpu_pipeline_info_t info;

	target->depth_copy_tried = true;
	SDL_zero(info);
	info.vertex_shader = gpu_load_shader("post.vert", 0, 0, 0);
	info.fragment_shader = gpu_load_shader("depth_copy.frag", 1, 0, 0);
	info.cull = SDL_GPU_CULLMODE_NONE;
	info.depth_test = true;
	info.depth_write = true;
	info.depth_compare = SDL_GPU_COMPAREOP_ALWAYS;
	info.depth_only = true;
	info.depth_format = gpu.depth_format;
	if (info.vertex_shader != NULL && info.fragment_shader != NULL)
	{
		target->depth_copy_pipeline = gpu_make_pipeline(&info, "scene depth copy");
	}
	if (info.vertex_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.vertex_shader);
	}
	if (info.fragment_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.fragment_shader);
	}
}

// The multisampled pass ended (its color and depth values resolved): the
// depth values into the plain depth, for what comes after
static void finish_multisampled(render_target_t *target)
{
	SDL_GPUDepthStencilTargetInfo depth;
	SDL_GPURenderPass *pass;

	target->multisampled_open = false;
	if (!target->depth_copy_tried)
	{
		create_depth_copy(target);
	}
	if (target->depth_copy_pipeline == NULL || gpu.command_buffer == NULL)
	{
		return;
	}
	SDL_zero(depth);
	depth.texture = target->depth_texture;
	depth.load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.store_op = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	pass = SDL_BeginGPURenderPass(gpu.command_buffer, NULL, 0, &depth);
	if (pass != NULL)
	{
		SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f };
		SDL_GPUTextureSamplerBinding binding = { target->depth_value_texture, gpu.nearest_clamp };

		SDL_SetGPUViewport(pass, &viewport);
		SDL_BindGPUGraphicsPipeline(pass, target->depth_copy_pipeline);
		SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}
}

SDL_GPURenderPass *render_target_begin(render_target_t *target, int window_width, int window_height, float render_scale,
	const float clear_color[4], int *scene_width_out, int *scene_height_out)
{
	// (past 1: drawn larger than the window and averaged down in the post
	// pass, its bilinear tap at 2 the mean of each 2 x 2: anti-aliasing)
	float scale = SDL_clamp(render_scale, 0.25f, 2.0f);
	int width = SDL_max((int)SDL_lroundf((float)window_width * scale), 1);
	int height = SDL_max((int)SDL_lroundf((float)window_height * scale), 1);
	SDL_GPUColorTargetInfo colors[2];
	SDL_GPUDepthStencilTargetInfo depth;

	*scene_width_out = width;
	*scene_height_out = height;
	if (gpu.command_buffer == NULL || gpu.final_texture == NULL)
	{
		return NULL;
	}
	if ((target->width != width || target->height != height || target->samples != wanted_samples(target)) && !create(target, width, height))
	{
		return NULL;
	}
	gpu_flush_uploads();
	SDL_zeroa(colors);
	colors[0].texture = target->color_texture;
	colors[0].clear_color.r = clear_color[0];
	colors[0].clear_color.g = clear_color[1];
	colors[0].clear_color.b = clear_color[2];
	colors[0].clear_color.a = clear_color[3];
	colors[0].load_op = SDL_GPU_LOADOP_CLEAR;
	colors[0].store_op = SDL_GPU_STOREOP_STORE;
	SDL_zero(depth);
	depth.texture = target->depth_texture;
	depth.clear_depth = 1.0f;
	depth.load_op = SDL_GPU_LOADOP_CLEAR;
	// Kept: the pass pauses for the depth pyramid (render_target_pause)
	depth.store_op = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	if (target->samples > 1)
	{
		// Multisampled: resolved as the pass ends, the samples themselves
		// not kept (the pass is never opened again on them)
		colors[0].texture = target->multisampled_color_texture;
		colors[0].store_op = SDL_GPU_STOREOP_RESOLVE;
		colors[0].resolve_texture = target->color_texture;
		colors[1].texture = target->multisampled_depth_value_texture;
		colors[1].clear_color.r = 1.0f;
		colors[1].clear_color.a = 1.0f;
		colors[1].load_op = SDL_GPU_LOADOP_CLEAR;
		colors[1].store_op = SDL_GPU_STOREOP_RESOLVE;
		colors[1].resolve_texture = target->depth_value_texture;
		depth.texture = target->multisampled_depth_texture;
		depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
		target->multisampled_open = true;
		return SDL_BeginGPURenderPass(gpu.command_buffer, colors, 2, &depth);
	}
	return SDL_BeginGPURenderPass(gpu.command_buffer, colors, 1, &depth);
}

void render_target_pause(render_target_t *target, SDL_GPURenderPass *pass)
{
	if (pass != NULL)
	{
		SDL_EndGPURenderPass(pass);
	}
	if (target->multisampled_open)
	{
		finish_multisampled(target);
	}
}

SDL_GPURenderPass *render_target_resume(render_target_t *target)
{
	SDL_GPUColorTargetInfo color;
	SDL_GPUDepthStencilTargetInfo depth;
	SDL_GPURenderPass *pass;

	if (gpu.command_buffer == NULL || target->color_texture == NULL)
	{
		return NULL;
	}
	SDL_zero(color);
	color.texture = target->color_texture;
	color.load_op = SDL_GPU_LOADOP_LOAD;
	color.store_op = SDL_GPU_STOREOP_STORE;
	SDL_zero(depth);
	depth.texture = target->depth_texture;
	depth.load_op = SDL_GPU_LOADOP_LOAD;
	// Kept again: the volumetric clouds read it after the pass
	depth.store_op = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	pass = SDL_BeginGPURenderPass(gpu.command_buffer, &color, 1, &depth);
	if (pass != NULL)
	{
		SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f };

		SDL_SetGPUViewport(pass, &viewport);
	}
	return pass;
}

void render_target_end(render_target_t *target, SDL_GPURenderPass *pass)
{
	if (pass != NULL)
	{
		SDL_EndGPURenderPass(pass);
	}
	if (target->multisampled_open)
	{
		finish_multisampled(target);
	}
}

void render_target_destroy(render_target_t *target)
{
	release(target);
	if (target->depth_copy_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, target->depth_copy_pipeline);
	}
	SDL_zerop(target);
}
