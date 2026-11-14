#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "rain.glsl"

// The snow's flakes (see src/rain.h): the rain's box around the eye
// (rain.vert), its drops' places from their numbers and the clock, but
// falling slowly, drifting with the wind and swaying from side to side as
// they go; a small soft disc each, facing the eye. One under the ground or
// the water is not drawn; those at the box's edges fade out.

layout(location = 0) out float flake_alpha;
layout(location = 1) out vec2 corner_out;

// The quad's corners, -1 .. 1
const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main()
{
	vec2 corner = corners[gl_VertexIndex % 6];
	uint number = uint(gl_InstanceIndex);
	vec3 random = rain_random3(number, 7u);
	float radius = area.w;
	float box_height = look.z;
	float clock = fall.w;
	vec3 eye = area.xyz;
	vec2 low_corner = eye.xz - radius;
	float bottom = eye.y - box_height * 0.5;
	// Each its own speed (lighter ones slower)
	float speed = fall.z * (0.7 + 0.6 * random.z);
	vec3 flake;

	flake.xz = low_corner + mod(random.xz * 2.0 * radius + fall.xy * clock - low_corner, 2.0 * radius);
	flake.y = bottom + mod(random.y * box_height - speed * clock - bottom, box_height);
	// Swaying as it falls
	{
		float phase = random.x * 37.0 + random.y * 11.0;
		float sway = 0.25 + 0.35 * random.z;

		flake.x += sin(clock * (0.9 + 0.8 * random.y) + phase) * sway;
		flake.z += cos(clock * (0.7 + 0.9 * random.x) + phase * 1.3) * sway;
	}
	if (flake.y < rain_surface(flake.xz))
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		flake_alpha = 0.0;
		corner_out = vec2(0.0);
		return;
	}
	float to_eye = length(flake - eye_position);
	vec3 view = (flake - eye_position) / max(to_eye, 1e-3);
	vec3 right = cross(view, vec3(0.0, 1.0, 0.0));

	right = dot(right, right) > 1e-6 ? normalize(right) : vec3(1.0, 0.0, 0.0);
	vec3 up = normalize(cross(right, view));
	// A flake a centimeter or two across: never smaller than a pixel and a
	// half, and fainter when drawn bigger than it is
	float size = look.x * (0.6 + 0.8 * random.x);
	float half_size = max(size * 0.5, look.w * to_eye * 0.75);
	float coverage = clamp(size * 0.5 / half_size, 0.08, 1.0);
	float fade = (1.0 - smoothstep(0.65 * radius, radius, length(flake.xz - eye.xz)))
		* smoothstep(0.0, 3.0, flake.y - bottom) * smoothstep(0.0, 3.0, bottom + box_height - flake.y)
		* smoothstep(0.3, 1.0, to_eye);

	gl_Position = world_to_clip(flake + (right * corner.x + up * corner.y) * half_size);
	flake_alpha = fade * coverage * (0.5 + 0.5 * look.y);
	corner_out = corner;
}
