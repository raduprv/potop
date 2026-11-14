#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The user interface: vertex color times the texture (the font atlas, an
// icon, or the atlas' white texel for plain shapes).

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D ui_texture;

layout(location = 0) in vec2 texture_uv;
layout(location = 1) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

void main()
{
	fragment_color = vertex_color * texture(ui_texture, texture_uv);
}
