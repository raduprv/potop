#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "impostor.glsl"

// An impostor's pixel (see impostor.vert): the three pictures blended
// (color, normal, depth), cut out where they are empty, then lit like an
// object without maps of its own (object.frag): the sun with the shadow
// maps and the clouds' shadows, the sky and the ground around, the local
// lights, haze. A leafy model (a tree's crown: mostly cut out materials)
// keeps the light that wraps round leaves. The depth is pushed back or forth by
// the pictures' depth, so the square meets the ground and other things
// about where the object would.

#include "shadows.glsl"
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D variation_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D albedo_atlas;           // sRGB; alpha: covered
layout(set = TEXTURE_SET, binding = 4) uniform sampler2D cloud_shadow_texture;
layout(set = TEXTURE_SET, binding = 5) uniform sampler2D normal_atlas;           // model axes * 0.5 + 0.5; alpha: depth
#define CLOUD_SHADOW_TEXTURE
#include "underwater.glsl"
#define CLOUDS_EXPLICIT_LOD
#include "clouds.glsl"
#define LIGHT_BINDING 6
#include "lights.glsl"

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec2 picture_point_0;
layout(location = 2) in vec2 picture_point_1;
layout(location = 3) in vec2 picture_point_2;
layout(location = 4) flat in ivec4 frames_01;
layout(location = 5) flat in ivec2 frame_2;
layout(location = 6) flat in vec3 weights;
layout(location = 7) flat in vec4 rotation;
layout(location = 8) flat in float fade;
layout(location = 9) flat in vec4 toward_eye;
layout(location = 10) flat in float leafy;
layout(location = 11) flat in vec4 sky_light;           // impostor.vert's: the light sent back (rgb), the sky seen (a)

layout(std140, set = UNIFORM_SET, binding = 1) uniform impostor_draw_block
{
	vec4 tint;                     // rgb; a: how much (0: none; debugging, to tell impostors from models)
};

layout(location = 0) out vec4 fragment_color;
#include "scene_depth.glsl"

// The atlases' mip levels the pictures may be read at: further down, the
// cells' neighbors bleed in
#define IMPOSTOR_MAXIMUM_LOD 3.0

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

vec3 surroundings(vec3 direction)
{
	vec3 ground_bounce = ground_bounce_color(1.0);

	return mix(ground_bounce, sky_color, direction.y * 0.5 + 0.5) * ambient_intensity;
}


float dither_threshold()
{
	const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
	ivec2 pixel = ivec2(gl_FragCoord.xy) & 3;

	return (bayer[pixel.y * 4 + pixel.x] + 0.5) / 16.0;
}

void main()
{
	scene_depth_write();
	bool highlighted = fade > 1.5;
	float drawn_fade = highlighted ? fade - 2.0 : fade;
	vec2 points[3] = vec2[3](picture_point_0, picture_point_1, picture_point_2);
	ivec2 frames[3] = ivec2[3](frames_01.xy, frames_01.zw, frame_2);
	float atlas_size = float(textureSize(albedo_atlas, 0).x);
	// The mip level, from how fast the first picture's point moves across
	// the pixels (the three move alike)
	vec2 texel_ddx = dFdx(picture_point_0) * 0.5 * atlas_size / float(IMPOSTOR_GRID);
	vec2 texel_ddy = dFdy(picture_point_0) * 0.5 * atlas_size / float(IMPOSTOR_GRID);
	float lod = clamp(0.5 * log2(max(dot(texel_ddx, texel_ddx), dot(texel_ddy, texel_ddy))), 0.0, IMPOSTOR_MAXIMUM_LOD);
	float coverage = 0.0;
	vec3 color = vec3(0.0);
	vec4 normal_depth = vec4(0.0);

	if (drawn_fade < 1.0 && drawn_fade < dither_threshold())
	{
		discard;
	}
	for (int k = 0; k < 3; k++)
	{
		if (weights[k] <= 0.0 || abs(points[k].x) > 1.0 || abs(points[k].y) > 1.0)
		{
			continue;
		}
		vec2 uv = impostor_atlas_uv(frames[k], points[k]);
		vec4 albedo_sample = textureLod(albedo_atlas, uv, lod);
		vec4 normal_sample = textureLod(normal_atlas, uv, lod);
		float weight = weights[k] * albedo_sample.a;

		coverage += weights[k] * albedo_sample.a;
		color += albedo_sample.rgb * weight;
		normal_depth += vec4(normal_sample.rgb * 2.0 - 1.0, normal_sample.a) * weight;
	}
	// Cut out as the leaves are (object.frag: the alpha boosted with the
	// mip level)
	float cut_alpha = alpha_cutout(coverage * (1.0 + 0.25 * lod), 0.5);

	// (with alpha to coverage a texel all but bare can stay, a sample or
	// two of it: no division by nothing, its NaN bloomed into blotches)
	color /= max(coverage, 1e-4);
	normal_depth /= max(coverage, 1e-4);

	vec3 normal = impostor_rotate(rotation, normal_depth.xyz / max(length(normal_depth.xyz), 1e-4));
	// The pictures' depth: 0 the sphere's near side, 1 its far side
	vec3 position = world_position + toward_eye.xyz * (0.5 - normal_depth.w) * 2.0 * toward_eye.w;
	vec4 clip = world_to_clip(position);

	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);

	vec3 albedo = pow(color, vec3(2.2));
	float facing_sun = dot(normal, sun_direction);
	float diffuse = mix(max(facing_sun, 0.0), max(facing_sun * 0.6 + 0.4, 0.0) + 0.35 * max(-facing_sun, 0.0), leafy);   // (plant.frag's LEAF_SUN_THROUGH)
	// The shadow maps hold the real model: looked up a little towards the
	// sun, so the pictures' rough depth does not shadow itself
	vec3 shadow_point = position + sun_direction * 0.5;
	vec4 shadow_position = light_view_projection * vec4(shadow_point, 1.0);
	vec4 near_shadow_position = near_light_view_projection * vec4(shadow_point, 1.0);
	float shadowed = sun_visibility(shadow_position, near_shadow_position) * cloud_light(position, sun_direction);
	vec3 lit = albedo * (sun_color * diffuse * shadowed + (surroundings(normal) * sky_light.a + sky_light.rgb) + local_light(position, normal, leafy * 0.4));
	float view_distance = length(position - eye_position);

	lit = mix(lit, sky_color, clamp(haze_amount(position), 0.0, 1.0));
	lit = apply_underwater(lit, position, view_distance, sun_color * diffuse * shadowed);
	fragment_color = vec4(mix(lit, tint.rgb * (0.3 + dot(lit, vec3(0.3, 0.6, 0.1))), tint.a), cut_alpha);
}
