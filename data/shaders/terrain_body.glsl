// The terrain's fragment shader, its two variants: terrain.frag (the
// materials worked out per pixel) and terrain_map.frag (from the material
// map: MATERIAL_MAP). Included by them alone.
#include "common.glsl"
#include "scene.glsl"
#include "constants.glsl"

// Terrain fragment shader.
//
// What the ground looks like is decided here, every frame, from data the
// simulation already has:
//   soil type (per cell)  -> which materials are possible
//   slope                 -> grass on gentle slopes, bare dirt and rock on steep ones
//   sun exposure          -> shady slopes stay greener and keep snow lower
//   height                -> snow line
//   surface state         -> silt the water left, bedrock it laid bare,
//                            darker wet ground
// Where materials meet, each breaks into patches of the others (procedural,
// in world space: no texture, nothing repeats) instead of fading into them;
// only the materials that show are read. Lit with the sun plus a sky/ground
// ambient term, and finally faded into the sky with distance fog.

#include "shadows.glsl"
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D variation_texture;          // tileable cloudy noise, R8
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D soil_weight_texture;        // RGBA8: blurred sand, earth, clay, rock weights
layout(set = TEXTURE_SET, binding = 4) uniform sampler2D normal_texture;             // RGBA8: surface normal x, z; material normal x, z
layout(set = TEXTURE_SET, binding = 5) uniform sampler2DArray material_color_array;  // RGB color, A height
layout(set = TEXTURE_SET, binding = 6) uniform sampler2DArray material_normal_array; // tangent space normals
layout(set = TEXTURE_SET, binding = 7) uniform sampler2D surface_texture;            // RGBA8: silt cover, wetness, bare bedrock, water on it
// Small integers stored as R8 (SDL_GPU samples no integer textures): read
// with texelFetch, times 255
layout(set = TEXTURE_SET, binding = 8) uniform sampler2D tile_texture;               // per water tile, see apply_tile_overlay
layout(set = TEXTURE_SET, binding = 9) uniform sampler2D navigation_texture;         // per navigation cell, see apply_navigation_overlay
layout(set = TEXTURE_SET, binding = 10) uniform sampler2D cloud_shadow_texture;      // the volumetric clouds' shadows (clouds.glsl)
layout(set = TEXTURE_SET, binding = 11) uniform sampler2D material_map_texture;      // RGBA8 per cell: base, cover, overlay ids, shelter (material_map.h)
layout(set = TEXTURE_SET, binding = 12) uniform sampler2D overlay_texture;           // RGBA16F per cell: snow, petals (ground_overlay.h)
layout(set = TEXTURE_SET, binding = 13) uniform sampler2D sky_occlusion_texture;     // R8 per cell: the sky it sees (sky_occlusion.h)
layout(set = TEXTURE_SET, binding = 14) uniform sampler2D sky_layers_texture;        // R8: the same in the air (sky_occlusion.glsl)
layout(set = TEXTURE_SET, binding = 15) uniform sampler2D sky_bounce_texture;        // RGBA8: the light the land round sends back (sky_occlusion.glsl)
#define CLOUD_SHADOW_TEXTURE
#include "underwater.glsl"

layout(std140, set = UNIFORM_SET, binding = 1) uniform terrain_block
{
	ivec4 material_layers[MATERIAL_COUNT];  // first layer in the arrays, number of variants
	vec4 material_scales[MATERIAL_COUNT];   // repeats per meter: detail, macro; z: its wet texture's layer (-1: none)
	vec4 material_patches[MATERIAL_COUNT];  // where it meets another: its patches' repeats per meter, their strength, their edge (meters)
	vec4 brush_line;                        // segment start x, z, end x, z
	vec4 slope_ranges;                      // grass start, grass end, cliff start, cliff end
	vec3 variation_parameters;              // 1 / tiling meters, strength, hue strength
	float brush_radius;                     // outline distance from the segment (<= 0 hides)
	vec2 detail_fade;                       // fine detail fades out between these distances
	vec2 navigation_cell_meters;            // cell size, cells per side
	int map_size;
	float cell_size;
	float max_height;
	float snow_height_fraction;
	float snow_aspect_shift;
	float soil_blend_cells;                 // width of soil transitions, in cells
	int debug_view;                         // 0 normal, 1 soil types
	int tile_overlay;                       // 1 shows the water tile overlay
	int navigation_overlay;                 // 1 shows it
	int material_map;                       // 1: the materials from the material map (0: worked out here, the old way)
};
#include "clouds.glsl"
#define SKY_OCCLUSION_NO_GROUND
#define SKY_BOUNCE
#include "sky_occlusion.glsl"
#define LIGHT_BINDING 16
#include "lights.glsl"
#include "decals.glsl"

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec4 shadow_position;
layout(location = 2) in vec4 near_shadow_position;

layout(location = 0) out vec4 fragment_color;
#include "scene_depth.glsl"

// ---------------------------------------------------------------------------
// Small hash based value noise, used to break up borders and repetition.
// Not tileable and not pretty up close, but cheap and good enough for that.

float hash_2d(vec2 position)
{
	vec3 value = fract(vec3(position.xyx) * 0.1031);

	value += dot(value, value.yzx + 33.33);
	return fract((value.x + value.y) * value.z);
}

float value_noise(vec2 position)
{
	vec2 cell = floor(position);
	vec2 fraction = fract(position);
	vec2 blend = fraction * fraction * (3.0 - 2.0 * fraction);

	return mix(
		mix(hash_2d(cell), hash_2d(cell + vec2(1.0, 0.0)), blend.x),
		mix(hash_2d(cell + vec2(0.0, 1.0)), hash_2d(cell + vec2(1.0, 1.0)), blend.x),
		blend.y);
}

#include "terrain_materials.glsl"

// ---------------------------------------------------------------------------
// Soil weights.
//
// The soil grid is binary per cell (each cell is exactly one soil). The C
// code blurs it into soil_weight_texture (sand, earth, clay, rock weights,
// a few cells per texel), so one filtered lookup gives soft borders. The
// lookup position is warped by noise so borders between soils wander
// organically instead of following the grid.

vec4 soil_weights(vec2 grid_position)
{
	vec2 warped = soil_warped_position(grid_position, soil_blend_cells);
	// Cell centers sit at whole grid coordinates
	vec4 weights = texture(soil_weight_texture, (warped + 0.5) / float(map_size));

	return weights / max(weights.x + weights.y + weights.z + weights.w, 1e-3);
}

// Fraction of sunlight getting through the moving clouds. The cloud layer
// is a flat sheet at cloud_motion.z meters; the point is traced towards the
// sun up to that sheet, so cloud shadows land where they really would on
// slopes instead of straight below the cloud.
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

// ---------------------------------------------------------------------------
// Material sampling with variants and triplanar projection.

// Screen space derivatives of the world position, taken once at the top of
// main() while every pixel of the 2x2 quad is still running the same code.
// All texture lookups below happen inside branches that differ from pixel
// to pixel (which materials are present, which projections are active), and
// there the implicit derivatives texture() relies on are undefined; that
// shows up as sparkles and seams at material borders. Passing explicit
// gradients to textureGrad makes the mip level choice exact everywhere.
vec3 position_ddx;
vec3 position_ddy;

// Picks one or two variant layers of a material from low frequency noise
// and returns the blended sample from the given array (or, fixed_layer 0 or
// more, that layer alone). uv_ddx / uv_ddy are the screen space derivatives
// of uv.
vec4 sample_variants(sampler2DArray array, int material, vec2 uv, vec2 uv_ddx, vec2 uv_ddy, float variant_noise, int fixed_layer)
{
	int count = material_layers[material].y;
	int first = material_layers[material].x;

	if (fixed_layer >= 0)
	{
		return textureGrad(array, vec3(uv, float(fixed_layer)), uv_ddx, uv_ddy);
	}
	if (count <= 1)
	{
		return textureGrad(array, vec3(uv, float(first)), uv_ddx, uv_ddy);
	}
	float position = variant_noise * float(count);
	int index_a = min(int(position), count - 1);
	int index_b = min(index_a + 1, count - 1);
	float blend = smoothstep(0.3, 0.7, fract(position));

	// Most of the time only one variant shows; skip the other lookup
	if (blend <= 0.001)
	{
		return textureGrad(array, vec3(uv, float(first + index_a)), uv_ddx, uv_ddy);
	}
	if (blend >= 0.999)
	{
		return textureGrad(array, vec3(uv, float(first + index_b)), uv_ddx, uv_ddy);
	}
	return mix(textureGrad(array, vec3(uv, float(first + index_a)), uv_ddx, uv_ddy),
		textureGrad(array, vec3(uv, float(first + index_b)), uv_ddx, uv_ddy), blend);
}

// Samples one scale of a material and returns color (rgb, sRGB) + texture
// height (a); world_normal_out receives the normal mapped surface normal.
//
// On flat ground only the top-down projection is used. On steep slopes the
// texture is also projected from the sides (triplanar), so cliffs and river
// banks do not show stretched streaks. Normal maps are combined with the
// geometry normal using the "UDN" blend per projection, which needs no
// tangent vectors at all. side_shift (meters) moves the side projections up
// and down (a rock's beds dipping and faulted: strata_shift)
vec4 sample_material_scale(int material, float repeats, vec3 position, vec3 normal, vec3 projection_weights, float variant_noise,
	int fixed_layer, float side_shift, out vec3 world_normal_out)
{
	vec4 color_height = vec4(0.0);
	vec3 blended_normal = vec3(0.0);

	if (projection_weights.y > 0.0)
	{
		vec2 uv = position.xz * repeats;
		vec2 uv_ddx = position_ddx.xz * repeats;
		vec2 uv_ddy = position_ddy.xz * repeats;
		vec4 color = sample_variants(material_color_array, material, uv, uv_ddx, uv_ddy, variant_noise, fixed_layer);
		vec3 tangent_normal = sample_variants(material_normal_array, material, uv, uv_ddx, uv_ddy, variant_noise, fixed_layer).xyz * 2.0 - 1.0;

		color_height += color * projection_weights.y;
		blended_normal += vec3(tangent_normal.x + normal.x, normal.y, tangent_normal.y + normal.z) * projection_weights.y;
	}
	if (projection_weights.x > 0.0)
	{
		vec2 uv = vec2(position.z, position.y + side_shift) * repeats;
		vec2 uv_ddx = position_ddx.zy * repeats;
		vec2 uv_ddy = position_ddy.zy * repeats;
		vec4 color = sample_variants(material_color_array, material, uv, uv_ddx, uv_ddy, variant_noise, fixed_layer);
		vec3 tangent_normal = sample_variants(material_normal_array, material, uv, uv_ddx, uv_ddy, variant_noise, fixed_layer).xyz * 2.0 - 1.0;

		color_height += color * projection_weights.x;
		blended_normal += vec3(normal.x, tangent_normal.y + normal.y, tangent_normal.x + normal.z) * projection_weights.x;
	}
	if (projection_weights.z > 0.0)
	{
		vec2 uv = vec2(position.x, position.y + side_shift) * repeats;
		vec2 uv_ddx = position_ddx.xy * repeats;
		vec2 uv_ddy = position_ddy.xy * repeats;
		vec4 color = sample_variants(material_color_array, material, uv, uv_ddx, uv_ddy, variant_noise, fixed_layer);
		vec3 tangent_normal = sample_variants(material_normal_array, material, uv, uv_ddx, uv_ddy, variant_noise, fixed_layer).xyz * 2.0 - 1.0;

		color_height += color * projection_weights.z;
		blended_normal += vec3(tangent_normal.x + normal.x, tangent_normal.y + normal.y, normal.z) * projection_weights.z;
	}
	world_normal_out = normalize(blended_normal);
	return color_height;
}

// A material at two scales: the macro scale (large repeats) everywhere, and
// the fine detail scale near the camera, fading out with distance before it
// would turn into mush or shimmer. The macro scale is projected from above
// (macro_weights), a rock's from the sides as well. A material with a macro
// texture of its own (macro_texture: a rock's beds) shows that from afar,
// and up close its variants (a massive stone) shaded by it, its relief in
// their normals: the beds seen through the stone
vec4 sample_material_dry(int material, vec3 position, vec3 normal, vec3 projection_weights, vec3 macro_weights, float variant_noise,
	float detail_weight, float side_shift, out vec3 world_normal_out)
{
	int macro_layer = material_layers[material].z;
	vec3 macro_normal;
	vec4 macro = sample_material_scale(material, material_scales[material].y, position, normal,
		macro_weights, variant_noise, macro_layer, side_shift, macro_normal);

	if (detail_weight <= 0.01)
	{
		world_normal_out = macro_normal;
		return macro;
	}

	vec3 detail_normal;
	vec4 detail = sample_material_scale(material, material_scales[material].x, position, normal,
		projection_weights, variant_noise, -1, 0.0, detail_normal);

	if (macro_layer >= 0)
	{
		// (its average: the macro texture's smallest level)
		vec3 average = textureLod(material_color_array, vec3(0.5, 0.5, float(macro_layer)), 16.0).rgb;
		float shade = dot(macro.rgb, vec3(0.3, 0.59, 0.11)) / max(dot(average, vec3(0.3, 0.59, 0.11)), 0.02);

		detail.rgb *= mix(1.0, shade, 0.5);
		detail_normal = normalize(detail_normal + (macro_normal - normal) * 0.8);
	}
	world_normal_out = normalize(mix(macro_normal, detail_normal, detail_weight));
	return mix(macro, detail, detail_weight);
}

// The same, soaked: a material with a wet texture of its own (clay: its
// cracks closed into smooth mud while it is wet; they open again as it
// dries) shows that as far as wet says (0..1), at both scales
vec4 sample_material(int material, vec3 position, vec3 normal, vec3 projection_weights, vec3 macro_weights, float variant_noise,
	float detail_weight, float side_shift, float wet, out vec3 world_normal_out)
{
	int wet_layer = int(floor(material_scales[material].z + 0.5));
	vec4 dry = vec4(0.0);
	vec3 dry_normal = normal;

	if (wet_layer < 0 || wet < 0.999)
	{
		dry = sample_material_dry(material, position, normal, projection_weights, macro_weights, variant_noise, detail_weight, side_shift,
			dry_normal);
	}
	if (wet_layer < 0 || wet <= 0.001)
	{
		world_normal_out = dry_normal;
		return dry;
	}
	vec3 macro_normal;
	vec4 soaked = sample_material_scale(material, material_scales[material].y, position, normal, macro_weights, variant_noise, wet_layer,
		side_shift, macro_normal);
	vec3 soaked_normal = macro_normal;

	if (detail_weight > 0.01)
	{
		vec3 detail_normal;
		vec4 detail = sample_material_scale(material, material_scales[material].x, position, normal, projection_weights, variant_noise,
			wet_layer, 0.0, detail_normal);

		soaked = mix(soaked, detail, detail_weight);
		soaked_normal = normalize(mix(macro_normal, detail_normal, detail_weight));
	}
	world_normal_out = normalize(mix(dry_normal, soaked_normal, wet));
	return mix(dry, soaked, wet);
}

// How much of an overlay (snow, petals: ground_overlay.h) covers the spot:
// where its mask (its normal map's alpha: low first) is below its amount,
// with a thin soft edge (wider far off, where the mask is a blur)
float overlay_coverage(int material, float amount, vec3 position, float detail_weight)
{
	if (amount <= 0.002)
	{
		return 0.0;
	}
	float layer = float(material_layers[material].x);
	float repeats = material_scales[material].x;
	float macro_repeats = material_scales[material].y;
	float mask = textureGrad(material_normal_array, vec3(position.xz * macro_repeats, layer), position_ddx.xz * macro_repeats,
		position_ddy.xz * macro_repeats).a;

	if (detail_weight > 0.01)
	{
		mask = mix(mask, textureGrad(material_normal_array, vec3(position.xz * repeats, layer), position_ddx.xz * repeats,
			position_ddy.xz * repeats).a, detail_weight);
	}
	float width = mix(0.2, 0.05, detail_weight);
	float level = amount * (1.0 + 2.0 * width) - width;

	return smoothstep(mask - width, mask + width, level);
}

// An overlay over the ground as far as it covers it: its texture from above,
// at one scale (its detail up close, its macro scale far off: it is cheap)
void apply_overlay(int material, float amount, vec3 position, vec3 normal, float detail_weight, float variant_noise, inout vec3 albedo,
	inout float roughness, inout vec3 shading_normal)
{
	float coverage = overlay_coverage(material, amount, position, detail_weight);

	if (coverage > 0.001)
	{
		vec3 overlay_normal;
		float repeats = detail_weight > 0.5 ? material_scales[material].x : material_scales[material].y;
		vec4 overlay_sample = sample_material_scale(material, repeats, position, normal, vec3(0.0, 1.0, 0.0), variant_noise, -1, 0.0,
			overlay_normal);

		albedo = mix(albedo, pow(overlay_sample.rgb, vec3(2.2)), coverage);
		roughness = mix(roughness, clamp(overlay_sample.a, 0.04, 1.0), coverage);
		shading_normal = normalize(mix(shading_normal, overlay_normal, coverage));
	}
}

// ---------------------------------------------------------------------------
// Rock faces: how far the beds stand off level here (meters up the side
// projections: they dip, wave and are offset at faults, so they do not ring
// a mountain as contour lines), and the big relief of its steep faces

float strata_shift(vec3 position)
{
	vec2 at = position.xz;
	// A broad dip and a slower wave (a slope of up to a quarter)
	float shift = (value_noise(at / 160.0) - 0.5) * 80.0 + (value_noise(at / 45.0 + 17.0) - 0.5) * 12.0;
	// Faults: blocks of some tens of meters, each moved up or down its own
	// way (their borders wander)
	vec2 warp = vec2(value_noise(at / 60.0 + 3.0), value_noise(at / 60.0 + 41.0)) - 0.5;
	vec2 block = floor(at / 38.0 + warp * 0.8);

	shift += (hash_2d(block) - 0.5) * 9.0;
	return shift;
}

// How low a rock face is here in its relief (1 out on a buttress .. 0 down a
// gully; 0.5 without one): where snow lies longer and water runs
float rock_relief_height(vec3 position, vec3 normal)
{
	int relief_layer = material_layers[MATERIAL_ROCK].w;

	if (relief_layer < 0)
	{
		return 0.5;
	}
	float repeats = material_patches[MATERIAL_ROCK].w;
	float weight_x = normal.x * normal.x;
	float weight_z = normal.z * normal.z;
	float total = max(weight_x + weight_z, 1e-4);
	float along_x = textureGrad(material_normal_array, vec3(position.zy * repeats, float(relief_layer)),
		position_ddx.zy * repeats, position_ddy.zy * repeats).a;
	float along_z = textureGrad(material_normal_array, vec3(position.xy * repeats, float(relief_layer)),
		position_ddx.xy * repeats, position_ddy.xy * repeats).a;

	return (along_x * weight_x + along_z * weight_z) / total;
}

// A rock face weathered, as seen from afar: green where something grows on
// it (its flatter bits, a ledge's top, a buttress's; more on its shady,
// northern side), dark streaks down it where the water runs (more down the
// gullies), rust and pale lichen over tens of meters, its whole tone
// changing from one face to the next. albedo linear; amount: how much of the
// pixel is rock
vec3 rock_weathering(vec3 albedo, vec3 position, vec3 normal, vec3 bent, float relief_height, float amount)
{
	vec2 at = position.xz;
	// (from the grass's own texture: its average)
	vec3 green = pow(textureLod(material_color_array, vec3(0.5, 0.5, float(material_layers[MATERIAL_GRASS].x)), 16.0).rgb, vec3(2.2)) * 0.8;
	float north = clamp(-normal.z, 0.0, 1.0);
	float flat_bits = smoothstep(0.5, 0.8, bent.y);
	float patches = smoothstep(0.45, 0.75, value_noise(at / 7.0 + position.y / 9.0) * 0.6 + value_noise(at / 23.0 + 5.0) * 0.6
		+ north * 0.25 - 0.1);
	float growing = flat_bits * patches * (1.0 - smoothstep(0.75, 1.0, position.y / max(max_height, 1.0)));
	// Streaks: long down the face, narrow across it
	float across = abs(normal.x) > abs(normal.z) ? position.z : position.x;
	float streak = smoothstep(0.55, 0.8, value_noise(vec2(across / 1.8, position.y / 22.0) + 11.0))
		* (0.5 + 0.8 * (1.0 - relief_height));
	float rust = smoothstep(0.55, 0.85, value_noise(at / 60.0 + 23.0));
	float lichen = smoothstep(0.6, 0.85, value_noise(at / 35.0 + 47.0));
	float tone = 0.85 + 0.3 * value_noise(at / 110.0 + 71.0);
	vec3 weathered = albedo * tone;

	weathered *= mix(vec3(1.0), vec3(1.25, 0.95, 0.72), rust * 0.45);
	weathered = mix(weathered, weathered * vec3(1.35, 1.38, 1.25), lichen * 0.35);
	weathered *= 1.0 - 0.4 * clamp(streak, 0.0, 1.0);
	weathered = mix(weathered, green, growing * 0.85);
	return mix(albedo, weathered, amount);
}

// Its relief (gullies and buttresses down its steep faces: a big normal map,
// its height in alpha, projected from the sides) and its ledges (the beds
// standing out a little, each a short steep step and a long slope); the
// normal bent by them and how much light reaches there (the gullies and
// under the ledges darker). amount: how much of it shows (0..1)
vec3 rock_relief(vec3 position, vec3 normal, vec3 shading_normal, float shift, float amount, out float cavity)
{
	int relief_layer = material_layers[MATERIAL_ROCK].w;
	float steep = 1.0 - smoothstep(0.55, 0.85, normal.y);
	vec3 bent = shading_normal;

	cavity = 1.0;
	amount *= steep;
	if (amount <= 0.001)
	{
		return shading_normal;
	}
	if (relief_layer >= 0)
	{
		float repeats = material_patches[MATERIAL_ROCK].w;
		float weight_x = normal.x * normal.x;
		float weight_z = normal.z * normal.z;
		float total = max(weight_x + weight_z, 1e-4);
		vec4 along_x = textureGrad(material_normal_array, vec3(position.zy * repeats, float(relief_layer)),
			position_ddx.zy * repeats, position_ddy.zy * repeats);
		vec4 along_z = textureGrad(material_normal_array, vec3(position.xy * repeats, float(relief_layer)),
			position_ddx.xy * repeats, position_ddy.xy * repeats);
		vec2 tangent_x = along_x.xy * 2.0 - 1.0;
		vec2 tangent_z = along_z.xy * 2.0 - 1.0;
		vec3 push = (vec3(0.0, tangent_x.y, tangent_x.x) * weight_x + vec3(tangent_z.x, tangent_z.y, 0.0) * weight_z) / total;
		float height = (along_x.a * weight_x + along_z.a * weight_z) / total;

		bent = normalize(bent + push * amount);
		cavity *= mix(1.0, 0.55 + 0.6 * height, amount);
	}
	// Ledges every few meters up the (shifted) beds; gone where a pixel
	// spans too much of one (no shimmer)
	{
		float period = 6.0;
		float rise = 0.85;
		float beds = (position.y + shift) / period;
		float phase = fract(beds);
		float footprint = max(abs(position_ddx.y), abs(position_ddy.y)) / period;
		// (not everywhere: in stretches, where the bed is a hard one)
		float there = smoothstep(0.35, 0.65, value_noise(vec2(position.x + position.z, floor(beds) * 7.3) / vec2(25.0, 1.0)));
		float fade = (1.0 - smoothstep(0.08, 0.3, footprint)) * there;
		// (meters out per meter up: the long slope up, the short step down)
		float slope = mix(0.5 / (rise * period), -0.5 / ((1.0 - rise) * period), smoothstep(rise - 0.04, rise + 0.02, phase))
			* (1.0 - smoothstep(0.97, 1.0, phase));
		vec3 up = normalize(vec3(0.0, 1.0, 0.0) - normal * normal.y + vec3(0.0, 1e-4, 0.0));

		bent = normalize(bent - up * slope * fade * amount);
		cavity *= mix(1.0, 0.9 + 0.1 * smoothstep(0.0, 0.12, phase), fade * amount);
	}
	return bent;
}

// ---------------------------------------------------------------------------
// Water tile overlay (F3): active tiles red, active with mud orange,
// sleeping blue, sleeping with mud violet; tile borders as thin lines.

// edge_width: screen space size of a pixel in tile units, taken at the
// top of main() where derivatives are well defined
vec3 apply_tile_overlay(vec3 color, vec3 position, vec2 edge_width)
{
	if (tile_overlay == 0)
	{
		return color;
	}
	vec2 tile_position = (position.xz / cell_size + 0.5) / 64.0;
	ivec2 tile = clamp(ivec2(floor(tile_position)), ivec2(0), ivec2(map_size / 64 - 1));
	uint state = uint(texelFetch(tile_texture, tile, 0).r * 255.0 + 0.5);
	vec3 tint = vec3(0.0);
	float amount = 0.0;

	if (state == 1u) { tint = vec3(0.15, 0.35, 1.0); amount = 0.30; }
	if (state == 2u) { tint = vec3(1.0, 0.1, 0.05); amount = 0.45; }
	if (state == 3u) { tint = vec3(1.0, 0.55, 0.0); amount = 0.45; }
	if (state == 4u) { tint = vec3(0.6, 0.2, 0.9); amount = 0.35; }
	color = mix(color, tint, amount);

	// Borders: distance to the nearest tile edge, a pixel or so wide
	vec2 edge_distance = abs(fract(tile_position + 0.5) - 0.5);
	float border = max(1.0 - smoothstep(0.0, edge_width.x, edge_distance.x), 1.0 - smoothstep(0.0, edge_width.y, edge_distance.y));

	return mix(color, vec3(0.0), border * 0.6);
}

// ---------------------------------------------------------------------------
// Navigation overlay (F8): what the first actor type finds in each cell
// (see navigation_overlay_t): objects in the way red, too little room
// orange, too steep on every side yellow (on some sides faint yellow: it
// can only go along the slope), an object's top to stand on blue, water it
// cannot pass violet, the actors' paths green with white waypoints.
// Cell borders as faint lines when close.

vec3 apply_navigation_overlay(vec3 color, vec3 position, vec2 edge_width)
{
	if (navigation_overlay == 0)
	{
		return color;
	}
	vec2 cell_position = position.xz / navigation_cell_meters.x;
	int cells = int(navigation_cell_meters.y);
	ivec2 cell = clamp(ivec2(floor(cell_position)), ivec2(0), ivec2(cells - 1));
	uint state = uint(texelFetch(navigation_texture, cell, 0).r * 255.0 + 0.5);
	vec3 tint = vec3(0.0);
	float amount = 0.0;

	if (state == 1u) { tint = vec3(1.0, 0.05, 0.05); amount = 0.55; }
	if (state == 2u) { tint = vec3(1.0, 0.5, 0.0); amount = 0.45; }
	if (state == 3u) { tint = vec3(1.0, 0.95, 0.1); amount = 0.40; }
	if (state == 4u) { tint = vec3(0.1, 0.4, 1.0); amount = 0.50; }
	if (state == 5u) { tint = vec3(0.6, 0.1, 0.9); amount = 0.45; }
	if (state == 6u) { tint = vec3(0.1, 1.0, 0.2); amount = 0.65; }
	if (state == 7u) { tint = vec3(1.0, 1.0, 1.0); amount = 0.85; }
	if (state == 8u) { tint = vec3(1.0, 0.95, 0.1); amount = 0.15; }
	if (state == 9u) { tint = vec3(0.1, 0.9, 0.9); amount = 0.45; }
	color = mix(color, tint, amount);

	vec2 edge_distance = abs(fract(cell_position + 0.5) - 0.5);
	float border = max(1.0 - smoothstep(0.0, edge_width.x, edge_distance.x), 1.0 - smoothstep(0.0, edge_width.y, edge_distance.y));
	// Only when a cell is several pixels wide
	border *= 1.0 - smoothstep(0.05, 0.2, max(edge_width.x, edge_width.y));

	return mix(color, vec3(0.0), border * 0.3);
}

// ---------------------------------------------------------------------------

vec3 debug_material_color(int material)
{
	const vec3 colors[MATERIAL_COUNT] = vec3[](
		vec3(0.2, 0.7, 0.15),   // grass
		vec3(0.55, 0.38, 0.2),  // dirt
		vec3(0.5, 0.5, 0.52),   // rock
		vec3(0.9, 0.82, 0.5),   // sand
		vec3(0.75, 0.42, 0.3),  // clay
		vec3(1.0, 1.0, 1.0),    // snow
		vec3(0.35, 0.3, 0.25),  // silt
		vec3(0.45, 0.25, 0.1),  // needles
		vec3(0.85, 0.5, 0.1),   // leaves
		vec3(0.6, 0.55, 0.4),   // trampled
		vec3(1.0, 0.6, 0.75),   // petals
		vec3(0.45, 0.35, 0.25)); // trampled dirt

	return colors[clamp(material, 0, MATERIAL_COUNT - 1)];
}

vec3 debug_soil_color(vec4 weights)
{
	return weights.x * vec3(0.85, 0.75, 0.40)
		+ weights.y * vec3(0.45, 0.32, 0.20)
		+ weights.z * vec3(0.75, 0.38, 0.22)
		+ weights.w * vec3(0.55, 0.55, 0.58);
}

void main()
{
	// Must come first, in uniform control flow (see position_ddx above)
	position_ddx = dFdx(world_position);
	position_ddy = dFdy(world_position);
	scene_depth_write();
	vec2 tile_edge_width = fwidth((world_position.xz / cell_size + 0.5) / 64.0) * 1.2;
	vec2 navigation_edge_width = fwidth(world_position.xz / max(navigation_cell_meters.x, 0.001)) * 1.2;

	// Global variation and clouds are sampled here, before any branch, so
	// the implicit derivatives texture() needs are well defined
	float variation_large = texture(variation_texture, world_position.xz * variation_parameters.x).r;
	float variation_small = texture(variation_texture, world_position.xz * variation_parameters.x * 3.83 + 0.61).r;
	float clouds = cloud_light(world_position, sun_direction);
	// Cell centers sit at whole grid coordinates, texel centers half a texel in
	vec4 surface = texture(surface_texture, (world_position.xz / cell_size + 0.5) / float(map_size));

	// Normals per pixel, from the texture: the same detail at any level of
	// detail of the mesh. The wide "material" normal decides the materials
	// (on fine grids the per-cell slope is noisy and would make grass and
	// rock borders follow every little bump); lighting uses the sharp one.
	vec4 encoded_normals = texture(normal_texture, (world_position.xz / cell_size + 0.5) / float(map_size)) * 2.0 - 1.0;
	vec3 normal = normalize(vec3(encoded_normals.x, sqrt(max(1.0 - dot(encoded_normals.xy, encoded_normals.xy), 0.0)), encoded_normals.y));
	vec3 smooth_normal = normalize(vec3(encoded_normals.z, sqrt(max(1.0 - dot(encoded_normals.zw, encoded_normals.zw), 0.0)), encoded_normals.w));
	vec2 grid_position = world_position.xz / cell_size;
	vec4 soil = soil_weights(grid_position);
	float slope = 1.0 - smooth_normal.y;                // 0 flat .. 1 vertical
	vec2 sun_horizontal = normalize(sun_direction.xz + vec2(1e-5));
	// Positive on slopes facing the sun, negative on shady ones; grows with
	// steepness because the horizontal part of the normal does
	float sun_facing = dot(smooth_normal.xz, sun_horizontal);
	float detail_noise = value_noise(world_position.xz * 0.11);
	float broad_noise = value_noise(world_position.xz * 0.013);
	// A rock face's relief here (gullies hold snow longer)
	float relief_height = soil.w > 0.01 || slope > 0.3 ? rock_relief_height(world_position, normal) : 0.5;
	float weights[MATERIAL_COUNT];

	if (debug_view == 1)
	{
		vec3 lit = debug_soil_color(soil) * (0.35 + 0.65 * max(dot(normal, sun_direction), 0.0));

		fragment_color = vec4(lit, 1.0);
		return;
	}

#ifdef MATERIAL_MAP
	// The map's (the CPU's: where, what), then what the water did (the bare
	// bedrock, the silt), then the snow by its line
	int slot_ids[MATERIAL_SLOTS];
	float slot_weights[MATERIAL_SLOTS];

	terrain_map_slots(material_map_texture, grid_position, float(map_size), slot_ids, slot_weights);
	terrain_slot_layer(slot_ids, slot_weights, 4, surface.b);
	terrain_slot_layer(slot_ids, slot_weights, 5, clamp(surface.r * (0.8 + 0.6 * detail_noise), 0.0, 1.0));
	terrain_slot_layer(slot_ids, slot_weights, 6, terrain_snow_line_amount(world_position, slope, sun_facing, broad_noise, relief_height,
		vec3(snow_height_fraction, snow_aspect_shift, max_height)));
	terrain_slots_jitter(slot_ids, slot_weights, world_position.xz);
#else
	terrain_material_weights(world_position, soil, surface, slope, sun_facing, detail_noise, broad_noise, variation_large, variation_small,
		relief_height, slope_ranges, vec3(snow_height_fraction, snow_aspect_shift, max_height), weights);
#endif
	if (debug_view == 2)
	{
		// The materials, flat colors
		int chosen_debug[3];
		float contributions_debug[3];
		vec3 color = vec3(0.0);

#ifdef MATERIAL_MAP
		terrain_slot_patches(slot_ids, slot_weights, world_position.xz, max(length(position_ddx), length(position_ddy)), material_patches,
			chosen_debug, contributions_debug);
#else
		terrain_material_patches(weights, world_position.xz, max(length(position_ddx), length(position_ddy)), material_patches,
			chosen_debug, contributions_debug);
#endif
		for (int k = 0; k < 3; k++)
		{
			color += chosen_debug[k] >= 0 ? debug_material_color(chosen_debug[k]) * contributions_debug[k] : vec3(0.0);
		}
		fragment_color = vec4(color * (0.35 + 0.65 * max(dot(normal, sun_direction), 0.0)), 1.0);
		return;
	}

	// Triplanar projection weights: sharpened so only really steep faces
	// get side projections
	vec3 projection_weights = pow(abs(normal), vec3(4.0));
	projection_weights /= projection_weights.x + projection_weights.y + projection_weights.z;
	if (normal.y > 0.8)
	{
		projection_weights = vec3(0.0, 1.0, 0.0);
	}
	projection_weights = step(vec3(0.02), projection_weights) * projection_weights;
	projection_weights /= projection_weights.x + projection_weights.y + projection_weights.z;
	// A rock's: from the sides down to gentler slopes (seen from above its
	// beds would lie on the slope like paving), the two sides by the
	// horizontal normal alone (no seams where they meet)
	vec3 rock_weights;
	{
		float top = smoothstep(0.6, 0.85, normal.y);
		float side_x = normal.x * normal.x;
		float side_z = normal.z * normal.z;
		float sides = max(side_x + side_z, 1e-5);

		rock_weights = vec3((1.0 - top) * side_x / sides, top, (1.0 - top) * side_z / sides);
		rock_weights = step(vec3(0.02), rock_weights) * rock_weights;
		rock_weights /= rock_weights.x + rock_weights.y + rock_weights.z;
	}

	// The materials that show here, and how much of each (see
	// terrain_materials.glsl): only those are read (each costs several
	// texture lookups)
	int chosen[3];
	float contributions[3];
	vec4 samples[3];
	vec3 sample_normals[3];
	float variant_noise = value_noise(world_position.xz * 0.02);
	float view_distance = length(world_position - eye_position);
	float detail_weight = 1.0 - smoothstep(detail_fade.x, detail_fade.y, view_distance);
	float pixel_meters = max(length(position_ddx), length(position_ddy));

#ifdef MATERIAL_MAP
	terrain_slot_patches(slot_ids, slot_weights, world_position.xz, pixel_meters, material_patches, chosen, contributions);
#else
	terrain_material_patches(weights, world_position.xz, pixel_meters, material_patches, chosen, contributions);
#endif
	for (int k = 0; k < 3; k++)
	{
		samples[k] = vec4(0.0);
		sample_normals[k] = normal;
	}

	vec3 albedo = vec3(0.0);
	vec3 shading_normal = vec3(0.0);
	float total = 0.0;
	float rock_shift = 0.0;
	float rock_amount = 0.0;
	// (the textures' alpha: 0 a mirror .. 1 matte)
	float roughness = 0.0;
	// How far a soaked material shows its wet texture: under water at once,
	// else as it soaks (clay soaks slowly), in patches as it dries (its
	// edges first)
	float wet_show = max(surface.a, smoothstep(0.35, 0.7, surface.g + (value_noise(world_position.xz * 0.6) - 0.5) * 0.25
		+ (detail_noise - 0.5) * 0.15));

	for (int k = 0; k < 3; k++)
	{
		float contribution = chosen[k] >= 0 ? contributions[k] : 0.0;

		if (contribution <= 0.001)
		{
			continue;
		}
		{
			bool rock = chosen[k] == MATERIAL_ROCK;

			if (rock)
			{
				rock_shift = strata_shift(world_position);
				rock_amount = contribution;
			}
			samples[k] = sample_material(chosen[k], world_position, normal, rock ? rock_weights : projection_weights,
				rock ? rock_weights : vec3(0.0, 1.0, 0.0), variant_noise, detail_weight, rock ? rock_shift : 0.0, wet_show, sample_normals[k]);
		}
		albedo += pow(samples[k].rgb, vec3(2.2)) * contribution;
		roughness += samples[k].a * contribution;
		shading_normal += sample_normals[k] * contribution;
		total += contribution;
	}
	albedo /= max(total, 1e-4);
	roughness /= max(total, 1e-4);
	shading_normal = normalize(shading_normal + normal * 1e-3);
	// A rock face's big relief: gullies, buttresses, ledges
	if (rock_amount > 0.0)
	{
		float cavity;

		shading_normal = rock_relief(world_position, normal, shading_normal, rock_shift, rock_amount / max(total, 1e-4), cavity);
		albedo *= cavity;
		albedo = rock_weathering(albedo, world_position, normal, shading_normal, relief_height, rock_amount / max(total, 1e-4));
	}

	// Global variation over every material: brightness from two unrelated
	// scales of the variation texture, plus a slight shift between a drier,
	// yellower tint and a lusher, greener one. Hides repetition and makes
	// large areas of one material look less flat.
	{
		float variation = (variation_large - 0.5) * 0.7 + (variation_small - 0.5) * 0.3;
		vec3 dry_tint = vec3(1.08, 1.02, 0.86);
		vec3 lush_tint = vec3(0.90, 1.02, 0.94);

		albedo *= 1.0 + variation * variation_parameters.y * 2.0;
		albedo *= mix(vec3(1.0), variation > 0.0 ? dry_tint : lush_tint, min(abs(variation) * 2.0, 1.0) * variation_parameters.z);
	}

	// Wet ground is darker (water fills the pores and stops the light
	// scattering back out)
	albedo *= 1.0 - 0.45 * surface.g;

	// Wet ground is smoother: the water in its pores (a wet texture has its
	// own roughness already), water standing on it nearly a mirror
	roughness = mix(roughness, min(roughness, 0.35), surface.g * 0.7);
	roughness = mix(roughness, 0.06, surface.a);
	roughness = clamp(roughness, 0.04, 1.0);

#ifdef MATERIAL_MAP
	// The overlays over all of it (not blended: as far as each covers; the
	// snow over the petals). Most of the ground has none: one lookup.
	{
		vec2 overlay = texture(overlay_texture, (world_position.xz / cell_size + 0.5) / float(map_size)).rg;

		if (overlay.g > 0.002)
		{
			apply_overlay(MATERIAL_PETALS, overlay.g, world_position, normal, detail_weight, variant_noise, albedo, roughness, shading_normal);
		}
		if (overlay.r > 0.002)
		{
			apply_overlay(MATERIAL_SNOW, overlay.r, world_position, normal, detail_weight, variant_noise, albedo, roughness, shading_normal);
		}
	}
#endif

	// The decals (a crater's scorch, mud, a bullet's mark), under the
	// gusts' sheen
	apply_decals(world_position, normal, albedo, shading_normal, roughness);

	// A gust rolling over grass: a paler wave (grass.vert's), as far as
	// this is grass
	if (scene_gusts.w > 0.0)
	{
		float sheen = gust_sheen_at(world_position.xz);

		if (sheen > 0.0)
		{
#ifdef MATERIAL_MAP
			float grass_share = 0.0;
			float all_share = 0.0;

			for (int k = 0; k < MATERIAL_SLOTS; k++)
			{
				grass_share += slot_ids[k] == MATERIAL_GRASS ? slot_weights[k] : 0.0;
				all_share += slot_weights[k];
			}
			grass_share /= max(all_share, 1e-4);
#else
			float grass_share = clamp(weights[MATERIAL_GRASS], 0.0, 1.0);
#endif
			albedo = mix(albedo, albedo * vec3(1.55, 1.6, 1.35) + vec3(0.02, 0.026, 0.016), clamp(scene_gusts.w * sheen * 0.75 * grass_share, 0.0, 1.5));
		}
	}

	// Lighting: sun (blocked by shadows) plus hemisphere ambient (sky
	// above, warm bounce below)
	float sun_light = sun_visibility(shadow_position, near_shadow_position) * clouds;
	float facing_sun = max(dot(shading_normal, sun_direction), 0.0);
	float diffuse = facing_sun * sun_light;
	vec3 ground_bounce = ground_bounce_color(1.0);
	// (the sky it sees: less in hollows, under trees, round rocks)
	float sky_raw = texture(sky_occlusion_texture, (world_position.xz / cell_size + 0.5) / float(map_size)).r;
	float sky_share = sky_seen(sky_raw);
	vec3 ambient = mix(ground_bounce, sky_color, shading_normal.y * 0.5 + 0.5) * ambient_intensity * sky_share
		+ bounce_light(world_position, shading_normal, 1.0 - sky_raw);
	vec3 lit = albedo * (sun_color * diffuse + ambient + local_light(world_position, shading_normal, 0.0));

	// The sun's highlight (GGX, Schlick's Fresnel off a dielectric: 4% head
	// on), and the sky mirrored at grazing angles by smooth (wet) ground
	{
		vec3 view = normalize(eye_position - world_position);
		vec3 halfway = normalize(sun_direction + view);
		float facing_view = max(dot(shading_normal, view), 1e-3);
		float facing_halfway = max(dot(shading_normal, halfway), 0.0);
		float alpha = roughness * roughness;
		float alpha_squared = alpha * alpha;
		float spread = facing_halfway * facing_halfway * (alpha_squared - 1.0) + 1.0;
		float distribution = alpha_squared / (3.14159 * spread * spread);
		float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
		float visibility = 1.0 / ((facing_sun * (1.0 - k) + k) * (facing_view * (1.0 - k) + k) * 4.0);
		float fresnel = 0.04 + 0.96 * pow(1.0 - max(dot(view, halfway), 0.0), 5.0);
		float sky_fresnel = 0.04 + 0.96 * pow(1.0 - facing_view, 5.0);
		// (only smooth ground mirrors the sky: wet, under water; a blade's
		// or a stone's sheen is the sun's highlight alone)
		float smooth_amount = 1.0 - smoothstep(0.1, 0.5, roughness);

		lit += sun_color * (distribution * visibility * fresnel * facing_sun * sun_light);
		lit += sky_color * ambient_intensity * sky_fresnel * smooth_amount * sky_share;
	}

	// Brush outline: a ring (a capsule around a line stamp) of constant
	// on-screen width, plus a faint fill
	if (brush_radius > 0.0)
	{
		vec2 segment = brush_line.zw - brush_line.xy;
		float along = clamp(dot(world_position.xz - brush_line.xy, segment) / max(dot(segment, segment), 1e-6), 0.0, 1.0);
		float distance_to_center = length(world_position.xz - (brush_line.xy + segment * along));
		float ring_width = fwidth(distance_to_center) * 1.5;
		float ring = 1.0 - smoothstep(0.0, ring_width, abs(distance_to_center - brush_radius));
		float inside = 1.0 - step(brush_radius, distance_to_center);

		// As bright as the light there is: at night a full white ring would
		// be the brightest thing in sight (and the exposure would follow it)
		float overlay_light = clamp(dot(sun_color + sky_color * ambient_intensity, vec3(0.3, 0.6, 0.1)), 0.02, 1.0);

		lit = mix(lit, vec3(1.0, 0.9, 0.4) * overlay_light, ring * 0.85);
		lit = mix(lit, vec3(1.0, 0.95, 0.7) * overlay_light, inside * 0.08);
	}

	lit = apply_tile_overlay(lit, world_position, tile_edge_width);
	lit = apply_navigation_overlay(lit, world_position, navigation_edge_width);

	// Distance fog towards the sky color
	float fog_amount = haze_amount(world_position);

	lit = mix(lit, sky_color, clamp(fog_amount, 0.0, 1.0));
	lit = apply_underwater(lit, world_position, view_distance, sun_color * diffuse);
	fragment_color = vec4(lit, 1.0);
}
