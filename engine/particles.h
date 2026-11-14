// Particles: fire, smoke, sparks, magic. Effects are described in
// data/particles.ini (one section each); the game plays them at a point or
// on an object or actor, and the GPU does the rest.
//
// Every particle lives on the GPU (compute shaders, see
// data/shaders/particles_*.comp): a pool of them, a list of the free slots
// and two lists of the living ones, used in turn. Each frame:
//   1. emit: the CPU says only "this many of effect E, here, shaped so";
//      each new particle takes a free slot and joins the living list
//   2. prepare: one thread sizes the next pass from the living count
//   3. simulate: each living particle ages, moves (gravity, drag,
//      turbulence, the ground and the water) and either dies (its slot is
//      free again) or joins the other living list, writing what is drawn
//      of it into the render buffer
//   4. draw: one instanced quad per living particle, the count straight
//      from the GPU (indirect draw), nothing read back
//
// The textures are made here, at start (no image files): a soft dot, a
// spark, smoke puffs, flame frames, a star and a ring, in one texture array.

#ifndef PARTICLES_H
#define PARTICLES_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"
#include "gpu.h"
#include "light_source.h"
#include "settings.h"

#define PARTICLES_EFFECTS_MAX 64
#define PARTICLES_EFFECT_NAME_LENGTH 32
#define PARTICLES_STOPS_MAX 4          // points of a color or size gradient
#define PARTICLES_EMITTERS_MAX 1024
#define PARTICLES_EMITS_MAX 256        // emit requests a frame (the rest wait for the next)

typedef enum
{
	PARTICLES_SHAPE_POINT = 0,
	PARTICLES_SHAPE_SPHERE,            // inside a ball
	PARTICLES_SHAPE_DISC,              // on a flat disc
	PARTICLES_SHAPE_RING,              // on a flat circle's edge
	PARTICLES_SHAPE_BOX,               // inside a box (the thing it is on)
	PARTICLES_SHAPE_OUTLINE,           // on the sides of a box: around the thing it is on
	PARTICLES_SHAPE_COUNT
} particles_shape_t;

typedef enum
{
	PARTICLES_COLLIDE_NONE = 0,
	PARTICLES_COLLIDE_DIE,             // gone when it reaches the ground or the water
	PARTICLES_COLLIDE_BOUNCE,          // bounces off them
	PARTICLES_COLLIDE_SLIDE,           // slides along them (down a slope, over the water), bounce: its friction (1 / seconds)
	PARTICLES_COLLIDE_COUNT
} particles_collide_t;

typedef enum
{
	PARTICLES_ATTACH_NONE = 0,         // at a point
	PARTICLES_ATTACH_OBJECT,           // on an object (follows it if the physics moves it)
	PARTICLES_ATTACH_ACTOR             // on an actor (follows it)
} particles_attach_t;

typedef struct
{
	char name[PARTICLES_EFFECT_NAME_LENGTH];
	// Texture: first layer in the texture array and how many frames
	int texture_layer;
	int texture_frames;
	float frames_per_second;           // 0: the frames play once over the particle's life
	bool emissive;                     // glows (added to what is behind) instead of being lit and covering it
	// Emitting
	bool burst;                        // count over duration seconds, then done; else rate a second for ever
	float rate;
	int count;
	float duration_seconds;
	particles_shape_t shape;
	float shape_radius;                // sphere, disc, ring (a point on an object or actor: their size)
	// Particles
	float life_minimum;
	float life_maximum;
	float speed_minimum;
	float speed_maximum;
	float direction[3];                // normalized
	float spread_degrees;              // around the direction; 180: every way
	float outward;                     // speed away from the shape's middle (meters per second), on top
	float gravity;                     // meters per second squared, down (negative: rises)
	float drag;                        // 1 / seconds
	float turbulence;                  // meters per second squared
	float turbulence_scale;            // meters: the size of the turbulence's swirls
	bool random_turn;                  // born turned any way (else upright: flames)
	float spin_minimum;                // radians per second
	float spin_maximum;
	float stretch;                     // > 0: drawn long along the motion (sparks): seconds of motion
	particles_collide_t collide;
	float bounce;                      // speed kept, up and down, after a bounce
	// Over its life (0 = born, 1 = dead): color (0..1, alpha last) and size (meters)
	int color_stop_count;
	float color_times[PARTICLES_STOPS_MAX];
	float colors[PARTICLES_STOPS_MAX][4];
	int size_stop_count;
	float size_times[PARTICLES_STOPS_MAX];
	float sizes[PARTICLES_STOPS_MAX];
	light_source_t light;              // the light it gives (fires): see lights.h
	// Flashing (fireflies): bursts of flash_count flashes, one each
	// flash_seconds, a burst each flash_burst_seconds (0: it does not
	// flash); flash_sync 1: all of them together (the game's clock), 0: each
	// at its own moment
	float flash_burst_seconds;
	float flash_seconds;
	float flash_count;
	float flash_sync;
} particles_effect_t;

typedef struct
{
	bool used;
	uint32_t id;
	int effect;
	particles_attach_t attach;
	int object_body;                   // ATTACH_OBJECT with a physics body: its slot (else the pose stays)
	uint32_t actor_id;                 // ATTACH_ACTOR
	// Where it is: the middle, the turn (quaternion) and half the size of
	// what it is on (0 at a point)
	float position[3];
	float rotation[4];
	float extents[3];
	float age_seconds;
	float emit_carry;                  // particles owed (fractions carried to the next frame)
	int emitted;
	// What the one who plays it says on top of the effect (particles_play_on
	// sets 1, 1, none): its rate times rate_scale, its particles' life times
	// life_scale, and push (meters per second, world) added to their speed
	// (a waterfall's: as fast as its water, the way it runs)
	float rate_scale;
	float life_scale;
	float push[3];
	// Placed by the player (the Effect tool): kept with the map
	bool kept;
} particles_emitter_t;

typedef struct
{
	bool created;
	particles_effect_t effects[PARTICLES_EFFECTS_MAX];
	int effect_count;
	particles_emitter_t emitters[PARTICLES_EMITTERS_MAX];
	uint32_t next_emitter_id;
	uint32_t random_state;
	int capacity;                      // particles the pool holds
	int current_list;                  // the living list emitted into and simulated from this frame
	float time_seconds;
	int emitted_last_frame;

	SDL_GPUComputePipeline *emit_pipeline;
	SDL_GPUComputePipeline *prepare_pipeline;
	SDL_GPUComputePipeline *simulate_pipeline;
	SDL_GPUGraphicsPipeline *draw_pipeline;
	SDL_GPUBuffer *particle_buffer;    // the pool
	SDL_GPUBuffer *state_buffer;       // counters, indirect arguments, free and living lists
	// A copy of the state's indirect arguments, which the GPU reads them
	// from (a buffer the shaders write is only seen right by them; the
	// copy pass makes the arguments visible to indirect reads)
	SDL_GPUBuffer *argument_buffer;
	SDL_GPUBuffer *render_buffer;      // what is drawn of each living particle (instance attributes)
	SDL_GPUBuffer *effect_buffer;      // the effects
	SDL_GPUBuffer *emit_buffer;        // this frame's emit requests
	SDL_GPUTexture *texture_array;
	int texture_layer_count;
} particles_t;

// Loads data/particles.ini, makes the textures, the shaders and the GPU
// buffers (capacity particles). False if the shaders cannot be made (the
// game goes on without particles)
bool particles_create(particles_t *particles, int capacity);
void particles_destroy(particles_t *particles);

// Reads data/particles.ini again (the effects playing keep going, with the
// new values; ones whose effect is gone stop)
void particles_reload(particles_t *particles);

int particles_find_effect(const particles_t *particles, const char *name);

// Starts an effect at a point; returns its emitter's id (0 if none was
// free or there is no such effect). A continuous one goes on until stopped
uint32_t particles_play(particles_t *particles, int effect, float x, float y, float z);

// On something: middle, turn (quaternion x, y, z, w) and half its size
uint32_t particles_play_on(particles_t *particles, int effect, const float position[3], const float rotation[4], const float extents[3]);

particles_emitter_t *particles_find_emitter(particles_t *particles, uint32_t id);
void particles_stop(particles_t *particles, uint32_t id);
void particles_stop_all(particles_t *particles);

// The emitter nearest to (x, y, z) within radius; 0 if none
uint32_t particles_nearest_emitter(const particles_t *particles, float x, float y, float z, float radius);

// Ages the emitters and runs the GPU passes for seconds of game time (0:
// paused; nothing moves, the particles are still drawn). height_texture is
// the ground (R32F, meters a cell), water_texture the water's depth on it
// (R32F, may be 0); map_meters the map's side
// The compute passes go into gpu_commands().
void particles_update(particles_t *particles, float seconds, SDL_GPUTexture *height_texture, SDL_GPUTexture *water_texture, int map_cells,
	float cell_size_meters);

// Draws the living particles in the scene's pass (after everything else;
// its scene block pushed)
// target_height: the pixels the scene is drawn in (the thinnest particles
// are widened to a pixel, dimmer, rather than lost between the pixels)
void particles_draw(particles_t *particles, SDL_GPURenderPass *pass, const camera_t *camera, const render_settings_t *settings, int target_height);

// Statistics: living particles is only known on the GPU; this is what
// was asked for last frame
int particles_emitter_count(const particles_t *particles);

#endif
