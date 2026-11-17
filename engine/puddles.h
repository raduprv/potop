#ifndef PUDDLES_H
#define PUDDLES_H

#include "camera.h"
#include "scene_uniforms.h"
#include "terrain_renderer.h"

#include <SDL3/SDL.h>
#include <stdbool.h>

// Puddles: after rain the ground holds water in its hollows (and the thin
// water the simulation leaves on it is one as it is). Where they are is
// worked out the same way in two places (puddles.glsl): the terrain's
// shader gives them their look (flat, darker, a mirror of the sky), and
// here, after the scene is drawn and copied for the water (water_renderer_
// capture), a pass over the screen gives them what they mirror: for each
// pixel on the ground (its depth's point at the ground's height) with a
// puddle, the water's screen space march (water_ssr.glsl) from the flat
// surface, blended over by Fresnel and how sure the march is.
// render.puddles: off, neither.

typedef struct
{
	bool tried;
	SDL_GPUGraphicsPipeline *pipeline;
} puddles_t;

void puddles_destroy(puddles_t *puddles);

// Over color_texture (the scene's, width x height), from the copy of it
// and its depth (water_renderer_capture's); the ground's textures the
// terrain renderer's, the water's depth (NULL: none)
void puddles_draw(puddles_t *puddles, SDL_GPUTexture *color_texture, int width, int height, SDL_GPUTexture *scene_color,
	SDL_GPUTexture *scene_depth, const scene_uniforms_t *scene, const camera_t *camera, const terrain_renderer_t *terrain_renderer,
	SDL_GPUTexture *water_depth_texture);

#endif
