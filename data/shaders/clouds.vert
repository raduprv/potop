#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// Volumetric clouds: one triangle over the whole screen (as the sky), each
// corner with its point on the screen (clip space x, y) for the fragment
// shader to turn into a ray and, with the scene's depth, a distance.

layout(location = 0) out vec2 screen_point;

void main()
{
	vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;

	screen_point = corner;
	gl_Position = vec4(corner, 0.0, 1.0);
}
