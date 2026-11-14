#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Shadow map pass: the terrain seen from the sun. Same chunk mesh and
// height texture as terrain.vert, only the position is needed.

#include "shadow_pass.glsl"

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D height_texture;   // R32F, meters

layout(std140, set = UNIFORM_SET, binding = 1) uniform chunk_block
{
	ivec2 chunk_origin;
	int map_size;
	float cell_size;
};

layout(location = 0) in vec2 local_position;

void main()
{
	ivec2 cell = min(chunk_origin + ivec2(local_position), ivec2(map_size - 1));
	float height = texelFetch(height_texture, cell, 0).r;

	gl_Position = light_view_projection * vec4(float(cell.x) * cell_size, height, float(cell.y) * cell_size, 1.0);
}
