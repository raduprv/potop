#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A particle's pixels, premultiplied: glowing ones add to what is behind
// (alpha 0), lit ones cover it. One blend mode for both, no sorting.

layout(std140, set = UNIFORM_SET, binding = 1) uniform particle_draw_block
{
	vec3 camera_right;
	float light;                   // how lit the lit ones are (sun and sky)
	vec3 camera_up;
	float sky_light;               // of light, what comes from the sky (less where the sky is hidden: sky_occlusion.glsl)
	vec3 fog_sky_color;            // what far away fades into (sRGB, as set)
	float pixel_per_distance;
	vec4 depth_to_distance;        // x, y: the scene's depth to the distance along the view, y / (depth - x); zw: 1 / its size
	float particle_seconds;       // the particles' clock (paused with them)
	float has_depth;               // 1: the scene's depth is there (the soft ones fade)
	vec2 draw_padding;
};

layout(set = TEXTURE_SET, binding = 0) uniform sampler2DArray particle_textures;
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D scene_depth_texture;   // the scene so far (particles_capture_depth)

layout(location = 0) in vec2 texture_uv;
layout(location = 1) in vec4 color;
layout(location = 2) flat in float texture_layer;
layout(location = 3) flat in float emissive;
layout(location = 4) in float view_distance;
layout(location = 5) in float above_surface;
layout(location = 6) flat in float sky_seen_here;
layout(location = 7) flat in vec4 look;                     // distortion, random number, soft (meters), age (0..1)

layout(location = 0) out vec4 fragment_color;

float particle_hash(vec2 cell)
{
	return fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);
}

float particle_noise(vec2 at)
{
	vec2 cell = floor(at);
	vec2 inside = at - cell;
	vec2 blend = inside * inside * (3.0 - 2.0 * inside);

	return mix(mix(particle_hash(cell), particle_hash(cell + vec2(1.0, 0.0)), blend.x),
		mix(particle_hash(cell + vec2(0.0, 1.0)), particle_hash(cell + vec2(1.0, 1.0)), blend.x), blend.y);
}

void main()
{
	vec2 uv = texture_uv;

	// A flame's licking: its texture bent by a noise rising through it,
	// more at its top than at its foot (each particle its own)
	if (look.x > 0.0)
	{
		vec2 at = uv * 2.5 + vec2(look.y * 37.0, -particle_seconds * 2.2 + look.y * 11.0);
		vec2 bend = vec2(particle_noise(at), particle_noise(at + vec2(17.3, 5.1))) - 0.5;
		bend += 0.5 * (vec2(particle_noise(at * 2.1 + 3.7), particle_noise(at * 2.1 + 9.2)) - 0.5);

		uv += bend * look.x * mix(0.35, 1.0, uv.y);
	}

	float mask = texture(particle_textures, vec3(uv, texture_layer)).r;
	// Fading out where it goes into the ground or the water, instead of
	// a hard line
	float alpha = mask * color.a * clamp(above_surface / 0.25, 0.0, 1.0);

	// And where it goes into anything else (a log in the fire): fading as
	// it nears what is behind it
	if (look.z > 0.0 && has_depth > 0.5)
	{
		float scene_depth = texture(scene_depth_texture, gl_FragCoord.xy * depth_to_distance.zw).r;
		float scene_distance = depth_to_distance.y / (scene_depth - depth_to_distance.x);
		float own_distance = depth_to_distance.y / (gl_FragCoord.z - depth_to_distance.x);

		alpha *= clamp((scene_distance - own_distance) / look.z, 0.0, 1.0);
	}
	float fog = 1.0 - exp(-(view_distance * fog_density) * (view_distance * fog_density));

	if (alpha < 0.003)
	{
		discard;
	}
	// The colors are set in sRGB; the scene is linear light
	vec3 linear_color = pow(color.rgb, vec3(2.2));

	if (emissive > 0.0)
	{
		// (emissive: its intensity, past white where it is set so)
		fragment_color = vec4(linear_color * emissive * alpha * (1.0 - fog), 0.0);
	}
	else
	{
		// (in a wood, a crater: less of the sky's light)
		float lit = max(light - sky_light * (1.0 - sky_seen_here), 0.1);

		fragment_color = vec4(mix(linear_color * lit, pow(fog_sky_color, vec3(2.2)), clamp(fog, 0.0, 1.0)) * alpha, alpha);
	}
}
