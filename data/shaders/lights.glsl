// Local lights (see src/lights.h): fires, torches, candles, none casting
// shadows. The CPU sorts them into clusters (the screen in tiles, each
// tile cut in slices by the distance from the eye); a pixel looks only at
// the lights of its own cluster. Include after scene.glsl, with
// LIGHT_BINDING the fragment shader's first storage buffer slot (its
// sampler count).

layout(std430, set = TEXTURE_SET, binding = LIGHT_BINDING) readonly buffer light_buffer
{
	uvec4 light_layout;                  // tiles across, tiles down, slices, how many lights
	vec4 light_slicing;                  // x: the first slice's end (meters), y: slices per log of the distance, w: the darkness (0 day .. 1 night)
	vec4 light_screen;                   // xy: the scene pass's width and height in pixels (0: no clusters)
	vec4 light_data[];                   // per light: position and radius, color (linear, intensity in), then its side (xyz: the way it faces, w: above 0, one sided: lighting only what is in front of the plane that far behind it, facing that way)
};

layout(std430, set = TEXTURE_SET, binding = LIGHT_BINDING + 1) readonly buffer cluster_buffer
{
	uint cluster_data[];                 // per cluster four words (lights.c), the indices, then the decals (decals.glsl)
};

// The cluster of this pixel, point its place in the world (lights.c: the
// screen in tiles, each cut in slices by the distance from the eye); false
// when there are none (no layout: an editor's empty buffers). The tile
// from gl_FragCoord (the framebuffer's top row first; the tiles count from
// the bottom, as clip space's y), not by projecting point again. A cluster
// has four words: its lights' first index and count, its decals'
// (decals.glsl)
bool cluster_of(vec3 point, out uint cluster)
{
	cluster = 0u;
	if (light_layout.x == 0u || light_screen.x <= 0.0 || light_screen.y <= 0.0)
	{
		return false;
	}
	vec2 screen = clamp(vec2(gl_FragCoord.x / light_screen.x, 1.0 - gl_FragCoord.y / light_screen.y), 0.0, 0.9999);
	uint tile_x = uint(screen.x * float(light_layout.x));
	uint tile_y = uint(screen.y * float(light_layout.y));
	float distance_to_eye = length(point - eye_position);
	uint slice = distance_to_eye <= light_slicing.x ? 0u
		: min(uint(log(distance_to_eye / light_slicing.x) * light_slicing.y), light_layout.z - 1u);

	cluster = (slice * light_layout.y + tile_y) * light_layout.x + tile_x;
	return true;
}

// The light the local lights give a point, facing normal: what to multiply
// the albedo by. wrap: 0 for solid things; above 0, light that comes
// round from behind (leaves let it through)
vec3 local_light(vec3 point, vec3 normal, float wrap)
{
	uint cluster;

	if (light_layout.w == 0u || !cluster_of(point, cluster))
	{
		return vec3(0.0);
	}
	uint first = cluster_data[4u * cluster];
	uint count = cluster_data[4u * cluster + 1u];
	vec3 total = vec3(0.0);

	for (uint i = 0u; i < count; i++)
	{
		uint index = cluster_data[first + i];
		vec4 position_radius = light_data[3u * index];
		vec3 color = light_data[3u * index + 1u].rgb;
		vec4 side = light_data[3u * index + 2u];
		vec3 to_light = position_radius.xyz - point;
		float distance_squared = dot(to_light, to_light);
		float reach = distance_squared / (position_radius.w * position_radius.w);

		if (reach >= 1.0)
		{
			continue;
		}
		// The square law, softened near the light (it is not a point) and
		// brought smoothly to nothing at its radius
		float window = 1.0 - reach * reach;
		float falloff = window * window / (distance_squared + 0.25);
		float facing = dot(normal, to_light * inversesqrt(max(distance_squared, 1e-6)));

		facing = max(facing * (1.0 - wrap) + wrap, 0.0);
		// (one sided, a window's: only in front of its wall, coming in
		// over 20 cm; the wall itself just in front, lit grazing)
		float in_front = side.w > 0.0 ? smoothstep(0.0, 0.2, side.w - dot(to_light, side.xyz)) : 1.0;

		total += color * falloff * facing * in_front;
	}
	return total;
}
