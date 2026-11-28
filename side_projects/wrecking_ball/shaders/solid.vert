#version 450
#extension GL_GOOGLE_include_directive : require
// A solid, lit: see solid.frag

#include "instance.glsl"

layout(location = 0) out vec3 out_world_position;
layout(location = 1) out vec3 out_world_normal;
layout(location = 2) out vec4 out_color;
layout(location = 3) out vec4 out_light_position;

void main()
{
	vec3 world_position = instance_world_position();
	vec3 world_normal = instance_world_normal();

	out_world_position = world_position;
	out_world_normal = world_normal;
	out_color = in_instance_color;
	// (pushed out along the normal a little: no shadow acne on the faces
	// turned towards the sun)
	out_light_position = light_view_projection * vec4(world_position + world_normal * 0.03, 1.0);
	gl_Position = view_projection * vec4(world_position, 1.0);
}
