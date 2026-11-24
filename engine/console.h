// The console, Quake style: ` (or ~) opens and closes it.
//
// It keeps the log (everything SDL_Log writes, kept as it goes to the
// terminal too) and a line to type commands into. The line is edited here,
// from the SDL events the application passes on while it is open, not by
// the interface: the interface only draws it (console_draw). A command
// typed and sent with Enter waits in console_take_command for the
// application to run it (console_run_command: the same commands in the
// game and the editor); what it answers goes through SDL_Log like
// everything else.
//
// Keys while open:
//   Enter             run the line
//   Up / Down         the commands typed before
//   Tab               completes a command or a setting (section.key);
//                     several fit: the common part, and the list
//   Page Up / Down    scroll the log (the wheel too)
//   Ctrl+L            clear the log
//   Ctrl+U            clear the line
//   Ctrl+V            paste
//   Escape, ` or ~    close
//
// The same commands will be what the game's API speaks later.

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "ui_backend.h"
#include "ui_style.h"

// Log categories for a line that goes one way only: into the game's
// console at once, to the terminal (or the file) later. The stutters'
// lines: the terminal drawing a line makes the game wait a frame, so those
// are written there together now and then (profile.c), but shown in the
// game as they come
#define CONSOLE_LOG_GAME_ONLY (SDL_LOG_CATEGORY_CUSTOM + 1)
#define CONSOLE_LOG_TERMINAL_ONLY (SDL_LOG_CATEGORY_CUSTOM + 2)

#define CONSOLE_LINES 1024
#define CONSOLE_LINE_LENGTH 240
#define CONSOLE_INPUT_LENGTH 240
#define CONSOLE_HISTORY 64

typedef struct
{
	const char *name;
	const char *usage;           // the arguments, for help
	const char *help;            // one line
} console_command_t;

#define CONSOLE_ADDED_MAX 64

// A command the game or the editor adds (console_add_command): it gets the
// rest of the line
typedef void (*console_command_function_t)(void *user, const char *arguments);

typedef struct
{
	char name[32];
	char usage[64];
	char help[128];
	console_command_function_t function;
	void *user;
} console_added_command_t;

// What a command asks of the application (console_run_command): it does
// it, with its own words to the player
typedef struct
{
	bool apply_render;             // apply: the settings that rebuild
	bool load_map;
	bool save_map;
	bool map_baked;                // save: baked
	char map_name[64];
	bool screenshot;
	bool quit;
} console_actions_t;

typedef struct
{
	bool open;

	// The log: a ring of lines, written from any thread
	SDL_Mutex *mutex;
	char lines[CONSOLE_LINES][CONSOLE_LINE_LENGTH];
	SDL_LogPriority priorities[CONSOLE_LINES];
	uint32_t line_count;         // ever written (the ring holds the last CONSOLE_LINES)
	int32_t scroll_lines;        // how far up from the newest line the view is

	// The line being typed
	char input[CONSOLE_INPUT_LENGTH];
	int32_t input_length;        // bytes
	int32_t cursor;              // byte offset in input

	char history[CONSOLE_HISTORY][CONSOLE_INPUT_LENGTH];
	int32_t history_count;
	int32_t history_index;       // -1: not browsing; else the entry shown
	char history_draft[CONSOLE_INPUT_LENGTH]; // what was typed before browsing

	// Sent with Enter, until the game takes it
	char command[CONSOLE_INPUT_LENGTH];
	bool command_ready;

	// The log function that was there before (the terminal)
	SDL_LogOutputFunction previous_output;
	void *previous_userdata;
	bool hooked;
	bool terminal_colors;          // the log written in color (a terminal)
	bool terminal;                 // the log goes to a terminal: each line with the seconds since the start

	// The commands added by the game or the editor
	console_added_command_t added[CONSOLE_ADDED_MAX];
	int added_count;
} console_t;

// The engine's commands (console_run_command), for help and Tab
extern const console_command_t console_commands[];
extern const int console_command_count;

// Starts keeping the log; call as early as possible
void console_create(console_t *console);
void console_destroy(console_t *console);

void console_set_open(console_t *console, bool open);

// An event while the console is open. True if the console took it (the
// game and the interface then do not see it)
bool console_handle_event(console_t *console, const SDL_Event *event);

// A line for the log without going through SDL_Log (not to the terminal)
void console_print(console_t *console, const char *format, ...) SDL_PRINTF_VARARG_FUNC(2);

void console_clear(console_t *console);

// The command sent with Enter, once; false if there is none
bool console_take_command(console_t *console, char *command_out, size_t command_out_size);

struct potop_s;

// Runs a line (the engine's commands, then the added ones); what it asks
// of the application in actions_out
void console_run_command(console_t *console, struct potop_s *potop, const char *line, console_actions_t *actions_out);
// A command of the game's or the editor's own, for help and Tab too; false
// if there is no room for more
bool console_add_command(console_t *console, const char *name, const char *usage, const char *help, console_command_function_t function, void *user);
// An added command by its name, run with the rest of the line; false if
// there is none
bool console_run_added(console_t *console, const char *command, const char *arguments);

// Lays it out across the top while open (the backend's frame); false if
// it is closed
bool console_draw(console_t *console, ui_backend_t *backend, const ui_style_t *style);

// For drawing: copies the line count_from_newest lines back (0: the newest)
// into text_out; false past the oldest kept line
bool console_line(console_t *console, int32_t count_from_newest, char *text_out, size_t text_out_size, SDL_LogPriority *priority_out);

#endif
