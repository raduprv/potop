// Contact shadows: the small shadows the shadow maps are too coarse for
// (their texels a few centimeters, their depth bias as much again): the
// rubble on a yard, a stone's foot on the ground, the gap under a door.
// After the scene's opaque things and the sky, before the water: each
// pixel near the eye marches a short way toward the sun through the
// scene's depth; where something stands in the way, the sun's share of
// its light (what the shadow maps leave lit of it, by its facing, against
// the sky's) is taken away, the color multiplied down (render.contact_shadows,
// render.contact_shadow_meters).

#ifndef CONTACT_SHADOWS_H
#define CONTACT_SHADOWS_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "scene_uniforms.h"
#include "settings.h"

typedef struct
{
	SDL_GPUGraphicsPipeline *pipeline;
	bool tried;                    // made once (failed: never again)
	uint32_t frame;                // the march's offsets turn with it (the temporal anti-aliasing smooths them)
} contact_shadows_t;

void contact_shadows_destroy(contact_shadows_t *contact);

// Over the scene's color (its depth read, not bound): its own pass
void contact_shadows_draw(contact_shadows_t *contact, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, uint32_t width, uint32_t height,
	const scene_uniforms_t *scene, const render_settings_t *settings, SDL_GPUTexture *far_shadow, SDL_GPUTexture *near_shadow);

#endif
