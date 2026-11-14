// The .pact file: one animated actor in one file (mesh, skeleton,
// animations, texture), written by tools/potop_convert and read by
// actor_model.c. The engine reads nothing else for actors; other formats
// (Cal3D now, glTF later) are the converter's business.
//
// Everything is little endian, 4 byte aligned, in game axes (Y up, meters,
// the same as the rest of the game) and ready to use: the converter has
// already turned the source's axes, sorted the bones so that a parent
// always comes before its children, cut the influences to four per vertex
// and resampled the animations at a fixed rate.
//
// Layout, in this order:
//   actor_file_header_t
//   actor_file_bone_t      [bone_count]
//   actor_file_vertex_t    [vertex_count]
//   uint32_t               [index_count]       three per triangle, counter clockwise
//   for each clip:
//     actor_file_clip_t
//     actor_file_key_t     [frame_count * bone_count]   frame by frame
//   actor_file_action_t    [action_count]
//   texture                texture_compressed_size bytes: RGBA8 rows,
//                          top row first, compressed with zlib (deflate)
// and from version 2, what physically based shading takes (each as the
// texture, the same size, the same UVs; left out where its size is 0):
//   normal map             normal_compressed_size bytes: tangent space,
//                          RGB = XYZ * 0.5 + 0.5 (the normal scale already in)
//   occlusion, roughness, metal   orm_compressed_size bytes: R occlusion
//                          (1 none), G roughness, B metalness (glTF's
//                          channels; the material's factors already in)
//   emissive               emissive_compressed_size bytes: RGB, sRGB
//   float[4]               [vertex_count] tangents, if extra_flags has
//                          ACTOR_FILE_HAS_TANGENTS: xyz along the
//                          texture's U, w the bitangent's sign
//
// Rotations are unit quaternions (x, y, z, w) that turn a vector v into
// q v q*, the usual convention. A bone's pose is relative to its parent:
// its global pose is parent_global * local. The skinning matrix of a bone
// is global_pose * inverse_bind.

#ifndef ACTOR_FORMAT_H
#define ACTOR_FORMAT_H

#include <stdint.h>

#define ACTOR_FILE_MAGIC "PACT"
#define ACTOR_FILE_VERSION 2u
#define ACTOR_FILE_OLDEST_VERSION 1u   // the game still reads this one (no shading maps)
#define ACTOR_FILE_NAME_LENGTH 32      // names are zero terminated, zero padded
#define ACTOR_FILE_MAX_BONES 64        // what the vertex shader's uniforms hold
#define ACTOR_FILE_INFLUENCES 4

// texture_flags
#define ACTOR_TEXTURE_ALPHA_TESTED 1u  // has cut out (transparent) texels

// extra_flags
#define ACTOR_FILE_HAS_TANGENTS 1u

// action flags
#define ACTOR_ACTION_LOOP 1u           // repeats (walk, idle); otherwise plays once

typedef struct
{
	char magic[4];                     // ACTOR_FILE_MAGIC
	uint32_t version;                  // ACTOR_FILE_VERSION
	uint32_t bone_count;
	uint32_t vertex_count;
	uint32_t index_count;
	uint32_t clip_count;
	uint32_t action_count;
	uint32_t texture_width;
	uint32_t texture_height;
	uint32_t texture_flags;
	uint32_t texture_compressed_size;
	float bounds_minimum[3];           // of the mesh in its bind pose
	float bounds_maximum[3];
	// From version 2 (0 in version 1)
	uint32_t normal_compressed_size;
	uint32_t orm_compressed_size;
	uint32_t emissive_compressed_size;
	uint32_t extra_flags;              // ACTOR_FILE_HAS_*
	uint32_t reserved;
} actor_file_header_t;

typedef struct
{
	char name[ACTOR_FILE_NAME_LENGTH];
	int32_t parent;                    // -1 for a root; always lower than the bone's own index
	float translation[3];              // bind pose, relative to the parent
	float rotation[4];
	// Model space to bone space in the bind pose: 3 rows of a 3x4 matrix
	// (rotation and translation)
	float inverse_bind[12];
} actor_file_bone_t;

typedef struct
{
	float position[3];                 // bind pose
	float normal[3];
	float uv[2];
	uint8_t bones[ACTOR_FILE_INFLUENCES];
	uint8_t weights[ACTOR_FILE_INFLUENCES]; // sum to 255; unused ones are 0
} actor_file_vertex_t;

// An animation as sampled data: frame_count poses, evenly spaced, the first
// at time 0 and the last at duration_seconds
typedef struct
{
	char name[ACTOR_FILE_NAME_LENGTH]; // the source file's name, without extension
	float duration_seconds;
	uint32_t frame_count;              // at least 2
	uint32_t reserved[2];
} actor_file_clip_t;

typedef struct
{
	float translation[3];              // relative to the parent, like the bind pose
	float rotation[4];
} actor_file_key_t;

// What the game asks for (walk, idle, die...): a clip and how to play it.
// Several actions may share a clip.
typedef struct
{
	char name[ACTOR_FILE_NAME_LENGTH];
	int32_t clip;
	uint32_t flags;                    // ACTOR_ACTION_*
	float duration_seconds;            // 0: the clip's own length
} actor_file_action_t;

#endif
