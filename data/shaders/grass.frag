#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A blade of grass (grass.vert): lit as the terrain is (the sun through the
// shadow maps and the clouds' shadows, once a blade by the scatter:
// grass_body.glsl; the sky and the ground around, the
// lamps), and besides: the sun through the blade from behind (thin, it lets
// light through), a sheen along it, darker in its own shade low down, wet
// in the rain; into the haze and under the water as everything.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D variation_texture;   // underwater.glsl's
#include "underwater.glsl"
#define LIGHT_BINDING 1
#include "lights.glsl"

// (centroid: with multisampling a blade's pixel partly covered is shaded
// at a point inside it, not its middle off the thin blade, where the
// values ran far past their ends: bright blotches, bloomed)
layout(location = 0) centroid in vec3 world_position;
layout(location = 1) centroid in vec3 world_normal;
layout(location = 2) centroid in vec3 albedo;
layout(location = 3) centroid in float sunlit;            // grass.vert's: what the shadow maps and the clouds leave of the sun (a blade's)
layout(location = 5) centroid in float along;
layout(location = 6) centroid in vec4 sky_light;           // grass.vert's: the light sent back (rgb), the sky seen (a)

layout(location = 0) out vec4 fragment_color;
#include "scene_depth.glsl"

vec3 surroundings(vec3 direction)
{
	vec3 ground_bounce = ground_bounce_color(1.0);

	return mix(ground_bounce, sky_color, direction.y * 0.5 + 0.5) * ambient_intensity * sky_light.a + sky_light.rgb;
}

void main()
{
	scene_depth_write();
	vec3 normal = normalize(world_normal);
	vec3 towards_eye = normalize(eye_position - world_position);

	// Lit from whichever side faces the eye
	if (dot(normal.xz, towards_eye.xz) < 0.0)
	{
		normal = normalize(vec3(-normal.x, normal.y, -normal.z));
	}
	float shadowed = sunlit;
	// Its own shade among the others, low down
	float occlusion = 0.75 + 0.25 * along;
	float diffuse = max(dot(normal, sun_direction), 0.0) * 0.85 + 0.15 * max(sun_direction.y, 0.0);
	// The sun through it, seen from the other side, more at the tip
	float through = pow(max(dot(-towards_eye, sun_direction), 0.0), 4.0) * (0.3 + 0.7 * along) * 0.6;
	// Under the winter's snow (weather.w, the snow on things) what is left
	// of it is faded, straw and frost, not the summer's green
	vec3 color = albedo;

	if (weather.w > 0.0)
	{
		float gray = dot(albedo, vec3(0.3, 0.59, 0.11));

		color = mix(albedo, vec3(gray * 1.25, gray * 1.12, gray * 0.8), 0.65 * weather.w);
		color = mix(color, vec3(0.62, 0.66, 0.7), 0.3 * weather.w * (0.4 + 0.6 * along));
	}
	vec3 lit = color * (sun_color * (diffuse * occlusion + through) * shadowed
		+ (surroundings(normal) + local_light(world_position, normal, 0.4)) * occlusion);

	// A sheen along the blade, and wet in the rain
	{
		float shine = pow(max(dot(reflect(-sun_direction, normal), towards_eye), 0.0), 24.0);

		lit += sun_color * shadowed * shine * (0.04 + 0.25 * weather.y) * along;
	}
	lit = mix(lit, sky_color, clamp(haze_amount(world_position), 0.0, 1.0));
	lit = apply_underwater(lit, world_position, length(world_position - eye_position), sun_color * diffuse * shadowed);
	fragment_color = vec4(lit, 1.0);
}
