#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The frame's last passes (post.h): one triangle over the whole target,
// with the place in it (0..1, from the top left) for looking up textures.

layout(location = 0) out vec2 screen_uv;

void main()
{
	vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;

	screen_uv = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
	gl_Position = vec4(corner, 0.0, 1.0);
}
