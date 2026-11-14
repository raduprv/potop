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

// Nine hardware filtered taps (each already a 2x2 comparison) give soft
// edges; spread is the tap spacing in texels. position is in the map's
// 0..1 texture space.
float shadow_lookup(sampler2DShadow map, vec3 position, float texel_size, float spread)
{
	float lit = 0.0;

	for (int offset_y = -1; offset_y <= 1; offset_y++)
	{
		for (int offset_x = -1; offset_x <= 1; offset_x++)
		{
			vec2 offset = vec2(float(offset_x), float(offset_y)) * texel_size * spread;

			lit += textureLod(map, vec3(position.xy + offset, position.z), 0.0);
		}
	}
	return lit / 9.0;
}

// Fraction of sunlight reaching this point: 1 = fully lit. The arguments
// are the point in each map's clip space.
float sun_visibility(vec4 far_clip, vec4 near_clip)
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
		far_lit = shadow_lookup(shadow_texture, far_position, shadow_texel_size, 1.5);
	}
	if (near_weight > 0.0)
	{
		// Tighter: the near map is fine enough for twigs, a wide blur
		// would wash them out
		near_lit = shadow_lookup(near_shadow_texture, near_position, near_shadow_texel_size, 0.75);
	}
	return mix(1.0, mix(far_lit, near_lit, near_weight), shadow_strength);
}
