// Day and night (render.day_cycle): from the hour, where the sun is and
// what it does to the light and the sky.
//
// The sun rises 90 degrees before its noon direction (sun_azimuth_degrees),
// stands at sun_elevation_degrees at noon, sets 90 degrees after, and goes
// as far under the horizon at midnight. The settings' sun color, light and
// sky colors are noon's; towards evening the sunlight turns orange and
// fades, the sky reddens at the horizon (twilight), then goes dark blue,
// the stars come out and the moon lights the land, faint and blue.
//
// The moon goes round its phases in render.moon_cycle_days (29.5, as the
// real one): it keeps the sun's path a little later each day, as far behind
// as the phase says (new: with the sun, up by day and unseen; first quarter:
// up from noon to midnight; full: all night; last quarter: midnight to noon).
// Its light goes with how much of it is lit (a quarter moon gives about a
// fifth of a full one's) and with its height; when it is down or new, the
// stars give a little. The scene has one light that casts shadows: the sun
// by day, the moon (or the starlight) by night; the swap happens when both
// are too faint to see.
// The exposure goes down at night, so night stays dark (but readable)
// instead of the automatic exposure making a gray day of it.

#ifndef DAYLIGHT_H
#define DAYLIGHT_H

#include <stdbool.h>

#include "settings.h"

typedef struct
{
	float sun_elevation_degrees;   // the sun's own (under the horizon at night)
	float sun_azimuth_degrees;     // the sun's own (the light may be the moon's)
	float twilight_glow;           // 0..1: how much the horizon glows on the sun's side (dawn, dusk)
	float twilight_brightness;     // 0..1: how bright that glow is
	float star_visibility;         // 0..1
	bool moon_lights;              // the light is not the sun's (the moon's, or the stars')
	float moon_azimuth_degrees;    // where the moon is (drawn on the sky whatever the light)
	float moon_elevation_degrees;
	float moon_phase;              // 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter
	float moon_lit;                // 0..1: how much of its disc is lit
} daylight_t;

// lit: base with the light and the sky of that hour (0..24) and the moon's
// age (days since new, 0..moon_cycle_days); base as it is with the day
// cycle off
void daylight_apply(const render_settings_t *base, double hour, double moon_age_days, render_settings_t *lit, daylight_t *info_out);

#endif
