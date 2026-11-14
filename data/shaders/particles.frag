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
};

layout(set = TEXTURE_SET, binding = 0) uniform sampler2DArray particle_textures;

layout(location = 0) in vec2 texture_uv;
layout(location = 1) in vec4 color;
layout(location = 2) flat in float texture_layer;
layout(location = 3) flat in float emissive;
layout(location = 4) in float view_distance;
layout(location = 5) in float above_surface;
layout(location = 6) flat in float sky_seen_here;

layout(location = 0) out vec4 fragment_color;

void main()
{
	float mask = texture(particle_textures, vec3(texture_uv, texture_layer)).r;
	// Fading out where it goes into the ground or the water, instead of
	// a hard line
	float alpha = mask * color.a * clamp(above_surface / 0.25, 0.0, 1.0);
	float fog = 1.0 - exp(-(view_distance * fog_density) * (view_distance * fog_density));

	if (alpha < 0.003)
	{
		discard;
	}
	// The colors are set in sRGB; the scene is linear light
	vec3 linear_color = pow(color.rgb, vec3(2.2));

	if (emissive > 0.5)
	{
		fragment_color = vec4(linear_color * alpha * (1.0 - fog), 0.0);
	}
	else
	{
		// (in a wood, a crater: less of the sky's light)
		float lit = max(light - sky_light * (1.0 - sky_seen_here), 0.1);

		fragment_color = vec4(mix(linear_color * lit, pow(fog_sky_color, vec3(2.2)), clamp(fog, 0.0, 1.0)) * alpha, alpha);
	}
}
