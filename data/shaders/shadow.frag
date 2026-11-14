#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Shadow map pass: only depth is written, so there is nothing to do here.
// (OpenGL ES still requires a fragment shader.)

void main()
{
}
