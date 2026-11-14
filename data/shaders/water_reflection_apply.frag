#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// The water's reflections, marched at half the size (water_reflection.comp),
// laid over the water drawn with the sky in their place: added to the
// scene, each pixel by its weight (how much of it the reflection is), what
// was found taking the sky's place. Each pixel takes the four nearest
// marched texels, by how near they are and how near their distance along
// the view is to its own (a far lake's texel is not spread over the near
// shore's water).

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D ray_texture;       // full size: the ray's direction, its weight
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D sky_texture;       // full size: the sky it mirrors, its distance along the view
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D seen_texture;      // half size: what was found, how sure
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D distance_texture;  // half size: the marched pixel's distance (0: none)

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float weight = texelFetch(ray_texture, pixel, 0).w;

	if (weight <= 0.0)
	{
		discard;
	}
	vec4 sky = texelFetch(sky_texture, pixel, 0);
	ivec2 half_size = textureSize(seen_texture, 0);
	vec2 at = (vec2(pixel) + 0.5) * 0.5 - 0.5;
	ivec2 base = ivec2(floor(at));
	vec2 fraction = at - vec2(base);
	float tolerance = 0.03 * sky.w + 0.05;
	float total = 0.0;
	float trust = 0.0;
	vec3 found = vec3(0.0);

	for (int k = 0; k < 4; k++)
	{
		ivec2 offset = ivec2(k & 1, k >> 1);
		ivec2 texel = clamp(base + offset, ivec2(0), half_size - 1);
		float distance_along = texelFetch(distance_texture, texel, 0).r;

		if (distance_along <= 0.0)
		{
			continue;
		}
		vec4 seen = texelFetch(seen_texture, texel, 0);
		float near = (offset.x == 1 ? fraction.x : 1.0 - fraction.x) * (offset.y == 1 ? fraction.y : 1.0 - fraction.y) + 1e-3;
		float alike = exp(-abs(distance_along - sky.w) / tolerance);
		float share = near * alike;

		total += share;
		trust += share * seen.a;
		found += share * seen.a * seen.rgb;
	}
	if (trust <= 1e-5)
	{
		discard;
	}
	found /= trust;
	trust /= max(total, 1e-5);
	fragment_color = vec4(weight * trust * (found - sky.rgb), 0.0);
}
