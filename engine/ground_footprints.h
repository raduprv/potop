// The ground under the buildings: where an object's model says it covers
// the ground (object_format.h, OBJECT_FILE_FOOTPRINT), the material map's
// ground byte is marked (material_map.h, MATERIAL_GROUND_*): no grass grows
// there, and where a floor covers a cell whole the terrain is not drawn
// (terrain_renderer.c leaves those quads out). Worked out at run time from
// the objects standing on the map, never saved; looked over a couple of
// times a second, done again only when what stands changed (an object
// added, taken away, moved) or the map's bytes were cleared (a restamp, a
// load). A footprint may lay a cover (a terrain material: a smithy's floor
// on the ground itself), put back on every look if something took it (a
// flood). Where a building stood and stands no more, the ground is left
// bare earth (the cover: dirt).

#ifndef GROUND_FOOTPRINTS_H
#define GROUND_FOOTPRINTS_H

#include <stdint.h>

#include "objects.h"
#include "terrain.h"

typedef struct
{
	uint32_t *cells;               // the cells marked now (indices into the terrain)
	uint8_t *covers;               // each one's cover laid by its footprint (OBJECT_FOOTPRINT_COVER; MATERIAL_NONE: none), kept there
	int cell_count;
	int cell_capacity;
	uint64_t signature;            // of what stands with a footprint, when they were marked
	float seconds_to_next_look;
	// The objects with a footprint (instance indices), found again when the
	// object list changes (objects_t.generation): the looks go through
	// these, not every object of a big map
	int *standing;
	int standing_count;
	int standing_capacity;
	uint32_t standing_generation;
	bool standing_known;
} ground_footprints_t;

void ground_footprints_initialize(ground_footprints_t *footprints);
void ground_footprints_destroy(ground_footprints_t *footprints);

// Once a frame (looks only every so often, at once when the map's bytes
// were cleared)
void ground_footprints_update(ground_footprints_t *footprints, terrain_t *terrain, const objects_t *objects, float frame_seconds);

#endif
