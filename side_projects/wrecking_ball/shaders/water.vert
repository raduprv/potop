#version 450
// The water's surface: see water.frag

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in float in_depth;

layout(set = 1, binding = 0) uniform frame_block
{
	mat4 view_projection;
	mat4 light_view_projection;
};

layout(location = 0) out vec3 out_world_position;
layout(location = 1) out vec3 out_world_normal;
layout(location = 2) out float out_depth;
layout(location = 3) out vec4 out_light_position;

void main()
{
	out_world_position = in_position;
	out_world_normal = in_normal;
	out_depth = in_depth;
	out_light_position = light_view_projection * vec4(in_position, 1.0);
	gl_Position = view_projection * vec4(in_position, 1.0);
}
