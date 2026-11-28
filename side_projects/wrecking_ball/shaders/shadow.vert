#version 450
#extension GL_GOOGLE_include_directive : require
// A solid into the sun's shadow map

#include "instance.glsl"

void main()
{
	gl_Position = light_view_projection * vec4(instance_world_position(), 1.0);
}
