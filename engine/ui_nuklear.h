// The one place Nuklear is included from. Every file that uses Nuklear
// includes this header, so all of them see the same configuration (the
// defines change the layout of Nuklear's structures).

#ifndef UI_NUKLEAR_H
#define UI_NUKLEAR_H

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
// The built-in font, for when the configured one cannot be read
#define NK_INCLUDE_DEFAULT_FONT
// 32 bit indices: a busy settings window can pass 65536 vertices
#define NK_UINT_DRAW_INDEX
// A draw command's user data: the HUD marks what is drawn in the world
// (a name over an actor) with its depth, for the depth-tested pipeline
#define NK_INCLUDE_COMMAND_USERDATA
#include "nuklear.h"

#endif
