#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Shadow map pass for the plants (plant.vert): object_shadow.vert's, and
// the foliage fallen in the autumn casts no shadow, the branches sag under
// the winter's snow (plant.glsl).

#include "shadow_pass.glsl"
#include "plant.glsl"

layout(location = 0) in vec3 model_position;
layout(location = 2) in vec2 model_uv;
layout(location = 3) in vec4 instance_placement;         // x, y, z of the origin, size
layout(location = 4) in vec4 instance_rotation;          // quaternion (x, y, z, w)
layout(location = 7) in vec4 plant_data;
layout(location = 8) in vec4 plant_snow;                 // plant.vert's

layout(std140, set = UNIFORM_SET, binding = 1) uniform plant_shadow_block
{
	vec4 season;                   // plant.glsl (z 0: the bark, which never falls)
	vec4 snow;                     // plant.glsl
	vec4 shape;                    // x: the plant's height (meters, the model's); y: 1 for the foliage
	vec4 month_cells[PLANT_MONTH_CELLS];   // plant.glsl
	vec4 month_offsets[PLANT_MONTH_CELLS];
};

layout(location = 0) out vec2 texture_uv;

vec3 rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

void main()
{
	texture_uv = shape.y > 0.5 ? plant_month_uv(model_uv, month_cells, month_offsets, int(snow.z), snow.x, plant_data.z) : model_uv;
	if (plant_card_fallen(season, plant_autumn_of(season, instance_placement.xyz), plant_data))
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	if (plant_snow.w > 0.5 && snow.x < PLANT_SNOW_SHOWN)
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	vec3 position = plant_sagged(snow, shape.x, model_position) + plant_snow.xyz * snow.x;

	gl_Position = light_view_projection * vec4(instance_placement.xyz + rotate(instance_rotation, position) * instance_placement.w, 1.0);
}
