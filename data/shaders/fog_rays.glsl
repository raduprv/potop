// The sun's rays' gathering passes (fog_rays.comp, fog_rays_wide.comp:
// after common.glsl and scene.glsl)

// The sun's rays through the fog (see src/fog.h), their second and third
// passes: each place gathers the mask (fog_rays_mask.comp) along the line
// towards the sun on the screen, the nearer the more: where trees stand in
// the way, the shafts between them; the fog's passes then add it as much
// as there is fog between the eye and what it sees. Soft and stable, from
// the very pixels on the screen (the froxels are too coarse for them).
//
// Sixty four steps in two passes of eight: this one (RAYS_WIDE not
// defined) the first eighth of the way, a step apart; the wide one, what
// the first left at eight places an eighth of the way apart. The steps'
// weights fall off one by one (RAY_DECAY), the product of the two passes'
// (as one pass of 64 would weigh them, a sixth of the reads of 48)

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D mask_texture;
layout(set = READWRITE_SET, binding = 0, r16f) uniform writeonly image2D rays_image;
#include "fog.glsl"

layout(local_size_x = 64) in;

const int RAY_TAPS = 8;
const float RAY_LONGEST = 0.7;         // of the screen's height: no farther towards the sun than this
const float RAY_DECAY = 0.9737;        // each of the 64 steps' weight, against the one before (0.965 over 48)

void main()
{
	ivec2 size = imageSize(rays_image);
	uint index = gl_GlobalInvocationID.x;

	if (index >= uint(size.x * size.y))
	{
		return;
	}
	ivec2 texel = ivec2(int(index) % size.x, int(index) / size.x);
	vec2 place = (vec2(texel) + 0.5) / vec2(size);
	float aspect = float(size.x) / float(size.y);
	vec2 towards = fog_sun.xy - place;
	float length_on_screen = length(towards * vec2(aspect, 1.0));
	vec2 whole_way = towards * min(1.0, RAY_LONGEST / max(length_on_screen, 1e-4));
#ifdef RAYS_WIDE
	vec2 step_size = whole_way / float(RAY_TAPS);
	float decay = pow(RAY_DECAY, float(RAY_TAPS));
	float dither = 0.0;
#else
	vec2 step_size = whole_way / float(RAY_TAPS * RAY_TAPS);
	float decay = RAY_DECAY;
	// (the steps' start dithered by the place alone: still from frame to
	// frame; the half size's blending smooths it)
	float dither = fract(52.9829189 * fract(dot(vec2(texel), vec2(0.06711056, 0.00583715))));
#endif
	float gathered = 0.0;
	float weights = 0.0;
	float weight = 1.0;

	for (int i = 0; i < RAY_TAPS; i++)
	{
		gathered += textureLod(mask_texture, place + step_size * (float(i) + dither), 0.0).r * weight;
		weights += weight;
		weight *= decay;
	}
	imageStore(rays_image, texel, vec4(gathered / weights, 0.0, 0.0, 0.0));
}
