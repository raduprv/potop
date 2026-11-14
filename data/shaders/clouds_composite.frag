#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// Volumetric clouds onto the scene (see clouds.h): each pixel looks its
// direction up in the dome, unless the land is nearer than the clouds
// along it (the layer starts above every mountain, so the land is either
// all in front of a ray's clouds or all behind them). Premultiplied: the
// scene shows through by the transparency.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D dome_texture;
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D depth_texture;

#include "clouds_volume.glsl"

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;

// Light over this is squeezed towards 1, so bright clouds by the sun keep
// some shape instead of burning white
#define SHOULDER 0.8

void main()
{
	vec3 direction = eye_direction(screen_point);
	float depth = texelFetch(depth_texture, ivec2(gl_FragCoord.xy), 0).r;
	float enter;
	float leave;
	float flock_enter;
	float flock_leave;
	bool heaps = layer_interval(eye_position, direction, 1e9, enter, leave);
	bool flocks = flock_interval(eye_position, direction, 1e9, flock_enter, flock_leave);

	fragment_color = vec4(0.0);
	if (underwater_color.a > 0.5 || !(heaps || flocks))
	{
		return;
	}
	// The nearest of the two layers
	enter = heaps ? (flocks ? min(enter, flock_enter) : enter) : flock_enter;
	if (depth < 1.0)
	{
		if (length(clip_to_eye(screen_point, depth * 2.0 - 1.0)) < enter)
		{
			return;
		}
	}
	{
		// A little smoothing over about a texel: the grain the jittered
		// marches leave behind (the four bilinear taps around the middle
		// one make a tent)
		vec2 coordinates = dome_coordinates(direction);
		vec2 texel = 0.75 / vec2(textureSize(dome_texture, 0));
		vec4 clouds = textureLod(dome_texture, coordinates, 0.0) * 0.4
			+ (textureLod(dome_texture, coordinates + vec2(texel.x, 0.0), 0.0) + textureLod(dome_texture, coordinates - vec2(texel.x, 0.0), 0.0)
			+ textureLod(dome_texture, coordinates + vec2(0.0, texel.y), 0.0) + textureLod(dome_texture, coordinates - vec2(0.0, texel.y), 0.0))
			* 0.15;
		float alpha = clamp(clouds.a, 0.0, 1.0);
		vec3 color = alpha > 0.001 ? clouds.rgb / alpha : vec3(0.0);
		float brightest = max(max(color.r, color.g), color.b);

		if (brightest > SHOULDER)
		{
			float squeezed = SHOULDER + (1.0 - SHOULDER) * (1.0 - exp(-(brightest - SHOULDER) / (1.0 - SHOULDER)));

			color *= squeezed / brightest;
		}
		// Linear light, premultiplied
		fragment_color = vec4(color * alpha, alpha);
	}
}
