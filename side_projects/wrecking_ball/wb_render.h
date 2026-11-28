// WreckingBall's renderer, on SDL_GPU: flat colored solids (no textures),
// lit by one sun with a shadow map and a sky/ground ambient, meant to run
// passably even on a software Vulkan (lavapipe).
//
// Everything is drawn from a few unit meshes (a box, a 12 sided prism, a
// sphere) and the ground's mesh, instanced: each instance is a place, a
// rotation, a scale along its own axes and a color. A frame:
//
//   wb_render_begin_frame       the instances emptied
//   wb_render_add_*             the frame's solids
//   wb_render_end_frame         the shadow pass, the scene pass, then the
//                               picture to the window (if there is one)
//                               and to a PNG (if asked)
//
// The picture is drawn into a texture of its own at the size given, and
// only then copied to the window: so the same pictures come out with no
// window at all (wb_render_create with window NULL), for checking from
// the command line.

#ifndef WB_RENDER_H
#define WB_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

typedef enum
{
	WB_MESH_BOX,                       // -1..1 on each axis: scale it by the half sizes
	WB_MESH_PRISM,                     // along X, -1..1, radius 1, 12 sides: scale by (half length, radius, radius)
	WB_MESH_SPHERE,                    // radius 1
	WB_MESH_TERRAIN,                   // the ground, in world space (wb_render_set_terrain)
	WB_MESH_COUNT
} wb_mesh_t;

typedef struct
{
	float eye[3];
	float target[3];
	float vertical_field_of_view_degrees;
} wb_camera_t;

typedef struct wb_render_s wb_render_t;

// shader_directory: where the .spv files are. window NULL: no window, the
// pictures only saved
wb_render_t *wb_render_create(SDL_Window *window, int width, int height, const char *shader_directory);
void wb_render_destroy(wb_render_t *render);

// The ground's mesh from its heights: (cells + 1) a side, row by row along
// z, the first corner at (origin_x, origin_z)
bool wb_render_set_terrain(wb_render_t *render, const float *heights, int cells, float cell_meters, float origin_x, float origin_z);

// The part of the world the sun's shadows cover: a box round a center
void wb_render_set_shadow_bounds(wb_render_t *render, const float center[3], float radius);

void wb_render_begin_frame(wb_render_t *render);
// A solid: color is 0xRRGGBB (sRGB)
void wb_render_add(wb_render_t *render, wb_mesh_t mesh, const float position[3], const float rotation[4], const float scale[3], uint32_t color);
// png_path NULL: none saved. False when the frame could not be drawn
bool wb_render_end_frame(wb_render_t *render, const wb_camera_t *camera, const char *png_path);

// The picture's size (the window's, if it has one)
void wb_render_size(const wb_render_t *render, int *width, int *height);

#endif
