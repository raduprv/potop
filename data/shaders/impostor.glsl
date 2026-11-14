// Impostors (see src/impostors.h): an object seen from far away is drawn as
// a picture of itself, picked from a grid of pictures taken from all round
// and above (a hemisphere). The grid is the hemisphere unfolded on a square
// (hemi-octahedral): the picture at grid point (i, j) looks along
// impostor_frame_direction(i, j). The baking (impostor_bake.vert) and the
// drawing (impostor.vert) share what is here, so that both agree on where
// each picture looks from and how it lies in its cell.

#define IMPOSTOR_GRID 8

// A direction on the upper hemisphere to the square -1..1 (and back)
vec2 impostor_hemi_encode(vec3 direction)
{
	direction.y = max(direction.y, 0.0);
	direction /= abs(direction.x) + direction.y + abs(direction.z);
	return vec2(direction.x + direction.z, direction.x - direction.z);
}

vec3 impostor_hemi_decode(vec2 square)
{
	vec2 flat_part = vec2(square.x + square.y, square.x - square.y) * 0.5;

	return normalize(vec3(flat_part.x, max(1.0 - abs(flat_part.x) - abs(flat_part.y), 0.0), flat_part.y));
}

// The direction a picture of the grid looks from (from the object towards
// the eye that took it)
vec3 impostor_frame_direction(ivec2 frame)
{
	return impostor_hemi_decode(vec2(frame) / float(IMPOSTOR_GRID - 1) * 2.0 - 1.0);
}

// The picture's axes: right and up on the picture, for a picture taken from
// direction (an eye looking back along it)
void impostor_basis(vec3 direction, out vec3 right, out vec3 up)
{
	vec3 reference = abs(direction.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);

	right = normalize(cross(reference, direction));
	up = cross(direction, right);
}

// Where a point of a picture (-1..1 on its axes) lies in the atlas
vec2 impostor_atlas_uv(ivec2 frame, vec2 on_picture)
{
	return (vec2(frame) + vec2(0.5 + 0.5 * on_picture.x, 0.5 - 0.5 * on_picture.y)) / float(IMPOSTOR_GRID);
}

vec3 impostor_rotate(vec4 rotation, vec3 vector)
{
	vec3 twice_cross = 2.0 * cross(rotation.xyz, vector);

	return vector + rotation.w * twice_cross + cross(rotation.xyz, twice_cross);
}
