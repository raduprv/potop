#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Shadow map pass for 3D objects: only depth is written; leaves and twigs
// are cut out by their texture's alpha, so their shadows have holes like
// they do. As in object.frag, the alpha is boosted with the mip level, or
// thin twigs would vanish from the shadow once the texture is minified.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D color_texture;

layout(std140, set = UNIFORM_SET, binding = 1) uniform material_block
{
	vec4 highlight_parameters;            // unused here (the same block as object.frag)
	int alpha_tested;
};

layout(location = 0) in vec2 texture_uv;

void main()
{
	if (alpha_tested != 0)
	{
		vec2 texel_position = texture_uv * vec2(textureSize(color_texture, 0));
		vec2 texel_ddx = dFdx(texel_position);
		vec2 texel_ddy = dFdy(texel_position);
		float mip_level = max(0.0, 0.5 * log2(max(dot(texel_ddx, texel_ddx), dot(texel_ddy, texel_ddy))));

		if (texture(color_texture, texture_uv).a * (1.0 + 0.25 * mip_level) < 0.5)
		{
			discard;
		}
	}
}
