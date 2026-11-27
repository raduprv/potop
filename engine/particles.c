// Particles. See particles.h.

#include "particles.h"

#include <SDL3/SDL.h>

#include "ini_parser.h"
#include "terrain_renderer.h"
#include "scene_uniforms.h"

#define TEXTURE_SIZE 64
#define FLAME_FRAMES 8
#define SMOKE_FRAMES 4
#define GROUP_SIZE 64

// The state buffer's layout (see particles_common.glsl)
#define STATE_LIST(k) (4 + 8 * (k))
#define STATE_DRAW 20
#define STATE_FREE_FIRST 32

// Floats a packed effect takes in the uniform block (std140: 13 vec4)
#define EFFECT_FLOATS 60
#define RENDER_FLOATS 20
#define EMIT_FLOATS 20

typedef struct
{
	const char *name;
	int layer;
	int frames;
} texture_entry_t;

enum
{
	LAYER_DOT = 0,
	LAYER_SPARK,
	LAYER_STAR,
	LAYER_RING,
	LAYER_SMOKE,
	LAYER_FLAME = LAYER_SMOKE + SMOKE_FRAMES,
	LAYER_COUNT = LAYER_FLAME + FLAME_FRAMES
};

static const texture_entry_t texture_entries[] = {
	{ "dot", LAYER_DOT, 1 },
	{ "spark", LAYER_SPARK, 1 },
	{ "star", LAYER_STAR, 1 },
	{ "ring", LAYER_RING, 1 },
	{ "smoke", LAYER_SMOKE, SMOKE_FRAMES },
	{ "flame", LAYER_FLAME, FLAME_FRAMES },
};

static const char *shape_names[PARTICLES_SHAPE_COUNT] = { "point", "sphere", "disc", "ring", "box", "outline" };
static const char *collide_names[PARTICLES_COLLIDE_COUNT] = { "none", "die", "bounce", "slide" };

// ---------------------------------------------------------------------------
// Textures, made here

static float hash_2d(int x, int y, int seed)
{
	uint32_t value = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;

	value = (value ^ (value >> 13)) * 1274126177u;
	value ^= value >> 16;
	return (float)(value & 0xffffff) / 16777215.0f;
}

// Smooth noise, 0..1, wrapping every period cells
static float value_noise(float x, float y, int period, int seed)
{
	int cell_x = (int)SDL_floorf(x);
	int cell_y = (int)SDL_floorf(y);
	float fraction_x = x - (float)cell_x;
	float fraction_y = y - (float)cell_y;
	float smooth_x = fraction_x * fraction_x * (3.0f - 2.0f * fraction_x);
	float smooth_y = fraction_y * fraction_y * (3.0f - 2.0f * fraction_y);
	int x0 = ((cell_x % period) + period) % period;
	int y0 = ((cell_y % period) + period) % period;
	int x1 = (x0 + 1) % period;
	int y1 = (y0 + 1) % period;
	float top = hash_2d(x0, y0, seed) + (hash_2d(x1, y0, seed) - hash_2d(x0, y0, seed)) * smooth_x;
	float bottom = hash_2d(x0, y1, seed) + (hash_2d(x1, y1, seed) - hash_2d(x0, y1, seed)) * smooth_x;

	return top + (bottom - top) * smooth_y;
}

static float fractal_noise(float x, float y, int seed)
{
	float sum = 0.0f;
	float amplitude = 0.5f;
	int period = 4;

	for (int octave = 0; octave < 4; octave++)
	{
		sum += amplitude * value_noise(x * (float)period, y * (float)period, period, seed + octave * 17);
		amplitude *= 0.5f;
		period *= 2;
	}
	return sum;
}

static float smooth_step(float edge_0, float edge_1, float x)
{
	float t = SDL_clamp((x - edge_0) / (edge_1 - edge_0), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

// One layer's coverage at (u, v), both -1..1 from the middle
static float texture_value(int layer, float u, float v)
{
	float distance = SDL_sqrtf(u * u + v * v);

	if (layer == LAYER_DOT)
	{
		float fade = SDL_max(1.0f - distance, 0.0f);

		return fade * fade;
	}
	if (layer == LAYER_SPARK)
	{
		// Long along v: a bright core, thin
		float across = SDL_max(1.0f - SDL_fabsf(u) * 3.0f, 0.0f);
		float along = SDL_max(1.0f - SDL_fabsf(v), 0.0f);

		return across * across * SDL_sqrtf(along);
	}
	if (layer == LAYER_STAR)
	{
		// Four thin rays and a glow
		float rays = SDL_max(1.0f - SDL_fabsf(u) * 12.0f, 0.0f) * SDL_max(1.0f - SDL_fabsf(v), 0.0f)
			+ SDL_max(1.0f - SDL_fabsf(v) * 12.0f, 0.0f) * SDL_max(1.0f - SDL_fabsf(u), 0.0f);
		float glow = SDL_max(1.0f - distance * 2.2f, 0.0f);

		return SDL_min(rays + glow * glow, 1.0f);
	}
	if (layer == LAYER_RING)
	{
		float band = 1.0f - SDL_fabsf(distance - 0.75f) * 6.0f;

		return SDL_max(band, 0.0f) * SDL_max(band, 0.0f);
	}
	if (layer >= LAYER_SMOKE && layer < LAYER_SMOKE + SMOKE_FRAMES)
	{
		// A puff: a soft disc broken up by noise (a different one a frame)
		float puff = smooth_step(1.0f, 0.3f, distance);
		float noise = fractal_noise(u * 0.5f + 0.5f, v * 0.5f + 0.5f, 11 + layer * 31);

		return SDL_clamp(puff * (noise * 1.6f - 0.15f), 0.0f, 1.0f);
	}
	if (layer >= LAYER_FLAME && layer < LAYER_FLAME + FLAME_FRAMES)
	{
		// A tongue: wide at the bottom (v = -1), pointed at the top, its
		// edge licked by noise that rises from frame to frame
		float frame = (float)(layer - LAYER_FLAME) / (float)FLAME_FRAMES;
		float height = v * 0.5f + 0.5f;
		float width = 0.75f * (1.0f - height) * (0.6f + 0.4f * SDL_sqrtf(SDL_max(1.0f - height, 0.0f)));
		float noise = fractal_noise(u * 0.25f + 0.5f, height * 0.5f - frame * 0.5f, 101);
		float edge = width * (0.6f + 0.8f * noise);
		float inside = smooth_step(edge, 0.0f, SDL_fabsf(u));

		return SDL_sqrtf(inside) * smooth_step(0.0f, 0.25f, height) * smooth_step(1.0f, 0.5f, height + 0.3f * (noise - 0.5f));
	}
	return 0.0f;
}

static void make_textures(particles_t *particles)
{
	uint8_t *pixels = SDL_malloc((size_t)TEXTURE_SIZE * TEXTURE_SIZE * LAYER_COUNT);

	if (pixels == NULL)
	{
		return;
	}
	for (int layer = 0; layer < LAYER_COUNT; layer++)
	{
		for (int y = 0; y < TEXTURE_SIZE; y++)
		{
			for (int x = 0; x < TEXTURE_SIZE; x++)
			{
				float u = ((float)x + 0.5f) / (float)TEXTURE_SIZE * 2.0f - 1.0f;
				float v = ((float)y + 0.5f) / (float)TEXTURE_SIZE * 2.0f - 1.0f;
				float value = SDL_clamp(texture_value(layer, u, v), 0.0f, 1.0f);

				pixels[((size_t)layer * TEXTURE_SIZE + (size_t)y) * TEXTURE_SIZE + (size_t)x] = (uint8_t)(value * 255.0f + 0.5f);
			}
		}
	}
	particles->texture_array = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8_UNORM, TEXTURE_SIZE, TEXTURE_SIZE,
		LAYER_COUNT, 0, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	if (particles->texture_array != NULL)
	{
		for (int layer = 0; layer < LAYER_COUNT; layer++)
		{
			gpu_upload_texture(particles->texture_array, 0, (uint32_t)layer, 0, 0, TEXTURE_SIZE, TEXTURE_SIZE,
				&pixels[(size_t)layer * TEXTURE_SIZE * TEXTURE_SIZE], (size_t)TEXTURE_SIZE * TEXTURE_SIZE);
		}
		gpu_generate_mipmaps(particles->texture_array);
	}
	particles->texture_layer_count = LAYER_COUNT;
	SDL_free(pixels);
}

// ---------------------------------------------------------------------------
// Effects, from data/particles.ini

static void default_effect(particles_effect_t *effect, const char *name)
{
	SDL_zerop(effect);
	SDL_strlcpy(effect->name, name, sizeof(effect->name));
	effect->texture_layer = LAYER_DOT;
	effect->texture_frames = 1;
	effect->intensity = 1.0f;
	effect->rate = 20.0f;
	effect->count = 50;
	effect->duration_seconds = 0.0f;
	effect->shape = PARTICLES_SHAPE_POINT;
	effect->life_minimum = 1.0f;
	effect->life_maximum = 1.0f;
	effect->speed_minimum = 1.0f;
	effect->speed_maximum = 1.0f;
	effect->direction[1] = 1.0f;
	effect->spread_degrees = 30.0f;
	effect->turbulence_scale = 1.0f;
	effect->bounce = 0.4f;
	effect->random_turn = true;
	effect->color_stop_count = 2;
	effect->color_times[1] = 1.0f;
	effect->colors[0][0] = effect->colors[0][1] = effect->colors[0][2] = effect->colors[0][3] = 1.0f;
	effect->colors[1][0] = effect->colors[1][1] = effect->colors[1][2] = 1.0f;
	effect->size_stop_count = 1;
	effect->sizes[0] = 0.2f;
	light_source_reset(&effect->light);
}

// "a" or "a, b": a range (one number: both ends)
static bool parse_range(const char *text, float *minimum, float *maximum)
{
	char copy[64];
	char *items[2];
	int count;

	SDL_strlcpy(copy, text, sizeof(copy));
	count = ini_split_list(copy, items, 2);
	if (count < 1 || !ini_value_to_float(items[0], minimum))
	{
		return false;
	}
	*maximum = *minimum;
	return count < 2 || ini_value_to_float(items[1], maximum);
}

// Stops separated by "|": "time value..." each, values_per_stop numbers
// after the time. Returns the number of stops, 0 if the text is wrong
static int parse_stops(const char *text, int values_per_stop, float scale, float times[PARTICLES_STOPS_MAX], float *values)
{
	char copy[256];
	char *cursor = copy;
	int count = 0;

	SDL_strlcpy(copy, text, sizeof(copy));
	while (cursor != NULL && *cursor != '\0' && count < PARTICLES_STOPS_MAX)
	{
		char *end = SDL_strchr(cursor, '|');
		char *number = cursor;
		float numbers[5];

		if (end != NULL)
		{
			*end = '\0';
		}
		for (int i = 0; i <= values_per_stop; i++)
		{
			char *after;
			double parsed = SDL_strtod(number, &after);

			if (after == number)
			{
				return 0;
			}
			numbers[i] = (float)parsed;
			number = after;
		}
		times[count] = SDL_clamp(numbers[0], 0.0f, 1.0f);
		for (int i = 0; i < values_per_stop; i++)
		{
			values[count * values_per_stop + i] = numbers[1 + i] * scale;
		}
		count++;
		cursor = end != NULL ? end + 1 : NULL;
	}
	return count;
}

static bool parse_vector(const char *text, float vector_out[3])
{
	char copy[96];
	char *items[3];
	float length;

	SDL_strlcpy(copy, text, sizeof(copy));
	if (ini_split_list(copy, items, 3) != 3 || !ini_value_to_float(items[0], &vector_out[0]) || !ini_value_to_float(items[1], &vector_out[1])
		|| !ini_value_to_float(items[2], &vector_out[2]))
	{
		return false;
	}
	length = SDL_sqrtf(vector_out[0] * vector_out[0] + vector_out[1] * vector_out[1] + vector_out[2] * vector_out[2]);
	if (length < 1e-6f)
	{
		vector_out[1] = 1.0f;
		return true;
	}
	for (int i = 0; i < 3; i++)
	{
		vector_out[i] /= length;
	}
	return true;
}

static int find_name(const char *const *names, int count, const char *value)
{
	for (int i = 0; i < count; i++)
	{
		if (SDL_strcasecmp(names[i], value) == 0)
		{
			return i;
		}
	}
	return -1;
}

typedef struct
{
	particles_effect_t effects[PARTICLES_EFFECTS_MAX];
	int count;
} load_state_t;

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	load_state_t *state = user_data;
	particles_effect_t *effect;

	if (section == NULL || section[0] == '\0')
	{
		return false;
	}
	if (state->count == 0 || SDL_strcmp(state->effects[state->count - 1].name, section) != 0)
	{
		if (state->count == PARTICLES_EFFECTS_MAX)
		{
			return false;
		}
		default_effect(&state->effects[state->count++], section);
	}
	effect = &state->effects[state->count - 1];
	{
		bool ok;

		if (light_source_parse(&effect->light, key, value, &ok))
		{
			return ok;
		}
	}
	if (SDL_strcmp(key, "texture") == 0)
	{
		for (size_t i = 0; i < SDL_arraysize(texture_entries); i++)
		{
			if (SDL_strcasecmp(texture_entries[i].name, value) == 0)
			{
				effect->texture_layer = texture_entries[i].layer;
				effect->texture_frames = texture_entries[i].frames;
				return true;
			}
		}
		return false;
	}
	if (SDL_strcmp(key, "frames_per_second") == 0)
	{
		return ini_value_to_float(value, &effect->frames_per_second);
	}
	if (SDL_strcmp(key, "blend") == 0)
	{
		effect->emissive = SDL_strcasecmp(value, "additive") == 0 || SDL_strcasecmp(value, "glow") == 0;
		return effect->emissive || SDL_strcasecmp(value, "alpha") == 0;
	}
	if (SDL_strcmp(key, "intensity") == 0)
	{
		return ini_value_to_float(value, &effect->intensity);
	}
	if (SDL_strcmp(key, "distortion") == 0)
	{
		return ini_value_to_float(value, &effect->distortion);
	}
	if (SDL_strcmp(key, "soft") == 0)
	{
		return ini_value_to_float(value, &effect->soft_meters);
	}
	if (SDL_strcmp(key, "emit") == 0)
	{
		effect->burst = SDL_strcasecmp(value, "burst") == 0;
		return effect->burst || SDL_strcasecmp(value, "continuous") == 0;
	}
	if (SDL_strcmp(key, "rate") == 0)
	{
		return ini_value_to_float(value, &effect->rate);
	}
	if (SDL_strcmp(key, "count") == 0)
	{
		return ini_value_to_int(value, &effect->count);
	}
	if (SDL_strcmp(key, "duration") == 0)
	{
		return ini_value_to_float(value, &effect->duration_seconds);
	}
	if (SDL_strcmp(key, "shape") == 0)
	{
		int shape = find_name(shape_names, PARTICLES_SHAPE_COUNT, value);

		effect->shape = shape >= 0 ? (particles_shape_t)shape : effect->shape;
		return shape >= 0;
	}
	if (SDL_strcmp(key, "shape_radius") == 0)
	{
		return ini_value_to_float(value, &effect->shape_radius);
	}
	if (SDL_strcmp(key, "life") == 0)
	{
		return parse_range(value, &effect->life_minimum, &effect->life_maximum);
	}
	if (SDL_strcmp(key, "speed") == 0)
	{
		return parse_range(value, &effect->speed_minimum, &effect->speed_maximum);
	}
	if (SDL_strcmp(key, "direction") == 0)
	{
		return parse_vector(value, effect->direction);
	}
	if (SDL_strcmp(key, "spread_degrees") == 0)
	{
		return ini_value_to_float(value, &effect->spread_degrees);
	}
	if (SDL_strcmp(key, "outward") == 0)
	{
		return ini_value_to_float(value, &effect->outward);
	}
	if (SDL_strcmp(key, "gravity") == 0)
	{
		return ini_value_to_float(value, &effect->gravity);
	}
	if (SDL_strcmp(key, "drag") == 0)
	{
		return ini_value_to_float(value, &effect->drag);
	}
	if (SDL_strcmp(key, "turbulence") == 0)
	{
		return ini_value_to_float(value, &effect->turbulence);
	}
	if (SDL_strcmp(key, "turbulence_scale") == 0)
	{
		return ini_value_to_float(value, &effect->turbulence_scale);
	}
	if (SDL_strcmp(key, "spin") == 0)
	{
		return parse_range(value, &effect->spin_minimum, &effect->spin_maximum);
	}
	if (SDL_strcmp(key, "random_turn") == 0)
	{
		return ini_value_to_bool(value, &effect->random_turn);
	}
	if (SDL_strcmp(key, "stretch") == 0)
	{
		return ini_value_to_float(value, &effect->stretch);
	}
	if (SDL_strcmp(key, "flash") == 0)
	{
		// burst seconds, flash seconds, flashes a burst, sync
		float burst = 0.0f;
		float flash = 0.0f;
		float count = 0.0f;
		float sync = 1.0f;

		if (SDL_sscanf(value, "%f , %f , %f , %f", &burst, &flash, &count, &sync) < 3)
		{
			return false;
		}
		effect->flash_burst_seconds = SDL_max(burst, 0.0f);
		effect->flash_seconds = SDL_max(flash, 0.01f);
		effect->flash_count = SDL_max(count, 1.0f);
		effect->flash_sync = SDL_clamp(sync, 0.0f, 1.0f);
		return true;
	}
	if (SDL_strcmp(key, "collide") == 0)
	{
		int collide = find_name(collide_names, PARTICLES_COLLIDE_COUNT, value);

		effect->collide = collide >= 0 ? (particles_collide_t)collide : effect->collide;
		return collide >= 0;
	}
	if (SDL_strcmp(key, "bounce") == 0)
	{
		return ini_value_to_float(value, &effect->bounce);
	}
	if (SDL_strcmp(key, "color") == 0)
	{
		float values[PARTICLES_STOPS_MAX * 4];
		int count = parse_stops(value, 4, 1.0f / 255.0f, effect->color_times, values);

		if (count == 0)
		{
			return false;
		}
		effect->color_stop_count = count;
		SDL_memcpy(effect->colors, values, sizeof(float) * 4 * (size_t)count);
		return true;
	}
	if (SDL_strcmp(key, "size") == 0)
	{
		int count = parse_stops(value, 1, 1.0f, effect->size_times, effect->sizes);

		if (count == 0)
		{
			return false;
		}
		effect->size_stop_count = count;
		return true;
	}
	return false;
}

// 15 vec4, as effect_t in particles_common.glsl
static void pack_effect(const particles_effect_t *effect, float out[EFFECT_FLOATS])
{
	SDL_memset(out, 0, sizeof(float) * EFFECT_FLOATS);
	for (int i = 0; i < PARTICLES_STOPS_MAX; i++)
	{
		int stop = SDL_min(i, effect->color_stop_count - 1);
		int size_stop = SDL_min(i, effect->size_stop_count - 1);

		SDL_memcpy(&out[i * 4], effect->colors[stop], sizeof(float) * 4);
		out[16 + i] = effect->color_times[stop];
		out[20 + i] = effect->sizes[size_stop];
		out[24 + i] = effect->size_times[size_stop];
	}
	out[28] = effect->gravity;
	out[29] = effect->drag;
	out[30] = effect->turbulence;
	out[31] = 1.0f / SDL_max(effect->turbulence_scale, 0.01f);
	out[32] = (float)effect->texture_layer;
	out[33] = (float)effect->texture_frames;
	out[34] = effect->frames_per_second;
	// (glowing: its intensity, above 0; lit: 0)
	out[35] = effect->emissive ? SDL_max(effect->intensity, 0.01f) : 0.0f;
	out[36] = (float)effect->collide;
	out[37] = effect->bounce;
	out[38] = effect->spin_minimum;
	out[39] = effect->spin_maximum;
	out[40] = effect->life_minimum;
	out[41] = effect->life_maximum;
	out[42] = effect->speed_minimum;
	out[43] = effect->speed_maximum;
	SDL_memcpy(&out[44], effect->direction, sizeof(float) * 3);
	out[47] = SDL_cosf(SDL_clamp(effect->spread_degrees, 0.0f, 180.0f) * (SDL_PI_F / 180.0f));
	out[48] = effect->outward;
	// Its sign: whether it is born turned any way
	out[49] = effect->random_turn ? effect->stretch : -1.0f - effect->stretch;
	out[50] = (float)effect->color_stop_count;
	out[51] = (float)effect->size_stop_count;
	out[52] = effect->flash_burst_seconds;
	out[53] = effect->flash_seconds;
	out[54] = effect->flash_count;
	out[55] = effect->flash_sync;
	out[56] = effect->distortion;
	out[57] = effect->soft_meters;
}

static void upload_effects(particles_t *particles)
{
	float *packed = SDL_calloc(PARTICLES_EFFECTS_MAX, sizeof(float) * EFFECT_FLOATS);

	if (packed == NULL)
	{
		return;
	}
	for (int i = 0; i < particles->effect_count; i++)
	{
		pack_effect(&particles->effects[i], &packed[i * EFFECT_FLOATS]);
	}
	gpu_upload_buffer(particles->effect_buffer, 0, packed, sizeof(float) * EFFECT_FLOATS * PARTICLES_EFFECTS_MAX);
	SDL_free(packed);
}

static void load_effects(particles_t *particles)
{
	load_state_t *state = SDL_calloc(1, sizeof(load_state_t));

	if (state == NULL)
	{
		return;
	}
	if (!ini_parse_data_file("particles.ini", load_key, state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Particles: no data/particles.ini, no effects");
	}
	SDL_memcpy(particles->effects, state->effects, sizeof(state->effects));
	particles->effect_count = state->count;
	SDL_free(state);
	SDL_Log("Particles: %d effects", particles->effect_count);
}

// ---------------------------------------------------------------------------
// GPU side

static void reset_state(particles_t *particles)
{
	size_t count = STATE_FREE_FIRST + 3 * (size_t)particles->capacity;
	uint32_t *state = SDL_calloc(count, sizeof(uint32_t));

	if (state == NULL)
	{
		return;
	}
	state[0] = (uint32_t)particles->capacity;
	for (int k = 0; k < 2; k++)
	{
		state[STATE_LIST(k)] = 4;
	}
	state[STATE_DRAW] = 4;
	for (int i = 0; i < particles->capacity; i++)
	{
		state[STATE_FREE_FIRST + i] = (uint32_t)(particles->capacity - 1 - i);
	}
	gpu_upload_buffer(particles->state_buffer, 0, state, count * sizeof(uint32_t));
	gpu_upload_buffer(particles->argument_buffer, 0, state, STATE_FREE_FIRST * sizeof(uint32_t));
	particles->current_list = 0;
	SDL_free(state);
}

// particle_pass_block in particles_common.glsl
typedef struct
{
	int32_t list;
	int32_t emit_total;
	int32_t emit_count;
	int32_t particle_capacity;
	float step_seconds;
	float time_seconds;
	int32_t map_cells;
	float cell_size_meters;
	int32_t has_water;
	int32_t padding[3];
} particle_pass_uniforms_t;

// particle_draw_block in particles.vert and particles.frag
typedef struct
{
	float camera_right[3];
	float light;
	float camera_up[3];
	float sky_light;
	float fog_sky_color[3];
	float pixel_per_distance;      // a pixel's size, meters, a meter from the eye
	float depth_to_distance[4];    // x, y: the scene's depth to the distance along the view, y / (depth - x); zw: 1 / its size
	float time_seconds;
	float has_depth;               // 1: the scene's depth is there (the soft ones fade)
	float padding[2];
} particle_draw_uniforms_t;

static bool create_pipelines(particles_t *particles)
{
	gpu_compute_layout_t layout = { 0, 2, 3, 1, GROUP_SIZE, 0, 0 };
	static const SDL_GPUVertexBufferDescription render_buffer = { 0, sizeof(float) * RENDER_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 };
	static const SDL_GPUVertexAttribute attributes[5] =
	{
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
		{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 8 },
		{ 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 12 },
		{ 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 16 },
	};
	SDL_GPUShader *vertex;
	SDL_GPUShader *fragment;
	gpu_pipeline_info_t info;

	particles->emit_pipeline = gpu_load_compute("particles_emit.comp", &layout);
	layout.threads_x = 1;
	particles->prepare_pipeline = gpu_load_compute("particles_prepare.comp", &layout);
	layout.threads_x = GROUP_SIZE;
	layout.samplers = 2;
	particles->simulate_pipeline = gpu_load_compute("particles_simulate.comp", &layout);

	vertex = gpu_load_shader("particles.vert", 2, 0, 2);
	fragment = gpu_load_shader("particles.frag", 2, 0, 2);
	if (vertex != NULL && fragment != NULL)
	{
		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = &render_buffer;
		info.vertex_buffer_count = 1;
		info.vertex_attributes = attributes;
		info.vertex_attribute_count = 5;
		info.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
		info.cull = SDL_GPU_CULLMODE_NONE;
		info.depth_test = true;
		info.depth_write = false;
		info.blend = GPU_BLEND_PREMULTIPLIED;
		particles->draw_pipeline = gpu_make_pipeline(&info, "particles");
	}
	SDL_ReleaseGPUShader(gpu.device, vertex);
	SDL_ReleaseGPUShader(gpu.device, fragment);
	return particles->emit_pipeline != NULL && particles->prepare_pipeline != NULL && particles->simulate_pipeline != NULL
		&& particles->draw_pipeline != NULL;
}

bool particles_create(particles_t *particles, int capacity)
{
	SDL_GPUBufferUsageFlags read_write = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;

	SDL_zerop(particles);
	particles->capacity = SDL_clamp(capacity, 1024, 4 * 1024 * 1024);
	particles->next_emitter_id = 1;
	particles->random_state = 0x2545f491u;
	if (!create_pipelines(particles))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Particles: the shaders could not be made, no particles");
		particles_destroy(particles);
		return false;
	}

	particles->particle_buffer = gpu_create_buffer(read_write, (uint32_t)(sizeof(float) * 12 * (size_t)particles->capacity));
	particles->render_buffer = gpu_create_buffer(read_write | SDL_GPU_BUFFERUSAGE_VERTEX,
		(uint32_t)(sizeof(float) * RENDER_FLOATS * (size_t)particles->capacity));
	particles->effect_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
		(uint32_t)(sizeof(float) * EFFECT_FLOATS * PARTICLES_EFFECTS_MAX));
	particles->emit_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
		(uint32_t)(sizeof(float) * EMIT_FLOATS * PARTICLES_EMITS_MAX));
	particles->state_buffer = gpu_create_buffer(read_write, (uint32_t)(sizeof(uint32_t) * (STATE_FREE_FIRST + 3 * (size_t)particles->capacity)));
	particles->argument_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDIRECT, (uint32_t)(sizeof(uint32_t) * STATE_FREE_FIRST));
	particles->blank_depth = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.depth_format, 1, 1, 1, 1,
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
	if (particles->particle_buffer == NULL || particles->render_buffer == NULL || particles->effect_buffer == NULL
		|| particles->emit_buffer == NULL || particles->state_buffer == NULL
		|| particles->argument_buffer == NULL || particles->blank_depth == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Particles: the GPU refused the buffers, no particles");
		particles_destroy(particles);
		return false;
	}
	reset_state(particles);

	make_textures(particles);
	load_effects(particles);
	upload_effects(particles);
	particles->created = particles->texture_array != NULL;
	SDL_Log("Particles: room for %d (%.1f MB on the GPU)", particles->capacity,
		(double)((size_t)particles->capacity * (12 + RENDER_FLOATS + 3) * sizeof(float)) / (1024.0 * 1024.0));
	return particles->created;
}

void particles_destroy(particles_t *particles)
{
	SDL_GPUComputePipeline *computes[3] = { particles->emit_pipeline, particles->prepare_pipeline, particles->simulate_pipeline };
	SDL_GPUBuffer *buffers[6] = { particles->argument_buffer, particles->particle_buffer, particles->state_buffer, particles->render_buffer, particles->effect_buffer,
		particles->emit_buffer };

	for (int i = 0; i < 3; i++)
	{
		if (computes[i] != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, computes[i]);
		}
	}
	if (particles->draw_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, particles->draw_pipeline);
	}
	for (int i = 0; i < 6; i++)
	{
		if (buffers[i] != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, buffers[i]);
		}
	}
	if (particles->texture_array != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, particles->texture_array);
	}
	if (particles->scene_depth != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, particles->scene_depth);
	}
	if (particles->blank_depth != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, particles->blank_depth);
	}
	SDL_zerop(particles);
}

void particles_reload(particles_t *particles)
{
	char names[PARTICLES_EMITTERS_MAX][PARTICLES_EFFECT_NAME_LENGTH];

	if (!particles->created)
	{
		return;
	}
	// The emitters find their effects again by name
	for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
	{
		const particles_emitter_t *emitter = &particles->emitters[i];

		names[i][0] = '\0';
		if (emitter->used && emitter->effect >= 0 && emitter->effect < particles->effect_count)
		{
			SDL_strlcpy(names[i], particles->effects[emitter->effect].name, PARTICLES_EFFECT_NAME_LENGTH);
		}
	}
	load_effects(particles);
	upload_effects(particles);
	for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
	{
		particles_emitter_t *emitter = &particles->emitters[i];

		if (emitter->used)
		{
			emitter->effect = particles_find_effect(particles, names[i]);
			emitter->used = emitter->effect >= 0;
		}
	}
}

int particles_find_effect(const particles_t *particles, const char *name)
{
	for (int i = 0; i < particles->effect_count; i++)
	{
		if (SDL_strcasecmp(particles->effects[i].name, name) == 0)
		{
			return i;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Emitters

uint32_t particles_play_on(particles_t *particles, int effect, const float position[3], const float rotation[4], const float extents[3])
{
	if (!particles->created || effect < 0 || effect >= particles->effect_count)
	{
		return 0;
	}
	for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
	{
		particles_emitter_t *emitter = &particles->emitters[i];

		if (!emitter->used)
		{
			SDL_zerop(emitter);
			emitter->used = true;
			emitter->id = particles->next_emitter_id++;
			if (particles->next_emitter_id == 0)
			{
				particles->next_emitter_id = 1;
			}
			emitter->effect = effect;
			emitter->object_body = -1;
			emitter->rate_scale = 1.0f;
			emitter->life_scale = 1.0f;
			SDL_memcpy(emitter->position, position, sizeof(emitter->position));
			SDL_memcpy(emitter->rotation, rotation, sizeof(emitter->rotation));
			SDL_memcpy(emitter->extents, extents, sizeof(emitter->extents));
			return emitter->id;
		}
	}
	return 0;
}

uint32_t particles_play(particles_t *particles, int effect, float x, float y, float z)
{
	float position[3] = { x, y, z };
	float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float extents[3] = { 0.0f, 0.0f, 0.0f };

	return particles_play_on(particles, effect, position, rotation, extents);
}

particles_emitter_t *particles_find_emitter(particles_t *particles, uint32_t id)
{
	for (int i = 0; i < PARTICLES_EMITTERS_MAX && id != 0; i++)
	{
		if (particles->emitters[i].used && particles->emitters[i].id == id)
		{
			return &particles->emitters[i];
		}
	}
	return NULL;
}

void particles_stop(particles_t *particles, uint32_t id)
{
	particles_emitter_t *emitter = particles_find_emitter(particles, id);

	if (emitter != NULL)
	{
		emitter->used = false;
	}
}

void particles_stop_all(particles_t *particles)
{
	for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
	{
		particles->emitters[i].used = false;
	}
}

uint32_t particles_nearest_emitter(const particles_t *particles, float x, float y, float z, float radius)
{
	uint32_t nearest = 0;
	float nearest_squared = radius * radius;

	for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
	{
		const particles_emitter_t *emitter = &particles->emitters[i];
		float dx;
		float dy;
		float dz;
		float squared;

		if (!emitter->used)
		{
			continue;
		}
		dx = emitter->position[0] - x;
		dy = emitter->position[1] - y;
		dz = emitter->position[2] - z;
		squared = dx * dx + dy * dy + dz * dz;
		if (squared <= nearest_squared)
		{
			nearest_squared = squared;
			nearest = emitter->id;
		}
	}
	return nearest;
}

int particles_emitter_count(const particles_t *particles)
{
	int count = 0;

	for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
	{
		count += particles->emitters[i].used ? 1 : 0;
	}
	return count;
}

// How many particles each emitter asks for this frame; burst emitters that
// are done go
static int gather_emits(particles_t *particles, float seconds, float *packed)
{
	int request_count = 0;
	int total = 0;

	for (int i = 0; i < PARTICLES_EMITTERS_MAX && request_count < PARTICLES_EMITS_MAX; i++)
	{
		particles_emitter_t *emitter = &particles->emitters[i];
		const particles_effect_t *effect;
		int count;
		float *request;
		float radius;

		if (!emitter->used)
		{
			continue;
		}
		if (emitter->effect < 0 || emitter->effect >= particles->effect_count)
		{
			emitter->used = false;
			continue;
		}
		effect = &particles->effects[emitter->effect];
		if (effect->burst)
		{
			int target = effect->count;

			if (effect->duration_seconds > 0.0f)
			{
				float done = SDL_clamp((emitter->age_seconds + seconds) / effect->duration_seconds, 0.0f, 1.0f);

				target = (int)SDL_floorf(done * (float)effect->count + 0.5f);
			}
			count = SDL_max(target - emitter->emitted, 0);
		}
		else
		{
			float owed = emitter->emit_carry + effect->rate * emitter->rate_scale * seconds;

			count = (int)SDL_floorf(owed);
			emitter->emit_carry = owed - (float)count;
		}
		emitter->age_seconds += seconds;
		emitter->emitted += count;
		// A burst is done once all of it is out; one that gives light (a
		// muzzle's flash) stays until its light has faded (lights.c reads
		// it from the emitter), emitting no more
		if (effect->burst && emitter->emitted >= effect->count
			&& (!light_source_gives_light(&effect->light)
				|| emitter->age_seconds >= effect->duration_seconds + SDL_max(effect->life_maximum, 0.05f)))
		{
			emitter->used = false;
		}
		if (count <= 0)
		{
			continue;
		}

		// Its own size where an effect leaves the radius to it
		radius = effect->shape_radius;
		if (radius <= 0.0f)
		{
			radius = SDL_max(SDL_max(emitter->extents[0], emitter->extents[2]), 0.1f);
		}
		request = &packed[request_count * EMIT_FLOATS];
		request[0] = emitter->position[0];
		request[1] = emitter->position[1];
		request[2] = emitter->position[2];
		request[3] = (float)emitter->effect;
		SDL_memcpy(&request[4], emitter->rotation, sizeof(float) * 4);
		request[8] = emitter->extents[0];
		request[9] = emitter->extents[1];
		request[10] = emitter->extents[2];
		if (effect->shape == PARTICLES_SHAPE_BOX || effect->shape == PARTICLES_SHAPE_OUTLINE)
		{
			// At a point, a box as big as the radius
			for (int axis = 0; axis < 3; axis++)
			{
				if (request[8 + axis] <= 0.0f)
				{
					request[8 + axis] = radius;
				}
			}
		}
		request[11] = (float)effect->shape;
		request[12] = (float)total;
		request[13] = (float)count;
		particles->random_state = particles->random_state * 1664525u + 1013904223u;
		request[14] = (float)(particles->random_state >> 8);
		request[15] = radius;
		request[16] = emitter->push[0];
		request[17] = emitter->push[1];
		request[18] = emitter->push[2];
		request[19] = emitter->life_scale;
		total += count;
		request_count++;
	}
	particles->emitted_last_frame = total;
	return request_count;
}

// One compute pass: reads and writes are not synchronized within a pass,
// so each step that needs the last one's results gets its own
static SDL_GPUComputePass *begin_compute(const particles_t *particles, SDL_GPUCommandBuffer *command_buffer,
	SDL_GPUComputePipeline *pipeline, const particle_pass_uniforms_t *uniforms)
{
	SDL_GPUStorageBufferReadWriteBinding read_write[3];
	SDL_GPUBuffer *read_only[2] = { particles->effect_buffer, particles->emit_buffer };
	SDL_GPUComputePass *pass;

	// Not cycled: the pool and the lists live on from frame to frame
	SDL_zeroa(read_write);
	read_write[0].buffer = particles->particle_buffer;
	read_write[1].buffer = particles->state_buffer;
	read_write[2].buffer = particles->render_buffer;
	pass = SDL_BeginGPUComputePass(command_buffer, NULL, 0, read_write, 3);

	if (pass != NULL)
	{
		SDL_BindGPUComputePipeline(pass, pipeline);
		SDL_BindGPUComputeStorageBuffers(pass, 0, read_only, 2);
		SDL_PushGPUComputeUniformData(command_buffer, 0, uniforms, sizeof(*uniforms));
	}
	return pass;
}

// The indirect arguments of both living lists, from the state into the
// argument buffer
static void copy_arguments(const particles_t *particles, SDL_GPUCommandBuffer *command_buffer)
{
	SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);
	SDL_GPUBufferLocation source = { particles->state_buffer, (uint32_t)(sizeof(uint32_t) * STATE_LIST(0)) };
	SDL_GPUBufferLocation destination = { particles->argument_buffer, (uint32_t)(sizeof(uint32_t) * STATE_LIST(0)) };

	if (copy_pass != NULL)
	{
		SDL_CopyGPUBufferToBuffer(copy_pass, &source, &destination, (uint32_t)(sizeof(uint32_t) * (STATE_DRAW + 4 - STATE_LIST(0))), false);
		SDL_EndGPUCopyPass(copy_pass);
	}
}

void particles_update(particles_t *particles, float seconds, SDL_GPUTexture *height_texture, SDL_GPUTexture *water_texture, int map_cells,
	float cell_size_meters)
{
	float packed[PARTICLES_EMITS_MAX * EMIT_FLOATS];
	int request_count;
	int list = particles->current_list;
	particle_pass_uniforms_t uniforms;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUComputePass *pass;

	if (!particles->created || height_texture == NULL)
	{
		return;
	}
	seconds = SDL_clamp(seconds, 0.0f, 0.25f);
	particles->time_seconds += seconds;
	request_count = seconds > 0.0f ? gather_emits(particles, seconds, packed) : 0;
	if (request_count > 0 && particles->emitted_last_frame > 0)
	{
		gpu_upload_buffer(particles->emit_buffer, 0, packed, sizeof(float) * EMIT_FLOATS * (size_t)request_count);
	}
	gpu_flush_uploads();
	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return;
	}

	SDL_zero(uniforms);
	uniforms.list = list;
	uniforms.emit_total = particles->emitted_last_frame;
	uniforms.emit_count = request_count;
	uniforms.particle_capacity = particles->capacity;
	uniforms.step_seconds = seconds;
	uniforms.time_seconds = particles->time_seconds;
	uniforms.map_cells = SDL_max(map_cells, 1);
	uniforms.cell_size_meters = SDL_max(cell_size_meters, 0.01f);
	uniforms.has_water = water_texture != NULL ? 1 : 0;

	// 1. New particles
	if (request_count > 0 && particles->emitted_last_frame > 0)
	{
		pass = begin_compute(particles, command_buffer, particles->emit_pipeline, &uniforms);
		if (pass != NULL)
		{
			SDL_DispatchGPUCompute(pass, (uint32_t)((particles->emitted_last_frame + GROUP_SIZE - 1) / GROUP_SIZE), 1, 1);
			SDL_EndGPUComputePass(pass);
		}
	}

	// 2. The simulation pass's size
	pass = begin_compute(particles, command_buffer, particles->prepare_pipeline, &uniforms);
	if (pass != NULL)
	{
		SDL_DispatchGPUCompute(pass, 1, 1, 1);
		SDL_EndGPUComputePass(pass);
	}
	copy_arguments(particles, command_buffer);

	// 3. Every living particle
	pass = begin_compute(particles, command_buffer, particles->simulate_pipeline, &uniforms);
	if (pass != NULL)
	{
		SDL_GPUTextureSamplerBinding textures[2] =
		{
			{ height_texture, gpu.nearest_clamp },
			{ water_texture != NULL ? water_texture : height_texture, gpu.nearest_clamp },
		};

		SDL_BindGPUComputeSamplers(pass, 0, textures, 2);
		SDL_DispatchGPUComputeIndirect(pass, particles->argument_buffer, (uint32_t)(sizeof(uint32_t) * (STATE_LIST(list) + 4)));
		SDL_EndGPUComputePass(pass);
	}
	// The survivors' draw arguments
	copy_arguments(particles, command_buffer);

	// The survivors are in the other list now: drawn from it, simulated
	// from it next time
	particles->current_list = 1 - list;
}

void particles_draw(particles_t *particles, SDL_GPURenderPass *pass, const camera_t *camera, const render_settings_t *settings, int target_height)
{
	const float *view = camera->view.m;
	vec3_t sun = terrain_renderer_sun_direction(settings);
	particle_draw_uniforms_t uniforms;
	SDL_GPUBufferBinding vertex_binding = { particles->render_buffer, 0 };
	SDL_GPUTextureSamplerBinding texture_bindings[2] =
	{
		{ particles->texture_array, gpu.linear_clamp },
		{ particles->depth_captured ? particles->scene_depth : particles->blank_depth, gpu.nearest_clamp },
	};

	if (!particles->created)
	{
		return;
	}
	SDL_zero(uniforms);
	uniforms.camera_right[0] = view[0];
	uniforms.camera_right[1] = view[4];
	uniforms.camera_right[2] = view[8];
	uniforms.camera_up[0] = view[1];
	uniforms.camera_up[1] = view[5];
	uniforms.camera_up[2] = view[9];
	// Lit particles (smoke): the sky's light and some of the sun's
	uniforms.light = SDL_clamp(0.55f * settings->ambient_intensity + 0.6f * settings->sun_intensity * SDL_max(sun.y, 0.0f), 0.25f, 1.2f);
	uniforms.sky_light = SDL_min(0.55f * settings->ambient_intensity, uniforms.light);
	SDL_memcpy(uniforms.fog_sky_color, settings->sky_color, sizeof(uniforms.fog_sky_color));
	uniforms.pixel_per_distance = 2.0f / (SDL_max(camera->projection.m[5], 1e-3f) * (float)SDL_max(target_height, 1));
	uniforms.depth_to_distance[0] = 0.5f - 0.5f * camera->projection.m[10];
	uniforms.depth_to_distance[1] = 0.5f * camera->projection.m[14];
	uniforms.depth_to_distance[2] = 1.0f / (float)SDL_max(particles->depth_width, 1);
	uniforms.depth_to_distance[3] = 1.0f / (float)SDL_max(particles->depth_height, 1);
	uniforms.time_seconds = particles->time_seconds;
	uniforms.has_depth = particles->depth_captured ? 1.0f : 0.0f;

	SDL_BindGPUGraphicsPipeline(pass, particles->draw_pipeline);
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
	SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
	SDL_BindGPUFragmentSamplers(pass, 0, texture_bindings, 2);
	scene_ground_bind_sky_vertex(pass, 0);
	// Slot by slot (the living list's order changes each frame: the alpha
	// ones, unsorted, would flicker); the free slots draw nothing
	SDL_DrawGPUPrimitivesIndirect(pass, particles->argument_buffer, (uint32_t)(sizeof(uint32_t) * STATE_DRAW), 1);
	particles->depth_captured = false;
}

void particles_capture_depth(particles_t *particles, SDL_GPUTexture *depth_texture, int width, int height)
{
	SDL_GPUCopyPass *copy;
	SDL_GPUTextureLocation source;
	SDL_GPUTextureLocation destination;

	particles->depth_captured = false;
	if (!particles->created || gpu.command_buffer == NULL || depth_texture == NULL || width <= 0 || height <= 0)
	{
		return;
	}
	if (particles->depth_width != width || particles->depth_height != height || particles->scene_depth == NULL)
	{
		if (particles->scene_depth != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, particles->scene_depth);
		}
		particles->scene_depth = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.depth_format, (uint32_t)width, (uint32_t)height, 1, 1,
			SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		particles->depth_width = particles->scene_depth != NULL ? width : 0;
		particles->depth_height = particles->scene_depth != NULL ? height : 0;
		if (particles->scene_depth == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Particles: cannot make the %d x %d copy of the depth: %s", width, height, SDL_GetError());
			return;
		}
	}
	copy = SDL_BeginGPUCopyPass(gpu.command_buffer);
	if (copy == NULL)
	{
		return;
	}
	SDL_zero(source);
	SDL_zero(destination);
	source.texture = depth_texture;
	destination.texture = particles->scene_depth;
	SDL_CopyGPUTextureToTexture(copy, &source, &destination, (uint32_t)width, (uint32_t)height, 1, false);
	SDL_EndGPUCopyPass(copy);
	particles->depth_captured = true;
}
