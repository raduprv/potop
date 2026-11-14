#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// Ambient occlusion, step 1b (ambient_occlusion.h): at half size, this
// frame's occlusion averaged with its history. The point (its half size
// pixel's top left scene pixel, as step 1 took it) is found where it was on
// the screen last frame (last frame's matrix, from last frame's eye); the
// history's four pixels round there count only where their distance from
// the eye is the point's then (else an edge come out, something moved:
// this frame's alone). Out: the occlusion, this frame's distance.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D depth_texture;
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D occlusion_texture;
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D history_texture;

layout(std140, set = UNIFORM_SET, binding = 1) uniform temporal_block
{
	mat4 previous_view_projection; // last frame's view_projection_relative (from last frame's eye)
	vec4 eye_moved;                // this frame's eye minus last frame's
	vec4 temporal_size;            // the scene's width and height, the half size target's
	vec4 temporal_history;         // x: the history's weight, y: 1 if there is a history
};

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;

void main()
{
	ivec2 size = ivec2(temporal_size.xy);
	ivec2 half_size = ivec2(temporal_size.zw);
	ivec2 half_pixel = ivec2(gl_FragCoord.xy);
	vec2 current = texelFetch(occlusion_texture, half_pixel, 0).rg;

	fragment_color = vec4(current, 0.0, 0.0);
	if (current.y < 0.0 || temporal_history.y < 0.5)
	{
		return;
	}
	ivec2 pixel = min(half_pixel * 2, size - 1);
	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec2 ndc = vec2((float(pixel.x) + 0.5) / float(size.x) * 2.0 - 1.0, 1.0 - (float(pixel.y) + 0.5) / float(size.y) * 2.0);
	// From last frame's eye
	vec3 point = clip_to_eye(ndc, depth * 2.0 - 1.0) + eye_moved.xyz;
	vec4 previous = previous_view_projection * vec4(point, 1.0);

	if (previous.w <= 1e-5)
	{
		return;
	}
	vec2 previous_ndc = previous.xy / previous.w;
	// (in the half size pixels, their centers on the integers)
	vec2 place = vec2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5) * vec2(half_size) - 0.5;
	ivec2 corner = ivec2(floor(place));
	vec2 fraction = place - vec2(corner);
	float expected = length(point);
	float tolerance = 0.02 * expected + 0.05;
	float total = 0.0;
	float weights = 0.0;

	for (int y = 0; y < 2; y++)
	{
		for (int x = 0; x < 2; x++)
		{
			ivec2 at = corner + ivec2(x, y);

			if (at.x < 0 || at.y < 0 || at.x >= half_size.x || at.y >= half_size.y)
			{
				continue;
			}
			vec2 there = texelFetch(history_texture, at, 0).rg;

			if (there.y < 0.0 || abs(there.y - expected) > tolerance || isnan(there.x))
			{
				continue;
			}
			float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);

			total += there.x * weight;
			weights += weight;
		}
	}
	// (a sliver of a pixel held: too little to go by)
	if (weights < 0.05)
	{
		return;
	}
	fragment_color = vec4(mix(current.x, total / weights, temporal_history.x), current.y, 0.0, 0.0);
}
