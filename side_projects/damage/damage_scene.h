// The scenes the damage tools show: each one a small world of its own,
// stepped at a fixed rate and drawn by the soft renderer. A scene also
// sets off the events of damage_events.h (balls thrown, blasts) as it
// comes to their frames, if it has anything for them to hit. The viewer and
// the headless tool go through this alone, so both show any scene, and a
// frame's number means the same thing in both.

#ifndef DAMAGE_SCENE_H
#define DAMAGE_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#include "soft_render.h"

// Every scene runs at this many frames a second, whatever the screen does
#define DAMAGE_SCENE_FRAMES_PER_SECOND 60

typedef struct
{
	const char *name;
	const char *about;
	// Makes the scene at its frame 0. False when it cannot be made.
	bool (*open)(void);
	void (*close)(void);
	void (*default_camera)(soft_render_camera_t *camera);
	// Brings the scene to a frame: forward by stepping it, back by
	// starting over and stepping again (the same steps give the same
	// world, so a frame can always be gone back to)
	void (*seek)(int frame_index);
	// The frame the scene is at: soft_render_begin, the solids, the
	// outlines if asked for, then the marks (the debug ones if asked for)
	void (*draw)(soft_render_t *renderer, const soft_render_camera_t *camera, bool draw_outlines, bool draw_debug);
	// A line about the frame the scene is at, for the screen (may be empty)
	void (*describe)(char *text, int text_size);
	// A number made of where everything is: the same on two machines when
	// they hold the same world
	uint32_t (*checksum)(void);
	// The first thing along a ray (a unit direction) within a distance:
	// where a blast aimed from the screen goes. False when there is
	// nothing; NULL in a scene with nothing to hit.
	bool (*cast_ray)(const float origin[3], const float direction[3], float distance, float hit_point[3]);
} damage_scene_t;

int damage_scene_count(void);
const damage_scene_t *damage_scene_get(int scene_index);
// NULL when there is no scene of that name
const damage_scene_t *damage_scene_find(const char *name);
// The scene shown when none is asked for: the one being worked on
const damage_scene_t *damage_scene_default(void);

#endif
