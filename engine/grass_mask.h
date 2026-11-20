// The grass round the things that stand: a fine mask round the eye of where
// the blades thin away, so that none grow out of a rock, a trunk, a post or
// a wall, and they thin out towards them instead of stopping at a cell's
// edge (the material map's no grass bit is a meter wide).
//
// The mask is GRASS_MASK_SIDE texels a side, GRASS_MASK_TEXELS_PER_METER a
// meter (a tenth of a meter: about a hundred meters round the eye, as far
// as the grass goes at the default distance), kept on the CPU and given to
// the grass's scatter as a storage buffer, a byte a texel: 0 the blades
// grow, 255 none. It wraps round (toroidal): as the eye moves only the
// strips that come into it are made again.
//
// What clears the grass: each object whose type says so (object_types.h,
// grass_clear_meters; by default the solid ones: physics static or body),
// standing (not loose, moving, hidden or gone). Where it meets the ground
// is its model's solid triangles (not its leaves) cut by a slab round its
// origin (GRASS_MASK_SLICE_BELOW .. GRASS_MASK_SLICE_ABOVE), the convex
// hull of that, worked out once a model (a flat one, a patch or a decal,
// none); the outlines of its footprints that say no grass (object_format.h)
// besides, as they are (convex or not). Inside, none; out to its
// grass_clear_meters the blades thin away.
//
// Made again whole when the objects changed (added, taken away, moved,
// come loose), at most every GRASS_MASK_LOOK_SECONDS.

#ifndef GRASS_MASK_H
#define GRASS_MASK_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"
#include "objects.h"

#define GRASS_MASK_SIDE 1024                // texels a side (a power of two: the wrap is a mask)
#define GRASS_MASK_TEXELS_PER_METER 10
#define GRASS_MASK_RECENTER_TEXELS 64       // the eye this far from the middle: the window moves
#define GRASS_MASK_LOOK_SECONDS 0.25f       // the objects changed: made again at most this often
#define GRASS_MASK_SLICE_BELOW 0.03f        // where a model meets the ground: from this far below its origin
#define GRASS_MASK_SLICE_ABOVE 0.05f        // to this far above (meters at size 1): the ground line, not what bulges over it
#define GRASS_MASK_AUTO_METERS 0.0f         // grass_clear_meters auto, for a solid thing: the blades right up to it
#define GRASS_MASK_HULL_POINTS 48           // a model's outline, at most
#define GRASS_MASK_FLAT_METERS 0.15f        // a model no taller (a patch, a decal) clears no grass

// Where a model meets the ground (model x, z, meters at size 1)
typedef struct
{
	float points[GRASS_MASK_HULL_POINTS][2];
	int point_count;               // under 3: it does not meet the ground
	const float *source;           // its collision_positions when worked out (another: reloaded, again)
	bool known;
} grass_mask_hull_t;

typedef struct
{
	SDL_GPUBuffer *buffer;         // the texels, as the scatter reads them (uint words, four texels each)
	uint8_t *texels;               // the same on the CPU, wrapped round
	int32_t origin_x;              // the window's first texel (texel coordinates: meters times the texels per meter)
	int32_t origin_z;
	bool valid;                    // made at least once, where origin says
	bool rebuild_pending;          // the objects changed since it was made
	uint64_t rebuilt_nanoseconds;  // when it was made whole (SDL_GetTicksNS)
	uint32_t objects_generation;   // what the objects were when it was made
	uint32_t objects_changes;
	int objects_count;
	grass_mask_hull_t *hulls;      // per model (objects_t.models)
	int hull_count;
	bool failed;
} grass_mask_t;

bool grass_mask_create(grass_mask_t *mask);
void grass_mask_destroy(grass_mask_t *mask);

// Once a frame, before the grass is scattered: follows the eye, made again
// where needed, uploaded (and flushed) when it changed
void grass_mask_update(grass_mask_t *mask, const objects_t *objects, float eye_x, float eye_z);

// The scatter's say (grass_body.glsl, mask_window): the window's first
// texel x, z, the texels per meter, the side (0: no mask)
void grass_mask_window(const grass_mask_t *mask, float window_out[4]);

#endif
