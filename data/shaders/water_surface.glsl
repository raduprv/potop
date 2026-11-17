#include "common.glsl"
#include "scene.glsl"

// Water surface fragment shader.
//
//   body color   shallow -> deep by depth (light absorbed on its way down)
//   reflection   strongest at grazing angles (Fresnel, Schlick): what is
//                on screen along the reflected ray (the land, the trees;
//                screen space reflections over a copy of the scene drawn
//                before the water), else the sky with the volumetric
//                clouds from their dome; the sky alone with the
//                reflections off
//   highlight    sharp sun glint
//   ripples      three layers of one tile of steep crested ripples (made at
//                the start, water_renderer.c), each its own size, drifting
//                its own way, bend the normal; on flowing water they drift
//                with the current (a flow map), choppier
//   foam         where the flow is fast
//   mud          water carrying sediment turns brown and opaque
//   shadows      terrain shadows dim the sunlit part and the glint
//   fog          same as the terrain
//   ice          (water_ice.h) over the water: thin, black and clear,
//                mirroring; thicker, white blue, cracked; snow on it; no
//                ripples, rain rings or foam through it

#include "shadows.glsl"
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D variation_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D tile_texture;  // R8 (0..255) per water tile, see apply_tile_overlay
layout(set = TEXTURE_SET, binding = 4) uniform sampler2D cloud_shadow_texture;  // the volumetric clouds' shadows (clouds.glsl)
layout(set = TEXTURE_SET, binding = 5) uniform sampler2D scene_color_texture;  // the scene before the water (linear light)
layout(set = TEXTURE_SET, binding = 6) uniform sampler2D scene_depth_texture;  // its depth (0..1)
layout(set = TEXTURE_SET, binding = 7) uniform sampler2D dome_texture;         // the volumetric clouds around the eye (premultiplied, linear)
layout(set = TEXTURE_SET, binding = 8) uniform sampler2D ice_texture;          // RGBA16F per cell: the ice's thickness (m), the snow on it (water_ice.h)
#define CLOUD_SHADOW_TEXTURE
#include "underwater.glsl"
#include "clouds.glsl"
#include "sky.glsl"
#include "dome_mapping.glsl"
layout(set = TEXTURE_SET, binding = 9) uniform sampler2D sky_occlusion_texture;   // R8 a cell: the sky it sees (sky_occlusion.h)
layout(set = TEXTURE_SET, binding = 10) uniform sampler2D sky_layers_texture;     // RGBA8: the four layers in the air (sky_occlusion.glsl)
#define SKY_OCCLUSION_NO_GROUND
#include "sky_occlusion.glsl"
layout(set = TEXTURE_SET, binding = 11) uniform sampler2D ripple_texture;        // RG16 SNORM: the small ripples' slopes (water_renderer.c)
#define LIGHT_BINDING 12
#include "lights.glsl"

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
	float step_blend;               // used by water.vert
	int reflections;                // 1: the scene's copy is bound (screen space reflections)
	int dome;                       // 0: no volumetric clouds, 1: their dome holds the whole sky, 2: its upper half
	vec4 depth_to_distance;         // x, y: the scene's depth to the distance along the view, y / (depth - x); zw: 1 / its size
	vec4 ice_parameters;            // x 1: the ice is bound; y: the small ripples' choppiness
	vec4 reflection_range;          // x: the land mirrored out to this far from the eye (m), y: fading out over the last this much; beyond, the sky alone; z: the refraction (0: none)
};

#ifdef WATER_SPLIT_REFLECTIONS
// The reflections made apart (water_reflection.comp, at half the size): what
// each pixel would march (the ray's direction, and how much of the pixel's
// final color its reflection is) and what it mirrors meanwhile (the sky,
// and its distance along the view)
layout(location = 1) out vec4 ray_out;
layout(location = 2) out vec4 sky_out;
#endif

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 surface_normal;
layout(location = 2) in float water_depth;
layout(location = 3) in float flow_speed;
layout(location = 4) in vec2 flow_velocity;
layout(location = 5) in float sediment_concentration;
layout(location = 6) in vec4 shadow_position;
layout(location = 7) in vec4 near_shadow_position;
layout(location = 8) in float film_mark;
layout(location = 9) in float thin_mark;
layout(location = 10) in float moving_mark;

layout(location = 0) out vec4 fragment_color;

// 0..1 from a lattice point (whole numbers): an integer hash, exact at
// any distance from the origin. The float hash this replaced lost precision
// far out on big maps, and how much depended on the driver's compiler: the
// ripples it made got long straight ridges, drawn as bright streaks.
float hash_2d(vec2 position)
{
	uvec2 lattice = uvec2(ivec2(position)) * uvec2(1597334673u, 3812015801u);
	uint mixed = (lattice.x ^ lattice.y) * 1597334673u;

	return float(mixed >> 8) * (1.0 / 16777216.0);
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

// The small ripples' slope here (height over meters, each layer's
// steepness times the tile's: spread 1): three sizes of the tile, a few
// centimeters to a meter across, each drifting its own way and slowly
const float RIPPLE_SLOPE_RANGE = 4.0;   // (water_renderer.c's)

vec2 ripple_slope(vec2 position)
{
	vec2 slope = texture(ripple_texture, (position + time_seconds * vec2(0.11, 0.05)) / 4.0).rg * 0.5;

	slope += texture(ripple_texture, (position + time_seconds * vec2(-0.06, 0.09)) / 1.7 + vec2(0.37, 0.71)).rg * 0.35;
	slope += texture(ripple_texture, (position + time_seconds * vec2(0.04, -0.07)) / 0.7 + vec2(0.83, 0.19)).rg * 0.25;
	return slope * RIPPLE_SLOPE_RANGE;
}

// Flow map: a pattern carried downstream by the current would stretch
// without end, so two copies are carried, each restarting every
// FLOW_CYCLE_SECONDS, half a cycle apart; each fades out just before it
// restarts, while the other is at its best. In still water both copies
// lie on each other and it is the plain pattern.
const float FLOW_CYCLE_SECONDS = 1.6;

struct flow_phases
{
	vec2 offset_a;
	vec2 offset_b;
	float weight_a;
};

flow_phases flow_phases_at(vec2 velocity)
{
	flow_phases phases;
	float phase_a = fract(time_seconds / FLOW_CYCLE_SECONDS);
	float phase_b = fract(time_seconds / FLOW_CYCLE_SECONDS + 0.5);

	phases.offset_a = velocity * (phase_a * FLOW_CYCLE_SECONDS);
	// The second copy is shifted too, or the two would show the same
	// pattern pulsing in place
	phases.offset_b = velocity * (phase_b * FLOW_CYCLE_SECONDS) + vec2(3.7, 1.3);
	phases.weight_a = 1.0 - abs(1.0 - 2.0 * phase_a);
	return phases;
}

vec2 flowing_ripple_slope(vec2 position, flow_phases phases)
{
	return mix(ripple_slope(position - phases.offset_b), ripple_slope(position - phases.offset_a), phases.weight_a);
}

// See terrain.frag
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
// Reflections

#include "water_ssr.glsl"

// The sky the surface mirrors: glow, the flat clouds or the volumetric ones
// from their dome (seen from the eye: they are far enough). The sun's disc
// is the glint. Below the horizon the reflected ray would see the far
// shore; the horizon color stands in for it.
// How much the clouds cover a direction, for the moon in the mirror
float water_moon_clouds(vec3 direction)
{
	float cloud = sky_cloud(direction, eye_position, 4.0) * SKY_FLAT_CLOUDS;

	if (dome != 0)
	{
		cloud = max(cloud, smoothstep(0.0, 0.6, textureLod(dome_texture, dome == 2 ? half_coordinates(direction) : sphere_coordinates(direction), 0.0).a));
	}
	return cloud;
}
#define moon_clouds water_moon_clouds
#include "moon.glsl"

vec3 sky_reflection(vec3 direction)
{
	vec3 sky = sky_radiance(direction, eye_position, 0.0, 4.0);

	if (dome != 0)
	{
		vec4 clouds = textureLod(dome_texture, dome == 2 ? half_coordinates(direction) : sphere_coordinates(direction), 0.0);
		float alpha = clamp(clouds.a, 0.0, 1.0);

		// Premultiplied; no brighter than white, as they are drawn
		sky = sky * (1.0 - alpha) + min(clouds.rgb, vec3(alpha));
	}
	// The moon, mirrored (a plain ball: the ripples smear it anyway)
	return sky + moon_disc(direction, 0.0).rgb;
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

// Rings where the rain falls on the water (see src/rain.h): a grid of
// cells, each with a ring spreading from a random point at its own moment,
// again and again. Returns the slope of their waves (the sum of each
// ring's wave, a few centimeters across, along its radius)
vec2 rain_ripple_slope(vec2 position, float clock)
{
	const float CELL = 0.45;
	const float WAVE_NUMBER = 60.0;
	vec2 base = floor(position / CELL);
	vec2 slope = vec2(0.0);

	for (int j = -1; j <= 1; j++)
	{
		for (int i = -1; i <= 1; i++)
		{
			vec2 cell = base + vec2(float(i), float(j));
			vec3 random = fract(sin(vec3(dot(cell, vec2(127.1, 311.7)), dot(cell, vec2(269.5, 183.3)), dot(cell, vec2(419.2, 371.9))))
				* 43758.5453);
			float period = 0.8 + random.z * 0.7;
			float age = fract(clock / period + random.z * 3.7);
			vec2 offset = position - (cell + 0.2 + 0.6 * random.xy) * CELL;
			float distance_to_center = length(offset);
			float behind = distance_to_center - age * 0.35;
			float envelope = exp(-behind * behind / 0.0012) * (1.0 - age) * (1.0 - age);

			if (distance_to_center > 1e-4)
			{
				slope += offset / distance_to_center * WAVE_NUMBER * cos(behind * WAVE_NUMBER) * envelope;
			}
		}
	}
	return slope;
}

void main()
{
#ifdef WATER_SPLIT_REFLECTIONS
	// (nothing to march, unless the surface says otherwise below)
	ray_out = vec4(0.0);
	sky_out = vec4(0.0);
	vec3 split_direction = vec3(0.0, 1.0, 0.0);
	float split_strength = 0.0;
	vec3 split_sky = vec3(0.0);
#endif
	float clouds = cloud_light(world_position, sun_direction);
	vec2 tile_edge_width = fwidth((world_position.xz / cell_size + 0.5) / 64.0) * 1.2;

	// Debugging: thin water moving faster than the sleep speed in bright
	// magenta (it keeps its tile awake), standing thin water in dark purple,
	// films (too thin to flow) in cyan; opaque and unlit, so every trickle
	// shows
	if (thin_highlight.x > 0.5 && (film_mark > 0.5 || thin_mark > 0.5))
	{
		if (film_mark > thin_mark)
		{
			fragment_color = vec4(0.0, 1.0, 1.0, 1.0);
		}
		else
		{
			fragment_color = moving_mark > 0.5 ? vec4(1.0, 0.0, 1.0, 1.0) : vec4(0.3, 0.0, 0.35, 1.0);
		}
		return;
	}
	if (water_depth < 0.002)
	{
		discard;
	}

	vec3 view_vector = eye_position - world_position;
	float view_distance = length(view_vector);
	vec3 view_direction = view_vector / view_distance;

	// Ripples: their slope bends the normal; weaker in very shallow water
	// and far away (the tile's levels of detail already calm it there).
	// Running water is choppier than a lake
	flow_phases phases = flow_phases_at(flow_velocity);
	float current = length(flow_velocity);
	float ripple_strength = 0.18 * ice_parameters.y * (1.0 + min(current, 2.0) * 0.8) * min(water_depth * 4.0, 1.0)
		* (1.0 - smoothstep(300.0, 1200.0, view_distance));
	vec2 ripple = flowing_ripple_slope(world_position.xz, phases) * ripple_strength;
	vec3 normal = normalize(normalize(surface_normal) + vec3(-ripple.x, 0.0, -ripple.y));

	// The ice: its thickness and the snow on it here (between the cells),
	// its edge ragged; under it no ripples, only its own small bumps
	vec2 ice_here = ice_parameters.x > 0.5 ? texture(ice_texture, (world_position.xz / cell_size + 0.5) / float(map_size)).rg : vec2(0.0);
	float ice_cover = 0.0;

	if (ice_here.x > 0.001)
	{
		float edge = smoothstep(0.001, 0.008, ice_here.x);

		ice_cover = edge >= 0.999 ? 1.0 : smoothstep(0.35, 0.65, edge + (value_noise(world_position.xz * 1.7) - 0.5) * 0.7);
		vec3 ice_normal = normalize(normalize(surface_normal)
			+ vec3(value_noise(world_position.xz * 2.3) - 0.5, 0.0, value_noise(world_position.zx * 2.3 + 5.0) - 0.5) * 0.05);

		normal = normalize(mix(normal, ice_normal, ice_cover));
	}

	// The rain's rings (see rain_ripple_slope), near the eye
	if (weather.x > 0.0 && view_distance < 90.0)
	{
		vec2 slope = rain_ripple_slope(world_position.xz, weather.z);
		float strength = 0.004 * weather.x * (1.0 - smoothstep(40.0, 90.0, view_distance)) * min(water_depth * 20.0, 1.0) * (1.0 - ice_cover);

		normal = normalize(normal + vec3(-slope.x, 0.0, -slope.y) * strength);
	}

	float visibility = sun_visibility(shadow_position, near_shadow_position) * clouds;

	// The surface seen from under water. Light from above bends at the
	// surface: looking up within about 49 degrees of straight up you see the
	// sky (Snell's window, wobbling with the ripples); further out the
	// surface mirrors the water below it (total internal reflection).
	if (underwater_color.a > 0.5 && !gl_FrontFacing)
	{
		vec3 towards_surface = -view_direction;
		float up = dot(towards_surface, normal);
		float window = smoothstep(0.62, 0.72, up);
		vec3 sky_seen = sky_radiance(towards_surface, eye_position, 0.0, 3.0) * 1.2
			+ sun_color * pow(max(dot(towards_surface, sun_direction), 0.0), 40.0) * 2.0 * visibility;
		vec3 mirrored = underwater_color.rgb * 0.7;
		vec3 underside = mix(mirrored, sky_seen, window);
		float underside_fog = clamp(1.0 - exp(-view_distance * underwater_parameters.y), 0.0, 1.0);

		underside = mix(underside, underwater_color.rgb, underside_fog);
		fragment_color = vec4(underside, 0.92);
		return;
	}

	float facing = max(dot(normal, view_direction), 0.0);
	float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);

	float absorption = 1.0 - exp(-water_depth / clarity_meters);
	vec3 body = mix(shallow_color, deep_color, absorption);
	// A few percent of sediment is enough to make water look like coffee
	// with milk: you cannot see the bottom any more
	float mud = smoothstep(0.0, 0.04, sediment_concentration);

	body = mix(body, mud_color, mud);
	// (the sky the surface sees: less in a crater, a narrow valley, under
	// the trees; what it mirrors of the sky, and what lights the water)
	float sky_share = sky_seen_above(world_position.xz, water_depth);
	vec3 ambient = sky_color * ambient_intensity * sky_share;
	vec3 body_lit = body * (sun_color * max(dot(normal, sun_direction), 0.0) * visibility * 0.6 + ambient * 1.4
		+ local_light(world_position, normal, 0.3) * 0.6);
	// What the surface mirrors: the land and the trees where the ray finds
	// them on screen (only where the mirror shows: a little of it looking
	// straight down is not worth the march), the sky elsewhere
	vec3 reflected = reflect(-view_direction, normal);
	vec3 reflection = sky_reflection(reflected) * sky_share;

	// (far water mirrors the sky alone: no march)
	float near_enough = 1.0 - smoothstep(reflection_range.x - reflection_range.y, reflection_range.x, view_distance);

	if (reflections != 0 && fresnel > 0.03 && near_enough > 0.0)
	{
		// The march follows only part of the ripples' bend. With all of it,
		// near the eye at the water's level neighboring pixels' rays go
		// every which way: the reflection breaks into long stripes. A third
		// still wobbles the mirrored shore.
		vec3 calm_normal = normalize(mix(normalize(surface_normal), normal, SSR_RIPPLE_SHARE));
		vec3 marched = reflect(-view_direction, calm_normal);
		// A ray going down meets the water again and bounces up from it:
		// mirrored upwards (marched as it is, it would find the bottom and
		// the stones under the surface). Not held flat at the surface: a
		// flat ray from every pixel whose ray went down finds the same strip
		// of shore, and the ripple blows that strip up into a lens
		marched = normalize(vec3(marched.x, max(abs(marched.y), 0.005), marched.z));
#ifdef WATER_SPLIT_REFLECTIONS
		// (marched apart, then laid over what is drawn now: the sky)
		split_direction = marched;
		split_strength = smoothstep(0.03, 0.06, fresnel) * near_enough;
		split_sky = reflection;
#else
		vec4 seen = screen_reflection(world_position + vec3(0.0, 0.02, 0.0), marched, hash_2d(gl_FragCoord.xy));

		reflection = mix(reflection, seen.rgb, seen.a * smoothstep(0.03, 0.06, fresnel) * near_enough);
#endif
	}
	vec3 glint = sun_color * pow(max(dot(reflect(-sun_direction, normal), view_direction), 0.0), 150.0) * 3.0 * visibility;
	vec3 color = mix(body_lit, reflection, fresnel) + glint;

	// Foam needs fast AND reasonably deep water: a thin sheet running down
	// a slope is quick but does not churn
	float foam = smoothstep(foam_speed, foam_speed * 2.0, flow_speed) * smoothstep(0.05, 0.3, water_depth)
		* (0.55 + 0.45 * mix(value_noise((world_position.xz - phases.offset_b) * 1.3), value_noise((world_position.xz - phases.offset_a) * 1.3),
			phases.weight_a));
	vec3 foam_color = vec3(0.85) * (sun_color * visibility * 0.7 + ambient * 1.5);

	foam *= 1.0 - ice_cover;

	color = mix(color, foam_color, foam);

	// Opacity grows with depth; the edge fades in over the first few
	// centimeters so shorelines are soft
	float alpha = 1.0 - exp(-water_depth * opacity_per_meter);
	alpha = max(alpha, fresnel);
	alpha = max(alpha, foam * 0.9);
	alpha = max(alpha, mud * 0.92);
	alpha *= smoothstep(0.002, 0.04, water_depth);

	// The ice over it: thin, black and glassy (the dark water seen through
	// it), a sharper mirror than the water; thicker, clear blue green, frosted
	// here and there (there it mirrors dimly, blurred, and glints broad),
	// small bubbles frozen in it, cracks through it, white where they catch
	// the light. It fades out at the shore as the water does
	ice_cover *= smoothstep(0.003, 0.03, water_depth);
	if (ice_cover > 0.0)
	{
		vec2 on_ice = world_position.xz;
		float thick = smoothstep(0.02, 0.12, ice_here.x);
		// (frost and cracks from the first: even thin it is not water to the eye)
		float frost = smoothstep(0.4, 0.8, value_noise(on_ice * 0.35) * 0.7 + value_noise(on_ice * 2.1) * 0.3) * (0.6 + 0.4 * thick);
		float crack_noise = value_noise(on_ice * 0.45) + 0.5 * value_noise(on_ice * 1.3 + 7.1);
		float crack = 1.0 - smoothstep(0.0, 0.025, abs(crack_noise - 0.75));
		float bubbles = step(0.93, value_noise(on_ice * 14.0)) * thick;
		vec3 tint = mix(vec3(1.0), vec3(0.55, 0.85, 0.9), thick);
		vec3 seen_through = mix(body * 0.4 + vec3(0.05, 0.06, 0.07), body * 0.6, thick) * tint;
		vec3 lit_through = seen_through * (sun_color * max(dot(normal, sun_direction), 0.0) * visibility * 0.6 + ambient * 1.3);
		// (frost: white, rough)
		vec3 frost_color = vec3(0.75, 0.82, 0.86) * (sun_color * max(dot(normal, sun_direction), 0.0) * visibility * 0.7 + ambient * 1.2);
		// (a dimmer mirror than the water's: its surface is never quite smooth)
		float ice_fresnel = (0.05 + 0.95 * pow(1.0 - facing, 5.0)) * 0.65;
		// (blurred where frosted: the sky's average color, not its picture)
		vec3 mirrored = mix(reflection, sky_color * ambient_intensity * 1.1, frost * 0.8);
		vec3 sharp_glint = sun_color * pow(max(dot(reflect(-sun_direction, normal), view_direction), 0.0), 400.0) * 5.0 * visibility;
		vec3 broad_glint = sun_color * pow(max(dot(reflect(-sun_direction, normal), view_direction), 0.0), 30.0) * 0.6 * visibility;
		vec3 ice_color = mix(lit_through, mirrored, ice_fresnel) + mix(sharp_glint, broad_glint, frost);

		ice_color = mix(ice_color, frost_color, frost * 0.55);
		ice_color = mix(ice_color, frost_color * 1.1, crack * (0.5 + 0.3 * thick));
		ice_color += frost_color * 0.35 * bubbles;
		color = mix(color, ice_color, ice_cover);
		alpha = mix(alpha, mix(0.7, 0.88, max(thick, frost)), ice_cover);
	}

	color = apply_tile_overlay(color, world_position, tile_edge_width);
	if (tile_overlay != 0)
	{
		// Let the overlay show through clear water as well as muddy
		alpha = max(alpha, 0.5 * smoothstep(0.002, 0.04, water_depth));
	}

	float fog_amount = haze_amount(world_position);

	color = mix(color, sky_color, clamp(fog_amount, 0.0, 1.0));
	color = apply_underwater(color, world_position, view_distance, vec3(0.0));
	// What lies under it seen through it, bent by the ripples (the scene's
	// copy before the water, with the reflections): drawn here, the water
	// opaque over it, instead of blended. The bend: the ripples' slant
	// against the calm surface's, the more the deeper the water under it
	// (a meter at most), taken to the screen; not from in front of the water
	// (a stone sticking out, the shore nearer the eye: the copy's depth
	// says), from straight under it there
	float blend = clamp(alpha, 0.0, 1.0);

	if (reflection_range.z > 0.0 && blend < 0.999)
	{
		vec2 screen_uv = gl_FragCoord.xy * depth_to_distance.zw;
		vec3 calm = normalize(surface_normal);
		vec3 bend = vec3(normal.x - calm.x, 0.0, normal.z - calm.z) * min(water_depth, 1.0) * 0.125 * reflection_range.z;
		vec4 bent_clip = world_to_clip(world_position + bend);
		vec2 bent_uv = vec2(bent_clip.x / bent_clip.w * 0.5 + 0.5, 0.5 - bent_clip.y / bent_clip.w * 0.5);
		float bent_depth = textureLod(scene_depth_texture, bent_uv, 0.0).r;
		float bent_distance = depth_to_distance.y / (bent_depth - depth_to_distance.x);

		// (the surface's own distance as the copy's depth gives it: along
		// the view, not straight from the eye)
		float surface_distance = depth_to_distance.y / (gl_FragCoord.z - depth_to_distance.x);

		if (bent_clip.w <= 0.0 || bent_distance < surface_distance || any(lessThan(bent_uv, vec2(0.0))) || any(greaterThan(bent_uv, vec2(1.0))))
		{
			bent_uv = screen_uv;
		}
		color = mix(textureLod(scene_color_texture, bent_uv, 0.0).rgb, color, blend);
		fragment_color = vec4(color, 1.0);
	}
	else
	{
		fragment_color = vec4(color, blend);
	}
#ifdef WATER_SPLIT_REFLECTIONS
	// How much of the final pixel the reflection is: through the Fresnel
	// mix, past the foam and the haze, by the water's own opacity (the
	// blend over the scene). What the march finds replaces the sky by that
	// much
	if (split_strength > 0.0)
	{
		float weight = blend * fresnel * (1.0 - foam) * (1.0 - clamp(fog_amount, 0.0, 1.0)) * split_strength;

		ray_out = vec4(split_direction, weight);
		sky_out = vec4(split_sky, view_distance);
	}
#endif
}
