// The instance inputs every solid shares (wb_render.c's instance_t) and
// placing a vertex of a unit mesh with them

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_instance_position;
layout(location = 3) in vec4 in_instance_rotation;   // a quaternion: x, y, z, w
layout(location = 4) in vec4 in_instance_scale;
layout(location = 5) in vec4 in_instance_color;

layout(set = 1, binding = 0) uniform frame_block
{
	mat4 view_projection;
	mat4 light_view_projection;
};

vec3 rotate_by_quaternion(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);
	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

vec3 instance_world_position()
{
	return in_instance_position.xyz + rotate_by_quaternion(in_instance_rotation, in_position * in_instance_scale.xyz);
}

vec3 instance_world_normal()
{
	return normalize(rotate_by_quaternion(in_instance_rotation, in_normal / in_instance_scale.xyz));
}
