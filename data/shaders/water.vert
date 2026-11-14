#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// Water surface vertex shader. Same chunk mesh as the terrain; each vertex
// sits at ground height + water depth.
//
// Dry vertices next to water are placed at the neighboring water level, so
// the flat surface continues into the bank and the depth test cuts it
// exactly where it meets the ground: that is the shoreline. Dry vertices
// away from water sink just below the ground and disappear.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D height_texture;   // R32F, ground height in meters
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D water_texture;    // RG32F: depth (m) after the last step, after the one before
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D flow_texture;     // RGBA16F: flow velocity along X and Z (m/s), sediment concentration, busiest pipe's speed (m/s)

layout(std140, set = UNIFORM_SET, binding = 1) uniform water_block
{
	ivec2 unused_origin;            // (once the chunk's origin: now per instance, water.vert's chunk_origin)
	int map_size;
	float cell_size;
	vec4 thin_highlight;            // on (1/0), film depth, thin depth (meters), moving speed (m/s): F6
	vec3 shallow_color;             // linear
	float clarity_meters;
	vec3 deep_color;                // linear
	float opacity_per_meter;
	vec3 mud_color;                 // linear
	float foam_speed;               // m/s where foam starts
	int tile_overlay;               // 1 shows the water tile overlay
	float step_blend;               // 0: the step before, 1: the last step (the time between them)
	int water_padding_1;
	int water_padding_2;
};

layout(location = 0) in vec2 local_position;
layout(location = 1) in ivec2 chunk_origin;   // per instance: the chunk drawn, in cells

// Thinner water is a wet film on the ground: treated as dry here, so films
// left on slopes do not pull the surface of their lower neighbors up the
// hill. Must be at least the simulation's film thickness (water.film_millimeters).
const float VISIBLE_DEPTH = 0.0015;
// Water on a steep face over deeper water shallower than this share of the
// drop down to it is a trickle, drawn as a bank (see main)
const float TRICKLE_SHARE = 0.5;
// Ripples drift with the flow up to this speed (m/s); faster they would smear
const float RIPPLE_MAXIMUM_SPEED = 4.0;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 surface_normal;
layout(location = 2) out float water_depth;
layout(location = 3) out float flow_speed;
layout(location = 4) out vec2 flow_velocity;
layout(location = 5) out float sediment_concentration;
layout(location = 6) out vec4 shadow_position;
layout(location = 7) out vec4 near_shadow_position;
layout(location = 8) out float film_mark;                // F6: 1 at film vertices, 0 elsewhere
layout(location = 9) out float thin_mark;                // F6: 1 at thin water vertices
layout(location = 10) out float moving_mark;             // F6: 1 where that water moves faster than the sleep speed

float ground_at(ivec2 cell)
{
	return texelFetch(height_texture, clamp(cell, ivec2(0), ivec2(map_size - 1)), 0).r;
}

// The depth drawn: between the last two steps, by the time since the last
vec4 water_at(ivec2 cell)
{
	vec2 steps = texelFetch(water_texture, clamp(cell, ivec2(0), ivec2(map_size - 1)), 0).xy;

	return vec4(mix(steps.y, steps.x, step_blend), 0.0, 0.0, 0.0);
}

// Water level of a wet cell, or -1e9 for a dry one
float wet_level(ivec2 cell)
{
	float depth = water_at(cell).x;

	return depth > VISIBLE_DEPTH ? ground_at(cell) + depth : -1e9;
}

// The level as drawn: a wet cell's level blended with its wet neighbors'
// (the cell itself weighs as much as all four). The simulation leaves small
// one cell zigzags on advancing fronts and short choppy waves; this irons
// them out on screen without touching the simulation. -1e9 for a dry cell.
// (a neighbor whose level is far from ours, a trickle up a cliff over a
// lake or the lake under it, is another water, not a wave: left out)
const float SMOOTHED_REACH = 0.25;

float smoothed_level(ivec2 cell)
{
	float own = wet_level(cell);

	if (own < -1e8)
	{
		return own;
	}
	float sum = own * 4.0;
	float weight = 4.0;
	float neighbor;

	neighbor = wet_level(cell + ivec2(-1, 0));
	if (neighbor > -1e8 && abs(neighbor - own) < SMOOTHED_REACH) { sum += neighbor; weight += 1.0; }
	neighbor = wet_level(cell + ivec2(1, 0));
	if (neighbor > -1e8 && abs(neighbor - own) < SMOOTHED_REACH) { sum += neighbor; weight += 1.0; }
	neighbor = wet_level(cell + ivec2(0, -1));
	if (neighbor > -1e8 && abs(neighbor - own) < SMOOTHED_REACH) { sum += neighbor; weight += 1.0; }
	neighbor = wet_level(cell + ivec2(0, 1));
	if (neighbor > -1e8 && abs(neighbor - own) < SMOOTHED_REACH) { sum += neighbor; weight += 1.0; }
	return sum / weight;
}

void main()
{
	ivec2 cell = min(chunk_origin + ivec2(local_position), ivec2(map_size - 1));
	float ground = ground_at(cell);
	vec4 water = water_at(cell);
	vec4 flow = texelFetch(flow_texture, cell, 0);
	float speed = length(flow.xy);
	float level_left = smoothed_level(cell + ivec2(-1, 0));
	float level_right = smoothed_level(cell + ivec2(1, 0));
	float level_back = smoothed_level(cell + ivec2(0, -1));
	float level_front = smoothed_level(cell + ivec2(0, 1));
	float surface;
	// The water round us, all eight neighbors (a triangle joins us to the
	// diagonal ones too): the highest level standing below our ground (a
	// bank's), the highest of all, the lowest
	float bank_level = -1e9;
	float highest_level = -1e9;
	float lowest_level = 1e9;

	for (int dy = -1; dy <= 1; dy++)
	{
		for (int dx = -1; dx <= 1; dx++)
		{
			if (dx == 0 && dy == 0)
			{
				continue;
			}
			float level = smoothed_level(cell + ivec2(dx, dy));

			if (level < -1e8)
			{
				continue;
			}
			highest_level = max(highest_level, level);
			lowest_level = min(lowest_level, level);
			if (level <= ground)
			{
				bank_level = max(bank_level, level);
			}
		}
	}

	// A trickle on a steep face over deeper water (its water shallow next to
	// the drop down to the neighbor's): the triangles between them would lay
	// a sheet of water up the face, as deep at its foot as the pool. Drawn
	// as a bank instead, at the pool's level: its surface cut where it meets
	// the face
	float drop = ground - lowest_level;

	if (water.x > VISIBLE_DEPTH && drop > SMOOTHED_REACH && water.x < drop * TRICKLE_SHARE)
	{
		surface = min(bank_level, ground - 0.05);
	}
	else if (water.x > VISIBLE_DEPTH)
	{
		surface = smoothed_level(cell);
	}
	else if (highest_level < -1e8)
	{
		// No water round: just under the ground, out of sight
		surface = ground - 0.05;
	}
	else if (bank_level > -1e8)
	{
		// Next to water whose level is below our ground (a bank), the flat
		// surface continues into the bank and the depth test cuts it at the
		// shoreline; so too where water stands above us on another side (a
		// trickle down the cliff over a lake). On the ground, or a little
		// under it, the vertex would lay a sheet of the lake up the cliff:
		// the lake's foot meters deep, the cliff's top 5 cm under, the
		// triangle between crossing the ground only near the top
		surface = bank_level;
	}
	else
	{
		// Only water standing above our ground (a front running down a
		// slope): its flat surface would hang in the air like a row of
		// teeth; the vertex sits on the ground with no depth, and the front
		// fades out along a thin ramp instead
		surface = ground;
	}

	// Slope of the water surface; dry neighbors count as level with us so
	// the shoreline does not get steep fake normals
	float left = level_left > -1e8 ? level_left : surface;
	float right = level_right > -1e8 ? level_right : surface;
	float back = level_back > -1e8 ? level_back : surface;
	float front = level_front > -1e8 ? level_front : surface;

	surface_normal = normalize(vec3(left - right, 2.0 * cell_size, back - front));
	water_depth = max(surface - ground, 0.0);
	flow_speed = speed;
	flow_velocity = speed > RIPPLE_MAXIMUM_SPEED ? flow.xy * (RIPPLE_MAXIMUM_SPEED / speed) : flow.xy;
	sediment_concentration = flow.z;
	world_position = vec3(float(cell.x) * cell_size, surface, float(cell.y) * cell_size);
	shadow_position = light_view_projection * vec4(world_position + vec3(0.0, 0.3, 0.0), 1.0);
	near_shadow_position = near_light_view_projection * vec4(world_position + vec3(0.0, 0.3, 0.0), 1.0);
	film_mark = 0.0;
	thin_mark = 0.0;
	moving_mark = flow.w > thin_highlight.w ? 1.0 : 0.0;
	if (thin_highlight.x > 0.5 && water.x > 0.0 && water.x <= thin_highlight.z)
	{
		// Debugging: every film and trickle is drawn, a little above the
		// ground so the terrain never hides it
		film_mark = water.x <= thin_highlight.y ? 1.0 : 0.0;
		thin_mark = 1.0 - film_mark;
		world_position.y = ground + water.x + 0.03;
	}
	gl_Position = world_to_clip(world_position);
}
