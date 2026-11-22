#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Bakes one picture of an impostor (see impostor_bake.vert), in one of two
// passes: the color (sRGB, as the model's texture has it, darkened where a
// plant's crown is deep; alpha: covered), or the normal (in the model's
// axes, * 0.5 + 0.5) and the depth (alpha).

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D color_texture;

layout(std140, set = UNIFORM_SET, binding = 0) uniform bake_material_block
{
	int alpha_tested;
	int lighting_normals;
	int mode;                      // 0: the color, 1: the normal and depth
	int plant;                     // a plant's foliage: its cards tinted as plant.frag tints them
	vec4 foliage;                  // x: its lighter tips (plant.frag's parameters.y)
	vec4 autumn_color_a;           // its autumn's colors (plant.frag's)
	vec4 autumn_color_b;
};

layout(location = 0) in vec2 texture_uv;
layout(location = 1) in vec3 normal;
layout(location = 2) in float tangent_size;
layout(location = 3) in vec4 card;                        // impostor_bake.glsl's
layout(location = 4) in float turned;

layout(location = 0) out vec4 fragment_color;

void main()
{
	vec4 color = texture(color_texture, texture_uv);
	vec3 facing_normal = normalize(normal);

	if (alpha_tested != 0)
	{
		// As object.frag: the alpha boosted with the mip level
		vec2 texel_position = texture_uv * vec2(textureSize(color_texture, 0));
		vec2 texel_ddx = dFdx(texel_position);
		vec2 texel_ddy = dFdy(texel_position);
		float mip_level = max(0.0, 0.5 * log2(max(dot(texel_ddx, texel_ddx), dot(texel_ddy, texel_ddy))));

		if (color.a * (1.0 + 0.25 * mip_level) < 0.5)
		{
			discard;
		}
		if (!gl_FrontFacing && lighting_normals == 0)
		{
			facing_normal = -facing_normal;
		}
	}
	if (mode == 0)
	{
		// The crown's depth shading (see object.frag), into the color: the
		// model's takes half of it off the sun, all of it off the sky's
		// light; the picture's is lit by both, the sun's most
		float shade = alpha_tested != 0 && tangent_size > 1e-3 ? min(tangent_size, 1.5) : 1.0;
		vec3 linear_color = pow(color.rgb, vec3(2.2)) * (0.375 + 0.625 * shade);

		// A plant's cards each their own shade, their tips lighter and
		// yellower (plant.frag's)
		if (plant != 0 && alpha_tested != 0)
		{
			float tip = smoothstep(0.5, 0.97, card.x) * foliage.x;
			vec3 young = vec3(0.16, 0.34, 0.035) / 0.28 * dot(linear_color, vec3(0.2126, 0.7152, 0.0722)) * 1.25;

			linear_color *= 0.88 + 0.24 * card.z;
			linear_color = mix(linear_color, young, clamp(tip * 0.6, 0.0, 0.5));
			// Its autumn color, keeping the picture's light and dark
			if (turned > 0.0)
			{
				vec3 autumn = mix(autumn_color_a.rgb, autumn_color_b.rgb, fract(card.z * 13.7));
				float lightness = dot(linear_color, vec3(0.2126, 0.7152, 0.0722));
				vec3 recolored = autumn * (lightness / max(dot(autumn, vec3(0.2126, 0.7152, 0.0722)), 0.01));

				recolored = mix(recolored, vec3(dot(recolored, vec3(0.2126, 0.7152, 0.0722))), 0.2);
				linear_color = mix(linear_color, recolored, clamp(turned, 0.0, 1.0));
			}
		}
		fragment_color = vec4(clamp(pow(linear_color, vec3(1.0 / 2.2)), 0.0, 1.0), 1.0);
	}
	else
	{
		fragment_color = vec4(facing_normal * 0.5 + 0.5, gl_FragCoord.z);
	}
}
