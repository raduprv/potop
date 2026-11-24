// The valley fog. See fog.h.

#include <SDL3/SDL.h>

#include "fog.h"
#include "scene_uniforms.h"
#include "math3d.h"

#define VALLEY_SIZE 256                // the valley map's texels a side
#define VALLEY_SECONDS 4.0             // made again this often
#define VALLEY_TOP_SMOOTHING 3         // texels: the top lies level
#define WATER_FOG 0.6f                 // over water: this much of the most the fog rises
#define NEAREST_METERS 0.5f            // the froxels' first slice starts here
#define SIMPLE_STEPS 16

// The froxels for each quality: across, down, slices
static const int grids[3][3] = { { 96, 54, 64 }, { 160, 90, 96 }, { 240, 135, 128 } };
// How much of each frame goes into the froxels (the rest: the frames before)
#define HISTORY_NEW_SHARE 0.08f

typedef struct
{
	float look[4];
	float slices[4];
	float map[4];
	float grid[4];
	float bounds[4];
	float previous_view_projection[16];
	float previous_eye[4];
	float jitter[4];
	float sun[4];
} fog_uniforms_t;

// The Halton sequence (base 2, 3...): offsets that cover a cell evenly
static float halton(uint32_t index, uint32_t base)
{
	float result = 0.0f;
	float fraction = 1.0f / (float)base;

	while (index > 0)
	{
		result += fraction * (float)(index % base);
		index /= base;
		fraction /= (float)base;
	}
	return result;
}

static void release_texture(SDL_GPUTexture **texture)
{
	if (*texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, *texture);
		*texture = NULL;
	}
}

static SDL_GPUGraphicsPipeline *make_apply(const char *fragment_name, const char *name)
{
	gpu_pipeline_info_t info;
	SDL_GPUGraphicsPipeline *pipeline = NULL;

	SDL_zero(info);
	// (the clouds' full screen triangle)
	info.vertex_shader = gpu_load_shader("clouds.vert", 0, 0, 1);
	info.fragment_shader = gpu_load_shader(fragment_name, 5, 0, 2);
	info.cull = SDL_GPU_CULLMODE_NONE;
	info.no_depth = true;
	info.blend = GPU_BLEND_PREMULTIPLIED;
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

static bool create(fog_t *fog)
{
	gpu_compute_layout_t inject;
	gpu_compute_layout_t integrate;

	SDL_zero(inject);
	inject.samplers = 7;
	inject.uniform_buffers = 2;
	inject.threads_x = 64;
	inject.readwrite_storage_textures = 1;
	gpu_compute_layout_t rays;

	integrate = inject;
	integrate.samplers = 1;
	rays = integrate;
	fog->rays_mask_pipeline = gpu_load_compute("fog_rays_mask.comp", &rays);
	fog->rays_pipeline = gpu_load_compute("fog_rays.comp", &rays);
	fog->rays_wide_pipeline = gpu_load_compute("fog_rays_wide.comp", &rays);
	fog->inject_pipeline = gpu_load_compute("fog_inject.comp", &inject);
	fog->integrate_pipeline = gpu_load_compute("fog_integrate.comp", &integrate);
	fog->apply_pipeline = make_apply("fog_apply.frag", "valley fog");
	fog->simple_pipeline = make_apply("fog_simple.frag", "valley fog (simple)");
	fog->valley_size = VALLEY_SIZE;
	fog->valley = SDL_calloc((size_t)VALLEY_SIZE * VALLEY_SIZE * 2, sizeof(float));
	fog->valley_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, VALLEY_SIZE, VALLEY_SIZE, 1, 1,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
	fog->created = true;
	return fog->rays_mask_pipeline != NULL && fog->rays_pipeline != NULL && fog->rays_wide_pipeline != NULL && fog->inject_pipeline != NULL && fog->integrate_pipeline != NULL && fog->apply_pipeline != NULL && fog->simple_pipeline != NULL
		&& fog->valley != NULL && fog->valley_texture != NULL;
}

void fog_destroy(fog_t *fog)
{
	if (gpu.device != NULL)
	{
		if (fog->rays_mask_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, fog->rays_mask_pipeline);
		}
		if (fog->rays_wide_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, fog->rays_wide_pipeline);
		}
		if (fog->rays_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, fog->rays_pipeline);
		}
		if (fog->inject_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, fog->inject_pipeline);
		}
		if (fog->integrate_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, fog->integrate_pipeline);
		}
		if (fog->apply_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, fog->apply_pipeline);
		}
		if (fog->simple_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, fog->simple_pipeline);
		}
		release_texture(&fog->valley_texture);
		release_texture(&fog->scatter_textures[0]);
		release_texture(&fog->scatter_textures[1]);
		release_texture(&fog->integrated_texture);
		release_texture(&fog->rays_mask_texture);
		release_texture(&fog->rays_texture);
	}
	SDL_free(fog->valley);
	for (int k = 0; k < 4; k++)
	{
		SDL_free(fog->valley_work[k]);
	}
	SDL_free(fog->valley_table);
	SDL_zerop(fog);
}

float fog_amount_at_hour(const render_settings_t *settings, float hour)
{
	float form = SDL_fmodf(settings->fog_form_hour + 24.0f, 24.0f);
	float clear = SDL_fmodf(settings->fog_clear_hour + 24.0f, 24.0f);
	// Hours since it began forming, and how long until it is gone
	float since = SDL_fmodf(hour - form + 48.0f, 24.0f);
	float span = SDL_fmodf(clear - form + 48.0f, 24.0f);
	float growing;
	float lifting;

	if (settings->fog_always)
	{
		return 1.0f;
	}
	if (since >= span || span <= 0.0f)
	{
		return 0.0f;
	}
	// Growing over its first four hours, lifting over its last two
	growing = SDL_min(since / 4.0f, 1.0f);
	lifting = SDL_min((span - since) / 2.0f, 1.0f);
	return SDL_min(growing, lifting);
}

// A box blur of a square grid (size a side) by a summed area table,
// radius in texels, the edges as they are
static void box_blur(const float *in, float *out, int size, int radius, double *table)
{
	int stride = size + 1;

	for (int x = 0; x <= size; x++)
	{
		table[x] = 0.0;
	}
	for (int y = 0; y < size; y++)
	{
		double row = 0.0;

		table[(size_t)(y + 1) * (size_t)stride] = 0.0;
		for (int x = 0; x < size; x++)
		{
			row += (double)in[(size_t)y * (size_t)size + (size_t)x];
			table[(size_t)(y + 1) * (size_t)stride + (size_t)(x + 1)] = table[(size_t)y * (size_t)stride + (size_t)(x + 1)] + row;
		}
	}
	for (int y = 0; y < size; y++)
	{
		int y0 = SDL_max(y - radius, 0);
		int y1 = SDL_min(y + radius + 1, size);

		for (int x = 0; x < size; x++)
		{
			int x0 = SDL_max(x - radius, 0);
			int x1 = SDL_min(x + radius + 1, size);
			double sum = table[(size_t)y1 * (size_t)stride + (size_t)x1] - table[(size_t)y0 * (size_t)stride + (size_t)x1]
				- table[(size_t)y1 * (size_t)stride + (size_t)x0] + table[(size_t)y0 * (size_t)stride + (size_t)x0];

			out[(size_t)y * (size_t)size + (size_t)x] = (float)(sum / (double)((x1 - x0) * (y1 - y0)));
		}
	}
}

// The valley map: for each texel the ground, how far below its
// surroundings it is (the fog's thickness there), the water; the top and
// the amount smoothed. In steps, a slice of rows of each a call
// (valley_step, valley_row): returns true once the map is made and sent up
enum
{
	VALLEY_STEP_GROUND = 0,                // the ground sampled, rows at a time
	VALLEY_STEP_AROUND,                    // ...smoothed over the reach
	VALLEY_STEP_THICKNESS,                 // the fog's thickness and the water, rows at a time
	VALLEY_STEP_SMOOTH,                    // the top and the amount smoothed, sent up
};
#define VALLEY_ROWS_A_STEP 32

static bool make_valleys_step(fog_t *fog, const terrain_t *terrain, const water_t *water, const render_settings_t *settings)
{
	int size = fog->valley_size;
	size_t texels = (size_t)size * (size_t)size;
	float extent = (float)terrain->size * terrain->cell_size_meters;
	float texel_meters = extent / (float)size;
	float most = SDL_max(settings->fog_valley_height_meters, 0.0f);
	int reach = SDL_clamp((int)(settings->fog_valley_reach_meters / SDL_max(texel_meters, 0.01f) + 0.5f), 1, size);
	float *ground = fog->valley_work[0];
	float *around = fog->valley_work[1];
	float *top = fog->valley_work[2];
	float *amount = fog->valley_work[3];
	double *table = fog->valley_table;
	int last_row = SDL_min(fog->valley_row + VALLEY_ROWS_A_STEP, size);

	if (ground == NULL)
	{
		for (int k = 0; k < 4; k++)
		{
			fog->valley_work[k] = SDL_malloc(texels * sizeof(float));
		}
		fog->valley_table = SDL_malloc((size_t)(size + 1) * (size_t)(size + 1) * sizeof(double));
		if (fog->valley_work[0] == NULL || fog->valley_work[1] == NULL || fog->valley_work[2] == NULL || fog->valley_work[3] == NULL
			|| fog->valley_table == NULL)
		{
			for (int k = 0; k < 4; k++)
			{
				SDL_free(fog->valley_work[k]);
				fog->valley_work[k] = NULL;
			}
			SDL_free(fog->valley_table);
			fog->valley_table = NULL;
			// (none made: not tried again each frame)
			return true;
		}
		ground = fog->valley_work[0];
		around = fog->valley_work[1];
		top = fog->valley_work[2];
		amount = fog->valley_work[3];
		table = fog->valley_table;
		fog->valley_step = VALLEY_STEP_GROUND;
		fog->valley_row = 0;
		last_row = SDL_min(VALLEY_ROWS_A_STEP, size);
	}
	switch (fog->valley_step)
	{
		case VALLEY_STEP_GROUND:
			for (int y = fog->valley_row; y < last_row; y++)
			{
				for (int x = 0; x < size; x++)
				{
					ground[(size_t)y * (size_t)size + (size_t)x] = terrain_sample_height_meters(terrain, ((float)x + 0.5f) * texel_meters,
						((float)y + 0.5f) * texel_meters);
				}
			}
			fog->valley_row = last_row;
			if (last_row == size)
			{
				fog->valley_step = VALLEY_STEP_AROUND;
			}
			return false;
		case VALLEY_STEP_AROUND:
			box_blur(ground, around, size, reach, table);
			fog->valley_step = VALLEY_STEP_THICKNESS;
			fog->valley_row = 0;
			return false;
		case VALLEY_STEP_THICKNESS:
			for (size_t t = (size_t)fog->valley_row * (size_t)size; t < (size_t)last_row * (size_t)size; t++)
			{
				float below = around[t] - ground[t];
				float thickness = SDL_clamp(below * 1.5f, 0.0f, most);
				int x = (int)(t % (size_t)size);
				int y = (int)(t / (size_t)size);
				float depth = 0.0f;
				float velocity_x;
				float velocity_z;

				if (water != NULL && water_sample(water, ((float)x + 0.5f) * texel_meters, ((float)y + 0.5f) * texel_meters, &depth, &velocity_x,
					&velocity_z) && depth > 0.05f)
				{
					thickness = SDL_max(thickness, most * WATER_FOG);
				}
				top[t] = ground[t] + thickness;
				amount[t] = most > 0.0f ? SDL_clamp(thickness / (0.25f * most), 0.0f, 1.0f) : 0.0f;
			}
			fog->valley_row = last_row;
			if (last_row == size)
			{
				fog->valley_step = VALLEY_STEP_SMOOTH;
			}
			return false;
		default:
			break;
	}
	// (the ground's and what lies around are done with: the smoothed top
	// and amount into them)
	box_blur(top, around, size, VALLEY_TOP_SMOOTHING, table);
	box_blur(amount, ground, size, VALLEY_TOP_SMOOTHING, table);
	fog->highest = -1e9f;
	for (size_t t = 0; t < texels; t++)
	{
		fog->valley[t * 2] = around[t];
		fog->valley[t * 2 + 1] = ground[t];
		if (ground[t] > 0.001f)
		{
			fog->highest = SDL_max(fog->highest, around[t]);
		}
	}
	gpu_upload_texture(fog->valley_texture, 0, 0, 0, 0, (uint32_t)size, (uint32_t)size, fog->valley, texels * 2 * sizeof(float));
	fog->valley_ready = true;
	fog->valley_step = VALLEY_STEP_GROUND;
	fog->valley_row = 0;
	return true;
}

void fog_update(fog_t *fog, const terrain_t *terrain, const water_t *water, const render_settings_t *settings, double seconds)
{
	if (settings->fog_mode == FOG_MODE_OFF || terrain == NULL || terrain->size <= 0 || fog->failed)
	{
		return;
	}
	if (!fog->created)
	{
		fog->failed = !create(fog);
		if (fog->failed)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Valley fog: cannot make it (%s)", SDL_GetError());
			return;
		}
	}
	// (the first one whole, at once: there is no map to show meanwhile)
	if (!fog->valley_ready)
	{
		while (!make_valleys_step(fog, terrain, water, settings))
		{
		}
		fog->next_valley_seconds = seconds + VALLEY_SECONDS;
		return;
	}
	if (seconds < fog->next_valley_seconds)
	{
		return;
	}
	// Due: a slice of it this frame, the next one counted from its end
	if (make_valleys_step(fog, terrain, water, settings))
	{
		fog->next_valley_seconds = seconds + VALLEY_SECONDS;
	}
}

// The rays' textures at half the screen's size (made again when it changes)
static bool make_rays(fog_t *fog, uint32_t width, uint32_t height)
{
	uint32_t wanted[2] = { SDL_max((width + 1) / 2, 1u), SDL_max((height + 1) / 2, 1u) };
	SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;

	if (fog->rays_texture != NULL && fog->rays_size[0] == wanted[0] && fog->rays_size[1] == wanted[1])
	{
		return true;
	}
	release_texture(&fog->rays_mask_texture);
	release_texture(&fog->rays_texture);
	fog->rays_mask_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, wanted[0], wanted[1], 1, 1, usage);
	fog->rays_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, wanted[0], wanted[1], 1, 1, usage);
	if (fog->rays_mask_texture == NULL || fog->rays_texture == NULL)
	{
		release_texture(&fog->rays_mask_texture);
		release_texture(&fog->rays_texture);
		return false;
	}
	fog->rays_size[0] = wanted[0];
	fog->rays_size[1] = wanted[1];
	return true;
}

// Runs one of the rays' passes: reading source, writing target
static void run_rays_pass(fog_t *fog, SDL_GPUComputePipeline *pipeline, SDL_GPUTexture *source, SDL_GPUSampler *sampler,
	SDL_GPUTexture *target_texture, const scene_uniforms_t *scene, const fog_uniforms_t *uniforms)
{
	SDL_GPUStorageTextureReadWriteBinding target;
	SDL_GPUTextureSamplerBinding binding = { source, sampler };
	SDL_GPUComputePass *compute;
	uint32_t texels = fog->rays_size[0] * fog->rays_size[1];

	SDL_zero(target);
	target.texture = target_texture;
	compute = SDL_BeginGPUComputePass(gpu.command_buffer, &target, 1, NULL, 0);
	if (compute == NULL)
	{
		return;
	}
	SDL_BindGPUComputePipeline(compute, pipeline);
	SDL_BindGPUComputeSamplers(compute, 0, &binding, 1);
	SDL_PushGPUComputeUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
	SDL_PushGPUComputeUniformData(gpu.command_buffer, 1, uniforms, sizeof(*uniforms));
	SDL_DispatchGPUCompute(compute, (texels + 63) / 64, 1, 1);
	SDL_EndGPUComputePass(compute);
}

// The froxels at this quality (made again when it changes)
static bool make_grid(fog_t *fog, int quality)
{
	const int *wanted = grids[SDL_clamp(quality, 0, 2)];
	SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;

	if (fog->scatter_textures[0] != NULL && fog->grid[0] == wanted[0] && fog->grid[1] == wanted[1] && fog->grid[2] == wanted[2])
	{
		return true;
	}
	release_texture(&fog->scatter_textures[0]);
	release_texture(&fog->scatter_textures[1]);
	release_texture(&fog->integrated_texture);
	fog->history_valid = false;
	for (int i = 0; i < 2; i++)
	{
		fog->scatter_textures[i] = gpu_create_texture(SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, (uint32_t)wanted[0],
			(uint32_t)wanted[1], (uint32_t)wanted[2], 1, usage);
	}
	fog->integrated_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, (uint32_t)wanted[0],
		(uint32_t)wanted[1], (uint32_t)wanted[2], 1, usage);
	if (fog->scatter_textures[0] == NULL || fog->scatter_textures[1] == NULL || fog->integrated_texture == NULL)
	{
		release_texture(&fog->scatter_textures[0]);
		release_texture(&fog->scatter_textures[1]);
		release_texture(&fog->integrated_texture);
		fog->grid[0] = 0;
		return false;
	}
	SDL_memcpy(fog->grid, wanted, sizeof(fog->grid));
	SDL_Log("Valley fog: %d x %d x %d froxels", wanted[0], wanted[1], wanted[2]);
	return true;
}

void fog_draw(fog_t *fog, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, uint32_t width, uint32_t height,
	const scene_uniforms_t *scene, const render_settings_t *settings, SDL_GPUTexture *far_shadow, SDL_GPUTexture *near_shadow,
	float hour, const float wind[2], double seconds, float map_extent_meters)
{
	fog_uniforms_t uniforms;
	bool volumetric = settings->fog_mode == FOG_MODE_VOLUMETRIC;
	float amount = fog_amount_at_hour(settings, hour);
	float farthest = SDL_max(settings->fog_distance_meters, 20.0f);
	SDL_GPURenderPass *pass;

	if (settings->fog_mode == FOG_MODE_OFF || fog->failed || !fog->created || !fog->valley_ready || amount <= 0.0f
		|| gpu.command_buffer == NULL || scene->underwater_color[3] > 0.5f)
	{
		// (what it was is stale when it comes back)
		fog->history_valid = false;
		return;
	}
	if (volumetric && !make_grid(fog, settings->fog_quality))
	{
		volumetric = false;
	}
	SDL_zero(uniforms);
	uniforms.look[0] = SDL_max(settings->fog_valley_density, 0.0f);
	uniforms.look[1] = SDL_max(settings->fog_softness_meters, 0.5f);
	uniforms.look[2] = amount;
	uniforms.look[3] = SDL_clamp(settings->fog_glow, 0.0f, 0.9f);
	uniforms.slices[0] = NEAREST_METERS;
	uniforms.slices[1] = farthest;
	uniforms.slices[2] = (float)(volumetric ? fog->grid[2] : 1);
	uniforms.slices[3] = (float)(fog->frame++ & 1023u);
	uniforms.map[0] = map_extent_meters;
	// It drifts slowly, much slower than the wind
	uniforms.map[1] = wind[0] * 0.15f;
	uniforms.map[2] = wind[1] * 0.15f;
	uniforms.map[3] = (float)SDL_fmod(seconds, 3600.0);
	uniforms.grid[0] = (float)(volumetric ? fog->grid[0] : 1);
	uniforms.grid[1] = (float)(volumetric ? fog->grid[1] : 1);
	uniforms.grid[2] = 1.0f / SDL_logf(farthest / NEAREST_METERS);
	uniforms.grid[3] = (float)SIMPLE_STEPS;
	uniforms.bounds[0] = fog->highest + SDL_max(settings->fog_softness_meters, 0.5f);
	uniforms.bounds[1] = SDL_clamp(settings->fog_shafts, 0.0f, 1.0f);
	// (lit evenly from every side, by night it outshines the dark ground
	// round it: dimmer then)
	uniforms.bounds[2] = 1.0f + (SDL_clamp(settings->fog_night_brightness, 0.0f, 1.0f) - 1.0f) * SDL_clamp(settings->night_amount, 0.0f, 1.0f);
	// Where the frame before's froxels were, and this frame's offset in
	// them (the frames cover each froxel between them)
	SDL_memcpy(uniforms.previous_view_projection, fog->previous_view_projection, sizeof(uniforms.previous_view_projection));
	SDL_memcpy(uniforms.previous_eye, fog->previous_eye, sizeof(fog->previous_eye));
	// (a jump, not a walk: none of it is of use)
	{
		float dx = scene->eye_position[0] - fog->previous_eye[0];
		float dy = scene->eye_position[1] - fog->previous_eye[1];
		float dz = scene->eye_position[2] - fog->previous_eye[2];

		if (dx * dx + dy * dy + dz * dz > 20.0f * 20.0f)
		{
			fog->history_valid = false;
		}
	}
	uniforms.previous_eye[3] = volumetric && fog->history_valid ? 1.0f : 0.0f;
	uniforms.jitter[0] = halton((fog->frame & 63u) + 1u, 2u) - 0.5f;
	uniforms.jitter[1] = halton((fog->frame & 63u) + 1u, 3u) - 0.5f;
	uniforms.jitter[2] = halton((fog->frame & 63u) + 1u, 5u);
	uniforms.jitter[3] = HISTORY_NEW_SHARE;
	{
		mat4_t inverse;
		mat4_t forward;

		// (from the eye: its translation left out, as the shaders' clip_to_eye;
		// the froxels then are looked up by where they are from the eye
		// before, fog_previous_eye: kilometers out, the other one put them a
		// little off each frame and the history smeared into a second fog)
		SDL_memcpy(inverse.m, scene->inverse_view_projection_relative, sizeof(inverse.m));
		if (mat4_inverse(inverse, &forward))
		{
			SDL_memcpy(fog->previous_view_projection, forward.m, sizeof(fog->previous_view_projection));
			SDL_memcpy(fog->previous_eye, scene->eye_position, sizeof(fog->previous_eye));
			// Where the sun is on the screen (far off along its way); its
			// rays fade as it leaves the screen, none behind the eye
			if (settings->fog_rays > 0.0f)
			{
				float far_point[4] =
				{
					scene->sun_direction[0] * 10000.0f,
					scene->sun_direction[1] * 10000.0f,
					scene->sun_direction[2] * 10000.0f,
					1.0f
				};
				float clip[4];

				for (int row = 0; row < 4; row++)
				{
					clip[row] = 0.0f;
					for (int column = 0; column < 4; column++)
					{
						clip[row] += forward.m[column * 4 + row] * far_point[column];
					}
				}
				if (clip[3] > 1e-3f)
				{
					float x = clip[0] / clip[3];
					float y = clip[1] / clip[3];
					float outside = SDL_max(SDL_fabsf(x), SDL_fabsf(y));
					float fade = 1.0f - SDL_clamp((outside - 1.0f) / 0.8f, 0.0f, 1.0f);

					uniforms.sun[0] = x * 0.5f + 0.5f;
					uniforms.sun[1] = 0.5f - y * 0.5f;
					uniforms.sun[2] = settings->fog_rays * fade;
				}
			}
		}
	}
	// The rays, first: from the screen as it is
	if (uniforms.sun[2] > 0.0f && fog->rays_mask_pipeline != NULL && make_rays(fog, width, height))
	{
		// (the mask into the rays' texture, its first eighth of the way into
		// the mask's, the whole way back into the rays': ping-pong, the two
		// the same size and format)
		run_rays_pass(fog, fog->rays_mask_pipeline, depth_texture, gpu.nearest_clamp, fog->rays_texture, scene, &uniforms);
		run_rays_pass(fog, fog->rays_pipeline, fog->rays_texture, gpu.linear_clamp, fog->rays_mask_texture, scene, &uniforms);
		run_rays_pass(fog, fog->rays_wide_pipeline, fog->rays_mask_texture, gpu.linear_clamp, fog->rays_texture, scene, &uniforms);
	}
	else
	{
		uniforms.sun[2] = 0.0f;
	}
	if (volumetric)
	{
		int current = (int)(fog->frame & 1u);
		SDL_GPUTexture *history = fog->scatter_textures[current ^ 1];
		uint32_t froxels = (uint32_t)(fog->grid[0] * fog->grid[1] * fog->grid[2]);
		uint32_t columns = (uint32_t)(fog->grid[0] * fog->grid[1]);
		SDL_GPUStorageTextureReadWriteBinding target;
		SDL_GPUComputePass *compute;
		SDL_GPUTextureSamplerBinding inject_samplers[4] =
		{
			{ far_shadow, gpu.shadow_compare },
			{ near_shadow, gpu.shadow_compare },
			{ fog->valley_texture, gpu.linear_clamp },
			{ history, gpu.linear_clamp },
		};
		SDL_GPUTextureSamplerBinding scatter = { fog->scatter_textures[current], gpu.nearest_clamp };

		SDL_zero(target);
		target.texture = fog->scatter_textures[current];
		compute = SDL_BeginGPUComputePass(gpu.command_buffer, &target, 1, NULL, 0);
		if (compute == NULL)
		{
			return;
		}
		SDL_BindGPUComputePipeline(compute, fog->inject_pipeline);
		SDL_BindGPUComputeSamplers(compute, 0, inject_samplers, 4);
		scene_ground_bind_sky_compute(compute, 4);
		SDL_PushGPUComputeUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUComputeUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(compute, (froxels + 63) / 64, 1, 1);
		SDL_EndGPUComputePass(compute);
		SDL_zero(target);
		target.texture = fog->integrated_texture;
		compute = SDL_BeginGPUComputePass(gpu.command_buffer, &target, 1, NULL, 0);
		if (compute == NULL)
		{
			return;
		}
		SDL_BindGPUComputePipeline(compute, fog->integrate_pipeline);
		SDL_BindGPUComputeSamplers(compute, 0, &scatter, 1);
		SDL_PushGPUComputeUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUComputeUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(compute, (columns + 63) / 64, 1, 1);
		SDL_EndGPUComputePass(compute);
		fog->history_valid = true;
	}
	else
	{
		fog->history_valid = false;
	}
	{
		SDL_GPUColorTargetInfo color;
		SDL_GPUTextureSamplerBinding textures[5] =
		{
			{ volumetric ? fog->integrated_texture : fog->valley_texture, gpu.linear_clamp },
			{ depth_texture, gpu.nearest_clamp },
			// (with no rays, any texture: not read)
			{ fog->rays_texture != NULL ? fog->rays_texture : fog->valley_texture, gpu.linear_clamp },
			// (the fog beyond the froxels; the simple one does not read them)
			{ fog->valley_texture, gpu.linear_clamp },
			{ far_shadow, gpu.shadow_compare },
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
		SDL_BindGPUGraphicsPipeline(pass, volumetric ? fog->apply_pipeline : fog->simple_pipeline);
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
		SDL_BindGPUFragmentSamplers(pass, 0, textures, 5);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}
}
