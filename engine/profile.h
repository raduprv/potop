// Tiny phase profiler for the simulation: accumulates the time spent in
// each phase until the diagnostics log reads and clears it.

#ifndef PROFILE_H
#define PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
	PROFILE_WATER_SOURCES = 0,
	PROFILE_WATER_FLUXES,
	PROFILE_WATER_DEPTHS,
	PROFILE_WATER_BOOKKEEPING,
	PROFILE_WATER_EVAPORATION,
	PROFILE_EROSION_ERODE,
	PROFILE_EROSION_COLLAPSE,
	PROFILE_EROSION_REFRESH,
	PROFILE_EROSION_SOAK,
	PROFILE_PHASE_COUNT
} profile_phase_t;

typedef struct
{
	uint64_t ticks[PROFILE_PHASE_COUNT];
	int64_t substeps;              // water substeps run
	int64_t tile_substeps;         // tiles simulated, summed over substeps
} profile_t;

extern profile_t profile;

// Returns the current time; pass it to profile_end with the phase
uint64_t profile_begin(void);
void profile_end(profile_phase_t phase, uint64_t start);

const char *profile_phase_name(profile_phase_t phase);

// ---------------------------------------------------------------------------
// The frame profiler: where each frame's time went, and what happened in
// it. A frame much longer than the usual (a stutter) is logged with its
// breakdown; the status line shows the slowest frame of each interval.

typedef enum
{
	FRAME_UPDATE = 0,              // (not timed: the parts below are)
	FRAME_POLL,                    // the GPU's copies applied to the CPU
	FRAME_WATER,                   // water steps recorded
	FRAME_EROSION,                 // erosion steps recorded
	FRAME_WETTING,                 // the ground's moisture
	FRAME_PHYSICS,                 // physics steps
	FRAME_PARTICLES,
	FRAME_WATER_END,               // water textures and the copy back asked for
	FRAME_COPY_WAIT,               // of which: waiting for the GPU to finish an old copy
	FRAME_ACQUIRE,                 // waiting for the swapchain (vsync, or a busy GPU)
	FRAME_OBJECTS_FOLLOW,          // objects put back on changed ground, floating ones on the water
	FRAME_NAVIGATION,              // the path grid following the map
	FRAME_TERRAIN_UPLOAD,          // terrain chunks rebuilt and uploaded
	FRAME_SHADOWS,                 // shadow maps recorded
	FRAME_OBJECTS_CULL,            // the depth pyramid and the objects' culling recorded
	FRAME_RENDER,                  // all of the drawing, the four above included
	FRAME_PRESENT,                 // submitting the frame
	FRAME_GAME,                    // the game's own update (between potop_frame_begin and potop_frame_end, and after it)
	FRAME_EVENTS,                  // the window's events read, the frame begun
	FRAME_HUD,                     // the HUD laid out, its requests, the console, the session
	FRAME_PLAYER,                  // the walker or the camera, the steps, the player's light
	FRAME_ACTORS,                  // the actors (their paths included)
	FRAME_PATHS,                   // of which (or of the game's): the path searches
	FRAME_WEAPONS,                 // the weapons, the shots, the explosives, the areas
	FRAME_UI,                      // the HUD drawn
	FRAME_SKY_OCCLUSION,           // the sky's occlusion brought to the CPU
	FRAME_LIMIT,                   // waited out for the frame rate limit (max_fps), or resting in the background
	// Inside objects follow and terrain upload (to find what costs there)
	FRAME_FOLLOW_WATER,            // floating objects on the water
	FRAME_FLOODED_COVER,           // the cover a flood took away, silt buried
	FRAME_TERRAIN_SETUP,           // the map's textures, the GPU's targets, which chunks can wait
	FRAME_TERRAIN_MATERIALS,       // the materials baked and sent up
	FRAME_CHUNK_PREPARE,           // the chunks' wetness, soils (heights, normals on the CPU) made
	FRAME_CHUNK_SEND,              // and handed to the GPU
	FRAME_SOIL_BLUR,               // the soils' weights of chunks whose soils changed
	FRAME_TERRAIN_TILES,           // the water's tiles sent up
	FRAME_CHUNK_BOUNDS,            // the changed chunks' heights, lowest and highest (in objects follow)
	// Inside physics
	FRAME_PHYSICS_SYNC,            // poses restored, the bodies matched to the objects again (after objects come or go), the walls
	FRAME_PHYSICS_TERRAIN,         // the ground tiles made again (tools, erosion)
	FRAME_PHYSICS_WATER,           // the water's push on the bodies (buoyancy, drag), waking them
	FRAME_PHYSICS_FLOOD,           // grass laid down by the water, standing up again
	FRAME_PHYSICS_WORLD,           // Box3D's step
	FRAME_PHYSICS_HITS,            // what hit what (damage, sounds)
	FRAME_PHYSICS_FOLLOW,          // the objects following their bodies
	FRAME_PHYSICS_WAKE,            // the neighbours of moved ones woken
	FRAME_PHYSICS_REMOVE,          // the objects gone (buried, off the map, destroyed) taken out
	FRAME_SECTION_COUNT
} frame_section_t;

typedef enum
{
	FRAME_STEPS = 0,
	FRAME_SUBSTEPS,
	FRAME_EVAPORATIONS,
	FRAME_COLLAPSES,
	FRAME_WATER_TILES,             // water tiles copied back
	FRAME_GROUND_TILES,            // ground tiles copied back that changed
	FRAME_WET_TILES,               // tiles whose moisture was updated
	FRAME_CHUNK_UPLOADS,
	FRAME_PHYSICS_REBUILDS,        // Box3D height fields rebuilt
	FRAME_NAVIGATION_REGIONS,      // path grid regions built
	FRAME_NAVIGATION_TILES,        // navigation mesh tiles built (the rectangles of a region, for a width of walker)
	FRAME_NAVIGATION_BUILD_MICROSECONDS, // what building those regions and tiles took, together
	FRAME_SHADOW_REDRAWS,
	FRAME_NEAR_SHADOW_REDRAWS,
	FRAME_MOVING_SHADOW_REDRAWS,   // the near map's moving layer over a copy of the still one
	FRAME_PATH_SEARCHES,           // paths searched for
	FRAME_PATH_EXPANSIONS,         // surfaces those searched, all together
	FRAME_OBJECTS_CHECKED,         // objects whose ground was looked at again
	FRAME_OBJECTS_MOVED,           // of which moved (their ground changed by more than a hair)
	FRAME_AWAKE_BODIES,            // after each physics step, the bodies awake (added up over the frame's steps)
	FRAME_BODIES_PUSHED,           // bodies the water pushed
	FRAME_BODY_MOVES,              // bodies that moved (Box3D's move events)
	FRAME_OBJECTS_REMOVED,         // objects taken out by the physics
	FRAME_GPU_WATER_TILES,         // water tiles the GPU simulated (per step, added up)
	FRAME_TERRAIN_CHUNKS_DRAWN,
	FRAME_WATER_CHUNKS_DRAWN,
	FRAME_SHADOW_DRAW_CALLS,       // the objects' draw calls into the shadow maps (and the sky's occlusion)
	FRAME_FAR_SHADOW_FOR_TERRAIN,  // a far shadow redraw asked for by the ground changing (the editor)
	FRAME_FAR_SHADOW_FOR_OBJECTS,  // ... by objects changing (the editor)
	FRAME_FAR_SHADOW_FOR_LIGHT,    // ... by the sun turning
	FRAME_FAR_SHADOW_FOR_CAMERA,   // ... by the camera leaving its square (a view distance)
	FRAME_OBJECT_SORTS,            // the objects sorted into their cells again, all of them (one taken out, many added)
	FRAME_BODY_FULL_SYNCS,         // the bodies matched to the objects again, all of them (after such a sort)
	FRAME_BODIES_MADE,             // bodies made (a cell's near the focus at once, the queue's a little a step)
	FRAME_CLOUD_PASSES,            // the clouds' dome marched: sixteenths of it this frame (by the clock: none, one, up to four after a slow frame)
	FRAME_CLOUD_WHOLE_DOMES,       // ...all of it at once (the eye moving: coarsely; a jump or the first: finely)
	FRAME_CLOUD_SHADOWS,           // the clouds' shadow map made again
	FRAME_WATERFALL_SEARCHES,      // the waterfalls looked for again round the player (every few seconds)
	FRAME_GRASS_MASK_REBUILDS,     // the grass's mask made again whole (objects changed, the eye far off) and sent up
	FRAME_GRASS_MASK_STRIPS,       // ...its strips coming into the window as the eye moves
	FRAME_OBJECT_CHANGES,          // objects' changes noted (killed, come loose, stirred, another type...)
	FRAME_UPLOAD_FLUSHES,          // times the staged uploads were copied into the (cycled) transfer buffer and recorded
	FRAME_UPLOAD_KILOBYTES,        // ...and how much went up
	FRAME_COUNTER_COUNT
} frame_counter_t;

// How many frames before the two just before a stutter its line looks at
#define FRAME_OLDER_FRAMES 16

typedef struct
{
	uint64_t ticks[FRAME_SECTION_COUNT];
	int32_t counters[FRAME_COUNTER_COUNT];
	// The two frames before (the GPU runs behind: a frame waiting for it
	// pays for what those asked of it)
	int32_t earlier_counters[FRAME_COUNTER_COUNT];
	int32_t previous_counters[FRAME_COUNTER_COUNT];
	// ...and the ones before those, FRAME_OLDER_FRAMES of them in a ring
	// (older_next: where the next goes): what a burst of waits began with
	// is often a few frames before its first slow frame
	int32_t older_counters[FRAME_OLDER_FRAMES][FRAME_COUNTER_COUNT];
	int older_next;
	uint64_t last_finish;
	double usual_milliseconds;     // slow running average of the frame time
	double worst_milliseconds;     // since frame_profile_take_worst
	int spikes_logged_this_second;
	double second_timer;
	int64_t spike_count;
	// Every frame's sections added up, for the diagnostics' averages
	uint64_t sum_ticks[FRAME_SECTION_COUNT];
	int32_t sum_frames;
	// The frame's slowest path search: whose, how long, how many surfaces,
	// how it came out ("found", "partial", "none")
	uint64_t slowest_path_ticks;
	uint32_t slowest_path_actor;
	const char *slowest_path_type;
	int32_t slowest_path_expansions;
	const char *slowest_path_result;
	float slowest_path_distance;   // from the actor to its goal, meters
} frame_profile_t;

extern frame_profile_t frame_profile;

void frame_profile_end(frame_section_t section, uint64_t start);

static inline void frame_profile_count(frame_counter_t counter, int32_t amount)
{
	frame_profile.counters[counter] += amount;
}

// A path search done (its time is FRAME_PATHS'): counted, and kept if it is
// the frame's slowest so far
void frame_profile_path(uint64_t ticks, uint32_t actor, const char *type_name, int32_t expansions, const char *result, float distance);

// At the end of each frame: logs it if it was a stutter, then starts the
// next one
void frame_profile_finish(void);

// The slowest frame (milliseconds) since the last call
double frame_profile_take_worst(void);

// The sections' average milliseconds per frame since the last call, as a
// line ("update 0.31, ... acquire 2.10, render 3.62, present 0.05";
// those under 0.01 left out; the sections inside render shown as they are,
// not added again), and the frames they were over (0: none yet)
int frame_profile_take_averages(char *text, size_t size);

// ---------------------------------------------------------------------------
// The trace: where the time of something goes, step by step, logged as one
// line (the first frames after a load, the stages of the start). Between
// trace_begin and trace_end every trace_mark adds the time since the mark
// before under its name; steps under half a millisecond go into "the
// rest". Marks with nothing begun cost a test.

extern bool trace_active;

void trace_begin(const char *label);
// The same, logged only if it took log_over_milliseconds or more in all,
// steps under step_milliseconds into "the rest" (every frame traced so,
// the slow ones logged with where their time went)
void trace_begin_if_slow(const char *label, double log_over_milliseconds, double step_milliseconds);
// Each step of live_milliseconds or more logged at once, as it ends (0:
// none), not only in the line at the end: a step that takes minutes shows
// where it is stuck (the one after the last logged)
void trace_log_steps_over(double live_milliseconds);
void trace_mark_slow(const char *name);
// Logs the line (and ends the trace)
void trace_end(void);
// A frame traced (the next, or frames_later after it), every step however
// small (console: profile frame [frames]);
// trace_frame_begin at the frame's start begins it (trace_end at its end)
void trace_next_frame(int frames_later);
void trace_frame_begin(void);

static inline void trace_mark(const char *name)
{
	if (trace_active)
	{
		trace_mark_slow(name);
	}
}

#endif
