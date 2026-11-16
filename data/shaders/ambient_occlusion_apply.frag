#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "shadows.glsl"

// Ambient occlusion, step 2 (ambient_occlusion.h), with the contact
// shadows in the same pass: at the scene's size, for each pixel, once:
// where it is, its facing (from its neighbors' depth), what the shadow maps
// leave of the sun there. Then the two things the maps and the sky's
// occlusion are too coarse for:
//
//   the sky's light: the half size occlusion (averaged over the frames:
//   step 1b) gathered over four by four of its pixels round this one, each
//   by how near its distance from the eye is to this pixel's (no blur
//   across an edge onto what is behind)
//   the sun's light: a short march toward the sun through the depth (near
//   the eye only); something standing in the way takes it
//
// and the light here as it is against what it was: (the sun's, less what
// the march found in its way) plus (the sky's, less its occlusion), over
// the sun's plus the sky's. Multiplied over the scene's color (the blend:
// the color times this).

layout(set = TEXTURE_SET, binding = 2) uniform sampler2D depth_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D occlusion_texture;

layout(std140, set = UNIFORM_SET, binding = 1) uniform occlusion_block
{
	vec4 occlusion_parameters;     // x: radius (m), y: the distance it is gone at (m), z: strength, w: 1 if the shadow maps are bound
	vec4 occlusion_size;           // the scene's width and height, the half size target's
	vec4 occlusion_frame;          // x: the frame (0..15: the pattern's turn), y: 1 if there is a history, z: the contact march's reach (m), w: the contact shadows' strength
};

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;

const int CONTACT_STEPS = 12;
const float CONTACT_DISTANCE = 40.0;   // the contact shadows gone this far from the eye
const float CONTACT_CONE = 0.06;       // the sun taken as wide as this (radians round its middle): soft edges

// Where a pixel's point is from the eye (world minus eye_position)
vec3 eye_point(ivec2 pixel, ivec2 size)
{
	pixel = clamp(pixel, ivec2(0), size - 1);
	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec2 ndc = vec2((float(pixel.x) + 0.5) / float(size.x) * 2.0 - 1.0, 1.0 - (float(pixel.y) + 0.5) / float(size.y) * 2.0);
	return clip_to_eye(ndc, depth * 2.0 - 1.0);
}

float luminance(vec3 color)
{
	return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// The half size occlusion round a pixel (1: none), four by four, the
// pixel's among the middle four, weighted by how near each distance is to
// its own (a texel with no point in it, the sky's: weightless)
float gathered_occlusion(ivec2 pixel, float distance_eye)
{
	ivec2 half_size = ivec2(occlusion_size.zw);
	ivec2 half_pixel = pixel / 2;
	float tolerance = 0.03 * distance_eye + 0.05;
	float total = 0.0;
	float weights = 1e-4;

	for (int y = -1; y <= 2; y++)
	{
		for (int x = -1; x <= 2; x++)
		{
			ivec2 at = clamp(half_pixel + ivec2(x, y), ivec2(0), half_size - 1);
			vec2 there = texelFetch(occlusion_texture, at, 0).rg;
			float weight = step(0.0, there.y) * exp(-abs(there.y - distance_eye) / tolerance);

			total += there.x * weight;
			weights += weight;
		}
	}
	// (none found: none taken; a history spoiled by a not-a-number before
	// this was fixed: none taken either)
	float occlusion = mix(1.0, total / weights, step(1e-3, weights - 1e-4));

	return isnan(occlusion) ? 1.0 : occlusion;
}

// How much of the sun something near stands in the way of (0..1): from
// just off the surface toward the sun, offset along it a little each pixel,
// its way turned a little round the sun's within a cone (a sun wider than
// it is: the shadows' edges soft, a grain). steps 0: none
float contact_blocked(ivec2 pixel, ivec2 size, vec3 point, vec3 normal, float pixel_meters, int steps)
{
	float reach = occlusion_frame.z;
	// (the same every frame: turned with the frames it shimmered, the
	// anti-aliasing never quite gathering it; fixed, a fine grain at the
	// edges that does not move)
	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float spin = fract(noise * 7.13 + 0.618034) * 6.2831853;
	float spread = sqrt(fract(noise * 3.71 + 0.3819660)) * CONTACT_CONE;
	vec3 side = normalize(cross(sun_direction, abs(sun_direction.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 lift = cross(side, sun_direction);
	vec3 toward = normalize(sun_direction + (side * cos(spin) + lift * sin(spin)) * spread);
	// (off the surface by what a pixel there spans, and more: the depth's
	// own steps, the terrain's facets, must not shade themselves)
	vec3 start = point + normal * (0.02 + pixel_meters * 3.0);
	float thickness = 0.07 + pixel_meters * 2.0;
	float bias = 0.01 + pixel_meters;
	float blocked = 0.0;

	for (int k = 0; k < steps; k++)
	{
		float t = (float(k) + noise) / float(CONTACT_STEPS);
		vec3 ray = start + toward * (reach * t * t + 0.004);
		vec4 clip = view_projection_relative * vec4(ray, 1.0);
		vec2 ndc = clip.xy / max(clip.w, 1e-6);

		// (behind the eye, off the screen: the march is over)
		if (clip.w <= 0.0 || abs(ndc.x) >= 1.0 || abs(ndc.y) >= 1.0)
		{
			break;
		}
		ivec2 at = ivec2(vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * vec2(size));
		vec3 there = eye_point(at, size);
		float in_front = length(ray) - length(there);
		// (and it stands up off the surface here by more than the ground's
		// own bumps: the terrain's facets, a texture's relief shaded
		// themselves in blotches)
		float standing = dot(there - point, normal);
		// Something in front of the ray, not far in front (it stands in the
		// sun's way; far in front it is something else, nearer the eye; how
		// thick it is taken to be: as a stone). Soft at each edge; fading
		// toward the end of the reach (the sky fills it in)
		float hit = smoothstep(bias, bias * 1.5 + 0.005, in_front) * (1.0 - smoothstep(thickness * 0.8, thickness, in_front))
			* smoothstep(0.015 + pixel_meters, 0.02 + pixel_meters * 1.5, standing);

		blocked = max(blocked, hit * (1.0 - smoothstep(0.55, 1.0, t)));
	}
	return blocked;
}

void main()
{
	ivec2 size = ivec2(occlusion_size.xy);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(depth_texture, pixel, 0).r;

	fragment_color = vec4(1.0);
	// (the sky, under water: nothing)
	if (depth >= 1.0 || underwater_color.a > 0.5)
	{
		return;
	}
	vec3 point = eye_point(pixel, size);
	float distance_eye = max(length(point), 1e-4);
	// (and in from under a meter: the grass at the eye's feet, a blade a
	// hand away, has no occlusion the screen can tell)
	float occlusion_fade = (1.0 - smoothstep(occlusion_parameters.y * 0.6, occlusion_parameters.y, distance_eye)) * smoothstep(0.4, 1.2, distance_eye);
	float contact_fade = 1.0 - smoothstep(CONTACT_DISTANCE * 0.6, CONTACT_DISTANCE, distance_eye);

	// Its facing, from its neighbors' points (the nearer of each pair: no
	// normal across an edge to what is behind)
	vec3 right = eye_point(pixel + ivec2(1, 0), size) - point;
	vec3 left = point - eye_point(pixel - ivec2(1, 0), size);
	vec3 down = eye_point(pixel + ivec2(0, 1), size) - point;
	vec3 up = point - eye_point(pixel - ivec2(0, 1), size);
	vec3 across = dot(right, right) < dot(left, left) ? right : left;
	vec3 along = dot(down, down) < dot(up, up) ? down : up;
	vec3 facing_cross = cross(along, across);
	float pixel_meters = distance_eye * 2.0 / (abs(view_projection[1][1]) * float(size.y));
	// (no facing to be had, its neighbors in a line (normalized it was not
	// a number): taken as facing the eye, and no contact shadow; on an edge
	// both ways, no contact shadow either)
	float facing_known = step(1e-20, dot(facing_cross, facing_cross))
		* step(min(dot(across, across), dot(along, along)), pixel_meters * pixel_meters * 36.0);
	vec3 normal = dot(facing_cross, facing_cross) > 1e-20 ? normalize(facing_cross) : -point / distance_eye;

	normal = dot(normal, point) > 0.0 ? -normal : normal;

	// What the shadow maps leave of the sun here (none bound: all of it)
	float sun_up = step(0.0, sun_direction.y);
	float facing = max(dot(normal, sun_direction), 0.0) * sun_up;
	vec3 lifted = point + eye_position + normal * 0.05;
	float mapped = 1.0;

	if (occlusion_parameters.w > 0.5)
	{
		mapped = sun_visibility(light_view_projection * vec4(lifted, 1.0), near_light_view_projection * vec4(lifted, 1.0));
	}
	// The light here as the scene's own shaders give it (prop_body.glsl's
	// surroundings: the ground's below, the sky's above, by the facing)
	float sun = luminance(sun_color) * facing * mapped;
	float sky = luminance(mix(ground_bounce_color(mapped), sky_color, normal.y * 0.5 + 0.5)) * max(ambient_intensity, 0.05);

	// The sky's share taken by the occlusion
	float occlusion = gathered_occlusion(pixel, distance_eye);
	float sky_taken = (1.0 - occlusion) * occlusion_parameters.z * occlusion_fade;
	// The sun's taken by what stands in its way near the eye (only where
	// the sun reaches at all, it faces it, near enough: else no march; as
	// the maps' shadows take the sun away: by shadow_strength)
	float marching = facing_known * contact_fade * step(0.02, facing) * step(0.02, mapped) * step(0.0, occlusion_frame.w - 1e-4);
	float blocked = contact_blocked(pixel, size, point, normal, pixel_meters, int(marching > 0.0) * CONTACT_STEPS);
	float sun_taken = occlusion_frame.w * shadow_strength * blocked * marching;
	float light = sun * max(1.0 - sun_taken, 0.0) + sky * max(1.0 - sky_taken, 0.0);

	fragment_color = vec4(vec3(clamp(light / max(sun + sky, 1e-4), 0.0, 1.0)), 1.0);
}
