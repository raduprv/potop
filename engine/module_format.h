// The .pmod file: a module, a building made of several objects (.pobj) that
// stand together and move together, with the states it can be in. Written
// by tools (tools/potop_house now, an editor later), never by hand; read by
// object_types.c, which makes an object type of each of its parts. Read
// and written by module_file.c, which the tools share with the game.
//
// The module stands where its first part (the body: parts[0]) stands: that
// part's origin is the module's, it is never moved by the states, and the
// objects.ini type that names the module is drawn with its model. The
// other parts are placed round it in the module's frame (meters, +Y up, +Z
// its front, as the body's model), each turned round Y by its yaw, and
// moved by the state the module is in: slid by an offset, turned round its
// axis (in its own frame) by an angle, spun round the same axis at spin
// times its spin_rpm, or hidden. A change of state takes the new state's
// seconds; a state may hold a while and go on to another by itself (a
// quarry's crane up, a pause, down again) until the game says otherwise.
//
// Everything is little endian, 4 byte aligned. Layout, in this order:
//   module_file_header_t
//   module_file_part_t  [part_count]
//   module_file_state_t [state_count]
//   module_file_pose_t  [state_count * part_count]   state by state, each its parts' poses in order

#ifndef MODULE_FORMAT_H
#define MODULE_FORMAT_H

#include <stdint.h>

#define MODULE_FILE_MAGIC "PMOD"
#define MODULE_FILE_VERSION 1u
#define MODULE_FILE_NAME_LENGTH 32
#define MODULE_FILE_MODEL_LENGTH 64
#define MODULE_FILE_MAX_PARTS 32
#define MODULE_FILE_MAX_STATES 16

// module_file_part_t.flags
#define MODULE_PART_COLLISION 0x01u    // a body for the physics: its model's triangles (only while it does not move)
#define MODULE_PART_SHADOW 0x02u       // casts a shadow
#define MODULE_PART_MOVES 0x04u        // some state moves it: drawn as a moving object (off the grid, no impostor, no body)
#define MODULE_PART_IMPOSTOR 0x08u     // far away, drawn as a picture of itself (if its model is worth one)
#define MODULE_PART_DOOR 0x10u         // a door or a gate: closed or open each on its own (objects_set_door), not by the module's state; its poses
                                       // in the states named "closed" and "open" (with MODULES_PART_MOVES)

// module_file_pose_t.flags
#define MODULE_POSE_HIDDEN 0x01u       // not there in this state

typedef struct
{
	char magic[4];                        // MODULE_FILE_MAGIC
	uint32_t version;                     // MODULE_FILE_VERSION
	uint32_t part_count;                  // 1 .. MODULE_FILE_MAX_PARTS
	uint32_t state_count;                 // 1 .. MODULE_FILE_MAX_STATES
	uint32_t first_state;                 // the state it is in when placed
	uint32_t flags;                       // none yet: 0
	uint32_t reserved[2];
} module_file_header_t;

typedef struct
{
	char name[MODULE_FILE_NAME_LENGTH];   // zero terminated, zero padded: the game names it by this ("sails")
	char model[MODULE_FILE_MODEL_LENGTH]; // its .pobj under data/objects/, no extension, as objects.ini's model ("windmill_sails")
	float position[3];                    // its origin in the module's frame (meters)
	float yaw_degrees;                    // turned round Y, from +Z towards +X
	float axis[3];                        // what it turns round (its own frame, unit; 0, 0, 0: never turns)
	float spin_rpm;                       // turns a minute while a state spins it at 1 (negative: the other way)
	int32_t parent;                       // reserved: -1
	uint32_t flags;                       // MODULE_PART_
	uint32_t reserved[2];
} module_file_part_t;

typedef struct
{
	char name[MODULE_FILE_NAME_LENGTH];   // "working", "up"...
	float seconds;                        // how long the change into it takes (0: at once)
	float hold_seconds;                   // with next: how long it stays before it goes on
	int32_t next;                         // the state it goes on to by itself (-1: it stays)
	uint32_t flags;                       // none yet: 0
} module_file_state_t;

typedef struct
{
	float offset[3];                      // slid this far from its place (the module's frame, meters)
	float angle_degrees;                  // turned this far round its axis (counter clockwise looking down the axis at it)
	float spin;                           // spun at this share of its spin_rpm (0: still)
	uint32_t flags;                       // MODULE_POSE_
} module_file_pose_t;

#endif
