// What every lit shader knows of the scene: the camera, the sun, the sky,
// the shadows, the clouds, the water over the eye. One block, pushed once a
// pass to both stages (scene_uniforms.c fills it; the two must match).

layout(std140, set = UNIFORM_SET, binding = 0) uniform scene_block
{
	mat4 view_projection;            // world to the screen (depth 0..1, for drawing)
	mat4 inverse_view_projection;    // OpenGL style clip space back to the world (rays)
	mat4 light_view_projection;      // world to the sun's far shadow map (OpenGL style, for looking up)
	mat4 near_light_view_projection; // the fine shadow map around the camera's target
	vec3 eye_position;
	float time_seconds;
	vec3 sun_direction;              // points towards the sun
	float ambient_intensity;
	vec3 sun_color;                  // linear, includes intensity
	float fog_density;
	vec3 sky_color;                  // linear: the horizon (and the fog)
	float shadow_strength;           // 0 disables shadows
	vec3 sky_zenith_color;           // linear
	float sky_horizon_drop;
	vec4 cloud_parameters;           // coverage, softness, shadow strength, 1 / tiling meters
	vec3 cloud_motion;               // wind offset x, z (meters), cloud height
	float shadow_texel_size;
	vec4 underwater_color;           // rgb: linear color far things fade into; a: 1 when the eye is under water
	vec4 underwater_parameters;      // x: water surface over the eye (m), y: fog per meter, z: time (s)
	float near_shadow_texel_size;    // 0: no near cascade
	float volumetric_clouds;         // 1: the sky leaves out its flat clouds (the volumetric ones are drawn)
	float cloud_shadow_ready;        // 1: the volumetric clouds' shadow map is there (cloud_shadow_texture)
	float sky_time_seconds;         // the sky's clock (the clouds; the console's time sets how fast it goes)
	vec4 fog_parameters;            // x: the haze's scale height (meters): it is e times thinner that much higher; y: how much the stars show (0..1), z: the hour (0..24), w: the volumetric clouds' dome for the sky (0: none, 1: whole, 2: upper half)
	vec4 twilight;                  // the glow on the horizon where the sun rises or sets: xy its direction (x, z), z how much (0: none), w how bright
	vec4 moon;                      // xyz: towards the moon; w: 1 when the light (sun_direction) is not the sun
	vec4 moon_shape;                // x: phase (0 new, 0.5 full), y: cos of its angular radius, z: its disc's brightness, w: earthshine
	vec4 aurora;                    // xy: towards the north (x, z); z: how much it shows (0: none, aurora.h); w: its clock (seconds)
	vec4 weather;                   // x: how hard it rains (0..1, rain.h), y: how wet things are (0..1), z: the rain's clock (seconds), w: the snow on things (0..1)
	vec4 ground_map;                // the ground and water textures (ground_water.glsl): cells per side, cell size (m), 1 if they are there, 1 if the water's marks are
	vec4 sky_occlusion;             // x: how strong (0..), y: 1 if there is one (sky_occlusion.h; the ground, the grass, the objects bind it), z: its layers' texels per side (0: none; sky_occlusion.glsl)
	vec4 scene_wind;                // the plants' wind: xy where it blows (x, z), z how hard (0..1), w its clock (seconds)
	vec4 scene_gusts;               // x: 2 pi over the gusts' spacing (per meter), y: how fast they roll (m/s), z: -, w: how pale a gust's wave makes the grass
	vec4 sky_bounce;                // the light the land round sends back (sky_occlusion.glsl): x how much, y 1 if it is there
	vec4 temporal_jitter;           // xy: the buildings' shift this frame (clip space, times w: the temporal anti-aliasing's); z: 1 multisampled (alpha to coverage: scene_depth.glsl)
	mat4 view_projection_relative;  // as view_projection, from the eye: for world minus eye_position (world_to_clip)
	mat4 inverse_view_projection_relative; // as inverse_view_projection, to the world minus eye_position (clip_to_eye)
};

// A world position onto the screen (depth 0..1), by way of the eye: the
// eye taken away first (two floats near each other: exact), then a matrix
// with no translation in it. view_projection times a position kilometers
// out loses millimeters that change with the eye from frame to frame (the
// houses trembled under the temporal anti-aliasing)
vec4 world_to_clip(vec3 world)
{
	return view_projection_relative * vec4(world - eye_position, 1.0);
}

// OpenGL style clip space (ndc_depth -1..1; 1: the far plane) back to where
// it is from the eye (world minus eye_position): a ray's direction, how far
// a pixel is. By the matrix without the eye's translation: the other one,
// inverted with the eye kilometers out and the far plane kilometers off,
// gave rays that wobbled from frame to frame (the clouds, the fog flickered)
vec3 clip_to_eye(vec2 ndc, float ndc_depth)
{
	vec4 point = inverse_view_projection_relative * vec4(ndc, ndc_depth, 1.0);

	return point.xyz / point.w;
}

// The direction a point of the screen looks in, from the eye, through the
// near plane: through the far plane (ndc_depth 1) the w came out of two
// numbers of about 1 / near taken one from the other to leave 1 / far,
// all but lost with the far plane kilometers off (a big map): rays that
// moved as the camera turned, the sun's disc snapping back
vec3 eye_direction(vec2 ndc)
{
	return normalize(clip_to_eye(ndc, -1.0));
}

// The light the ground round sends up into a surface facing down, over
// ambient_intensity (the surroundings multiply by it): its warm albedo times
// the sky's light and the sun on it (the moon's at night, sun_direction
// then). The sun's share is what lights a shaded wall in the day: without
// it, the shade is only the sky's blue
const vec3 GROUND_ALBEDO = vec3(0.30, 0.26, 0.20);

// sunlit: how much of the ground there the sun reaches (0..1: in a shadow,
// the sun's share is gone)
vec3 ground_bounce_color(float sunlit)
{
	return GROUND_ALBEDO * (sky_color + sun_color * max(sun_direction.y, 0.0) * sunlit / max(ambient_intensity, 0.05));
}

// How much a gust bows the grass at spot now (0..1: grass.vert's)
float gust_sheen_at(vec2 spot)
{
	if (scene_wind.z <= 0.0)
	{
		return 0.0;
	}
	float travel = dot(spot, scene_wind.xy) * scene_gusts.x - scene_wind.w * scene_gusts.x * scene_gusts.y;
	float gust = clamp(0.5 + 0.35 * sin(travel) + 0.15 * sin(travel * 2.7 + 1.3), 0.0, 1.0);

	return smoothstep(0.45, 0.9, gust) * clamp(scene_wind.z * 3.0, 0.0, 1.0);
}

// The sky a point of the ground sees (sky_occlusion.h): 1 all of it, less
// in hollows, under trees; seen is the texel (0..1) at the point
float sky_seen(float seen)
{
	return sky_occlusion.y > 0.5 ? max(mix(1.0, seen, sky_occlusion.x), 0.0) : 1.0;
}


// The haze's density along a straight way between two heights, of what it
// is at height 0: it thins exponentially upwards (fog_parameters.x), and
// the heights go linearly from one end to the other
float haze_mean_density(float height_from, float height_to)
{
	float scale = max(fog_parameters.x, 10.0);
	float a = clamp(height_from, -2000.0, 100000.0) / scale;
	float b = clamp(height_to, -2000.0, 100000.0) / scale;

	return abs(b - a) < 1e-3 ? exp(-a) : (exp(-a) - exp(-b)) / (b - a);
}

// How much of the sky's color covers a point seen from the eye (0..1):
// exp(-(distance * density)^2), the density thinned with the height
float haze_amount(vec3 point)
{
	float depth = length(point - eye_position) * fog_density * haze_mean_density(eye_position.y, point.y);

	return 1.0 - exp(-depth * depth);
}
