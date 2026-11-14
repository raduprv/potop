// Volumetric clouds. See clouds.h.

#include "clouds.h"
#include "file_io.h"
#include "math3d.h"

#include <SDL3/SDL.h>

#define SHAPE_SIZE 128
#define DETAIL_SIZE 64
// The shape noise repeats every this many meters, the detail every this many
#define SHAPE_TILE_METERS 1400.0f
#define DETAIL_TILE_METERS 200.0f
// The noise, computed once and kept in the preference directory (change
// the name when the noise changes)
#define NOISE_CACHE_NAME "cache/cloud_noise_4.bin"
// Clouds are marched no farther than this (by then the haze has them; the
// layer's top reaches the horizon about 35 km away, see CLOUD_PLANET_RADIUS)
#define MAXIMUM_DISTANCE_METERS 80000.0f
// The dome's format, and its size (texels a side) when render.cloud_dome_size
// is 0: about this many times the screen's height (2048 at 1440 pixels,
// about 0.07 degrees a texel under the clouds: two pixels)
// How much of the clouds' light from the sky is gone at full night
#define CLOUD_NIGHT_DARKENING 0.8f

#define DOME_FORMAT SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
#define DOME_AUTO_PER_PIXEL 1.4f
#define DOME_MINIMUM_SIZE 512
#define DOME_MAXIMUM_SIZE 4096
// With the eye this far under the clouds (or less) the dome holds the upper
// half of the sky only (DOME_BAND in clouds_volume.glsl: the curved layer
// is seen down to about sqrt(2 * height / radius) under the horizon)
#define DOME_HALF_MAXIMUM_EYE_METERS 600.0f
// The share of each march in what a texel ends up holding while the eye
// moves (coarse: less of the old view lingers). Still, it comes from the
// settings (cloud_refresh_seconds, cloud_memory_seconds)
#define DOME_WEIGHT_MOVING 0.7f
// Sixteenths of the dome marched in one frame at most (a slow frame does
// not try to catch up on all it owes)
#define DOME_MAXIMUM_PASSES 4
// The clouds' shadow map: texels a side over the cloud map, and how often it
// is made again (the clouds change slowly: wind moves the map, not them)
#define SHADOW_MAP_SIZE 512
#define SHADOW_REFRESH_SECONDS 0.5
// The flocks: at a trend of 1 they go from none to all in this long (the
// sky's clock), and their layer is this thick for the size of each (at
// least, at most: meters)
#define FLOCK_TREND_SECONDS 300.0f
#define FLOCK_THICKNESS_SHARE 0.35f
#define FLOCK_MINIMUM_THICKNESS 40.0f
#define FLOCK_MAXIMUM_THICKNESS 400.0f

// The shadow map the land reads (see clouds_shadow_texture): one sky, so one
static SDL_GPUTexture *shadow_for_shading = NULL;
// The eye moves when, seen from the nearest clouds, it shifts by more than
// this (radians a frame; a dome texel is about 0.0016): then the whole dome
// is marched each frame, coarsely. Jumps (meters): all of it, finely
#define DOME_MOVING_RADIANS 0.0004f
#define DOME_JUMP_METERS 500.0f

// ---------------------------------------------------------------------------
// Tiling noise (period in cells: wraps around)

// PCG's output permutation, chained over the coordinates: well mixed (a
// plain xor of products leaves the feature points lined up in rows)
static uint32_t pcg(uint32_t value)
{
	uint32_t state = value * 747796405u + 2891336453u;
	uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;

	return (word >> 22u) ^ word;
}

static uint32_t hash3(int x, int y, int z, uint32_t seed)
{
	return pcg(pcg(pcg(pcg(seed) + (uint32_t)x) + (uint32_t)y) + (uint32_t)z);
}

static float hash_unit(uint32_t h)
{
	return (float)(h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

static int wrap(int value, int period)
{
	return ((value % period) + period) % period;
}

// Worley: 1 near a feature point, 0 far from all of them (inverted, so it
// reads like billows)
static float worley(float x, float y, float z, int period, uint32_t seed)
{
	int cell_x = (int)SDL_floorf(x);
	int cell_y = (int)SDL_floorf(y);
	int cell_z = (int)SDL_floorf(z);
	float nearest = 1e9f;

	for (int k = -1; k <= 1; k++)
	{
		for (int j = -1; j <= 1; j++)
		{
			for (int i = -1; i <= 1; i++)
			{
				int neighbor_x = cell_x + i;
				int neighbor_y = cell_y + j;
				int neighbor_z = cell_z + k;
				uint32_t h = hash3(wrap(neighbor_x, period), wrap(neighbor_y, period), wrap(neighbor_z, period), seed);
				float point_x = (float)neighbor_x + hash_unit(h);
				float point_y = (float)neighbor_y + hash_unit(h * 747796405u + 1u);
				float point_z = (float)neighbor_z + hash_unit(h * 2891336453u + 7u);
				float dx = point_x - x;
				float dy = point_y - y;
				float dz = point_z - z;

				nearest = SDL_min(nearest, dx * dx + dy * dy + dz * dz);
			}
		}
	}
	return 1.0f - SDL_min(SDL_sqrtf(nearest), 1.0f);
}

static float fade(float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// The dot product with a gradient picked by h: a direction spread evenly
// over the sphere (the usual twelve cube edges leave ridges along the axes
// and diagonals)
static float gradient_dot(uint32_t h, float x, float y, float z)
{
	float height = hash_unit(h) * 2.0f - 1.0f;
	float angle = hash_unit(pcg(h)) * 6.28318531f;
	float ring = SDL_sqrtf(SDL_max(1.0f - height * height, 0.0f));

	// Scaled like the cube edges' (length about 1.4), so the octaves keep
	// their range
	return (ring * SDL_cosf(angle) * x + height * y + ring * SDL_sinf(angle) * z) * 1.41421356f;
}

// Perlin, about -1 .. 1
static float perlin(float x, float y, float z, int period, uint32_t seed)
{
	int cell_x = (int)SDL_floorf(x);
	int cell_y = (int)SDL_floorf(y);
	int cell_z = (int)SDL_floorf(z);
	float fx = x - (float)cell_x;
	float fy = y - (float)cell_y;
	float fz = z - (float)cell_z;
	float u = fade(fx);
	float v = fade(fy);
	float w = fade(fz);
	float corners[8];

	for (int c = 0; c < 8; c++)
	{
		int i = c & 1;
		int j = (c >> 1) & 1;
		int k = (c >> 2) & 1;
		uint32_t h = hash3(wrap(cell_x + i, period), wrap(cell_y + j, period), wrap(cell_z + k, period), seed);

		corners[c] = gradient_dot(h, fx - (float)i, fy - (float)j, fz - (float)k);
	}
	{
		float x00 = corners[0] + (corners[1] - corners[0]) * u;
		float x10 = corners[2] + (corners[3] - corners[2]) * u;
		float x01 = corners[4] + (corners[5] - corners[4]) * u;
		float x11 = corners[6] + (corners[7] - corners[6]) * u;
		float y0 = x00 + (x10 - x00) * v;
		float y1 = x01 + (x11 - x01) * v;

		return y0 + (y1 - y0) * w;
	}
}

// Worley octaves at 'frequency' cells per tile and twice and four times that
static float worley_octaves(float u, float v, float w, int frequency, uint32_t seed)
{
	return worley(u * (float)frequency, v * (float)frequency, w * (float)frequency, frequency, seed) * 0.625f
		+ worley(u * (float)(frequency * 2), v * (float)(frequency * 2), w * (float)(frequency * 2), frequency * 2, seed + 1u) * 0.25f
		+ worley(u * (float)(frequency * 4), v * (float)(frequency * 4), w * (float)(frequency * 4), frequency * 4, seed + 2u) * 0.125f;
}

static float remap(float value, float old_low, float old_high, float new_low, float new_high)
{
	return new_low + (value - old_low) / SDL_max(old_high - old_low, 1e-4f) * (new_high - new_low);
}

static uint8_t to_byte(float value)
{
	return (uint8_t)SDL_clamp((int)(value * 255.0f + 0.5f), 0, 255);
}

static void make_shape(uint8_t *texels)
{
#pragma omp parallel for schedule(dynamic, 1)
	for (int z = 0; z < SHAPE_SIZE; z++)
	{
		for (int y = 0; y < SHAPE_SIZE; y++)
		{
			for (int x = 0; x < SHAPE_SIZE; x++)
			{
				float u = ((float)x + 0.5f) / (float)SHAPE_SIZE;
				float v = ((float)y + 0.5f) / (float)SHAPE_SIZE;
				float w = ((float)z + 0.5f) / (float)SHAPE_SIZE;
				float perlin_octaves = 0.0f;
				float amplitude = 1.0f;
				float total = 0.0f;
				uint8_t *texel = &texels[(((size_t)z * SHAPE_SIZE + (size_t)y) * SHAPE_SIZE + (size_t)x) * 4];

				for (int octave = 0; octave < 4; octave++)
				{
					int frequency = 4 << octave;

					perlin_octaves += perlin(u * (float)frequency, v * (float)frequency, w * (float)frequency, frequency, 11u + (uint32_t)octave) * amplitude;
					total += amplitude;
					amplitude *= 0.5f;
				}
				perlin_octaves = perlin_octaves / total * 0.5f + 0.5f;
				{
					// Perlin-Worley: the Perlin's smooth hills dented by the cells
					float cells = worley_octaves(u, v, w, 4, 21u);
					float perlin_worley = remap(perlin_octaves, cells - 1.0f, 1.0f, 0.0f, 1.0f);

					texel[0] = to_byte(perlin_worley);
					texel[1] = to_byte(worley_octaves(u, v, w, 4, 31u));
					texel[2] = to_byte(worley_octaves(u, v, w, 8, 41u));
					texel[3] = to_byte(worley_octaves(u, v, w, 16, 51u));
				}
			}
		}
	}
}

static void make_detail(uint8_t *texels)
{
#pragma omp parallel for schedule(dynamic, 1)
	for (int z = 0; z < DETAIL_SIZE; z++)
	{
		for (int y = 0; y < DETAIL_SIZE; y++)
		{
			for (int x = 0; x < DETAIL_SIZE; x++)
			{
				float u = ((float)x + 0.5f) / (float)DETAIL_SIZE;
				float v = ((float)y + 0.5f) / (float)DETAIL_SIZE;
				float w = ((float)z + 0.5f) / (float)DETAIL_SIZE;
				uint8_t *texel = &texels[(((size_t)z * DETAIL_SIZE + (size_t)y) * DETAIL_SIZE + (size_t)x) * 4];

				texel[0] = to_byte(worley_octaves(u, v, w, 2, 61u));
				texel[1] = to_byte(worley_octaves(u, v, w, 4, 71u));
				texel[2] = to_byte(worley_octaves(u, v, w, 8, 81u));
				texel[3] = 255;
			}
		}
	}
}

// Stretches each channel so that its 2nd and 98th percentiles become 0 and
// 255: the noises come out bunched in the middle, the clouds want contrast
static void normalize_channels(uint8_t *texels, size_t texel_count, int channels)
{
	for (int c = 0; c < channels; c++)
	{
		size_t histogram[256] = { 0 };
		size_t seen = 0;
		int low = 0;
		int high = 255;

		for (size_t i = 0; i < texel_count; i++)
		{
			histogram[texels[i * 4 + (size_t)c]]++;
		}
		for (int v = 0; v < 256; v++)
		{
			seen += histogram[v];
			if (seen <= texel_count / 50)
			{
				low = v;
			}
			if (seen < texel_count - texel_count / 50)
			{
				high = v + 1;
			}
		}
		high = SDL_max(high, low + 1);
		for (size_t i = 0; i < texel_count; i++)
		{
			uint8_t *value = &texels[i * 4 + (size_t)c];

			*value = (uint8_t)SDL_clamp(((int)*value - low) * 255 / (high - low), 0, 255);
		}
	}
}

// ---------------------------------------------------------------------------

static bool create(clouds_t *clouds)
{
	uint64_t start = SDL_GetPerformanceCounter();
	size_t shape_bytes = (size_t)SHAPE_SIZE * SHAPE_SIZE * SHAPE_SIZE * 4;
	size_t detail_bytes = (size_t)DETAIL_SIZE * DETAIL_SIZE * DETAIL_SIZE * 4;
	uint8_t *shape = SDL_malloc(shape_bytes);
	uint8_t *detail = SDL_malloc(detail_bytes);
	gpu_pipeline_info_t info;
	bool ok = shape != NULL && detail != NULL;

	if (ok)
	{
		char path[1024] = "";
		size_t cached_size = 0;
		uint8_t *cached = file_io_preference_path(NOISE_CACHE_NAME, path, sizeof(path)) ? file_io_load_file(path, &cached_size) : NULL;

		if (cached != NULL && cached_size == shape_bytes + detail_bytes)
		{
			SDL_memcpy(shape, cached, shape_bytes);
			SDL_memcpy(detail, cached + shape_bytes, detail_bytes);
		}
		else
		{
			uint8_t *both = SDL_malloc(shape_bytes + detail_bytes);

			make_shape(shape);
			make_detail(detail);
			normalize_channels(shape, (size_t)SHAPE_SIZE * SHAPE_SIZE * SHAPE_SIZE, 4);
			normalize_channels(detail, (size_t)DETAIL_SIZE * DETAIL_SIZE * DETAIL_SIZE, 3);
			if (both != NULL && path[0] != '\0')
			{
				SDL_memcpy(both, shape, shape_bytes);
				SDL_memcpy(both + shape_bytes, detail, detail_bytes);
				file_io_save_file(path, both, shape_bytes + detail_bytes);
			}
			SDL_free(both);
		}
		SDL_free(cached);
		clouds->shape_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SHAPE_SIZE, SHAPE_SIZE, SHAPE_SIZE,
			1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		clouds->detail_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, DETAIL_SIZE, DETAIL_SIZE,
			DETAIL_SIZE, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		ok = clouds->shape_texture != NULL && clouds->detail_texture != NULL;
	}
	if (ok)
	{
		gpu_upload_volume(clouds->shape_texture, SHAPE_SIZE, SHAPE_SIZE, SHAPE_SIZE, shape, shape_bytes);
		gpu_upload_volume(clouds->detail_texture, DETAIL_SIZE, DETAIL_SIZE, DETAIL_SIZE, detail, detail_bytes);
	}
	SDL_free(shape);
	SDL_free(detail);
	if (ok)
	{
		// Blending in the dome needs its format read and written in one
		// shader; where it cannot be, each march simply replaces the last
		SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
		gpu_compute_layout_t layout;

		clouds->dome_blends = SDL_GPUTextureSupportsFormat(gpu.device, DOME_FORMAT, SDL_GPU_TEXTURETYPE_2D, usage);
		if (!clouds->dome_blends)
		{
			usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			SDL_Log("Volumetric clouds: the dome cannot blend (the clouds will be grainier)");
		}
		clouds->dome_usage = usage;
		SDL_zero(layout);
		layout.samplers = 7;
		layout.readonly_storage_buffers = 1;
		layout.readwrite_storage_textures = 1;
		layout.uniform_buffers = 2;
		layout.threads_x = 64;
		clouds->dome_pipeline = gpu_load_compute(clouds->dome_blends ? "clouds_dome_blend.comp" : "clouds_dome.comp", &layout);
		clouds->shadow_pipeline = gpu_load_compute("clouds_shadow.comp", &layout);
		clouds->shadow_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SHADOW_MAP_SIZE,
			SHADOW_MAP_SIZE, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);

		SDL_zero(info);
		info.vertex_shader = gpu_load_shader("clouds.vert", 0, 0, 1);
		info.fragment_shader = gpu_load_shader("clouds_composite.frag", 2, 0, 2);
		info.cull = SDL_GPU_CULLMODE_NONE;
		info.no_depth = true;
		info.blend = GPU_BLEND_PREMULTIPLIED;
		clouds->composite_pipeline = gpu_make_pipeline(&info, "volumetric clouds");
		SDL_ReleaseGPUShader(gpu.device, info.fragment_shader);
		SDL_ReleaseGPUShader(gpu.device, info.vertex_shader);
		ok = clouds->dome_pipeline != NULL && clouds->composite_pipeline != NULL && clouds->shadow_pipeline != NULL
			&& clouds->shadow_texture != NULL;
	}
	if (ok)
	{
		SDL_Log("Volumetric clouds: noise made in %.0f ms",
			(double)(SDL_GetPerformanceCounter() - start) * 1000.0 / (double)SDL_GetPerformanceFrequency());
	}
	return ok;
}

static void release_texture(SDL_GPUTexture **texture)
{
	if (*texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, *texture);
		*texture = NULL;
	}
}

void clouds_destroy(clouds_t *clouds)
{
	if (gpu.device != NULL)
	{
		cloud_weather_destroy(&clouds->weather);
		if (shadow_for_shading == clouds->shadow_texture)
		{
			shadow_for_shading = NULL;
		}
		release_texture(&clouds->shadow_texture);
		if (clouds->shadow_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, clouds->shadow_pipeline);
		}
		release_texture(&clouds->dome_texture);
		release_texture(&clouds->flock_texture);
		release_texture(&clouds->shape_texture);
		release_texture(&clouds->detail_texture);
		if (clouds->dome_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, clouds->dome_pipeline);
		}
		if (clouds->composite_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, clouds->composite_pipeline);
		}
	}
	SDL_zerop(clouds);
}

void clouds_forget(clouds_t *clouds)
{
	clouds->dome_valid = false;
}

SDL_GPUTexture *clouds_shadow_texture(void)
{
	return shadow_for_shading;
}

// How the dome is marched this frame, and how many times (passes_out).
// Still: as many sixteenths of it as the clock says are due (each 4x4 block
// one texel; the shader picks which), the whole of it every
// cloud_refresh_seconds, maybe none this frame; moving: all of it coarsely;
// after a jump or at first, all of it finely. The dome is seen from the eye
// alone, so how far it may move before the clouds shift depends on how
// near they are: tens of meters under them, nothing at all inside them
static void choose_refresh(clouds_t *clouds, const float eye[3], float base_height, float top_height, const render_settings_t *settings,
	float dome_out[4], float refresh_out[4], int32_t *passes_out)
{
	uint64_t now = SDL_GetTicksNS();
	float elapsed = clouds->refresh_ticks != 0 ? SDL_min((float)(now - clouds->refresh_ticks) * 1e-9f, 0.25f) : 0.0f;
	float refresh_seconds = SDL_max(settings->cloud_refresh_seconds, 0.01f);
	float memory_seconds = SDL_max(settings->cloud_memory_seconds, 0.01f);

	float dx = eye[0] - clouds->dome_eye[0];
	float dy = eye[1] - clouds->dome_eye[1];
	float dz = eye[2] - clouds->dome_eye[2];
	float moved = SDL_sqrtf(dx * dx + dy * dy + dz * dz);
	float nearest = eye[1] < base_height ? base_height - eye[1] : eye[1] > top_height ? eye[1] - top_height : 0.0f;
	bool half = eye[1] < base_height && eye[1] < DOME_HALF_MAXIMUM_EYE_METERS;

	if (half != clouds->dome_half)
	{
		// The other mapping: everything anew
		clouds->dome_half = half;
		clouds->dome_valid = false;
	}
	clouds->refresh_ticks = now;
	dome_out[0] = (float)clouds->dome_size;
	dome_out[2] = (float)(clouds->frame & 15u);
	refresh_out[2] = 0.0f;
	refresh_out[3] = 0.0f;
	*passes_out = 1;
	if (!clouds->dome_valid || moved > DOME_JUMP_METERS)
	{
		dome_out[1] = 1.0f;
		dome_out[3] = 0.0f;
		refresh_out[0] = 1.0f;
		refresh_out[1] = (float)(clouds->frame & 1023u);
		clouds->dome_valid = true;
	}
	else if (moved > DOME_MOVING_RADIANS * SDL_max(nearest, 1.0f))
	{
		dome_out[1] = 2.0f;
		dome_out[3] = 1.0f;
		refresh_out[0] = DOME_WEIGHT_MOVING;
		refresh_out[1] = (float)(clouds->frame & 1023u);
	}
	else
	{
		// Each texel is marched once a refresh: what it held fades over the
		// memory's time
		clouds->passes_due = SDL_min(clouds->passes_due + elapsed * 16.0f / refresh_seconds, (float)DOME_MAXIMUM_PASSES);
		*passes_out = (int32_t)clouds->passes_due;
		clouds->passes_due -= (float)*passes_out;
		dome_out[1] = 4.0f;
		dome_out[3] = 0.0f;
		refresh_out[0] = 1.0f - SDL_expf(-refresh_seconds / memory_seconds);
		// (the pass's texel and its jitter: set for each pass, see clouds_draw)
		refresh_out[1] = 0.0f;
	}
	if (!clouds->dome_blends)
	{
		refresh_out[0] = 1.0f;
	}
	SDL_memcpy(clouds->dome_eye, eye, sizeof(clouds->dome_eye));
}

// The dome at the size asked for (0: from the screen's height), made anew
// when that changes
static bool make_dome(clouds_t *clouds, int32_t asked_size, uint32_t screen_height)
{
	uint32_t size = asked_size > 0 ? (uint32_t)asked_size : (uint32_t)((float)screen_height * DOME_AUTO_PER_PIXEL);

	// A multiple of 4 (the still refresh's blocks), in range
	size = (uint32_t)SDL_clamp((int32_t)size, DOME_MINIMUM_SIZE, DOME_MAXIMUM_SIZE);
	if (asked_size <= 0)
	{
		size = (size + 255u) / 256u * 256u;
	}
	size &= ~3u;
	if (clouds->dome_texture != NULL && clouds->dome_size == size)
	{
		return true;
	}
	release_texture(&clouds->dome_texture);
	clouds->dome_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, DOME_FORMAT, size, size, 1, 1, clouds->dome_usage);
	clouds->dome_size = clouds->dome_texture != NULL ? size : 0;
	clouds->dome_valid = false;
	if (clouds->dome_texture != NULL)
	{
		// Cleared: a new texture holds whatever the memory held before (on
		// some drivers what another texture just let go of), and the dome
		// fills a texel in sixteen at a time, blending with what is there
		size_t bytes = (size_t)size * (size_t)size * 8;
		void *zeros = SDL_calloc(1, bytes);

		if (zeros != NULL)
		{
			gpu_upload_texture(clouds->dome_texture, 0, 0, 0, 0, size, size, zeros, bytes);
			SDL_free(zeros);
		}
		SDL_Log("Volumetric clouds: dome of %u x %u texels (%.0f MB)", size, size, (double)size * size * 8.0 / 1048576.0);
	}
	return clouds->dome_texture != NULL;
}

// The flocks' map (the paint's flocks channel) up to the GPU when it
// changed, and how far they have grown: uniforms for the shaders (flock: the
// layer's bottom, its thickness, the size of each, how many, 0: none at all;
// flock_wind: the wind's direction, for their rows)
static bool update_flocks(clouds_t *clouds, const render_settings_t *settings, const cloud_paint_t *paint, float sky_seconds,
	float flock_out[4], float flock_wind_out[4])
{
	float amount = clamp_float(settings->flock_amount, 0.0f, 1.0f);
	float elapsed = sky_seconds - clouds->flock_clock;
	float wind = degrees_to_radians(settings->wind_direction_degrees);
	float size = clamp_float(settings->flock_size_meters, 20.0f, 5000.0f);

	if (clouds->flock_texture == NULL)
	{
		clouds->flock_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8_UNORM, CLOUD_PAINT_SIZE, CLOUD_PAINT_SIZE,
			1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
		clouds->flock_uploaded = false;
		if (clouds->flock_texture == NULL)
		{
			return false;
		}
	}
	if (!clouds->flock_uploaded || (paint != NULL && paint->revision != clouds->flock_revision))
	{
		uint8_t *texels = SDL_calloc((size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE, 1);

		if (texels == NULL)
		{
			return false;
		}
		clouds->flock_painted = false;
		if (paint != NULL)
		{
			for (size_t t = 0; t < (size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE; t++)
			{
				texels[t] = paint->values[t * 4 + CLOUD_PAINT_FLOCKS];
				clouds->flock_painted |= texels[t] != 0;
			}
			clouds->flock_revision = paint->revision;
		}
		gpu_upload_texture(clouds->flock_texture, 0, 0, 0, 0, CLOUD_PAINT_SIZE, CLOUD_PAINT_SIZE, texels,
			(size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE);
		SDL_free(texels);
		gpu_flush_uploads();
		clouds->flock_uploaded = true;
	}
	// Growing or dissolving, all of them at once: never past none or all,
	// so turning the trend round turns them at once. A new Amount starts over
	if (amount != clouds->flock_amount_seen)
	{
		clouds->flock_amount_seen = amount;
		clouds->flock_growth = 0.0f;
	}
	if (elapsed > 0.0f && elapsed < 60.0f)
	{
		clouds->flock_growth += clamp_float(settings->flock_trend, -1.0f, 1.0f) * elapsed / FLOCK_TREND_SECONDS;
	}
	clouds->flock_growth = clamp_float(clouds->flock_growth, -amount, 1.0f - amount);
	clouds->flock_clock = sky_seconds;

	flock_out[0] = SDL_max(settings->flock_height_meters, CLOUD_MINIMUM_HEIGHT_METERS);
	flock_out[1] = clamp_float(size * FLOCK_THICKNESS_SHARE, FLOCK_MINIMUM_THICKNESS, FLOCK_MAXIMUM_THICKNESS);
	flock_out[2] = size;
	flock_out[3] = clouds->flock_painted ? clamp_float(amount + clouds->flock_growth, 0.0f, 1.0f) : 0.0f;
	flock_wind_out[0] = SDL_cosf(wind);
	flock_wind_out[1] = SDL_sinf(wind);
	flock_wind_out[2] = clamp_float(settings->flock_density, 0.05f, 4.0f);
	flock_wind_out[3] = clamp_float(settings->flock_shade, 0.0f, 1.0f);
	return true;
}

SDL_GPURenderPass *clouds_draw(clouds_t *clouds, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, uint32_t width,
	uint32_t height, SDL_GPUTexture *variation_texture, const scene_uniforms_t *scene, const render_settings_t *settings,
	const cloud_paint_t *paint)
{
	SDL_GPURenderPass *pass;
	int32_t passes = 1;
	bool shadows_due = false;
	struct
	{
		float base_height;
		float thickness;
		float density;
		float steps;
		float shape_scale;
		float detail_scale;
		float maximum_distance;
		float frame;
		float vertical_factors[4];
		float dome[4];
		float refresh[4];
		float look[4];
		float flock[4];
		float flock_wind[4];
		float planet[4];
	} uniforms;

	if (clouds->composite_pipeline == NULL && !clouds->failed)
	{
		clouds->failed = !create(clouds);
		if (clouds->failed)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Volumetric clouds: cannot make them; the flat ones stay");
		}
		// The noise goes up first
		gpu_flush_uploads();
	}
	if (clouds->composite_pipeline == NULL || gpu.command_buffer == NULL || !make_dome(clouds, settings->cloud_dome_size, height))
	{
		return NULL;
	}
	{
		bool rebuilt;

		if (!cloud_weather_update(&clouds->weather, settings, paint, &rebuilt))
		{
			return NULL;
		}
		if (rebuilt)
		{
			// The new map goes up before the march reads it, and the whole
			// dome follows it (and the shadows)
			gpu_flush_uploads();
			clouds->dome_valid = false;
		}
		shadows_due = rebuilt || !clouds->shadow_ready
			|| (double)(SDL_GetTicksNS() - clouds->shadow_ticks) * 1e-9 > SHADOW_REFRESH_SECONDS;
	}

	uniforms.base_height = SDL_max(settings->cloud_height_meters, CLOUD_MINIMUM_HEIGHT_METERS);
	uniforms.thickness = SDL_max(settings->cloud_thickness_meters, 10.0f);
	choose_refresh(clouds, scene->eye_position, uniforms.base_height, uniforms.base_height + uniforms.thickness, settings, uniforms.dome,
		uniforms.refresh, &passes);
	uniforms.density = SDL_max(settings->cloud_density, 0.0f) * 0.02f;
	uniforms.steps = (float)SDL_clamp(settings->cloud_steps, CLOUD_MINIMUM_STEPS, 256);
	uniforms.shape_scale = 1.0f / (SHAPE_TILE_METERS * clamp_float(settings->cloud_size, 0.25f, 8.0f));
	uniforms.detail_scale = 1.0f / DETAIL_TILE_METERS;
	uniforms.maximum_distance = MAXIMUM_DISTANCE_METERS;
	uniforms.frame = (float)(clouds->frame++ & 1023u);
	// The noises only roughen the surfaces of the cloud map's shapes: as
	// lumpy upwards as across
	uniforms.vertical_factors[0] = uniforms.shape_scale;
	uniforms.vertical_factors[1] = uniforms.detail_scale;
	uniforms.vertical_factors[2] = 0.0f;
	uniforms.vertical_factors[3] = 0.0f;
	uniforms.look[0] = clamp_float(settings->cloud_towers, 0.0f, 1.0f);
	uniforms.look[1] = clamp_float(settings->cloud_base_darkness, 0.0f, 1.0f);
	uniforms.look[2] = clouds->dome_half ? 1.0f : 0.0f;
	uniforms.look[3] = clamp_float(settings->cloud_size, 0.25f, 8.0f);
	uniforms.planet[0] = clamp_float(settings->cloud_horizon_drop_meters, 0.0f, CLOUD_MAXIMUM_HORIZON_DROP_METERS);
	// At night the clouds have only the dark sky's light and the moon's:
	// their light from the sky is let down with the night (the stars'
	// visibility, daylight.h), or they glow gray against the black
	uniforms.planet[1] = 1.0f - CLOUD_NIGHT_DARKENING * SDL_clamp(scene->fog_parameters[1], 0.0f, 1.0f);
	uniforms.planet[2] = 0.0f;
	uniforms.planet[3] = 0.0f;
	if (!update_flocks(clouds, settings, paint, scene->padding[2], uniforms.flock, uniforms.flock_wind))
	{
		return NULL;
	}

	// This frame's share of the dome (none, if the clock says nothing is due)
	if (passes > 0)
	{
		SDL_GPUStorageTextureReadWriteBinding dome;
		SDL_GPUTextureSamplerBinding textures[7] =
		{
			{ variation_texture, gpu.linear_repeat },
			{ clouds->shape_texture, gpu.linear_repeat },
			{ clouds->detail_texture, gpu.linear_repeat },
			{ clouds->weather.map_texture, gpu.nearest_clamp },
			{ clouds->weather.atlas_texture, gpu.linear_clamp },
			{ clouds->weather.density_atlas_texture, gpu.linear_clamp },
			{ clouds->flock_texture, gpu.linear_repeat },
		};
		uint32_t cells = clouds->dome_size / (uint32_t)uniforms.dome[1];
		SDL_GPUComputePass *compute;

		SDL_zero(dome);
		dome.texture = clouds->dome_texture;
		compute = SDL_BeginGPUComputePass(gpu.command_buffer, &dome, 1, NULL, 0);
		if (compute == NULL)
		{
			return NULL;
		}
		SDL_BindGPUComputePipeline(compute, clouds->dome_pipeline);
		SDL_BindGPUComputeSamplers(compute, 0, textures, 7);
		SDL_BindGPUComputeStorageBuffers(compute, 0, &clouds->weather.stamp_buffer, 1);
		SDL_PushGPUComputeUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		for (int32_t p = 0; p < passes; p++)
		{
			if (uniforms.dome[1] == 4.0f)
			{
				// Still: the next of the sixteen texels of each block, and its
				// jitter (a new one each time round the sixteen)
				uniforms.dome[2] = (float)(clouds->pass_count & 15u);
				uniforms.refresh[1] = (float)((clouds->pass_count >> 4) & 1023u);
				clouds->pass_count++;
			}
			SDL_PushGPUComputeUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
			SDL_DispatchGPUCompute(compute, (cells * cells + 63) / 64, 1, 1);
		}
		SDL_EndGPUComputePass(compute);
	}

	// The shadows on the land, now and then (their columns of the layer,
	// in the cloud map's frame; the land reads them from the next frame)
	if (shadows_due)
	{
		SDL_GPUStorageTextureReadWriteBinding shadow;
		SDL_GPUTextureSamplerBinding textures[7] =
		{
			{ variation_texture, gpu.linear_repeat },
			{ clouds->shape_texture, gpu.linear_repeat },
			{ clouds->detail_texture, gpu.linear_repeat },
			{ clouds->weather.map_texture, gpu.nearest_clamp },
			{ clouds->weather.atlas_texture, gpu.linear_clamp },
			{ clouds->weather.density_atlas_texture, gpu.linear_clamp },
			{ clouds->flock_texture, gpu.linear_repeat },
		};
		SDL_GPUComputePass *compute;

		SDL_zero(shadow);
		shadow.texture = clouds->shadow_texture;
		compute = SDL_BeginGPUComputePass(gpu.command_buffer, &shadow, 1, NULL, 0);
		if (compute != NULL)
		{
			SDL_BindGPUComputePipeline(compute, clouds->shadow_pipeline);
			SDL_BindGPUComputeSamplers(compute, 0, textures, 7);
			SDL_BindGPUComputeStorageBuffers(compute, 0, &clouds->weather.stamp_buffer, 1);
			SDL_PushGPUComputeUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
			SDL_PushGPUComputeUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
			SDL_DispatchGPUCompute(compute, (SHADOW_MAP_SIZE * SHADOW_MAP_SIZE + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(compute);
			clouds->shadow_ready = true;
			clouds->shadow_ticks = SDL_GetTicksNS();
			shadow_for_shading = clouds->shadow_texture;
		}
	}

	// Onto the scene
	{
		SDL_GPUColorTargetInfo color;
		SDL_GPUTextureSamplerBinding textures[2] =
		{
			{ clouds->dome_texture, gpu.linear_clamp },
			{ depth_texture, gpu.nearest_clamp },
		};

		SDL_zero(color);
		color.texture = color_texture;
		color.load_op = SDL_GPU_LOADOP_LOAD;
		color.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass(gpu.command_buffer, &color, 1, NULL);
		if (pass == NULL)
		{
			return NULL;
		}
		{
			SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };

			SDL_SetGPUViewport(pass, &viewport);
		}
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
		SDL_BindGPUGraphicsPipeline(pass, clouds->composite_pipeline);
		SDL_BindGPUFragmentSamplers(pass, 0, textures, 2);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	}
	return pass;
}
