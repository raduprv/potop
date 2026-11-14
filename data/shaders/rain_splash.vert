#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "rain.glsl"

// Where the rain lands (see src/rain.h), each splash somewhere new every
// time it comes round (its number and how many times it came round,
// hashed), each at its own moment. Their places are in the world, in a
// square around the eye that wraps as it moves (as the drops' box): a
// splash stays where it is while the eye walks past it. On water, a ring
// spreading flat on the surface; on the ground, a little crown of drops
// thrown up (rain_splash.frag), on a quad standing up and facing the eye.

layout(location = 0) out vec2 local;
layout(location = 1) out float age;
layout(location = 2) out float splash_alpha;
layout(location = 3) out float on_water;
layout(location = 4) out float seed;

const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

// The crown's quad: this wide (half) and this high (meters)
const float CROWN_HALF_WIDTH = 0.08;
const float CROWN_HEIGHT = 0.08;

void main()
{
	vec2 corner = corners[gl_VertexIndex % 6];
	uint number = uint(gl_InstanceIndex);
	float life = map.w;
	float radius = map.z;
	float when = fall.w / life + rain_random3(number, 7u).x;
	uint round_number = uint(floor(when));
	vec3 random = rain_random3(number * 7919u + round_number * 65537u, 11u);
	vec2 low_corner = area.xz - radius;
	vec2 place = low_corner + mod(random.xz * 2.0 * radius - low_corner, 2.0 * radius);

	age = fract(when);
	local = corner;
	seed = random.y;
	if (!rain_on_map(place))
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		splash_alpha = 0.0;
		on_water = 0.0;
		return;
	}
	int size = int(map.x);
	ivec2 cell = clamp(ivec2(floor(place / map.y + 0.5)), ivec2(0), ivec2(size - 1));
	float depth = texelFetch(water_texture, cell, 0).x;
	float surface = texelFetch(height_texture, cell, 0).r + (depth > 0.002 ? depth : 0.0);
	vec3 position;

	on_water = depth > 0.01 ? 1.0 : 0.0;
	if (on_water > 0.5)
	{
		float ring_size = mix(0.02, 0.11, sqrt(age)) * (0.7 + 0.6 * random.y);

		position = vec3(place.x, surface + 0.01, place.y) + vec3(corner.x, 0.0, corner.y) * ring_size;
	}
	else
	{
		vec3 base = vec3(place.x, surface, place.y);
		vec3 towards_eye = eye_position - base;
		vec3 right = normalize(vec3(towards_eye.z, 0.0, -towards_eye.x) + vec3(1e-4, 0.0, 0.0));

		// Local y from 0 (the ground) to 1 (the top)
		local.y = corner.y * 0.5 + 0.5;
		position = base + right * corner.x * CROWN_HALF_WIDTH + vec3(0.0, local.y * CROWN_HEIGHT - 0.01, 0.0);
	}
	splash_alpha = (1.0 - smoothstep(0.6 * radius, radius, length(place - area.xz))) * (0.5 + 0.5 * look.y);
	gl_Position = world_to_clip(position);
}
