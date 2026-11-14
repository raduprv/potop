#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "constants.glsl"

// Terrain vertex shader.
//
// Written in the common subset of GLSL 3.30 core and GLSL ES 3.00; the
// version line and precision statements are added by the loader.
//
// The vertex buffer only holds local grid coordinates (0..64); each
// instance (one per chunk drawn, a level of detail a draw) gives the
// chunk's origin. The real
// position comes from the chunk origin and the height texture, so the same
// tiny mesh is reused for every chunk and never rebuilt.
//
// Level of detail: a chunk drawn with lod_step s uses every s-th vertex.
// Towards the distance where the next level (step 2s) takes over, the
// vertices that level drops slide down (or up) onto the coarse triangles,
// so the switch itself changes nothing on screen. The distance is measured
// from lod_eye with a fixed height (see lod_eye_height in the C code), so
// a vertex on the border of two chunks moves the same way in both.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D height_texture;   // R32F, meters

layout(std140, set = UNIFORM_SET, binding = 1) uniform chunk_block
{
	ivec2 unused_origin;            // (once the chunk's origin: now per instance, chunk_origin)
	int map_size;                   // cells per side
	float cell_size;                // meters
	vec3 lod_eye;                   // eye x, height above the ground, eye z
	int lod_step;                   // cells between this chunk's vertices
	vec2 lod_morph_range;           // morph start distance, 1 / morph length
};

layout(location = 0) in vec2 local_position;
layout(location = 1) in ivec2 chunk_origin;   // per instance: the chunk drawn, in cells

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec4 shadow_position;
layout(location = 2) out vec4 near_shadow_position;

float height_at(ivec2 cell)
{
	cell = clamp(cell, ivec2(0), ivec2(map_size - 1));
	return texelFetch(height_texture, cell, 0).r;
}

// Central differences over 'reach' cells: the normal of a height field
// h(x, z) is (-dh/dx, 1, -dh/dz), here scaled by 2 * reach * cell_size
vec3 normal_at(ivec2 cell, int reach)
{
	float left = height_at(cell + ivec2(-reach, 0));
	float right = height_at(cell + ivec2(reach, 0));
	float back = height_at(cell + ivec2(0, -reach));
	float front = height_at(cell + ivec2(0, reach));

	return vec3(left - right, 2.0 * float(reach) * cell_size, back - front);
}

void main()
{
	// The last chunk row/column reaches one cell past the map; clamping
	// collapses those quads to zero area instead of reading outside
	ivec2 local_cell = ivec2(local_position);
	ivec2 cell = min(chunk_origin + local_cell, ivec2(map_size - 1));
	float height = height_at(cell);
	vec2 horizontal = vec2(float(cell.x), float(cell.y)) * cell_size;
	float distance_to_eye = length(vec3(horizontal.x - lod_eye.x, lod_eye.y, horizontal.y - lod_eye.z));
	float morph = clamp((distance_to_eye - lod_morph_range.x) * lod_morph_range.y, 0.0, 1.0);
	int vertex_step = lod_step;

	if (morph > 0.0)
	{
		// Which vertices the coarser level drops: those at odd multiples of
		// the step. They slide onto the edge (or, for the quad centers, the
		// diagonal from corner 10 to corner 01) of the coarse triangle.
		ivec2 odd = (local_cell / vertex_step) % 2;
		ivec2 first = ivec2(0);
		ivec2 second = ivec2(0);

		if (odd.x == 1 && odd.y == 0)
		{
			first = ivec2(-vertex_step, 0);
			second = ivec2(vertex_step, 0);
		}
		else if (odd.x == 0 && odd.y == 1)
		{
			first = ivec2(0, -vertex_step);
			second = ivec2(0, vertex_step);
		}
		else if (odd.x == 1 && odd.y == 1)
		{
			first = ivec2(vertex_step, -vertex_step);
			second = ivec2(-vertex_step, vertex_step);
		}
		if (odd.x == 1 || odd.y == 1)
		{
			float coarse = 0.5 * (height_at(chunk_origin + local_cell + first) + height_at(chunk_origin + local_cell + second));

			height = mix(height, coarse, morph);
		}
	}

	world_position = vec3(horizontal.x, height, horizontal.y);
	// Looking the point up slightly off the surface, along its normal,
	// keeps lit slopes from shadowing themselves ("shadow acne"). Lighting
	// itself uses per pixel normals from a texture (see terrain.frag).
	{
		vec3 normal = normalize(normal_at(cell, vertex_step));

		shadow_position = light_view_projection * vec4(world_position + normal * (0.6 * cell_size), 1.0);
		near_shadow_position = near_light_view_projection * vec4(world_position + normal * (0.6 * cell_size), 1.0);
	}
	gl_Position = world_to_clip(world_position);
}
