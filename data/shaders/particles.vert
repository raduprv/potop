#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A particle: one quad facing the camera (or drawn long along its motion).
// The per particle data comes from the simulation's render buffer, one
// entry per instance. See particles.h.

layout(location = 0) in vec4 particle_position_size;
layout(location = 1) in vec4 particle_color;
layout(location = 2) in vec4 particle_extra;
layout(location = 3) in vec4 particle_velocity;

layout(std140, set = UNIFORM_SET, binding = 1) uniform particle_draw_block
{
	vec3 camera_right;
	float light;                   // how lit the lit ones are (sun and sky)
	vec3 camera_up;
	float sky_light;               // of light, what comes from the sky (less where the sky is hidden: sky_occlusion.glsl)
	vec3 fog_sky_color;            // what far away fades into (as drawn: sRGB)
	float pixel_per_distance;      // a pixel's size, meters, a meter from the eye
};

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D sky_occlusion_texture;   // R8 a cell (sky_occlusion.h)
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D sky_layers_texture;      // RGBA8: the four layers in the air (sky_occlusion.glsl)
#define SKY_OCCLUSION_NO_GROUND
#include "sky_occlusion.glsl"

layout(location = 0) out vec2 texture_uv;
layout(location = 1) out vec4 color;
layout(location = 2) flat out float texture_layer;
layout(location = 3) flat out float emissive;
layout(location = 4) out float view_distance;
layout(location = 5) out float above_surface;               // meters this point of it is over the ground or water under its middle
layout(location = 6) flat out float sky_seen_here;          // how much of the sky its middle sees (the glowing ones: all)

void main()
{
	// Triangle strip corners: (-1, -1), (1, -1), (-1, 1), (1, 1)
	vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1)) * 2.0 - 1.0;
	vec3 position = particle_position_size.xyz;
	float half_size = 0.5 * particle_position_size.w;
	vec3 velocity = particle_velocity.xyz;
	float speed = length(velocity);
	vec3 right;
	vec3 up;
	vec3 world;
	// Thinner than a pixel and it falls between the pixels (a bullet's
	// tracer far off): as wide as a pixel, as much dimmer
	float least_half = 0.6 * pixel_per_distance * length(position - eye_position);
	float thin = half_size < least_half ? half_size / least_half : 1.0;
	bool stretched = particle_velocity.w > 0.0 && speed > 1e-3;

	if (particle_position_size.w <= 0.0)
	{
		// A free slot (drawn slot by slot: particles.h): nothing, all its
		// corners one point outside the view
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		texture_uv = vec2(0.0);
		color = vec4(0.0);
		texture_layer = 0.0;
		emissive = 0.0;
		view_distance = 0.0;
		above_surface = 0.0;
		sky_seen_here = 0.0;
		return;
	}

	half_size = max(half_size, least_half);
	color = particle_color;
	color.a *= stretched ? thin : thin * thin;

	if (stretched)
	{
		// Long along the motion, as wide as its size
		vec3 along = velocity / speed;
		vec3 to_eye = normalize(eye_position - position);
		vec3 across = cross(along, to_eye);

		right = (dot(across, across) > 1e-6 ? normalize(across) : camera_right) * half_size;
		up = along * (half_size + 0.5 * speed * particle_velocity.w);
	}
	else
	{
		float c = cos(particle_extra.x);
		float s = sin(particle_extra.x);

		right = (c * camera_right + s * camera_up) * half_size;
		up = (c * camera_up - s * camera_right) * half_size;
	}
	world = position + right * corner.x + up * corner.y;
	gl_Position = world_to_clip(world);
	texture_uv = corner * 0.5 + 0.5;
	texture_layer = particle_extra.y;
	emissive = particle_extra.z;
	view_distance = length(world - eye_position);
	above_surface = world.y - particle_extra.w;
	sky_seen_here = particle_extra.z > 0.5 ? 1.0 : sky_seen_above(position.xz, position.y - particle_extra.w);
}
