// Drawing a wreck model (engine/wreck.h) with the soft renderer. The
// model draws nothing itself: this reads its pieces, links, groups and
// hits.

#ifndef DAMAGE_DRAW_H
#define DAMAGE_DRAW_H

#include <stdbool.h>
#include <stdint.h>

#include "soft_render.h"
#include "wreck.h"

// The pieces (each its own object, numbered from first_object_id) and the
// projectiles
void damage_draw_solids(const wreck_model_t *model, soft_render_t *renderer, uint32_t first_object_id);

// With draw_debug the links (green whole, yellow hurt, orange overloaded
// and giving way, red just broken) and the hits with their impulses,
// shown through the solids
void damage_draw_marks(const wreck_model_t *model, soft_render_t *renderer, int frame_index, bool draw_debug);

// One piece looked into: its links drawn through the solids as lines to
// what they hold it to (green lightly loaded, yellow over half its
// limit, orange overloaded), and written out from the given corner what
// it is and what each of its links bears
void damage_draw_inspection(const wreck_model_t *model, soft_render_t *renderer, int piece_index, int text_x, int text_y, int text_scale);

#endif
