#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// The rainbow (see src/rain.h): one triangle over the whole screen, like
// the sky's; the fragment shader places each pixel's depth where the bow
// hangs.

// (from the eye, never through the world: the far plane is kilometers
// out, and the eye kilometers from the origin; added and taken off again
// the direction lost its precision, the sun's and the moon's discs broke)
layout(location = 0) out vec3 view_ray;

void main()
{
	vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
	// (the near plane's point: on the far plane its w was all but lost)
	view_ray = clip_to_eye(corner, -1.0);
	gl_Position = vec4(corner, 1.0, 1.0);
}
