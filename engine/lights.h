// Local lights: fires, torches, lanterns, candles. Many of them (hundreds),
// none casting shadows. Each frame the lights near the eye are gathered
// (from the particle effects that give light, the objects whose type does,
// see particles.ini and objects.ini, "light", and the ones placed with the
// Light tool), sorted into clusters (the screen cut in tiles, each tile cut
// in slices by the distance from the eye) on the CPU, and handed to the lit
// shaders in two storage buffers (data/shaders/lights.glsl): each pixel
// looks only at the lights of its cluster.
//
// By day the sun drowns them: they fade out as it rises (and cost nothing
// at all in full day), and in as it sets. Lamps (light_switched, the
// objects' default, and the placed ones) are lit at dusk and put out at
// dawn, each at its own moment, or all at once when the game says
// (lights_set_lamps, the console's "lights").
//
// The lit windows of the houses (their emissive groups named "window",
// objects_light_windows) each give a light too: warm, a little out in
// front of the glass, lighting only what is in front of the wall (one
// sided: without shadows it would light the house's insides and through
// the wall), so the grass and the walls round a window take its glow.
//
// A light can have a core: a small glow where it is (a candle's flame, a
// lantern's glass), drawn facing the camera and added to the picture; the
// bloom makes its halo. The models themselves need no glowing textures.

#ifndef LIGHTS_H
#define LIGHTS_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"
#include "decals.h"
#include "gpu.h"
#include "light_source.h"
#include "objects.h"
#include "particles.h"

#define LIGHTS_MAX 512                 // nearest first; the rest are not lit
#define LIGHTS_TILES_X 16
#define LIGHTS_TILES_Y 9
#define LIGHTS_SLICES 24
#define LIGHTS_CLUSTERS (LIGHTS_TILES_X * LIGHTS_TILES_Y * LIGHTS_SLICES)
#define LIGHTS_INDICES_MAX 65536       // light references over all clusters
#define LIGHTS_NEAR_METERS 1.0f        // the first slice ends here
#define LIGHTS_FAR_METERS 300.0f       // no light farther than this is gathered
#define LIGHTS_PLACED_MAX 256          // with the Light tool
#define LIGHTS_MOVING_MAX 128          // carried (a torch, a lantern) and flashes
// Fragment storage buffer slots (after each lit shader's samplers)
#define LIGHTS_STORAGE_BUFFERS 2

// The lamps: lit and put out with the day, or by the game
typedef enum
{
	LIGHTS_LAMPS_AUTOMATIC = 0,        // lit at dusk, out at dawn
	LIGHTS_LAMPS_ON,
	LIGHTS_LAMPS_OFF
} lights_lamps_t;

typedef struct
{
	light_source_t source;
	float position[3];
} placed_light_t;

// A light that moves (carried: its owner puts it where it is each frame)
// or a flash (gone after its seconds, fading)
typedef struct
{
	bool used;
	uint32_t key;                      // its owner's (0: a flash)
	light_source_t source;
	float position[3];
	double start_seconds;              // a flash: when it began, how long it lasts
	float life_seconds;
} moving_light_t;

typedef struct
{
	bool created;
	SDL_GPUBuffer *light_buffer;       // header, then the lights (position and radius, color)
	SDL_GPUBuffer *cluster_buffer;     // each cluster's first index and count, then the indices
	SDL_GPUBuffer *core_buffer;        // the cores to draw: position and size, color
	SDL_GPUGraphicsPipeline *core_pipeline;
	int count;                         // lights this frame
	int core_count;                    // cores this frame
	int dropped;                       // gathered but past LIGHTS_MAX or out of index room
	lights_lamps_t lamps;
	// The objects that give light, found again now and then (their
	// instance indices); where they are is read each frame
	int *lit_objects;
	int lit_object_count;
	int lit_object_capacity;
	// The objects with windows (emissive groups named "window"), found
	// with them; which are lit is read each frame
	int *window_objects;
	int window_object_count;
	int window_object_capacity;
	double next_object_scan_seconds;
	int scanned_instance_count;
	placed_light_t placed[LIGHTS_PLACED_MAX];
	int placed_count;
	// The decals go into the clusters too (decals.h): how many this frame,
	// and the atlas last sent up (sent again when it is another)
	int decal_count;
	const uint32_t *sent_atlas;
	moving_light_t moving[LIGHTS_MOVING_MAX];
} lights_t;

// What a frame's lights depend on
typedef struct
{
	bool enabled;                      // render.local_lights
	float intensity_scale;             // render.local_light_intensity
	float darkness;                    // 0 full day .. 1 night (lights_darkness)
	float view_projection[16];         // as the shaders get it (scene_uniforms_t)
	float scene_pixels[2];             // the scene pass's size (pixels; lights.glsl: the tile from gl_FragCoord)
	float eye[3];
	double seconds;                    // the game's clock (flicker, the decals' age)
	bool enabled_decals;               // render.decals
	bool window_lights;                // render.window_lights
	float window_light_intensity;      // render.window_light_intensity (as a light source's intensity)
} lights_frame_t;

bool lights_create(lights_t *lights);
void lights_destroy(lights_t *lights);

// How dark it is for the lights (0..1), from the sun's height over the
// horizon: nothing at 8 degrees and above, all of it at 4 below
float lights_darkness(float sun_elevation_degrees);

// The lamps: lit with the day (automatic), or all on, or all off
void lights_set_lamps(lights_t *lights, lights_lamps_t lamps);

// The Light tool's lights (kept with the map, map_file.h): placed at a point,
// removed nearest to a point (within meters; false if none), all removed
bool lights_place(lights_t *lights, const float position[3], const light_source_t *source);
bool lights_remove_near(lights_t *lights, const float position[3], float meters);
void lights_clear_placed(lights_t *lights);

// A carried light, by its owner's key (not 0: the player's, an actor's):
// put there (made the first time), or put out
void lights_set_carried(lights_t *lights, uint32_t key, const float position[3], const light_source_t *source);
void lights_remove_carried(lights_t *lights, uint32_t key);
// Whether that key carries one
bool lights_has_carried(const lights_t *lights, uint32_t key);
// A flash at a point: at full at once, gone after seconds (now: the game's
// clock, as lights_frame_t.seconds); false if there is no room
bool lights_flash(lights_t *lights, const float position[3], const light_source_t *source, float seconds, double now);

// Gathers this frame's lights, clusters them for the camera and uploads
// them (before the scene's render pass); the decals in view with them, into
// the same clusters (decals: NULL for none)
void lights_update(lights_t *lights, const particles_t *particles, const objects_t *objects, const decals_t *decals, const lights_frame_t *frame);
// How much the lights of the last update light a point (all of them: the
// fires, the lamps, the torches carried, the flashes), as the noon sun
// would (1); none through walls (it is not looked for)
float lights_light_at(const lights_t *lights, const float point[3]);

// Binds the two buffers for the fragment shader of a lit pipeline (after
// the pipeline is bound): terrain, objects, actors, water
void lights_bind(SDL_GPURenderPass *pass);

// Draws the cores (in the scene's pass, after what they glow in front of)
void lights_draw_cores(lights_t *lights, SDL_GPURenderPass *pass, const camera_t *camera);

#endif
