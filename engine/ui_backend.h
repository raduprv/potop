// Nuklear on our side: the font, the drawing (through SDL_GPU)
// and the SDL events, including which of them the interface keeps for
// itself.
//
// The interface works in its own units: window points divided by the
// interface scale (ui.ini), so a scale of 1.25 makes everything a quarter
// bigger. The font is baked at the pixel size it is shown at (window
// points times the screen's pixel density times the scale), so text stays
// sharp on high density screens.
//
// Which events belong to the interface is decided from the previous
// frame's layout: a click or a wheel turn over a window of the interface,
// or anything while one of its widgets is held (a slider being dragged),
// is the interface's; key presses are the interface's only while a text
// field is being typed into. Releases of mouse buttons go to both, so
// nothing stays held.

#ifndef UI_BACKEND_H
#define UI_BACKEND_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#include "gpu.h"
#include "ui_nuklear.h"


typedef struct
{
	struct nk_context context;
	struct nk_font_atlas atlas;
	struct nk_font *font;
	struct nk_font *large_font;    // the same, bigger (large_scale times): the dialogs' lines
	float large_scale;
	struct nk_draw_null_texture null_texture;
	struct nk_buffer commands;
	struct nk_buffer vertices;
	struct nk_buffer elements;
	bool initialized;

	SDL_Window *window;
	float scale;                   // window points per interface unit
	float width;                   // the window, in interface units
	float height;

	SDL_GPUGraphicsPipeline *pipeline;
	// What is drawn in the world (commands whose user data is a depth:
	// ui_backend_world_depth), hidden behind the scene: the scene's depth
	// buffer for this frame (NULL: drawn as the rest)
	SDL_GPUGraphicsPipeline *depth_pipeline;
	// The two into the window's picture straight (its format; NULL: the same as ours)
	SDL_GPUGraphicsPipeline *window_pipeline;
	SDL_GPUGraphicsPipeline *window_depth_pipeline;
	SDL_GPUTexture *scene_depth;
	SDL_GPUBuffer *vertex_buffer;
	SDL_GPUBuffer *element_buffer;
	uint32_t vertex_buffer_bytes;
	uint32_t element_buffer_bytes;
	SDL_GPUTexture *font_texture;  // images are SDL_GPUTexture pointers in their nk_handle

	// From the last layout (see above)
	bool mouse_captured;
	bool keyboard_captured;
	// Set by the interface while it lays out a text field that has the
	// focus; the backend then asks SDL for text input
	bool wants_text_input;
	bool text_input_started;
} ui_backend_t;

// font_path is relative to the data directory; font_size is in interface
// units. Returns false (after logging) if the shaders or the font fail.
// large_scale: how much bigger the large font is (the dialogs')
bool ui_backend_create(ui_backend_t *backend, SDL_Window *window, const char *font_path, float font_size, float large_scale, float scale);

// The user data that marks what follows as drawn in the world at this
// depth (the scene's depth buffer's 0..1; nk_set_user_data); 0 for the
// rest again
nk_handle ui_backend_world_depth(float depth);
// The scene's depth buffer, for this frame's world-drawn commands (NULL:
// none; they are drawn as the rest)
void ui_backend_set_scene_depth(ui_backend_t *backend, SDL_GPUTexture *depth_texture);
void ui_backend_destroy(ui_backend_t *backend);

// Feeding the events of one frame: begin, every event, end. handle_event
// returns true if the interface keeps the event for itself.
void ui_backend_input_begin(ui_backend_t *backend);
bool ui_backend_handle_event(ui_backend_t *backend, const SDL_Event *event);
void ui_backend_input_end(ui_backend_t *backend);

// After the interface was laid out: works out what it captures next frame
// (see above) and starts or stops SDL's text input.
void ui_backend_update_capture(ui_backend_t *backend);

// Draws the interface over the frame's final picture (gpu.final_texture,
// pixel_width by pixel_height) in a pass of its own, then clears it for
// the next frame.
void ui_backend_render(ui_backend_t *backend, int pixel_width, int pixel_height);

// Loads a PNG (relative to the data directory) as an image the interface
// can draw. Returns false if it cannot be read; the image is then empty.
bool ui_backend_load_image(const char *path, struct nk_image *image_out);
void ui_backend_free_image(struct nk_image *image);

#endif
