#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// The volumetric fog onto the scene (see src/fog.h): each pixel looks up,
// at its distance, the light the fog sends it and how much of the scene
// still shows (fog_integrate.comp). Premultiplied.

layout(set = TEXTURE_SET, binding = 0) uniform sampler3D integrated_texture;
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D depth_texture;
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D rays_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D valley_texture;
layout(set = TEXTURE_SET, binding = 4) uniform sampler2DShadow far_shadow_texture;
#include "fog.glsl"

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;

void main()
{
	float depth = texelFetch(depth_texture, ivec2(gl_FragCoord.xy), 0).r;
	// (the sky: as far as the map goes)
	float distance_meters = 1e9;

	if (underwater_color.a > 0.5)
	{
		discard;
	}
	if (depth < 1.0)
	{
		distance_meters = length(clip_to_eye(screen_point, depth * 2.0 - 1.0));
	}
	float slices = fog_slices.z;
	float along = log(max(distance_meters, fog_slices.x) / fog_slices.x) * fog_grid.z;
	vec3 coordinates = vec3(gl_FragCoord.xy / vec2(textureSize(depth_texture, 0)), clamp(along - 0.5 / slices, 0.5 / slices, 1.0 - 0.5 / slices));
	vec4 fog = textureLod(integrated_texture, coordinates, 0.0);

	// Nearer than the first slice's end: that much of it
	fog = mix(vec4(0.0, 0.0, 0.0, 1.0), fog, clamp(along * slices, 0.0, 1.0));
	// Beyond the froxels, seen through them
	if (distance_meters > fog_slices.y)
	{
		vec4 beyond = fog_beyond(eye_direction(screen_point), distance_meters, gl_FragCoord.xy, valley_texture,
			far_shadow_texture);

		fog.rgb += fog.a * beyond.rgb;
		fog.a *= beyond.a;
	}
	fragment_color = vec4(fog.rgb + fog_rays_light(rays_texture, gl_FragCoord.xy, vec2(textureSize(depth_texture, 0)), 1.0 - fog.a), 1.0 - fog.a);
}
