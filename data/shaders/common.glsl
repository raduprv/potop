// Included first by every shader (after #version). SDL_GPU wants each
// stage's resources in fixed descriptor sets (see gpu.h): these macros give
// the right ones for the stage being compiled (the Makefile defines
// VERTEX_SHADER, FRAGMENT_SHADER or COMPUTE_SHADER).
//
// Uniform blocks: binding 0 is the scene (scene.glsl), binding 1 what one
// renderer pushes for its draws. Samplers: numbered from 0 in each stage,
// in the order the C side binds them.

#ifdef VERTEX_SHADER
#define TEXTURE_SET 0
#define UNIFORM_SET 1
#endif
#ifdef FRAGMENT_SHADER
#define TEXTURE_SET 2
#define UNIFORM_SET 3
#endif
#ifdef COMPUTE_SHADER
#define TEXTURE_SET 0
#define READONLY_SET 0
#define READWRITE_SET 1
#define UNIFORM_SET 2
#endif

// The clouds' layer is a shell around a small planet whose top touches the
// ground under the eye (clouds_volume.glsl, sky.glsl): towards the horizon
// the clouds come down to it. Its radius, in meters: a layer h meters up
// reaches the horizon sqrt(2 * radius * h) away (at 200 km, about 20 km for
// a base at 1000 m; the Earth's would put it at 110 km, too far to march,
// and leave a clear band above the horizon)
#define CLOUD_PLANET_RADIUS 200000.0

// How much of a cut out leaf picture's texel is thin green tissue (lets the
// sun through), not wood (a twig picture's stem, its bark: opaque): its
// green over its red and blue, of its green (linear color). The conifers'
// twigs lit through their brown shoots glowed rust red against the sun
float leaf_tissue(vec3 linear_color)
{
	float excess = linear_color.g - max(linear_color.r, linear_color.b);

	return smoothstep(0.02, 0.15, excess / max(linear_color.g, 1e-3));
}
