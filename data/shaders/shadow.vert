#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Shadow map pass: the terrain seen from the sun. Same chunk mesh and
// height texture as terrain.vert, only the position is needed.

#include "shadow_pass.glsl"

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D height_texture;   // R32F, meters

// A run of chunks, one instance each (shadow_map.c's draw_chunk_run): the
// run's first chunk (row after row), the map's chunks per side, a chunk's
// cells, the map's size, its cell
layout(std140, set = UNIFORM_SET, binding = 1) uniform chunk_block
{
	int first_chunk;
	int chunks_per_side;
	int chunk_cells;
	int map_size;
	float cell_size;
};

layout(location = 0) in vec2 local_position;

void main()
{
	int chunk = first_chunk + gl_InstanceIndex;
	ivec2 chunk_origin = ivec2(chunk % chunks_per_side, chunk / chunks_per_side) * chunk_cells;
	ivec2 cell = min(chunk_origin + ivec2(local_position), ivec2(map_size - 1));
	float height = texelFetch(height_texture, cell, 0).r;

	gl_Position = light_view_projection * vec4(float(cell.x) * cell_size, height, float(cell.y) * cell_size, 1.0);
}
