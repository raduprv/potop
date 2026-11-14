// The sky: one triangle over the whole screen at the far plane, colored per
// pixel by the direction it looks (sky.vert, sky.frag, sky.glsl).

#ifndef SKY_RENDERER_H
#define SKY_RENDERER_H

#include <stdbool.h>

#include "camera.h"
#include "gpu.h"
#include "settings.h"
#include "terrain.h"

typedef struct
{
	SDL_GPUGraphicsPipeline *pipeline; // the triangle comes from the vertex index, no buffers
	SDL_GPUTexture *moon_texture;      // the moon's map: color, height (sky.frag)
} sky_renderer_t;

bool sky_renderer_create(sky_renderer_t *renderer);
void sky_renderer_destroy(sky_renderer_t *renderer);

// Into the scene pass, after everything solid (the scene's uniforms are
// pushed already: the sky reads the camera, sun and clouds from them)
// dome_texture: the volumetric clouds' dome (they hide the stars; NULL:
// none, as the scene's fog_parameters.w says)
void sky_renderer_draw(sky_renderer_t *renderer, SDL_GPURenderPass *pass, SDL_GPUTexture *variation_texture, SDL_GPUTexture *dome_texture);

// Seen from high up the land ends below eye level: the sine of the angle
// down to where the haze swallows it (for the scene's uniforms)
float sky_horizon_drop(const camera_t *camera, const terrain_t *terrain, const render_settings_t *settings);

#endif
