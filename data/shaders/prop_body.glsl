// The props' fragment shader's body (prop.frag; prop_shell.frag: with
// MOSS_SHELL, a shell of the moss under water, one layer of it; building.frag:
// with BUILDING, a building's aged materials).
//
// Buildings (OBJECT_MATERIAL_AGED, BUILDING): weathered as the props, by
// their vertices' baked weathering (x open to the sky, y edge, z shelter
// from the rain: object_format.h), and besides: the ambient light closed out
// where the sky is (joints, under the eaves); the edges worn lighter and
// rougher; streaks where the rain runs down the walls; no rain or snow
// under a roof; no moss, no lichen, no dirt up their foot, no flood's
// mark (they read as stains on a house, and cost every pixel of its walls).
// How much: the type's age (show_parameters y, its spread z by where it
// stands).

// The props' fragment shader (rocks, fences, walls: tools/potop_prop): as
// object.frag (its light, its physically based shading), and besides:
//
// Triplanar (OBJECT_MATERIAL_TRIPLANAR): the textures laid on from the
// model's sides, from above and below (in its own axes, so they stay with
// it as it turns), each where the surface faces that way, one repeat every
// triplanar_meters; the normal map bent in each of them (Golus' whiteout
// blend). No texture coordinates needed: a rock of any shape, holes and
// all, with no seams.
//
// Weathered (OBJECT_MATERIAL_WEATHERED): moss where it faces up and north
// and in its crevices (never on its sharp edges), greener the wetter it is;
// lichen in round patches where it is open to the sky and dry; snow on what
// faces up through the snow's months. Triplanar ones say by their vertices
// where the crevices, the edges and the open sky are (object_format.h); the
// others only by which way they face.

#include "shadows.glsl"
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D variation_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D color_texture;
layout(set = TEXTURE_SET, binding = 4) uniform sampler2D cloud_shadow_texture;  // the volumetric clouds' shadows (clouds.glsl)
layout(set = TEXTURE_SET, binding = 5) uniform sampler2D normal_texture;        // tangent space, XYZ * 0.5 + 0.5
layout(set = TEXTURE_SET, binding = 6) uniform sampler2D orm_texture;           // occlusion, roughness, metal; alpha: how much it reflects at all (0: the neutral map of a model without one)
layout(set = TEXTURE_SET, binding = 7) uniform sampler2D emissive_texture;      // sRGB
#define CLOUD_SHADOW_TEXTURE
#include "underwater.glsl"

layout(std140, set = UNIFORM_SET, binding = 1) uniform material_block
{
	vec4 highlight_parameters;            // the highlight's color (sRGB 0..1), time (seconds, for the pulse)
	int alpha_tested;
	int pbr;                              // 1: physically based (see above)
	int lighting_normals;                 // 1: the normals light the face, the same from either side (plant cards)
	int triplanar;                        // 1: triplanar (see above)
	vec4 weathering_parameters;           // x: meters a repeat (triplanar), y: how much lichen grows (0..1; below 0: not weathered), z: the snow (0..1), w: how much moss (0..1)
	vec4 show_parameters;                 // x: 0 as it is; the weathering as gray: 1 its crevices, 2 its edges, 3 its sky (tools/prop_editor)
	vec4 relief_parameters;               // a building's thatch: x 1 its texture's relief marched into (parallax), y how deep it goes (m)
	vec4 look_roof;                       // the buildings' look (settings): each roof's lightness, fading, warmth; its weathered patches
	vec4 look_more;                       // x the roofs' runs down the slope, y their mended stretches, z how much old wood dulls out of the sky
};
#define CLOUDS_EXPLICIT_LOD
#include "clouds.glsl"
#define GROUND_BINDING 8
#include "ground_water.glsl"
#define LIGHT_BINDING 14
#include "lights.glsl"
#include "decals.glsl"

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec2 texture_uv;
layout(location = 3) in vec4 shadow_position;
layout(location = 4) in vec4 near_shadow_position;
layout(location = 5) flat in float fade;
layout(location = 6) in vec4 world_tangent;
layout(location = 7) in vec3 model_point;
layout(location = 8) in vec3 own_normal;
layout(location = 9) in vec4 weathering;
layout(location = 10) flat in vec4 turning;
#ifdef MOSS_SHELL
layout(location = 11) flat in float shell_layer;     // how far up the moss this layer is (0..1)
#endif
#ifdef BUILDING
// Its model's look (objects.c, through the instance): x its type's age, y
// how much that differs from one to the next, z how much lichen, w moss
layout(location = 11) flat in vec4 building_look;
#endif

layout(location = 0) out vec4 fragment_color;
#include "scene_depth.glsl"

// Same as cloud_light in terrain.frag
float cloud_light(vec3 position, vec3 towards_sun)
{
	if (cloud_parameters.z <= 0.0)
	{
		return 1.0;
	}
	float climb = max(cloud_motion.z - position.y, 0.0) / max(towards_sun.y, 0.05);
	vec2 on_sheet = position.xz + towards_sun.xz * climb - cloud_motion.xy;
	float cloud = cloud_shade(on_sheet);

	return 1.0 - cloud * cloud_parameters.z;
}

#define PI 3.14159265
// How bright the emissive map shines (its full white over the sun's)
#define EMISSIVE_STRENGTH 1.5
// Of it, what still shows in full day
#define EMISSIVE_DAY 0.1
// The buildings' look (material_block, from the settings): a roof's
// variety (see where a building's roof is aged), the old wood dulled
#define ROOF_LIGHTNESS look_roof.x
#define ROOF_FADE look_roof.y
#define ROOF_WARMTH look_roof.z
#define ROOF_PATCHES look_roof.w
#define ROOF_RUNS look_more.x
#define ROOF_MENDED look_more.y
#define WOOD_DULLED look_more.z

// The sky and the ground around, seen along a direction: what lights the
// shadows (the ambient), and what a surface reflects
// The sky this pixel sees (object_sky_seen: worked out once, in main)
float sky_here = 1.0;
// How much of the ground round it the sun reaches, for the light the
// ground sends up: as much as the sky it sees (shut in among houses or
// trees, the ground there is mostly in their shade; out in the open, in
// the sun). Looked up in the far shadow map, the edges of the shadows on
// the ground ran up the walls as hard upright lines.
float ground_sunlit = 1.0;
// How far out from its face the sky it sees is looked up (meters)
const float SKY_LOOKUP_OUT_METERS = 1.5;
// The least of the land round's sky a building's faces keep
const float BUILDING_SKY_FLOOR = 0.5;
const float BUILDING_MOSS_UNDER = 0.6;
const float BUILDING_MOSS_STONE = 0.7;          // how much moss on stone, along its stones' tops and in its joints, on the oldest wall where it is damp          // how much the moss under a jetty or the eaves tints the wall, on the oldest house

// The light from round about, from the sky above and the ground below.
// building_ambient (a building's baked sky_open) closes out the sky alone:
// under the eaves the ground still lights the wall, warmer than anywhere
float building_ambient = 1.0;

vec3 surroundings(vec3 direction)
{
	vec3 ground_bounce = ground_bounce_color(ground_sunlit);

	return mix(ground_bounce, sky_color * building_ambient, direction.y * 0.5 + 0.5) * ambient_intensity * sky_here;
}

// GGX's distribution of the microfacets' normals
float distribution_ggx(float normal_half, float roughness)
{
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float denominator = normal_half * normal_half * (alpha2 - 1.0) + 1.0;

	return alpha2 / (PI * denominator * denominator);
}

// Smith's shadowing and masking, Schlick's fit (direct light)
float geometry_smith(float normal_view, float normal_light, float roughness)
{
	float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;

	return normal_view / (normal_view * (1.0 - k) + k) * normal_light / (normal_light * (1.0 - k) + k);
}

// The split sum's scale and bias for the reflected surroundings, an
// analytic fit (Karis, mobile)
vec2 environment_brdf(float normal_view, float roughness)
{
	const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
	const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
	vec4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * normal_view)) * r.x + r.y;

	return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Threshold of the pixel in a 4x4 Bayer matrix, in (0, 1)
float dither_threshold()
{
	const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
	ivec2 pixel = ivec2(gl_FragCoord.xy) & 3;

	return (bayer[pixel.y * 4 + pixel.x] + 0.5) / 16.0;
}


vec3 rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

// Smooth value noise in 3D (0..1)
float prop_hash(vec3 point)
{
	point = fract(point * 0.3183099 + 0.1);
	point *= 17.0;
	return fract(point.x * point.y * point.z * (point.x + point.y + point.z));
}

float prop_noise(vec3 point)
{
	vec3 cell = floor(point);
	vec3 inside = fract(point);
	vec3 weight = inside * inside * (3.0 - 2.0 * inside);

	return mix(mix(mix(prop_hash(cell), prop_hash(cell + vec3(1.0, 0.0, 0.0)), weight.x),
			mix(prop_hash(cell + vec3(0.0, 1.0, 0.0)), prop_hash(cell + vec3(1.0, 1.0, 0.0)), weight.x), weight.y),
		mix(mix(prop_hash(cell + vec3(0.0, 0.0, 1.0)), prop_hash(cell + vec3(1.0, 0.0, 1.0)), weight.x),
			mix(prop_hash(cell + vec3(0.0, 1.0, 1.0)), prop_hash(cell + vec3(1.0, 1.0, 1.0)), weight.x), weight.y), weight.z);
}

// Lichen: round patches, one round each cell's own point (at random in
// it), those whose number is below chance there; every patch near enough
// counted, not only the nearest point's (a patch reaches past its cell: it
// would be cut straight along the cells' borders). How much of it is
// covered (0..1), the most covering patch's number and how far from its
// middle, of its radius (raggedness: its radius times this). Whether a
// patch is there goes by the colonies at its middle (point is the model's
// times scale, plus offset), and by open here, softly: a patch fades where
// the stone stops being open, it is not cut off along a line
float prop_lichen(vec3 point, float scale, float offset, float amount, float open, float raggedness, out float number_out,
	out float across_out)
{
	vec3 cell = floor(point);
	float best = 0.0;

	number_out = 0.0;
	across_out = 1.0;
	for (int z = -1; z <= 1; z++)
	{
		for (int y = -1; y <= 1; y++)
		{
			for (int x = -1; x <= 1; x++)
			{
				vec3 other = cell + vec3(x, y, z);
				float number = prop_hash(other + 5.7);
				vec3 middle = other + vec3(prop_hash(other), prop_hash(other + 11.3), prop_hash(other + 27.1));
				float colonies = prop_noise((middle - offset) / scale * 1.7 + 9.0);
				float chance = amount * open * smoothstep(0.3, 0.7, colonies + 0.3 * amount);
				float there = clamp((chance - number) / 0.12, 0.0, 1.0);

				if (there <= 0.0)
				{
					continue;
				}
				float radius = (0.25 + 0.3 * fract(number * 7.13)) * raggedness;
				float distance = length(point - middle);
				float covered = (1.0 - smoothstep(radius * 0.7, radius, distance)) * there;

				if (covered > best)
				{
					best = covered;
					number_out = number;
					across_out = distance / radius;
				}
			}
		}
	}
	return best;
}

// The three projections' weights (where the surface faces each way)
vec3 triplanar_weights(vec3 normal)
{
	vec3 weights = pow(abs(normal), vec3(4.0));

	return weights / max(weights.x + weights.y + weights.z, 1e-4);
}

vec4 triplanar_sample(sampler2D map, vec3 point, vec3 weights)
{
	// (each projection a little elsewhere on the texture: their repeats
	// not lined up)
	return texture(map, point.zy + vec2(0.37, 0.11)) * weights.x + texture(map, point.xz) * weights.y
		+ texture(map, point.xy + vec2(0.71, 0.53)) * weights.z;
}

// The normal map in each projection, bent onto the surface (Golus'
// whiteout blend), in the model's axes
vec3 triplanar_normal(vec3 point, vec3 normal, vec3 weights)
{
	vec3 bump_x = texture(normal_texture, point.zy + vec2(0.37, 0.11)).xyz * 2.0 - 1.0;
	vec3 bump_y = texture(normal_texture, point.xz).xyz * 2.0 - 1.0;
	vec3 bump_z = texture(normal_texture, point.xy + vec2(0.71, 0.53)).xyz * 2.0 - 1.0;
	// (green up the picture, OpenGL's way: against the coordinates' second)
	bump_x.y = -bump_x.y;
	bump_y.y = -bump_y.y;
	bump_z.y = -bump_z.y;
	vec3 along_x = vec3(bump_x.xy + normal.zy, abs(bump_x.z) * normal.x);
	vec3 along_y = vec3(bump_y.xy + normal.xz, abs(bump_y.z) * normal.y);
	vec3 along_z = vec3(bump_z.xy + normal.xy, abs(bump_z.z) * normal.z);

	return normalize(along_x.zyx * weights.x + along_y.xzy * weights.y + along_z.xyz * weights.z);
}

// The moss that grows on the rocks under water (what keeps under it long:
// the algae, ground_water.glsl; without its memory, or on a rock moved
// there, wherever it is under water now): its share at a point, in
// patches, not under overhangs
float water_moss_share(water_marks_t marks, bool own_marks, vec3 normal, vec3 point)
{
	float grown = ground_map.w > 0.5 && own_marks ? marks.algae : (marks.under > 0.0 ? 1.0 : 0.0);

	if (grown <= 0.0)
	{
		return 0.0;
	}
	float patches = smoothstep(0.3, 0.6, prop_noise(point * 2.3 + 7.0) * 0.7 + grown * 0.45);

	return grown * patches * smoothstep(-0.45, 0.25, normal.y);
}

// How tall the moss stands here (0..1 of its full height): strands a few
// millimeters apart, in clumps. By the point in the model's axes, so each
// strand stands straight out of the surface through all the shells
float water_moss_strands(vec3 point)
{
	float strand = smoothstep(0.3, 0.85, prop_noise(point * 170.0));
	float clump = 0.55 + 0.45 * prop_noise(point * 41.0 + 3.0);

	return strand * clump;
}

// Its color: living green-brown in the water, pale and dry out of it
vec3 water_moss_color(vec3 point, float wet)
{
	vec3 living = mix(vec3(0.025, 0.06, 0.012), vec3(0.08, 0.15, 0.03), prop_noise(point * 23.0));
	vec3 dried = vec3(0.29, 0.28, 0.19) * (0.85 + 0.3 * prop_noise(point * 23.0));

	return mix(dried, living, clamp(wet, 0.0, 1.0));
}

// Its full height (m): what the parallax looks into (MOSS_SHELL's own
// height comes from prop_shell.vert)
const float WATER_MOSS_DEPTH = 0.02;

// A building's (BUILDING): the ambient light it lets in, how much rougher
// its worn edges are, how sheltered from the rain it is here, how old this
// one is (0..1)
float building_rough = 0.0;
float building_shelter = 0.0;
// A building's (main, its weathering): its material by the kind (0 wood,
// 1 plaster, 2 stone, 3 roof), how close something stands out over it,
// how open to the sky, its age eased (none new .. 1)
float building_kind = 0.0;
float building_under = 0.0;
float building_sky_open = 1.0;
float building_aged = 0.0;
float age_here = 1.0;


// Where the textures are read: the vertex's coordinates, or a thatch's
// moved by its relief (parallax_uv)
vec2 surface_uv;

#ifdef BUILDING
// A thatch's relief (relief_parameters): the height its normal map keeps
// in its alpha (tools/house_textures.c: the courses' lower edges standing
// out, the reeds' ends), the view marched down into it in steps from the top, the coordinates moved
// to where it meets it; seen at a slant, the bundles hide what is behind
// them. Its depth in texture coordinates by how far they go a meter here
// (from the screen's derivatives: worked out before anything is cut away)
const int RELIEF_STEPS = 12;

vec2 parallax_uv(vec2 uv, float depth_meters)
{
	vec2 uv_dx = dFdx(uv);
	vec2 uv_dy = dFdy(uv);
	float meters_per_uv = length(fwidth(world_position)) / max(length(fwidth(uv)), 1e-7);
	vec3 normal = normalize(world_normal);
	vec3 tangent = normalize(world_tangent.xyz - normal * dot(normal, world_tangent.xyz));
	vec3 bitangent = cross(normal, tangent) * (world_tangent.w < 0.0 ? -1.0 : 1.0);
	vec3 towards_eye = normalize(eye_position - world_position);
	vec3 seen = vec3(dot(towards_eye, tangent), dot(towards_eye, bitangent), max(dot(towards_eye, normal), 0.2));
	// (the whole depth's shift across the texture, at this slant)
	vec2 shift = seen.xy / seen.z * depth_meters / max(meters_per_uv, 1e-4);
	vec2 step_shift = shift / float(RELIEF_STEPS);
	float step_depth = 1.0 / float(RELIEF_STEPS);
	float depth = 0.0;
	vec2 at = uv;
	float surface = 1.0 - textureGrad(normal_texture, at, uv_dx, uv_dy).a;
	float before_depth = 0.0;
	float before_surface = surface;

	for (int i = 0; i < RELIEF_STEPS && depth < surface; i++)
	{
		before_depth = depth;
		before_surface = surface;
		at -= step_shift;
		depth += step_depth;
		surface = 1.0 - textureGrad(normal_texture, at, uv_dx, uv_dy).a;
	}
	// (between the last two steps, where the ray crossed it)
	float after_gap = surface - depth;
	float before_gap = before_surface - before_depth;
	float share = clamp(after_gap / min(after_gap - before_gap, -1e-5), 0.0, 1.0);

	return mix(at, at + step_shift, share);
}
#endif

void main()
{
		scene_depth_write();
	surface_uv = texture_uv;
#ifdef BUILDING
	if (relief_parameters.x > 0.5)
	{
		surface_uv = parallax_uv(texture_uv, relief_parameters.y);
	}
#endif
	// The sky the land round leaves it (sky_occlusion.glsl), looked up a
	// little out from its face: the occlusion was drawn with this very
	// object in it, and right at its face that is its own body hiding half
	// the sky, darker the lower (its own shading has that already)
	{
		vec2 out_flat = world_normal.xz;
		float out_length = length(out_flat);

		// (a face looking up or down: no way out to speak of, looked up
		// where it is; between, out the less the more it looks up, not a
		// step: a turning face, a mill's paddle, jumped 1.5 m out and lit
		// all at once)
		float out_amount = smoothstep(0.1, 0.5, out_length);

		sky_here = object_sky_seen(world_position + vec3(out_flat / max(out_length, 1e-4) * SKY_LOOKUP_OUT_METERS * out_amount, 0.0).xzy);
#ifdef BUILDING
		// A building's own body is in the occlusion too, all round it (under
		// its eaves, over its door, its walls' feet): its baked sky_open has
		// that already, the land round only dims it a little
		sky_here = mix(BUILDING_SKY_FLOOR, 1.0, sky_here);
#endif
		ground_sunlit = sky_here;
	}
	// A highlighted object comes with 2 added to its fade (see objects.h),
	// and 4 times how much (of 255) of what its next level of detail leaves
	// out is gone (objects_cull.comp)
	// (NO_MARKS_FADE added: the water's marks left on the spot are not its)
	float marks_off = floor(fade / NO_MARKS_FADE);
	float packed_fade = fade - NO_MARKS_FADE * marks_off;
	float level_steps = floor(packed_fade / 4.0);
	float own_fade = packed_fade - 4.0 * level_steps;
	bool highlighted = own_fade > 1.5;
	float drawn_fade = highlighted ? own_fade - 2.0 : own_fade;

	if (drawn_fade < 1.0 && drawn_fade < dither_threshold())
	{
		discard;
	}
	vec3 own = normalize(own_normal);
	// (by the smoothed normal: by the faces' own, the shares jump at every
	// edge between them, a seam along each)
	vec3 weights = triplanar_weights(own);
	vec3 laid = model_point / max(weathering_parameters.x, 0.01);
	vec4 color = triplanar != 0 ? triplanar_sample(color_texture, laid, weights) : texture(color_texture, surface_uv);
	vec3 normal = normalize(world_normal);
	vec3 surface_normal = normal;
	float diffuse;
	// (the cut out edge's coverage: alpha_cutout)
	float cut_alpha = 1.0;

	if (alpha_tested != 0)
	{
		// Mip level from the screen space size of a texel
		vec2 texel_position = surface_uv * vec2(textureSize(color_texture, 0));
		vec2 texel_ddx = dFdx(texel_position);
		vec2 texel_ddy = dFdy(texel_position);
		float mip_level = max(0.0, 0.5 * log2(max(dot(texel_ddx, texel_ddx), dot(texel_ddy, texel_ddy))));

		cut_alpha = alpha_cutout(color.a * (1.0 + 0.25 * mip_level), 0.5);
		// Leaves: lit from either side, and light wraps around a bit
		// (thin leaves let some through)
		if (!gl_FrontFacing && lighting_normals == 0)
		{
			normal = -normal;
		}
		diffuse = max(dot(normal, sun_direction) * 0.6 + 0.4, 0.0);
	}
	else
	{
		diffuse = max(dot(normal, sun_direction), 0.0);
	}

	vec3 albedo = pow(color.rgb, vec3(2.2));
#ifdef BUILDING
	{
		// This one's age: its type's, more or less by where it stands
		vec3 origin = world_position - rotate(turning, model_point);
		float sky_open = clamp(weathering.x, 0.0, 1.0);
		float edge_amount = clamp(weathering.y, 0.0, 1.0);

		// (the age every building is forced to: show_parameters.y, -1 none)
		float age_given = show_parameters.y >= 0.0 ? show_parameters.y : building_look.x;
		float age_spread = show_parameters.y >= 0.0 ? 0.0 : building_look.y;

		age_here = clamp(age_given + (prop_hash(floor(origin * 0.37) + 3.1) - 0.5) * 2.0 * age_spread, 0.0, 1.0);
		building_shelter = clamp(weathering.z, 0.0, 1.0);
		// The joints, under the eaves: little of the sky's light
		building_ambient = mix(0.3, 1.0, sky_open);
		// The edges worn: lighter, rougher, unevenly
		{
			float wear = edge_amount * age_here * (0.45 + 0.55 * prop_noise(model_point * 13.0));

			albedo = mix(albedo, min(albedo * 1.35 + 0.03, vec3(1.0)), wear * 0.7);
			building_rough = wear * 0.3;
		}
		// What the vertex's kind says (object_format.h): its whole part the
		// material (0 wood, 1 plaster, 2 stone, 3 roof), rounded, not floored
		// (a vertex's 1.0, interpolated, comes out 0.99999 here and there, by
		// where the triangle's edges fall on the screen); its fraction how
		// close something stands out over it (a jetty, the eaves)
		float kind_whole = floor(weathering.w + 0.005);
		float under = clamp((weathering.w - kind_whole) / 0.99, 0.0, 1.0);
		// (4 and 5: plaster and stone blackened by smoke, a smithy's hood
		// and chimney; their fraction how much soot, nothing over them)
		float soot = 0.0;

		if (kind_whole > 3.5)
		{
			soot = under;
			under = 0.0;
			kind_whole -= 3.0;
		}
		float wood = abs(kind_whole) < 0.5 ? 1.0 : 0.0;
		float plaster = abs(kind_whole - 1.0) < 0.5 ? 1.0 : 0.0;
		float upright = 1.0 - abs(normalize(world_normal).y);
		// (a middling age shows it already, the oldest fully)
		float aged_by = smoothstep(0.1, 0.7, age_here);

		building_kind = kind_whole;
		building_under = under;
		building_sky_open = sky_open;
		building_aged = aged_by;
		// The silver gray old wood weathers to where the rain and the sky
		// reach it; under the eaves and the jetties it stays brown
		vec3 silver = vec3(dot(albedo, vec3(0.3, 0.59, 0.11)) * 1.25 + 0.03) * vec3(0.96, 0.97, 1.0);
		{
			float exposed = (1.0 - building_shelter) * smoothstep(0.3, 0.9, sky_open);

			albedo = mix(albedo, silver, wood * exposed * aged_by * 0.75);
			// (and where the sky does not reach, under a log, under the
			// eaves: not fresh either, dulled to an old brown; a fresh
			// bark's bare wood stayed bright orange there, as if lit by a
			// fire from below the silvered tops)
			{
				float dull_gray = dot(albedo, vec3(0.3, 0.59, 0.11));
				vec3 old_brown = vec3(dull_gray * 0.95, dull_gray * 0.82, dull_gray * 0.68);

				albedo = mix(albedo, old_brown, wood * (1.0 - exposed) * aged_by * WOOD_DULLED);
			}
		}
		// Streaks down the walls, where the rain reaches: darker
		{
			float runs = prop_noise(vec3(model_point.x * 7.0, model_point.y * 0.35, model_point.z * 7.0));
			// (on plaster and stone as they were; on the wood more of them,
			// darker, the silvered wood dark again: grayer on gray was lost)
			float streaks_plain = smoothstep(0.45, 0.85, runs) * age_here * 0.35;
			float streaks_wood = smoothstep(0.35, 0.75, runs) * aged_by * 0.5;
			// (none on a roof's kind: a roof is not upright, but a rick of hay
			// weathered as one is, and the rain does not run down it in
			// streaks)
			float not_roof = abs(kind_whole - 3.0) < 0.5 ? 0.0 : 1.0;

			albedo *= 1.0 - upright * (1.0 - building_shelter) * mix(streaks_plain, streaks_wood, wood) * not_roof;
		}
		// A wooden wall's foot, its lowest 70 cm where it stands on the
		// ground (a barn's, a log cabin's): darker and a little green, the
		// rain's splashes and the damp from below
		{
			float foot = 1.0 - smoothstep(0.15, 0.7, model_point.y);

			// (after the silvering, which is strongest down there, the
			// most exposed: else the foot came out lighter)
			albedo = mix(albedo, albedo * vec3(0.5, 0.56, 0.4), foot * upright * wood * aged_by);
		}
		// A film of moss right under what stands out over a wall (a jetty,
		// the eaves), the damp there: a tint only, patchy, running down a
		// little, more the older the house; none on a new one; on plaster,
		// and fainter on wood
		{
			// (patchy, never quite gone between the patches: cut to nothing
			// there, half the film was lost)
			float patches = 0.35 + 0.65 * smoothstep(0.3, 0.7, prop_noise(vec3(model_point.x * 2.3, model_point.y * 1.1, model_point.z * 2.3)));
			// (no fine grain in it: a noise of centimeters, unfiltered, shimmered
			// in moving stripes at any distance; the texture has its grain)
			float fine = 0.85 + 0.3 * prop_noise(model_point * 1.7 + 11.0);
			float film = smoothstep(0.1, 0.85, under) * upright * patches * (plaster + wood) * aged_by * BUILDING_MOSS_UNDER;
			vec3 moss_color = vec3(0.17, 0.21, 0.08) * fine;

			albedo = mix(albedo, moss_color, clamp(film, 0.0, 1.0));
		}
		// The soot: black, patchy (thicker in blotches and in runs up where
		// the smoke rose), duller
		if (soot > 0.0)
		{
			float blotches = 0.6 + 0.4 * smoothstep(0.25, 0.75, prop_noise(vec3(model_point.x * 3.1, model_point.y * 1.3, model_point.z * 3.1) + 5.0));
			// (in linear light: half covered still read as nearly clean)
			float thin = 1.0 - clamp(soot * blotches * 1.1, 0.0, 1.0);
			float covered = clamp(1.0 - thin * thin * thin, 0.0, 0.97);

			albedo = mix(albedo, vec3(0.018, 0.016, 0.014), covered);
			building_rough += covered * 0.25;
		}
		// A roof's own wear, so that no two roofs of a village read alike
		// from afar: each house's a little lighter or darker, warmer or
		// cooler, more or less faded; over it big patches meters across
		// (some places weather faster), runs down the slope where the rain
		// goes (darker the older), and here and there a stretch mended
		// with newer shingles or tiles (lighter, browner)
		if (abs(kind_whole - 3.0) < 0.5)
		{
			vec3 house_cell = floor(origin * 0.37);
			float lightness = prop_hash(house_cell + 7.7);
			float faded = prop_hash(house_cell + 13.3);
			float warmth = prop_hash(house_cell + 21.1);
			vec3 fresh = albedo;
			float gray = dot(albedo, vec3(0.3, 0.59, 0.11));
			// (weathered: grayer and lighter, dusty, lichen gray; on dark
			// slate that is what shows, on red tiles the darkening does)
			vec3 weathered = vec3(gray * 1.6 + 0.025, gray * 1.6 + 0.024, gray * 1.5 + 0.018);
			float wear = 0.4 + 0.6 * age_here;

			albedo = mix(vec3(gray), albedo, mix(1.0 - ROOF_FADE, 1.0 + ROOF_FADE * 0.5, faded));
			albedo *= mix(1.0 - ROOF_LIGHTNESS, 1.0 + ROOF_LIGHTNESS * 0.6, lightness);
			albedo *= mix(vec3(1.0 + ROOF_WARMTH, 1.0, 1.0 - ROOF_WARMTH * 1.5), vec3(1.0 - ROOF_WARMTH, 1.0, 1.0 + ROOF_WARMTH), warmth);
			// (the noises read in turned axes: a value noise's grid along
			// the model's axes drew straight lines over the slopes)
			vec3 turned = vec3(dot(model_point, vec3(0.80, 0.36, -0.48)), dot(model_point, vec3(-0.28, 0.93, 0.24)), dot(model_point, vec3(0.53, -0.07, 0.85)));
			{
				float big = prop_noise(turned * 0.3 + lightness * 17.0);
				float middling = prop_noise(turned * 0.9 + 3.3);
				float patches = big * 0.65 + middling * 0.35;

				albedo = mix(albedo, weathered, smoothstep(0.5, 0.8, patches) * ROOF_PATCHES * wear);
				albedo *= 1.0 - smoothstep(0.45, 0.15, patches) * ROOF_PATCHES * 0.5 * wear;
			}
			{
				// (darker runs on a light roof, dusty ones on a dark one)
				float runs = smoothstep(0.5, 0.85, prop_noise(vec3(turned.x * 4.0, model_point.y * 0.45, turned.z * 4.0) + 9.0));
				vec3 run_color = gray < 0.1 ? weathered : albedo * 0.6;

				albedo = mix(albedo, run_color, runs * ROOF_RUNS * wear);
			}
			{
				// (stretches laid new, in soft patches a meter or so across:
				// the fresh color, none of the wear, a little warmer; more the
				// older the roof, there having been more to mend)
				float mended = smoothstep(0.78, 0.86, prop_noise(turned * 1.3 + 31.7));

				albedo = mix(albedo, fresh * vec3(1.08, 1.02, 0.95), mended * ROOF_MENDED * wear);
			}
		}
		// (no dirt splashed up its foot, no moss, no flood's mark: on a
		// house they read as a stain, and cost every pixel of its walls)
	}
#endif
	// Wet in the rain (rain.h): darker, the more the surface faces up, and
	// shinier (below); under a roof, dry
	float wet = weather.y * (0.35 + 0.65 * clamp(normal.y, 0.0, 1.0)) * (1.0 - building_shelter);
	// What the water left on it (ground_water.glsl): the mark, the wet, the algae
	int water_marks_kind = WATER_MARKS_SOLID;
	water_marks_t water_marks = water_marks_at(world_position);
	if (marks_off > 0.5)
	{
		water_marks = water_marks_present_only(water_marks);
	}
#ifdef BUILDING
	water_marks = water_marks_present_only(water_marks);
#endif

	// The moss under water stands in for the algae's film on the rocks
	float water_moss = 0.0;

#ifndef BUILDING
	if (weathering_parameters.y >= 0.0)
	{
		water_moss = water_moss_share(water_marks, marks_off < 0.5, normal, model_point);
		water_marks.algae = 0.0;
	}
#endif
#ifdef MOSS_SHELL
	// A shell: only its strands that reach up to it
	if (water_moss * water_moss_strands(model_point) <= shell_layer)
	{
		discard;
	}
	// (darker down among the strands)
	albedo = water_moss_color(model_point, water_marks.wet) * (0.3 + 0.7 * shell_layer);
	wet = 0.0;
#else
	apply_water_marks(water_marks, world_position, water_marks_kind, albedo, wet);
#endif

	albedo *= 1.0 - 0.3 * wet;
	// What grows on it, and the snow: their shares of the surface, their
	// colors, how rough they are
	float moss = 0.0;
	float lichen = 0.0;
	float snow = 0.0;
	vec3 lichen_color = vec3(0.0);

	// (the moss under water, its bumps; a shell is all moss)
	float moss_cover = 0.0;
	vec3 moss_normal = normal;

#ifdef MOSS_SHELL
	moss = 1.0;
#else
	if (weathering_parameters.y >= 0.0)
	{
		bool baked = triplanar != 0;
		float crevice = baked ? clamp(weathering.x, 0.0, 1.0) : 0.0;
		float edge = baked ? clamp(weathering.y, 0.0, 1.0) : 0.0;
		float sky = baked ? clamp(weathering.z, 0.0, 1.0) : 1.0;
		// (moss and lichen as much as its age says on a building)
		float moss_amount = weathering_parameters.w;
		float lichen_amount = weathering_parameters.y;
#ifdef BUILDING
		crevice = 1.0 - clamp(weathering.x, 0.0, 1.0);
		edge = clamp(weathering.y, 0.0, 1.0);
		sky = clamp(weathering.x, 0.0, 1.0);
		moss_amount = building_look.w * age_here;
		lichen_amount = building_look.z * age_here;
#endif
		// The north (away from the noon sun), as the game has it
		vec2 north = normalize(aurora.xy + vec2(1e-5));
		float north_facing = clamp(dot(surface_normal.xz, north), 0.0, 1.0);
		float up = surface_normal.y;
		float blotches = prop_noise(model_point * 3.1) * 0.65 + prop_noise(model_point * 9.7 + 4.1) * 0.35;
#ifndef BUILDING
		float wetness = 0.35 + 0.65 * weather.y;
		// Moss: where it faces up and north, in the crevices, never on the
		// sharp edges; the more it grows, the lower the bar; its edge broken
		// in tufts
		float score = up * 0.5 + north_facing * 0.35 + crevice * 0.7 - edge * 0.8 + (blotches - 0.5) * 0.9;
		float bar = 1.05 - moss_amount * 0.95;
		// (more where the sky is hidden: deep in a wood, in a hollow)
		float hidden = 0.0;

		if (sky_occlusion.y > 0.5 && ground_map.z > 0.5)
		{
			ivec2 ground_cell = clamp(ivec2(floor(world_position.xz / ground_map.y + 0.5)), ivec2(0), ivec2(int(ground_map.x) - 1));

			hidden = sky_hidden_above(world_position.xz, world_position.y - texelFetch(ground_height_texture, ground_cell, 0).x);
		}
		score += hidden * 0.35 * moss_amount;

		moss = moss_amount > 0.001 ? smoothstep(bar - 0.2, bar + 0.2, score) : 0.0;
		// (under a roof it stays dry: little grows)
		moss *= 1.0 - 0.6 * building_shelter;
		moss *= smoothstep(0.25, 0.55, prop_noise(model_point * 31.0) * 0.7 + moss * 0.5);
		{
			vec3 lush = mix(vec3(0.05, 0.09, 0.015), vec3(0.14, 0.21, 0.03), prop_noise(model_point * 23.0));
			vec3 dry = vec3(0.13, 0.13, 0.045);

			// (its own small shadows: the tufts)
			float tufts = 0.65 + 0.6 * prop_noise(model_point * 85.0) * prop_noise(model_point * 37.0 + 2.0);

			albedo = mix(albedo, mix(dry, lush, wetness) * tufts, moss);
		}
#endif
		// (no lichen on a building: a wall filling the screen paid for every
		// pixel's 27 cells, and its patches read as stains)
#ifndef BUILDING
		// Lichen: crusts on the open, dry, bare stone, in colonies (a slow
		// noise says where), each a ragged round patch, a few big ones among
		// many small
		{
			float open = sky * (1.0 - crevice) * (1.0 - moss);
			int sizes = 2;

			for (int size = 0; size < sizes; size++)
			{
				float scale = size == 0 ? 17.0 : 6.0;
				float offset = float(size) * 3.7;
				float number;
				float across;
				float here = prop_lichen(model_point * scale + offset, scale, offset, lichen_amount * (size == 0 ? 1.0 : 0.35),
					open, 0.75 + 0.5 * prop_noise(model_point * 70.0), number, across);

				if (here > 0.0)
				{
					float pick = fract(number * 13.7 + float(size) * 0.31);
					vec3 crust = pick < 0.35 ? vec3(0.55, 0.45, 0.12) : (pick < 0.7 ? vec3(0.42, 0.47, 0.36)
						: (pick < 0.92 ? vec3(0.62, 0.62, 0.56) : vec3(0.62, 0.30, 0.06)));

					// (lighter at its rim, where it grows)
					crust *= 0.8 + 0.3 * smoothstep(0.3, 0.9, across);
					lichen = max(lichen, here);
					albedo = mix(albedo, crust * (0.85 + 0.3 * prop_noise(model_point * 120.0)), here * 0.85);
				}
			}
		}
#endif
		// Snow on what faces up (and is open to the sky)
		snow = weathering_parameters.z * smoothstep(0.3, 0.7, up + (blotches - 0.5) * 0.3) * (0.4 + 0.6 * sky);
		// (none under water, nor under a roof)
		snow *= 1.0 - smoothstep(0.0, 0.03, under_water_depth(world_position));
		snow *= 1.0 - building_shelter;
		albedo = mix(albedo, vec3(0.85, 0.87, 0.9), snow);
	}
	// The moss under water, seen into: a few steps along the view down
	// through its strands (in the model's axes) to where the eye meets the
	// first, its color there, darker the deeper; its strands bump the
	// light
	if (water_moss > 0.0)
	{
		vec4 back = vec4(-turning.xyz, turning.w);
		vec3 towards_eye = rotate(back, normalize(eye_position - world_position));
		float facing = max(dot(towards_eye, own), 0.3);
		vec3 across = (towards_eye - own * dot(towards_eye, own)) / facing;
		float depth = WATER_MOSS_DEPTH * water_moss;
		vec3 point = model_point;
		float height = water_moss * water_moss_strands(point);
		bool near = length(eye_position - world_position) < 15.0;

		if (near && height < 1.0)
		{
			for (int step_index = 1; step_index <= 4; step_index++)
			{
				float level = 1.0 - 0.25 * float(step_index);

				point = model_point - across * depth * (1.0 - level);
				height = water_moss * water_moss_strands(point);
				if (height >= level)
				{
					break;
				}
			}
		}
		moss_cover = smoothstep(0.04, 0.2, water_moss);
		albedo = mix(albedo, water_moss_color(point, water_marks.wet) * (0.3 + 0.7 * height), moss_cover);
		moss = max(moss, moss_cover);
		// The bumps: the strands' slope, along the surface
		{
			float step_size = 0.004;
			float here = water_moss_strands(point);
			vec3 slope = vec3(water_moss_strands(point + vec3(step_size, 0.0, 0.0)) - here,
				water_moss_strands(point + vec3(0.0, step_size, 0.0)) - here, water_moss_strands(point + vec3(0.0, 0.0, step_size)) - here);

			slope -= own * dot(slope, own);
			moss_normal = normalize(rotate(turning, normalize(own - slope * 0.6 * water_moss)));
		}
		normal = normalize(mix(normal, moss_normal, moss_cover));
		diffuse = max(dot(normal, sun_direction), 0.0);
	}
#endif
	// The tangent's w: its sign the bitangent's, its size how lit the vertex
	// is (tools/potop_plant: foliage deep in a crown, each card a little
	// lighter or darker); 1 everywhere else. Only cut out faces read it (a
	// mirrored seam's triangle may have both signs, its w passing 0)
	float tangent_size = abs(world_tangent.w);
	float shade = alpha_tested != 0 && tangent_size > 1e-3 ? min(tangent_size, 1.5) : 1.0;
	float sun_shade = 0.5 + 0.5 * shade;      // the sun reaches in more than the sky does
	float shadowed = sun_visibility(shadow_position, near_shadow_position) * cloud_light(world_position, sun_direction);
	float sun = diffuse * shadowed;
	vec3 lit;

	if (pbr != 0)
	{
		vec3 view = normalize(eye_position - world_position);
		vec3 tangent = world_tangent.xyz - normal * dot(normal, world_tangent.xyz);
		vec4 orm = triplanar != 0 ? triplanar_sample(orm_texture, laid, weights) : texture(orm_texture, surface_uv);
		float reflecting = orm.a;
		float occlusion = orm.r;
		// (moss and lichen are rough, wet stone smooth, snow between)
		float roughness = clamp(mix(mix(orm.g * (1.0 - 0.55 * wet), 0.95, max(moss, lichen)), 0.55, snow) + building_rough, 0.045, 1.0);
		float metal = orm.b;
		vec3 reflectance = mix(vec3(0.04), albedo, metal);

		// The normal map: triplanar, bent in the model's axes and turned as
		// the object is; else in the tangent frame (none where the model has
		// no usable tangent). What grows on it and the snow smooth it over
#ifndef MOSS_SHELL
		if (triplanar != 0)
		{
			vec3 bent = rotate(turning, triplanar_normal(laid, own, weights));

			normal = normalize(mix(bent, normal, 0.6 * max(moss, snow)));
		}
		else if (dot(tangent, tangent) > 1e-8)
		{
			vec3 bumped = texture(normal_texture, surface_uv).xyz * 2.0 - 1.0;

			tangent = normalize(tangent);
			// (the frame turns with the normal on a leaf's back)
			vec3 bitangent = cross(normal, tangent) * (world_tangent.w < 0.0 ? -1.0 : 1.0);

			normal = normalize(tangent * bumped.x + bitangent * bumped.y + normal * bumped.z);
		}
		normal = normalize(mix(normal, moss_normal, moss_cover));
#endif
#ifdef BUILDING
		// Moss on stone, by its relief: along the stones' tops, where the
		// normal map tilts up off the wall (the rain and the dust settle
		// there), and a little in the joints (the occlusion low), where it
		// is damp: at the wall's foot, under a jetty or the eaves, where it
		// sees little sky (and there a film over the stone too, faint: on a
		// fine stone, a smooth base, the tops and the joints alone were too
		// thin to see); a film even on the oldest wall, never green all
		// over (a house lived in, not a ruin); on flat stone (a base's top,
		// a sill) faint
		if (abs(building_kind - 2.0) < 0.5)
		{
			float flat_face = smoothstep(0.6, 0.9, surface_normal.y);
			float tilt_up = normal.y - surface_normal.y;
			// (a dressed stone's relief is shallower than rubble's, a base's
			// all but flat: the tops from a small tilt on, the joints by a
			// little occlusion already)
			float tops = smoothstep(0.04, 0.25, tilt_up);
			float joints = smoothstep(0.93, 0.6, orm.r);
			float foot = 1.0 - smoothstep(0.2, 0.9, model_point.y);
			float damp = max(max(foot, building_under * 0.8), (1.0 - building_sky_open) * 0.7);
			float patches = 0.3 + 0.7 * smoothstep(0.3, 0.7, prop_noise(vec3(model_point.x * 1.7, model_point.y * 0.9, model_point.z * 1.7) + 5.0));
			float film = building_aged * patches * (0.35 * damp + (0.7 * tops + 0.6 * joints) * mix(0.4, 1.0, damp)) * mix(1.0, 0.3, flat_face) * BUILDING_MOSS_STONE;
			vec3 moss_color = vec3(0.15, 0.2, 0.07) * (0.85 + 0.3 * prop_noise(model_point * 1.3 + 2.0));

			albedo = mix(albedo, moss_color, clamp(film, 0.0, 0.6));
		}
#endif
#ifndef MOSS_SHELL
		// The decals over it all (a bullet's hole through the moss too)
		apply_decals(world_position, surface_normal, albedo, normal, roughness);
#endif
		{
			float normal_view = max(dot(normal, view), 1e-4);
			float normal_light = max(dot(normal, sun_direction), 0.0);
			vec3 half_way = normalize(view + sun_direction);
			float normal_half = max(dot(normal, half_way), 0.0);
			float view_half = max(dot(view, half_way), 0.0);
			vec3 fresnel = (reflectance + (1.0 - reflectance) * pow(1.0 - view_half, 5.0)) * reflecting;
			// Leaves keep their light that wraps around (see above)
			float diffuse_light = alpha_tested != 0 ? max(dot(normal, sun_direction) * 0.6 + 0.4, 0.0) : normal_light;
			vec3 specular = distribution_ggx(normal_half, roughness) * geometry_smith(normal_view, normal_light, roughness) * fresnel
				/ max(4.0 * normal_view * normal_light, 1e-4);
			// (the face itself turned from the sun: its normal map lights
			// none of it, a tilted bump never catching the sun behind it;
			// leaves let it through. Only past the terminator: faded in
			// over the lit side, a roof at a slant to a low sun lost all
			// its sunlight, flat)
			float facing_sun = alpha_tested != 0 ? 1.0 : smoothstep(-0.06, 0.0, dot(surface_normal, sun_direction));
			vec3 direct = ((1.0 - fresnel) * (1.0 - metal) * albedo * diffuse_light + specular * PI * normal_light) * sun_color * shadowed
				* facing_sun;
			vec2 split = environment_brdf(normal_view, roughness);
			vec3 around = albedo * (1.0 - metal) * (surroundings(normal) + object_bounce(world_position, normal, sky_here))
				+ surroundings(reflect(-view, normal)) * (reflectance * split.x + split.y) * reflecting;
			// (lit at night: drowned by day, as the lamps are)
			vec3 own_light = pow(texture(emissive_texture, surface_uv).rgb, vec3(2.2)) * EMISSIVE_STRENGTH * mix(EMISSIVE_DAY, 1.0, light_slicing.w);

			lit = direct * sun_shade + around * occlusion * shade + own_light
				+ (1.0 - metal) * albedo * local_light(world_position, normal, alpha_tested != 0 ? 0.4 : 0.0) * occlusion * shade;
			sun = normal_light * shadowed;
		}
	}
	else
	{
#ifndef MOSS_SHELL
		float plain_roughness = 1.0;

		apply_decals(world_position, surface_normal, albedo, normal, plain_roughness);
#endif
		lit = albedo * (sun_color * sun * sun_shade + ((surroundings(normal) + object_bounce(world_position, normal, sky_here))
			+ local_light(world_position, normal, alpha_tested != 0 ? 0.4 : 0.0)) * shade);
	}
	if (wet > 0.0)
	{
		vec3 towards_eye = normalize(eye_position - world_position);
		float shine = pow(max(dot(reflect(-sun_direction, normal), towards_eye), 0.0), 40.0);

		lit += sun_color * shadowed * shine * wet * 0.25;
	}
	float view_distance = length(world_position - eye_position);
	lit += water_marks_light(water_marks, world_position, water_marks_kind, sun_color * shadowed);
	float fog_amount = haze_amount(world_position);

	lit = mix(lit, sky_color, clamp(fog_amount, 0.0, 1.0));
	lit = apply_underwater(lit, world_position, view_distance, sun_color * sun);
	if (highlighted)
	{
		// A glow that is strongest at the outline (where the surface turns
		// away from the eye), pulsing
		float facing = abs(dot(normal, normalize(eye_position - world_position)));
		float rim = pow(1.0 - facing, 2.0);
		float pulse = 0.65 + 0.35 * sin(highlight_parameters.w * 5.0);

		lit += highlight_parameters.rgb * (0.18 + 1.1 * rim) * pulse;
	}
	if (show_parameters.x > 0.5)
	{
		int shown = int(show_parameters.x + 0.5);
		float value = shown == 1 ? weathering.x : (shown == 2 ? weathering.y : weathering.z);

		lit = vec3(value * value) * 0.6;
	}
#ifdef BUILDING
	// (alpha 0.25: a building's pixel, the temporal anti-aliasing's to
	// smooth; the passes blended over the scene leave the alpha as it is)
	fragment_color = vec4(lit, scene_alpha(cut_alpha, 0.25));
#else
	fragment_color = vec4(lit, cut_alpha);
#endif
}
