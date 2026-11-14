#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A snowflake (snow.vert): a small soft white disc, lit by the sky and the
// sun. Premultiplied.

layout(location = 0) in float flake_alpha;
layout(location = 1) in vec2 corner_out;

layout(location = 0) out vec4 fragment_color;

void main()
{
	float alpha = flake_alpha * (1.0 - smoothstep(0.35, 1.0, length(corner_out)));

	if (alpha <= 0.002)
	{
		discard;
	}
	vec3 light = sky_color * ambient_intensity * 1.6 + sun_color * 0.45;

	fragment_color = vec4(light * alpha, alpha);
}
