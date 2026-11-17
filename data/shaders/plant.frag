#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A plant's fragment shader (tools/potop_plant's plants; plant.vert): lit
// as the terrain is (the sun through the shadow maps and the clouds'
// shadows, the sky and the ground around, the lamps; the haze, under the
// water), without object.frag's physically based shading: a plant has no
// use for it, and its foliage is most of what is drawn.
//
// The bark: its normal map, if it has one, and moss on the side that faces
// the north (render.sun_azimuth_degrees' opposite: the aurora's north),
// the more the thicker the branch and the lower, patchy.
//
// The foliage, cut out by its alpha (its picture for the month: plant.vert
// moves it to that one's cell), lit from either side and the light
// wrapping round, the sun shining through it from behind; darker deep in
// the crown (the tangent's size, tools/potop_plant); each card a little
// lighter or darker than the next, its tip lighter (the new growth); in
// the autumn turned to its material's colors, a card at a time (how far:
// plant.vert), keeping the picture's light and dark.
//
// The winter's snow on the bark: on what faces up, in patches (the
// foliage's is in its pictures).
//
// Fading out near the draw distance and towards the next level of detail
// in a dither, highlighted, wet in the rain: as object.frag.

#include "shadows.glsl"
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D variation_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D color_texture;
layout(set = TEXTURE_SET, binding = 4) uniform sampler2D cloud_shadow_texture;  // the volumetric clouds' shadows (clouds.glsl)
layout(set = TEXTURE_SET, binding = 5) uniform sampler2D normal_texture;        // tangent space, XYZ * 0.5 + 0.5
layout(set = TEXTURE_SET, binding = 6) uniform sampler2D unused_orm_texture;    // object.frag's place: the plants take none
layout(set = TEXTURE_SET, binding = 7) uniform sampler2D unused_emissive_texture;
#define CLOUD_SHADOW_TEXTURE
#include "underwater.glsl"

layout(std140, set = UNIFORM_SET, binding = 1) uniform plant_material_block
{
	vec4 highlight_parameters;            // the highlight's color (sRGB 0..1), time (seconds, for the pulse)
	vec4 autumn_color_a;                  // rgb: what the foliage turns to (linear), one end
	vec4 autumn_color_b;                  // rgb: the other end
	vec4 parameters;                      // x: the moss (bark), y: the lighter tips (foliage), z: 1 cut out (foliage), w: 1 lit by its normals (cards)
	vec4 more;                            // x: 1 if it has a normal map of its own; y: the snow on the bark (0..1); z: 1 the snow lying on its boughs (OBJECT_FILE_SNOW)
};
#define CLOUDS_EXPLICIT_LOD
#include "clouds.glsl"
#define GROUND_BINDING 8
#include "ground_water.glsl"
#define LIGHT_BINDING 13

// How much more moss where the sky is hidden (sky_occlusion.glsl)
const float MOSS_IN_SHADE = 1.2;

// How much of the sun a leaf turned away from it lets through
const float LEAF_SUN_THROUGH = 0.35;
#include "lights.glsl"

// (centroid: as grass.frag's, the thin leaves' pixels partly covered shaded
// inside them)
layout(location = 0) centroid in vec3 world_position;
layout(location = 1) centroid in vec3 world_normal;
layout(location = 2) centroid in vec2 texture_uv;
layout(location = 3) centroid in vec4 shadow_position;
layout(location = 4) centroid in vec4 near_shadow_position;
layout(location = 5) flat in float fade;
layout(location = 6) centroid in vec4 world_tangent;
layout(location = 7) flat in float level_drops;
layout(location = 8) centroid in vec4 plant_info;              // along, height, its own number, out in the crown or thickness
layout(location = 9) centroid in float turned;
layout(location = 11) centroid in float haze;                    // plant.vert's haze_amount
layout(location = 10) centroid in vec4 sky_light;               // plant.vert's: the light the land round sends back (rgb), the sky seen (a)

layout(location = 0) out vec4 fragment_color;
#include "scene_depth.glsl"

// As object.frag's
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

vec3 surroundings(vec3 direction)
{
	vec3 ground_bounce = ground_bounce_color(1.0);

	return mix(ground_bounce, sky_color, direction.y * 0.5 + 0.5) * ambient_intensity * sky_light.a;
}

float dither_threshold()
{
	const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
	ivec2 pixel = ivec2(gl_FragCoord.xy) & 3;

	return (bayer[pixel.y * 4 + pixel.x] + 0.5) / 16.0;
}

float luminance(vec3 color)
{
	return dot(color, vec3(0.3, 0.59, 0.11));
}

void main()
{
	scene_depth_write();
	// The fade, the highlight and the level of detail, as object.frag has them
	// (DRY_FADE added: no water anywhere near it, ground_water.glsl)
	float dry = floor(fade / DRY_FADE);
	float packed_fade = fade - DRY_FADE * dry;
	float level_steps = floor(packed_fade / 4.0);
	float own_fade = packed_fade - 4.0 * level_steps;
	bool highlighted = own_fade > 1.5;
	float drawn_fade = highlighted ? own_fade - 2.0 : own_fade;
	bool cut_out = parameters.z > 0.5;

	if (drawn_fade < 1.0 && drawn_fade < dither_threshold())
	{
		discard;
	}
	if (level_drops > 0.5 && level_steps > 0.0 && 1.0 - level_steps / 255.0 < dither_threshold())
	{
		discard;
	}
	vec4 color = texture(color_texture, texture_uv);
	vec3 normal = normalize(world_normal);
	vec3 towards_eye = normalize(eye_position - world_position);
	vec3 albedo;
	float diffuse;
	float shade = 1.0;
	float through = 0.0;
	// (the cut out edge's coverage: alpha_cutout)
	float cut_alpha = 1.0;

	bool snow_cover = more.z > 0.5;

	if (snow_cover)
	{
		// The snow lying on its boughs: its edge ragged where the bough's
		// picture under it is (blurred: it bridges the gaps between the
		// shoots), white, a little blue in its hollows, glittering
		float under = textureLod(color_texture, texture_uv, 3.0).a;
		float grain = textureLod(variation_texture, world_position.xz * 3.1 + world_position.y * 1.7, 0.0).r;
		float fine = textureLod(variation_texture, world_position.xz * 23.0 + world_position.y * 11.0, 0.0).r;
		// How deep it lies here, of the most (tools/plant_grow.c: the
		// plant data's last): where thin, the needles show through it
		float thick = clamp(plant_info.w * 1.6, 0.0, 1.0);

		if (under < 0.25 + 0.2 * grain || fine > 0.25 + 2.2 * thick)
		{
			discard;
		}
		// Its surface lumpy (small lumps, the grains of it)
		{
			float lump_x = textureLod(variation_texture, world_position.xz * 7.0 + 0.31, 0.0).r - 0.5;
			float lump_z = textureLod(variation_texture, world_position.zx * 7.0 + 0.67, 0.0).r - 0.5;

			normal = normalize(normal + vec3(lump_x, 0.0, lump_z) * 0.9);
		}
		// White on top, grey blue where it is thin, at its edges and in its
		// hollows (the light gets through, the needles' green under it)
		albedo = mix(vec3(0.42, 0.50, 0.58), vec3(0.9, 0.92, 0.95), smoothstep(0.05, 0.55, thick) * clamp(normal.y * 0.7 + 0.35, 0.0, 1.0))
			* (0.92 + 0.12 * grain);
		// (it lets light in: wrapped round a little)
		diffuse = max(dot(normal, sun_direction) * 0.7 + 0.3, 0.0);
	}
	else if (cut_out)
	{
		// Mip level from the screen space size of a texel: the alpha kept up
		// far away, or the far crowns go bald
		vec2 texel_position = texture_uv * vec2(textureSize(color_texture, 0));
		vec2 texel_ddx = dFdx(texel_position);
		vec2 texel_ddy = dFdy(texel_position);
		float mip_level = max(0.0, 0.5 * log2(max(dot(texel_ddx, texel_ddx), dot(texel_ddy, texel_ddy))));

		cut_alpha = alpha_cutout(color.a * (1.0 + 0.25 * mip_level), 0.5);
		if (!gl_FrontFacing && parameters.w < 0.5)
		{
			normal = -normal;
		}
		albedo = pow(color.rgb, vec3(2.2));
		// Each card its own shade of green, its tip a little lighter and
		// yellower: the new growth (the conifers' most). Subtle: as light as
		// the leaf itself, a quarter more (a dark needle's tip stays dark,
		// only greener), half of it at most over the picture's own
		{
			float tip = smoothstep(0.5, 0.97, plant_info.x) * parameters.y;
			// (the yellow green's hue, at the leaf's own lightness)
			vec3 young = vec3(0.16, 0.34, 0.035) / 0.28 * luminance(albedo) * 1.25;

			albedo *= 0.88 + 0.24 * plant_info.z;
			albedo = mix(albedo, young, clamp(tip * 0.6, 0.0, 0.5));
		}
		// The autumn: its color, keeping the picture's light and dark
		if (turned > 0.0)
		{
			vec3 autumn = mix(autumn_color_a.rgb, autumn_color_b.rgb, fract(plant_info.z * 13.7));
			vec3 recolored = autumn * (luminance(albedo) / max(luminance(autumn), 0.01));

			// A little muted, as the leaves are (not a fire)
			recolored = mix(recolored, vec3(luminance(recolored)), 0.2);
			albedo = mix(albedo, recolored, clamp(turned, 0.0, 1.0));
		}
		// (a leaf turned away from the sun still lets some of it through:
		// its normal is the crown's or the sky's, not its own, so the far
		// side of a plant would go dark when the sun is low)
		{
			float facing_sun = dot(normal, sun_direction);

			diffuse = max(facing_sun * 0.6 + 0.4, 0.0) + LEAF_SUN_THROUGH * max(-facing_sun, 0.0);
		}
		// Deep in the crown (the tangent's size, tools/potop_plant)
		{
			float tangent_size = abs(world_tangent.w);

			shade = tangent_size > 1e-3 ? min(tangent_size, 1.5) : 1.0;
		}
		// The sun through the leaves, seen against it
		through = pow(max(dot(-towards_eye, sun_direction), 0.0), 3.0) * 0.45;
	}
	else
	{
		// The bark: its normal map, if any
		if (more.x > 0.5)
		{
			vec3 tangent = world_tangent.xyz - normal * dot(normal, world_tangent.xyz);

			if (dot(tangent, tangent) > 1e-8)
			{
				vec3 bumped = texture(normal_texture, texture_uv).xyz * 2.0 - 1.0;
				vec3 bitangent;

				tangent = normalize(tangent);
				bitangent = cross(normal, tangent) * (world_tangent.w < 0.0 ? -1.0 : 1.0);
				normal = normalize(tangent * bumped.x + bitangent * bumped.y + normal * bumped.z);
			}
		}
		albedo = pow(color.rgb, vec3(2.2));
		// Moss on the north side, the more the thicker the branch and the
		// lower down, in patches
		if (parameters.x > 0.0)
		{
			vec2 north = aurora.xy;
			float facing = dot(normalize(normal.xz + vec2(1e-5)), north) * length(normal.xz);
			float patches = textureLod(variation_texture, world_position.xz * 0.37 + world_position.y * 0.21, 0.0).r;
			float amount = parameters.x * smoothstep(-0.25, 0.5, facing) * (0.35 + 0.65 * clamp(plant_info.w, 0.0, 1.0))
				* (0.45 + 0.55 * (1.0 - plant_info.y)) * smoothstep(0.25, 0.65, patches)
				// (more where the sky is hidden: deep in a wood, in a hollow)
				* (1.0 + MOSS_IN_SHADE * sky_hidden_above(world_position.xz, world_position.y - texelFetch(ground_height_texture,
					clamp(ivec2(floor(world_position.xz / ground_map.y + 0.5)), ivec2(0), ivec2(int(ground_map.x) - 1)), 0).x));
			// A fresh, saturated moss green (linear), lighter and darker in patches
			vec3 moss = vec3(0.09, 0.24, 0.025) * (0.7 + 0.6 * patches);

			albedo = mix(albedo, moss, clamp(amount * 1.6, 0.0, 0.95));
		}
		diffuse = max(dot(normal, sun_direction), 0.0);
	}
	// The snow on the bark: on what faces up, in patches
	if (more.y > 0.0 && !cut_out && !snow_cover)
	{
		float patches = textureLod(variation_texture, world_position.xz * 0.53 + world_position.y * 0.31, 0.0).r;
		float cover = clamp(more.y * 1.6 * smoothstep(0.3, 0.8, normal.y) - (1.0 - patches) * 0.6, 0.0, 1.0)
			* (1.0 - smoothstep(0.0, 0.03, under_water_depth(world_position)));
		vec3 snow = vec3(0.85, 0.88, 0.92) * (0.75 + 0.25 * clamp(luminance(albedo) * 6.0, 0.0, 1.0));

		albedo = mix(albedo, snow, smoothstep(0.05, 0.45, cover));
	}
	// Wet in the rain (rain.h): darker, the more it faces up, and shinier
	float wet = snow_cover ? 0.0 : weather.y * (0.35 + 0.65 * clamp(normal.y, 0.0, 1.0));
	// The water there now (ground_water.glsl): wet under it
	int water_kind = cut_out ? WATER_FOLIAGE : WATER_BARK;
	water_at_t water = water_at(world_position, dry > 0.5);

	apply_water_wet(water, water_kind, wet);

	albedo *= 1.0 - 0.3 * wet;
	float sun_shade = 0.5 + 0.5 * shade;      // the sun reaches in more than the sky does
	// (bark turned from the sun gets none of it, whatever the shadow maps
	// say: they are not looked up. Not for leaves and the snow on them,
	// which let it through, nor wet bark, which shines, nor where there is
	// water about, whose light plays on it)
	bool sun_reaches = cut_out || snow_cover || diffuse > 0.0 || wet > 0.0 || water.to_surface < 1e5;
	// (the leaves: the far map alone, looked up out of their own crown,
	// plant.vert)
	float shadowed = sun_reaches ? (cut_out ? sun_visibility_far(shadow_position) : sun_visibility(shadow_position, near_shadow_position))
		* cloud_light(world_position, sun_direction) : 0.0;
	// What shines through is yellower than what it lights
	vec3 around = surroundings(normal) + sky_light.rgb;
	vec3 lit = albedo * (sun_color * (diffuse * sun_shade + through * shade * vec3(1.1, 1.15, 0.7)) * shadowed
		+ (around + local_light(world_position, normal, cut_out ? 0.4 : 0.0)) * shade);

	if (wet > 0.0)
	{
		float shine = pow(max(dot(reflect(-sun_direction, normal), towards_eye), 0.0), 40.0);

		lit += sun_color * shadowed * shine * wet * 0.25;
	}
	if (snow_cover)
	{
		// (its crystals: a sparkle here and there towards the sun)
		float glint = step(0.985, fract(sin(dot(floor(world_position * 90.0), vec3(12.9898, 78.233, 37.719))) * 43758.5453));

		lit += sun_color * shadowed * glint * pow(max(dot(reflect(-sun_direction, normal), towards_eye), 0.0), 8.0) * 0.8;
	}
	lit += water_light(water, world_position, sun_color * shadowed);
	lit = mix(lit, sky_color, haze);
	lit = apply_underwater(lit, world_position, length(world_position - eye_position), sun_color * diffuse * shadowed);
	if (highlighted)
	{
		float facing = abs(dot(normal, towards_eye));
		float rim = pow(1.0 - facing, 2.0);
		float pulse = 0.65 + 0.35 * sin(highlight_parameters.w * 5.0);

		lit += highlight_parameters.rgb * (0.18 + 1.1 * rim) * pulse;
	}
	fragment_color = vec4(lit, cut_alpha);
}
