#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// Sky: one triangle over the whole screen, at the far plane, so the depth
// test lets it through only where nothing else was drawn. For each corner
// the point on the near plane it looks through; the fragment shader turns that
// into a direction.

// (from the eye, never through the world: the far plane is kilometers
// out, and the eye kilometers from the origin; added and taken off again
// the direction lost its precision, the sun's and the moon's discs broke)
layout(location = 0) out vec3 view_ray;

void main()
{
	// Corners (-1, -1), (3, -1), (-1, 3): the triangle covers the screen
	vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
	// (the near plane's point: on the far plane its w was all but lost)
	view_ray = clip_to_eye(corner, -1.0);
	// Depth 1: the far plane in both clip conventions
	gl_Position = vec4(corner, 1.0, 1.0);
}
