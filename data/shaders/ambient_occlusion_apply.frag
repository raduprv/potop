#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "shadows.glsl"

// Ambient occlusion, step 2 (ambient_occlusion.h): at the scene's size, the
// half size occlusion (averaged over the frames: step 1b) gathered over four by four of its pixels round this
// one (step 1's fixed four by four pattern averaged away), each by how near
// its distance from the eye is to this pixel's (no blur across an edge onto
// what is behind); then only the sky's share of the light here (against
// the sun's, as the shadow maps leave it, by its facing) taken away.
// Then the bounced light (step 1's second target): what it adds is the
// pixel's own color times that light over the light it has (sun and sky,
// as worked out for the share): its albedo times the bounced light, no
// albedo needed (there is no surface buffer). Multiplied over the scene's
// color (the blend: the color times this; over 1 where light comes in).

layout(set = TEXTURE_SET, binding = 2) uniform sampler2D depth_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D occlusion_texture;
layout(set = TEXTURE_SET, binding = 4) uniform sampler2D indirect_texture;

layout(std140, set = UNIFORM_SET, binding = 1) uniform occlusion_block
{
	vec4 occlusion_parameters;     // x: radius (m), y: the distance it is gone at (m), z: strength, w: 1 if the shadow maps are bound
	vec4 occlusion_size;           // the scene's width and height, the half size target's
	vec4 occlusion_frame;          // x: the frame (0..15: the pattern's turn), y: 1 if there is a history, z: the bounced light's strength (0: none)
};

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;

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

void main()
{
	ivec2 size = ivec2(occlusion_size.xy);
	ivec2 half_size = ivec2(occlusion_size.zw);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(depth_texture, pixel, 0).r;

	fragment_color = vec4(1.0);
	if (depth >= 1.0 || underwater_color.a > 0.5 || occlusion_parameters.z <= 0.0)
	{
		return;
	}
	vec3 point = eye_point(pixel, size);
	float distance_eye = length(point);
	float fade = 1.0 - smoothstep(occlusion_parameters.y * 0.6, occlusion_parameters.y, distance_eye);

	if (fade <= 0.0)
	{
		return;
	}
	// The half size occlusion round it: four by four, this pixel's among
	// the middle four, weighted by how near each distance is to its own
	ivec2 half_pixel = pixel / 2;
	float tolerance = 0.03 * distance_eye + 0.05;
	float total = 0.0;
	vec3 total_indirect = vec3(0.0);
	float weights = 0.0;

	for (int y = -1; y <= 2; y++)
	{
		for (int x = -1; x <= 2; x++)
		{
			ivec2 at = clamp(half_pixel + ivec2(x, y), ivec2(0), half_size - 1);
			vec2 there = texelFetch(occlusion_texture, at, 0).rg;

			if (there.y < 0.0)
			{
				continue;
			}
			float weight = exp(-abs(there.y - distance_eye) / tolerance);

			total += there.x * weight;
			total_indirect += texelFetch(indirect_texture, at, 0).rgb * weight;
			weights += weight;
		}
	}
	if (weights < 1e-3)
	{
		return;
	}
	float occlusion = total / weights;
	vec3 indirect = total_indirect / weights * occlusion_frame.z;

	if (occlusion >= 0.999 && dot(indirect, indirect) < 1e-8)
	{
		return;
	}
	// The sky's share of the light here: all of it in the shade, facing
	// away from the sun, at night; less where the sun reaches it
	vec3 right = eye_point(pixel + ivec2(1, 0), size) - point;
	vec3 left = point - eye_point(pixel - ivec2(1, 0), size);
	vec3 down = eye_point(pixel + ivec2(0, 1), size) - point;
	vec3 up = point - eye_point(pixel - ivec2(0, 1), size);
	vec3 across = dot(right, right) < dot(left, left) ? right : left;
	vec3 along = dot(down, down) < dot(up, up) ? down : up;
	vec3 normal = normalize(cross(along, across));

	if (dot(normal, point) > 0.0)
	{
		normal = -normal;
	}
	float facing = max(dot(normal, sun_direction), 0.0);
	float mapped = 0.0;

	if (facing > 0.0 && sun_direction.y > 0.0)
	{
		vec3 lifted = point + eye_position + normal * 0.05;

		mapped = occlusion_parameters.w > 0.5 ? sun_visibility(light_view_projection * vec4(lifted, 1.0),
			near_light_view_projection * vec4(lifted, 1.0)) : 1.0;
	}
	float sun = luminance(sun_color) * facing * mapped * (sun_direction.y > 0.0 ? 1.0 : 0.0);
	float sky = luminance(sky_color) * max(ambient_intensity, 0.05) + luminance(ground_bounce_color(mapped)) * max(ambient_intensity, 0.05) * 0.3;
	float share = sky / max(sun + sky, 1e-4);
	float taken = (1.0 - occlusion) * occlusion_parameters.z * fade * share;

	// The light it has, by the channel (the bounced light over it: how much
	// more of its own color comes back); held: a pixel near black in the
	// dark is not made a lamp
	vec3 lit = sun_color * facing * mapped * (sun_direction.y > 0.0 ? 1.0 : 0.0)
		+ (sky_color + ground_bounce_color(mapped) * 0.3) * max(ambient_intensity, 0.05);
	vec3 gained = min(indirect * fade / max(lit, vec3(0.02)), vec3(3.0));

	fragment_color = vec4(vec3(max(1.0 - taken, 0.0)) + gained, 1.0);
}
