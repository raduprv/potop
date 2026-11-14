// Ambient occlusion from the scene's depth (ambient_occlusion.h)

#include "ambient_occlusion.h"

#include "gpu.h"

// The shaders' own block (the same in both)
typedef struct
{
	float parameters[4];           // x: radius (m), y: the distance it is gone at (m), z: strength, w: 1 if the shadow maps are bound
	float size[4];                 // the scene's width and height, the half size target's
	float frame[4];                // x: the frame (the pattern's turn), y: 1 if there is a history
} occlusion_uniforms_t;

// Step 1b's own block
typedef struct
{
	float previous_view_projection[16];   // last frame's view_projection_relative
	float eye_moved[4];                   // this frame's eye minus last frame's
	float size[4];                        // the scene's width and height, the half size target's
	float history[4];                     // x: the history's weight, y: 1 if there is a history
} temporal_uniforms_t;

// The history's share each frame where it holds (an eighth new: the
// flicker averaged over about eight frames)
#define OCCLUSION_HISTORY_WEIGHT 0.875f
// The eye moved more than this in a frame (a jump): no history
#define OCCLUSION_HISTORY_JUMP_METERS 4.0f

static SDL_GPUGraphicsPipeline *make_pass(const char *fragment_name, uint32_t samplers, SDL_GPUTextureFormat format, gpu_blend_t blend,
	const char *name)
{
	gpu_pipeline_info_t info;
	SDL_GPUGraphicsPipeline *pipeline = NULL;

	SDL_zero(info);
	// (the clouds' full screen triangle)
	info.vertex_shader = gpu_load_shader("clouds.vert", 0, 0, 1);
	info.fragment_shader = gpu_load_shader(fragment_name, samplers, 0, 2);
	info.cull = SDL_GPU_CULLMODE_NONE;
	info.no_depth = true;
	info.blend = blend;
	info.color_format = format;
	if (info.vertex_shader != NULL && info.fragment_shader != NULL)
	{
		pipeline = gpu_make_pipeline(&info, name);
	}
	if (info.vertex_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.vertex_shader);
	}
	if (info.fragment_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.fragment_shader);
	}
	return pipeline;
}

static void create(ambient_occlusion_t *occlusion)
{
	occlusion->tried = true;
	occlusion->occlusion_pipeline = make_pass("ambient_occlusion.frag", 1, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, GPU_BLEND_NONE,
		"ambient occlusion");
	occlusion->temporal_pipeline = make_pass("ambient_occlusion_temporal.frag", 3, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, GPU_BLEND_NONE,
		"ambient occlusion (over the frames)");
	occlusion->apply_pipeline = make_pass("ambient_occlusion_apply.frag", 4, 0, GPU_BLEND_MULTIPLY, "ambient occlusion (applied)");
	if (occlusion->occlusion_pipeline == NULL || occlusion->temporal_pipeline == NULL || occlusion->apply_pipeline == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Ambient occlusion: its pipelines were refused (%s); none drawn", SDL_GetError());
	}
}

void ambient_occlusion_destroy(ambient_occlusion_t *occlusion)
{
	if (occlusion->occlusion_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, occlusion->occlusion_pipeline);
	}
	if (occlusion->apply_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, occlusion->apply_pipeline);
	}
	if (occlusion->temporal_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, occlusion->temporal_pipeline);
	}
	if (occlusion->occlusion_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, occlusion->occlusion_texture);
	}
	for (int i = 0; i < 2; i++)
	{
		if (occlusion->history_textures[i] != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, occlusion->history_textures[i]);
		}
	}
	SDL_zerop(occlusion);
}

// The half size targets, made again when the scene's size changed (the
// history then gone)
static bool ensure_target(ambient_occlusion_t *occlusion, uint32_t width, uint32_t height)
{
	uint32_t half_width = SDL_max((width + 1) / 2, 1u);
	uint32_t half_height = SDL_max((height + 1) / 2, 1u);
	SDL_GPUTexture **textures[3] = { &occlusion->occlusion_texture, &occlusion->history_textures[0], &occlusion->history_textures[1] };
	bool made = true;

	if (occlusion->history_textures[1] != NULL && occlusion->width == half_width && occlusion->height == half_height)
	{
		return true;
	}
	for (int i = 0; i < 3; i++)
	{
		if (*textures[i] != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, *textures[i]);
		}
		*textures[i] = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, half_width, half_height, 1, 1,
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		made = made && *textures[i] != NULL;
	}
	occlusion->history_valid = false;
	occlusion->width = made ? half_width : 0;
	occlusion->height = made ? half_height : 0;
	return made;
}

// A half size pass into a target, with the scene's block and one of its own
static bool half_pass(ambient_occlusion_t *occlusion, SDL_GPUTexture *target, SDL_GPUGraphicsPipeline *pipeline,
	const scene_uniforms_t *scene, const void *uniforms, uint32_t uniforms_size, const SDL_GPUTextureSamplerBinding *textures, uint32_t texture_count)
{
	SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)occlusion->width, (float)occlusion->height, 0.0f, 1.0f };
	SDL_GPUColorTargetInfo color;
	SDL_GPURenderPass *pass;

	SDL_zero(color);
	color.texture = target;
	color.load_op = SDL_GPU_LOADOP_DONT_CARE;
	color.store_op = SDL_GPU_STOREOP_STORE;
	pass = SDL_BeginGPURenderPass(gpu.command_buffer, &color, 1, NULL);
	if (pass == NULL)
	{
		return false;
	}
	SDL_SetGPUViewport(pass, &viewport);
	SDL_BindGPUGraphicsPipeline(pass, pipeline);
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, uniforms, uniforms_size);
	SDL_BindGPUFragmentSamplers(pass, 0, textures, texture_count);
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
	return true;
}

void ambient_occlusion_draw(ambient_occlusion_t *occlusion, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, uint32_t width,
	uint32_t height, const scene_uniforms_t *scene, const render_settings_t *settings, SDL_GPUTexture *far_shadow, SDL_GPUTexture *near_shadow)
{
	occlusion_uniforms_t uniforms;
	temporal_uniforms_t temporal;
	bool shadows = far_shadow != NULL && near_shadow != NULL;
	uint64_t now = SDL_GetTicks();
	float eye_moved[3];
	int written;
	SDL_GPUColorTargetInfo color;
	SDL_GPURenderPass *pass;

	if (!occlusion->tried)
	{
		create(occlusion);
	}
	if (occlusion->occlusion_pipeline == NULL || occlusion->temporal_pipeline == NULL || occlusion->apply_pipeline == NULL
		|| !ensure_target(occlusion, width, height))
	{
		return;
	}
	// The history holds if it was drawn a moment ago and the eye did not jump
	for (int i = 0; i < 3; i++)
	{
		eye_moved[i] = scene->eye_position[i] - occlusion->previous_eye[i];
	}
	if (now - occlusion->previous_ticks > 250
		|| eye_moved[0] * eye_moved[0] + eye_moved[1] * eye_moved[1] + eye_moved[2] * eye_moved[2]
			> OCCLUSION_HISTORY_JUMP_METERS * OCCLUSION_HISTORY_JUMP_METERS)
	{
		occlusion->history_valid = false;
	}
	occlusion->frame++;
	uniforms.parameters[0] = SDL_clamp(settings->ambient_occlusion_meters, 0.1f, 4.0f);
	uniforms.parameters[1] = SDL_clamp(settings->ambient_occlusion_distance, 5.0f, 2000.0f);
	uniforms.parameters[2] = SDL_clamp(settings->ambient_occlusion_strength, 0.0f, 2.0f);
	uniforms.parameters[3] = shadows ? 1.0f : 0.0f;
	uniforms.size[0] = (float)width;
	uniforms.size[1] = (float)height;
	uniforms.size[2] = (float)occlusion->width;
	uniforms.size[3] = (float)occlusion->height;
	uniforms.frame[0] = (float)(occlusion->frame & 15u);
	uniforms.frame[1] = occlusion->history_valid ? 1.0f : 0.0f;
	uniforms.frame[2] = 0.0f;
	uniforms.frame[3] = 0.0f;

	// 1. The occlusion at half size
	{
		SDL_GPUTextureSamplerBinding textures[1] = { { depth_texture, gpu.nearest_clamp } };

		if (!half_pass(occlusion, occlusion->occlusion_texture, occlusion->occlusion_pipeline, scene, &uniforms,
			sizeof(uniforms), textures, 1))
		{
			return;
		}
	}
	// 1b. Averaged with its history (the other history texture: last
	// frame's; without one, this frame's alone goes in)
	written = 1 - occlusion->history_written;
	{
		SDL_GPUTextureSamplerBinding textures[3] =
		{
			{ depth_texture, gpu.nearest_clamp },
			{ occlusion->occlusion_texture, gpu.nearest_clamp },
			{ occlusion->history_textures[occlusion->history_written], gpu.nearest_clamp },
		};

		SDL_memcpy(temporal.previous_view_projection, occlusion->previous_view_projection, sizeof(temporal.previous_view_projection));
		temporal.eye_moved[0] = eye_moved[0];
		temporal.eye_moved[1] = eye_moved[1];
		temporal.eye_moved[2] = eye_moved[2];
		temporal.eye_moved[3] = 0.0f;
		SDL_memcpy(temporal.size, uniforms.size, sizeof(temporal.size));
		temporal.history[0] = OCCLUSION_HISTORY_WEIGHT;
		temporal.history[1] = occlusion->history_valid ? 1.0f : 0.0f;
		temporal.history[2] = 0.0f;
		temporal.history[3] = 0.0f;
		if (!half_pass(occlusion, occlusion->history_textures[written], occlusion->temporal_pipeline,
			scene, &temporal, sizeof(temporal), textures, 3))
		{
			return;
		}
	}
	occlusion->history_written = written;
	occlusion->history_valid = true;
	SDL_memcpy(occlusion->previous_view_projection, scene->view_projection_relative, sizeof(occlusion->previous_view_projection));
	SDL_memcpy(occlusion->previous_eye, scene->eye_position, sizeof(occlusion->previous_eye));
	occlusion->previous_ticks = now;
	// 2. Over the scene's color (without shadow maps the depth stands in
	// for them, never read: the sun reaches everywhere then)
	{
		SDL_GPUTextureSamplerBinding textures[4] =
		{
			{ shadows ? far_shadow : depth_texture, gpu.shadow_compare },
			{ shadows ? near_shadow : depth_texture, gpu.shadow_compare },
			{ depth_texture, gpu.nearest_clamp },
			{ occlusion->history_textures[written], gpu.nearest_clamp },
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
		SDL_BindGPUGraphicsPipeline(pass, occlusion->apply_pipeline);
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
		SDL_BindGPUFragmentSamplers(pass, 0, textures, 4);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}
}
