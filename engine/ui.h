// The editor's and the testbed's interface, built with Nuklear (see
// ui_backend.h); what the player sees on foot is the HUD's (hud.h), drawn
// with the same backend and colors:
//
//   brushes      always on screen: the tools by group (terrain, water,
//                nature, villages), the brush size and the options of the
//                current tool. Movable, collapsible, see-through until the
//                pointer is over it.
//   icon bar     top right: settings, save, load, new map, pause and speed,
//                diagnostics, the status line.
//   status line  bottom: frame rate, speed, water, objects, the tool.
//   settings     tabs of sliders over settings_t. Cheap values change at
//                once; the ones that rebuild something (texture sizes,
//                shadow maps, vegetation density) wait for Apply; terrain
//                generation values are used by the next new map. Save keeps
//                them in settings_changed.ini (see settings.h).
//   diagnostics  what the GPU and the simulation spend, live.
//   inspector    everything about one cell, while the inspect tool is on
//                or a cell is pinned (see inspector.h).
//   maps         saving and loading maps (see map_file.h).
//   sky          the clouds' map from above, painted with the mouse: how
//                many clouds, how heavy, which kind (see cloud_paint.h).
//   console      across the top while open (` or ~): the log and a
//                line for commands (see console.h).
//
// data/ui.ini sets the font, the size of everything, the see-through
// amount, the colors and the icons. An icon is a PNG named for a button;
// a button without one shows its name. Where the windows are and which are
// open is kept in ui_state.ini in the preference directory.
//
// The interface does not change the world itself: tools and switches go
// through tool_state_t and input_state_t like the keys do, and everything
// bigger (saving, loading, new maps, rebuilding textures) is asked of the
// game in ui_requests_t.

#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#include "input.h"
#include "cloud_paint.h"
#include "console.h"
#include "inspector.h"
#include "layouts.h"
#include "map_file.h"
#include "object_types.h"
#include "particles.h"
#include "lights.h"
#include "settings.h"
#include "tools.h"
#include "ui_backend.h"
#include "water.h"

#define UI_ICONS_MAX 128
#define UI_DIAGNOSTIC_LINES 24
#define UI_DIAGNOSTIC_LINE_LENGTH 192

typedef struct
{
	char key[48];
	char path[160];
	struct nk_image image;
	bool tried;
	bool loaded;
} ui_icon_t;

#define UI_PICK_LINES 10

// What the interface asks of the game; cleared every frame
typedef struct
{
	bool save_map;
	bool load_map;
	char map_name[MAP_FILE_NAME_LENGTH];   // save: without the kind (map_baked says which); load: the file's
	bool map_baked;
	bool new_map;
	uint32_t new_map_seed;         // 0: a random one
	bool apply_render;             // textures, shadow maps, vsync
	bool apply_objects;            // vegetation density: grow it anew
	bool regrow;
	bool remove_objects;
	bool save_settings;
	bool restore_settings;         // back to the values of settings.ini
	bool quit;
} ui_requests_t;

// The state of the game the interface shows, for one frame
typedef struct
{
	settings_t *settings;
	tool_state_t *tools;
	input_state_t *input;          // for one-shot requests (pause, rain, ...)
	water_t *water;
	const object_types_t *object_types;
	const layouts_t *layouts;
	uint32_t seed;
	bool map_baked;                // a baked map (map_file.h): it saves baked only
	bool paused;
	float time_scale;
	float reached_time_scale;      // below time_scale when the machine cannot keep up
	int object_count;
	inspector_t *inspector;
	particles_t *particles;        // the effect tool's list (may be empty)
	lights_t *lights;              // the light tool's
	bool *objects_hidden;          // the World window: the objects not drawn (they stay, and go on, only unseen)
	float *fireflies;              // the World window: how many fireflies on dark nights (0..1)
	char *actor_name;              // the World window: what K gives the actors it puts (a name: 32 bytes; empty: none)
	int *actor_game_id;            // ...the game's number for them (0: none)
	bool *actor_hostile;           // ...enemies (the name in red)
	bool *no_terrain_occlusion;    // the World window: objects behind the terrain drawn all the same
	bool *impostors_tinted;        // the World window (debugging): impostors tinted
	bool *aurora_on;               // the World window: the northern lights
	bool *raining;                 // the World window: rain (water.raining)
	bool *rainbow_on;              // the World window: a rainbow (rain.h)
	float *month;                  // the World window: the year in the plants (render.month)
	float *snow_first_month;       // and the snow's months
	float *snow_last_month;
	float *tips_first_month;       // and the new growth's
	float *tips_last_month;
	float *bloom_first_month;
	float *bloom_last_month;
	bool *physics_off;             // the World window (debugging): physics stopped
	bool *far_shadow_off;          // the World window (debugging): the far shadow map off
	bool *near_shadow_off;         // ... the near one
	cloud_paint_t *cloud_paint;    // the painted sky (the sky window paints it)
	float sky_eye[2];              // where the eye is on the clouds' map (meters, wind taken off)
	double *day_hour;              // the time of day, 0..24 (the sky window sets it; NULL: no day cycle)
	bool *day_frozen;
	console_t *console;            // drawn while open (it edits itself, see console.h)
	// Alt held: what the ray under the pointer goes through, a line each
	int pick_line_count;
	char pick_lines[UI_PICK_LINES][96];
} ui_view_t;

typedef enum
{
	UI_MAPS_CLOSED = 0,
	UI_MAPS_SAVE,
	UI_MAPS_LOAD,
	UI_MAPS_NEW
} ui_maps_mode_t;

typedef struct
{
	ui_backend_t backend;
	bool available;
	SDL_Window *window;
	bool hidden;                   // F10: no interface at all (screenshots)

	// From ui.ini (and ui_state.ini)
	char font_path[160];
	float font_size;
	float dialog_text_scale;       // the dialogs' text, times font_size
	float scale;
	float panel_opacity;
	bool show_status_line;
	bool icon_labels;              // tool buttons with an icon show their name next to it
	struct nk_color colors[NK_COLOR_COUNT];
	ui_icon_t icons[UI_ICONS_MAX];
	int icon_count;

	// Windows
	struct nk_rect brushes_bounds;
	bool brushes_collapsed;
	bool brushes_placed;           // nk_begin ran once with the stored bounds
	bool settings_open;
	int settings_tab;
	char settings_search[64];      // the settings window's search: rows of every tab whose name has it
	struct nk_rect settings_bounds;
	bool diagnostics_open;
	struct nk_rect diagnostics_bounds;
	struct nk_rect inspector_bounds;
	ui_maps_mode_t maps_mode;
	struct nk_rect maps_bounds;
	bool sky_open;
	struct nk_rect sky_bounds;
	bool world_open;               // the World window: what is shown (objects, ...)
	struct nk_rect world_bounds;
	int sky_channel;               // cloud_paint_channel_t being painted
	float sky_brush_kilometers;    // the brush's radius
	float sky_brush_strength;      // the whole range in this many seconds at full: 1 / strength
	bool sky_brush_neutral;        // the brush brings back the settings' sky
	uint64_t sky_last_ticks;       // the last frame painted (ms)
	bool console_shown;            // the console was laid out this frame (drawn even with F10)
	bool state_loaded;

	// Settings that wait for Apply
	bool render_pending;
	bool objects_pending;
	bool interface_pending;
	float pending_scale;
	float pending_font_size;
	float pending_dialog_text_scale;

	// Maps window
	char map_name_input[MAP_FILE_NAME_LENGTH];
	bool save_baked;               // the save window's "Baked" box
	bool save_locked;              // ...held on (the map is baked)
	char seed_input[16];
	map_file_entry_t map_entries[MAP_FILE_LIST_MAX];
	int map_entry_count;
	int selected_map;
	bool confirm_overwrite;
	bool confirm_delete;
	char last_map_name[MAP_FILE_NAME_LENGTH];
	char message[160];             // the result of the last save or load
	double message_until_seconds;

	// Filled in by the game
	char status_text[512];
	char diagnostics[UI_DIAGNOSTIC_LINES][UI_DIAGNOSTIC_LINE_LENGTH];
	int diagnostic_count;

	ui_requests_t requests;
} ui_t;

// Reads ui.ini and ui_state.ini and starts Nuklear. Returns false if the
// interface cannot work (the game then runs with the keys alone).
bool ui_create(ui_t *ui, SDL_Window *window);
// Keeps ui_state.ini and releases everything
void ui_destroy(ui_t *ui);

void ui_input_begin(ui_t *ui);
// True if the interface keeps the event (see ui_backend.h)
bool ui_handle_event(ui_t *ui, const SDL_Event *event);
void ui_input_end(ui_t *ui);

// Lays out the interface for this frame; afterwards ui->requests says
// what it asks of the game. time_seconds is the game's clock (for
// messages that fade).
void ui_update(ui_t *ui, const ui_view_t *view, double time_seconds);

void ui_render(ui_t *ui, int pixel_width, int pixel_height);

// The loading screen: a box in the middle with what is being done and how
// far along (0..1), drawn over whatever is in the frame's picture (the
// caller clears it)
void ui_show_loading(ui_t *ui, int pixel_width, int pixel_height, const char *what, float fraction);

// A short message at the bottom of the screen (saved, loaded, failed)
void ui_show_message(ui_t *ui, const char *message, double time_seconds);

// Escape: closes the topmost open window (hud_close_windows first); false
// if none was open
bool ui_close_windows(ui_t *ui);

// The game finished a request: remembered for the save window's name field
void ui_set_map_name(ui_t *ui, const char *name);

// Rebuilds the backend (new interface scale or font size). Returns false
// if that failed; the interface is then off.
bool ui_rebuild(ui_t *ui);

#endif
