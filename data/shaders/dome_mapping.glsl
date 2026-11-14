// The volumetric clouds' dome as a texture (see clouds_volume.glsl): a
// direction to its place in the octahedral map, and back. Apart from the
// clouds' own uniforms, so the water can look its reflections up in it.

// With the eye under the clouds the dome holds only the sky above the
// horizon and this far below it (radians): from up to a few hundred meters
// the curved layer is seen down to there, and below it is the ground. Then
// all of the dome's texels go to the sky that has clouds
#define DOME_BAND 0.2

vec2 sign_not_zero(vec2 value)
{
	return vec2(value.x >= 0.0 ? 1.0 : -1.0, value.y >= 0.0 ? 1.0 : -1.0);
}

// The whole sphere: the upper half is the diamond in the middle, the lower
// half folded out into the corners
vec2 sphere_coordinates(vec3 direction)
{
	vec2 point = direction.xz / (abs(direction.x) + abs(direction.y) + abs(direction.z));

	if (direction.y < 0.0)
	{
		point = (1.0 - abs(point.yx)) * sign_not_zero(point);
	}
	return point * 0.5 + 0.5;
}

vec3 sphere_direction(vec2 coordinates)
{
	vec2 point = coordinates * 2.0 - 1.0;
	float up = 1.0 - abs(point.x) - abs(point.y);

	if (up < 0.0)
	{
		point = (1.0 - abs(point.yx)) * sign_not_zero(point);
	}
	return normalize(vec3(point.x, up, point.y));
}

// The upper half (from DOME_BAND below the horizon) over the whole square:
// the elevation stretched from -DOME_BAND .. 90 degrees to 0 .. 90, then the
// half sphere's octahedron turned by 45 degrees to fill the square
vec2 half_coordinates(vec3 direction)
{
	float quarter = 1.5707963;
	float elevation = asin(clamp(direction.y, -1.0, 1.0));
	float stretched = clamp((elevation + DOME_BAND) / (quarter + DOME_BAND), 0.0, 1.0) * quarter;
	vec2 around = direction.xz;
	float around_length = length(around);
	vec3 warped;
	vec2 point;

	around = around_length > 1e-6 ? around / around_length : vec2(1.0, 0.0);
	warped = vec3(around.x * cos(stretched), sin(stretched), around.y * cos(stretched));
	point = warped.xz / (abs(warped.x) + abs(warped.y) + abs(warped.z));
	return vec2(point.x + point.y, point.x - point.y) * 0.5 + 0.5;
}

vec3 half_direction(vec2 coordinates)
{
	float quarter = 1.5707963;
	vec2 square = coordinates * 2.0 - 1.0;
	vec2 point = vec2(square.x + square.y, square.x - square.y) * 0.5;
	vec3 warped = normalize(vec3(point.x, max(1.0 - abs(point.x) - abs(point.y), 0.0), point.y));
	float stretched = asin(clamp(warped.y, 0.0, 1.0));
	float elevation = stretched / quarter * (quarter + DOME_BAND) - DOME_BAND;
	vec2 around = warped.xz;
	float around_length = length(around);

	around = around_length > 1e-6 ? around / around_length : vec2(1.0, 0.0);
	return vec3(around.x * cos(elevation), sin(elevation), around.y * cos(elevation));
}
