#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// Sky fragment shader: the dome's color in the direction of the pixel.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D variation_texture;
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D dome_texture;  // the volumetric clouds (fog_parameters.w)
// The moon's map, longitude by latitude (the near side in the middle):
// rgb its color (sRGB), a its height
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D moon_texture;
#include "clouds.glsl"
// The flat clouds, unless the volumetric ones are drawn over the sky
#define SKY_FLAT_CLOUDS (1.0 - volumetric_clouds)
#include "sky.glsl"
#include "dome_mapping.glsl"

layout(location = 0) in vec3 view_ray;      // the near plane's point from the eye (its direction)

layout(location = 0) out vec4 fragment_color;
#include "scene_depth.glsl"

// The stars (fog_parameters.y: how much they show, z: the hour): points
// scattered over the sphere in cells, one or two cells in a hundred holding
// one, most faint, some bright, a little of each one's own color. The
// sphere turns about the pole once a day. Each star is a couple of pixels
// wide whatever the resolution (from the derivatives), so they do not
// flicker as the sky turns; they twinkle a little, and fade into the haze
// near the horizon.
#define STAR_CELLS 160.0
#define STAR_SHARE 0.01

uint star_hash(uvec3 cell)
{
	uint value = cell.x * 1597334673u ^ cell.y * 3812015801u ^ cell.z * 2798796415u;

	value ^= value >> 15;
	value *= 2246822519u;
	value ^= value >> 13;
	return value;
}

vec3 stars(vec3 direction)
{
	float visibility = fog_parameters.y;

	if (visibility <= 0.001 || direction.y <= 0.0)
	{
		return vec3(0.0);
	}
	// Turned about the pole (tilted towards -Z) by the hour
	vec3 pole = normalize(vec3(0.0, 0.75, -0.66));
	float angle = fog_parameters.z / 24.0 * 6.2831853;
	vec3 turned = direction * cos(angle) + cross(pole, direction) * sin(angle) + pole * dot(pole, direction) * (1.0 - cos(angle));
	vec3 point = turned * STAR_CELLS;
	vec3 cell = floor(point);
	float pixel = max(length(fwidth(point)), 1e-4);
	vec3 light = vec3(0.0);

	// The cell and its neighbors (a star near an edge shows in both)
	for (int z = -1; z <= 1; z++)
	{
		for (int y = -1; y <= 1; y++)
		{
			for (int x = -1; x <= 1; x++)
			{
				vec3 neighbor = cell + vec3(x, y, z);
				uint hash = star_hash(uvec3(ivec3(neighbor) + 4096));

				if (float(hash & 1023u) / 1024.0 >= STAR_SHARE)
				{
					continue;
				}
				vec3 jitter = vec3(float((hash >> 10) & 255u), float((hash >> 18) & 255u), float((hash >> 24) & 255u)) / 255.0;
				vec3 center = neighbor + 0.2 + 0.6 * jitter;
				// Only its distance across the line of sight
				vec3 offset = center - point;
				vec3 across = offset - turned * dot(offset, turned);
				float spread = pixel * 0.45;
				float shape = exp(-dot(across, across) / (spread * spread));
				float brightness = pow(jitter.x, 8.0) * 4.0 + 0.06;
				float twinkle = 0.8 + 0.2 * sin(time_seconds * (2.0 + 3.0 * jitter.y) + jitter.z * 40.0);
				vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.85, 0.7), jitter.z);

				light += tint * shape * brightness * twinkle;
			}
		}
	}
	// Behind the flat clouds, and faint near the horizon
	float cloud = sky_cloud(direction, eye_position, 0.0) * SKY_FLAT_CLOUDS;

	// Behind the volumetric ones: they are drawn over the sky by how
	// opaque they are, but a bright star would still shine through a thin
	// one; gone well before the cloud is opaque
	if (fog_parameters.w > 0.5)
	{
		vec2 place = fog_parameters.w > 1.5 ? half_coordinates(direction) : sphere_coordinates(direction);

		cloud = max(cloud, smoothstep(0.0, 0.4, textureLod(dome_texture, place, 0.0).a));
	}

	return light * visibility * smoothstep(0.0, 0.25, direction.y) * (1.0 - cloud);
}

// How much the clouds cover a direction (the flat ones, or the volumetric
// ones' dome)
float cloud_cover_towards(vec3 direction)
{
	float cloud = sky_cloud(direction, eye_position, 0.0) * SKY_FLAT_CLOUDS;

	if (fog_parameters.w > 0.5)
	{
		vec2 place = fog_parameters.w > 1.5 ? half_coordinates(direction) : sphere_coordinates(direction);

		cloud = max(cloud, smoothstep(0.0, 0.6, textureLod(dome_texture, place, 0.0).a));
	}
	return cloud;
}

#define MOON_MAP moon_texture
#define moon_clouds cloud_cover_towards
#include "moon.glsl"

void main()
{
	// (the far plane, as nothing drawn: it writes no depth)
	fragment_depth = 1.0;
	vec3 direction = normalize(view_ray);
	float pixel_angle = length(fwidth(direction));
	// No sun's disc where the light is the moon's (sun_direction is the moon then)
	vec4 moon_seen = moon_disc(direction, pixel_angle);
	vec3 color = sky_radiance(direction, eye_position, 1.0 - moon.w, 0.0) + stars(direction) * (1.0 - moon_seen.a) + moon_seen.rgb;

	// Linear light: the frame's last pass (post.frag) makes it a picture
	fragment_color = vec4(color, 1.0);
}
