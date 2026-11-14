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
	int padding;
};

layout(location = 0) in vec2 texture_uv;
layout(location = 1) in vec3 normal;
layout(location = 2) in float tangent_size;

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
		// The crown's depth shading (see object.frag), into the color
		float shade = alpha_tested != 0 && tangent_size > 1e-3 ? min(tangent_size, 1.5) : 1.0;

		fragment_color = vec4(clamp(color.rgb * pow(shade, 1.0 / 2.2), 0.0, 1.0), 1.0);
	}
	else
	{
		fragment_color = vec4(facing_normal * 0.5 + 0.5, gl_FragCoord.z);
	}
}
