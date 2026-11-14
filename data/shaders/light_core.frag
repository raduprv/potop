#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// A light's core: brightest in the middle, gone at the quad's edge; added
// to what is behind (premultiplied, alpha 0)

layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 color;

layout(location = 0) out vec4 fragment_color;

void main()
{
	float reach = dot(corner, corner);

	if (reach >= 1.0)
	{
		discard;
	}
	// A bright point and a soft skirt
	float glow = exp(-reach * 9.0) + 0.25 * (1.0 - reach) * (1.0 - reach);
	fragment_color = vec4(color * glow, 0.0);
}
