// Bakes one picture of an impostor's grid (see impostor.glsl): the model,
// seen from the picture's direction with no perspective, its bounding
// sphere filling the picture's cell (the viewport). Depth: 0 at the near
// side of the sphere, 1 at the far side. A plant's (PLANT_BAKE,
// impostor_bake_plant.vert): its foliage's pictures as plant.vert takes
// them for the month (its month cells), and what plant.frag needs to tint
// its cards (plant_data).

layout(location = 0) in vec3 model_position;
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 3) in vec4 model_tangent;
#ifdef PLANT_BAKE
layout(location = 4) in vec4 plant_data;                  // plant.vert's
layout(location = 5) in vec4 plant_snow;                  // plant.vert's: w its month cell
#endif

layout(std140, set = UNIFORM_SET, binding = 0) uniform bake_block
{
	ivec4 frame;                   // xy: the picture in the grid; z: a plant's (impostor.glsl's plant pictures); w: its month cells (plant.glsl)
	vec4 sphere;                   // the model's bounding sphere: center, radius (model units)
#ifdef PLANT_BAKE
	vec4 month_offsets[PLANT_MONTH_CELLS];
#endif
};

layout(location = 0) out vec2 texture_uv;
layout(location = 1) out vec3 normal;
layout(location = 2) out float tangent_size;
layout(location = 3) out vec4 card;                       // a plant's plant_data (none: no tip, its own shade the middle)

void main()
{
	vec3 direction = frame.z != 0 ? impostor_plant_direction(frame.xy) : impostor_frame_direction(frame.xy);
	vec3 right;
	vec3 up;
	vec3 relative = model_position - sphere.xyz;

	impostor_basis(direction, right, up);
	texture_uv = model_uv;
	card = vec4(0.0, 0.0, 0.5, 0.0);
#ifdef PLANT_BAKE
	{
		int cell = plant_cell_of(plant_snow.w);

		texture_uv = plant_month_uv(model_uv, cell, month_offsets[clamp(cell, 0, PLANT_MONTH_CELLS - 1)], frame.w, 0.0, plant_data.z);
		card = plant_data;
	}
#endif
	normal = model_normal;
	tangent_size = abs(model_tangent.w);
	gl_Position = vec4(dot(relative, right) / sphere.w, dot(relative, up) / sphere.w, 0.5 - 0.5 * dot(relative, direction) / sphere.w, 1.0);
}
