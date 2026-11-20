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
//   maps         map projects: new, save or bake, open (see project.h); maps
//                saved before projects brought in as one, or just loaded.
//   sketch       over the whole screen: a new map's land painted roughly
//                from above (mountains here, a plain there, a lake), the
//                land it makes shown as it is painted; previewed in 3D a
//                quarter as big, made into a project (terrain_sketch.h).
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
#include "project.h"
#include "sketch_view.h"
#include "terrain_sketch.h"
#include "object_types.h"
#include "particles.h"
#include "lights.h"
#include "settings.h"
#include "tools.h"
#include "ui_backend.h"
#include "ui_style.h"
#include "settings_window.h"
#include "water.h"
#include "areas.h"
#include "terrain.h"
#include "math3d.h"

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
	// Map projects (project.h): project_name the one asked (new, open,
	// save as, import into); the seed for a new one (0: a random one)
	bool project_new;
	bool project_open;
	bool project_save;
	bool project_save_as;
	bool project_bake;
	bool project_import;           // a saved map (import_map, its file name) as a new project
	char project_name[PROJECT_NAME_LENGTH];
	uint32_t project_seed;
	char import_map[MAP_FILE_NAME_LENGTH];
	// The sketch window's: its land in 3D a quarter as big (outside any
	// project), or made into the project ui_t.sketch_name_input names (its
	// map replaced if there is one); the map's size and the rest as ui_t's
	// sketch_* say
	bool sketch_preview;
	bool sketch_generate;
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
	const char *project_name;      // the project open ("" none)
	bool paused;
	float time_scale;
	float reached_time_scale;      // below time_scale when the machine cannot keep up
	int object_count;
	inspector_t *inspector;
	particles_t *particles;        // the effect tool's list (may be empty)
	lights_t *lights;              // the light tool's
	areas_t *areas;                // the area tool's (drawn while it is chosen)
	const terrain_t *terrain;      // the areas drawn on it
	mat4_t view_projection;        // the camera's, for drawing them
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
	terrain_sketch_t *sketch;      // the new map's sketch (the sketch window paints it)
	sketch_view_t *sketch_view;    // its picture
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

	// From ui.ini (and ui_state.ini): the look (shared with the HUD), and
	// the editor's own
	ui_style_t style;
	bool show_status_line;
	bool icon_labels;              // tool buttons with an icon show their name next to it
	ui_icon_t icons[UI_ICONS_MAX];
	int icon_count;

	// Windows
	struct nk_rect brushes_bounds;
	bool brushes_collapsed;
	bool brushes_placed;           // nk_begin ran once with the stored bounds
	settings_window_t settings;     // the settings (settings_window.h)
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

	// Sketch window (terrain_sketch.h): open over everything else
	bool sketch_open;
	bool sketch_known;             // its fields filled in (when first opened)
	int sketch_brush;              // a preset (sketch_presets), SKETCH_PRESET_COUNT custom, + 1 erase
	int sketch_custom_plane;       // custom: 0 height, 1 relief, 2 water
	float sketch_custom_value;     // ...to this (0..1)
	float sketch_brush_meters;     // the brush's radius
	float sketch_brush_strength;   // how fast it paints (all the way in 1 / this seconds, at its middle)
	uint64_t sketch_last_ticks;
	char sketch_name_input[PROJECT_NAME_LENGTH];
	char sketch_seed_input[16];
	int sketch_size_index;         // 1024, 2048, 4096, 8192 cells a side
	float sketch_cell_meters;
	float sketch_max_height;       // meters
	float sketch_erosion;          // droplets a texel (terrain_settings_t.erosion_droplets)
	bool sketch_confirm_replace;
	bool sketch_new_pending;       // opened as New (the command line's): its fields afresh when it opens
	bool state_loaded;

	// Maps window
	char seed_input[16];
	map_file_entry_t map_entries[MAP_FILE_LIST_MAX];
	int map_entry_count;
	int selected_map;
	bool confirm_delete;
	char last_map_name[MAP_FILE_NAME_LENGTH];
	project_info_t projects[PROJECT_LIST_MAX];
	int project_count;
	int selected_project;
	char project_name_input[PROJECT_NAME_LENGTH];
	bool confirm_project_delete;
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

// A short message at the bottom of the screen (saved, loaded, failed)
void ui_show_message(ui_t *ui, const char *message, double time_seconds);

// Escape: closes the topmost open window (hud_close_windows first); false
// if none was open
bool ui_close_windows(ui_t *ui);

// New: the sketch window, its fields afresh (a new name and seed, a blank
// sheet)
void ui_open_new_map(ui_t *ui, const ui_view_t *view);

// The sketch window's map: cells a side (its size choice)
int32_t ui_sketch_size(const ui_t *ui);
// Its seed (typed; 0 if none yet)
uint32_t ui_sketch_seed(const ui_t *ui);

// The game finished a request: remembered for the save window's name field
void ui_set_map_name(ui_t *ui, const char *name);

// Rebuilds the backend (new interface scale or font size). Returns false
// if that failed; the interface is then off.
bool ui_rebuild(ui_t *ui);

#endif
