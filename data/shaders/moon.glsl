// The moon on the sky (scene.glsl's moon, moon_shape). Include after
// scene.glsl, with moon_clouds(direction) defined (how much the clouds
// cover a direction, 0..1) and, for its map, MOON_MAP its sampler (else it
// is a plain gray ball: the water's mirror).

// The moon (moon, moon_shape): a ball seen from far, its near side to us,
// north up, its map on it; lit from the side its phase says (waxing: from
// the right), its craters casting their shadows along the line between
// day and night (normals from its height map), and its dark part faintly
// lit by the earth. rgb: its light added to the sky's; a: how much of the
// pixel it covers (the stars do not show through it)
#define MOON_RELIEF 18.0
vec4 moon_disc(vec3 direction, float pixel_angle)
{
	vec3 towards = moon.xyz;
	float across_cosine = dot(direction, towards);

	if (moon_shape.z <= 0.0 || across_cosine < moon_shape.y)
	{
		return vec4(0.0);
	}
	// Where on the disc: x right, y up, 0..1 from the middle to the edge
	vec3 right = normalize(cross(towards, vec3(0.0, 1.0, 0.0)));
	vec3 up = cross(right, towards);
	float radius = sqrt(max(1.0 - moon_shape.y * moon_shape.y, 1e-8));
	vec2 disc = vec2(dot(direction, right), dot(direction, up)) / radius;
	float reach = dot(disc, disc);

	if (reach >= 1.0)
	{
		return vec4(0.0);
	}
	// The point on the ball (z towards us) and where it is on the map
	vec3 normal = vec3(disc, sqrt(1.0 - reach));
	float longitude = atan(normal.x, normal.z);
	float latitude = asin(clamp(normal.y, -1.0, 1.0));
	vec2 uv = vec2(0.5 + longitude / 6.2831853, 0.5 - latitude / 3.1415927);
#ifdef MOON_MAP
	vec2 texel = 1.0 / vec2(textureSize(MOON_MAP, 0));
	// The map's level from how many pixels the disc covers (half the map's
	// width is seen across it); from the angle, not the derivatives, which
	// mean nothing in here (not every pixel comes this far)
	float level = max(log2(0.5 / texel.x / max(2.0 * radius / max(pixel_angle, 1e-6), 1.0)), 0.0);
	vec4 map = textureLod(MOON_MAP, uv, level);
	// The height's slope along the map, east and north, into the normal
	vec2 step_size = texel * exp2(level);
	float east_slope = (textureLod(MOON_MAP, uv + vec2(step_size.x, 0.0), level).a - textureLod(MOON_MAP, uv - vec2(step_size.x, 0.0), level).a)
		/ (2.0 * step_size.x * 6.2831853 * max(cos(latitude), 0.05));
	float north_slope = (textureLod(MOON_MAP, uv - vec2(0.0, step_size.y), level).a - textureLod(MOON_MAP, uv + vec2(0.0, step_size.y), level).a)
		/ (2.0 * step_size.y * 3.1415927);
	vec3 east = vec3(cos(longitude), 0.0, -sin(longitude));
	vec3 north = vec3(-sin(latitude) * sin(longitude), cos(latitude), -sin(latitude) * cos(longitude));
	vec3 bumped = normalize(normal - (east * east_slope + north * north_slope) * (MOON_RELIEF / 1737.0));
#else
	// Without the map (the water's mirror, rippled anyway): its mean gray
	vec4 map = vec4(vec3(0.42), 0.5);
	vec3 bumped = normal;
#endif
	// The sun on it: behind it at new moon, from the right at first
	// quarter, behind us at full
	float elongation = moon_shape.x * 6.2831853;
	vec3 sun_on_moon = vec3(sin(elongation), 0.0, -cos(elongation));
	float incidence = dot(bumped, sun_on_moon);
	// Lommel-Seeliger: dust lights about evenly, so the full moon is a flat
	// disc, not a ball; softened over the line between day and night
	float lit = max(incidence, 0.0) / (max(incidence, 0.0) + max(normal.z, 0.05)) * 2.0;
	vec3 albedo = pow(map.rgb, vec3(2.2));
	vec3 light = albedo * moon_shape.z * (lit * smoothstep(-0.02, 0.06, incidence) + moon_shape.w);
	// A soft edge, behind clouds, dim low down (through more air)
	float edge = 1.0 - smoothstep(0.92, 1.0, reach);
	float clear = 1.0 - moon_clouds(direction) * 0.95;
	float air = smoothstep(-0.02, 0.15, towards.y);

	return vec4(light * edge * clear * mix(vec3(1.0, 0.6, 0.4), vec3(1.0), air), edge * clear);
}

