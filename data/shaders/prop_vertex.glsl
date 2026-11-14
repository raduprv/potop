// The props' vertex shader's body (prop.vert; building.vert: with BUILDING,
// the weathering from a buffer of its own, not the tangent).
//
// The props' vertex shader (rocks, fences, walls: tools/potop_prop): as
// object.vert, without the wind (they stand still), and handing prop.frag
// what it lays their textures and their weathering by: the point and the
// normal in the model's own axes (meters, at the object's size), its
// vertex's weathering (the tangent of a triplanar material: see
// object_format.h) and how the object is turned.

layout(location = 0) in vec3 model_position;
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 3) in vec4 instance_placement;         // x, y, z of the origin, size
layout(location = 4) in vec4 instance_rotation;          // quaternion (x, y, z, w)
layout(location = 5) in float instance_fade;             // 1 = fully there, 0 = gone
layout(location = 6) in vec4 model_tangent;              // along the texture's U; w: the bitangent's sign (triplanar: the weathering)
#ifdef BUILDING
layout(location = 7) in vec4 model_weathering;           // a building's: open to the sky, edge, shelter (object_format.h)
layout(location = 8) in vec4 instance_look;              // its model's: age, its spread, lichen, moss (objects.c)
#endif

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec2 texture_uv;
layout(location = 3) out vec4 shadow_position;
layout(location = 4) out vec4 near_shadow_position;
layout(location = 5) flat out float fade;
layout(location = 6) out vec4 world_tangent;
layout(location = 7) out vec3 model_point;               // in the model's axes, meters at its size
layout(location = 8) out vec3 own_normal;                // in the model's axes
layout(location = 9) out vec4 weathering;                // the vertex's (its tangent as given)
layout(location = 10) flat out vec4 turning;             // the object's rotation
#ifdef BUILDING
layout(location = 11) flat out vec4 building_look;       // instance_look, on
#endif

vec3 rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}

void main()
{
	vec3 turned = rotate(instance_rotation, model_position);
	vec3 turned_normal = rotate(instance_rotation, model_normal);

	world_position = instance_placement.xyz + turned * instance_placement.w;
	world_normal = turned_normal;
	world_tangent = vec4(rotate(instance_rotation, model_tangent.xyz), model_tangent.w);
	texture_uv = model_uv;
	fade = instance_fade;
	model_point = model_position * instance_placement.w;
	own_normal = model_normal;
#ifdef BUILDING
	weathering = model_weathering;
	building_look = instance_look;
#else
	weathering = model_tangent;
#endif
	turning = instance_rotation;
	shadow_position = light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	near_shadow_position = near_light_view_projection * vec4(world_position + turned_normal * 0.05, 1.0);
	gl_Position = world_to_clip(world_position);
#ifdef BUILDING
	// The temporal anti-aliasing: the buildings alone a fraction of a pixel
	// off each frame (taa.frag averages them; the rest is left as it is)
	gl_Position.xy += temporal_jitter.xy * gl_Position.w;
#endif
}
