#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A blade of grass (see grass.h, grass.comp): no mesh, made here from its
// instance (where it grows, how tall and wide, which way it faces, where
// the water laid it). Near, seven vertices, a strip: three segments up a
// curve (a quadratic Bezier, bowed the way it leans) and its tip; far off
// (the scatter's second list, already widened: coarse.z), three, one
// triangle from its foot to its tip.
//
// It leans a little its own way, bows with the wind (the same gusts that
// roll through the trees, object.vert's, and its own quick sway), and lies
// down where the water flows over it. Its color is the ground's grass
// under it (so it melts into the terrain), darker at its foot, lighter and
// a little yellow at its tip, some blades dry; muddy under silt. What is
// the same all up the blade (its color before the tip's, the silt, the sky
// seen and the light the land round sends back, the sun the shadow maps
// and the clouds leave it) comes worked out by the scatter, once a blade
// (grass_body.glsl), not here seven times.

// The blade's record (grass_body.glsl, packed: see there)
layout(location = 0) in uvec4 blade_first;
layout(location = 1) in uvec4 blade_second;

layout(std140, set = UNIFORM_SET, binding = 1) uniform grass_draw_block
{
	vec4 wind;                     // xy: where it blows towards (x, z); z: how hard (0..1); w: time (seconds)
	vec4 gusts;                    // x: 2 pi over the gusts' spacing (per meter); y: how fast they roll (m/s); z: how far a blade bows in a gale (radians); w: how pale a gust's wave makes it
	vec4 coarse;                   // z: 1 for the far list (one triangle a blade: the scatter's, past grass_coarse_meters)
};

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 albedo;
layout(location = 3) out float sunlit;            // what the shadow maps and the clouds leave of the sun on it (the scatter's)
layout(location = 4) out float haze;               // how much the haze hides it (scene.glsl's haze_amount, per vertex: it changes slowly over a blade, a card, a picture)
layout(location = 5) out float along;             // 0 at its foot .. 1 at its tip
layout(location = 6) out vec4 sky_light;          // the light the land round sends back (rgb), the sky seen (a)

// The blade's own (the scatter's): how it lies, the way its face looks
vec2 blade_flatten;

float hash_1d(vec2 position)
{
	return fract(sin(dot(position, vec2(12.9898, 78.233))) * 43758.5453);
}

// Where a blade's vertex is at the wind's clock (wind.w): its tangent and
// its side, and how much a gust bows it
vec3 blade_point(float clock, vec3 root, float height, float width, float t, float side, float own, float own_2, vec2 facing,
	out vec3 tangent, out vec3 across, out float gust_sheen)
{
	// How it leans: a little its own way (across its face), with the wind,
	// and down where the water laid it
	vec2 lean = vec2(-facing.y, facing.x) * (own - 0.5) * 0.7;
	// How much a gust bows it now (0..1): the gust rolling over the meadow
	// shows, a paler wave (bowed, the blades' sides catch the light)
	gust_sheen = 0.0;

	if (wind.z > 0.0)
	{
		float travel = dot(root.xz, wind.xy) * gusts.x - clock * gusts.x * gusts.y;
		float gust = clamp(0.5 + 0.35 * sin(travel) + 0.15 * sin(travel * 2.7 + 1.3), 0.0, 1.0);

		gust_sheen = smoothstep(0.45, 0.9, gust) * clamp(wind.z * 3.0, 0.0, 1.0);
		float sway = 0.25 * sin(clock * (2.3 + 1.4 * own) + own_2 * 6.2831853);

		lean += wind.xy * wind.z * gusts.z * (0.35 + 0.65 * gust + sway);
	}
	{
		vec2 flatten = blade_flatten;
		float laid = length(flatten);

		if (laid > 0.001)
		{
			lean = mix(lean, flatten / laid * 1.5, clamp(laid, 0.0, 1.0));
		}
	}
	float angle = min(length(lean), 1.5);
	vec2 towards = angle > 1e-4 ? normalize(lean) : facing;
	// The tip, a little nearer the more it bows (it bends, it does not
	// stretch); the middle point keeps it rising before it bends over
	vec3 tip = root + vec3(towards.x * sin(angle), cos(angle), towards.y * sin(angle)) * height * (1.0 - 0.18 * angle / 1.5);
	vec3 middle = root + vec3(0.0, height * 0.55 * (1.0 - 0.35 * angle / 1.5), 0.0) + vec3(towards.x, 0.0, towards.y) * height * 0.12 * angle;
	vec3 point = (1.0 - t) * (1.0 - t) * root + 2.0 * (1.0 - t) * t * middle + t * t * tip;
	tangent = normalize(2.0 * (1.0 - t) * (middle - root) + 2.0 * t * (tip - middle) + vec3(0.0, 1e-4, 0.0));
	across = vec3(facing.x, 0.0, facing.y);

	across = normalize(across - tangent * dot(across, tangent) + vec3(1e-4, 0.0, 0.0));
	// Narrowing to the tip (the last vertex is the tip itself)
	float half_width = width * (1.0 - pow(t, 1.4));

	point += across * side * half_width * 2.0;
	return point;
}

void main()
{
	uint vertex = uint(gl_VertexIndex);
	// (the far list: its foot's two vertices, then its tip)
	uint segment = coarse.z > 0.5 ? (vertex < 2u ? 0u : 3u) : vertex / 2u;
	float side = (vertex & 1u) == 0u ? -0.5 : 0.5;
	vec3 root = uintBitsToFloat(blade_first.xyz);
	vec2 height_width = unpackHalf2x16(blade_first.w);
	vec4 bytes = unpackUnorm4x8(blade_second.y);
	vec4 shared_light = unpackUnorm4x8(blade_second.w);
	float height = height_width.x;
	float width = height_width.y;
	float own = hash_1d(root.xz);

	blade_flatten = unpackHalf2x16(blade_second.x);
	float t = float(segment) / 3.0;
	float own_2 = hash_1d(root.xz + 17.31);
	float facing_angle = bytes.x * 6.2831853;
	vec2 facing = vec2(cos(facing_angle), sin(facing_angle));

	vec3 tangent;
	vec3 across;
	float gust_sheen;
	vec3 point = blade_point(wind.w, root, height, width, t, side, own, own_2, facing, tangent, across, gust_sheen);

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
		vec3 color = unpackUnorm4x8(blade_second.z).rgb;
		float silt = bytes.w;

		color = color * color * 2.0;
		color = mix(color, color * vec3(1.25, 1.2, 0.85), t * 0.6);
		color = mix(color, vec3(0.16, 0.12, 0.08), clamp(silt * 1.2, 0.0, 0.85));
		albedo = color * (0.8 + 0.2 * t);
		// (the wave: paler and a little grey green, more up the blade)
		albedo = mix(albedo, albedo * vec3(1.55, 1.6, 1.35) + vec3(0.02, 0.026, 0.016), clamp(gusts.w * gust_sheen * (0.5 + 0.5 * t), 0.0, 1.5));
	}
	along = t;
	world_position = point;
	sunlit = bytes.y;
	sky_light = vec4(shared_light.rgb * exp2(shared_light.a * 255.0 - 128.0), bytes.z);
	haze = clamp(haze_amount(point), 0.0, 1.0);
	gl_Position = world_to_clip(point);
}
