// The frame's last passes: the scene's light (linear, in floats: render
// target) into the picture on the screen (gpu.final_texture), stretched
// over the window at the same time (render_scale).
//
// With render.hdr on:
//
//   bloom      the scene halved over and over (bloom_down.frag), then
//              summed back up (bloom_up.frag): the picture blurred at
//              every size at once, mixed in a little (bloom_strength).
//              Every light glows a bit; what is far brighter than white
//              (the sun on the water) glows a lot.
//   exposure   the mean brightness of the smallest bloom level, the way
//              the eye takes it (by the logarithm, the middle counting
//              more), brought to middle gray (exposure.comp), followed
//              slowly as the eye adapts (exposure_adapt_seconds), moved
//              by exposure_ev. Off (auto_exposure), the set one alone.
//   tone       the light into 0..1 without cutting it: AgX (bright colors
//              go towards white; plain or punchy), ACES or Reinhard
//              (tone_mapper).
//
// Off: the light cut at 1, as before. Either way: gamma, then the color
// grading (color_grade: a look, as a 32^3 table from the picture's colors
// to the graded ones, made here from the preset's numbers whenever it
// changes), and a speck of noise so gradients do not break into bands.

#ifndef POST_H
#define POST_H

#include <stdbool.h>

#include "gpu.h"
#include "settings.h"

#define POST_BLOOM_LEVELS 6

typedef struct
{
	SDL_GPUGraphicsPipeline *down_pipeline;
	SDL_GPUGraphicsPipeline *up_pipeline;
	SDL_GPUGraphicsPipeline *final_pipeline;
	SDL_GPUGraphicsPipeline *window_pipeline;   // the same into the window's picture straight (its format; NULL: the same as ours)
	SDL_GPUComputePipeline *exposure_pipeline;
	// The bloom's levels, half the scene and smaller: going down, and
	// summed back up
	SDL_GPUTexture *down[POST_BLOOM_LEVELS];
	SDL_GPUTexture *up[POST_BLOOM_LEVELS];
	uint32_t level_width[POST_BLOOM_LEVELS];
	uint32_t level_height[POST_BLOOM_LEVELS];
	int level_count;
	int scene_width;
	int scene_height;
	// The exposure, 1 x 1: this frame's is written from last frame's
	SDL_GPUTexture *exposure[2];
	// The color grading's table (32^3) and the preset it was made for (-1: none yet)
	SDL_GPUTexture *grade_texture;
	int grade_preset;
	int exposure_index;
	double last_seconds;
	// The temporal anti-aliasing (post_temporal): its pass, the two
	// histories (written in turn: last frame's read, this frame's written),
	// their size, whether the one read holds anything yet
	SDL_GPUGraphicsPipeline *temporal_pipeline;
	SDL_GPUTexture *history[2];
	int history_index;
	int history_width;
	int history_height;
	bool history_valid;
	bool failed;
} post_t;

bool post_create(post_t *post);
void post_destroy(post_t *post);

// The scene's color (its pass ended) into gpu.final_texture. now_seconds:
// the clock, for the adaptation; instant: the exposure is where it would
// settle at once (screenshots)
// The temporal anti-aliasing: the scene's color (its pass ended, drawn with
// the projection jittered) averaged with the history, found where each
// pixel was last frame by reprojection (this frame's OpenGL style clip
// space, unjittered, into last frame's: column major). still_depth: the
// depth (0..1) under which nothing moves (the weapon in hand); 0: none.
// Returns the picture to make the frame from (the scene's own texture if
// it cannot). post_temporal_reset forgets the history (it is off, the view
// jumped)
SDL_GPUTexture *post_temporal(post_t *post, SDL_GPUTexture *scene_texture, SDL_GPUTexture *depth_texture, int scene_width, int scene_height,
	const float reprojection[16], float history_weight, float still_depth, float sharpness);
void post_temporal_reset(post_t *post);

void post_draw(post_t *post, SDL_GPUTexture *scene_texture, int scene_width, int scene_height, const render_settings_t *settings,
	double now_seconds, bool instant);

#endif
