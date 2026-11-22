// Input as actions.
//
// SDL events are translated into game-level actions here and nowhere else:
// "rotate the camera by this much", "apply the current tool", "add water".
// The rest of the game only looks at input_state_t, so adding touch
// gestures for Android later means changing this file only.
//
// Current desktop mapping:
//   left mouse           apply the current terrain tool
//   shift + left mouse   add water
//   ctrl + left mouse    sponge (remove water)
//   right mouse drag     rotate camera (yaw and pitch)
//   middle mouse drag    pan camera
//   mouse wheel          zoom
//   W A S D / arrows     pan camera
//   Q / E                rotate camera
//   1 2 3 4              terrain tool: raise, dig, paint soil, smooth
//   5 6 7 8              soil: sand, earth, clay, rock
//   T                    stamp tool; again: next stamp (crater, dam, hill,
//                        channel, lake, plateau). Dam and channel are
//                        dragged from one end to the other.
//   G                    plant tool; again: next category (trees, bushes,
//                        grass, flowers, rocks, ...)
//   H                    clear tool: removes the objects under the brush
//   O / shift + O        place one object of the plant category under the
//                        cursor / remove the nearest object
//   K / shift + K        place an actor (the first in actors.ini) under the
//                        cursor, facing the camera / remove the nearest actor
//   J / shift + J        every actor to its next / previous action (debug)
//   V / shift + V        grow the vegetation of the map anew / remove all objects
//   L                    layout tool; again: next layout (homestead, camp, ...);
//                        click places it
//   delete               remove the layout nearest the cursor
//   - =                  stamp height
//   [ ]                  brush radius
//   F                    place or remove a spring (fountain) under the cursor
//   R                    rain on / off
//   B                    map borders: drain / wall
//   M                    water model: depth scaled pipes / classic fixed pipes
//   N                    new terrain with a random seed
//   space / P            pause (in first person, space jumps; P pauses)
//   C                    first person on foot / back to the orbit camera
//                        (W A S D walk, mouse looks, shift runs, space jumps
//                        or swims up, ctrl dives (swimming forward also
//                        dives or rises where the walker looks); tools work at the
//                        middle of the screen; escape leaves it too)
//   , .                  slower / faster simulation
//   F1                   wireframe (desktop GL only)
//   F2                   debug view (soil types)
//   F3                   overlay of the water tiles: active, active with mud, sleeping
//   F4                   log the simulation diagnostics now
//   F6                   thin water highlight (debugging)
//   F7                   stop / start the water simulation (the physics keeps running)
//   F8                   navigation overlay: where the first actor type cannot go, and why;
//                        the actors' paths
//   F5 / F9              quick save / quick load of the map
//   F10                  hide or show the interface
//   F12                  screenshot
//   escape               close the open windows of the interface
//   ` / ~                the console (main.c and console.c: it takes the
//                        keyboard while open)
//   tab                  on foot: let the pointer out for the interface and
//                        take it back (main.c)
//   ctrl+Q               quit
//   F11                  full screen on and off

#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <SDL3/SDL.h>

typedef enum
{
	WATER_ACTION_NONE = 0,
	WATER_ACTION_ADD,
	WATER_ACTION_REMOVE
} water_action_t;

typedef struct
{
	// Continuous actions accumulated over the current frame
	float rotate_drag_x_pixels;
	float rotate_drag_y_pixels;
	float pan_drag_x_pixels;
	float pan_drag_y_pixels;
	float pan_keyboard_x;          // -1..1, right is positive
	float pan_keyboard_y;          // -1..1, forward is positive
	float rotate_keyboard;         // -1..1
	float zoom_steps;              // positive zooms in
	// First person: mouse movement (any button), and held keys
	float look_x_pixels;
	float look_y_pixels;
	bool first_person;             // set by the game: space jumps instead of pausing
	bool jump_pressed;             // space went down this frame (first person)
	bool jump_held;
	bool run_held;                 // shift
	bool dive_held;                // ctrl
	bool dive_pressed;             // ctrl went down this frame (on dry ground: crawling on or off)
	bool pick_held;                // alt: highlight and list what is under the pointer

	// Pointer used for picking and tools, in window points
	float pointer_x;
	float pointer_y;
	bool pointer_in_window;
	// The pointer is over the user interface: the tools do not see it
	bool pointer_over_ui;
	bool left_button_down;         // from the events the interface did not take
	bool right_button_down;        // the same (first person, a weapon up: aiming)
	bool right_clicked;            // let go this frame, the pointer barely moved while it was down (not a turn of the camera)
	float right_drag_pixels;       // how far it moved while down
	bool keyboard_captured;        // a text field of the interface has the keyboard
	// Held while the left button is down: either the terrain tool or, with
	// a modifier, a water action
	bool apply_tool_held;
	bool apply_tool_pressed;       // went down this frame
	bool apply_tool_released;      // went up this frame
	water_action_t water_action;

	// One-shot requests (reset every frame)
	int select_tool;               // -1 if none
	int select_soil;               // -1 if none
	int brush_size_steps;
	int stamp_height_steps;
	bool cycle_stamp;
	bool place_object;             // O: put one object under the cursor
	bool remove_object;            // Shift+O: remove the object nearest the cursor
	bool place_actor;              // K: put an actor under the cursor
	bool cycle_actor_type;         // Ctrl+K: the next type K puts
	bool remove_actor;             // Shift+K: remove the actor nearest the cursor
	int cycle_actor_action;        // J: 1, Shift+J: -1, else 0
	bool cycle_plant;              // G: plant tool, next category
	bool select_clear;             // H: clear tool
	bool request_repopulate;       // V: vegetation anew
	bool request_remove_objects;   // Shift+V: no objects at all
	bool cycle_layout;             // L: layout tool, next layout
	bool request_remove_layout;    // Delete: the layout nearest the cursor
	int time_scale_steps;
	bool request_regenerate;
	bool request_screenshot;
	bool toggle_spring;
	bool toggle_rain;
	bool toggle_borders;
	bool bake_sky_occlusion;       // U: the sky's occlusion worked out anew
	bool toggle_water_model;
	bool toggle_pause;
	bool toggle_wireframe;
	bool toggle_debug_view;
	bool toggle_tile_overlay;
	bool toggle_thin_highlight;    // F6
	bool toggle_water_freeze;      // F7
	bool toggle_navigation_overlay; // F8
	bool toggle_first_person;      // C
	bool request_diagnostics;
	bool request_quick_save;       // F5
	bool request_quick_load;       // F9
	bool toggle_interface;         // F10: hide or show the interface
	bool toggle_fullscreen;        // F11
	bool toggle_hands;             // 0: on foot, put the tool away (no brush), or take it back
	bool interact;                 // E: on foot, what the eye is on (loot, pick up)
	bool toggle_inventory;         // I: on foot, the inventory
	bool request_escape;           // escape: lets go of the actor, leaves first person, closes the interface's windows
	bool request_quit;
	bool focus_gained;             // the window got the keyboard focus back this frame
	bool in_background;            // the window has no keyboard focus, or is minimized
} input_state_t;

void input_initialize(input_state_t *input);

// Clears the per-frame accumulators; call before polling events.
void input_begin_frame(input_state_t *input);

// Feeds one SDL event into the action state.
void input_handle_event(input_state_t *input, const SDL_Event *event);

// Samples held keys and buttons; call after all events of the frame.
void input_end_frame(input_state_t *input);

#endif
