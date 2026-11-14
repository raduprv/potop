#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The user interface: Nuklear's triangles, in interface units with the
// origin at the top left of the window.

layout(std140, set = UNIFORM_SET, binding = 0) uniform ui_block
{
	vec2 screen_scale;               // 2 / size of the window in interface units
	vec2 ui_padding;
};

layout(location = 0) in vec2 ui_position;
layout(location = 1) in vec2 ui_uv;
layout(location = 2) in vec4 ui_color;

layout(location = 0) out vec2 texture_uv;
layout(location = 1) out vec4 vertex_color;

void main()
{
	texture_uv = ui_uv;
	vertex_color = ui_color;
	gl_Position = vec4(ui_position.x * screen_scale.x - 1.0, 1.0 - ui_position.y * screen_scale.y, 0.0, 1.0);
}
