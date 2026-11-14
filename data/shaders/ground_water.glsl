// The ground and the water on it, for the objects' shaders (bound by
// scene_ground_bind at GROUND_BINDING and the next five): how far a point is
// under the water's surface, and what the water left on things
// (water_marks.h): the flood's mark (mud up to where the water stood), the
// wet that dries, the algae on what stays under water. And how they look:
// apply_water_marks (the color, the wet) and water_marks_light (sunlight
// played over what is under water, the bright line where it meets the
// surface).
//
// Wants underwater.glsl (underwater_caustics) and the scene block before it.

layout(set = TEXTURE_SET, binding = GROUND_BINDING) uniform sampler2D ground_height_texture;       // R32F: the ground (m)
layout(set = TEXTURE_SET, binding = GROUND_BINDING + 1) uniform sampler2D ground_water_texture;    // RG32F: the water's depth (m)
layout(set = TEXTURE_SET, binding = GROUND_BINDING + 2) uniform sampler2D water_marks_texture;     // RGBA8, per 2 x 2 cells: water_marks.comp
layout(set = TEXTURE_SET, binding = GROUND_BINDING + 3) uniform sampler2D sky_occlusion_texture;   // R8 a cell: the sky it sees (sky_occlusion.h)
layout(set = TEXTURE_SET, binding = GROUND_BINDING + 4) uniform sampler2D sky_layers_texture;      // R8: the same in the air, four layers (sky_occlusion.glsl)

layout(set = TEXTURE_SET, binding = GROUND_BINDING + 5) uniform sampler2D sky_bounce_texture;      // RGBA8: what hides the sky, which way, how far (sky_occlusion.glsl)
#define SKY_BOUNCE
#include "sky_occlusion.glsl"

// water_marks.comp's
const float WATER_MARK_RANGE = 4.0;

// objects_cull.comp's: added to an object's fade when the marks left on its
// spot are not its (it floats, or was moved)
const float NO_MARKS_FADE = 4096.0;

// What a surface takes (apply_water_marks): a solid thing (a rock, a house:
// all of it), bark (no shine), foliage (cut out cards: only the mud and
// the sunlight under water)
const int WATER_MARKS_SOLID = 0;
const int WATER_MARKS_BARK = 1;
const int WATER_MARKS_FOLIAGE = 2;

struct water_marks_t
{
	float under;                   // meters under the water's surface (0: above it, or none)
	float to_surface;              // meters to the surface, up (below 0: above it; very large: no water)
	float mark;                    // 0..1: below the flood's mark, as fresh as it is
	float mark_line;               // 0..1: at its edge, the darker line
	float algae;                   // 0..1
	float wet;                     // 0..1: wet from the water (not the rain)
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

water_marks_t water_marks_at(vec3 position)
{
	water_marks_t marks;

	marks.under = 0.0;
	marks.to_surface = 1e6;
	marks.mark = 0.0;
	marks.mark_line = 0.0;
	marks.algae = 0.0;
	marks.wet = 0.0;
	if (ground_map.z < 0.5)
	{
		return marks;
	}
	ivec2 cell = clamp(ivec2(floor(position.xz / ground_map.y + 0.5)), ivec2(0), ivec2(int(ground_map.x) - 1));
	float ground = texelFetch(ground_height_texture, cell, 0).x;
	// The surface between the 4 cells round it (of those under water: a
	// cell's own steps would break the line where it meets things)
	{
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
			marks.to_surface = surface / weight - position.y;
			marks.under = max(marks.to_surface, 0.0);
		}
	}
	// (ground_map.w: 1 the marks are kept, 2 a baked map: what is under
	// water has always been, its algae grown in full)
	if (ground_map.w > 1.5)
	{
		marks.algae = smoothstep(0.0, 0.05, marks.under);
	}
	else if (ground_map.w > 0.5)
	{
		vec4 left = texelFetch(water_marks_texture, cell / 2, 0);
		float mark_top = ground + left.r * WATER_MARK_RANGE;
		float below = mark_top - position.y;

		if (left.g > 0.0 && left.r > 0.0)
		{
			// (it sets in slowly at first, and only shows above the water:
			// under it there is only the wet)
			float covered = marks.to_surface < 1e5 ? smoothstep(-0.01, 0.01, marks.to_surface) : 0.0;
			float set_in = smoothstep(0.25, 1.0, left.g) * (1.0 - covered);

			marks.mark = set_in * smoothstep(-0.005, 0.015, below);
			marks.mark_line = set_in * (1.0 - smoothstep(0.0, 0.03, abs(below - 0.01)));
			marks.algae = left.b * smoothstep(0.0, 0.05, below);
			marks.wet = left.a * smoothstep(-0.01, 0.02, below);
		}
	}
	if (marks.under > 0.0)
	{
		marks.wet = 1.0;
	}
	return marks;
}

// Only what the water there now does (an object the marks on its spot are
// not the marks of: it floats, or was moved): wet under it, its light
water_marks_t water_marks_present_only(water_marks_t marks)
{
	marks.mark = 0.0;
	marks.mark_line = 0.0;
	marks.algae = 0.0;
	marks.wet = marks.under > 0.0 ? 1.0 : 0.0;
	return marks;
}

// The color and the wet: under water (and drying after) darker and wet,
// mud below the mark (a darker line at its edge), algae on what stayed
// under water long (green-brown under it, pale and dry once out of it)
void apply_water_marks(water_marks_t marks, vec3 position, int kind, inout vec3 albedo, inout float wet)
{
	if (marks.mark <= 0.0 && marks.wet <= 0.0 && marks.algae <= 0.0)
	{
		return;
	}
	float patches = texture(variation_texture, position.xz * 0.9 + position.y * 0.7).r;
	// The mud: dark brown while wet, a pale dusty grey-beige once dry
	vec3 wet_mud = vec3(0.13, 0.10, 0.065);
	vec3 dry_mud = vec3(0.43, 0.39, 0.32);
	vec3 mud = mix(dry_mud, wet_mud, marks.wet) * (0.8 + 0.4 * patches);

	if (kind == WATER_MARKS_FOLIAGE)
	{
		// (only dulled by the mud: leaves take no film, no shine)
		albedo = mix(albedo, mud, 0.4 * marks.mark);
		return;
	}
	albedo = mix(albedo, mud, 0.7 * marks.mark * (0.6 + 0.4 * patches));
	albedo *= 1.0 - 0.35 * marks.mark_line;
	if (marks.algae > 0.0)
	{
		float film = smoothstep(0.25, 0.75, marks.algae + (patches - 0.5) * 0.6);
		vec3 living = vec3(0.045, 0.075, 0.02);
		vec3 dried = vec3(0.32, 0.31, 0.24);

		albedo = mix(albedo, mix(dried, living, marks.wet), 0.85 * film);
	}
	wet = max(wet, kind == WATER_MARKS_BARK ? 0.6 * marks.wet : marks.wet);
}

// Light added after the lighting: the sun played over what is under water
// (its caustics, fading with the depth), and the bright, wobbling line
// where it meets the surface (not on foliage). sun_light: the sun reaching
// it (its shadows)
vec3 water_marks_light(water_marks_t marks, vec3 position, int kind, vec3 sun_light)
{
	vec3 light = vec3(0.0);

	if (marks.under > 0.0)
	{
		light += sun_light * underwater_caustics(position.xz) * 0.5 * exp(-marks.under * 0.8) * smoothstep(0.0, 0.03, marks.under);
	}
	if (kind != WATER_MARKS_FOLIAGE && marks.to_surface < 1e5)
	{
		float wobble = 0.008 * sin(time_seconds * 2.3 + position.x * 3.1 + position.z * 2.7)
			+ 0.005 * sin(time_seconds * 3.7 - position.x * 1.9 + position.z * 4.3);
		float line = 1.0 - smoothstep(0.0, 0.02, abs(marks.to_surface + wobble));

		light += (sun_light * 0.6 + sky_color * ambient_intensity) * 0.5 * line;
	}
	return light;
}
