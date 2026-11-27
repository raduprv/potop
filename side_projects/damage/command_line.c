// What the damage tools' command lines share: see command_line.h

#include "command_line.h"

#include <stdio.h>

bool command_line_read_size(const char *text, int *width, int *height)
{
	int read_width;
	int read_height;

	if (sscanf(text, "%dx%d", &read_width, &read_height) != 2 || read_width < 16 || read_height < 16 || read_width > 8192 || read_height > 8192)
	{
		return false;
	}
	*width = read_width;
	*height = read_height;
	return true;
}

bool command_line_read_camera(const char *text, soft_render_camera_t *camera)
{
	float values[6];
	int i;

	if (sscanf(text, "%f,%f,%f,%f,%f,%f", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6)
	{
		return false;
	}
	for (i = 0; i < 3; i++)
	{
		camera->eye[i] = values[i];
		camera->target[i] = values[i + 3];
	}
	return true;
}

void command_line_write_camera(const soft_render_camera_t *camera, char *text, int text_size)
{
	snprintf(text, (size_t)text_size, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
		(double)camera->eye[0], (double)camera->eye[1], (double)camera->eye[2],
		(double)camera->target[0], (double)camera->target[1], (double)camera->target[2]);
}
