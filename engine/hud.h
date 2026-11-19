// What the player sees on foot, built with Nuklear (see ui_backend.h):
//
//   compass      across the top: north, east, south, west turning with the
//                eye, the markers as dots (the active one: how far).
//   notices      what the game says for a few seconds (hud_notify), stacked
//                under the compass, fading.
//   dialog       a conversation the game holds (dialog.h): who talks, the
//                line, the answers (a click or 1..9).
//   journal      J: the quest log (journal.h) the game fills; the player
//                tracks quests, puts an objective in focus, sees it on the
//                map.
//   map          M: the world map (world_map.h) with the markers and the
//                player; the wheel zooms, a drag moves it, a right click
//                puts a marker of one's own (or takes it away).
//   inventory    I: what the player carries, a list with a sign for each
//                category, and the body's slots (see inventory.h).
//   loot         what a container holds (a dead animal's, a chest's) next to
//                the player's: a click moves a row, Take all moves all.
//   prompt       what E would do with what the eye is on.
//   names        over the named actors near the eye (red: enemies).
//
// The HUD shows what the game gives it each frame (hud_view_t) and draws
// into the interface's backend (the application's, with its font, scale and
// colors); what the player asks of the game comes back in hud_requests_t.
// Which windows are open is the application's to say too (the keys that
// open them are its).

#ifndef HUD_H
#define HUD_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#include "dialog.h"
#include "inventory.h"
#include "journal.h"
#include "markers.h"
#include "ui_backend.h"

#define HUD_NAMEPLATES_MAX 16
#define HUD_NOTICES_MAX 6
#define HUD_DEBUG_LINES 24
#define HUD_DEBUG_LINE_LENGTH 96

// A name over an actor's head, where the screen shows it
typedef struct
{
	float screen_x;                // -1..1 across, -1..1 up (clip space)
	float screen_y;
	float alpha;                   // fades with the distance
	float depth;                   // as the scene's depth buffer has it there (what stands nearer hides it)
	bool hostile;
	char name[32];
} hud_nameplate_t;

// How a notice looks (hud_notify)
typedef enum
{
	HUD_NOTIFY_INFO = 0,
	HUD_NOTIFY_QUEST,              // gold: a quest's news
	HUD_NOTIFY_WARNING             // red
} hud_notify_style_t;

typedef struct
{
	char text[160];
	hud_notify_style_t style;
	double start_seconds;
	double until_seconds;
} hud_notice_t;

// What the player asks of the game; cleared every frame
typedef struct
{
	int drop_item;                 // the inventory's Drop: this item (-1: none), this many
	int drop_count;
	bool map_marker;               // a right click on the map: a marker of one's own there, or the one there away
	float map_marker_x;
	float map_marker_z;
} hud_requests_t;

// A shape drawn in the world (the navigation mesh's overlay): its corners
// on the screen (pixels), its depth as the scene's depth buffer has it
// (what stands nearer hides it), its color; a line is two corners and a
// thickness
typedef struct
{
	float points[8];                   // x, y of up to four corners
	uint8_t count;                     // 4: a filled shape; 2: a line
	uint8_t color[4];
	float thickness;                   // a line's, pixels
	float depth;
	char label[12];                    // written at its first corner (empty: none)
} hud_world_shape_t;

// What the HUD shows, for one frame
typedef struct
{
	// The inventories (NULL: none): the player's container (the one being
	// looted is hud_t.loot_container)
	inventories_t *inventories;
	uint32_t player_inventory;
	// What E would do with what the eye is on ("[E] Loot Bear"; empty:
	// nothing)
	const char *interaction_prompt;
	// On foot (the compass, the map, the names): where the eye is, which
	// way it looks (degrees: 0 north (-Z), 90 east (+X))
	bool on_foot;
	float eye_x;
	float eye_z;
	float heading_degrees;
	markers_t *markers;
	journal_t *journal;            // NULL: none
	dialog_t *dialog;              // NULL: none
	hud_nameplate_t nameplates[HUD_NAMEPLATES_MAX];
	int nameplate_count;
	// Shapes drawn in the world, under everything else of the HUD (NULL: none)
	const hud_world_shape_t *world_shapes;
	int world_shape_count;
	// The world map's picture (NULL: none) and how many meters it spans
	SDL_GPUTexture *map_texture;
	int map_texture_width;
	int map_texture_height;
	float map_extent_meters;
	bool map_from_artist;
} hud_view_t;

typedef struct
{
	bool inventory_open;           // I: the player's inventory
	struct nk_rect inventory_bounds;
	int inventory_selected;        // its row (-1: none)
	uint32_t loot_container;       // the container being looted (0: the window is closed)
	struct nk_rect loot_bounds;
	int inventory_last_click_row;  // a double click: the row clicked last, when (ms)
	uint64_t inventory_last_click_ticks;
	bool map_open;                 // M: the world map
	float map_zoom;                // 1: the whole map fits
	float map_center_x;            // meters: the map's point in the window's middle
	float map_center_z;
	bool map_centered;             // centered on the player once opened
	bool journal_open;             // J: the quest log
	struct nk_rect journal_bounds;
	char journal_selected[JOURNAL_ID_LENGTH];
	hud_notice_t notices[HUD_NOTICES_MAX];
	int notice_count;
	double now_seconds;            // the game's clock at the last update
	// The game's debug window (F3 in the game): its lines, as the game
	// gave them this frame
	bool debug_open;
	char debug_lines[HUD_DEBUG_LINES][HUD_DEBUG_LINE_LENGTH];
	int debug_line_count;
	dialog_t *dialog;              // the view's (Escape closes it)

	// Given with each update
	ui_backend_t *backend;
	const struct nk_color *colors; // NK_COLOR_COUNT of them

	hud_requests_t requests;
} hud_t;

// Everything closed, the windows where they start
void hud_init(hud_t *hud);

// Lays out the HUD for this frame into the backend (before the backend's
// render; NULL: nothing shown, F10); afterwards hud->requests says what the
// player asks. time_seconds is the game's clock (the notices fade by it).
void hud_update(hud_t *hud, ui_backend_t *backend, const struct nk_color *colors, const hud_view_t *view, double time_seconds);

// What the game says for that many seconds, under the compass (the oldest
// goes when there are too many)
void hud_notify(hud_t *hud, const char *text, float seconds, hud_notify_style_t style);

// The map opened on a place (meters), closer in
void hud_show_map_at(hud_t *hud, float x, float z);

// Escape: the conversation ends, or the topmost open window closes (only
// while shown); false if there was nothing
bool hud_close_windows(hud_t *hud, bool shown);

#endif
