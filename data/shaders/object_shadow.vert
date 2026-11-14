#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Shadow map pass for 3D objects: the same placement as object.vert, seen
// from the sun. The texture coordinates go along for the alpha test.

#include "shadow_pass.glsl"

layout(location = 0) in vec3 model_position;
layout(location = 2) in vec2 model_uv;
layout(location = 3) in vec4 instance_placement;         // x, y, z of the origin, size
layout(location = 4) in vec4 instance_rotation;          // quaternion (x, y, z, w)

layout(location = 0) out vec2 texture_uv;

vec3 rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

void main()
{
	texture_uv = model_uv;
	gl_Position = light_view_projection * vec4(instance_placement.xyz + rotate(instance_rotation, model_position) * instance_placement.w, 1.0);
}
