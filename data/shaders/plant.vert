#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "plant.glsl"
#define SKY_VERTEX_BINDING 0
#include "sky_vertex.glsl"

// A plant's vertex shader (tools/potop_plant's trees, bushes and small
// plants; object_format.h, OBJECT_FILE_PLANT): object.vert's, with what
// each vertex is on the plant (plant_data) handed on to plant.frag, and the
// autumn (plant.glsl): how far each card of the foliage has turned, and
// the cards fallen are not drawn (their four corners are put out of view,
// so they cost nothing past this).
//
// As object.vert:
//
// Model vertices are already in game axes (Y up, meters); the object is
// turned by its rotation (a quaternion: its yaw, or whatever the physics
// made of it), scaled and moved to its place on the map.
// Drawn instanced: the placement comes per object, from the instance buffer,
// together with how far the object has faded out (near its draw distance).
//
// Plants move in the wind (none of it is real: the physics and the shadows
// keep them still): the whole plant leans with it, more the higher up (with
// the square of the height, so the foot stays), the lean swelling in gusts
// that roll downwind as waves (a plant's place decides when one reaches
// it) and swinging a little on its own, only as far as the wind is over its
// stiffness (a bush stands still in a breeze); the leaves (cut out
// materials) shake fast besides, each card from its foot.
//
// The winter's snow (plant.glsl): the branches sag under it; or, where it
// was baked in, each vertex goes where the snow on things takes it, and the
// snow lying on the boughs grows from nothing (none: not drawn).

layout(location = 0) in vec3 model_position;
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 3) in vec4 instance_placement;         // x, y, z of the origin, size
layout(location = 4) in vec4 instance_rotation;          // quaternion (x, y, z, w)
layout(location = 5) in float instance_fade;             // 1 = fully there, 0 = gone
layout(location = 6) in vec4 model_tangent;              // along the texture's U; w: the bitangent's sign
layout(location = 7) in vec4 plant_data;                 // along its branch or card, height on the plant, its own number, out in the crown or thickness
layout(location = 8) in vec4 plant_snow;                 // its baked snow (object_format.h, OBJECT_FILE_SNOW): where all of it takes it; w: of the snow lying on it, and its month cell (plant_cell_of, plant_snow_lying)

layout(std140, set = UNIFORM_SET, binding = 1) uniform wind_block
{
	vec4 wind;                     // xy: where it blows towards (x, z); z: how hard (0..1); w: time (seconds)
	vec4 gusts;                    // x: 2 pi over the gusts' spacing (per meter); y: how fast they roll (m/s)
	vec4 plant;                    // x: its top's lean in a gale (meters at size 1); y: its leaves' shaking (meters); z: its lowest point; w: its height
	vec4 cut_out;                  // x: 1 for a cut out material (the leaves shake); y: the deepest branch level the next level of detail keeps (0: it keeps all); z: its stiffness (0..1); w: 2 for its fruit (each shakes as one)
	vec4 season;                   // plant.glsl
	vec4 snow;                     // plant.glsl (z: how many month_cells; below 0: by the snow)
	vec4 month_cells[PLANT_MONTH_CELLS];   // plant.glsl
	vec4 month_offsets[PLANT_MONTH_CELLS];
};

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec2 texture_uv;
layout(location = 3) out vec4 shadow_position;
layout(location = 4) out vec4 near_shadow_position;
layout(location = 5) flat out float fade;
layout(location = 6) out vec4 world_tangent;
// 1 where this vertex's branch goes away at the next level of detail: a
// plant's bark carries its branch's level in the tangent's size
layout(location = 7) flat out float level_drops;
layout(location = 8) out vec4 plant_info;               // plant_data, as given
layout(location = 9) out float turned;                  // how far its card has turned in the autumn (0..1)
layout(location = 11) out float haze;                   // how much the haze hides it (scene.glsl's haze_amount, per vertex: it changes slowly over a blade, a card, a picture)
layout(location = 10) out vec4 sky_light;              // the light the land round sends back (rgb), the sky seen (a): sky_vertex.glsl

vec3 rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

// How far the wind has moved a vertex (at position on the plant, sagged)
// at its clock (wind.w)
vec3 wind_offset(float clock, vec3 position)
{
	if (wind.z <= 0.0 || (plant.x <= 0.0 && plant.y <= 0.0))
	{
		return vec3(0.0);
	}
	float size = instance_placement.w;
	float height = clamp((position.y - plant.z) / max(plant.w, 0.01), 0.0, 1.0);
	float own_phase = fract(sin(dot(instance_placement.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
	float travel = dot(instance_placement.xz, wind.xy) * gusts.x - clock * gusts.x * gusts.y;
	float gust = clamp(0.5 + 0.35 * sin(travel) + 0.15 * sin(travel * 2.7 + 1.3), 0.0, 1.0);
	// How hard it blows here now (the gusts rolling over it), and how
	// much of that bends the plant: nothing up to its stiffness, then
	// with the square of what is over it (the wind's push grows with its
	// speed squared; a stiff bush stands still but in a storm's gusts)
	float stiffness = clamp(cut_out.z, 0.0, 0.95);
	float blowing = wind.z * (0.3 + 0.7 * gust);
	float over = clamp((blowing - stiffness) / (1.0 - stiffness), 0.0, 1.0);
	float lean = over * over + 0.1 * over * sin(clock * (1.5 + 0.5 * own_phase / 6.2831853) + own_phase);
	vec2 push = wind.xy * plant.x * size * height * height * lean;
	// Leaning, the top comes down a little (it bends, it does not stretch)
	float drop = dot(push, push) / max(2.0 * height * plant.w * size, 0.05);
	vec3 offset = vec3(push.x, -drop, push.y);

	// The leaves shake whatever the stiffness, each card as one (its own
	// number its phase), from its foot (still: on its branch) to its
	// tip (the most)
	if (cut_out.x > 0.5 && plant.y > 0.0)
	{
		float card_phase = plant_data.z * 6.2831853 * 7.0 + own_phase;
		vec3 shake = vec3(sin(clock * 11.0 + card_phase), 0.6 * sin(clock * 13.0 + card_phase * 1.3),
			cos(clock * 9.0 + card_phase * 0.7));

		offset += shake * plant.y * size * plant_data.x * wind.z * (0.3 + 0.7 * gust);
	}
	// Its fruit with the leaves round it: each as one (all its vertices and
	// its stalk's one number, the grower's), its stalk from still at the
	// branch to as much as the fruit (along it)
	else if (cut_out.w > 1.5 && plant.y > 0.0)
	{
		float fruit_phase = plant_data.z * 6.2831853 * 7.0 + own_phase;
		vec3 shake = vec3(sin(clock * 11.0 + fruit_phase), 0.4 * sin(clock * 13.0 + fruit_phase * 1.3),
			cos(clock * 9.0 + fruit_phase * 0.7));

		offset += shake * plant.y * size * 0.6 * plant_data.x * wind.z * (0.3 + 0.7 * gust);
	}
	return offset;
}

void main()
{
	// The autumn: how far this card has turned, and whether it has fallen
	{
		float tree_autumn = plant_autumn_of(season, instance_placement.xyz);

		plant_info = plant_data;
		turned = cut_out.x > 0.5 ? plant_card_turned(season, tree_autumn, plant_data) : 0.0;
		if (cut_out.x > 0.5 && plant_card_fallen(season, tree_autumn, plant_data))
		{
			// Out of view, all four corners: nothing of it is drawn
			gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
			world_position = vec3(0.0);
			world_normal = vec3(0.0, 1.0, 0.0);
		sky_light = vec4(0.0, 0.0, 0.0, 1.0);
			sky_light = vec4(0.0, 0.0, 0.0, 1.0);
			world_tangent = vec4(1.0, 0.0, 0.0, 1.0);
			texture_uv = vec2(0.0);
			shadow_position = vec4(0.0);
			near_shadow_position = vec4(0.0);
			fade = 1.0;
			level_drops = 0.0;
			return;
		}
	}
	// (the snow lying on it: not there without snow)
	if (plant_snow_lying(plant_snow.w) && snow.x < PLANT_SNOW_SHOWN)
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		world_position = vec3(0.0);
		world_normal = vec3(0.0, 1.0, 0.0);
		sky_light = vec4(0.0, 0.0, 0.0, 1.0);
		world_tangent = vec4(1.0, 0.0, 0.0, 1.0);
		texture_uv = vec2(0.0);
		shadow_position = vec4(0.0);
		near_shadow_position = vec4(0.0);
		fade = 1.0;
		level_drops = 0.0;
		return;
	}
	vec3 position = plant_sagged(snow, plant.w, model_position) + plant_snow.xyz * snow.x;
	vec3 turned = rotate(instance_rotation, position);
	vec3 turned_normal = rotate(instance_rotation, model_normal);

	world_position = instance_placement.xyz + turned * instance_placement.w;
	world_position += wind_offset(wind.w, position);
	world_normal = turned_normal;
	// (over the ground: over the plant's foot)
	sky_light = sky_light_at(world_position, world_position.y - instance_placement.y, turned_normal);
	world_tangent = vec4(rotate(instance_rotation, model_tangent.xyz), model_tangent.w);
	if (cut_out.x > 0.5)
	{
		int cell = plant_cell_of(plant_snow.w);

		texture_uv = plant_month_uv(model_uv, cell, month_offsets[clamp(cell, 0, PLANT_MONTH_CELLS - 1)], int(snow.z), snow.x, plant_data.z);
	}
	else
	{
		texture_uv = model_uv;
	}
	fade = instance_fade;
	level_drops = cut_out.y > 0.0 && cut_out.x < 0.5 && abs(model_tangent.w) > cut_out.y + 0.5 ? 1.0 : 0.0;
	// A small offset along the normal keeps lit faces from shadowing
	// themselves, as on the terrain. (The leaves take the far map alone,
	// plant.frag: their crown's own shadow from it, broad and soft, the
	// same at every distance: no jump where the near map gives way)
	vec3 looked_up = world_position + turned_normal * 0.05;

	shadow_position = light_view_projection * vec4(looked_up, 1.0);
	near_shadow_position = near_light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	haze = clamp(haze_amount(world_position), 0.0, 1.0);
	gl_Position = world_to_clip(world_position);
}
