// The sketch window's picture (terrain_sketch.h): the sheet as painted (its
// height alone, the unpainted low; lakes where the water is painted) or the
// land as it will come out, worked out small (SKETCH_VIEW_SIZE a side) from
// the seed and the sketch; shaded from the north west, colored by height,
// lakes blue, and what is painted tinted over it if asked. The seed's land is kept and
// worked out again only when the seed or the land's settings change; the
// sketch over it whenever it changes (a little while after, while it is
// being painted, every tenth of a second); the rain's erosion only once it
// has been left alone a moment (it takes longer).

#ifndef SKETCH_VIEW_H
#define SKETCH_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "settings.h"
#include "terrain_sketch.h"

#define SKETCH_VIEW_SIZE 512
#define SKETCH_VIEW_QUIET_SECONDS 0.1   // the sketch changing: worked out again at most this often
#define SKETCH_VIEW_EROSION_SECONDS 0.8 // ...and with the erosion once left alone this long

typedef struct
{
	float *land;                   // the seed's land (meters), as terrain_gen.c makes it at this size
	float *heights;                // with the sketch (and the erosion) over it
	float *paper;                  // the sheet as painted (its height alone, meters)
	uint8_t *pixels;               // RGBA
	SDL_GPUTexture *texture;
	// What the land and the picture were made for
	uint32_t land_seed;
	terrain_settings_t land_settings;
	float land_map_meters;
	bool land_valid;
	uint32_t heights_revision;     // the sketch's
	bool heights_eroded;
	bool heights_valid;
	bool tint_painted;             // what is painted shown over it
	bool show_result;              // the land as it will come out (else the sheet as painted)
	bool shown_result;             // ...as the picture was last made
	uint32_t paper_revision;
	bool painted;                  // anything painted (else the map is the seed's)
	uint32_t painted_revision;
	bool paper_valid;
	bool picture_dirty;
	uint64_t changed_ticks;        // when the sketch was seen to change (ms)
	uint64_t computed_ticks;       // when the sketch was last laid over the land
	uint32_t seen_revision;
} sketch_view_t;

// The brushes' presets (the sketch window's buttons, the console's "sketch
// paint"): what each paints
typedef struct
{
	const char *name;
	const char *tooltip;
	terrain_sketch_brush_t brush;
} sketch_preset_t;

#define SKETCH_PRESET_COUNT 5
extern const sketch_preset_t sketch_presets[SKETCH_PRESET_COUNT];

// A preset by its name (any case; "erase" too: SKETCH_PRESET_COUNT), -1 if none
int sketch_preset_find(const char *name);

bool sketch_view_create(sketch_view_t *view);
void sketch_view_destroy(sketch_view_t *view);

// Once a frame while the window is open (and for a picture asked for):
// works out what changed, uploads the picture. now_ticks: SDL_GetTicks()
void sketch_view_update(sketch_view_t *view, terrain_sketch_t *sketch, const terrain_settings_t *settings, uint32_t seed, int32_t map_size,
	float cell_size_meters, uint64_t now_ticks);

// The picture as a PNG (the console's "sketch picture", tests)
bool sketch_view_save_picture(const sketch_view_t *view, const char *path);

#endif
