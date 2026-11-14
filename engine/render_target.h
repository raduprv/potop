// Offscreen target for the 3D scene: its color and depth, at the window's
// size times the render scale.
//
// With render_scale below 1 the scene is drawn smaller, then stretched over
// the frame's final picture by its last pass (post.h). Pixel shading is most of
// the terrain's cost on a big screen, so 0.75 saves about 40% of it, at
// the price of a softer image.
//
// Multisampled (gpu.scene_samples past 1, render.msaa): the land, the
// things on it and the sky are drawn into multisampled targets of their
// own, a color and a depth with each sample's depth beside the color (a
// multisampled depth can neither be sampled nor resolved). The first pause
// resolves them: the color into color_texture, the depth values (averaged
// at the edges) into a texture a pass writes back into depth_texture. What
// comes after (the depth pyramid, the water, the clouds, the fog, the
// particles, the rain) is drawn and reads as without it.

#ifndef RENDER_TARGET_H
#define RENDER_TARGET_H

#include <stdbool.h>

#include "gpu.h"

typedef struct
{
	// The world's scene (set before the first begin): multisampled as
	// render.msaa asks; else (the tools' previews) never
	bool world_scene;
	SDL_GPUTexture *color_texture;
	SDL_GPUTexture *depth_texture;
	int width;
	int height;
	// Multisampled: the targets of its first part, and the depth values
	// resolved (made with the size; none without multisampling)
	SDL_GPUTexture *multisampled_color_texture;
	SDL_GPUTexture *multisampled_depth_value_texture;
	SDL_GPUTexture *multisampled_depth_texture;
	SDL_GPUTexture *depth_value_texture;
	uint32_t samples;
	bool multisampled_open;        // the pass open is the multisampled one
	SDL_GPUGraphicsPipeline *depth_copy_pipeline;
	bool depth_copy_tried;
} render_target_t;

// Sizes the target for this frame's window and scale, and opens the
// scene's pass on it, cleared to clear_color (linear light) and
// the far depth. Returns NULL if there is nothing to draw into.
SDL_GPURenderPass *render_target_begin(render_target_t *target, int window_width, int window_height, float render_scale,
	const float clear_color[4], int *scene_width_out, int *scene_height_out);

// Ends the pass for something between draws that cannot happen inside one
// (the depth pyramid), and opens it again where it stopped: the color and
// the depth drawn so far kept, the viewport set again
void render_target_pause(render_target_t *target, SDL_GPURenderPass *pass);
SDL_GPURenderPass *render_target_resume(render_target_t *target);

// Ends the pass (post_draw then makes the picture of it)
void render_target_end(render_target_t *target, SDL_GPURenderPass *pass);

void render_target_destroy(render_target_t *target);

#endif
