#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// Actor vertex shader: the skinned mesh, placed on the map. Hands the
// fragment shader the same as object.vert, so actors are lit by
// object.frag exactly like the objects around them.

layout(location = 0) in vec3 model_position;
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 5) in vec4 model_tangent;              // along the texture's U; w: the bitangent's sign
#include "actor_skinning.glsl"

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec2 texture_uv;
layout(location = 3) out vec4 shadow_position;
layout(location = 4) out vec4 near_shadow_position;
layout(location = 5) flat out float fade;
layout(location = 6) out vec4 world_tangent;
layout(location = 7) flat out float level_drops;   // object.frag: no levels of detail here
layout(location = 8) flat out uvec2 emissive_off;   // object.frag: no emissive groups here

void main()
{
	vec4 row_0;
	vec4 row_1;
	vec4 row_2;

	skinning_rows(row_0, row_1, row_2);
	vec3 posed = skin_point(row_0, row_1, row_2, model_position);
	vec3 posed_normal = skin_direction(row_0, row_1, row_2, model_normal);
	vec3 turned_normal = rotate(actor_rotation, posed_normal);

	world_position = actor_placement.xyz + rotate(actor_rotation, posed) * actor_placement.w;
	world_normal = turned_normal;
	world_tangent = vec4(rotate(actor_rotation, skin_direction(row_0, row_1, row_2, model_tangent.xyz)), model_tangent.w);
	texture_uv = model_uv;
	fade = actor_highlight.x > 0.5 ? 3.0 : 1.0;
	level_drops = 0.0;
	emissive_off = uvec2(0u);
	shadow_position = light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	near_shadow_position = near_light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	gl_Position = world_to_clip(world_position);
}
