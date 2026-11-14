#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A raindrop's streak (rain.vert): the sky's light it catches, brightest
// along its middle, fading at both ends. Premultiplied.

layout(location = 0) in float drop_alpha;
layout(location = 1) in float across;
layout(location = 2) in float along;

layout(location = 0) out vec4 fragment_color;

void main()
{
	float alpha = drop_alpha * (1.0 - across * across) * smoothstep(0.0, 0.25, along) * smoothstep(1.0, 0.6, along) * 0.55;

	if (alpha <= 0.002)
	{
		discard;
	}
	vec3 light = sky_color * ambient_intensity * 1.8 + sun_color * 0.12;

	fragment_color = vec4(light * alpha, alpha * 0.5);
}
