#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// What the puddles mirror (puddles.h): over the screen, after the scene is
// drawn and copied (the water's copy). A pixel whose depth's point lies on
// the ground (at its height there: not a blade, a leaf, a boot over it),
// out of the water (the water mirrors itself) and in a puddle
// (puddles.glsl, as the terrain found it) gets the water's march from the
// flat surface: what it finds, blended over by Fresnel, the puddle and how
// sure the march is. Elsewhere, and where nothing is found, the terrain's
// own (its mirrored sky) stays.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D scene_color_texture;   // the copy of the scene
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D scene_depth_texture;   // and its depth
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D ground_height_texture; // R32F: the ground (m)
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D ground_water_texture;  // RG32F: the water's depth (m)
layout(set = TEXTURE_SET, binding = 4) uniform sampler2D sky_occlusion_texture; // R8 a cell: the sky it sees
layout(set = TEXTURE_SET, binding = 5) uniform sampler2D normal_texture;        // the terrain's normals
layout(set = TEXTURE_SET, binding = 6) uniform sampler2D surface_texture;       // the terrain's surface: silt, wetness, bedrock, water on it
layout(set = TEXTURE_SET, binding = 7) uniform sampler2D variation_texture;     // tileable cloudy noise

layout(std140, set = UNIFORM_SET, binding = 1) uniform puddles_block
{
	vec4 depth_to_distance;        // x, y: the copy's depth to the distance along the view, y / (depth - x); z, w: 1 / its size
	vec4 puddle_flags;             // x: 1 if the water's depth is bound
};

#include "water_ssr.glsl"
#include "puddles.glsl"

layout(location = 0) in vec2 screen_point;
layout(location = 0) out vec4 fragment_color;

// Farther than this nothing is looked for (the march's reach, a puddle's
// pixels too few)
const float PUDDLE_REACH = 200.0;

// The ground's height between the cells (R32F is not filtered everywhere):
// the four round it in one gather (as grass_body.glsl)
float ground_height(vec2 world_xz)
{
	int size = int(ground_map.x);
	vec2 grid_position = clamp(world_xz / ground_map.y, vec2(0.0), vec2(float(size - 1)));
	ivec2 low = ivec2(floor(grid_position));
	vec2 fraction = grid_position - vec2(low);
	vec4 corners = textureGather(ground_height_texture, (vec2(low) + 1.0) / float(size), 0);

	return mix(mix(corners.w, corners.z, fraction.x), mix(corners.x, corners.y, fraction.x), fraction.y);
}

void main()
{
	float depth = texelFetch(scene_depth_texture, ivec2(gl_FragCoord.xy), 0).r;

	fragment_color = vec4(0.0);
	if (depth >= 1.0 || ground_map.z < 0.5)
	{
		return;
	}
	vec3 from_eye = clip_to_eye(screen_point, depth * 2.0 - 1.0);
	float distance_eye = length(from_eye);
	vec3 position = eye_position + from_eye;

	if (distance_eye > PUDDLE_REACH || abs(position.y - ground_height(position.xz)) > 0.06 + 0.003 * distance_eye)
	{
		return;
	}
	vec2 uv = (position.xz / ground_map.y + 0.5) / ground_map.x;

	if (puddle_flags.x > 0.5)
	{
		ivec2 cell = clamp(ivec2(floor(position.xz / ground_map.y + 0.5)), ivec2(0), ivec2(int(ground_map.x) - 1));

		if (texelFetch(ground_water_texture, cell, 0).x > 0.05)
		{
			return;
		}
	}
	float puddle = puddle_amount(position.xz, texture(surface_texture, uv), texture(sky_occlusion_texture, uv).r);

	if (puddle < 0.01)
	{
		return;
	}
	vec3 view = from_eye / distance_eye;
	vec3 direction = reflect(view, vec3(0.0, 1.0, 0.0));
	float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + time_seconds * 0.618034);
	vec4 found = screen_reflection(position + vec3(0.0, 0.02, 0.0), direction, jitter);
	float fresnel = 0.02 + 0.98 * pow(1.0 - max(-view.y, 0.0), 5.0);

	fragment_color = vec4(found.rgb, found.a * fresnel * puddle * (1.0 - smoothstep(0.7 * PUDDLE_REACH, PUDDLE_REACH, distance_eye)));
}
