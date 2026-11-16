// The sky's occlusion for things on the ground (sky_occlusion.h): how much
// of the sky a point sees, from the ground's value under it and the four
// layers in the air above it (2, 6, 15, 35 m up: sky_occlusion_layer_heights),
// between them by its height above the ground; above the last, the last.
//
// Wants the scene block, and before it: sky_occlusion_texture (R8 a cell),
// sky_layers_texture (RGBA8: the four layers in one texel, r the lowest:
// scene sky_occlusion.z texels per side), and for object_sky_seen
// ground_height_texture (R32F; without it, SKY_OCCLUSION_NO_GROUND defined:
// sky_seen_above alone, the height above the ground given).

const float SKY_LAYER_HEIGHTS[4] = float[4](2.0, 6.0, 15.0, 35.0);

// The four layers at spot, in one read
vec4 sky_layer_values(vec2 spot)
{
	return texture(sky_layers_texture, clamp(spot / (ground_map.x * ground_map.y), vec2(0.0), vec2(1.0)));
}

// How much a canopy hides the sky over the ground at spot (0 open .. 1 under
// dense crowns): the ground's value against the highest layer's (what the
// land round hides is in both, what is over it only in the ground's)
float canopy_cover(vec2 spot)
{
	if (sky_occlusion.y < 0.5 || sky_occlusion.z < 1.0)
	{
		return 0.0;
	}
	float ground = texture(sky_occlusion_texture, (spot / ground_map.y + 0.5) / ground_map.x).r;
	float top = sky_layer_values(spot).a;

	return 1.0 - clamp(ground / max(top, 0.05), 0.0, 1.0);
}

// The sky seen at spot (x, z: meters) above meters over the ground
float sky_seen_above(vec2 spot, float above)
{
	if (sky_occlusion.y < 0.5 || sky_occlusion.z < 1.0)
	{
		return 1.0;
	}
	vec2 cells = spot / ground_map.y;
	vec4 values = sky_layer_values(spot);
	float below_value = texture(sky_occlusion_texture, (cells + 0.5) / ground_map.x).r;
	float below_height = 0.0;
	float seen = below_value;

	for (int layer = 0; layer < 4; layer++)
	{
		float value = values[layer];

		if (above <= SKY_LAYER_HEIGHTS[layer])
		{
			seen = mix(below_value, value, clamp((above - below_height) / (SKY_LAYER_HEIGHTS[layer] - below_height), 0.0, 1.0));
			break;
		}
		below_value = value;
		below_height = SKY_LAYER_HEIGHTS[layer];
		seen = value;
	}
	return sky_seen(seen);
}

// How hidden a spot is from the sky (0 open .. 1 all), raw (not by the
// strength): more moss there
float sky_hidden_above(vec2 spot, float above)
{
	float strength = sky_occlusion.x;
	float seen = sky_seen_above(spot, above);

	// (sky_seen_above is by the strength: undone)
	return strength > 0.01 ? clamp((1.0 - seen) / strength, 0.0, 1.0) : 0.0;
}

#ifndef SKY_OCCLUSION_NO_GROUND
float object_sky_seen(vec3 position)
{
	if (sky_occlusion.y < 0.5 || ground_map.z < 0.5 || sky_occlusion.z < 1.0)
	{
		return 1.0;
	}
	ivec2 cell = clamp(ivec2(floor(position.xz / ground_map.y + 0.5)), ivec2(0), ivec2(int(ground_map.x) - 1));

	return sky_seen_above(position.xz, position.y - texelFetch(ground_height_texture, cell, 0).x);
}
#endif

#ifdef SKY_BOUNCE
// The light the land round sends back (one bounce of the sun's): where the
// sky is hidden, what hides it lights the point instead, as far as the sun
// is on it (the far shadow map there), facing the sun the way it faces
// back. Wants sky_bounce_texture (RGBA8: the way to what hides the sky, on
// the whole, and how far: sky_occlusion_bounce_finish.comp), the far shadow
// map (shadows.glsl's shadow_texture). hidden: how much of the sky the
// point does not see (0..1, raw)
const float SKY_BOUNCE_FARTHEST = 400.0;             // sky_occlusion.c's SKY_OCCLUSION_BOUNCE_FARTHEST
const vec3 SKY_BOUNCE_ALBEDO = vec3(0.34, 0.30, 0.24); // what the land round reflects, on the whole (linear)

vec3 bounce_light(vec3 position, vec3 normal, float hidden)
{
	if (sky_bounce.y < 0.5 || sky_bounce.x <= 0.0 || hidden <= 0.0 || sun_direction.y < -0.05)
	{
		return vec3(0.0);
	}
	vec4 texel = texture(sky_bounce_texture, clamp(position.xz / (ground_map.x * ground_map.y), vec2(0.0), vec2(1.0)));

	if (texel.a < 0.5 / 255.0)
	{
		return vec3(0.0);
	}
	vec3 way = normalize(texel.rgb * 2.0 - 1.0);
	float distance = texel.a * texel.a * SKY_BOUNCE_FARTHEST;
	// (a little this side of it: not in its own shadow)
	vec3 there = position + way * max(distance - 1.5, 0.0);
	float sunlit = 1.0;

	if (shadow_strength > 0.0 && shadow_texel_size > 0.0)
	{
		vec4 clip = light_view_projection * vec4(there, 1.0);
		vec3 lookup = clip.xyz / clip.w * 0.5 + 0.5;

		lookup.y = 1.0 - lookup.y;
		if (lookup.x >= 0.0 && lookup.x <= 1.0 && lookup.y >= 0.0 && lookup.y <= 1.0 && lookup.z <= 1.0)
		{
			sunlit = mix(1.0, textureLod(shadow_texture, lookup, 0.0), shadow_strength);
		}
	}
	// (what hides the sky faces back this way, a little up: a slope's; what
	// hides it from overhead, a crown, shows its underside, out of the sun)
	vec3 its_face = normalize(-way + vec3(0.0, 0.3, 0.0));
	float its_sun = max(dot(its_face, sun_direction), 0.0) * smoothstep(-0.05, 0.08, sun_direction.y);
	float taken = 0.5 + 0.5 * max(dot(normal, way), 0.0);

	return sun_color * SKY_BOUNCE_ALBEDO * (its_sun * sunlit * hidden * taken * sky_bounce.x);
}
#endif

#if defined(SKY_BOUNCE) && !defined(SKY_OCCLUSION_NO_GROUND)
// bounce_light for a point of a thing on the ground whose object_sky_seen
// is seen (worked out once: not the layers walked again)
vec3 object_bounce(vec3 position, vec3 normal, float seen)
{
	if (sky_bounce.y < 0.5 || ground_map.z < 0.5)
	{
		return vec3(0.0);
	}
	return bounce_light(position, normal, sky_occlusion.x > 0.01 ? clamp((1.0 - seen) / sky_occlusion.x, 0.0, 1.0) : 0.0);
}
#endif

#ifdef SKY_BOUNCE
// Both at once for position, above meters over the ground (one walk through
// the layers): rgb the light sent back onto normal, a the sky seen (by the
// strength: sky_seen)
vec4 sky_light_at(vec3 position, float above, vec3 normal)
{
	if (sky_occlusion.y < 0.5)
	{
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	float seen = sky_seen_above(position.xz, above);
	float hidden = sky_occlusion.x > 0.01 ? clamp((1.0 - seen) / sky_occlusion.x, 0.0, 1.0) : 0.0;

	return vec4(bounce_light(position, normal, hidden), seen);
}
#endif
