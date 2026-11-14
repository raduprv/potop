// How much cloud covers a point of the cloud sheet (included by every
// shader that shades with the clouds or draws them, after its
// variation_texture and cloud_parameters, so the clouds in the sky and their
// shadows on the land are the same). on_sheet: meters on the sheet, wind
// already taken off. blur: a level of detail bias (reflections).
//
// Three layers of the same tiling noise texture, each turned by a
// different angle and scaled by a factor that does not divide the others:
// one layer alone repeats every cloud_tiling_meters, and towards the
// horizon its copies line up into a grid; three turned ones never line up.
// Shaders without derivatives (or in branches) define CLOUDS_EXPLICIT_LOD
// first and get level 0, or CLOUDS_LOD_VARIABLE as the name of a variable
// holding the level.

#if defined(CLOUDS_LOD_VARIABLE)
#define CLOUD_SAMPLE(position) textureLod(variation_texture, (position), CLOUDS_LOD_VARIABLE).r
#elif defined(CLOUDS_EXPLICIT_LOD)
#define CLOUD_SAMPLE(position) textureLod(variation_texture, (position), 0.0).r
#else
#define CLOUD_SAMPLE(position) texture(variation_texture, (position), blur).r
#endif

// The three layers mixed, before the coverage picks what is cloud (0..1)
float cloud_field(vec2 on_sheet, float blur)
{
	vec2 base = on_sheet * cloud_parameters.w;
	vec2 fine = mat2(0.80, 0.60, -0.60, 0.80) * base * 2.31 + vec2(0.37, 0.71);
	vec2 broad = mat2(0.28, -0.96, 0.96, 0.28) * base * 0.43 + vec2(0.13, 0.52);

	return CLOUD_SAMPLE(base) * 0.50 + CLOUD_SAMPLE(fine) * 0.25 + CLOUD_SAMPLE(broad) * 0.25;
}

// How much of the field is cloud (soft edges: cloud_parameters.y)
float cloud_cover_of_field(float field)
{
	return smoothstep(1.0 - cloud_parameters.x - cloud_parameters.y, 1.0 - cloud_parameters.x + cloud_parameters.y, field);
}

float cloud_cover(vec2 on_sheet, float blur)
{
	return cloud_cover_of_field(cloud_field(on_sheet, blur));
}

#ifdef CLOUD_SHADOW_TEXTURE
// How much of the sun the clouds over a point of the sheet take away: the
// volumetric clouds' own shadow map (clouds.c: their columns looked through,
// over the cloud map's 25 km, repeating), or the flat sheet without them
float cloud_shade(vec2 on_sheet)
{
	if (cloud_shadow_ready > 0.5)
	{
		return textureLod(cloud_shadow_texture, on_sheet * (1.0 / 25000.0), 0.0).r;
	}
	return cloud_cover(on_sheet, 0.0);
}
#endif
