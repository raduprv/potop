// Rain and rainbows, as they are seen. See rain.h.

#include "rain.h"

#include <SDL3/SDL.h>

// They come and go over this long
#define RAIN_EASE_SECONDS 3.0f
#define RAINBOW_EASE_SECONDS 4.0f
// Things get soaked in about this long, and dry in about this long
#define RAIN_WET_SECONDS 20.0f
#define RAIN_DRY_SECONDS 90.0f

// The box of drops around the eye: its half width and its height (meters)
#define RAIN_BOX_RADIUS 30.0f
#define RAIN_BOX_HEIGHT 24.0f
// Drops in it at render.rain_intensity 1 (a downpour)
#define RAIN_MAXIMUM_DROPS 60000
// The snow (in the snow's months the rain falls as snow, snow.vert): this
// many flakes in a box of this half width at most, falling this fast, a
// flake this big, drifting this much with the wind (of a drop's: it
// falls nearly straight down, as heavy wet snow does, not a blizzard)
#define SNOW_MAXIMUM_FLAKES 270000
#define SNOW_BOX_RADIUS 22.0f
#define SNOW_FALL_SPEED 1.1f
#define SNOW_FLAKE_METERS 0.025f
#define SNOW_WIND_DRIFT 0.15f
// How fast they fall (meters per second: a raindrop of a few millimeters)
#define RAIN_FALL_SPEED 8.5f
// Splashes at once at full intensity, over a square this wide around the
// eye (half width, meters), each lasting this long
#define RAIN_MAXIMUM_SPLASHES 5000
#define RAIN_SPLASH_RADIUS 18.0f
#define RAIN_SPLASH_SECONDS 0.3f

// How much the rain takes from the light at full intensity: the sun, the
// sky's light, and how much thicker the haze gets
#define RAIN_SUN_DIMMING 0.65f
#define RAIN_AMBIENT_DIMMING 0.3f
#define RAIN_HAZE_THICKENING 2.5f
#define RAIN_SKY_GRAYING 0.6f

// The rainbow's sun: none at night, fading in as the sun clears the
// horizon (degrees)
#define RAINBOW_SUN_FROM -1.0f
#define RAINBOW_SUN_FULL 4.0f

// rain.vert, rain_splash.vert (std140, vertex uniform slot 1)
typedef struct
{
	float area[4];                 // the box's center (the eye) x, y, z; its half width
	float fall[4];                 // the wind x, z (m/s), the fall speed (m/s), the clock (seconds, wrapped)
	float look[4];                 // streak length (m), intensity (0..1), the box's height, meters per pixel at 1 m
	float map[4];                  // cells per side, cell size (m), the splashes' half width, a splash's life (s)
} rain_uniforms_t;

static SDL_GPUGraphicsPipeline *make_pipeline(const char *vertex_name, uint32_t vertex_samplers, uint32_t vertex_uniforms,
	const char *fragment_name, uint32_t fragment_uniforms, const char *name)
{
	gpu_pipeline_info_t info;
	SDL_GPUGraphicsPipeline *pipeline = NULL;

	SDL_zero(info);
	info.vertex_shader = gpu_load_shader(vertex_name, vertex_samplers, 0, vertex_uniforms);
	info.fragment_shader = gpu_load_shader(fragment_name, 0, 0, fragment_uniforms);
	info.cull = SDL_GPU_CULLMODE_NONE;
	info.depth_test = true;
	info.depth_write = false;
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

bool rain_create(rain_t *rain)
{
	SDL_zerop(rain);
	rain->rainbow_strength = 1.0f;
	rain->drop_pipeline = make_pipeline("rain.vert", 2, 2, "rain.frag", 1, "rain drops");
	rain->splash_pipeline = make_pipeline("rain_splash.vert", 2, 2, "rain_splash.frag", 1, "rain splashes");
	rain->rainbow_pipeline = make_pipeline("rainbow.vert", 0, 1, "rainbow.frag", 2, "rainbow");
	rain->snow_pipeline = make_pipeline("snow.vert", 2, 2, "snow.frag", 1, "snowflakes");
	return rain->drop_pipeline != NULL && rain->splash_pipeline != NULL && rain->rainbow_pipeline != NULL && rain->snow_pipeline != NULL;
}

void rain_destroy(rain_t *rain)
{
	SDL_GPUGraphicsPipeline *pipelines[4] = { rain->drop_pipeline, rain->splash_pipeline, rain->rainbow_pipeline, rain->snow_pipeline };

	for (int i = 0; i < 4; i++)
	{
		if (pipelines[i] != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, pipelines[i]);
		}
	}
	SDL_zerop(rain);
}

void rainbow_set(rain_t *rain, bool on, float strength)
{
	rain->rainbow_on = on;
	rain->rainbow_strength = SDL_max(strength, 0.0f);
}

static float ease_towards(float value, float target, float step)
{
	return value < target ? SDL_min(value + step, target) : SDL_max(value - step, target);
}

void rain_update(rain_t *rain, float seconds)
{
	seconds = SDL_max(seconds, 0.0f);
	rain->shown = ease_towards(rain->shown, rain->raining ? 1.0f : 0.0f, seconds / RAIN_EASE_SECONDS);
	rain->rainbow_shown = ease_towards(rain->rainbow_shown, rain->rainbow_on ? 1.0f : 0.0f, seconds / RAINBOW_EASE_SECONDS);
	// Wet as hard as it rains; dry once it stops
	if (rain->shown > rain->wetness)
	{
		rain->wetness = SDL_min(rain->wetness + seconds / RAIN_WET_SECONDS, rain->shown);
	}
	else
	{
		rain->wetness = SDL_max(rain->wetness - seconds / RAIN_DRY_SECONDS, 0.0f);
	}
}

float rain_intensity(const rain_t *rain, const render_settings_t *settings)
{
	return rain->shown * SDL_clamp(settings->rain_intensity, 0.0f, 1.0f);
}

float rainbow_intensity(const rain_t *rain, const render_settings_t *settings, float sun_elevation_degrees)
{
	float sun = SDL_clamp((sun_elevation_degrees - RAINBOW_SUN_FROM) / (RAINBOW_SUN_FULL - RAINBOW_SUN_FROM), 0.0f, 1.0f);

	return rain->rainbow_shown * rain->rainbow_strength * SDL_max(settings->rainbow_strength, 0.0f) * sun;
}

void rain_dim_light(const rain_t *rain, const render_settings_t *settings, render_settings_t *lit)
{
	float amount = rain_intensity(rain, settings);
	float gray;

	if (amount <= 0.0f)
	{
		return;
	}
	lit->sun_intensity *= 1.0f - RAIN_SUN_DIMMING * amount;
	lit->ambient_intensity *= 1.0f - RAIN_AMBIENT_DIMMING * amount;
	lit->fog_density *= 1.0f + RAIN_HAZE_THICKENING * amount;
	// The sky goes gray (towards its own brightness, a little darker)
	gray = 0.85f * (0.3f * lit->sky_color[0] + 0.6f * lit->sky_color[1] + 0.1f * lit->sky_color[2]);
	for (int i = 0; i < 3; i++)
	{
		lit->sky_color[i] += (gray - lit->sky_color[i]) * RAIN_SKY_GRAYING * amount;
	}
	gray = 0.85f * (0.3f * lit->sky_zenith_color[0] + 0.6f * lit->sky_zenith_color[1] + 0.1f * lit->sky_zenith_color[2]);
	for (int i = 0; i < 3; i++)
	{
		lit->sky_zenith_color[i] += (gray - lit->sky_zenith_color[i]) * RAIN_SKY_GRAYING * amount;
	}
}

void rain_draw(rain_t *rain, SDL_GPURenderPass *pass, const camera_t *camera, const render_settings_t *settings,
	const rain_world_t *world, float sun_elevation_degrees)
{
	float intensity = rain_intensity(rain, settings);
	float rainbow = rainbow_intensity(rain, settings, sun_elevation_degrees);

	if (pass == NULL)
	{
		return;
	}
	float snow = SDL_clamp(world->snow, 0.0f, 1.0f);

	// (snow makes none: it takes raindrops)
	rainbow *= 1.0f - snow;
	if (rainbow > 0.0f && rain->rainbow_pipeline != NULL)
	{
		float uniforms[4] = { rainbow, 0.0f, 0.0f, 0.0f };

		SDL_BindGPUGraphicsPipeline(pass, rain->rainbow_pipeline);
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, uniforms, sizeof(uniforms));
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	}
	if (intensity > 0.0f && world->height_texture != NULL && world->water_texture != NULL)
	{
		rain_uniforms_t uniforms;
		SDL_GPUTextureSamplerBinding samplers[2] =
		{
			{ world->height_texture, gpu.nearest_clamp },
			{ world->water_texture, gpu.nearest_clamp },
		};
		// As much of it rain as is not snow (sleet between)
		uint32_t drops = (uint32_t)((float)RAIN_MAXIMUM_DROPS * intensity * (1.0f - snow));
		uint32_t splashes = (uint32_t)((float)RAIN_MAXIMUM_SPLASHES * intensity * (1.0f - snow) * SDL_clamp(settings->rain_splashes, 0.0f, 1.0f));
		uint32_t flakes = (uint32_t)((float)SNOW_MAXIMUM_FLAKES * intensity * snow);

		SDL_zero(uniforms);
		uniforms.area[0] = camera->eye_position.x;
		uniforms.area[1] = camera->eye_position.y;
		uniforms.area[2] = camera->eye_position.z;
		uniforms.area[3] = RAIN_BOX_RADIUS;
		uniforms.fall[0] = world->wind[0];
		uniforms.fall[1] = world->wind[1];
		uniforms.fall[2] = RAIN_FALL_SPEED;
		// Wrapped where every drop's cycle (and every splash's) starts over
		uniforms.fall[3] = (float)SDL_fmod((double)world->seconds, 3600.0);
		uniforms.look[0] = SDL_clamp(settings->rain_streak_meters, 0.05f, 2.0f);
		uniforms.look[1] = intensity;
		uniforms.look[2] = RAIN_BOX_HEIGHT;
		uniforms.look[3] = 2.0f / (SDL_max(camera->projection.m[5], 0.01f) * SDL_max(world->screen_height, 1.0f));
		uniforms.map[0] = (float)world->map_size;
		uniforms.map[1] = world->cell_size_meters;
		uniforms.map[2] = RAIN_SPLASH_RADIUS;
		uniforms.map[3] = RAIN_SPLASH_SECONDS;
		if (splashes > 0 && rain->splash_pipeline != NULL)
		{
			SDL_BindGPUGraphicsPipeline(pass, rain->splash_pipeline);
			SDL_BindGPUVertexSamplers(pass, 0, samplers, 2);
			SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
			SDL_DrawGPUPrimitives(pass, 6, splashes, 0, 0);
		}
		if (drops > 0 && rain->drop_pipeline != NULL)
		{
			SDL_BindGPUGraphicsPipeline(pass, rain->drop_pipeline);
			SDL_BindGPUVertexSamplers(pass, 0, samplers, 2);
			SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
			SDL_DrawGPUPrimitives(pass, 6, drops, 0, 0);
		}
		if (flakes > 0 && rain->snow_pipeline != NULL)
		{
			rain_uniforms_t snow_uniforms = uniforms;

			snow_uniforms.area[3] = SNOW_BOX_RADIUS;
			snow_uniforms.fall[0] = world->wind[0] * SNOW_WIND_DRIFT;
			snow_uniforms.fall[1] = world->wind[1] * SNOW_WIND_DRIFT;
			snow_uniforms.fall[2] = SNOW_FALL_SPEED;
			snow_uniforms.look[0] = SNOW_FLAKE_METERS;
			SDL_BindGPUGraphicsPipeline(pass, rain->snow_pipeline);
			SDL_BindGPUVertexSamplers(pass, 0, samplers, 2);
			SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &snow_uniforms, sizeof(snow_uniforms));
			SDL_DrawGPUPrimitives(pass, 6, flakes, 0, 0);
		}
	}
}
