// The valley fog (see src/fog.h), what its shaders share: their block, the
// fog's density at a point and how it scatters the light. Included after
// scene.glsl.
//
// The fog lies in the valleys and over the water, as the valley map says
// (src/fog.c: for each place of the map, how high its fog's top is and how
// much fog there is), up to a soft top torn into wisps by a slow noise that
// drifts with the wind; how thick, by the hour (it forms in the evening,
// lifts in the morning: the block's fog_look.z).

layout(std140, set = UNIFORM_SET, binding = 1) uniform fog_block
{
	vec4 fog_look;                  // x: density (per meter, a full valley's), y: the top's softness (m), z: how much there is now (0..1), w: how much forward it scatters (0..0.9)
	vec4 fog_slices;                // x: the nearest froxels (m), y: the farthest (m), z: slices, w: the frame
	vec4 fog_map;                   // x: the map's extent (m), y, z: the drift (m/s, x and z), w: the clock (s)
	vec4 fog_grid;                  // x, y: froxels across and down, z: 1 / ln(farthest / nearest), w: steps (the simple fog's)
	vec4 fog_bounds;                // x: the highest the fog reaches anywhere (m), y: how much the shadows darken it (0..1), z: its light's share (dimmer by night)
	mat4 fog_previous_view_projection;  // the frame before's, from its eye (world minus fog_previous_eye) to clip space (OpenGL style): where its froxels were
	vec4 fog_previous_eye;          // xyz: the frame before's eye; w: 1 if its froxels can be taken
	vec4 fog_jitter;                // xy: this frame's offset in a froxel (-0.5..0.5), z: along the slice (0..1), w: how much of this frame goes in
	vec4 fog_sun;                   // xy: the sun on the screen (0..1, the first row the top; may be off it), z: its rays' strength (0: none)
};

// The part of a ray (from the eye, up to a distance) that is low enough to
// be in the fog: false if none
bool fog_span(vec3 direction, float distance_meters, out float enter, out float leave)
{
	float highest = fog_bounds.x;

	enter = 0.0;
	leave = distance_meters;
	if (abs(direction.y) < 1e-5)
	{
		return eye_position.y < highest;
	}
	float crossing = (highest - eye_position.y) / direction.y;

	if (eye_position.y < highest)
	{
		leave = direction.y > 0.0 ? min(crossing, distance_meters) : distance_meters;
	}
	else
	{
		if (direction.y >= 0.0)
		{
			return false;
		}
		enter = crossing;
	}
	return enter < leave;
}

float fog_hash(vec3 point)
{
	point = fract(point * 0.3183099 + 0.1);
	point *= 17.0;
	return fract(point.x * point.y * point.z * (point.x + point.y + point.z));
}

// Smooth value noise (0..1)
float fog_noise(vec3 point)
{
	vec3 cell = floor(point);
	vec3 inside = fract(point);
	vec3 weight = inside * inside * (3.0 - 2.0 * inside);

	return mix(mix(mix(fog_hash(cell), fog_hash(cell + vec3(1.0, 0.0, 0.0)), weight.x),
			mix(fog_hash(cell + vec3(0.0, 1.0, 0.0)), fog_hash(cell + vec3(1.0, 1.0, 0.0)), weight.x), weight.y),
		mix(mix(fog_hash(cell + vec3(0.0, 0.0, 1.0)), fog_hash(cell + vec3(1.0, 0.0, 1.0)), weight.x),
			mix(fog_hash(cell + vec3(0.0, 1.0, 1.0)), fog_hash(cell + vec3(1.0, 1.0, 1.0)), weight.x), weight.y), weight.z);
}

// The fog's density at a point (per meter). valley: the valley map (r: the
// top's height, g: how much)
float valley_fog_density(vec3 point, sampler2D valley)
{
	vec2 on_map = point.xz / max(fog_map.x, 1.0);

	if (fog_look.z <= 0.0 || on_map.x < 0.0 || on_map.y < 0.0 || on_map.x > 1.0 || on_map.y > 1.0)
	{
		return 0.0;
	}
	vec2 here = textureLod(valley, on_map, 0.0).rg;
	float softness = max(fog_look.y, 0.5);
	// Under its top, thinning into it
	float layer = 1.0 - smoothstep(here.x - softness, here.x + softness * 0.5, point.y);

	if (layer * here.y <= 0.001)
	{
		return 0.0;
	}
	vec3 drifted = point - vec3(fog_map.y, 0.0, fog_map.z) * fog_map.w;
	// Big calm banks, long across and flat, a little finer detail on them
	float noise = fog_noise(drifted * vec3(1.0 / 45.0, 1.0 / 12.0, 1.0 / 45.0)) * 0.75 + fog_noise(drifted * vec3(0.07, 0.12, 0.07) + 7.1) * 0.25;
	// Near the eye it is all round it: evened out (no blots on the glass)
	noise = mix(0.6, noise, smoothstep(4.0, 30.0, length(point - eye_position)));
	// Thick inside, thinning into soft wisps towards the top
	float wisps = smoothstep(0.1, 0.8, noise + (layer - 0.5) * 0.8);

	return fog_look.x * fog_look.z * here.y * layer * wisps;
}

// How much light it sends towards the eye from the sun's way (Henyey
// Greenstein, times 4 pi: 1 for fog that scatters every way alike)
float fog_phase(float cosine)
{
	float g = clamp(fog_look.w, 0.0, 0.9);
	float denominator = 1.0 + g * g - 2.0 * g * cosine;

	return (1.0 - g * g) / (denominator * sqrt(denominator));
}

// The light a bit of fog at a point sends towards the eye, per its density
// (sunlit: the sun's visibility there): the sun's straight from it, the
// sky's, and the sun's after bouncing round in the fog many times (milky
// white, even where the sun is hidden; a rough stand in for it); of the
// sky's, sky_share (sky_occlusion.glsl: less in a wood, a crater)
vec3 fog_light(vec3 towards_eye_from_point, float sunlit, float sky_share)
{
	// None of the sun (or the moon) from under the horizon, nor its glow
	// towards it; what bounces round reaches the shadows only a little
	float up = smoothstep(-0.02, 0.06, sun_direction.y);
	float lit = sunlit * up;

	return (sun_color * (lit * fog_phase(dot(-towards_eye_from_point, sun_direction)) + 0.3 * up * (0.1 + 0.9 * lit))
		+ sky_color * ambient_intensity * 1.4 * sky_share) * fog_bounds.z;
}

// The sun's way through to a point, as the fog takes it: the far shadow
// map only (coarse, so already the shadow's mean over a froxel's breadth;
// the near one's fine twigs would flash on and off as the froxels move),
// one tap; how much the shadows darken it, fog_bounds.y
float fog_sun_visibility(vec3 point, sampler2DShadow far_shadow)
{
	if (shadow_strength <= 0.0 || shadow_texel_size <= 0.0)
	{
		return 1.0;
	}
	vec4 far_clip = light_view_projection * vec4(point, 1.0);
	vec3 far_position = far_clip.xyz / far_clip.w * 0.5 + 0.5;

	far_position.y = 1.0 - far_position.y;
	if (all(greaterThanEqual(far_position, vec3(0.0))) && all(lessThanEqual(far_position, vec3(1.0))))
	{
		return mix(1.0, textureLod(far_shadow, far_position, 0.0), shadow_strength * fog_bounds.y);
	}
	return 1.0;
}

// Where a ray from the eye leaves the map (m; the fog lies on it only)
float fog_map_exit(vec3 direction)
{
	float extent = max(fog_map.x, 1.0);
	float exit_distance = 1e9;

	if (abs(direction.x) > 1e-6)
	{
		exit_distance = min(exit_distance, ((direction.x > 0.0 ? extent : 0.0) - eye_position.x) / direction.x);
	}
	if (abs(direction.z) > 1e-6)
	{
		exit_distance = min(exit_distance, ((direction.z > 0.0 ? extent : 0.0) - eye_position.z) / direction.z);
	}
	return max(exit_distance, 0.0);
}

// The fog beyond the froxels (past fog_slices.y), out to what the pixel
// sees, the sky as far as the map goes: the far valleys and the milky
// horizon when the eye is in it. A few steps through the part of it low
// enough for fog, dithered by the pixel and turned a golden step each frame
// (the anti-aliasing smooths the dots the long steps leave: held still, the
// pattern showed on the horizon); rgb: the light it sends back, a: how much
// of what is behind still shows
vec4 fog_beyond(vec3 direction, float distance_meters, vec2 pixel, sampler2D valley, sampler2DShadow far_shadow)
{
	float enter;
	float leave;
	float farthest = min(distance_meters, fog_map_exit(direction));

	if (farthest <= fog_slices.y || !fog_span(direction, farthest, enter, leave))
	{
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	enter = max(enter, fog_slices.y);
	if (leave <= enter)
	{
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	const int steps = 16;
	float step_meters = (leave - enter) / float(steps);
	float jitter = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))) + fog_slices.w * 0.618034);
	vec3 gathered = vec3(0.0);
	float through = 1.0;

	for (int i = 0; i < steps && through > 0.01; i++)
	{
		vec3 point = eye_position + direction * (enter + (float(i) + jitter) * step_meters);
		float density = valley_fog_density(point, valley);

		if (density > 0.0)
		{
			float step_through = exp(-density * step_meters);

			gathered += through * fog_light(-direction, fog_sun_visibility(point, far_shadow), 1.0) * (1.0 - step_through);
			through *= step_through;
		}
	}
	return vec4(gathered, through);
}

// The sun's rays (fog_rays.comp) a pixel adds over what it sees: opacity,
// how much of it the fog hides (the more fog between, the more of them)
vec3 fog_rays_light(sampler2D rays, vec2 pixel, vec2 screen_size, float opacity)
{
	if (fog_sun.z <= 0.0)
	{
		return vec3(0.0);
	}
	float up = smoothstep(-0.02, 0.06, sun_direction.y);

	return sun_color * (fog_sun.z * up * opacity * textureLod(rays, pixel / screen_size, 0.0).r) * fog_bounds.z;
}
