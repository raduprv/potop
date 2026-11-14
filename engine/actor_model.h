// Animated actor models, loaded from .pact files (see actor_format.h).
//
// The mesh and its texture go to the GPU; the skeleton and the animation
// clips stay on the CPU, where the poses are computed. A pose ends up as
// one skinning matrix per bone, three rows of a 3x4 matrix, which the vertex
// shader (data/shaders/actor_skinning.glsl) blends per vertex.

#ifndef ACTOR_MODEL_H
#define ACTOR_MODEL_H

#include <stdbool.h>

#include "actor_format.h"
#include "gpu.h"
#include "math3d.h"

#define ACTOR_MODEL_PATH_LENGTH 128

// Where on the body a bone is, by its name (what a hit there means is the
// game's: a head shot)
typedef enum
{
	ACTOR_ZONE_BODY = 0,
	ACTOR_ZONE_HEAD,                   // head, mouth, jaw, ears, eyes
	ACTOR_ZONE_LIMB                    // legs, arms, wings, tail, feet, hands, toes
} actor_zone_t;

typedef struct
{
	actor_file_clip_t info;
	actor_file_key_t *keys;            // frame_count * bone_count, frame by frame
} actor_clip_t;

typedef struct
{
	char path[ACTOR_MODEL_PATH_LENGTH]; // relative to data/
	SDL_GPUBuffer *vertex_buffer;      // actor_file_vertex_t, as in the file
	SDL_GPUBuffer *tangent_buffer;     // float[4] per vertex (from the file, or made from the UVs)
	SDL_GPUBuffer *index_buffer;
	SDL_GPUIndexElementSize index_size;
	int index_count;
	SDL_GPUTexture *texture;
	// The shading maps (version 2 files); NULL: the neutral ones
	// (object_default_texture)
	SDL_GPUTexture *normal_texture;
	SDL_GPUTexture *orm_texture;
	SDL_GPUTexture *emissive_texture;
	bool shaded;                       // any of the maps: lit physically (render.pbr_shading), else the plain diffuse light
	bool alpha_tested;
	vec3_t bounds_minimum;             // bind pose, game axes, meters
	vec3_t bounds_maximum;
	// Where it stands: the extent (x and z) of the bind pose's lowest
	// vertices, its feet, in model space
	float feet_minimum_x;
	float feet_maximum_x;
	float feet_minimum_z;
	float feet_maximum_z;

	int bone_count;
	actor_file_bone_t *bones;
	int clip_count;
	actor_clip_t *clips;
	int action_count;
	actor_file_action_t *actions;
	// The bind pose's skinning matrices (all but identity): what an actor
	// without an animation is drawn with
	float *bind_rows;                  // bone_count * 12
	// What hits it: per bone a box in the bone's own space around the
	// vertices it moves most (none: hit_box_vertices 0), posed with the bone
	float *bone_to_model;              // bone_count * 12: the bind pose's bone space to model space (inverse_bind's inverse)
	float *hit_boxes;                  // bone_count * 6: minimum x y z, maximum x y z (bone space)
	int *hit_box_vertices;             // bone_count: the vertices in it
	actor_zone_t *bone_zones;          // bone_count
} actor_model_t;

// Loads a .pact file from the data directory. Returns false (after logging
// why) if it is missing or malformed.
bool actor_model_load(actor_model_t *model, const char *relative_path);
void actor_model_destroy(actor_model_t *model);

// Skinning matrices (bone_count * 12 floats) for a pose given as each
// bone's translation and rotation relative to its parent
void actor_model_pose_rows(const actor_model_t *model, const actor_file_key_t *local_pose, float *rows_out);
// The same in two steps, for changing a pose in model space in between
// (feet on the ground): each bone's place and turn in model space, then
// the skinning matrices from those
void actor_model_compose_global(const actor_model_t *model, const actor_file_key_t *local_pose, actor_file_key_t *global_out);
void actor_model_global_rows(const actor_model_t *model, const actor_file_key_t *global_pose, float *rows_out);

// The clip's pose at a time (clamped to the clip), interpolated between its
// frames: each bone's translation and rotation relative to its parent
void actor_model_sample_clip(const actor_model_t *model, int clip, float seconds, actor_file_key_t *pose_out);

// Index of the action with that name (walk, idle...), -1 if there is none
int actor_model_find_action(const actor_model_t *model, const char *name);

// The first action whose name starts with prefix (die: die1...), -1 if none
int actor_model_find_action_prefix(const actor_model_t *model, const char *prefix);

// 3 rows of 3x4 affine matrices: out = left * right; the inverse
void actor_rows_multiply(const float left[12], const float right[12], float out[12]);
bool actor_rows_invert(const float rows[12], float out[12]);

#endif
