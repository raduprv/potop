// Seeing from under water (included by the terrain, object and water
// shaders, after their variation_texture). When the eye is under water,
// everything fades into the water's color with distance, the light gets
// bluer the deeper a thing lies, and sunlight focused by the ripples of the
// surface plays over the ground (caustics).

// underwater_color and underwater_parameters come with the scene (scene.glsl)

// Bright wobbling lines, where two drifting layers of cloudy noise cross
float underwater_caustics(vec2 position)
{
	float time = underwater_parameters.z;
	float a = texture(variation_texture, position * 0.21 + vec2(time * 0.031, time * 0.017)).r;
	float b = texture(variation_texture, position * 0.17 + vec2(-time * 0.023, time * 0.029) + 0.37).r;

	return pow(1.0 - abs(a - b), 12.0);
}

// lit: the linear color as lit above water; sun_light: the sunlight that
// reaches the point (for the caustics, black for none)
vec3 apply_underwater(vec3 lit, vec3 position, float view_distance, vec3 sun_light)
{
	if (underwater_color.a < 0.5)
	{
		return lit;
	}
	float below = max(underwater_parameters.x - position.y, 0.0);

	if (below > 0.0)
	{
		// The water swallows red first: deeper things turn the water's color
		vec3 tint = underwater_color.rgb / max(max(underwater_color.r, underwater_color.g), max(underwater_color.b, 0.001));

		lit *= mix(vec3(1.0), tint, clamp(below * 0.04, 0.0, 0.6));
		lit += sun_light * underwater_caustics(position.xz) * 0.6 * exp(-below * 0.1);
	}
	return mix(lit, underwater_color.rgb, clamp(1.0 - exp(-view_distance * underwater_parameters.y), 0.0, 1.0));
}
