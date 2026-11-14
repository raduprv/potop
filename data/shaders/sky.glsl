// The sky (included by the sky and water shaders, after their
// variation_texture, sky_color, sun_direction, sun_color, cloud_parameters
// and cloud_motion). A dome at infinity: a color for every direction.
//
//   gradient   sky_color at the horizon (the color distant land fades
//              into, so the two meet without a seam), sky_zenith_color
//              straight up
//   sun        a small bright disc and its glow
//   clouds     the same moving sheet whose shadows cross the land (see
//              cloud_light in terrain.frag): a ray from the eye is traced
//              up to the sheet, so a cloud in the sky is the one that
//              shades the hill under it

// sky_zenith_color and sky_horizon_drop come with the scene (scene.glsl).
// SKY_FLAT_CLOUDS: how much of the flat cloud sheet the sky shows (the
// water always shows it; the sky not when the volumetric clouds are drawn)
#ifndef SKY_FLAT_CLOUDS
#define SKY_FLAT_CLOUDS 1.0
#endif
#include "aurora.glsl"
// The drop: seen from high up the land ends below eye level, the sine of
// the angle down to where the haze swallows it. The sky's horizon is
// lowered to there, so a mountain top does not see a band of bare haze
// under it.

// How much cloud there is where the ray from the eye meets the sheet (0
// with no clouds; little far away, in the haze). blur: a texture level of
// detail bias, for reflections, whose rays the ripples scatter (sharp
// clouds would sparkle there)
float sky_cloud(vec3 direction, vec3 eye, float blur)
{
	if (cloud_parameters.x <= 0.0)
	{
		return 0.0;
	}
	// The sheet is curved like the volumetric layer (CLOUD_PLANET_RADIUS):
	// towards the horizon it comes down to meet it, instead of running off
	// flat to infinity
	float eye_radius = CLOUD_PLANET_RADIUS + eye.y;
	float sheet_radius = CLOUD_PLANET_RADIUS + max(cloud_motion.z, eye.y + 1.0);
	float b = eye_radius * direction.y;
	float distance_to_sheet = -b + sqrt(max(b * b - eye_radius * eye_radius + sheet_radius * sheet_radius, 0.0));
	vec2 on_sheet = eye.xz + direction.xz * distance_to_sheet - cloud_motion.xy;
	// Far away the sheet fades into the haze, as the land does (more
	// gently: it is high), and its pattern, too fine to draw, with it
	float haze = distance_to_sheet * fog_density * 0.3;

	return cloud_cover(on_sheet, blur) * exp(-haze * haze) * smoothstep(-0.15, 0.02, direction.y);
}

// Linear color of the sky in a direction. sun_disc: 1 draws the sun's disc,
// 0 leaves it out (the water draws its own glint); cloud_blur, see sky_cloud
vec3 sky_radiance(vec3 direction, vec3 eye, float sun_disc, float cloud_blur)
{
	// Height above the (lowered) horizon, and from it a soft blend: haze
	// at and below the horizon, the deep color overhead, no edge between
	float elevation = (direction.y + sky_horizon_drop) / (1.0 + sky_horizon_drop);
	float overhead = smoothstep(-0.05, 0.75, elevation);
	vec3 color = mix(sky_color, sky_zenith_color, overhead);
	float towards_sun = max(dot(direction, sun_direction), 0.0);

	// Dawn and dusk: the low sky glows warm on the sun's side only (the
	// sun itself, there, may be under the horizon), fading round to the
	// other side and up towards the top
	if (twilight.z > 0.0)
	{
		float around = length(direction.xz);
		float facing = around > 1e-4 ? dot(direction.xz / around, twilight.xy) * 0.5 + 0.5 : 0.5;
		float low = 1.0 - smoothstep(-0.05, 0.5, elevation);
		vec3 glow = vec3(0.85, 0.36, 0.17) * (0.15 + 0.85 * twilight.w);

		color = mix(color, glow, twilight.z * pow(facing, 3.0) * low);
	}
	// The aurora, behind the clouds
	color += aurora_light(direction);
	float cloud = sky_cloud(direction, eye, cloud_blur) * SKY_FLAT_CLOUDS;

	// The glow around the sun, wide and faint, then tight and bright
	color += sun_color * (pow(towards_sun, 6.0) * 0.06 + pow(towards_sun, 64.0) * 0.25);

	// Clouds: white where the sun lights them, a silver edge towards the sun.
	// Their white is as bright as the sky around: by night (the sky dark,
	// the moon the light) they are dim, not lit as by day
	float sky_brightness = clamp(dot(sky_color, vec3(0.3, 0.59, 0.11)) / 0.6, 0.0, 1.0);
	vec3 cloud_color = mix(sky_color, vec3(sky_brightness), 0.65) * (0.55 + 0.45 * max(sun_direction.y, 0.0));
	cloud_color += sun_color * pow(towards_sun, 8.0) * 0.2;
	color = mix(color, cloud_color, cloud * 0.92);

	// The disc itself (half a degree across), hidden behind thick cloud
	float disc = smoothstep(0.99985, 0.99993, towards_sun);
	color += sun_color * disc * 6.0 * sun_disc * (1.0 - cloud * 0.9);
	return color;
}
