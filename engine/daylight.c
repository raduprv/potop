// Day and night. See daylight.h.

#include "daylight.h"

#include <SDL3/SDL.h>

// How bright the full moon is, of the noon sun (sun_intensity); its color.
// A part lit gives less than its share (the light falls on it slanting, and
// its shadows show): the lit share to this power
#define MOON_INTENSITY 0.15f
#define MOON_PHASE_POWER 2.0f
// What the stars give when there is no moon, of the noon sun
#define STARLIGHT_INTENSITY 0.02f
static const float moon_color[3] = { 0.62f, 0.72f, 1.0f };

// The sky (sRGB, as the settings): at twilight, a dusky blue all round
// (the warm glow is only on the sun's side: sky.glsl, twilight); at night
// both go dark blue
static const float twilight_horizon[3] = { 0.52f, 0.55f, 0.64f };
static const float twilight_zenith[3] = { 0.32f, 0.40f, 0.58f };
static const float night_horizon[3] = { 0.07f, 0.09f, 0.16f };
static const float night_zenith[3] = { 0.02f, 0.03f, 0.07f };
// Low sunlight, through more air
static const float low_sun_color[3] = { 1.0f, 0.82f, 0.66f };
// The golden hour: the sun's last (first) degrees, through the most air;
// the shadows, lit by the blue sky alone, look cooler against it
static const float golden_sun_color[3] = { 1.0f, 0.58f, 0.30f };
static const float golden_shade_sky[3] = { 0.46f, 0.56f, 0.76f };
// The blue hour: the sun a few degrees under, the sky's own deep blue light
// all round (before the night's dark blue)
// (all the sky about as deep as overhead, the horizon a little lighter; the
// zenith only colors the sky, the horizon's color is also its light: dark
// there, made up for by BLUE_HOUR_LIGHT)
static const float blue_hour_horizon[3] = { 0.02f, 0.055f, 0.34f };
static const float blue_hour_zenith[3] = { 0.012f, 0.038f, 0.28f };
// How much more of the sky's light then (it is all the light there is),
// and how many stops brighter it is shown than the night's darkening would
#define BLUE_HOUR_LIGHT 8.0f
// How much more the sky's light is at night, all of it (the moonless side
// of a house came out black: what the walls, the ground and the rest round
// about send back, which nothing here works out, made up for)
#define NIGHT_AMBIENT_LIGHT 2.0f
#define BLUE_HOUR_EXPOSURE_EV 0.4f

// Dawn and dusk: an hour each, the sun from the horizon to this far under it
#define TWILIGHT_HOURS 1.0f
#define TWILIGHT_DEPTH 12.0f

// How much the low sky on the sun's side takes the warm glow at its most
#define TWILIGHT_GLOW 0.7f

// How many stops darker the night is shown than the automatic exposure
// would make it
#define NIGHT_EXPOSURE_EV -2.2f

// The light's lowest elevation: the shadow map looks along it (flatter
// would stretch it over the whole map); the light is all but gone by then
#define LOWEST_LIGHT_DEGREES 1.0f

static float smooth(float edge_low, float edge_high, float value)
{
	float t = SDL_clamp((value - edge_low) / (edge_high - edge_low), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

static void mix3(float out[3], const float a[3], const float b[3], float t)
{
	for (int i = 0; i < 3; i++)
	{
		out[i] = a[i] + (b[i] - a[i]) * t;
	}
}

// Where the sun is at an hour: its elevation, and how far it has turned
// from its noon direction (degrees). The day (daylight_hours, about noon)
// is an arc up to the noon height and down; an hour of dusk after it and
// one of dawn before take it TWILIGHT_DEPTH under the horizon; the night
// between goes as deep as the noon height, the moon as high (opposite).
static void sun_path(double hour, float daylight_hours, float noon_elevation, float *elevation_out, float *turn_out)
{
	float day_hours = SDL_clamp(daylight_hours, 2.0f, 21.0f);
	float rise = 12.0f - 0.5f * day_hours;
	float set = 12.0f + 0.5f * day_hours;
	float t = (float)SDL_fmod(hour + 24.0, 24.0);

	if (t >= rise && t <= set)
	{
		float along = (t - rise) / day_hours;

		*elevation_out = noon_elevation * SDL_sinf(along * SDL_PI_F);
		*turn_out = (along - 0.5f) * 180.0f;
	}
	else
	{
		float dark_hours = 24.0f - day_hours;
		float after_set = SDL_fmodf(t - set + 24.0f, 24.0f);

		if (after_set < TWILIGHT_HOURS)
		{
			*elevation_out = -TWILIGHT_DEPTH * after_set / TWILIGHT_HOURS;
		}
		else if (after_set > dark_hours - TWILIGHT_HOURS)
		{
			*elevation_out = -TWILIGHT_DEPTH * (dark_hours - after_set) / TWILIGHT_HOURS;
		}
		else
		{
			float along = (after_set - TWILIGHT_HOURS) / (dark_hours - 2.0f * TWILIGHT_HOURS);

			*elevation_out = -TWILIGHT_DEPTH - (SDL_max(noon_elevation, TWILIGHT_DEPTH) - TWILIGHT_DEPTH) * SDL_sinf(along * SDL_PI_F);
		}
		// Back round under the horizon, from where it set to where it rises
		*turn_out = 90.0f + after_set / dark_hours * 180.0f;
	}
}

void daylight_apply(const render_settings_t *base, double hour, double moon_age_days, render_settings_t *lit, daylight_t *info_out)
{
	float elevation;
	float turn;
	float moon_elevation;
	float moon_turn;
	float cycle = SDL_max(base->moon_cycle_days, 1.0f);
	float phase = (float)SDL_fmod(SDL_fmod(moon_age_days, (double)cycle) / (double)cycle + 1.0, 1.0);
	float moon_lit = 0.5f * (1.0f - SDL_cosf(2.0f * SDL_PI_F * phase));
	float moon_azimuth;

	sun_path(hour, base->daylight_hours, base->sun_elevation_degrees, &elevation, &turn);
	// The moon: the sun's path, as far behind as its phase
	sun_path(hour - (double)phase * 24.0, base->daylight_hours, base->sun_elevation_degrees, &moon_elevation, &moon_turn);
	moon_azimuth = SDL_fmodf(base->sun_azimuth_degrees + moon_turn + 360.0f, 360.0f);
	float sun_strength = smooth(-3.0f, 5.0f, elevation);
	// The moon fades in only once the sun is gone, and out before it comes
	float moon_strength = smooth(3.0f, 12.0f, -elevation);
	bool moon = elevation < -3.0f;
	float twilight = smooth(-14.0f, -2.0f, elevation);
	float day = smooth(-4.0f, 8.0f, elevation);
	float horizon[3];
	float zenith[3];
	float blue_amount = 0.0f;

	*lit = *base;
	if (info_out != NULL)
	{
		info_out->sun_elevation_degrees = elevation;
		info_out->sun_azimuth_degrees = SDL_fmodf(base->sun_azimuth_degrees + turn + 360.0f, 360.0f);
		info_out->star_visibility = 1.0f - smooth(-14.0f, -4.0f, elevation);
		info_out->moon_lights = moon;
		info_out->twilight_glow = smooth(-12.0f, -3.0f, elevation) * (1.0f - smooth(2.0f, 12.0f, elevation)) * TWILIGHT_GLOW;
		info_out->twilight_brightness = smooth(-12.0f, -1.0f, elevation);
		info_out->moon_azimuth_degrees = moon_azimuth;
		info_out->moon_elevation_degrees = moon_elevation;
		info_out->moon_phase = phase;
		info_out->moon_lit = moon_lit;
	}
	if (!base->day_cycle)
	{
		if (info_out != NULL)
		{
			info_out->sun_elevation_degrees = base->sun_elevation_degrees;
			info_out->sun_azimuth_degrees = base->sun_azimuth_degrees;
			info_out->twilight_glow = 0.0f;
			info_out->star_visibility = 0.0f;
			info_out->moon_lights = false;
			info_out->moon_elevation_degrees = -90.0f;
		}
		return;
	}
	// The light: the sun along its path; at night the moon, as much as it
	// is lit and as high as it is, or the stars when it gives less
	if (moon)
	{
		float moon_light = MOON_INTENSITY * SDL_powf(moon_lit, MOON_PHASE_POWER) * smooth(-1.0f, 8.0f, moon_elevation);

		if (moon_light >= STARLIGHT_INTENSITY)
		{
			lit->sun_azimuth_degrees = moon_azimuth;
			lit->sun_elevation_degrees = SDL_max(moon_elevation, LOWEST_LIGHT_DEGREES);
			lit->sun_intensity = base->sun_intensity * moon_light * moon_strength;
		}
		else
		{
			// From high up: the starlight has no direction to speak of
			lit->sun_azimuth_degrees = base->sun_azimuth_degrees;
			lit->sun_elevation_degrees = 70.0f;
			lit->sun_intensity = base->sun_intensity * STARLIGHT_INTENSITY * moon_strength;
		}
		SDL_memcpy(lit->sun_color, moon_color, sizeof(lit->sun_color));
	}
	else
	{
		lit->sun_azimuth_degrees = SDL_fmodf(base->sun_azimuth_degrees + turn + 360.0f, 360.0f);
		lit->sun_elevation_degrees = SDL_max(elevation, LOWEST_LIGHT_DEGREES);
		lit->sun_intensity = base->sun_intensity * sun_strength;
		mix3(lit->sun_color, low_sun_color, base->sun_color, smooth(0.0f, 15.0f, elevation));
	}
	// The sky: night, to twilight, to the settings' day
	mix3(horizon, night_horizon, twilight_horizon, twilight);
	mix3(zenith, night_zenith, twilight_zenith, twilight);
	// The blue hour (when it is on: not every dusk has one): from the sun
	// about 1 degree under; all the sky a deep blue (the warm glow on the
	// sun's side gone too), its light stronger, darkening straight into the
	// night (no dusky grey, nothing reddish between)
	if (base->blue_hour_on)
	{
		float blue = SDL_min((1.0f - smooth(-3.0f, 0.5f, elevation)) * SDL_clamp(base->blue_hour, 0.0f, 2.0f), 1.0f);
		float blue_horizon[3];
		float blue_zenith[3];

		mix3(blue_horizon, night_horizon, blue_hour_horizon, twilight);
		mix3(blue_zenith, night_zenith, blue_hour_zenith, twilight);
		mix3(horizon, horizon, blue_horizon, blue);
		mix3(zenith, zenith, blue_zenith, blue);
		// (its light and brightness as much as the blue hour is still there)
		blue_amount = blue * twilight;
		if (info_out != NULL)
		{
			info_out->twilight_glow *= 1.0f - blue;
		}
	}
	mix3(lit->sky_color, horizon, base->sky_color, day);
	mix3(lit->sky_zenith_color, zenith, base->sky_zenith_color, day);
	// The golden hour: the sun up to about 8 degrees, deepest at 1 to 3;
	// the sky's light (the shadows) a little cooler then
	if (!moon)
	{
		float golden = smooth(-1.5f, 1.0f, elevation) * (1.0f - smooth(3.0f, 9.0f, elevation)) * SDL_clamp(base->golden_hour, 0.0f, 2.0f);

		mix3(lit->sun_color, lit->sun_color, golden_sun_color, SDL_min(golden, 1.0f));
		mix3(lit->sky_color, lit->sky_color, golden_shade_sky, SDL_min(golden * 0.35f, 1.0f));
	}
	lit->night_amount = 1.0f - smooth(-12.0f, 0.0f, elevation);
	lit->exposure_ev = base->exposure_ev + NIGHT_EXPOSURE_EV * lit->night_amount + BLUE_HOUR_EXPOSURE_EV * blue_amount;
	lit->ambient_intensity = base->ambient_intensity * (1.0f + BLUE_HOUR_LIGHT * blue_amount + NIGHT_AMBIENT_LIGHT * lit->night_amount);
}
