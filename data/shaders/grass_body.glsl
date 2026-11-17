// The grass' scatter (compute), its two variants: grass.comp (the
// materials worked out per blade) and grass_map.comp (from the material
// map: MATERIAL_MAP). Included by them alone.
#include "common.glsl"
#include "constants.glsl"

// The grass's blades, scattered anew every frame (see grass.h). Nothing is
// kept: each frame, around the eye, every spot of a grid fixed to the world
// (so the blades stay put as the eye moves) is looked at, and those that
// grow grass and are in view are written into the blade buffer
// (grass.vert's instances), with the indirect draw's arguments.
//
// Near the eye the grid is fine; farther, ring after ring, each twice as
// far, it is twice as coarse, and within a ring the blades thin out with
// the square of the distance (and get wider), so that about as many show
// on screen anywhere and the work grows only with the logarithm of the
// distance:
//
//   level 0          the blades out to full_radius, all of the grid
//   level L >= 1     from full_radius * 2^(L-1) to twice that, every
//                    2^(L-1)-th spot of the same grid
//
// Every blade has a threshold of its own, and shows while the share kept
// there, (full_radius / distance)^2, is above it (shrinking into the
// ground as the share nears it): the blades go one by one as the eye
// moves away and the same ones come back, never a whole ring at once. The
// blades on the coarser grids have the lower thresholds, so a ring's
// coarse grid holds every blade that can show there.
//
//   MODE_CLEAR     one thread: the count back to zero
//   MODE_SCATTER   one thread per spot of the level's grid
//   MODE_ARGUMENTS one thread: the draw's arguments
//
// Where grass grows is worked out as terrain.frag works out where it shows
// the grass material (soil, slope, sun, silt, bare rock, cliffs, snow),
// without its finer touches. Water on the ground lays the blades down the
// way it flows (and they stay down where it left silt); deeper than a blade
// by a little, there is none.
//
// What is the same all up a blade is worked out here, once, not seven
// times in grass.vert (once per vertex): its color (the ground's grass
// under it, the terrain's variation, wet, dry, its own shade), the silt
// over it, the sky seen and the light the land round sends back, and the
// sun the shadow maps and the clouds leave it (at half its height, a
// little towards the sun: a blade stands above the ground the maps hold).
// Into the blade's record, packed (BLADE_FLOATS: grass.vert reads them as
// two uvec4s):
//
//   0..2   its foot (x, y, z: float bits)
//   3      half floats: its height, its width (a far one's widened)
//   4      half floats: laid down towards x, z (its length: how far)
//   5      bytes: the way its face looks (0..1 of a turn), the sun left it,
//          the sky seen, the silt
//   6      bytes: its color (sqrt of half of it, linear: the darks keep
//          their steps), unused
//   7      bytes: the light the land round sends back, shared exponent
//          (r, g, b, 2^(e - 128))
//
// Two lists in the one buffer: the near blades from its start (drawn as
// seven vertices), the far ones (past grass_coarse_meters, a blade by
// blade distance: one triangle, three vertices) from its end; their
// counts and draws in counts (MODE_ARGUMENTS)

layout(local_size_x = 256) in;

#define MODE_CLEAR 1
#define MODE_SCATTER 2
#define MODE_ARGUMENTS 3

// Floats per blade (grass.vert reads them as two vec4s)
#define BLADE_FLOATS 8u
// The draws: a strip of seven vertices per near blade (three segments and
// a tip), a triangle per far one
#define BLADE_VERTICES 7u
#define FAR_BLADE_VERTICES 3u

#include "scene.glsl"
// The shadow maps first (shadows.glsl: samplers 0 and 1), then the ground's
#include "shadows.glsl"
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D height_texture;       // R32F: the ground (meters)
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D water_texture;        // RG32F: the water on it (meters)
layout(set = TEXTURE_SET, binding = 4) uniform sampler2D flow_texture;         // RGBA16F: its flow x, z (m/s)
layout(set = TEXTURE_SET, binding = 5) uniform sampler2D soil_weight_texture;  // RGBA8: sand, earth, clay, rock
layout(set = TEXTURE_SET, binding = 6) uniform sampler2D normal_texture;       // RGBA8: surface normal x, z; material normal x, z
layout(set = TEXTURE_SET, binding = 7) uniform sampler2D surface_texture;      // RGBA8: silt, wetness, bare bedrock
layout(set = TEXTURE_SET, binding = 8) uniform sampler2D variation_texture;    // R8: tileable cloudy noise
layout(set = TEXTURE_SET, binding = 9) uniform sampler2D material_map_texture; // RGBA8: the material map (material_map.h)
layout(set = TEXTURE_SET, binding = 10) uniform sampler2D overlay_texture;     // RGBA16F: snow, petals (ground_overlay.h)
layout(set = TEXTURE_SET, binding = 11) uniform sampler2DArray material_color_array;  // the terrain's materials: RGB color
layout(set = TEXTURE_SET, binding = 12) uniform sampler2D sky_occlusion_texture;      // R8 a cell (sky_occlusion.h)
layout(set = TEXTURE_SET, binding = 13) uniform sampler2D sky_layers_texture;         // RGBA8: the four layers in the air
layout(set = TEXTURE_SET, binding = 14) uniform sampler2D sky_bounce_texture;         // RGBA8: what hides the sky
layout(set = TEXTURE_SET, binding = 15) uniform sampler2D cloud_shadow_texture;       // the volumetric clouds' shadows (clouds.glsl)
#define SKY_OCCLUSION_NO_GROUND
#define SKY_BOUNCE
#include "sky_occlusion.glsl"
#define CLOUD_SHADOW_TEXTURE
#define CLOUDS_EXPLICIT_LOD
#include "clouds.glsl"

layout(std430, set = READWRITE_SET, binding = 0) buffer blade_buffer { uint blades[]; };
// The depth pyramid of the frame before (hiz.h): every level, one after
// the other, row by row (a blade behind what was drawn then is left out).
// (In a compute shader's first set the read only buffers come after the
// samplers: binding 16, the sixteen above before it; at 0 it read the
// first sampler's slot, and every blade came out hidden)
layout(std430, set = READONLY_SET, binding = 16) readonly buffer hiz_buffer { float hiz_depths[]; };
// The count of blades written, then the draw's arguments (vertices,
// instances, first vertex, first instance)
layout(std430, set = READWRITE_SET, binding = 1) buffer count_buffer { uint counts[]; };

layout(std140, set = UNIFORM_SET, binding = 1) uniform grass_block
{
	vec4 planes[6];            // the view's frustum (inside: dot(plane.xyz, p) + plane.w >= 0)
	vec4 eye;                  // xyz; w: how far the grass goes (meters)
	vec4 grid;                 // this level's spacing (m), its level, its spots across (its rows: control.w), full_radius (m)
	vec4 grid_origin;          // x, z of its first spot's cell (meters); z: the finest spacing; w: blades the buffer holds
	vec4 map;                  // cells per side, cell size (m), the soils' blend (cells), unused
	vec4 slopes;               // grass from, grass to, cliff from, cliff to (terrain.frag's slope_ranges)
	vec4 terrain;              // snow line (of the highest ground), its shift with the sun, the highest ground (m), 1 / the variation's tiling (m)
	vec4 blade;                // a blade's height (m), its width (m), the sun's x, z (towards it)
	ivec4 control;             // mode, the material map (1: read it), the overlay (1: read it), this level's rows (grid.z its columns)
	vec4 material_patches[MATERIAL_COUNT]; // terrain.frag's: repeats per meter, strength, edge (meters)
	mat4 hiz_view_projection;  // the camera's the pyramid was made with (depth 0..1)
	vec4 hiz_size;             // level 0's width and height, the level count, 1 if it is there
	vec4 look;                 // the blades' color: the grass material's first layer, its macro repeats per meter, the variation's strength, its hue's
	vec4 tint;                 // rgb: multiplies the ground's grass color; w: the share of dry blades
	vec4 coarse;               // x: from how far off (m) a blade is one triangle (0: never); y: how much wider then
	ivec4 hiz_levels[16];      // the pyramid's levels (hiz_level_table): where each starts in hiz_depths, its width, its height
};

// --- Noise, as terrain.frag's ------------------------------------------------

float hash_2d(vec2 position)
{
	vec3 value = fract(vec3(position.xyx) * 0.1031);

	value += dot(value, value.yzx + 33.33);
	return fract((value.x + value.y) * value.z);
}

// The value noise (0..1 at lattice cells), one read: the lattice in the
// variation texture's green (noise_lattice.h), the cell plus the
// smoothstep of where in it, plus half a texel: the bilinear filter blends
// the four corners by the smoothstep's weights. At mip 0 (the derivatives
// of that uv mean nothing). The CPU's: noise_lattice_value
const float NOISE_LATTICE_SIZE = 512.0;

float value_noise(vec2 position)
{
	vec2 cell = floor(position);
	vec2 fraction = position - cell;
	vec2 blend = fraction * fraction * (3.0 - 2.0 * fraction);

	// (the cell wrapped first: far out, the filter keeps its fraction)
	return textureLod(variation_texture, (mod(cell, NOISE_LATTICE_SIZE) + blend + 0.5) / NOISE_LATTICE_SIZE, 0.0).g;
}

#include "terrain_materials.glsl"

// A well mixed hash of a grid spot and a seed (PCG)
uint pcg(uint value)
{
	uint state = value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;

	return (word >> 22u) ^ word;
}

vec4 random4(ivec2 spot, uint level)
{
	uint a = pcg(uint(spot.x) * 73856093u ^ uint(spot.y) * 19349663u ^ level * 83492791u);
	uint b = pcg(a ^ 0x68bc21ebu);
	uint c = pcg(b ^ 0x02e5be93u);
	uint d = pcg(c ^ 0x9e3779b9u);

	return vec4(uvec4(a, b, c, d) >> 8u) * (1.0 / 16777216.0);
}

// --- The ground ---------------------------------------------------------------

// A cell's texel of a map
ivec2 cell_of(vec2 world_xz)
{
	int size = int(map.x);

	return clamp(ivec2(floor(world_xz / map.y + 0.5)), ivec2(0), ivec2(size - 1));
}

// The ground's height, between the cells (R32F is not filtered everywhere)
float ground_height(vec2 world_xz)
{
	int size = int(map.x);
	vec2 grid_position = clamp(world_xz / map.y, vec2(0.0), vec2(float(size - 1)));
	ivec2 low = ivec2(floor(grid_position));
	vec2 fraction = grid_position - vec2(low);
	// The four cells round it in one gather: sampled halfway between their
	// middles (its sampler clamps: past the last cell, the last again);
	// x (low, high), y (high, high), z (high, low), w (low, low)
	vec4 corners = textureGather(height_texture, (vec2(low) + 1.0) / float(size), 0);

	return mix(mix(corners.w, corners.z, fraction.x), mix(corners.x, corners.y, fraction.x), fraction.y);
}

// How much grass grows here (0..1; y: the ground's height), and the silt
// over it: as much as the terrain shows the grass material there
// (terrain_materials.glsl, as terrain.frag works it out)
float grass_amount(vec2 world_xz, float y, out float silt)
{
	vec2 grid_position = world_xz / map.y;
	vec2 uv = (grid_position + 0.5) / map.x;
	vec4 encoded = textureLod(normal_texture, uv, 0.0) * 2.0 - 1.0;
	vec3 smooth_normal = normalize(vec3(encoded.z, sqrt(max(1.0 - dot(encoded.zw, encoded.zw), 0.0)), encoded.w));
	vec4 surface = textureLod(surface_texture, uv, 0.0);
	float slope = 1.0 - smooth_normal.y;
	float sun_facing = dot(smooth_normal.xz, normalize(blade.zw + vec2(1e-5)));
	float detail_noise = value_noise(world_xz * 0.11);
	float broad_noise = value_noise(world_xz * 0.013);
	float amount = 0.0;

	silt = clamp(surface.r * (0.8 + 0.6 * detail_noise), 0.0, 1.0);
	// Under a building none (the material map's ground byte, worked out from
	// the objects' footprints: ground_footprints.h; its own cell, not moved)
	if ((int(texelFetch(material_map_texture, cell_of(world_xz), 0).a * 255.0 + 0.5) & MATERIAL_GROUND_NO_GRASS) != 0)
	{
		return 0.0;
	}
#ifdef MATERIAL_MAP
	// The cell the terrain shows there (moved as the terrain moves it, the
	// patches between cells left out: a blade does not see them): grass, or
	// not; less of it under what the water did and the snow
	{
		ivec2 cell = clamp(ivec2(floor(soil_warped_position(grid_position, 1.5) + 0.5)), ivec2(0), ivec2(int(map.x) - 1));
		vec4 map_cell = texelFetch(material_map_texture, cell, 0);
		int cover = int(map_cell.g * 255.0 + 0.5);
		int shown = cover != 255 ? cover : int(map_cell.r * 255.0 + 0.5);

		if (shown != MATERIAL_GRASS)
		{
			return 0.0;
		}
		amount = (1.0 - surface.b) * (1.0 - silt)
			* (1.0 - terrain_snow_line_amount(vec3(world_xz.x, y, world_xz.y), slope, sun_facing, broad_noise, 0.5, terrain.xyz));
		// (under the snow none: it buries them as it deepens)
		if (control.z == 1)
		{
			amount *= 1.0 - smoothstep(0.25, 0.75, texelFetch(overlay_texture, cell_of(world_xz), 0).r);
		}
	}
#else
	{
		vec4 soil = textureLod(soil_weight_texture, (soil_warped_position(grid_position, map.z) + 0.5) / map.x, 0.0);
		float variation_large = textureLod(variation_texture, world_xz * terrain.w, 0.0).r;
		float variation_small = textureLod(variation_texture, world_xz * terrain.w * 3.83 + 0.61, 0.0).r;
		float weights[MATERIAL_COUNT];
		int chosen[3];
		float contributions[3];

		soil /= max(soil.x + soil.y + soil.z + soil.w, 1e-3);
		// (no rock relief here: the gullies' lower snow is for the rock)
		terrain_material_weights(vec3(world_xz.x, y, world_xz.y), soil, surface, slope, sun_facing, detail_noise, broad_noise,
			variation_large, variation_small, 0.5, slopes, terrain.xyz, weights);
		terrain_material_patches(weights, world_xz, 0.0, material_patches, chosen, contributions);
		for (int k = 0; k < 3; k++)
		{
			amount += chosen[k] == MATERIAL_GRASS ? contributions[k] : 0.0;
		}
	}
#endif
	return amount;
}

// Whether what was drawn the frame before hides a blade, its foot to its
// top a little wider (objects_cull.comp's test, for a sliver): its nearest
// point farther than the farthest depth over it, read at the level where
// it spans a texel or two
bool hidden_by_pyramid(vec3 foot, vec3 top, float radius)
{
	vec2 screen_minimum = vec2(1.0);
	vec2 screen_maximum = vec2(0.0);
	float nearest = 1.0;

	if (hiz_size.w < 0.5)
	{
		return false;
	}
	for (int k = 0; k < 2; k++)
	{
		vec4 clip = hiz_view_projection * vec4(k == 0 ? foot : top, 1.0);

		if (clip.w <= 0.05)
		{
			return false;
		}
		vec3 ndc = clip.xyz / clip.w;
		vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
		// (as wide as the blade may lean, on screen)
		vec2 reach = vec2(radius / clip.w) * vec2(abs(hiz_view_projection[0][0]), abs(hiz_view_projection[1][1])) * 0.5;

		screen_minimum = min(screen_minimum, uv - reach);
		screen_maximum = max(screen_maximum, uv + reach);
		nearest = min(nearest, ndc.z);
	}
	if (screen_maximum.x < 0.0 || screen_maximum.y < 0.0 || screen_minimum.x > 1.0 || screen_minimum.y > 1.0 || nearest <= 0.0)
	{
		return false;
	}
	screen_minimum = clamp(screen_minimum, vec2(0.0), vec2(1.0));
	screen_maximum = clamp(screen_maximum, vec2(0.0), vec2(1.0));

	vec2 extent = (screen_maximum - screen_minimum) * hiz_size.xy;
	int level = int(clamp(ceil(log2(max(max(extent.x, extent.y), 1.0))), 0.0, hiz_size.z - 1.0));
	int level_offset = hiz_levels[level].x;
	ivec2 size = hiz_levels[level].yz;
	ivec2 base_size = ivec2(hiz_size.xy);
	ivec2 first = clamp(clamp(ivec2(floor(screen_minimum * hiz_size.xy)), ivec2(0), base_size - 1) >> level, ivec2(0), size - 1);
	ivec2 last = clamp(clamp(ivec2(floor(screen_maximum * hiz_size.xy)), ivec2(0), base_size - 1) >> level, ivec2(0), size - 1);
	float farthest = 0.0;

	// (two or three texels across at that level, never more read)
	if (last.x > first.x + 2 || last.y > first.y + 2)
	{
		return false;
	}
	for (int y = first.y; y <= last.y; y++)
	{
		for (int x = first.x; x <= last.x; x++)
		{
			farthest = max(farthest, hiz_depths[level_offset + y * size.x + x]);
		}
	}
	return nearest > farthest;
}

bool sphere_visible(vec3 center, float radius)
{
	for (int i = 0; i < 6; i++)
	{
		if (dot(planes[i].xyz, center) + planes[i].w < -radius)
		{
			return false;
		}
	}
	return true;
}

// --- What is the same all up a blade ------------------------------------------

float hash_1d(vec2 position)
{
	return fract(sin(dot(position, vec2(12.9898, 78.233))) * 43758.5453);
}

// Its color before what changes up it (grass.vert: lighter and yellower to
// the tip, the silt's brown, the gust's sheen): the ground's grass under it
// (the far, smooth scale of its texture), the terrain's variation over its
// materials (as terrain.frag has it), darker wet, its own shade, some dry
vec3 blade_color(vec3 root, vec4 surface)
{
	vec3 ground = textureLod(material_color_array, vec3(root.xz * look.y, look.x), 3.0).rgb;
	vec3 color = pow(ground, vec3(2.2)) * tint.rgb;
	float large = textureLod(variation_texture, root.xz * terrain.w, 0.0).r;
	float small = textureLod(variation_texture, root.xz * terrain.w * 3.83 + 0.61, 0.0).r;
	float amount = (large - 0.5) * 0.7 + (small - 0.5) * 0.3;

	color *= 1.0 + amount * look.z * 2.0;
	color *= mix(vec3(1.0), amount > 0.0 ? vec3(1.08, 1.02, 0.86) : vec3(0.90, 1.02, 0.94), min(abs(amount) * 2.0, 1.0) * look.w);
	color *= 1.0 - 0.3 * surface.g;
	color *= 0.85 + 0.3 * hash_1d(root.xz + 17.31);
	if (hash_1d(root.xz) < tint.w)
	{
		color = mix(color, vec3(0.32, 0.26, 0.12), 0.65);
	}
	return color;
}

// How much of the sun the clouds over a point leave it (as object.frag's)
float cloud_light(vec3 position, vec3 towards_sun)
{
	if (cloud_parameters.z <= 0.0)
	{
		return 1.0;
	}
	float climb = max(cloud_motion.z - position.y, 0.0) / max(towards_sun.y, 0.05);
	vec2 on_sheet = position.xz + towards_sun.xz * climb - cloud_motion.xy;

	return 1.0 - cloud_shade(on_sheet) * cloud_parameters.z;
}

// Light (rgb, 0 and up) into four bytes: three and a shared exponent
uint pack_shared_exponent(vec3 light)
{
	float largest = max(max(light.r, light.g), max(light.b, 1e-8));
	float exponent = clamp(ceil(log2(largest)), -60.0, 60.0);
	vec3 scaled = clamp(light * exp2(-exponent), 0.0, 1.0);

	return packUnorm4x8(vec4(scaled, (exponent + 128.0) / 255.0));
}

// The blade's record (see the top), the parts that are the same all up it
// worked out here
void write_blade(uint base, vec3 root, float height, float width, float facing_turn, vec2 flatten)
{
	vec2 surface_uv = (root.xz / map.y + 0.5) / map.x;
	vec4 surface = textureLod(surface_texture, surface_uv, 0.0);
	vec3 color = blade_color(root, surface);
	// Half up it, a little towards the sun
	vec3 middle = root + vec3(0.0, height * 0.5, 0.0);
	vec3 looked_up = middle + sun_direction * 0.3;
	float sun = sun_visibility_taps(light_view_projection * vec4(looked_up, 1.0), near_light_view_projection * vec4(looked_up, 1.0), true)
		* cloud_light(middle, sun_direction);
	vec4 sky = sky_light_at(middle, height * 0.5, vec3(0.0, 1.0, 0.0));

	blades[base] = floatBitsToUint(root.x);
	blades[base + 1u] = floatBitsToUint(root.y);
	blades[base + 2u] = floatBitsToUint(root.z);
	blades[base + 3u] = packHalf2x16(vec2(height, width));
	blades[base + 4u] = packHalf2x16(flatten);
	blades[base + 5u] = packUnorm4x8(vec4(facing_turn, sun, sky.a, surface.r));
	blades[base + 6u] = packUnorm4x8(vec4(sqrt(clamp(color * 0.5, 0.0, 1.0)), 0.0));
	blades[base + 7u] = pack_shared_exponent(sky.rgb);
}

float blade_hash(vec2 position)
{
	return fract(sin(dot(position, vec2(12.9898, 78.233))) * 43758.5453);
}


void main()
{
	if (control.x == MODE_CLEAR)
	{
		if (gl_GlobalInvocationID.x == 0u)
		{
			counts[0] = 0u;
			counts[5] = 0u;
			counts[6] = 0u;
		}
		return;
	}
	if (control.x == MODE_ARGUMENTS)
	{
		if (gl_GlobalInvocationID.x == 0u)
		{
			// (the near list from the buffer's start, the far one from its
			// end: its first instance where it begins)
			uint far_count = counts[6];

			counts[1] = BLADE_VERTICES;
			counts[2] = counts[5];
			counts[3] = 0u;
			counts[4] = 0u;
			counts[8] = FAR_BLADE_VERTICES;
			counts[9] = far_count;
			counts[10] = 0u;
			counts[11] = uint(grid_origin.w) - far_count;
		}
		return;
	}
	uint side = uint(grid.z);
	uint index = gl_GlobalInvocationID.x;

	if (index >= side * uint(control.w))
	{
		return;
	}
	float spacing = grid.x;
	uint level = uint(grid.y);
	float full_radius = grid.w;
	// The spot on the finest grid: a coarser level looks at every
	// 2^(level - 1)-th of its spots, the same blades (nested grids)
	int step = level == 0u ? 1 : (1 << (level - 1u));
	ivec2 spot = (ivec2(floor(grid_origin.xy / spacing + 0.5)) + ivec2(index % side, index / side)) * step;
	vec4 random = random4(spot, 0u);
	// Anywhere in its cell (of the finest grid)
	vec2 world_xz = (vec2(spot) + random.xy) * grid_origin.z;
	float horizontal = length(world_xz - eye.xz);
	float inner = level == 0u ? 0.0 : full_radius * exp2(float(level) - 1.0);
	float outer = level == 0u ? full_radius : full_radius * exp2(float(level));
	float map_extent = (map.x - 1.0) * map.y;

	if (horizontal < inner || horizontal >= outer || horizontal >= eye.w)
	{
		return;
	}
	if (world_xz.x < 0.0 || world_xz.y < 0.0 || world_xz.x > map_extent || world_xz.y > map_extent)
	{
		return;
	}
	// Thinned out with the square of the distance, past full_radius. Each
	// blade has its own threshold and is there while the share kept is
	// above it: farther, the share drops and blades go one by one, each
	// shrinking into the ground first, and the same ones come back nearer;
	// nothing changes as a whole. The blades on a coarser grid (lying on
	// every 2^m-th spot: m the trailing zeros of both its coordinates) get
	// the lower thresholds (4^-(m+1) .. 4^-m), so that the coarse grid a
	// far ring looks at holds every blade that can be there (the share there
	// is below 4^-(level-1)), and a ring's edge changes nothing
	float kept = min((full_radius / max(horizontal, 0.01)) * (full_radius / max(horizontal, 0.01)), 1.0);
	float threshold;
	{
		int zeros_x = spot.x == 0 ? 12 : findLSB(spot.x);
		int zeros_y = spot.y == 0 ? 12 : findLSB(spot.y);
		int coarse = min(min(zeros_x, zeros_y), 12);

		threshold = exp2(-2.0 * float(coarse)) * (coarse >= 12 ? random.z : 0.25 + 0.75 * random.z);
	}
	if (threshold >= kept)
	{
		return;
	}
	// Out of view whatever the ground's height there (from the lowest to the
	// highest ground, a blade over it): before the ground's four reads
	for (int i = 0; i < 6; i++)
	{
		float across = dot(planes[i].xz, world_xz) + planes[i].w;
		float most = across + max(0.0, planes[i].y * (terrain.z + 2.0 * blade.x));

		if (most < -(blade.x + 0.3))
		{
			return;
		}
	}
	// Coming or going: smaller the nearer the share is to its threshold
	float thinning = clamp((kept - threshold) / max(0.4 * threshold, 1e-6), 0.0, 1.0);
	float y = ground_height(world_xz);

	// Out of view (as tall as a blade gets): the cheap test before the
	// ground's look
	if (!sphere_visible(vec3(world_xz.x, y + 0.6 * blade.x, world_xz.y), 2.0 * blade.x + 0.3))
	{
		return;
	}
	// Behind a house, a hill (what the frame before drew), as tall as a
	// blade gets and as far as it leans
	if (hidden_by_pyramid(vec3(world_xz.x, y, world_xz.y), vec3(world_xz.x, y + 1.6 * blade.x, world_xz.y), blade.x + 0.1))
	{
		return;
	}
	float silt;
	float amount = grass_amount(world_xz, y, silt);

	// Fewer where less grows (none where the ground hardly shows grass),
	// the edges ragged
	if (random.w >= smoothstep(0.45, 0.85, amount))
	{
		return;
	}
	vec4 more = random4(spot, 101u);
	// Clumps: taller and shorter patches
	float clump = 0.65 + 0.7 * value_noise(world_xz * 0.9);
	float height = blade.x * (0.6 + 0.8 * more.x) * (0.55 + 0.45 * amount) * clump * thinning;
	float distance = length(vec3(world_xz.x, y, world_xz.y) - eye.xyz);
	// Wider far away, so the thinned out blades still cover the ground
	float widen = min(pow(max(horizontal / full_radius, 1.0), 1.2), 8.0);
	float width = blade.y * (0.75 + 0.5 * more.y) * widen;

	// They shrink into the ground over the last fifth of the distance
	height *= 1.0 - smoothstep(0.8 * eye.w, eye.w, distance);
	if (height < 0.01)
	{
		return;
	}
	// Water: deeper than the blade by a little, none; shallower, laid down
	// the way it flows, more the faster and the deeper; silt leaves them down
	vec2 flatten = vec2(0.0);
	{
		ivec2 cell = cell_of(world_xz);
		float depth = texelFetch(water_texture, cell, 0).x;
		vec2 flow = texelFetch(flow_texture, cell, 0).xy;
		float speed = length(flow);
		vec2 random_direction = vec2(cos(more.z * 6.2831853), sin(more.z * 6.2831853));

		if (depth > height + 0.4)
		{
			return;
		}
		if (depth > 0.005)
		{
			float under = clamp(depth / max(height, 0.05), 0.0, 1.0);

			flatten = (speed > 0.01 ? flow / speed : random_direction) * under * (0.35 + 0.65 * clamp(speed / 0.4, 0.0, 1.0));
		}
		if (silt > 0.05 && dot(flatten, flatten) < silt * silt * 0.6)
		{
			flatten = random_direction * silt * 0.8;
		}
	}
	vec3 root = vec3(world_xz.x, y, world_xz.y);
	// (grass.vert's own hash of it, the same)
	float own = blade_hash(root.xz);
	// Far off, a blade is one triangle, its foot to its tip, a little wider
	// (a few pixels: the segments' bow is not seen, and four triangles of a
	// pixel each cost the most of anything drawn). Where that starts
	// differs blade by blade (no ring on the meadow)
	bool far_blade = coarse.x > 0.0 && distance > coarse.x * (0.85 + 0.3 * own);
	uint capacity = uint(grid_origin.w);
	uint place;

	// (the two lists share the buffer: a blade taken only while both
	// together fit)
	if (atomicAdd(counts[0], 1u) >= capacity)
	{
		return;
	}
	if (far_blade)
	{
		width *= coarse.y;
		place = capacity - 1u - atomicAdd(counts[6], 1u);
	}
	else
	{
		place = atomicAdd(counts[5], 1u);
	}
	// Which way its face looks (a share of a turn), its width, laid down
	// towards (and how far)
	write_blade(place * BLADE_FLOATS, root, height, width, more.w, flatten);
}
