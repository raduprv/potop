#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A blade of grass (see grass.h, grass.comp): no mesh, made here from its
// instance (where it grows, how tall and wide, which way it faces, where
// the water laid it). Seven vertices, a strip: three segments up a curve
// (a quadratic Bezier, bowed the way it leans) and its tip.
//
// It leans a little its own way, bows with the wind (the same gusts that
// roll through the trees, object.vert's, and its own quick sway), and lies
// down where the water flows over it. Its color is the ground's grass
// under it (so it melts into the terrain), darker at its foot, lighter and
// a little yellow at its tip, some blades dry; muddy under silt.

layout(location = 0) in vec4 blade_root;          // x, y, z of its foot; its height
layout(location = 1) in vec4 blade_shape;         // the way its face looks (radians), its width, laid down towards x, z (its length: how far)

layout(set = TEXTURE_SET, binding = 0) uniform sampler2DArray material_color_array;  // the terrain's materials: RGB color
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D surface_texture;            // RGBA8: silt, wetness, bare bedrock
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D variation_texture;          // R8: tileable cloudy noise (the terrain's variation)
#define SKY_VERTEX_BINDING 3
#include "sky_vertex.glsl"

layout(std140, set = UNIFORM_SET, binding = 1) uniform grass_draw_block
{
	vec4 wind;                     // xy: where it blows towards (x, z); z: how hard (0..1); w: time (seconds)
	vec4 gusts;                    // x: 2 pi over the gusts' spacing (per meter); y: how fast they roll (m/s); z: how far a blade bows in a gale (radians); w: how pale a gust's wave makes it
	vec4 look;                     // x: the grass material's first layer; y: its macro repeats per meter; z: cells per side; w: cell size (m)
	vec4 tint;                     // rgb: multiplies the ground's grass color; w: the share of dry blades
	vec4 variation;                // the terrain's: 1 / its tiling (m), its strength, its hue's strength
	vec4 coarse;                   // x: from how far off (m) a blade is one triangle (0: never); y: how much wider then
};

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 albedo;
layout(location = 3) out vec4 shadow_position;
layout(location = 4) out vec4 near_shadow_position;
layout(location = 5) out float along;             // 0 at its foot .. 1 at its tip
layout(location = 6) out vec4 sky_light;          // the light the land round sends back (rgb), the sky seen (a): sky_vertex.glsl

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
		vec2 flatten = blade_shape.zw;
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
	uint segment = vertex / 2u;
	float side = (vertex & 1u) == 0u ? -0.5 : 0.5;
	vec3 root = blade_root.xyz;
	float height = blade_root.w;
	float width = blade_shape.y;
	float own = hash_1d(root.xz);
	// Far off, a blade is one triangle, its foot to its tip, a little wider
	// (a few pixels: the segments' bow is not seen, and four triangles of a
	// pixel each cost the most of anything drawn): the other vertices all at
	// the tip, their triangles empty. Where it starts differs blade by blade
	// (no ring on the meadow)
	if (coarse.x > 0.0 && distance(root, eye_position) > coarse.x * (0.85 + 0.3 * own))
	{
		segment = segment > 0u ? 3u : 0u;
		width *= coarse.y;
	}
	float t = float(segment) / 3.0;
	float own_2 = hash_1d(root.xz + 17.31);
	vec2 facing = vec2(cos(blade_shape.x), sin(blade_shape.x));

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
	// Its color: the ground's grass under it (the far, smooth scale of its
	// texture), darker at the foot, lighter and yellower at the tip; some
	// dry; brown under silt
	{
		vec3 ground = textureLod(material_color_array, vec3(root.xz * look.y, look.x), 3.0).rgb;
		vec3 color = pow(ground, vec3(2.2)) * tint.rgb;
		vec2 surface_uv = (root.xz / look.w + 0.5) / look.z;
		float silt = textureLod(surface_texture, surface_uv, 0.0).r;

		// The terrain's variation over its materials, as terrain.frag has it
		{
			float large = textureLod(variation_texture, root.xz * variation.x, 0.0).r;
			float small = textureLod(variation_texture, root.xz * variation.x * 3.83 + 0.61, 0.0).r;
			float amount = (large - 0.5) * 0.7 + (small - 0.5) * 0.3;

			color *= 1.0 + amount * variation.y * 2.0;
			color *= mix(vec3(1.0), amount > 0.0 ? vec3(1.08, 1.02, 0.86) : vec3(0.90, 1.02, 0.94), min(abs(amount) * 2.0, 1.0) * variation.z);
			// Wet: darker, as the ground
			color *= 1.0 - 0.3 * textureLod(surface_texture, surface_uv, 0.0).g;
		}
		color *= 0.85 + 0.3 * own_2;
		if (own < tint.w)
		{
			color = mix(color, vec3(0.32, 0.26, 0.12), 0.65);
		}
		color = mix(color, color * vec3(1.25, 1.2, 0.85), t * 0.6);
		color = mix(color, vec3(0.16, 0.12, 0.08), clamp(silt * 1.2, 0.0, 0.85));
		albedo = color * (0.8 + 0.2 * t);
		// (the wave: paler and a little grey green, more up the blade)
		albedo = mix(albedo, albedo * vec3(1.55, 1.6, 1.35) + vec3(0.02, 0.026, 0.016), clamp(gusts.w * gust_sheen * (0.5 + 0.5 * t), 0.0, 1.5));
	}
	along = t;
	world_position = point;
	sky_light = sky_light_at(point, point.y - root.y, world_normal);
	// The shadow maps looked up a little towards the sun: a blade stands a
	// hand above ground whose own depth the maps hold, coarser than that
	{
		vec3 looked_up = point + sun_direction * 0.3;

		shadow_position = light_view_projection * vec4(looked_up, 1.0);
		near_shadow_position = near_light_view_projection * vec4(looked_up, 1.0);
	}
	gl_Position = world_to_clip(point);
}
