// The weapon in hand, drawn in first person (the "viewmodel"): in the
// camera's space with its own field of view, its depth squeezed into the
// nearest sliver so it never goes into a wall; brought up, put away, kicked
// back by a shot, dipped for a reload, bobbing as the walker walks. Lit by
// the sun and the sky (and its own muzzle flash). And the crosshair.
//
// The model: "procedural:rifle" or "procedural:scoped_rifle" for now (a
// rifle made of boxes and prisms, until there are real ones: weapons.ini's
// model). Aimed (the right button) it comes to the middle, its sights (or
// its scope) at the eye; through a scope the screen is the scope's view.

#ifndef VIEWMODEL_H
#define VIEWMODEL_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#include "camera.h"
#include "settings.h"
#include "weapons.h"

typedef struct
{
	SDL_GPUGraphicsPipeline *pipeline;
	SDL_GPUBuffer *rifle_buffer;
	uint32_t rifle_vertex_count;
	SDL_GPUBuffer *scoped_buffer;      // the rifle with a scope ("procedural:scoped_rifle")
	uint32_t scoped_vertex_count;
	SDL_GPUBuffer *scope_view_buffer;  // looking through the scope: black round it, the reticle
	uint32_t scope_view_vertex_count;
	SDL_GPUBuffer *crosshair_buffer;
	uint32_t crosshair_vertex_count;
	float bob_phase;
	bool created;
	// A light the player carries (a torch in the other hand), lighting the
	// weapon too: where (world), its light (linear, as strong as the night
	// lets it); 0: none (viewmodel_set_carried_light)
	float carried_position[3];
	float carried_light[3];
} viewmodel_t;

bool viewmodel_create(viewmodel_t *viewmodel);
void viewmodel_destroy(viewmodel_t *viewmodel);

// Where the muzzle is in the world now (for its flash), as the weapon is
// held
void viewmodel_muzzle_position(const viewmodel_t *viewmodel, const weapons_t *weapons, const camera_t *camera, float position_out[3]);

// The weapon (as far as it is up) and the crosshair, into the scene's pass,
// last. lit: the scene's light (render_settings_t as the day makes it);
// walking_speed: meters a second (the bob)
// The carried light for the next draws (light NULL: none): world position,
// linear color times its strength
void viewmodel_set_carried_light(viewmodel_t *viewmodel, const float position[3], const float light[3]);

void viewmodel_draw(viewmodel_t *viewmodel, SDL_GPURenderPass *pass, const weapons_t *weapons, const camera_t *camera,
	const render_settings_t *lit, float aspect_ratio, float walking_speed, float seconds);

#endif
