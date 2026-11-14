// Skinning for actors (included by actor.vert and actor_shadow.vert).
//
// Each bone's skinning matrix (its pose times its inverse bind pose) comes
// as three rows of a 3x4 matrix; a vertex is moved by up to four bones,
// weighted. MAX_BONES must match ACTOR_MAX_BONES_DRAWN in actors.c.

#define MAX_BONES 64

// The actor's placement on the map (turned by a quaternion, scaled,
// moved), whether it is highlighted, and its bones
layout(std140, set = UNIFORM_SET, binding = 1) uniform actor_block
{
	vec4 actor_placement;           // x, y, z of the origin, size
	vec4 actor_rotation;            // quaternion (x, y, z, w)
	vec4 actor_highlight;           // x: 1 drawn highlighted (see object.frag)
	vec4 bone_rows[MAX_BONES * 3];
};

layout(location = 3) in uvec4 model_bones;               // bone indices
layout(location = 4) in vec4 model_bone_weights;         // sum to 1

// The weighted sum of the vertex's bone matrices, as three rows
void skinning_rows(out vec4 row_0, out vec4 row_1, out vec4 row_2)
{
	row_0 = vec4(0.0);
	row_1 = vec4(0.0);
	row_2 = vec4(0.0);
	for (int k = 0; k < 4; k++)
	{
		int bone = int(model_bones[k]) * 3;
		float weight = model_bone_weights[k];

		row_0 += bone_rows[bone] * weight;
		row_1 += bone_rows[bone + 1] * weight;
		row_2 += bone_rows[bone + 2] * weight;
	}
}

vec3 skin_point(vec4 row_0, vec4 row_1, vec4 row_2, vec3 point)
{
	vec4 extended = vec4(point, 1.0);

	return vec3(dot(row_0, extended), dot(row_1, extended), dot(row_2, extended));
}

// Bones only turn and move, so normals take the rotation part as it is
vec3 skin_direction(vec4 row_0, vec4 row_1, vec4 row_2, vec3 direction)
{
	return vec3(dot(row_0.xyz, direction), dot(row_1.xyz, direction), dot(row_2.xyz, direction));
}

vec3 rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}
