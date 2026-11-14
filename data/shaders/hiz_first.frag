#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The depth pyramid's level 0 (see hiz.h): each texel the farthest depth of
// the 2 x 2 of the scene's depth it covers (and, on the last row or column
// of an odd sized scene, of the 3 there). A fragment shader, so that it
// reads the depth only once the scene's pass has written it.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D depth_texture;

layout(std140, set = UNIFORM_SET, binding = 0) uniform first_block
{
	ivec2 source_size;
	ivec2 destination_size;
};

layout(location = 0) out float farthest_out;

void main()
{
	ivec2 texel = ivec2(gl_FragCoord.xy);
	ivec2 first = texel * 2;
	ivec2 last = min(first + 1, source_size - 1);
	float farthest = 0.0;

	if (texel.x == destination_size.x - 1)
	{
		last.x = source_size.x - 1;
	}
	if (texel.y == destination_size.y - 1)
	{
		last.y = source_size.y - 1;
	}
	for (int y = first.y; y <= last.y; y++)
	{
		for (int x = first.x; x <= last.x; x++)
		{
			farthest = max(farthest, texelFetch(depth_texture, ivec2(x, y), 0).r);
		}
	}
	farthest_out = farthest;
}
