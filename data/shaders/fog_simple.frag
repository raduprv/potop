#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// The simple valley fog (see src/fog.h; where the volumetric one costs too
// much): along each pixel's ray, a few steps through the fog to what it
// sees, the sun's and the sky's light without shadows. Premultiplied.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D valley_texture;
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D depth_texture;
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D rays_texture;
layout(set = TEXTURE_SET, binding = 3) uniform sampler2D unused_texture;
layout(set = TEXTURE_SET, binding = 4) uniform sampler2DShadow far_shadow_texture;
#include "fog.glsl"

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;

void main()
{
	float depth = texelFetch(depth_texture, ivec2(gl_FragCoord.xy), 0).r;
	vec3 direction = eye_direction(screen_point);
	// (as far as the map goes, the sky too)
	float distance_meters = fog_map_exit(direction);
	int steps = int(fog_grid.w);

	if (underwater_color.a > 0.5)
	{
		discard;
	}
	if (depth < 1.0)
	{
		distance_meters = min(length(clip_to_eye(screen_point, depth * 2.0 - 1.0)), distance_meters);
	}
	float enter;
	float leave;

	if (!fog_span(direction, distance_meters, enter, leave))
	{
		discard;
	}
	// Steps over the part of the ray low enough for fog only; dithered by
	// the pixel alone (still from frame to frame)
	float step_meters = (leave - enter) / float(steps);
	float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	vec3 light = fog_light(-direction, 1.0, 1.0);
	vec3 gathered = vec3(0.0);
	float through = 1.0;

	for (int i = 0; i < steps && through > 0.01; i++)
	{
		vec3 point = eye_position + direction * (enter + (float(i) + jitter) * step_meters);
		float density = valley_fog_density(point, valley_texture);
		float step_through = exp(-density * step_meters);

		gathered += through * light * (1.0 - step_through);
		through *= step_through;
	}
	fragment_color = vec4(gathered + fog_rays_light(rays_texture, gl_FragCoord.xy, vec2(textureSize(depth_texture, 0)), 1.0 - through), 1.0 - through);
}
