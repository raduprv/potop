// The ground and the water on it, for the objects' shaders (bound by
// scene_ground_bind at GROUND_BINDING and the next four): how far a point is
// under the water's surface, and what that does to it: wet under it, the
// sun played over it (water_light), the moss on the rocks (prop_body.glsl).
// Only the water there now: nothing is remembered (the flood's mud mark,
// the drying, the algae's memory and the bright line at the surface were
// taken out in 39s: they looked bad and cost every pixel of every thing).
//
// Wants underwater.glsl (underwater_caustics) and the scene block before it.

layout(set = TEXTURE_SET, binding = GROUND_BINDING) uniform sampler2D ground_height_texture;       // R32F: the ground (m)
layout(set = TEXTURE_SET, binding = GROUND_BINDING + 1) uniform sampler2D ground_water_texture;    // RG32F: the water's depth (m)
layout(set = TEXTURE_SET, binding = GROUND_BINDING + 2) uniform sampler2D sky_occlusion_texture;   // R8 a cell: the sky it sees (sky_occlusion.h)
layout(set = TEXTURE_SET, binding = GROUND_BINDING + 3) uniform sampler2D sky_layers_texture;      // RGBA8: the four layers in the air (sky_occlusion.glsl)
layout(set = TEXTURE_SET, binding = GROUND_BINDING + 4) uniform sampler2D sky_bounce_texture;      // RGBA8: what hides the sky, which way, how far (sky_occlusion.glsl)
#define SKY_BOUNCE
#include "sky_occlusion.glsl"

// objects_cull.comp's: added to an object's fade when there is no water
// anywhere near it (objects.c: its grid cell, by the water's tiles): its
// pixels skip water_at's reads (the same for all of them: no cost to the
// branch)
const float DRY_FADE = 4096.0;

// What a surface takes (apply_water_wet): a solid thing (a rock, a house:
// all of it), bark (less), foliage (cut out cards: nothing)
const int WATER_SOLID = 0;
const int WATER_BARK = 1;
const int WATER_FOLIAGE = 2;

struct water_at_t
{
	float under;                   // meters under the water's surface (0: above it, or none)
	float to_surface;              // meters to the surface, up (below 0: above it; very large: no water)
};

// How far a point is under the water (0 above it, or where there is none)
float under_water_depth(vec3 position)
{
	if (ground_map.z < 0.5)
	{
		return 0.0;
	}
	ivec2 cell = clamp(ivec2(floor(position.xz / ground_map.y + 0.5)), ivec2(0), ivec2(int(ground_map.x) - 1));
	float depth = texelFetch(ground_water_texture, cell, 0).x;

	if (depth < 0.005)
	{
		return 0.0;
	}
	return max(texelFetch(ground_height_texture, cell, 0).x + depth - position.y, 0.0);
}

water_at_t water_at(vec3 position, bool dry)
{
	water_at_t water;

	water.under = 0.0;
	water.to_surface = 1e6;
	if (ground_map.z < 0.5 || dry)
	{
		return water;
	}
	// The surface between the 4 cells round it (of those under water: a
	// cell's own steps would break the line where it meets things)
	vec2 cell_position = position.xz / ground_map.y;
	ivec2 corner = ivec2(floor(cell_position));
	vec2 fraction = cell_position - vec2(corner);
	ivec2 last = ivec2(int(ground_map.x) - 1);
	float surface = 0.0;
	float weight = 0.0;

	for (int i = 0; i < 4; i++)
	{
		ivec2 offset = ivec2(i & 1, i >> 1);
		ivec2 neighbor = clamp(corner + offset, ivec2(0), last);
		float neighbor_depth = texelFetch(ground_water_texture, neighbor, 0).x;
		float share = (offset.x == 1 ? fraction.x : 1.0 - fraction.x) * (offset.y == 1 ? fraction.y : 1.0 - fraction.y);

		if (neighbor_depth >= 0.005)
		{
			surface += share * (texelFetch(ground_height_texture, neighbor, 0).x + neighbor_depth);
			weight += share;
		}
	}
	if (weight > 0.001)
	{
		water.to_surface = surface / weight - position.y;
		water.under = max(water.to_surface, 0.0);
	}
	return water;
}

// Wet under the water (not on foliage; bark less)
void apply_water_wet(water_at_t water, int kind, inout float wet)
{
	if (water.under > 0.0 && kind != WATER_FOLIAGE)
	{
		wet = max(wet, kind == WATER_BARK ? 0.6 : 1.0);
	}
}

// Light added after the lighting: the sun played over what is under water
// (its caustics, fading with the depth). sun_light: the sun reaching it
// (its shadows)
vec3 water_light(water_at_t water, vec3 position, vec3 sun_light)
{
	if (water.under <= 0.0)
	{
		return vec3(0.0);
	}
	return sun_light * underwater_caustics(position.xz) * 0.5 * exp(-water.under * 0.8) * smoothstep(0.0, 0.03, water.under);
}
