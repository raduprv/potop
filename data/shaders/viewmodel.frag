#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The weapon in hand (viewmodel.vert): the sun, the sky and the ground
// around (in the camera's space), its muzzle flash; the crosshair unlit.

layout(location = 0) in vec3 view_normal;
layout(location = 1) in vec3 albedo;
layout(location = 2) in vec3 view_position;

layout(std140, set = UNIFORM_SET, binding = 0) uniform viewmodel_fragment_block
{
	vec4 sun_direction;            // towards the sun, in the camera's space; w: 1 unlit (the crosshair)
	vec4 sun_light;                // its color times its strength (and how much of it reaches the eye's place)
	vec4 sky_light;                // the sky's light
	vec4 ground_light;             // the ground's, from below
	vec4 up;                       // up, in the camera's space
	vec4 flash;                    // the muzzle flash's light (rgb)
	vec4 flash_position;           // in the camera's space
	vec4 carried;                  // a light the player carries (rgb; 0: none)
	vec4 carried_position;         // in the camera's space
};

layout(location = 0) out vec4 fragment_color;

void main()
{
	if (sun_direction.w > 0.5)
	{
		fragment_color = vec4(albedo * sun_light.rgb, 1.0);
		return;
	}
	vec3 normal = normalize(view_normal);
	float facing_up = dot(normal, up.xyz);
	vec3 ambient = mix(ground_light.rgb, sky_light.rgb, facing_up * 0.5 + 0.5);
	float diffuse = max(dot(normal, sun_direction.xyz), 0.0);
	vec3 towards_flash = flash_position.xyz - view_position;
	float flash_distance = length(towards_flash);
	float flash_light = max(dot(normal, towards_flash / max(flash_distance, 1e-3)), 0.0) / (1.0 + flash_distance * flash_distance * 8.0);
	vec3 towards_carried = carried_position.xyz - view_position;
	float carried_distance = length(towards_carried);
	float carried_light = max(dot(normal, towards_carried / max(carried_distance, 1e-3)), 0.0) * 0.6 + 0.15;

	carried_light /= 1.0 + carried_distance * carried_distance * 2.0;
	// (a little sheen on the metal: the sun's highlight)
	vec3 view = normalize(-view_position);
	float highlight = pow(max(dot(reflect(-sun_direction.xyz, normal), view), 0.0), 24.0) * 0.25;

	fragment_color = vec4(albedo * (sun_light.rgb * diffuse + ambient + flash.rgb * flash_light + carried.rgb * carried_light)
		+ sun_light.rgb * highlight * diffuse, 1.0);
}
