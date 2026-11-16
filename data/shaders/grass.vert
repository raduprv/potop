#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A blade of grass (see grass.h, grass.comp): no mesh, made here from its
// instance (where it grows, where its tip is now, how wide, which way it
// faces). Near, seven vertices, a strip: three segments up a curve (a
// quadratic Bezier, bowed the way it leans) and its tip; far off (the
// scatter's second list: coarse.z), three, one triangle from its foot to
// its tip.
//
// How it leans now (its own way, the wind's gusts and sway, the water
// laying it down) is the scatter's, once a blade: its tip. Its color is the
// ground's grass under it, darker at its foot, lighter and a little yellow
// at its tip; muddy under silt. What is the same all up the blade (its
// color before the tip's, the silt, the sky seen and the light the land
// round sends back, the sun the shadow maps and the clouds leave it, the
// gust's sheen) comes worked out by the scatter (grass_body.glsl), not
// here seven times.

// The blade's record (grass_body.glsl, packed: see there)
layout(location = 0) in uvec4 blade_first;
layout(location = 1) in uvec4 blade_second;

layout(std140, set = UNIFORM_SET, binding = 1) uniform grass_draw_block
{
	vec4 wind;                     // (the scatter's now: unused)
	vec4 gusts;                    // w: how pale a gust's wave makes it
	vec4 coarse;                   // z: 1 for the far list (one triangle a blade)
};

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 albedo;
layout(location = 3) out float sunlit;            // what the shadow maps and the clouds leave of the sun on it (the scatter's)
layout(location = 4) out float haze;               // how much the haze hides it (scene.glsl's haze_amount, per vertex: it changes slowly over a blade, a card, a picture)
layout(location = 5) out float along;             // 0 at its foot .. 1 at its tip
layout(location = 6) out vec4 sky_light;          // the light the land round sends back (rgb), the sky seen (a)

void main()
{
	uint vertex = uint(gl_VertexIndex);
	// (the far list: its foot's two vertices, then its tip)
	uint segment = coarse.z > 0.5 ? (vertex < 2u ? 0u : 3u) : vertex / 2u;
	float side = (vertex & 1u) == 0u ? -0.5 : 0.5;
	vec3 root = uintBitsToFloat(blade_first.xyz);
	vec3 tip_offset = vec3(unpackHalf2x16(blade_first.w).x, unpackHalf2x16(blade_second.x).x, unpackHalf2x16(blade_first.w).y);
	float width = unpackHalf2x16(blade_second.x).y;
	vec4 bytes = unpackUnorm4x8(blade_second.y);
	vec4 color_sheen = unpackUnorm4x8(blade_second.z);
	vec4 shared_light = unpackUnorm4x8(blade_second.w);
	float t = float(segment) / 3.0;
	float facing_angle = bytes.x * 6.2831853;
	vec2 facing = vec2(cos(facing_angle), sin(facing_angle));
	// The middle of its curve from its tip (the scatter's blade_tip: the
	// tip is (sin a along the lean, cos a) times height (1 - 0.12 a)): it
	// keeps rising before it bends over
	float reach = length(tip_offset.xz);
	float angle = atan(reach, max(tip_offset.y, 1e-6));
	float height = length(tip_offset) / (1.0 - 0.12 * angle);
	vec2 towards = reach > 1e-5 ? tip_offset.xz / reach : facing;
	vec3 tip = root + tip_offset;
	vec3 middle = root + vec3(0.0, height * 0.55 * (1.0 - 0.35 * angle / 1.5), 0.0) + vec3(towards.x, 0.0, towards.y) * height * 0.12 * angle;
	vec3 point = (1.0 - t) * (1.0 - t) * root + 2.0 * (1.0 - t) * t * middle + t * t * tip;
	vec3 tangent = normalize(2.0 * (1.0 - t) * (middle - root) + 2.0 * t * (tip - middle) + vec3(0.0, 1e-4, 0.0));
	vec3 across = vec3(facing.x, 0.0, facing.y);

	across = normalize(across - tangent * dot(across, tangent) + vec3(1e-4, 0.0, 0.0));
	// Narrowing to the tip (the last vertex is the tip itself)
	point += across * side * width * (1.0 - pow(t, 1.4)) * 2.0;
	{
		vec3 face = normalize(cross(across, tangent));

		// Lit mostly as the ground under it is (a meadow is lit from the
		// sky, not blade by blade), rounded a little (its sides turn away),
		// more its own the higher up the blade
		world_normal = normalize(mix(vec3(0.0, 1.0, 0.0), normalize(face + across * side * 0.6), 0.25 + 0.25 * t));
	}
	// Its color (the scatter's: the ground's grass under it, the terrain's
	// variation, wet, dry, its own shade), lighter and yellower at the tip;
	// brown under silt
	{
		vec3 color = color_sheen.rgb;
		float silt = bytes.w;

		color = color * color * 2.0;
		color = mix(color, color * vec3(1.25, 1.2, 0.85), t * 0.6);
		color = mix(color, vec3(0.16, 0.12, 0.08), clamp(silt * 1.2, 0.0, 0.85));
		albedo = color * (0.8 + 0.2 * t);
		// (the wave: paler and a little grey green, more up the blade)
		albedo = mix(albedo, albedo * vec3(1.55, 1.6, 1.35) + vec3(0.02, 0.026, 0.016), clamp(gusts.w * color_sheen.a * (0.5 + 0.5 * t), 0.0, 1.5));
	}
	along = t;
	world_position = point;
	sunlit = bytes.y;
	sky_light = vec4(shared_light.rgb * exp2(shared_light.a * 255.0 - 128.0), bytes.z);
	haze = clamp(haze_amount(point), 0.0, 1.0);
	gl_Position = world_to_clip(point);
}
