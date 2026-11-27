// What the damage tools' command lines share: a picture's size and a
// camera, written the same way for the viewer and the headless tool (the
// viewer prints its camera in this form, to be pasted to the other).

#ifndef COMMAND_LINE_H
#define COMMAND_LINE_H

#include <stdbool.h>

#include "soft_render.h"

// "1280x720"
bool command_line_read_size(const char *text, int *width, int *height);
// "eye_x,eye_y,eye_z,target_x,target_y,target_z"
bool command_line_read_camera(const char *text, soft_render_camera_t *camera);
// Writes the camera as the text command_line_read_camera reads
void command_line_write_camera(const soft_render_camera_t *camera, char *text, int text_size);

#endif
