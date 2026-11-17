// Sun shadows, shared by the terrain, object and water fragment shaders
// (pulled in with #include "shadows.glsl").
//
// Two shadow maps: the far one covers the whole map at a fixed, coarse
// resolution; the near one covers a square around the point the camera
// looks at, finely enough for branches. Inside the near square the near
// map is used, fading into the far one towards its edges.

// The strength and texel sizes come with the scene (scene.glsl); the two
// maps are the first two samplers of every shader that includes this
layout(set = TEXTURE_SET, binding = 0) uniform sampler2DShadow shadow_texture;       // far cascade
layout(set = TEXTURE_SET, binding = 1) uniform sampler2DShadow near_shadow_texture;  // near cascade

// Soft edges: what nine hardware filtered taps (each already a 2x2
// comparison) a texel apart would give, from four (Castano's way). Along
// each axis the three taps weigh the four texels under them 1 - f, 1, 1, f
// (f: where the point is between texel centers); two taps weigh them the
// same, each between the two texels of its pair where the filter's own
// blend gives their ratio, weighted 2 - f and 1 + f. position is in the
// map's 0..1 texture space; texel_size its texel (square)
float shadow_lookup(sampler2DShadow map, vec3 position, float texel_size)
{
	vec2 texels = position.xy / texel_size - 0.5;
	vec2 corner = floor(texels);
	vec2 fraction = texels - corner;
	vec2 weight_low = 2.0 - fraction;
	vec2 weight_high = 1.0 + fraction;
	vec2 at_low = (corner - 0.5 + 1.0 / weight_low) * texel_size;
	vec2 at_high = (corner + 1.5 + fraction / weight_high) * texel_size;
	float lit = weight_low.x * weight_low.y * textureLod(map, vec3(at_low.x, at_low.y, position.z), 0.0)
		+ weight_high.x * weight_low.y * textureLod(map, vec3(at_high.x, at_low.y, position.z), 0.0)
		+ weight_low.x * weight_high.y * textureLod(map, vec3(at_low.x, at_high.y, position.z), 0.0)
		+ weight_high.x * weight_high.y * textureLod(map, vec3(at_high.x, at_high.y, position.z), 0.0);

	return lit / 9.0;
}

// Fraction of sunlight reaching this point: 1 = fully lit. The arguments
// are the point in each map's clip space. single: one comparison a map
// (the sampler's own 2 x 2), not shadow_lookup's four (a blade of grass:
// its sun is the blade's, an average anyway)
float sun_visibility_taps(vec4 far_clip, vec4 near_clip, bool single)
{
	if (shadow_strength <= 0.0)
	{
		return 1.0;
	}
	vec3 far_position = far_clip.xyz / far_clip.w * 0.5 + 0.5;
	vec3 near_position = near_clip.xyz / near_clip.w * 0.5 + 0.5;
	// A texture's first row is its top: the maps' v runs downwards
	far_position.y = 1.0 - far_position.y;
	near_position.y = 1.0 - near_position.y;
	float near_weight = 0.0;
	float far_lit = 1.0;
	float near_lit = 1.0;

	if (near_shadow_texel_size > 0.0 && near_position.z <= 1.0)
	{
		float edge = max(abs(near_position.x - 0.5), abs(near_position.y - 0.5)) * 2.0;

		near_weight = 1.0 - smoothstep(0.8, 0.95, edge);
	}
	// shadow_texel_size 0: the far map is off (the World window, debugging)
	if (shadow_texel_size > 0.0 && near_weight < 1.0 && far_position.x >= 0.0 && far_position.x <= 1.0 && far_position.y >= 0.0 && far_position.y <= 1.0
		&& far_position.z <= 1.0)
	{
		far_lit = single ? textureLod(shadow_texture, far_position, 0.0) : shadow_lookup(shadow_texture, far_position, shadow_texel_size);
	}
	if (near_weight > 0.0)
	{
		near_lit = single ? textureLod(near_shadow_texture, near_position, 0.0)
			: shadow_lookup(near_shadow_texture, near_position, near_shadow_texel_size);
	}
	return mix(1.0, mix(far_lit, near_lit, near_weight), shadow_strength);
}

float sun_visibility(vec4 far_clip, vec4 near_clip)
{
	return sun_visibility_taps(far_clip, near_clip, false);
}

// The far map's alone (a plant's leaves: plant.frag; no jump where the near
// map gives way, the crown's own fine shadow not on it)
float sun_visibility_far(vec4 far_clip)
{
	if (shadow_strength <= 0.0 || shadow_texel_size <= 0.0)
	{
		return 1.0;
	}
	vec3 far_position = far_clip.xyz / far_clip.w * 0.5 + 0.5;

	far_position.y = 1.0 - far_position.y;
	if (far_position.x < 0.0 || far_position.x > 1.0 || far_position.y < 0.0 || far_position.y > 1.0 || far_position.z > 1.0)
	{
		return 1.0;
	}
	return mix(1.0, shadow_lookup(shadow_texture, far_position, shadow_texel_size), shadow_strength);
}
