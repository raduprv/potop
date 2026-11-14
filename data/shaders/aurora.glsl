// The aurora (see src/aurora.h), for sky.glsl: curtains of light in a
// layer from about 95 km to 300 km up, over the north. A ray from the eye
// is followed up through the layer (over the round earth, so towards the
// horizon it meets it far away) in steps of height, fine at the bottom
// (where the light is, its edge sharp) and coarser above; at each step the
// curtains are bands running east to west, wavy, their waves rolling
// slowly, made of vertical rays that shimmer. The light is brightest at
// a curtain's sharp lower edge and fades upwards; green (oxygen, low),
// going red and violet high up, with a pink fringe at the very bottom.
// Needs the scene (aurora: north, how much, clock).

#define AURORA_STEPS 24
#define AURORA_EARTH_KM 6371.0
#define AURORA_BOTTOM_KM 95.0
#define AURORA_TOP_KM 300.0

// Where in its step each pixel samples (interleaved gradient noise: an
// even grain, finer than random)
float aurora_jitter()
{
	return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

// How bright a set of curtains is at a point of the layer: across (km
// north of the eye), along (km east), how far the waves have rolled
float aurora_curtains(float across, float along, float clock, float blur_km)
{
	// Two bands of curtains, the nearer one brighter
	float total = 0.0;

	for (int band = 0; band < 2; band++)
	{
		float offset = band == 0 ? 280.0 : 520.0;
		float wave = offset + 110.0 * sin(along * 0.0035 + clock * 0.03 + float(band) * 2.1)
			+ 55.0 * sin(along * 0.0091 - clock * 0.05 + float(band)) + 18.0 * sin(along * 0.031 + clock * 0.17);
		float distance_km = across - wave;
		// Thin curtains, a little thicker for the far band
		// (wider far away, where a step covers much: no grain)
		float width = (band == 0 ? 22.0 : 35.0) + blur_km;
		float curtain = exp(-distance_km * distance_km / (2.0 * width * width));
		// The rays along it, shimmering
		// (smoothed away where a step covers more than a ray: no grain)
		float ray_contrast = 0.45 * exp(-blur_km * blur_km * 0.002);
		float rays = 1.0 - ray_contrast + ray_contrast * sin(along * 0.22 + clock * 0.6 + 3.0 * sin(along * 0.043 - clock * 0.11));

		total += curtain * rays * (band == 0 ? 1.0 : 0.55);
	}
	return total;
}

vec3 aurora_light(vec3 direction)
{
	if (aurora.z <= 0.0 || direction.y <= 0.0)
	{
		return vec3(0.0);
	}
	vec2 north = aurora.xy;
	vec2 east = vec2(-north.y, north.x);
	float sine = direction.y;
	vec2 flat_direction = direction.xz / max(length(direction.xz), 1e-4);
	float clock = aurora.w;
	float jitter = aurora_jitter();
	float range_km = AURORA_TOP_KM - AURORA_BOTTOM_KM;
	float cosine = sqrt(max(1.0 - sine * sine, 0.0));
	vec3 total = vec3(0.0);

	for (int i = 0; i < AURORA_STEPS; i++)
	{
		// The heights go with the square of the step: close together low down
		float u = (float(i) + jitter) / float(AURORA_STEPS);
		float height = AURORA_BOTTOM_KM + range_km * u * u;
		float step_km = range_km * 2.0 * u / float(AURORA_STEPS) + 0.2;
		// How far along the ground the ray is under that height (the earth
		// is round: towards the horizon, far)
		float b = AURORA_EARTH_KM * sine;
		float way = -b + sqrt(b * b + 2.0 * AURORA_EARTH_KM * height + height * height);
		float ground = way * cosine;
		vec2 place = flat_direction * ground;
		float across = dot(place, north);
		float along = dot(place, east);
		// Bright at the lower edge, fading upwards
		float profile = smoothstep(AURORA_BOTTOM_KM, AURORA_BOTTOM_KM + 10.0, height) * exp(-(height - 105.0) / 70.0);
		vec3 color = mix(vec3(0.15, 1.0, 0.45), vec3(0.75, 0.12, 0.45), smoothstep(150.0, 260.0, height));

		color += vec3(0.9, 0.2, 0.6) * (1.0 - smoothstep(AURORA_BOTTOM_KM, AURORA_BOTTOM_KM + 6.0, height)) * 0.8;
		// How far apart, along the ground, the steps are here: the
		// curtains blurred that much
		float blur = step_km * cosine / max(sine, 0.05) * 0.8;

		total += color * profile * aurora_curtains(across, along, clock, blur) * step_km;
	}
	// Dimmed near the horizon (a long way through the air)
	return total * (1.0 - exp(-sine * 12.0)) * aurora.z * (0.6 / range_km);
}
