// The loading screen: a frame of its own, a box in the middle with what is
// being made and how far along, over a dark ground (while a world is made:
// world_t.progress)

#ifndef UI_LOADING_H
#define UI_LOADING_H

#include "ui_backend.h"

// Draws and presents a whole frame (the window's events pumped: it answers
// meanwhile); fraction 0..1
void ui_loading_frame(ui_backend_t *backend, const char *title, const char *what, float fraction);

#endif
