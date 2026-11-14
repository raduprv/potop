#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The user interface drawn in the world (the names over the actors): as
// ui.frag, but hidden where the scene is nearer than it (a tree, a wall
// between the eye and the name).

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D ui_texture;
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D scene_depth;

layout(std140, set = UNIFORM_SET, binding = 0) uniform ui_depth_block
{
	float depth;                       // the thing's own depth (as the scene's depth buffer has it)
	float bias;                        // nearer than this before it hides it
	vec2 inverse_size;                 // 1 / the target's size in pixels
};

layout(location = 0) in vec2 texture_uv;
layout(location = 1) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

void main()
{
	float scene = texture(scene_depth, gl_FragCoord.xy * inverse_size).r;

	if (scene < depth - bias)
	{
		discard;
	}
	fragment_color = vertex_color * texture(ui_texture, texture_uv);
}
