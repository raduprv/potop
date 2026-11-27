// Shared by the particle compute shaders (see particles.h). Every one of
// them has the same resources, bound the same way: in set 0 the samplers
// (PARTICLE_SAMPLERS of them, defined before this is included: the ground
// and the water, for the simulation), then the effects and this frame's
// emit requests (read only); in set 1 the pool, the state and the render
// buffer (read and written); in set 2 the pass's uniforms.

#ifndef PARTICLE_SAMPLERS
#define PARTICLE_SAMPLERS 0
#endif

struct particle_t
{
	vec4 position_age;                 // meters; seconds lived
	vec4 velocity_life;                // meters per second; seconds it lives
	vec4 data;                         // effect, random number (0..1), turn (radians), spin (radians a second)
};

struct render_t
{
	vec4 position_size;                // meters; meters across
	vec4 color;                        // 0..1, alpha last
	vec4 extra;                        // turn (radians), texture layer, emissive (1) or lit (0), the ground or water under it
	vec4 velocity;                     // meters per second; stretch (seconds of motion it is drawn long)
	vec4 look;                         // distortion, random number (0..1), soft (meters), age (0..1)
};

// Must match pack_effect in particles.c
struct effect_t
{
	vec4 colors[4];
	vec4 color_times;
	vec4 sizes;
	vec4 size_times;
	vec4 motion;                       // gravity (down), drag, turbulence, 1 / turbulence scale
	vec4 look;                         // first layer, frames, frames per second, emissive
	vec4 collision;                    // collide mode, bounce, spin minimum, spin maximum
	vec4 emission;                     // life minimum, maximum, speed minimum, maximum
	vec4 direction;                    // normalized; cosine of the spread
	vec4 extra;                        // outward speed, stretch (negative: -1 - stretch, born upright), color stops, size stops
	vec4 flash;                        // seconds a burst (0: no flashing), seconds a flash, flashes a burst, how together (0..1)
	vec4 look_extra;                   // distortion, soft (meters), -, -
};

// Must match emit_request in particles.c
struct emit_t
{
	vec4 position_effect;              // the emitter's middle; effect
	vec4 rotation;                     // its turn, quaternion
	vec4 extents_shape;                // half its size; shape
	vec4 range;                        // first thread, count, random seed, radius
	vec4 push;                         // added to the speed (world, m/s); life scale
};

layout(std430, set = READWRITE_SET, binding = 0) buffer particle_buffer
{
	particle_t particles[];
};

// Counters, the indirect arguments and the lists, all unsigned:
//   [0]                 free slots
//   [4 + 8 k ..]        living list k: draw arguments (4, count, 0, 0),
//                       then dispatch arguments (groups, 1, 1)
//   [20 ..]             the draw's arguments (4, highest slot ever used + 1,
//                       0, 0): drawn slot by slot, not in the living list's
//                       order (that comes out of the atomics different each
//                       frame, and the alpha ones, unsorted, would flicker)
//   [32 ..]             the free slots
//   [32 + capacity (1 + k) ..]  living list k
layout(std430, set = READWRITE_SET, binding = 1) buffer state_buffer
{
	uint state[];
};

layout(std430, set = READWRITE_SET, binding = 2) buffer render_buffer
{
	render_t renders[];
};

layout(std430, set = READONLY_SET, binding = PARTICLE_SAMPLERS) readonly buffer effect_buffer
{
	effect_t effects[];
};

layout(std430, set = READONLY_SET, binding = PARTICLE_SAMPLERS + 1) readonly buffer emit_buffer
{
	emit_t emits[];
};

// What one pass needs (the same block for all three)
layout(std140, set = UNIFORM_SET, binding = 0) uniform particle_pass_block
{
	int list;                      // the living list emitted into / simulated from
	int emit_total;                // emit: threads with work
	int emit_count;                // emit: requests
	int particle_capacity;         // particles the pool holds
	float step_seconds;            // simulate
	float time_seconds;
	int map_cells;
	float cell_size_meters;
	int has_water;
	int pass_padding_0;
	int pass_padding_1;
	int pass_padding_2;
};

#define STATE_FREE_COUNT 0u
#define STATE_LIST(k) (4u + 8u * uint(k))
#define STATE_DRAW 20u
#define STATE_FREE_FIRST 32u
#define STATE_ALIVE_FIRST(k) (32u + uint(particle_capacity) * (1u + uint(k)))

uint hash_uint(uint value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

// 0..1, and the seed moves on
float random_float(inout uint seed)
{
	seed = hash_uint(seed + 0x9e3779b9u);
	return float(seed >> 8) * (1.0 / 16777216.0);
}

vec3 rotate_by(vec4 quaternion, vec3 vector)
{
	vec3 t = 2.0 * cross(quaternion.xyz, vector);

	return vector + quaternion.w * t + cross(quaternion.xyz, t);
}
