#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Shadow map pass for actors: the skinned mesh seen from the sun (drawn
// with object_shadow.frag).

#include "shadow_pass.glsl"

layout(location = 0) in vec3 model_position;
layout(location = 2) in vec2 model_uv;
#include "actor_skinning.glsl"

layout(location = 0) out vec2 texture_uv;

void main()
{
	vec4 row_0;
	vec4 row_1;
	vec4 row_2;

	skinning_rows(row_0, row_1, row_2);
	vec3 posed = skin_point(row_0, row_1, row_2, model_position);

	texture_uv = model_uv;
	gl_Position = light_view_projection * vec4(actor_placement.xyz + rotate(actor_rotation, posed) * actor_placement.w, 1.0);
}
