// Ambient occlusion from the scene's depth (GTAO: ground truth ambient
// occlusion, Jimenez et al. 2016): the small scale the sky occlusion's half
// meter texels miss (sky_occlusion.h): a house's foot on the ground, the
// corner under the eaves, a barrel in a yard. After the scene's opaque
// things and the sky, before the water and the contact shadows:
//
//   1. at half the scene's size, each pixel looks along a few directions
//      on the screen, both ways, for the highest horizon the depth shows
//      within render.ambient_occlusion_meters, and turns the horizons
//      into how much of the sky it sees (cosine weighted, as the diffuse
//      light takes it): ambient_occlusion.frag, the occlusion and the
//      pixel's distance from the eye into a two channel target
//   1b. at half size, the occlusion averaged with what it was over the
//      last frames (its own history, found where the point was on the
//      screen last frame; dropped where the distance from the eye there
//      is not this point's: an edge come out, something moved):
//      ambient_occlusion_temporal.frag. The grass in the wind and thin
//      things flickered without it. The four by four pattern is turned
//      each frame, so the history gathers more than one
//   2. at full size, the half size occlusion gathered over four by four of
//      its pixels (its offsets' four by four pattern averaged away),
//      by how near each one's distance is to the pixel's (no blur across
//      edges); then only the sky's share of the pixel's light (against the
//      sun's, as the shadow maps leave it) taken away, the scene's color
//      multiplied down: ambient_occlusion_apply.frag
//
// Faded out with the distance (render.ambient_occlusion_distance): far
// off a pixel spans more than the radius. render.ambient_occlusion: off,
// nothing drawn.


#ifndef AMBIENT_OCCLUSION_H
#define AMBIENT_OCCLUSION_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "scene_uniforms.h"
#include "settings.h"

typedef struct
{
	SDL_GPUGraphicsPipeline *occlusion_pipeline;   // step 1, into occlusion_texture
	SDL_GPUGraphicsPipeline *temporal_pipeline;    // step 1b, into history_textures[history_written]
	SDL_GPUGraphicsPipeline *apply_pipeline;       // step 2, over the scene's color
	SDL_GPUTexture *occlusion_texture;             // half size, R16G16 float: the occlusion (1 open), the distance from the eye (m; below 0: the sky)
	SDL_GPUTexture *history_textures[2];           // the same, averaged over the frames: written in turn, the other read
	int history_written;                           // the one written last
	bool history_valid;                            // the other one holds last frame's
	float previous_view_projection[16];            // last frame's scene view_projection_relative and eye
	float previous_eye[3];
	uint64_t previous_ticks;                       // when it was drawn last (a pause: the history is stale)
	uint32_t frame;                                // turns the pattern
	uint32_t width;                                // occlusion_texture's size
	uint32_t height;
	bool tried;                                    // made once (failed: never again)
} ambient_occlusion_t;

void ambient_occlusion_destroy(ambient_occlusion_t *occlusion);

// Over the scene's color (its depth read, not bound): passes of its own.
// The shadow maps may be NULL (no shadows: the sun taken to reach everywhere)
void ambient_occlusion_draw(ambient_occlusion_t *occlusion, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, uint32_t width,
	uint32_t height, const scene_uniforms_t *scene, const render_settings_t *settings, SDL_GPUTexture *far_shadow, SDL_GPUTexture *near_shadow);

#endif
