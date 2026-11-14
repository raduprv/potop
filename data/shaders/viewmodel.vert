#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The weapon in hand (viewmodel.h): in the camera's space, with its own
// field of view, its depth squeezed into the nearest sliver so nothing of
// the world comes in front of it. And the crosshair (flat, on the screen).

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 color;

layout(std140, set = UNIFORM_SET, binding = 0) uniform viewmodel_vertex_block
{
	mat4 model_view;               // the weapon's place in the camera's space
	mat4 projection;               // its own (depth 0..1)
	vec4 parameters;               // x: the depth's squeeze, y: 1 flat on the screen (the crosshair), z: the screen's height over its width
};

layout(location = 0) out vec3 view_normal;
layout(location = 1) out vec3 albedo;
layout(location = 2) out vec3 view_position;

void main()
{
	albedo = color;
	if (parameters.y > 0.5)
	{
		view_normal = vec3(0.0, 0.0, 1.0);
		view_position = vec3(0.0);
		gl_Position = vec4(position.x * parameters.z, position.y, 0.0, 1.0);
		return;
	}
	vec4 view = model_view * vec4(position, 1.0);

	view_position = view.xyz;
	view_normal = mat3(model_view) * normal;
	gl_Position = projection * view;
	gl_Position.z *= parameters.x;
}
