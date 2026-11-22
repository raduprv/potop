#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "impostor.glsl"

// Bakes one picture of an impostor's grid (see impostor.glsl): the model,
// seen from the picture's direction with no perspective, its bounding
// sphere filling the picture's cell (the viewport). Depth: 0 at the near
// side of the sphere, 1 at the far side.

layout(location = 0) in vec3 model_position;
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 3) in vec4 model_tangent;

layout(std140, set = UNIFORM_SET, binding = 0) uniform bake_block
{
	ivec4 frame;                   // xy: the picture in the grid; z: a plant's (impostor.glsl's plant pictures)
	vec4 sphere;                   // the model's bounding sphere: center, radius (model units)
};

layout(location = 0) out vec2 texture_uv;
layout(location = 1) out vec3 normal;
layout(location = 2) out float tangent_size;

void main()
{
	vec3 direction = frame.z != 0 ? impostor_plant_direction(frame.x + 2 * frame.y, 0.0) : impostor_frame_direction(frame.xy);
	vec3 right;
	vec3 up;
	vec3 relative = model_position - sphere.xyz;

	impostor_basis(direction, right, up);
	texture_uv = model_uv;
	normal = model_normal;
	tangent_size = abs(model_tangent.w);
	gl_Position = vec4(dot(relative, right) / sphere.w, dot(relative, up) / sphere.w, 0.5 - 0.5 * dot(relative, direction) / sphere.w, 1.0);
}
