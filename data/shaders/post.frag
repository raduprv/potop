#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The frame's picture from the scene's light (see post.h): stretched over
// the window (the render scale), and, with HDR on, the exposure, the bloom
// mixed in and the tone mapping that fits any light into what the screen
// shows; off, the light cut at 1, as the game was. Then gamma, and a speck
// of noise against banding in the sky's gradients.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D scene_texture;     // linear light
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D bloom_texture;     // the bloom's top level
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D exposure_texture;  // 1 x 1

layout(std140, set = UNIFORM_SET, binding = 0) uniform post_block
{
	int hdr;                        // 1: exposure, bloom, tone mapping
	int tone_mapper;                // 0: AgX, 1: AgX punchy, 2: ACES (fitted), 3: Reinhard
	float bloom_strength;           // 0: no bloom
	float bloom_levels;             // what the top level is the sum of
	float night_vision;             // 0..1: dim light loses its colors and turns blue
	float night_vision_luminance;   // the scene's light under which it starts
};

layout(location = 0) in vec2 screen_uv;

layout(location = 0) out vec4 fragment_color;

// AgX (Troy Sobotka's, in the polynomial fit by Benjamin Wrensch): the
// colors squeezed into a wider set first, so bright ones go towards white
// instead of turning into flat yellow or cyan. What comes out is already
// for the screen (gamma and all)
vec3 agx_contrast(vec3 x)
{
	vec3 x2 = x * x;
	vec3 x4 = x2 * x2;

	return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// punchy: Wrensch's "punchy" look (more contrast and color), else plain
vec3 agx(vec3 color, bool punchy)
{
	const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
		0.0784335999999992, 0.878468636469772, 0.0784336,
		0.0792237451477643, 0.0791661274605434, 0.879142973793104);
	const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
		-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
		-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
	const float minimum_ev = -12.47393;
	const float maximum_ev = 4.026069;
	vec3 value = clamp(log2(max(inset * color, vec3(1e-10))), minimum_ev, maximum_ev);

	value = (value - minimum_ev) / (maximum_ev - minimum_ev);
	value = agx_contrast(value);
	if (punchy)
	{
		float luma = dot(value, vec3(0.2126, 0.7152, 0.0722));

		value = pow(max(value, vec3(0.0)), vec3(1.35));
		value = luma + 1.4 * (value - luma);
	}
	return clamp(outset * value, 0.0, 1.0);
}

// Narkowicz's fit of the ACES film curve: more contrast, saturated
// highlights. Linear out
vec3 aces(vec3 color)
{
	return clamp(color * (2.51 * color + 0.03) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
}

// The plainest: by the brightness, the hue kept. Linear out
vec3 reinhard(vec3 color)
{
	float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));

	return clamp(color / (1.0 + luminance), 0.0, 1.0);
}

float hash_pixel(vec2 pixel)
{
	uvec2 lattice = uvec2(ivec2(pixel)) * uvec2(1597334673u, 3812015801u);
	uint mixed = (lattice.x ^ lattice.y) * 1597334673u;

	return float(mixed >> 8) * (1.0 / 16777216.0);
}

void main()
{
	vec3 color = max(textureLod(scene_texture, screen_uv, 0.0).rgb, vec3(0.0));
	vec3 shown;

	if (hdr != 0)
	{
		if (bloom_strength > 0.0)
		{
			vec3 bloom = textureLod(bloom_texture, screen_uv, 0.0).rgb / max(bloom_levels, 1.0);

			color = mix(color, bloom, bloom_strength);
		}
		// Seeing in the dark: under night_vision_luminance the eye's rods
		// take over from its cones, which see no color and most of the
		// blue-green: the colors fade, towards a cold blue (the Purkinje
		// shift), all the way at a tenth of it. By the scene's own light,
		// before the exposure: the picture brightened for the night is
		// still a dim one, and a fire keeps its colors
		if (night_vision > 0.0)
		{
			float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
			float dim = 1.0 - smoothstep(0.1 * night_vision_luminance, night_vision_luminance, luminance);
			// Rods: most sensitive at 507 nm; luminance kept, its color a pale blue
			float rods = dot(color, vec3(0.05, 0.6, 0.35));
			vec3 seen = rods * vec3(0.78, 0.95, 1.35);

			color = mix(color, seen, dim * night_vision);
		}
		color *= texelFetch(exposure_texture, ivec2(0), 0).r;
		if (tone_mapper == 2)
		{
			shown = pow(aces(color), vec3(1.0 / 2.2));
		}
		else if (tone_mapper == 3)
		{
			shown = pow(reinhard(color), vec3(1.0 / 2.2));
		}
		else
		{
			shown = agx(color, tone_mapper == 1);
		}
	}
	else
	{
		shown = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
	}
	// Half a step of 8 bits up or down, triangular: smooth gradients stay
	// smooth instead of breaking into bands
	shown += (hash_pixel(gl_FragCoord.xy) + hash_pixel(gl_FragCoord.xy + vec2(17.0, 59.0)) - 1.0) / 255.0;
	fragment_color = vec4(shown, 1.0);
}
