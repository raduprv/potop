// The settings window: the settings (settings.h) by tabs, a row each,
// with a search over all of them; cheap values change at once, those that
// rebuild something (textures, shadow maps, the vegetation) wait for Apply,
// the new map's are used by the next new map; Save keeps them in
// settings_changed.ini. The game's and the editor's (a row the game does not
// show is the editor's only).

#ifndef SETTINGS_WINDOW_H
#define SETTINGS_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"
#include "ui_backend.h"
#include "ui_style.h"

// What the window asks of its application; cleared every frame
typedef struct
{
	bool apply_render;             // textures, shadow maps, vsync
	bool apply_objects;            // vegetation density: grow it anew
	bool regrow;
	bool remove_objects;
	bool save_settings;
	bool restore_settings;         // back to the values of settings.ini
	bool new_map;
	uint32_t new_map_seed;         // 0: a random one
	bool toggle_water_model;       // the depth scaled pipes on or off
	bool bake_sky_occlusion;
} settings_window_requests_t;

// What it shows besides the settings, for one frame
typedef struct
{
	bool editor;                   // every row (else not the editor's only)
	bool depth_scaled_pipes;       // the water's, now
	uint32_t seed;                 // this map's
	// More rows at the end of the interface tab (NULL: none)
	void (*interface_extra)(void *user, struct nk_context *context);
	void *user;
} settings_window_view_t;

typedef struct
{
	bool open;
	int tab;
	char search[64];               // rows of every tab whose name has it
	struct nk_rect bounds;
	bool render_pending;
	bool objects_pending;
	bool interface_pending;
	float pending_scale;           // the interface's, until Apply
	float pending_font_size;
	float pending_dialog_text_scale;
	char seed_input[16];
	settings_window_requests_t requests;
	// Given with each draw
	struct nk_context *context;
	ui_backend_t *backend;
	ui_style_t *style;
} settings_window_t;

// Closed, where it opens, the interface's values as the style has them
void settings_window_init(settings_window_t *window, const ui_style_t *style);
// Lays it out (while open); afterwards window->requests says what it asks.
// The interface's values go into style on Apply (the backend is then made
// again by the application: its scale, its font)
void settings_window_draw(settings_window_t *window, ui_backend_t *backend, ui_style_t *style, settings_t *settings,
	const settings_window_view_t *view);

#endif
