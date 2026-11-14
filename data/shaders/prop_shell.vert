#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A shell of the moss under water on the rocks (prop_shell.frag): the prop
// as prop.vert has it, lifted off its surface along the normal, one layer of
// the moss's height a draw, its tips swaying a little in the water. Only
// near the eye: farther objects are put out of view whole.

layout(location = 0) in vec3 model_position;
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 3) in vec4 instance_placement;         // x, y, z of the origin, size
layout(location = 4) in vec4 instance_rotation;          // quaternion (x, y, z, w)
layout(location = 5) in float instance_fade;             // 1 = fully there, 0 = gone
layout(location = 6) in vec4 model_tangent;              // along the texture's U; w: the bitangent's sign (triplanar: the weathering)

layout(std140, set = UNIFORM_SET, binding = 1) uniform shell_block
{
	vec4 shell;                    // x: this layer (0..1 of the height), y: the moss's height (m), z: how far from the eye shells are drawn (m), w: time (s)
};

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec2 texture_uv;
layout(location = 3) out vec4 shadow_position;
layout(location = 4) out vec4 near_shadow_position;
layout(location = 5) flat out float fade;
layout(location = 6) out vec4 world_tangent;
layout(location = 7) out vec3 model_point;               // in the model's axes, meters at its size (the surface's: the strands stand straight)
layout(location = 8) out vec3 own_normal;                // in the model's axes
layout(location = 9) out vec4 weathering;                // the vertex's (its tangent as given)
layout(location = 10) flat out vec4 turning;             // the object's rotation
layout(location = 11) flat out float shell_layer;

vec3 rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

void main()
{
	vec3 turned = rotate(instance_rotation, model_position);
	vec3 turned_normal = rotate(instance_rotation, model_normal);
	vec3 surface = instance_placement.xyz + turned * instance_placement.w;
	float layer = shell.x;
	// (the tips sway the most)
	vec3 sway = vec3(sin(shell.w * 1.3 + surface.x * 4.1 + surface.z * 2.3), 0.0, cos(shell.w * 1.1 + surface.z * 3.7 - surface.x * 1.9))
		* shell.y * 0.35 * layer * layer;

	world_position = surface + turned_normal * shell.y * layer + sway;
	world_normal = turned_normal;
	world_tangent = vec4(rotate(instance_rotation, model_tangent.xyz), model_tangent.w);
	texture_uv = model_uv;
	fade = instance_fade;
	model_point = model_position * instance_placement.w;
	own_normal = model_normal;
	weathering = model_tangent;
	turning = instance_rotation;
	shell_layer = layer;
	shadow_position = light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	near_shadow_position = near_light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	gl_Position = world_to_clip(world_position);
	// Too far: all of it out of view (nothing drawn)
	if (length(instance_placement.xyz - eye_position) > shell.z + instance_placement.w * 2.0)
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
	}
}
