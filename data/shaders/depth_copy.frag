#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The multisampled scene's depth values, resolved, written into the plain
// depth for what is drawn after (render_target.h)

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D depth_values;

layout(location = 0) in vec2 screen_uv;

void main()
{
	gl_FragDepth = texelFetch(depth_values, ivec2(gl_FragCoord.xy), 0).r;
}
