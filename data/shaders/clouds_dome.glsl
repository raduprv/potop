#include "common.glsl"
#include "scene.glsl"

// Volumetric clouds into the dome (included by clouds_dome.comp and, with
// DOME_BLEND, clouds_dome_blend.comp; see clouds.h and clouds_volume.glsl):
// this frame's share of its texels, each marched along its direction
// through the layer, and blended with what the texel held. Out: the light
// the clouds send to the eye (linear, already weighted by their opacity)
// and the opacity. Nothing on the map reaches the clouds (the layer starts
// well above the highest mountain), so they do not depend on what is in
// front of them: the composite hides them behind the land.
//
// Where the clouds are comes from the flat cloud field (clouds.glsl) that
// casts the shadows on the land. Their shape: a 3D noise of broad billows
// (Perlin-Worley and Worley octaves), eaten at the edges by a finer one;
// rounded at the bottom, thinning to the top. The march takes long steps
// through the clear air with the shape alone, and short ones with the
// detail once it is in a cloud. The light: Beer-Lambert absorption towards
// the eye and the sun (a few steps, without the detail), the powder
// effect, forward scattering around the sun and the sky's light from above.

layout(local_size_x = 64) in;

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D variation_texture;
layout(set = TEXTURE_SET, binding = 1) uniform sampler3D shape_texture;
layout(set = TEXTURE_SET, binding = 2) uniform sampler3D detail_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D stamp_map;
layout(set = TEXTURE_SET, binding = 4) uniform sampler3D atlas_texture;
// The clouds of real density (cloud_weather.h), each at its own size
layout(set = TEXTURE_SET, binding = 5) uniform sampler3D density_atlas;
// Where the flocks are (the sky window's paint, over the cloud map: 0 none,
// 1 all the Amount setting gives)
layout(set = TEXTURE_SET, binding = 6) uniform sampler2D flock_map;
// The density shapes' table (DENSITY_TABLE_VEC4S: two vec4 each, atlas
// texel origin x, z, size x, z; size y), then the clouds' stamps
// (cloud_weather.c): x, z, width, base (meters); cosine, sine of the turn,
// shape, kind (0: a shape of the blob atlas; else a density shape, as dense
// as its size says, mirrored if negative)
layout(std430, set = READONLY_SET, binding = 7) readonly buffer stamp_buffer { vec4 stamps[]; };
#define DENSITY_TABLE_VEC4S 128u

#if defined(CLOUD_SHADOW_MAP)
// The clouds' shadows: how much of the sun each patch of the cloud map
// takes away, looking straight up through the layer (r)
layout(set = READWRITE_SET, binding = 0, rgba16f) uniform writeonly image2D shadow_image;
#elif defined(DOME_BLEND)
layout(set = READWRITE_SET, binding = 0, rgba16f) uniform image2D dome_image;
#else
layout(set = READWRITE_SET, binding = 0, rgba16f) uniform writeonly image2D dome_image;
#endif

// The cover's texture level for the point being marched: far away a pixel
// spans many of its texels, which would sparkle at level 0
float cover_level = 0.0;
#define CLOUDS_LOD_VARIABLE cover_level
#include "clouds.glsl"
#include "clouds_volume.glsl"

// The detail is not worth reading this far away (a few pixels a cloud)
#define DETAIL_DISTANCE 7000.0

// 0 near, 1 where the shape noise is too fine to draw (see main)
float far_blend = 0.0;

float remap(float value, float old_low, float old_high, float new_low, float new_high)
{
	return new_low + (value - old_low) / max(old_high - old_low, 1e-4) * (new_high - new_low);
}

// The light towards the sun: as is up to PHASE_KNEE, then bending
// smoothly towards PHASE_LIMIT (the forward lobe alone would burn the
// clouds white; a hard cap would leave a ring around the sun)
#define PHASE_KNEE 1.5
#define PHASE_LIMIT 2.5
// The cloud map and its atlas of shapes (cloud_weather.h)
#define WEATHER_METERS 25000.0
#define WEATHER_MAP_SIZE 512.0
#define SHAPE_TEXELS 64.0
#define SHAPE_REACH_UP 0.75
#define ATLAS_COLUMNS 8.0
#define ATLAS_ROWS 10.0
#define SHAPE_DISTANCE_RANGE 0.1
// How far the billows push a cloud's surface in and out (a share of its
// width, at most BILLOW_MAXIMUM_METERS), and over how much the density rises
// from the surface (meters)
#define BILLOW_SHARE 0.05
#define BILLOW_MAXIMUM_METERS 300.0
// The billows' and the fine detail's noise tiles across a cloud's width
#define BILLOW_TILES 2.0
#define DETAIL_TILES 10.0
#define EDGE_METERS 25.0
// The clouds of real density are this much thicker than the shapes of the
// blob atlas at the same setting: their insides are not all at full
// density, and a real cumulus is opaque within a few tens of meters
#define REAL_DENSITY_SCALE 4.0
// How much of the low density at their edges the carving may eat (of the
// densest), and the carving noises' tiles (meters: the billows' a few of a
// big cloud's texels, the fine noise's smaller still)
#define REAL_EROSION 0.6
// The density over which a cloud goes from clear to full beyond the carved
// threshold (of the densest): small, a sharp surface
#define REAL_EDGE_BAND 0.25
// A cloud's life (of its length): it forms until LIFE_FORMING, dissolves
// from LIFE_FADING to LIFE_GONE, then is clear sky until it forms again
#define LIFE_FORMING 0.2
#define LIFE_FADING 0.7
#define LIFE_GONE 0.9
#define REAL_BILLOW_METERS 180.0
#define REAL_DETAIL_METERS 45.0
#define LIGHT_EDGE_METERS 120.0

// Where the light's steps start (0..1; set by march)
float light_jitter = 0.5;


float soft_limit(float value)
{
	if (value <= PHASE_KNEE)
	{
		return value;
	}
	return PHASE_KNEE + (PHASE_LIMIT - PHASE_KNEE) * (1.0 - exp(-(value - PHASE_KNEE) / (PHASE_LIMIT - PHASE_KNEE)));
}

// A density shape's density (0..1) at a point of its stamp: local across
// (x, z; of the stamp's width, -0.5..0.5 over its longest side) and up (of
// its width)
float shape_density(uint shape, vec2 local, float up)
{
	vec4 placed = stamps[shape * 2u];
	float size_up = stamps[shape * 2u + 1u].x;
	float across = max(placed.z, placed.w);
	vec3 size = vec3(placed.z, size_up, placed.w);
	// Its texels, from its corner
	vec3 texel = vec3(local.x * across + placed.z * 0.5, up * across, local.y * across + placed.w * 0.5);

	if (any(lessThan(texel, vec3(0.0))) || any(greaterThanEqual(texel, size)))
	{
		return 0.0;
	}
	// Half a texel inside its box: its neighbors in the atlas do not bleed in
	texel = clamp(texel, vec3(0.5), size - 0.5);
	return textureLod(density_atlas, (vec3(placed.x, 0.0, placed.y) + texel) / vec3(textureSize(density_atlas, 0)), 0.0).r;
}

// How far inside the nearest cloud of the cloud map a point is (meters,
// negative outside; see cloud_weather.h): the stamps over its patch of the
// map, each looked up in its shape's distance field. width_out: that
// cloud's width (meters); local_out: where in that cloud the point is (its
// widths, the cloud turned with it, plus the cloud's own offset, so each
// one gets other billows). density_out: the density of the clouds of real
// density there (the densest), 0 if none
float inside_clouds(vec2 on_sheet, float height_meters, out float width_out, out vec3 local_out, out float density_out,
	out float density_scale_out, out float presence_out)
{
	uvec4 stamps_here = uvec4(round(texelFetch(stamp_map, ivec2(fract(on_sheet * (1.0 / WEATHER_METERS)) * WEATHER_MAP_SIZE), 0) * 65535.0));
	float inside = -1e6;

	width_out = 0.0;
	local_out = vec3(0.0);
	density_out = 0.0;
	density_scale_out = 1.0;
	presence_out = 1.0;
	for (int k = 0; k < 4; k++)
	{
		uint stamp = stamps_here[k];

		if (stamp == 0u)
		{
			break;
		}
		vec4 placed = stamps[DENSITY_TABLE_VEC4S + (stamp - 1u) * 3u];
		vec4 turned = stamps[DENSITY_TABLE_VEC4S + (stamp - 1u) * 3u + 1u];
		vec2 offset = on_sheet - placed.xy;

		offset -= round(offset * (1.0 / WEATHER_METERS)) * WEATHER_METERS;
		vec2 local = vec2(offset.x * turned.x + offset.y * turned.y, -offset.x * turned.y + offset.y * turned.x) / placed.z;
		float up = (height_meters - placed.w) / placed.z;

		if (turned.w != 0.0)
		{
			// Of real density: as it is (mirrored by the sign)
			if (up >= 0.0 && abs(local.x) < 0.5 && abs(local.y) < 0.5)
			{
				float here = shape_density(uint(turned.z), vec2(local.x * sign(turned.w), local.y), up);

				// How far through its life it is: forming from its densest
				// cores, whole, then dissolving from its thin edges inwards
				// (here: how present it is, 0..1; see cloud_density)
				vec4 life = stamps[DENSITY_TABLE_VEC4S + (stamp - 1u) * 3u + 2u];
				float present = 1.0;

				if (life.y > 0.0)
				{
					float age = fract(sky_time_seconds / life.y + life.x);

					present = smoothstep(0.0, LIFE_FORMING, age) * (1.0 - smoothstep(LIFE_FADING, LIFE_GONE, age));
				}
				// Below what it lets through it is not there yet (or no more)
				here *= step(1e-3, present);
				if (here > density_out)
				{
					density_out = here;
					presence_out = present;
					density_scale_out = abs(turned.w);
					width_out = placed.z;
					local_out = vec3(local.x, up, local.y) + vec3(float(stamp) * 0.371, float(stamp) * 0.113, float(stamp) * 0.537);
				}
			}
			continue;
		}
		if (abs(local.x) >= 0.5 || abs(local.y) >= 0.5 || up < 0.0 || up >= SHAPE_REACH_UP)
		{
			continue;
		}
		// Into the shape's cell of the atlas (kept half a texel inside it,
		// so its neighbors do not bleed in)
		float shape = turned.z;
		vec2 cell = vec2(mod(shape, ATLAS_COLUMNS), floor(shape / ATLAS_COLUMNS));
		vec2 within = clamp(local + 0.5, vec2(0.5 / SHAPE_TEXELS), vec2(1.0 - 0.5 / SHAPE_TEXELS));
		vec3 coordinates = vec3((cell.x + within.x) / ATLAS_COLUMNS, up / SHAPE_REACH_UP, (cell.y + within.y) / ATLAS_ROWS);
		float meters = (textureLod(atlas_texture, coordinates, 0.0).r - 0.5) * 2.0 * SHAPE_DISTANCE_RANGE * placed.z;

		if (meters > inside)
		{
			inside = meters;
			width_out = placed.z;
			local_out = vec3(local.x, up, local.y) + vec3(float(stamp) * 0.371, float(stamp) * 0.113, float(stamp) * 0.537);
		}
	}
	return inside;
}

// Cloud density at a point (0 outside the layer); detail: the fine noise
// too. The clouds' shapes come from the cloud map; the billows noise
// pushes their surfaces in and out (by a share of each cloud's width) into
// heaped lumps, the density rises over edge_meters from the surface
// (EDGE_METERS for what is seen; LIGHT_EDGE_METERS for the light's way to
// the sun, smoother, so the shading has no steps), and the fine noise
// frays the thin edge.
float density_on_sheet(vec2 on_sheet, float height_meters, bool detailed, float edge_meters)
{
	float width;
	vec3 local;
	float real_density;
	float density_scale;
	float presence;
	float inside = inside_clouds(on_sheet, height_meters, width, local, real_density, density_scale, presence);
	float billow_meters = clamp(width * BILLOW_SHARE, 15.0, BILLOW_MAXIMUM_METERS);

	if (real_density > 0.0)
	{
		// A cloud of real density: its own lumps down to its texels (a few
		// meters to a few tens). Under that, near, the noise carves smaller
		// ones into its edges: where the density is low, the billows and the
		// fine noise eat it, so the edge is a crisp lumpy surface instead of
		// a blur a texel wide. Far away (and on the light's way) the same
		// carving on average, without the noise
		float erosion = 0.5;

		if (detailed)
		{
			// In meters, not of the cloud's width: a big cloud's texels are
			// big, and it needs the small lumps as much as a small one
			vec4 lumps = textureLod(shape_texture, local * (width / REAL_BILLOW_METERS), 0.0);
			vec3 detail = textureLod(detail_texture, local * (width / REAL_DETAIL_METERS) + vec3(0.0, sky_time_seconds * 0.002, 0.0), 0.0).rgb;
			float billows = lumps.g * 0.625 + lumps.b * 0.25 + lumps.a * 0.125;
			float fine = detail.r * 0.625 + detail.g * 0.25 + detail.b * 0.125;

			erosion = mix(1.0 - (billows * 0.7 + fine * 0.3), 0.5, far_blend);
		}
		// Rising over a narrow band of the density above the carved
		// threshold: the surface is where the density crosses it (smooth,
		// between the texels), not the texels' blur
		// Forming or dissolving, the threshold is higher: only the densest
		// parts are there (none at all at the ends of its life)
		float low = mix(1.0, erosion * REAL_EROSION, presence);

		// (each cloud as dense as its stamp says: white and puffy .. heavy)
		return clamp(remap(real_density, low, low + REAL_EDGE_BAND, 0.0, 1.0), 0.0, 1.0) * REAL_DENSITY_SCALE * density_scale;
	}
	if (inside < -billow_meters)
	{
		return 0.0;
	}
	float height = height_meters / thickness;
	// The noises in the cloud's own frame, sized to it: a small cloud gets
	// small billows, a storm cloud big ones, and they travel with it
	vec4 shape = textureLod(shape_texture, local * BILLOW_TILES, 0.0);
	float billows = shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
	// Far away the billows are smaller than a pixel: the plain outline
	billows = mix(billows, 0.5, far_blend);
	float surface = inside + (billows - 0.55) * 2.0 * billow_meters;
	float cloud = clamp(surface / edge_meters, 0.0, 1.0);

	if (detailed && cloud > 0.0 && cloud < 1.0)
	{
		vec3 detail = textureLod(detail_texture, local * DETAIL_TILES + vec3(0.0, sky_time_seconds * 0.002, 0.0), 0.0).rgb;
		float fine = detail.r * 0.625 + detail.g * 0.25 + detail.b * 0.125;
		// Wispy at the bottom, billowing at the top
		float erosion = mix(fine, 1.0 - fine, clamp(height * 4.0, 0.0, 1.0));

		cloud = clamp(remap(cloud, erosion * 0.2, 1.0, 0.0, 1.0), 0.0, 1.0);
	}
	return cloud;
}

float cloud_density(vec3 point, bool detailed, float edge_meters)
{
	float height_meters = curved_height(point) - base_height;

	if (height_meters <= 0.0 || height_meters >= thickness)
	{
		return 0.0;
	}
	return density_on_sheet(point.xz - cloud_motion.xy, height_meters, detailed, edge_meters);
}

float henyey_greenstein(float cosine, float g)
{
	float g2 = g * g;

	return (1.0 - g2) / (4.0 * 3.14159265 * pow(1.0 + g2 - 2.0 * g * cosine, 1.5));
}

// The cloud between a point and the sun (optical depth: extinction times
// meters). The steps grow away from the point and start at a jittered
// place (light_jitter, the march's own), so the few of them do not leave
// bands of shade; over the frames the jitter averages out
float depth_towards_sun(vec3 point)
{
	float total = 0.0;
	// The first steps short (about a texel of a cloud of real density: its
	// lumps shade each other), each twice the last
	float step_length = clamp(thickness * 0.012, 8.0, 40.0);
	vec3 position = point + sun_direction * step_length * (light_jitter - 0.5);

	for (int i = 0; i < 6; i++)
	{
		position += sun_direction * step_length;
		total += cloud_density(position, false, LIGHT_EDGE_METERS) * step_length;
		step_length *= 2.0;
	}
	return total * density;
}

// How much of a cloud first_hit meters along a direction the haze hides
// (0..1): as the land's (haze_amount, scene.glsl), the haze thinning
// upwards, but along the ray over the curved ground, which stays low for a
// long way towards the horizon. A hidden cloud is not painted the haze's
// color: it fades out, and the sky behind it (hazy towards the horizon
// itself) shows through, so it never stands out as a patch of another blue
float cloud_haze(vec3 direction, float first_hit)
{
	float scale = max(fog_parameters.x, 10.0);
	float step_length = first_hit / 8.0;
	float depth = 0.0;

	for (int i = 0; i < 8; i++)
	{
		vec3 point = eye_position + direction * ((float(i) + 0.5) * step_length);

		// (no denser than at the ground: under the horizon, where the
		// planet is let down, the way goes below it)
		depth += exp(-clamp(curved_height(point), 0.0, 100000.0) / scale);
	}
	depth *= step_length * fog_density;
	return clamp(1.0 - exp(-depth * depth), 0.0, 1.0);
}

// The haze in front of what march_heaps and march_flocks found (set by
// them; see march: it is applied after the two are put together)
float heaps_haze = 0.0;
float flocks_haze = 0.0;

// A well mixed hash of a texel (PCG)
uint hash_texel(ivec2 texel)
{
	uint value = uint(texel.x) * 747796405u + uint(texel.y) * 2891336453u + 1u;

	value = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
	return (value >> 22u) ^ value;
}

// The big clouds along a direction: light (premultiplied) and opacity.
// jitter (0..1): where in its first step the march starts
vec4 march_heaps(vec3 direction, float jitter)
{
	float enter;
	float leave;
	vec4 result = vec4(0.0);

	if (cloud_parameters.x <= 0.0 || !layer_interval(eye_position, direction, maximum_distance, enter, leave))
	{
		return result;
	}

	{
		// Short steps inside clouds, three times longer through clear air;
		// both grow with the distance (far clouds are small on screen)
		float fine_length = 700.0 / steps;
		float distance = enter + fine_length * 3.0 * (1.0 + enter / 1500.0) * jitter;

		light_jitter = fract(jitter * 7.31 + 0.37);
		float transmittance = 1.0;
		vec3 light = vec3(0.0);
		float cosine = dot(direction, sun_direction);
		// Light bounces many times inside a cloud, which is why thick
		// clouds are bright and not black inside: approximated by three
		// octaves, each with half the light, seeing half the cloud (it gets
		// through more easily) and scattering more evenly (Wrenninge's)
		float phases[3];

		for (int octave = 0; octave < 3; octave++)
		{
			float spread = pow(0.5, float(octave));

			// Forward scattering (silver linings) with a little back scattering
			phases[octave] = soft_limit(mix(henyey_greenstein(cosine, -0.2 * spread), henyey_greenstein(cosine, 0.5 * spread), 0.6)
				* 4.0 * 3.14159265);
		}
		float first_hit = -1.0;
		// The sky's light, less blue than the sky looks (what reaches a
		// cloud's shaded side is mostly the light of other clouds and the
		// land; a pure sky blue makes them look like blue plastic)
		vec3 sky_light = mix(sky_color, sky_zenith_color, 0.5);
		vec3 ambient = mix(vec3(dot(sky_light, vec3(0.3, 0.59, 0.11))), sky_light, 0.4) * (0.6 + ambient_intensity * 0.6) * planet.y;
		// A thick layer hides the sky from its bottom (storm clouds' dark bases)
		float base_shade = 1.0 - look.y;
		bool inside = false;
		float cover_texels_per_meter = float(textureSize(variation_texture, 0).x) * cloud_parameters.w;
		int clear_streak = 0;

		for (int i = 0; i < 384 && distance < leave; i++)
		{
			float grow = 1.0 + distance / 1500.0;
			float step_length = (inside ? fine_length : fine_length * 3.0) * grow;
			vec3 point = eye_position + direction * distance;

			// A dome texel spans about 2 / its size radians; along a
			// grazing ray it stretches over the layer
			cover_level = log2(max(distance * 2.5 / dome.x / max(abs(direction.y), 0.15) * cover_texels_per_meter, 1.0));
			far_blend = smoothstep(4000.0, 12000.0, distance);

			if (!inside)
			{
				// Probing the clear air with the shape alone
				if (cloud_density(point, false, EDGE_METERS) > 0.0)
				{
					inside = true;
					clear_streak = 0;
					// Back to where the cloud may begin
					distance = max(distance - fine_length * 2.0 * grow, enter);
					continue;
				}
				distance += step_length;
				continue;
			}
			{
				float sample_density = cloud_density(point, distance < DETAIL_DISTANCE, EDGE_METERS);

				if (sample_density <= 0.0)
				{
					clear_streak++;
					if (clear_streak > 4)
					{
						inside = false;
					}
					distance += step_length;
					continue;
				}
				clear_streak = 0;
				if (first_hit < 0.0)
				{
					first_hit = distance;
				}
				float extinction = sample_density * density;
				float optical_depth = depth_towards_sun(point);
				float powder = 1.0 - exp(-extinction * step_length * 4.0);
				float height = clamp((curved_height(point) - base_height) / thickness, 0.0, 1.0);
				// (divided by the three octaves' sum, so a cloud's thin edge
				// is as bright as with a single scattering)
				float sun_light = (phases[0] * exp(-optical_depth) + 0.5 * phases[1] * exp(-optical_depth * 0.5)
					+ 0.25 * phases[2] * exp(-optical_depth * 0.25)) * (1.0 / 1.75);
				vec3 scattered = sun_color * sun_light * mix(0.5, 1.0, powder)
					+ ambient * mix(0.1 + 0.25 * base_shade, 1.0, smoothstep(0.0, mix(0.3, 0.8, look.y), height));
				float step_transmittance = exp(-extinction * step_length);

				// Energy conserving: what this step scatters, seen through
				// what is in front of it
				light += transmittance * scattered * (1.0 - step_transmittance);
				transmittance *= step_transmittance;
				distance += step_length;
			}
			if (transmittance < 0.01)
			{
				break;
			}
		}
		if (first_hit < 0.0)
		{
			return result;
		}
		// Far clouds fade into the haze (see cloud_haze)
		result = vec4(light, 1.0 - transmittance);
		heaps_haze = cloud_haze(direction, first_hit);
	}
	return result;
}


// ---------------------------------------------------------------------------
// The flocks (clouds.h): small white clouds in a thin layer high up. Each is
// a cell of the shape noise's Worley octaves (its blue channel: 8 cells a
// tile, and two finer octaves that crinkle them), its tile sized to the
// flocks' size. The cells are a little longer along the wind, pushed off
// their grid by the broad noise, and bunched in rows across the wind; they
// boil slowly (the noise's third axis goes by with the sky's clock). How
// many: a threshold on the cells (the Amount, grown or dissolved, times the
// paint). Thin and not very dense: white, lit by the sun through what is
// above them in the layer.

#define FLOCK_CELLS_PER_TILE 8.0
// Extinction per meter at full density
#define FLOCK_EXTINCTION 0.06
// Tiles of the noise's third axis a second: how fast they boil
#define FLOCK_BOIL 0.0008
// Over how much of the cells' range they go from their thin edges to their
// dense middles (wide: puffs, bright at the edges and gray in the middle
// seen from below)
#define FLOCK_EDGE 0.3
// How far into a cloud (of the edge's width) the fine noise frays it
#define FLOCK_FRAY 0.6
// How much of the sun their shadows take away, of what their own thickness
// would (thin clouds scatter most of it forwards)
#define FLOCK_SHADOW 0.5

// The noise's texture level for the point being marched (see march_flocks)
float flock_level = 0.0;

// Their density (0..1) at a point of the cloud map and a height in their
// layer (meters)
float flock_density(vec2 on_sheet, float height_in_layer)
{
	// A light touch of the brush is already most of them (the brush fades
	// to nothing at its edge: taken as it is, a stroke left a few strays
	// in the middle of a patch of clear sky)
	float painted = min(textureLod(flock_map, on_sheet * (1.0 / WEATHER_METERS), 0.0).r * 2.5, 1.0);
	float amount = flock.w * painted;

	if (amount <= 0.002)
	{
		return 0.0;
	}
	float tile = flock.z * FLOCK_CELLS_PER_TILE;
	vec2 wind = flock_wind.xy;
	// In the wind's frame (x along it, y across), in tiles
	vec2 frame = vec2(dot(on_sheet, wind), dot(on_sheet, vec2(-wind.y, wind.x))) / tile;
	// Off the grid: pushed around by the broad noise, up to half a cell
	vec2 warp = vec2(textureLod(shape_texture, vec3(frame.x * 0.29, 0.21, frame.y * 0.29), flock_level).r,
		textureLod(shape_texture, vec3(frame.x * 0.29 + 0.5, 0.63, frame.y * 0.29 + 0.5), flock_level).r) - 0.5;
	vec2 cell = frame + warp * (1.0 / FLOCK_CELLS_PER_TILE);
	float up = height_in_layer / flock.y;
	float cells = textureLod(shape_texture, vec3(cell.x * 0.75, height_in_layer / tile + sky_time_seconds * FLOCK_BOIL, cell.y),
		flock_level).b;
	// Rows across the wind, two or three cells apart (wavy)
	float rows = sin(frame.y * (FLOCK_CELLS_PER_TILE / 2.4) * 6.2831853 + warp.x * 5.0);
	// Many of them: a low threshold, most of each cell is cloud
	float threshold = 1.0 - amount * 0.85 - rows * 0.05 * amount;
	// Flat bottoms, rounded tops: higher near the layer's ends
	float ends = up < 0.3 ? pow((0.3 - up) / 0.3, 3.0) : pow((up - 0.3) / 0.7, 2.0);

	threshold += ends * 0.35;
	float inside = (cells - threshold) / FLOCK_EDGE;

	if (inside <= -FLOCK_FRAY)
	{
		return 0.0;
	}
	// Each one's own texture, sized to it (a few lumps across, many fine
	// tatters), fading out where it is too fine to draw (it would sparkle):
	// the detail noise frays the edge into tatters and wisps, the broad
	// lumps make the middle thick in knots and thin, almost open, between
	// them. They boil with the cells
	vec3 local = vec3(on_sheet.x, height_in_layer + sky_time_seconds * FLOCK_BOIL * tile, on_sheet.y);
	float detail_level = flock_level + 2.7;
	float lump_level = flock_level + 2.4;
	float fine = 1.0 - smoothstep(2.0, 4.0, detail_level);
	float broad = 1.0 - smoothstep(2.0, 4.0, lump_level);
	vec3 tatters = textureLod(detail_texture, local / (flock.z * 0.6), detail_level).rgb;
	vec4 knots = textureLod(shape_texture, local / (flock.z * 1.5) + vec3(0.37, 0.11, 0.73), lump_level);
	float fray = (1.0 - (tatters.r * 0.5 + tatters.g * 0.3 + tatters.b * 0.2)) * FLOCK_FRAY * fine;
	float thickness = mix(1.0, mix(0.04, 1.6, smoothstep(0.25, 0.8, knots.g * 0.7 + knots.b * 0.3)), broad);

	return clamp(inside + FLOCK_FRAY * 0.5 * fine - fray * 1.5, 0.0, 1.0) * thickness;
}

#ifndef CLOUD_SHADOW_MAP
// The flocks along a direction, as march_heaps
vec4 march_flocks(vec3 direction, float jitter)
{
	float enter;
	float leave;

	if (!flock_interval(eye_position, direction, maximum_distance, enter, leave))
	{
		return vec4(0.0);
	}
	float span = leave - enter;
	int count = int(clamp(span / (flock.y * 0.25), 4.0, 24.0));
	float step_length = span / float(count);
	float transmittance = 1.0;
	vec3 light = vec3(0.0);
	float first_hit = -1.0;
	float sun_up = max(sun_direction.y, 0.08);
	float cosine = dot(direction, sun_direction);
	// Mostly forwards (bright towards the sun), never dark away from it
	float phase = soft_limit(mix(henyey_greenstein(cosine, -0.15), henyey_greenstein(cosine, 0.6), 0.55) * 4.0 * 3.14159265) + 0.35;
	vec3 sky_light = mix(sky_color, sky_zenith_color, 0.5);
	vec3 ambient = mix(vec3(dot(sky_light, vec3(0.3, 0.59, 0.11))), sky_light, 0.5) * (0.7 + ambient_intensity * 0.6) * planet.y;
	float texels_per_meter = float(textureSize(shape_texture, 0).x) / (flock.z * FLOCK_CELLS_PER_TILE);

	for (int i = 0; i < count; i++)
	{
		float distance = enter + (float(i) + jitter) * step_length;
		vec3 point = eye_position + direction * distance;
		float height = curved_height(point) - flock.x;

		if (height <= 0.0 || height >= flock.y)
		{
			continue;
		}
		// Far away (and along grazing rays) a cell is a few texels of the
		// dome: blurred to what it averages to, or it sparkles
		flock_level = log2(max(distance * 2.5 / dome.x / max(abs(direction.y), 0.15) * texels_per_meter, 1.0));
		float cloud = flock_density(point.xz - cloud_motion.xy, height);

		if (cloud <= 0.0)
		{
			continue;
		}
		if (first_hit < 0.0)
		{
			first_hit = distance;
		}
		float extinction = cloud * FLOCK_EXTINCTION * flock_wind.z;
		// Towards the sun: the rest of the layer above, about as dense
		float to_sun = extinction * 0.6 * (flock.y - height) / sun_up;
		// (the Shade setting: how much of that shadow they show)
		vec3 scattered = sun_color * phase * mix(1.0, exp(-to_sun), flock_wind.w) + ambient;
		float step_transmittance = exp(-extinction * step_length);

		light += transmittance * scattered * (1.0 - step_transmittance);
		transmittance *= step_transmittance;
		if (transmittance < 0.01)
		{
			break;
		}
	}
	if (first_hit < 0.0)
	{
		return vec4(0.0);
	}
	// Into the haze, as the big clouds
	flocks_haze = cloud_haze(direction, first_hit);
	return vec4(light, 1.0 - transmittance);
}

// Everything along a direction: the big clouds in front of the flocks.
// Each fades by the haze in front of it to the sky behind them all (hazy
// itself), and the flocks are seen through what the big clouds let through
// alone: a big cloud the haze hides hides them as well (the haze in front
// of it is in front of them too)
vec4 march(vec3 direction, float jitter)
{
	heaps_haze = 0.0;
	flocks_haze = 0.0;
	vec4 heaps = march_heaps(direction, jitter);

	if (flock.w <= 0.0 || heaps.a > 0.99)
	{
		return heaps * (1.0 - heaps_haze);
	}
	vec4 flocks = march_flocks(direction, fract(jitter * 3.17 + 0.5));

	return heaps * (1.0 - heaps_haze) + (1.0 - heaps.a) * flocks * (1.0 - max(flocks_haze, heaps_haze));
}
#endif

// Frame n of a still dome marches, in each 4x4 block, the texel of Bayer
// rank n (shifted by the block's own hash, so the blocks do not all take
// the same texel at once and leave a lattice where the clouds move)
const uint bayer_order[16] = uint[16](0u, 10u, 2u, 8u, 5u, 15u, 7u, 13u, 1u, 11u, 3u, 9u, 4u, 14u, 6u, 12u);

// A texel's start along its rays: each march of it a golden ratio step
// further than the last, so a few of them already spread evenly
float texel_jitter(ivec2 texel, float sequence)
{
	return fract(float(hash_texel(texel) >> 8u) * (1.0 / 16777216.0) + sequence * 0.6180339887);
}

#ifdef CLOUD_SHADOW_MAP
// The shadow map: each texel's column of the layer, in steps up through it
#define SHADOW_STEPS 24
void main()
{
	int size = imageSize(shadow_image).x;
	ivec2 texel = ivec2(int(gl_GlobalInvocationID.x) % size, int(gl_GlobalInvocationID.x) / size);

	if (texel.y >= size)
	{
		return;
	}
	vec2 on_sheet = (vec2(texel) + 0.5) / float(size) * WEATHER_METERS;
	float step_meters = thickness / float(SHADOW_STEPS);
	float depth = 0.0;

	for (int k = 0; k < SHADOW_STEPS; k++)
	{
		depth += density_on_sheet(on_sheet, (float(k) + 0.5) * step_meters, false, LIGHT_EDGE_METERS) * step_meters;
	}
	depth *= density;
	// The flocks' too (thin: a light dappling)
	if (flock.w > 0.0)
	{
		float flock_step = flock.y / 6.0;

		flock_level = 1.0;
		for (int k = 0; k < 6; k++)
		{
			depth += flock_density(on_sheet, (float(k) + 0.5) * flock_step) * FLOCK_EXTINCTION * FLOCK_SHADOW * flock_step;
		}
	}
	imageStore(shadow_image, texel, vec4(1.0 - exp(-depth)));
}
#else
void store(ivec2 texel, vec4 result)
{
#ifdef DOME_BLEND
	result = mix(imageLoad(dome_image, texel), result, refresh.x);
#endif
	imageStore(dome_image, texel, result);
}

void main()
{
	uint spacing = uint(dome.y);
	uint cells = uint(dome.x) / spacing;
	uint index = gl_GlobalInvocationID.x;
	ivec2 cell = ivec2(index % cells, index / cells);

	if (index >= cells * cells)
	{
		return;
	}
	if (dome.w > 0.5)
	{
		// Coarse, while the eye moves: one march for each 2x2 block, into
		// all four texels
		ivec2 corner = cell * 2;
		vec4 result = march(dome_direction((vec2(corner) + 1.0) / dome.x), texel_jitter(cell, refresh.y));

		store(corner, result);
		store(corner + ivec2(1, 0), result);
		store(corner + ivec2(0, 1), result);
		store(corner + ivec2(1, 1), result);
		return;
	}
	{
		ivec2 texel = cell * int(spacing);

		if (spacing == 4u)
		{
			uint position = bayer_order[(uint(dome.z) + hash_texel(cell)) & 15u];

			texel += ivec2(position & 3u, position >> 2u);
		}
		store(texel, march(dome_direction((vec2(texel) + 0.5) / dome.x), texel_jitter(texel, refresh.y)));
	}
}
#endif
