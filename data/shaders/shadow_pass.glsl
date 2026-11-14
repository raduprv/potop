// Shadow map passes (terrain, objects, actors): the sun's view, pushed once
// a pass.

layout(std140, set = UNIFORM_SET, binding = 0) uniform shadow_pass_block
{
	mat4 light_view_projection;      // world to the shadow map (depth 0..1, for drawing)
};
