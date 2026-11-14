#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A light's core (see lights.h): a small glow where the light is (the
// flame of a candle, the glass of a lantern), one quad facing the camera,
// added to what is behind it; the bloom makes its halo.

layout(location = 0) in vec4 core_position_size;   // where, and the core's size (meters)
layout(location = 1) in vec4 core_color;           // linear light, how bright

layout(std140, set = UNIFORM_SET, binding = 1) uniform core_block
{
	vec3 camera_right;
	float core_padding_0;
	vec3 camera_up;
	float core_padding_1;
};

layout(location = 0) out vec2 corner_out;
layout(location = 1) out vec3 color_out;

void main()
{
	// Triangle strip corners: (-1, -1), (1, -1), (-1, 1), (1, 1)
	vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1)) * 2.0 - 1.0;
	vec3 position = core_position_size.xyz;
	float size = core_position_size.w;
	// A little towards the eye, so the glass or the flame around it does
	// not hide it (what stands farther in front still does)
	vec3 to_eye = eye_position - position;
	float eye_distance = length(to_eye);

	position += to_eye / max(eye_distance, 1e-3) * min(size * 3.0, 0.5 * eye_distance);
	position += (corner.x * camera_right + corner.y * camera_up) * size;
	corner_out = corner;
	color_out = core_color.rgb;
	gl_Position = world_to_clip(position);
}
