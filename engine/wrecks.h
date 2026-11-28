// The breakable buildings on the map (wreck.h's model, one each): read from
// data/wrecks/NAME.wreck, placed in the physics' world, their pieces drawn
// as objects of procedural types (objects_procedural_type: a box or a log
// of its size and material, textured as data/wreck.ini's [looks] says).
//
// The physics calls round each step of its world: wrecks_before_step,
// then wrecks_take_hits with the step's hit events (each to the buildings
// whose pieces it touches), then wrecks_after_step. Once a frame
// wrecks_update_objects moves the pieces' objects where their bodies are,
// makes objects for pieces cut in two and takes away those of pieces that
// are gone.
//
// A .wreck file (ini_parser.h's form, in the building's own frame: meters,
// y up, its foot at y 0):
//
//   [pieces]
//   box = MATERIAL, X Y Z, QX QY QZ QW, HALF_X HALF_Y HALF_Z
//   log = MATERIAL, X Y Z, QX QY QZ QW, RADIUS HALF_LENGTH
//   [links]
//   link = PIECE_A, PIECE_B, TYPE, X Y Z
//
// Pieces are numbered in the order they come, from 0; PIECE_B -1 is the
// ground. MATERIAL: wood, brick, glass. TYPE: nails, mortar, foundation,
// glazing, notch. A log lies along its own X.

#ifndef WRECKS_H
#define WRECKS_H

#include <stdbool.h>
#include <stdint.h>

#include "objects.h"
#include "terrain.h"
#include "wreck.h"

#define WRECKS_MAX 16
#define WRECKS_NAME_LENGTH 64

typedef struct
{
	bool used;
	char name[WRECKS_NAME_LENGTH];
	wreck_model_t *model;              // on the heap (its bodies point at it: it never moves)
	uint32_t piece_objects[WRECK_PIECES_MAX];   // each piece's object's id (0: none yet, or gone)
	bool piece_gone[WRECK_PIECES_MAX];          // its object taken away for good
	float piece_sizes[WRECK_PIECES_MAX][3];     // its size when its object was made (a cut piece is shorter: its object made anew)
	// Half the length of the whole piece a piece was cut from (its own,
	// for a piece never cut): a cut piece is drawn at one of a few parts
	// of it (wrecks_t's cut_steps), so its pieces share a few models
	float piece_whole[WRECK_PIECES_MAX];
	int placed_piece_count;                     // the pieces the building came with (the rest were cut off them)
	// The moving pieces drawn between the last two steps (the physics goes
	// at a fixed rate): where each was and is (x, y, z, then a quaternion)
	float pose_from[WRECK_PIECES_MAX][7];
	float pose_to[WRECK_PIECES_MAX][7];
	bool piece_moving[WRECK_PIECES_MAX];
} wrecks_building_t;

typedef struct
{
	char albedo[128];                  // textures, relative to data/
	char normal[128];
	char orm[128];                     // empty: the neutral one
	float texture_meters;              // one repeat of the textures
} wrecks_look_t;

typedef struct wrecks_s
{
	wreck_settings_t settings;
	wrecks_look_t looks[WRECK_MATERIAL_COUNT];
	// [cuts]: a cut piece is drawn at the nearest of steps parts of its
	// whole piece (6: a sixth, a third, a half...), and not at all under
	// vanish_meters long (the physics keeps its exact size)
	int cut_steps;
	float vanish_meters;
	wrecks_building_t buildings[WRECKS_MAX];
	int frame_index;                   // the physics' steps
} wrecks_t;

// Reads data/wreck.ini (the model's numbers, as side_projects/damage's
// damage.ini, [looks] and [cuts])
void wrecks_initialize(wrecks_t *wrecks);

// A building of data/wrecks/NAME.wreck placed on the ground at (x, z),
// turned yaw (radians) round the vertical; its number, -1 if it cannot be
void wrecks_place(wrecks_t *wrecks, b3WorldId world, objects_t *objects, const terrain_t *terrain, const char *name, float x, float z,
	float yaw_radians);

// Every building taken away (its bodies and its objects)
void wrecks_clear(wrecks_t *wrecks, objects_t *objects);

int wrecks_count(const wrecks_t *wrecks);

// Round a step of the physics' world (see above)
void wrecks_before_step(wrecks_t *wrecks);
void wrecks_take_hits(wrecks_t *wrecks, b3WorldId world);
void wrecks_after_step(wrecks_t *wrecks, float step_seconds);

// After each step of the physics: the pieces' objects made, retyped and
// taken away, and where the moving ones are going
void wrecks_update_objects(wrecks_t *wrecks, objects_t *objects, const terrain_t *terrain);

// Once a frame: the moving pieces drawn between the last two steps
// (blend: how far from the one before to the last, 0..1)
void wrecks_interpolate(wrecks_t *wrecks, objects_t *objects, float blend);

// A blast of so many kilograms of explosive (world meters): on every
// building
void wrecks_blast(wrecks_t *wrecks, const float position[3], float kilograms);

// A thing thrown at the buildings (a launcher's grenade, a ball): their
// models mind its speed, so it goes on through what it breaks; and when
// it is gone
void wrecks_add_projectile(wrecks_t *wrecks, b3BodyId body, float radius);
void wrecks_remove_projectile(wrecks_t *wrecks, b3BodyId body);

#endif
