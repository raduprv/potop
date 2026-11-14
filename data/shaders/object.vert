#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// 3D object vertex shader.
//
// Model vertices are already in game axes (Y up, meters); the object is
// turned by its rotation (a quaternion: its yaw, or whatever the physics
// made of it), scaled and moved to its place on the map.
// Drawn instanced: the placement comes per object, from the instance buffer,
// together with how far the object has faded out (near its draw distance).
//
// Plants move in the wind (none of it is real: the physics and the shadows
// keep them still): the whole plant leans with it, more the higher up (with
// the square of the height, so the foot stays), the lean swelling in gusts
// that roll downwind as waves (a plant's place decides when one reaches
// it) and swinging a little on its own; the leaves (cut out materials)
// shake fast besides.

layout(location = 0) in vec3 model_position;
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 3) in vec4 instance_placement;         // x, y, z of the origin, size
layout(location = 4) in vec4 instance_rotation;          // quaternion (x, y, z, w)
layout(location = 5) in float instance_fade;             // 1 = fully there, 0 = gone
layout(location = 6) in vec4 model_tangent;              // along the texture's U; w: the bitangent's sign
layout(location = 7) in uvec2 instance_emissive_off;     // its emissive groups put out (object_format.h, OBJECT_FILE_EMISSIVE_GROUPS)

layout(std140, set = UNIFORM_SET, binding = 1) uniform wind_block
{
	vec4 wind;                     // xy: where it blows towards (x, z); z: how hard (0..1); w: time (seconds)
	vec4 gusts;                    // x: 2 pi over the gusts' spacing (per meter); y: how fast they roll (m/s)
	vec4 plant;                    // x: its top's lean in a gale (meters at size 1); y: its leaves' shaking (meters); z: its lowest point; w: its height
	vec4 cut_out;                  // x: 1 for a cut out material (the leaves shake); y: the deepest branch level the next level of detail keeps (0: it keeps all); w: 1 for a building's details (shifted with it, the temporal anti-aliasing's)
};

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec2 texture_uv;
layout(location = 3) out vec4 shadow_position;
layout(location = 4) out vec4 near_shadow_position;
layout(location = 5) flat out float fade;
layout(location = 6) out vec4 world_tangent;
// 1 where this vertex's branch goes away at the next level of detail: a
// plant's bark carries its branch's level in the tangent's size
layout(location = 7) flat out float level_drops;
layout(location = 8) flat out uvec2 emissive_off;

vec3 rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

// How far the wind has moved a vertex at its clock (wind.w)
vec3 wind_offset(float clock)
{
	if (wind.z <= 0.0 || (plant.x <= 0.0 && plant.y <= 0.0))
	{
		return vec3(0.0);
	}
	float size = instance_placement.w;
	float height = clamp((model_position.y - plant.z) / max(plant.w, 0.01), 0.0, 1.0);
	float own_phase = fract(sin(dot(instance_placement.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
	float travel = dot(instance_placement.xz, wind.xy) * gusts.x - clock * gusts.x * gusts.y;
	float gust = clamp(0.5 + 0.35 * sin(travel) + 0.15 * sin(travel * 2.7 + 1.3), 0.0, 1.0);
	float lean = wind.z * (0.3 + 0.7 * gust) + wind.z * 0.1 * sin(clock * (1.5 + 0.5 * own_phase / 6.2831853) + own_phase);
	vec2 push = wind.xy * plant.x * size * height * height * lean;
	// Leaning, the top comes down a little (it bends, it does not stretch)
	float drop = dot(push, push) / max(2.0 * height * plant.w * size, 0.05);
	vec3 offset = vec3(push.x, -drop, push.y);

	if (cut_out.x > 0.5 && plant.y > 0.0)
	{
		float vertex_phase = dot(model_position, vec3(7.1, 3.7, 5.3)) + own_phase;
		vec3 shake = vec3(sin(clock * 11.0 + vertex_phase), 0.6 * sin(clock * 13.0 + vertex_phase * 1.3),
			cos(clock * 9.0 + vertex_phase * 0.7));

		offset += shake * plant.y * size * height * wind.z * (0.3 + 0.7 * gust);
	}
	return offset;
}

void main()
{
	vec3 turned = rotate(instance_rotation, model_position);
	vec3 turned_normal = rotate(instance_rotation, model_normal);

	world_position = instance_placement.xyz + turned * instance_placement.w;
	world_position += wind_offset(wind.w);
	world_normal = turned_normal;
	world_tangent = vec4(rotate(instance_rotation, model_tangent.xyz), model_tangent.w);
	texture_uv = model_uv;
	fade = instance_fade;
	level_drops = cut_out.y > 0.0 && cut_out.x < 0.5 && abs(model_tangent.w) > cut_out.y + 0.5 ? 1.0 : 0.0;
	// A small offset along the normal keeps lit faces from shadowing
	// themselves, as on the terrain
	shadow_position = light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	near_shadow_position = near_light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	gl_Position = world_to_clip(world_position);
	// A building's details shifted as its walls are (prop_vertex.glsl)
	if (cut_out.w > 0.5)
	{
		gl_Position.xy += temporal_jitter.xy * gl_Position.w;
	}
	emissive_off = instance_emissive_off;
}
