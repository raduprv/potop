#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Bloom, going down (see post.h): each level half the one above, with the
// thirteen taps of Jimenez's filter (Call of Duty, 2014): a wide, smooth
// average that does not flicker as things move by a pixel. The first level
// (from the scene) weighs its four groups by how bright they are (Karis'
// average), so a single burning pixel (the sun in a ripple) does not grow
// into a blinking disc.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D source_texture;

layout(std140, set = UNIFORM_SET, binding = 0) uniform bloom_down_block
{
	vec2 source_texel;              // 1 / the source's size
	float first;                    // 1: from the scene (Karis' average)
	float bloom_down_padding;
};

layout(location = 0) in vec2 screen_uv;

layout(location = 0) out vec4 fragment_color;

vec3 tap(vec2 offset)
{
	return textureLod(source_texture, screen_uv + offset * source_texel, 0.0).rgb;
}

float karis_weight(vec3 color)
{
	return 1.0 / (1.0 + dot(color, vec3(0.2126, 0.7152, 0.0722)));
}

void main()
{
	vec3 a = tap(vec2(-2.0, -2.0));
	vec3 b = tap(vec2(0.0, -2.0));
	vec3 c = tap(vec2(2.0, -2.0));
	vec3 d = tap(vec2(-2.0, 0.0));
	vec3 e = tap(vec2(0.0, 0.0));
	vec3 f = tap(vec2(2.0, 0.0));
	vec3 g = tap(vec2(-2.0, 2.0));
	vec3 h = tap(vec2(0.0, 2.0));
	vec3 i = tap(vec2(2.0, 2.0));
	vec3 j = tap(vec2(-1.0, -1.0));
	vec3 k = tap(vec2(1.0, -1.0));
	vec3 l = tap(vec2(-1.0, 1.0));
	vec3 m = tap(vec2(1.0, 1.0));
	// Five groups of four: the middle one counts half, the corners an
	// eighth each
	vec3 groups[5] = vec3[5]((j + k + l + m) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25, (d + e + g + h) * 0.25,
		(e + f + h + i) * 0.25);
	float weights[5] = float[5](0.5, 0.125, 0.125, 0.125, 0.125);
	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;

	for (int n = 0; n < 5; n++)
	{
		float weight = weights[n] * (first > 0.5 ? karis_weight(groups[n]) : 1.0);

		sum += groups[n] * weight;
		weight_sum += weight;
	}
	// Nothing below zero or not a number gets in (a bad pixel would spread
	// over the whole picture)
	fragment_color = vec4(clamp(sum / max(weight_sum, 1e-6), vec3(0.0), vec3(65000.0)), 1.0);
}
