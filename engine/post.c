// The frame's last passes. See post.h.

#include "post.h"

#include <SDL3/SDL.h>

#define LEVEL_FORMAT SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT

// The exposure's limits, against a black or a blinding picture (in stops
// around 1, before exposure_ev)
#define MINIMUM_EXPOSURE 0.0625f
#define MAXIMUM_EXPOSURE 16.0f

// The color grading's table: this many steps a side
#define GRADE_SIZE 32

static float clamp_unit(float value)
{
	return SDL_clamp(value, 0.0f, 1.0f);
}

// One color (the screen's, 0..1) through a preset's look
static void grade_color(int preset, const float in[3], float out[3])
{
	float luminance = 0.2126f * in[0] + 0.7152f * in[1] + 0.0722f * in[2];

	if (preset == 1)
	{
		// Neutral: the punchy tone mapping's color taken back a little, the
		// contrast a touch softer
		for (int i = 0; i < 3; i++)
		{
			float color = luminance + 0.8f * (in[i] - luminance);

			out[i] = clamp_unit(0.5f + 0.94f * (color - 0.5f));
		}
	}
	else if (preset == 2)
	{
		// Gloomy: most of the color gone, the blacks lifted into a damp
		// gray-blue, the whites dulled, all of it a bit darker and cold
		const float shadow_tint[3] = { 0.30f, 0.36f, 0.40f };
		const float highlight_tint[3] = { 0.95f, 0.97f, 1.0f };
		const float cold[3] = { 0.93f, 0.98f, 1.04f };
		float dim = SDL_powf(clamp_unit(luminance), 1.15f);

		for (int i = 0; i < 3; i++)
		{
			float color = luminance + 0.4f * (in[i] - luminance);

			color = SDL_powf(clamp_unit(color), 1.15f);
			color = 0.5f + 0.85f * (color - 0.5f);
			color = color * (dim * highlight_tint[i] + (1.0f - dim)) + 0.07f * (1.0f - dim) * shadow_tint[i] / 0.36f;
			out[i] = clamp_unit(0.04f + 0.9f * color * cold[i]);
		}
	}
	else
	{
		out[0] = in[0];
		out[1] = in[1];
		out[2] = in[2];
	}
}

// The table for a preset, into the texture (made if there is none yet)
static void build_grade(post_t *post, int preset)
{
	size_t size = (size_t)GRADE_SIZE * GRADE_SIZE * GRADE_SIZE * 4;
	uint8_t *table;

	if (post->grade_texture == NULL)
	{
		post->grade_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, GRADE_SIZE, GRADE_SIZE,
			GRADE_SIZE, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (post->grade_texture == NULL)
		{
			return;
		}
	}
	table = SDL_malloc(size);
	if (table == NULL)
	{
		return;
	}
	for (int blue = 0; blue < GRADE_SIZE; blue++)
	{
		for (int green = 0; green < GRADE_SIZE; green++)
		{
			for (int red = 0; red < GRADE_SIZE; red++)
			{
				float in[3] = { (float)red / (GRADE_SIZE - 1), (float)green / (GRADE_SIZE - 1), (float)blue / (GRADE_SIZE - 1) };
				float out[3];
				uint8_t *texel = table + (((size_t)blue * GRADE_SIZE + green) * GRADE_SIZE + red) * 4;

				grade_color(preset, in, out);
				for (int i = 0; i < 3; i++)
				{
					texel[i] = (uint8_t)(out[i] * 255.0f + 0.5f);
				}
				texel[3] = 255;
			}
		}
	}
	gpu_upload_volume(post->grade_texture, GRADE_SIZE, GRADE_SIZE, GRADE_SIZE, table, size);
	SDL_free(table);
	post->grade_preset = preset;
}

static void release_levels(post_t *post)
{
	for (int i = 0; i < POST_BLOOM_LEVELS; i++)
	{
		if (post->down[i] != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, post->down[i]);
		}
		if (post->up[i] != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, post->up[i]);
		}
		post->down[i] = NULL;
		post->up[i] = NULL;
	}
	post->level_count = 0;
	post->scene_width = 0;
	post->scene_height = 0;
}

static bool make_levels(post_t *post, int scene_width, int scene_height)
{
	uint32_t width = (uint32_t)SDL_max((scene_width + 1) / 2, 1);
	uint32_t height = (uint32_t)SDL_max((scene_height + 1) / 2, 1);

	release_levels(post);
	for (int i = 0; i < POST_BLOOM_LEVELS; i++)
	{
		post->down[i] = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, LEVEL_FORMAT, width, height, 1, 1,
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		post->up[i] = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, LEVEL_FORMAT, width, height, 1, 1,
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (post->down[i] == NULL || post->up[i] == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot make the bloom's levels: %s", SDL_GetError());
			release_levels(post);
			return false;
		}
		post->level_width[i] = width;
		post->level_height[i] = height;
		post->level_count = i + 1;
		if (width <= 8 || height <= 8)
		{
			break;
		}
		width = SDL_max((width + 1) / 2, 1u);
		height = SDL_max((height + 1) / 2, 1u);
	}
	post->scene_width = scene_width;
	post->scene_height = scene_height;
	return true;
}

static SDL_GPUGraphicsPipeline *make_pipeline(const char *fragment_name, uint32_t samplers, SDL_GPUTextureFormat format, const char *name)
{
	SDL_GPUShader *vertex = gpu_load_shader("post.vert", 0, 0, 0);
	SDL_GPUShader *fragment = gpu_load_shader(fragment_name, samplers, 0, 1);
	SDL_GPUGraphicsPipeline *pipeline = NULL;
	gpu_pipeline_info_t info;

	if (vertex != NULL && fragment != NULL)
	{
		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.cull = SDL_GPU_CULLMODE_NONE;
		info.fill = SDL_GPU_FILLMODE_FILL;
		info.color_format = format;
		info.no_depth = true;
		pipeline = gpu_make_pipeline(&info, name);
	}
	SDL_ReleaseGPUShader(gpu.device, vertex);
	SDL_ReleaseGPUShader(gpu.device, fragment);
	return pipeline;
}

bool post_create(post_t *post)
{
	gpu_compute_layout_t layout = { 2, 0, 0, 1, 64, 0, 1 };
	float one = 1.0f;

	SDL_zerop(post);
	post->down_pipeline = make_pipeline("bloom_down.frag", 1, LEVEL_FORMAT, "bloom down");
	post->up_pipeline = make_pipeline("bloom_up.frag", 2, LEVEL_FORMAT, "bloom up");
	post->grade_preset = -1;
	post->final_pipeline = make_pipeline("post.frag", 4, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "post");
	post->exposure_pipeline = gpu_load_compute("exposure.comp", &layout);
	post->temporal_pipeline = make_pipeline("taa.frag", 3, GPU_SCENE_COLOR_FORMAT, "temporal anti-aliasing");
	for (int i = 0; i < 2; i++)
	{
		post->exposure[i] = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 1, 1, 1, 1,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
		if (post->exposure[i] != NULL)
		{
			gpu_upload_texture(post->exposure[i], 0, 0, 0, 0, 1, 1, &one, sizeof(one));
		}
	}
	build_grade(post, 0);
	post->failed = post->final_pipeline == NULL || post->grade_texture == NULL;
	return !post->failed;
}

void post_destroy(post_t *post)
{
	if (gpu.device == NULL)
	{
		SDL_zerop(post);
		return;
	}
	release_levels(post);
	for (int i = 0; i < 2; i++)
	{
		if (post->exposure[i] != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, post->exposure[i]);
		}
		if (post->history[i] != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, post->history[i]);
		}
	}
	if (post->grade_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, post->grade_texture);
	}
	if (post->temporal_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, post->temporal_pipeline);
	}
	if (post->down_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, post->down_pipeline);
	}
	if (post->up_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, post->up_pipeline);
	}
	if (post->final_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, post->final_pipeline);
	}
	if (post->exposure_pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(gpu.device, post->exposure_pipeline);
	}
	SDL_zerop(post);
}

// One full screen triangle into target, from the samplers given
static bool draw_into(SDL_GPUTexture *target, uint32_t width, uint32_t height, SDL_GPUGraphicsPipeline *pipeline,
	const SDL_GPUTextureSamplerBinding *samplers, uint32_t sampler_count, const void *uniforms, uint32_t uniform_size)
{
	SDL_GPUColorTargetInfo color;
	SDL_GPURenderPass *pass;
	SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };

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
	SDL_BindGPUFragmentSamplers(pass, 0, samplers, sampler_count);
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, uniforms, uniform_size);
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
	return true;
}

void post_temporal_reset(post_t *post)
{
	post->history_valid = false;
}

SDL_GPUTexture *post_temporal(post_t *post, SDL_GPUTexture *scene_texture, SDL_GPUTexture *depth_texture, int scene_width, int scene_height,
	const float reprojection[16], float history_weight, float still_depth, float sharpness)
{
	// (std140: taa.frag's temporal_block)
	struct
	{
		float reprojection[16];
		float texel[4];
		float parameters[4];
	} uniforms;
	SDL_GPUTextureSamplerBinding samplers[3];
	int written;

	if (post->temporal_pipeline == NULL || gpu.command_buffer == NULL || scene_texture == NULL || depth_texture == NULL)
	{
		return scene_texture;
	}
	if (post->history_width != scene_width || post->history_height != scene_height || post->history[0] == NULL || post->history[1] == NULL)
	{
		for (int i = 0; i < 2; i++)
		{
			if (post->history[i] != NULL)
			{
				SDL_ReleaseGPUTexture(gpu.device, post->history[i]);
			}
			post->history[i] = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, GPU_SCENE_COLOR_FORMAT, (uint32_t)scene_width, (uint32_t)scene_height, 1,
				1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		}
		post->history_width = scene_width;
		post->history_height = scene_height;
		post->history_valid = false;
		if (post->history[0] == NULL || post->history[1] == NULL)
		{
			post->history_width = 0;
			return scene_texture;
		}
	}
	written = 1 - post->history_index;
	SDL_memcpy(uniforms.reprojection, reprojection, sizeof(uniforms.reprojection));
	uniforms.texel[0] = 1.0f / (float)scene_width;
	uniforms.texel[1] = 1.0f / (float)scene_height;
	uniforms.texel[2] = SDL_clamp(history_weight, 0.0f, 0.98f);
	uniforms.texel[3] = post->history_valid ? 1.0f : 0.0f;
	uniforms.parameters[0] = still_depth;
	uniforms.parameters[1] = SDL_clamp(sharpness, 0.0f, 1.0f);
	uniforms.parameters[2] = 0.0f;
	uniforms.parameters[3] = 0.0f;
	samplers[0].texture = scene_texture;
	samplers[0].sampler = gpu.nearest_clamp;
	samplers[1].texture = post->history[post->history_index];
	samplers[1].sampler = gpu.linear_clamp;
	samplers[2].texture = depth_texture;
	samplers[2].sampler = gpu.nearest_clamp;
	if (!draw_into(post->history[written], (uint32_t)scene_width, (uint32_t)scene_height, post->temporal_pipeline, samplers, 3, &uniforms,
		sizeof(uniforms)))
	{
		return scene_texture;
	}
	post->history_index = written;
	post->history_valid = true;
	return post->history[written];
}

// The bloom's levels down, and (if wanted) back up. False if there are none
static bool bloom(post_t *post, SDL_GPUTexture *scene_texture, int scene_width, int scene_height, bool up)
{
	SDL_GPUTexture *source = scene_texture;
	uint32_t source_width = (uint32_t)scene_width;
	uint32_t source_height = (uint32_t)scene_height;

	if (post->down_pipeline == NULL || post->up_pipeline == NULL)
	{
		return false;
	}
	if ((post->scene_width != scene_width || post->scene_height != scene_height || post->level_count == 0)
		&& !make_levels(post, scene_width, scene_height))
	{
		return false;
	}
	for (int i = 0; i < post->level_count; i++)
	{
		SDL_GPUTextureSamplerBinding sampler = { source, gpu.linear_clamp };
		float uniforms[4] = { 1.0f / (float)source_width, 1.0f / (float)source_height, i == 0 ? 1.0f : 0.0f, 0.0f };

		if (!draw_into(post->down[i], post->level_width[i], post->level_height[i], post->down_pipeline, &sampler, 1, uniforms,
				sizeof(uniforms)))
		{
			return false;
		}
		source = post->down[i];
		source_width = post->level_width[i];
		source_height = post->level_height[i];
	}
	if (!up)
	{
		return true;
	}
	// The smallest level summed up is itself
	for (int i = post->level_count - 2; i >= 0; i--)
	{
		SDL_GPUTexture *below = i == post->level_count - 2 ? post->down[i + 1] : post->up[i + 1];
		SDL_GPUTextureSamplerBinding samplers[2] = { { post->down[i], gpu.linear_clamp }, { below, gpu.linear_clamp } };
		float uniforms[4] = { 1.0f / (float)post->level_width[i + 1], 1.0f / (float)post->level_height[i + 1], 1.0f, 0.0f };

		if (!draw_into(post->up[i], post->level_width[i], post->level_height[i], post->up_pipeline, samplers, 2, uniforms,
				sizeof(uniforms)))
		{
			return false;
		}
	}
	return true;
}

// This frame's exposure, from last frame's; false if it could not be made
static bool expose(post_t *post, const render_settings_t *settings, float adaptation, bool from_picture)
{
	SDL_GPUStorageTextureReadWriteBinding destination;
	SDL_GPUComputePass *pass;
	int previous = post->exposure_index;
	int next = 1 - previous;
	int smallest = post->level_count - 1;
	SDL_GPUTextureSamplerBinding samplers[2] =
	{
		{ from_picture && smallest >= 0 ? post->down[smallest] : post->exposure[previous], gpu.nearest_clamp },
		{ post->exposure[previous], gpu.nearest_clamp },
	};
	struct
	{
		int32_t level_size[2];
		float automatic;
		float adaptation;
		float compensation;
		float minimum_exposure;
		float maximum_exposure;
		float padding;
	} uniforms;

	if (post->exposure_pipeline == NULL || post->exposure[0] == NULL || post->exposure[1] == NULL)
	{
		return false;
	}
	SDL_zero(uniforms);
	uniforms.level_size[0] = from_picture && smallest >= 0 ? (int32_t)post->level_width[smallest] : 1;
	uniforms.level_size[1] = from_picture && smallest >= 0 ? (int32_t)post->level_height[smallest] : 1;
	uniforms.automatic = from_picture && settings->auto_exposure ? 1.0f : 0.0f;
	uniforms.adaptation = adaptation;
	uniforms.compensation = SDL_powf(2.0f, settings->exposure_ev);
	uniforms.minimum_exposure = MINIMUM_EXPOSURE;
	uniforms.maximum_exposure = MAXIMUM_EXPOSURE;
	SDL_zero(destination);
	destination.texture = post->exposure[next];
	pass = SDL_BeginGPUComputePass(gpu.command_buffer, &destination, 1, NULL, 0);
	if (pass == NULL)
	{
		return false;
	}
	SDL_BindGPUComputePipeline(pass, post->exposure_pipeline);
	SDL_BindGPUComputeSamplers(pass, 0, samplers, 2);
	SDL_PushGPUComputeUniformData(gpu.command_buffer, 0, &uniforms, sizeof(uniforms));
	SDL_DispatchGPUCompute(pass, 1, 1, 1);
	SDL_EndGPUComputePass(pass);
	post->exposure_index = next;
	return true;
}

void post_draw(post_t *post, SDL_GPUTexture *scene_texture, int scene_width, int scene_height, const render_settings_t *settings,
	double now_seconds, bool instant)
{
	bool hdr = settings->hdr && !post->failed;
	bool bloomed = false;
	float elapsed = post->last_seconds > 0.0 ? (float)SDL_clamp(now_seconds - post->last_seconds, 0.0, 1.0) : 0.0f;
	float adaptation;
	SDL_GPUTextureSamplerBinding samplers[4];
	int preset = SDL_clamp(settings->color_grade, 0, 2);
	struct
	{
		int32_t hdr;
		int32_t tone_mapper;
		float bloom_strength;
		float bloom_levels;
		float night_vision;
		float night_vision_luminance;
		float grade_strength;
		float padding;
	} uniforms;

	post->last_seconds = now_seconds;
	if (gpu.command_buffer == NULL || gpu.final_texture == NULL || scene_texture == NULL || post->final_pipeline == NULL)
	{
		return;
	}
	// The eye's adaptation: most of the way in exposure_adapt_seconds
	adaptation = instant || settings->exposure_adapt_seconds <= 0.0f ? 1.0f
		: 1.0f - SDL_expf(-elapsed * 3.0f / settings->exposure_adapt_seconds);
	if (hdr)
	{
		bool picture = settings->auto_exposure || (settings->bloom && settings->bloom_strength > 0.0f);

		// The levels down feed the exposure; up only for the bloom
		bloomed = picture && bloom(post, scene_texture, scene_width, scene_height, settings->bloom && settings->bloom_strength > 0.0f);
		expose(post, settings, adaptation, bloomed && settings->auto_exposure);
		bloomed = bloomed && settings->bloom && settings->bloom_strength > 0.0f && post->level_count > 1;
	}
	SDL_zero(uniforms);
	uniforms.hdr = hdr ? 1 : 0;
	uniforms.tone_mapper = settings->tone_mapper;
	uniforms.bloom_strength = bloomed ? SDL_clamp(settings->bloom_strength, 0.0f, 1.0f) : 0.0f;
	uniforms.bloom_levels = (float)post->level_count;
	uniforms.night_vision = SDL_clamp(settings->night_vision, 0.0f, 1.0f);
	uniforms.night_vision_luminance = SDL_max(settings->night_vision_luminance, 1e-4f);
	// The grading's table remade only when the preset changes (none: not looked up)
	if (preset != 0 && preset != post->grade_preset)
	{
		build_grade(post, preset);
	}
	uniforms.grade_strength = preset != 0 && preset == post->grade_preset ? clamp_unit(settings->color_grade_strength) : 0.0f;
	samplers[0].texture = scene_texture;
	samplers[0].sampler = gpu.linear_clamp;
	samplers[1].texture = bloomed ? post->up[0] : scene_texture;
	samplers[1].sampler = gpu.linear_clamp;
	samplers[2].texture = post->exposure[post->exposure_index] != NULL ? post->exposure[post->exposure_index] : scene_texture;
	samplers[2].sampler = gpu.nearest_clamp;
	samplers[3].texture = post->grade_texture;
	samplers[3].sampler = gpu.linear_clamp;
	draw_into(gpu.final_texture, gpu.final_width, gpu.final_height, post->final_pipeline, samplers, 4, &uniforms, sizeof(uniforms));
}
