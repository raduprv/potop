// Puddles (puddles.h): where the ground holds water after rain, and the
// thin water the simulation leaves on it. Shared by the terrain (their look:
// flat, dark, a mirror) and puddles.frag (what they mirror: the scene, by
// the water's screen space march). The same answer in both: the same reads.
//
// Wants the scene block (weather, ground_map, sky_occlusion) and, by these
// names: normal_texture (the terrain's: RGBA8 surface normal x, z; the wide
// material normal x, z), variation_texture (tileable cloudy noise) and the
// cell's surface (the terrain's surface_texture: silt, wetness, bedrock,
// water on it) and the sky it sees (sky_occlusion_texture's, raw).

// How much of a puddle is here (0..1): water gathers where the ground is
// nearly flat and lower than round it (a hollow: the wide normals spread
// in, their divergence below zero), in patches (two scales of noise); the
// wetter the ground (the rain's, under the open sky; the soil's own), the
// lower the patches it fills, so puddles grow from the hollows' middles out
// and shrink back as it dries. None under snow. The simulation's thin water
// on the ground is a puddle as it is
float puddle_amount(vec2 world_xz, vec4 surface, float sky_raw)
{
	float shelter = sky_occlusion.y > 0.5 ? smoothstep(0.35, 0.85, sky_raw) : 1.0;
	float wet = max(weather.y * shelter, surface.g * 0.8) * (1.0 - weather.w);
	float amount = 0.0;

	if (wet > 0.05)
	{
		float cells = ground_map.x;
		float cell = ground_map.y;
		vec2 uv = (world_xz / cell + 0.5) / cells;
		float step_uv = 2.0 / cells;
		vec2 middle = texture(normal_texture, uv).zw * 2.0 - 1.0;
		float tilt_flat = 1.0 - smoothstep(0.025, 0.07, length(middle));
		float spread = (texture(normal_texture, uv + vec2(step_uv, 0.0)).z - texture(normal_texture, uv - vec2(step_uv, 0.0)).z
			+ texture(normal_texture, uv + vec2(0.0, step_uv)).w - texture(normal_texture, uv - vec2(0.0, step_uv)).w) * 2.0 / (4.0 * cell);
		float hollow = clamp(-spread * 6.0, 0.0, 1.0);
		float patches = texture(variation_texture, world_xz / 9.0).r + 0.4 * texture(variation_texture, world_xz / 2.3 + 0.37).r;
		float level = 0.6 * hollow + 0.55 * patches;
		float threshold = 1.2 - 0.8 * wet;

		amount = smoothstep(threshold, threshold + 0.06, level) * tilt_flat;
	}
	return max(amount, surface.a);
}
