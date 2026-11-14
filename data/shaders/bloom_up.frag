#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Bloom, going up (see post.h): this level as it came down, plus the level
// below it (the smaller one, already summed up from the bottom) blown up
// with a 3 x 3 tent. The top level then holds the picture blurred at every
// size at once.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D level_texture;  // this level, going down
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D below_texture;  // the smaller level, summed up

layout(std140, set = UNIFORM_SET, binding = 0) uniform bloom_up_block
{
	vec2 below_texel;               // 1 / the smaller level's size
	float radius;                   // the tent's size in its texels
	float bloom_up_padding;
};

layout(location = 0) in vec2 screen_uv;

layout(location = 0) out vec4 fragment_color;

void main()
{
	vec2 step_uv = below_texel * radius;
	vec3 below = textureLod(below_texture, screen_uv, 0.0).rgb * 4.0;

	below += (textureLod(below_texture, screen_uv + vec2(-step_uv.x, 0.0), 0.0).rgb
		+ textureLod(below_texture, screen_uv + vec2(step_uv.x, 0.0), 0.0).rgb
		+ textureLod(below_texture, screen_uv + vec2(0.0, -step_uv.y), 0.0).rgb
		+ textureLod(below_texture, screen_uv + vec2(0.0, step_uv.y), 0.0).rgb) * 2.0;
	below += textureLod(below_texture, screen_uv - step_uv, 0.0).rgb
		+ textureLod(below_texture, screen_uv + step_uv, 0.0).rgb
		+ textureLod(below_texture, screen_uv + vec2(-step_uv.x, step_uv.y), 0.0).rgb
		+ textureLod(below_texture, screen_uv + vec2(step_uv.x, -step_uv.y), 0.0).rgb;
	fragment_color = vec4(textureLod(level_texture, screen_uv, 0.0).rgb + below / 16.0, 1.0);
}
