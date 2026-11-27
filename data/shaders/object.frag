#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// 3D object fragment shader (objects and actors): textured, lit like the
// terrain (sun with shadow map and cloud shadows, sky / ground ambient,
// distance fog).
//
// With PBR shading on (render.pbr_shading), physically based: the normal
// map bends the normal (in the tangent frame the vertex shader hands over),
// occlusion-roughness-metal shape the light (Lambert for the diffuse part,
// GGX with Smith's shadowing and Schlick's Fresnel for the sun's
// reflection; the sky and the ground around reflected through an analytic
// fit of the split sum, Karis'), and the emissive map adds the surface's own
// light. Models without those maps get neutral ones (a flat normal, not
// metal, and no reflection at all: the neutral occlusion-roughness-metal
// map's alpha is 0): they look exactly as before. Their textures were
// painted for diffuse light alone; a sheen of the sky on them (dark
// needles above all) turns them gray blue. The light keeps the
// game's convention: the sun's color is what a white surface facing it
// shows (no factor of pi).
//
// Alpha tested materials (leaves) are cut out where the texture's alpha is
// low and lit from both sides. Mipmapping averages the alpha of leaves with
// the empty space between them, so far trees would go bald; the alpha is
// boosted with the mip level to keep their coverage.
//
// Near their draw distance objects fade out: a growing share of their
// pixels is dropped in a fixed 4x4 pattern (ordered dither), which looks
// like transparency at a distance without any sorting or blending.

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
	int material_padding;
	vec4 deciduous;                       // a climber dropping its leaves (OBJECT_MATERIAL_DECIDUOUS): x how red, y how many gone, z 1 if it is one; w 1: a building's detail (its pixels marked for the temporal anti-aliasing)
	vec4 material_show;                   // (prop.frag's: unused here)
	vec4 material_relief;
	vec4 material_look[2];
	// The fires (burning.h), OBJECT_BURN_SLOTS of 4 each: the object's
	// origin and 1 / its size; its turn undone (quaternion); where it was
	// lit (model space, turned so that its longest side lies along X: the
	// grain) and how far the front has gone (model meters);
	// the front's speed, char, burn and ash seconds
	vec4 burns[32];
};
#define OBJECT_BURN_SLOTS 8
#define CLOUDS_EXPLICIT_LOD
#include "clouds.glsl"
#define GROUND_BINDING 8
#include "ground_water.glsl"
#define LIGHT_BINDING 13
#include "lights.glsl"
#include "decals.glsl"

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec2 texture_uv;
layout(location = 3) in vec4 shadow_position;
layout(location = 4) in vec4 near_shadow_position;
layout(location = 5) flat in float fade;
layout(location = 6) in vec4 world_tangent;
// 1 where this part goes away at the next level of detail (object.vert)
layout(location = 7) flat in float level_drops;
// Its emissive groups put out (object_format.h, OBJECT_FILE_EMISSIVE_GROUPS):
// a vertex's group the whole part of its U, group g's bit g - 1
layout(location = 8) flat in uvec2 emissive_off;

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

vec3 surroundings(vec3 direction)
{
	vec3 ground_bounce = ground_bounce_color(ground_sunlit);

	return mix(ground_bounce, sky_color, direction.y * 0.5 + 0.5) * ambient_intensity * sky_here;
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

// --- On fire (burning.h) ---------------------------------------------------

float burn_hash(vec3 cell)
{
	return fract(sin(dot(cell, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float burn_noise(vec3 at)
{
	vec3 cell = floor(at);
	vec3 inside = at - cell;
	vec3 blend = inside * inside * (3.0 - 2.0 * inside);

	return mix(mix(mix(burn_hash(cell), burn_hash(cell + vec3(1.0, 0.0, 0.0)), blend.x),
		mix(burn_hash(cell + vec3(0.0, 1.0, 0.0)), burn_hash(cell + vec3(1.0, 1.0, 0.0)), blend.x), blend.y),
		mix(mix(burn_hash(cell + vec3(0.0, 0.0, 1.0)), burn_hash(cell + vec3(1.0, 0.0, 1.0)), blend.x),
		mix(burn_hash(cell + vec3(0.0, 1.0, 1.0)), burn_hash(cell + vec3(1.0, 1.0, 1.0)), blend.x), blend.y), blend.z);
}

vec3 burn_rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

// What a fire did to this point (slot: its fire's): seconds since the
// front reached it (below 0: not yet) and where it is in the model
float burn_since(int slot, out vec3 model_position)
{
	vec4 placement = burns[slot * 4];
	vec4 undo = burns[slot * 4 + 1];
	vec4 front = burns[slot * 4 + 2];
	vec4 timing = burns[slot * 4 + 3];

	model_position = burn_rotate(undo, world_position - placement.xyz) * placement.w;
	// (a ragged front, not a sphere)
	float reached = length(model_position - front.xyz) + (burn_noise(model_position * 5.0) - 0.5) * 0.3
		+ (burn_noise(model_position * 17.0) - 0.5) * 0.08;

	return (front.w - reached) / max(timing.x, 1e-4);
}

// Threshold of the pixel in a 4x4 Bayer matrix, in (0, 1)
float dither_threshold()
{
	const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
	ivec2 pixel = ivec2(gl_FragCoord.xy) & 3;

	return (bayer[pixel.y * 4 + pixel.x] + 0.5) / 16.0;
}

void main()
{
	scene_depth_write();
	// The sky the land round leaves it (sky_occlusion.glsl), looked up a
	// little out from its face: the occlusion was drawn with this very
	// object in it, and right at its face that is its own body hiding half
	// the sky, darker the lower (its own shading has that already)
	{
		vec2 out_flat = world_normal.xz;
		float out_length = length(out_flat);

		// (a face looking up or down: no way out to speak of, looked up
		// where it is)
		sky_here = object_sky_seen(world_position + vec3(out_length > 0.3 ? out_flat / out_length * SKY_LOOKUP_OUT_METERS : vec2(0.0), 0.0).xzy);
		ground_sunlit = sky_here;
	}
	// A highlighted object comes with 2 added to its fade (see objects.h),
	// and 4 times how much (of 255) of what its next level of detail leaves
	// out is gone (objects_cull.comp)
	// (DRY_FADE added: no water anywhere near it, ground_water.glsl)
	// (and BURN_FADE times its fire's slot + 1: burning.h)
	float burn_number = floor(fade / BURN_FADE);
	float unburnt_fade = fade - BURN_FADE * burn_number;
	float dry = floor(unburnt_fade / DRY_FADE);
	float packed_fade = unburnt_fade - DRY_FADE * dry;
	float level_steps = floor(packed_fade / 4.0);
	float own_fade = packed_fade - 4.0 * level_steps;
	bool highlighted = own_fade > 1.5;
	float drawn_fade = highlighted ? own_fade - 2.0 : own_fade;

	if (drawn_fade < 1.0 && drawn_fade < dither_threshold())
	{
		discard;
	}
	// On fire: how long ago the front reached this point; crumbled away
	// once its ash time is past (in a noise, bit by bit)
	int burn_slot = int(burn_number) - 1;
	float burn_seconds_since = -1e6;
	vec3 burn_model = vec3(0.0);
	vec4 burn_timing = vec4(1.0);

	if (burn_slot >= 0 && burn_slot < OBJECT_BURN_SLOTS)
	{
		burn_timing = burns[burn_slot * 4 + 3];
		burn_seconds_since = burn_since(burn_slot, burn_model);
		if (burn_seconds_since > burn_timing.z)
		{
			float gone = (burn_seconds_since - burn_timing.z) / max(burn_timing.w, 1e-3);

			if (burn_noise(burn_model * 9.0) * 0.7 + burn_noise(burn_model * 31.0) * 0.3 < gone)
			{
				discard;
			}
		}
	}
	// Nearing the next level of detail, what it leaves out (the finest
	// branches) thins out in a dither, so it does not pop away
	if (level_drops > 0.5 && level_steps > 0.0 && 1.0 - level_steps / 255.0 < dither_threshold())
	{
		discard;
	}
	vec4 color = texture(color_texture, texture_uv);
	vec3 normal = normalize(world_normal);
	float diffuse;
	// (the cut out edge's coverage: alpha_cutout)
	float cut_alpha = 1.0;

	if (alpha_tested != 0)
	{
		// Mip level from the screen space size of a texel
		vec2 texel_position = texture_uv * vec2(textureSize(color_texture, 0));
		vec2 texel_ddx = dFdx(texel_position);
		vec2 texel_ddy = dFdy(texel_position);
		float mip_level = max(0.0, 0.5 * log2(max(dot(texel_ddx, texel_ddx), dot(texel_ddy, texel_ddy))));

		cut_alpha = alpha_cutout(color.a * (1.0 + 0.25 * mip_level), 0.5);
		// A deciduous climber: its leaves (the green of it, by how much
		// greener than red) redden, then fall leaf by leaf (a noise of
		// their size: not pixel by pixel), the brown vines staying
		if (deciduous.z > 0.5)
		{
			float leafy = smoothstep(-0.02, 0.06, color.g - color.r);
			float which = fract(sin(dot(floor(texture_uv * 48.0), vec2(12.9898, 78.233))) * 43758.5453);

			if (leafy > 0.5 && which < deciduous.y)
			{
				discard;
			}
			vec3 autumn = mix(vec3(0.62, 0.30, 0.04), vec3(0.55, 0.06, 0.04), clamp(which * 1.4, 0.0, 1.0)) * (dot(color.rgb, vec3(0.3, 0.59, 0.11)) * 2.6 + 0.08);

			color.rgb = mix(color.rgb, autumn, leafy * deciduous.x);
		}
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
	// On fire (burning.h): scorched a little ahead of the front, then
	// charred black, then glowing through as solid embers (a coal's glow,
	// mottled, hotter and cooler patches drifting, a thin black skin coming
	// and going over it), dying down to gray ash spreading in patches; dull
	// all over
	float burn_char = 0.0;
	vec3 burn_glow = vec3(0.0);

	if (burn_seconds_since > -8.0)
	{
		float char_seconds = max(burn_timing.y, 1e-3);
		float burn_seconds = max(burn_timing.z, 1e-3);
		float time = highlight_parameters.w;
		float scorch = smoothstep(-8.0, 0.0, burn_seconds_since);
		// (long along the grain: the model turned so that it runs along X)
		vec3 grain = burn_model * vec3(3.0, 8.0, 8.0);
		float patches = burn_noise(grain) * 0.6 + burn_noise(grain * 2.3 + 7.1) * 0.3 + burn_noise(grain * 5.7 + 3.3) * 0.1;
		// (the ash comes in patches, the coolest first)
		float ash_progress = smoothstep(burn_seconds * 0.7, burn_seconds * 1.02, burn_seconds_since);
		float ash = smoothstep(patches + 0.1, patches + 0.3, ash_progress * 1.3);
		vec3 char_color = vec3(0.010, 0.009, 0.008) * (0.7 + 0.6 * patches);
		vec3 ash_color = vec3(0.30, 0.29, 0.28) * (0.75 + 0.5 * burn_noise(grain * 4.0));

		burn_char = smoothstep(0.0, char_seconds, burn_seconds_since + char_seconds * 0.2 * (patches - 0.5));
		albedo *= 1.0 - 0.45 * scorch * (1.0 - burn_char);
		albedo = mix(albedo, char_color, burn_char);
		albedo = mix(albedo, ash_color, ash);
		// The embers: from the time it is charred enough until it is burnt
		// out, over all of it but the ash
		{
			float burning_now = smoothstep(char_seconds * 0.3, char_seconds, burn_seconds_since)
				* (1.0 - smoothstep(burn_seconds * 0.6, burn_seconds, burn_seconds_since));
			// (hot and cool patches drifting slowly, a quicker shimmer on top)
			float heat = burn_noise(grain * 1.5 + vec3(time * 0.08, time * 0.05, 0.0)) * 0.7
				+ burn_noise(grain * 4.0 + vec3(0.0, time * 0.4, time * 0.3)) * 0.3;
			// (a thin black skin over the coolest, coming and going)
			float skin = smoothstep(0.38, 0.22, heat + 0.15 * (burn_noise(grain * 9.0 + time * 0.2) - 0.5));
			vec3 ember_color = mix(vec3(0.55, 0.05, 0.01), vec3(1.0, 0.38, 0.06), smoothstep(0.3, 0.85, heat));
			float strength = mix(0.25, 2.2, smoothstep(0.2, 0.9, heat)) * (1.0 - 0.85 * skin);

			burn_glow = ember_color * strength * burning_now * (1.0 - ash);
		}
	}
	// Wet in the rain (rain.h): darker, the more the surface faces up, and
	// shinier (below)
	float wet = weather.y * (0.35 + 0.65 * clamp(normal.y, 0.0, 1.0));
	// The water there now (ground_water.glsl): wet under it
	int water_kind = alpha_tested != 0 ? WATER_FOLIAGE : WATER_SOLID;
	water_at_t water = water_at(world_position, dry > 0.5);

	apply_water_wet(water, water_kind, wet);

	albedo *= 1.0 - 0.3 * wet;
	// The snow on it (weather.w, main.c's): on what faces up, in patches
	// (less where it slopes), not on leaves and stalks (cut out: they shake
	// it off). Matte where it lies, and the rain's shine gone with it
	float snow = 0.0;

	if (weather.w > 0.0 && alpha_tested == 0)
	{
		float patches = texture(variation_texture, world_position.xz * 0.21 + world_position.y * 0.07).r;
		float up = normalize(world_normal).y;

		snow = weather.w * smoothstep(0.35, 0.75, up + (patches - 0.5) * 0.5);
		// (none under water)
		snow *= 1.0 - smoothstep(0.0, 0.03, under_water_depth(world_position));
		snow = smoothstep(0.15, 0.6, snow);
		albedo = mix(albedo, pow(vec3(0.85, 0.87, 0.9), vec3(2.2)), snow);
		wet *= 1.0 - snow;
	}
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
		vec4 orm = texture(orm_texture, texture_uv);
		float reflecting = orm.a * (1.0 - 0.8 * burn_char);
		float occlusion = orm.r;
		float roughness = clamp(mix(mix(orm.g, 0.95, burn_char), 0.6, snow), 0.045, 1.0);
		float metal = orm.b * (1.0 - snow);
		vec3 reflectance = mix(vec3(0.04), albedo, metal);

		// The normal map, in the tangent frame (none where the model has no
		// usable tangent)
		if (dot(tangent, tangent) > 1e-8)
		{
			vec3 bumped = texture(normal_texture, texture_uv).xyz * 2.0 - 1.0;

			tangent = normalize(tangent);
			// (the frame turns with the normal on a leaf's back)
			vec3 bitangent = cross(normal, tangent) * (world_tangent.w < 0.0 ? -1.0 : 1.0);

			// (the snow smooths it over)
			bumped = normalize(mix(bumped, vec3(0.0, 0.0, 1.0), 0.7 * snow));
			normal = normalize(tangent * bumped.x + bitangent * bumped.y + normal * bumped.z);
		}
		apply_decals(world_position, normalize(world_normal), albedo, normal, roughness);
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
			vec3 direct = ((1.0 - fresnel) * (1.0 - metal) * albedo * diffuse_light + specular * PI * normal_light) * sun_color * shadowed;
			vec2 split = environment_brdf(normal_view, roughness);
			vec3 around = albedo * (1.0 - metal) * (surroundings(normal) + object_bounce(world_position, normal, sky_here))
				+ surroundings(reflect(-view, normal)) * (reflectance * split.x + split.y) * reflecting;
			// (lit at night: drowned by day, as the lamps are)
			vec3 own_light = pow(texture(emissive_texture, texture_uv).rgb, vec3(2.2)) * EMISSIVE_STRENGTH * mix(EMISSIVE_DAY, 1.0, light_slicing.w);
			// (its group put out: none; the U's whole part, a little in from
			// it, so a group's own edge stays in it)
			int emissive_group = int(floor(texture_uv.x + 0.0005));

			if (emissive_group >= 1 && emissive_group <= 64 && (emissive_off[(emissive_group - 1) >> 5] & (1u << uint((emissive_group - 1) & 31))) != 0u)
			{
				own_light = vec3(0.0);
			}

			lit = direct * sun_shade + around * occlusion * shade + own_light
				+ (1.0 - metal) * albedo * local_light(world_position, normal, alpha_tested != 0 ? 0.4 : 0.0) * occlusion * shade;
			sun = normal_light * shadowed;
		}
	}
	else
	{
		lit = albedo * (sun_color * sun * sun_shade + (surroundings(normal) + object_bounce(world_position, normal, sky_here) + local_light(world_position, normal, alpha_tested != 0 ? 0.4 : 0.0)) * shade);
	}
	if (wet > 0.0)
	{
		vec3 towards_eye = normalize(eye_position - world_position);
		float shine = pow(max(dot(reflect(-sun_direction, normal), towards_eye), 0.0), 40.0);

		lit += sun_color * shadowed * shine * wet * 0.25;
	}
	// (a fire's embers: its own light)
	lit += burn_glow;
	float view_distance = length(world_position - eye_position);
	lit += water_light(water, world_position, sun_color * shadowed);
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
	fragment_color = vec4(lit, scene_alpha(cut_alpha, deciduous.w > 0.5 ? 0.25 : 1.0));
}
