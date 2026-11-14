// Volumetric clouds, drawn over the finished scene when
// render.volumetric_clouds is on; otherwise the sky keeps its
// flat cloud sheet (cheap, for weak devices). Either way the clouds stand
// where the flat cloud field says (the one whose shadows cross the land),
// and the water reflects the flat sheet.
//
// The 3D noise the shape is made of is computed on the CPU the first time
// it is needed (about half a second, on all cores): a 128^3 texture of the
// broad billows (Perlin-Worley in red, Worley octaves in green, blue,
// alpha) and a 32^3 one of the detail that eats their edges.
//
// The clouds start well above the highest mountain (settings.h,
// CLOUD_MINIMUM_HEIGHT_METERS), so nothing on the map is ever inside them
// and they can be marched once for every direction around the eye, into a
// dome (clouds_dome.glsl), instead of once for every pixel:
//   dome       a sixteenth of its texels is marched again each frame and
//              blended with what they held: turning the camera costs
//              nothing, and the jittered marches average into smooth
//              clouds. While the eye moves enough for the clouds to shift,
//              all of it, coarsely (a march for 2x2 texels); after a jump,
//              all of it finely
//   composite  clouds_composite.frag looks each pixel's direction up in the
//              dome, unless the land is nearer than the clouds
// The layer is curved (CLOUD_PLANET_RADIUS, common.glsl), so the clouds
// reach down to the horizon and fade into its haze.
//
// Above it, the flocks: a thin layer of small white clouds (altocumulus,
// cirrocumulus) made of the shape noise's Worley cells alone, only where
// the sky is painted for them, marched into the same dome after the big
// clouds and seen through them.

#ifndef CLOUDS_H
#define CLOUDS_H

#include <stdbool.h>

#include "cloud_weather.h"
#include "gpu.h"
#include "scene_uniforms.h"
#include "settings.h"

typedef struct
{
	cloud_weather_t weather;             // where the clouds are and their shapes
	SDL_GPUTexture *shape_texture;
	SDL_GPUTexture *detail_texture;
	SDL_GPUComputePipeline *dome_pipeline;
	SDL_GPUComputePipeline *shadow_pipeline;
	SDL_GPUTexture *shadow_texture;      // the clouds' shadows on the land (see clouds_shadow_texture)
	uint64_t shadow_ticks;               // when it was last made (ns)
	bool shadow_ready;
	SDL_GPUGraphicsPipeline *composite_pipeline;
	SDL_GPUTexture *dome_texture;
	SDL_GPUTextureUsageFlags dome_usage;
	uint32_t dome_size;                  // texels a side
	bool dome_half;                      // it holds the upper half of the sky only (the eye is under the clouds)
	bool dome_blends;                    // the format can be read and written in one shader
	bool dome_valid;                     // every texel has been marched since the last forget
	float dome_eye[3];                   // the eye last frame
	bool failed;
	uint32_t frame;
	// The still dome's refresh, on the clock (not by frames: the same at
	// 30 and at 400 frames a second)
	uint64_t refresh_ticks;              // when the last frame was drawn (ns)
	float passes_due;                    // sixteenths of the dome owed by the clock
	uint32_t pass_count;                 // sixteenths marched so far (which texel of each block next)
	// The flocks (settings.h, flock_*): where the sky window painted them,
	// as a texture over the cloud map, and how far they have grown or
	// dissolved (added to the Amount setting, on the sky's clock)
	SDL_GPUTexture *flock_texture;
	uint32_t flock_revision;             // the paint's revision in it
	bool flock_uploaded;
	bool flock_painted;                  // some of the map has them
	float flock_growth;
	float flock_amount_seen;             // the Amount the growth started from (a new one starts it over)
	float flock_clock;                   // the sky's clock last frame (seconds)
} clouds_t;

void clouds_destroy(clouds_t *clouds);

// Draws the clouds over the scene's color target (outside any pass: opens
// and returns its own, which the caller ends). scene: the uniforms pushed
// for the scene in this command buffer. paint: the painted sky (may be NULL).
SDL_GPURenderPass *clouds_draw(clouds_t *clouds, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, uint32_t width,
	uint32_t height, SDL_GPUTexture *variation_texture, const scene_uniforms_t *scene, const render_settings_t *settings,
	const cloud_paint_t *paint);

// The dome is to be marched anew (the clouds were not drawn for a while)
void clouds_forget(clouds_t *clouds);

// The volumetric clouds' shadow map, for every shader that shades with the
// clouds (cloud_shade in clouds.glsl): over the cloud map's 25 km, how much
// of the sun the clouds above each patch take away (r). Made by
// clouds_draw now and then (a frame old when the land reads it); NULL until
// it is there or with the volumetric clouds off: then the flat sheet's
// shadows stay (the shaders are told by scene.cloud_shadow_ready)
SDL_GPUTexture *clouds_shadow_texture(void);

#endif
