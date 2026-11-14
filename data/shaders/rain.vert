#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "rain.glsl"

// The rain's drops (see src/rain.h): a streak each, two triangles facing
// the eye, in a box around it that moves with it. A drop's place follows
// from its number and the clock alone: it falls through the box's height
// again and again, drifting with the wind, and the box wraps around the
// eye (both ways: the drops stay where they are in the world as the eye
// moves). A drop under the ground or the water is not drawn; nor are the
// drops at the box's edges, faded out so that none pops in or out.

layout(location = 0) out float drop_alpha;
layout(location = 1) out float across;
layout(location = 2) out float along;

// The quad's corners: along the streak 0 (its head) .. 1 (its tail), across -1 .. 1
const vec2 corners[6] = vec2[6](vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(0.0, -1.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void hide()
{
	gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
	drop_alpha = 0.0;
	across = 0.0;
	along = 0.0;
}

void main()
{
	vec2 corner = corners[gl_VertexIndex % 6];
	uint number = uint(gl_InstanceIndex);
	vec3 random = rain_random3(number, 1u);
	float radius = area.w;
	float box_height = look.z;
	float clock = fall.w;
	vec3 eye = area.xyz;
	vec2 low_corner = eye.xz - radius;
	float bottom = eye.y - box_height * 0.5;
	vec3 head;

	// Where it is: its start in the box, carried by the wind and its fall,
	// wrapped into the box around the eye
	head.xz = low_corner + mod(random.xz * 2.0 * radius + fall.xy * clock - low_corner, 2.0 * radius);
	head.y = bottom + mod(random.y * box_height - fall.z * clock - bottom, box_height);

	float surface = rain_surface(head.xz);

	if (head.y < surface)
	{
		hide();
		return;
	}
	vec3 direction = normalize(vec3(fall.x, -fall.z, fall.y));
	// Longer drops are a little heavier: some spread in the streaks
	float length_meters = look.x * (0.75 + 0.5 * fract(random.x * 7.31 + random.z * 3.17));
	// The tail stays above the head, never under the ground
	vec3 position = head - direction * length_meters * corner.x;
	float to_eye = length(position - eye_position);
	vec3 view = (position - eye_position) / max(to_eye, 1e-3);
	vec3 side = cross(direction, view);

	side = dot(side, side) > 1e-8 ? normalize(side) : vec3(1.0, 0.0, 0.0);
	// A raindrop is a millimeter or two wide: never thinner than a pixel,
	// and fainter when drawn wider than it is
	float half_width = max(0.001, look.w * to_eye * 0.7);
	float coverage = clamp(0.0015 / half_width, 0.06, 1.0);
	// Faded at the box's sides, top and bottom, and very near the eye
	float fade = (1.0 - smoothstep(0.65 * radius, radius, length(head.xz - eye.xz)))
		* smoothstep(0.0, 3.0, head.y - bottom) * smoothstep(0.0, 3.0, bottom + box_height - head.y)
		* smoothstep(0.4, 1.5, to_eye);

	position += side * half_width * corner.y;
	gl_Position = world_to_clip(position);
	drop_alpha = fade * coverage * (0.45 + 0.55 * look.y);
	across = corner.y;
	along = corner.x;
}
