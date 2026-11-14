// Decals: marks put on whatever is there (a bullet's hole in a wall, a
// scorch where something blew up, a splash of mud, a damp stain). Each is a
// box laid on a surface: what the box takes in of the ground, the
// buildings, the props and the actors gets the decal's picture over its
// own color, its roughness and its relief, before the light (so a hole is
// dark in the sun and in the shade alike, and a wet stain shines).
//
// The renderer is forward, so there is nothing to paint them on after the
// scene: they go the way the local lights go (lights.h). Each frame the
// decals in view are sorted into the lights' clusters on the CPU, in the
// lights' cluster buffer, and every lit shader that includes decals.glsl
// looks at the decals of its pixel's cluster. Their pictures are an atlas
// of DECALS_ATLAS_CELLS x DECALS_ATLAS_CELLS cells, kept in the same buffer
// (read with filtering done by hand: no sampler slot is left in the
// terrain's shader).
//
// The atlas: data/props/textures/decals_color.png (RGB the color, A how
// much it covers) and decals_normal.png (RG the relief as a normal map,
// B the roughness, A unused), DECALS_ATLAS_SIZE square, if they are there;
// else stand-ins drawn here. A kind of decal is a cell of it (or several,
// one picked at random): data/decals.ini names them.
//
// They stay where they were put in the world: a decal on something that
// moves away stays behind (for now only the ground and what stands get
// them: the projectiles leave none on loose bodies and actors).
#ifndef DECALS_H
#define DECALS_H

#include <stdbool.h>
#include <stdint.h>

#define DECALS_MAX 512                 // the oldest goes when a new one needs room
#define DECALS_KINDS_MAX 32
#define DECALS_ATLAS_SIZE 512          // texels a side (as decals.glsl has it)
#define DECALS_ATLAS_CELLS 4           // cells a side
#define DECALS_RECORD_WORDS 20         // a decal as the shaders read it (decals.glsl)
#define DECALS_FAR_METERS 80.0f        // none farther than this is drawn (they fade out before)

typedef struct
{
	char name[32];
	int first_cell;                    // the atlas' cells (left to right, top to bottom)
	int cell_count;                    // one of these, at random
	float size_meters;                 // across, unless the caller says
	float depth_meters;                // how far into and out of the surface it reaches
	float opacity;
	float relief;                      // how much its normal map counts (0..2)
	float roughness_weight;            // how much its roughness replaces the surface's (0..1)
	float life_seconds;                // 0: until pushed out by newer ones
	float fade_seconds;                // fading out at the end of its life
} decal_kind_t;

typedef struct
{
	bool used;
	int kind;
	int cell;
	float center[3];
	float axes[3][3];                  // along, across (on the surface), out of it (its normal)
	float half_sizes[3];
	double born_seconds;
} decal_t;

typedef struct
{
	decal_t list[DECALS_MAX];
	int next;                          // where the next one goes (round)
	int count;                         // in use
	decal_kind_t kinds[DECALS_KINDS_MAX];
	int kind_count;
	uint32_t *atlas;                   // DECALS_ATLAS_SIZE^2 color texels, then as many normal ones (RGBA8); lights.c sends it up
	uint32_t seed;
} decals_t;

// Reads data/decals.ini and the atlas (or draws the stand-ins)
bool decals_create(decals_t *decals);
void decals_destroy(decals_t *decals);

// The kind of that name (-1: none)
int decals_find_kind(const decals_t *decals, const char *name);

// A decal of a kind at a point of a surface facing normal (normalized),
// size meters across (0: the kind's), turned at random about the normal;
// now: the game's clock. False if the kind is not known
bool decals_add(decals_t *decals, int kind, const float point[3], const float normal[3], float size_meters, double now);

// All of them gone (a new map)
void decals_clear(decals_t *decals);

// The ones whose life is over put away (each frame, before lights_update)
void decals_update(decals_t *decals, double now);

// A decal as decals.glsl reads it, into record (DECALS_RECORD_WORDS words);
// fade: 0..1 by its age and how far it is
void decals_pack(const decals_t *decals, const decal_t *decal, float fade, uint32_t record[DECALS_RECORD_WORDS]);

// How faded one is by its age now (1: not at all)
float decals_age_fade(const decals_t *decals, const decal_t *decal, double now);

#endif
