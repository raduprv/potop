#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "shadows.glsl"

// Contact shadows (src/contact_shadows.h): each pixel near the eye marches
// a short way toward the sun through the scene's depth; where something
// stands in the way, what the sun gives it (the shadow maps' leaving it
// lit, by its facing, against the sky's light) is taken away. Multiplied
// over the scene's color (the blend: the color times this).

layout(set = TEXTURE_SET, binding = 2) uniform sampler2D depth_texture;

layout(std140, set = UNIFORM_SET, binding = 1) uniform contact_block
{
	vec4 contact_parameters;       // x: how far it marches (m), y: how far out it fades away (m), z: - (was the frame), w: strength
};

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;

const int CONTACT_STEPS = 12;
const float CONTACT_CONE = 0.06;    // the sun taken as wide as this (radians round its middle): soft edges

// A point of the scene at a pixel (its depth there), in the world
vec3 scene_point(ivec2 pixel, ivec2 size)
{
	pixel = clamp(pixel, ivec2(0), size - 1);
	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec2 ndc = vec2((float(pixel.x) + 0.5) / float(size.x) * 2.0 - 1.0, 1.0 - (float(pixel.y) + 0.5) / float(size.y) * 2.0);
	return clip_to_eye(ndc, depth * 2.0 - 1.0) + eye_position;
}

float luminance(vec3 color)
{
	return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
	ivec2 size = textureSize(depth_texture, 0);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(depth_texture, pixel, 0).r;

	fragment_color = vec4(1.0);
	// (the sky, under water, the sun down: nothing)
	if (depth >= 1.0 || underwater_color.a > 0.5 || sun_direction.y <= 0.0 || contact_parameters.w <= 0.0)
	{
		return;
	}
	vec3 point = scene_point(pixel, size);
	float distance_eye = length(point - eye_position);
	float fade = 1.0 - smoothstep(contact_parameters.y * 0.6, contact_parameters.y, distance_eye);

	if (fade <= 0.0)
	{
		return;
	}
	// Its facing, from its neighbors' points (the nearer of each pair: no
	// normal across an edge to what is behind)
	vec3 right = scene_point(pixel + ivec2(1, 0), size) - point;
	vec3 left = point - scene_point(pixel - ivec2(1, 0), size);
	vec3 down = scene_point(pixel + ivec2(0, 1), size) - point;
	vec3 up = point - scene_point(pixel - ivec2(0, 1), size);
	vec3 across = dot(right, right) < dot(left, left) ? right : left;
	vec3 along = dot(down, down) < dot(up, up) ? down : up;
	vec3 normal = normalize(cross(along, across));
	// (on an edge both ways, its facing is not to be had: left alone)
	float pixel_meters = distance_eye * 2.0 / (abs(view_projection[1][1]) * float(size.y));

	if (min(dot(across, across), dot(along, along)) > pixel_meters * pixel_meters * 36.0)
	{
		return;
	}

	if (dot(normal, eye_position - point) < 0.0)
	{
		normal = -normal;
	}
	float facing = dot(normal, sun_direction);

	if (facing <= 0.02)
	{
		return;
	}
	// What the shadow maps leave of the sun here: none, nothing to take
	vec3 lifted = point + normal * 0.05;
	float mapped = sun_visibility(light_view_projection * vec4(lifted, 1.0), near_light_view_projection * vec4(lifted, 1.0));

	if (mapped <= 0.02)
	{
		return;
	}
	// The march: from just off the surface toward the sun, offset along it
	// a little each pixel, its way turned a little round the sun's within a
	// cone (a sun wider than it is: the shadows' edges soft, a grain)
	float reach = contact_parameters.x;
	// (the same every frame: turned with the frames it shimmered, the
	// anti-aliasing never quite gathering it, and it is off standing still;
	// fixed, a fine grain at the edges that does not move)
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

	for (int k = 0; k < CONTACT_STEPS; k++)
	{
		float t = (float(k) + noise) / float(CONTACT_STEPS);
		vec3 ray = start + toward * (reach * t * t + 0.004);
		vec4 clip = world_to_clip(ray);

		if (clip.w <= 0.0)
		{
			break;
		}
		vec2 ndc = clip.xy / clip.w;

		if (abs(ndc.x) >= 1.0 || abs(ndc.y) >= 1.0)
		{
			break;
		}
		ivec2 at = ivec2(vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * vec2(size));
		vec3 there = scene_point(at, size);
		float in_front = length(ray - eye_position) - length(there - eye_position);
		// (and it stands up off the surface here by more than the ground's
		// own bumps: the terrain's facets, a texture's relief shaded
		// themselves in blotches)
		float standing = dot(there - point, normal);
		// Something in front of the ray, not far in front (it stands in the
		// sun's way; far in front it is something else, nearer the eye; how
		// thick it is taken to be: as a stone; thicker, a thin stalk threw
		// a strip of shadow behind it along the ray, as seen from the eye).
		// Soft at each of these edges, not cut; the farther along the ray,
		// the fainter (a shadow fading away from what casts it, as the sky
		// fills it in)
		float hit = smoothstep(bias, bias * 1.5 + 0.005, in_front) * (1.0 - smoothstep(thickness * 0.8, thickness, in_front))
			* smoothstep(0.015 + pixel_meters, 0.02 + pixel_meters * 1.5, standing);

		// (as dark as the maps' own shadows near what casts it; fading only
		// toward the end of the reach)
		blocked = max(blocked, hit * (1.0 - smoothstep(0.55, 1.0, t)));
	}
	if (blocked <= 0.0)
	{
		return;
	}
	// The sun's share of the light here (against the sky's and the ground's)
	float sun = luminance(sun_color) * facing * mapped;
	float sky = luminance(sky_color) * max(ambient_intensity, 0.05) + luminance(ground_bounce_color(mapped)) * max(ambient_intensity, 0.05) * 0.3;
	float share = sun / max(sun + sky, 1e-4);

	// (as the maps' shadows take the sun away: by shadow_strength, the
	// setting over it; at 1, no lighter than them)
	fragment_color = vec4(vec3(max(1.0 - contact_parameters.w * shadow_strength * blocked * share * fade, 0.0)), 1.0);
}
