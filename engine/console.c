// The console. See console.h.

#include "console.h"
#include "settings.h"

#include <stdarg.h>

const console_command_t console_commands[] =
{
	{ "help", "[command]", "the commands, or one of them" },
	{ "list", "[part of a name]", "the settings and their values (those whose name has the part)" },
	{ "get", "<setting>", "a setting's value (section.key, or the key alone where it is the only one)" },
	{ "set", "<setting> <value>", "changes a setting, as settings.ini writes it (true/false, 1.5, 255, 128, 0)" },
	{ "apply", "", "rebuilds what waits for Apply in the render settings (texture sizes, shadow maps)" },
	{ "where", "", "where the eye is and where it looks" },
	{ "goto", "<x> <z> [height]", "goes to a point of the map (meters; on foot: stands there)" },
	{ "look", "<yaw> [pitch]", "turns the eye (degrees, as where shows them; pitch up is positive)" },
	{ "water", "[x z]", "the water simulation at x z, or where the middle of the screen looks: the cell, its tile, and 5 x 5 cells round it (depth, walls)" },
	{ "cell", "<actor type> <x> <z>", "what an actor of that type finds at x z for its ways (room, what takes it, water, steepness)" },
	{ "bake", "paths", "all the navigation built now (every region, the rectangles for every type of actor): no search waits for a build after" },
	{ "log", "paths", "the walkers' ways into the log, or no more: each search and its points, where one is stuck and what is there" },
	{ "navmesh", "", "the navigation mesh: its tiles, rectangles, portals, memory, the last build and search" },
	{ "path", "<actor type> <x> <z>", "the way an actor of that type would walk from where you stand to x z (its points)" },
	{ "module", "[state]", "the module (a building whose parts move: a mill, a quarry's crane) nearest where you look or stand, within 40 m: its states; with one, sent to it" },
	{ "camera", "<x> <y> <z> [<at x> <at y> <at z>] | off", "first person: the eye put there (meters, y its height, even in the ground), looking at the point if given; off: back to the walker" },
	{ "distance", "<meters>", "how far the eye is from what it looks at (the wheel's zoom)" },
	{ "time", "[speed]", "how fast the sky's clock goes: clouds, wind, their life (1: as it is, 0: still)" },
	{ "hour", "[0..24]", "the time of day, or sets it (18.5: half past six in the evening)" },
	{ "freeze", "", "stops the day at this hour, or lets it go on" },
	{ "profile", "[frame [frames]]", "frame: the next frame (or the one that many frames later) step by step, each step's ms; else where the frames' time went on the CPU since the last profile, ms a frame (acquire: waiting for the GPU)" },
	{ "noclip", "", "on foot: flies through walls and everything, where you look (Space up, Ctrl down, Shift fast); again: walks" },
	{ "objects", "impostors [all|auto|show|hide] | occlusion [on|off|show|depth|all|hide] | relief [on|off] | age [0..1|off] | windows [auto|on|off]", "impostors all: every object that has one drawn as its impostor, even near (auto: only far away); show: impostors tinted magenta (hide: not); occlusion off: what the last frame's depth hid (terrain, houses) drawn all the same; show: the boxes it hid outlined in red, the pyramid's texels read for each in yellow; depth: the pyramid in gray under them too; all: those it let be drawn too, in green; hide: none; relief off: a thatch drawn flat, without its parallax; age: every building as old as given (0 new .. 1 the oldest: streaks, worn edges, moss), off: each its type's again; windows: the houses' windows lit by the hour (auto), all lit (on) or all dark (off)" },
	{ "aurora", "[on|off] [strength]", "the northern lights: on (shown when it is dark) or off" },
	{ "rain", "[on|off] [intensity]", "rain: on (with render.rain_intensity, 0..1) or off" },
	{ "rainbow", "[on|off] [strength]", "a rainbow: on (shown when the sun is low, opposite it) or off" },
	{ "moon", "[age]", "the moon's age in days (0 new, 14.75 full of 29.5), or sets it" },
	{ "decal", "<kind> [size] | clear", "a decal (data/decals.ini) where the middle of the screen looks; clear: all gone" },
	{ "torch", "[on|off]", "a torch in the player's hand (on foot; it lights as much as the night lets it)" },
	{ "lights", "[on|off|auto]", "the lamps (lanterns, candles, placed lights): all lit, all out, or lit at dusk and out at dawn" },
	{ "paint", "<coverage|heaviness|kind|flocks> <0..1> [km]", "paints the whole sky (as the Sky window; 0.5 is normal, flocks: 0 none); with km, a brush that far around you, adding (or taking away, below 0) that much" },
	{ "maps", "", "the saved maps (newest first), by the names load takes" },
	{ "load", "<name>", "loads a saved map (name, name.active or name.baked; without the kind: the active one if there is one)" },
	{ "save", "<name>", "saves the map as its kind: name.active, or name.baked for a baked map" },
	{ "bake", "<name>", "bakes the map (for play) as name.baked" },
	{ "screenshot", "", "a screenshot (as F12)" },
	{ "clear", "", "clears the log" },
	{ "quit", "", "leaves the game" },
};

const int console_command_count = (int)SDL_arraysize(console_commands);

// ---------------------------------------------------------------------------
// The log

static void add_line(console_t *console, SDL_LogPriority priority, const char *text, size_t length)
{
	uint32_t slot = console->line_count % CONSOLE_LINES;
	size_t kept = SDL_min(length, (size_t)CONSOLE_LINE_LENGTH - 1);

	SDL_memcpy(console->lines[slot], text, kept);
	console->lines[slot][kept] = '\0';
	console->priorities[slot] = priority;
	console->line_count++;
	// Looking back: the view stays on the same lines
	if (console->scroll_lines > 0)
	{
		console->scroll_lines = SDL_min(console->scroll_lines + 1, CONSOLE_LINES - 1);
	}
}

// A message can have several lines, and lines longer than a slot go on in
// the next one
static void add_message(console_t *console, SDL_LogPriority priority, const char *message)
{
	const char *start = message;

	if (console->mutex != NULL)
	{
		SDL_LockMutex(console->mutex);
	}
	while (start != NULL)
	{
		const char *end = SDL_strchr(start, '\n');
		size_t length = end != NULL ? (size_t)(end - start) : SDL_strlen(start);

		if (end == NULL && length == 0 && start != message)
		{
			break;
		}
		do
		{
			size_t piece = SDL_min(length, (size_t)CONSOLE_LINE_LENGTH - 1);

			add_line(console, priority, start, piece);
			start += piece;
			length -= piece;
		}
		while (length > 0);
		start = end != NULL ? end + 1 : NULL;
	}
	if (console->mutex != NULL)
	{
		SDL_UnlockMutex(console->mutex);
	}
}

static void SDLCALL log_output(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
	console_t *console = userdata;

	if (console->previous_output != NULL)
	{
		console->previous_output(console->previous_userdata, category, priority, message);
	}
	add_message(console, priority, message);
}

void console_create(console_t *console)
{
	SDL_zerop(console);
	console->history_index = -1;
	console->mutex = SDL_CreateMutex();
	SDL_GetLogOutputFunction(&console->previous_output, &console->previous_userdata);
	SDL_SetLogOutputFunction(log_output, console);
	console->hooked = true;
}

void console_destroy(console_t *console)
{
	if (console->hooked)
	{
		SDL_SetLogOutputFunction(console->previous_output, console->previous_userdata);
		console->hooked = false;
	}
	if (console->mutex != NULL)
	{
		SDL_DestroyMutex(console->mutex);
		console->mutex = NULL;
	}
}

void console_print(console_t *console, const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	SDL_vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	add_message(console, SDL_LOG_PRIORITY_INFO, text);
}

void console_clear(console_t *console)
{
	if (console->mutex != NULL)
	{
		SDL_LockMutex(console->mutex);
	}
	console->line_count = 0;
	console->scroll_lines = 0;
	if (console->mutex != NULL)
	{
		SDL_UnlockMutex(console->mutex);
	}
}

bool console_line(console_t *console, int32_t count_from_newest, char *text_out, size_t text_out_size, SDL_LogPriority *priority_out)
{
	bool found = false;

	if (console->mutex != NULL)
	{
		SDL_LockMutex(console->mutex);
	}
	if (count_from_newest >= 0 && (uint32_t)count_from_newest < SDL_min(console->line_count, (uint32_t)CONSOLE_LINES))
	{
		uint32_t slot = (console->line_count - 1u - (uint32_t)count_from_newest) % CONSOLE_LINES;

		SDL_strlcpy(text_out, console->lines[slot], text_out_size);
		if (priority_out != NULL)
		{
			*priority_out = console->priorities[slot];
		}
		found = true;
	}
	if (console->mutex != NULL)
	{
		SDL_UnlockMutex(console->mutex);
	}
	return found;
}

// ---------------------------------------------------------------------------
// The line

void console_set_open(console_t *console, bool open)
{
	console->open = open;
	console->scroll_lines = 0;
}

static void set_input(console_t *console, const char *text)
{
	SDL_strlcpy(console->input, text, sizeof(console->input));
	console->input_length = (int32_t)SDL_strlen(console->input);
	console->cursor = console->input_length;
}

static void insert_text(console_t *console, const char *text)
{
	int32_t length = (int32_t)SDL_strlen(text);

	// Not past the end, and not half a character
	while (length > 0 && console->input_length + length >= CONSOLE_INPUT_LENGTH)
	{
		length--;
		while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80)
		{
			length--;
		}
	}
	if (length <= 0)
	{
		return;
	}
	SDL_memmove(console->input + console->cursor + length, console->input + console->cursor,
		(size_t)(console->input_length - console->cursor + 1));
	SDL_memcpy(console->input + console->cursor, text, (size_t)length);
	console->input_length += length;
	console->cursor += length;
}

static int32_t previous_character(const console_t *console, int32_t position)
{
	if (position <= 0)
	{
		return 0;
	}
	position--;
	while (position > 0 && ((unsigned char)console->input[position] & 0xC0) == 0x80)
	{
		position--;
	}
	return position;
}

static int32_t next_character(const console_t *console, int32_t position)
{
	if (position >= console->input_length)
	{
		return console->input_length;
	}
	position++;
	while (position < console->input_length && ((unsigned char)console->input[position] & 0xC0) == 0x80)
	{
		position++;
	}
	return position;
}

static void erase(console_t *console, int32_t from, int32_t to)
{
	if (to <= from)
	{
		return;
	}
	SDL_memmove(console->input + from, console->input + to, (size_t)(console->input_length - to + 1));
	console->input_length -= to - from;
	console->cursor = from;
}

static void browse_history(console_t *console, int32_t direction)
{
	int32_t index = console->history_index;

	if (console->history_count == 0)
	{
		return;
	}
	if (index < 0)
	{
		if (direction > 0)
		{
			return;
		}
		SDL_strlcpy(console->history_draft, console->input, sizeof(console->history_draft));
		index = console->history_count;
	}
	index += direction;
	if (index < 0)
	{
		index = 0;
	}
	if (index >= console->history_count)
	{
		console->history_index = -1;
		set_input(console, console->history_draft);
		return;
	}
	console->history_index = index;
	set_input(console, console->history[index]);
}

static void submit(console_t *console)
{
	const char *text = console->input;

	while (*text == ' ')
	{
		text++;
	}
	console->history_index = -1;
	console->scroll_lines = 0;
	if (*text == '\0')
	{
		set_input(console, "");
		return;
	}
	// The same command twice in a row is kept once
	if (console->history_count == 0 || SDL_strcmp(console->history[console->history_count - 1], text) != 0)
	{
		if (console->history_count == CONSOLE_HISTORY)
		{
			SDL_memmove(console->history[0], console->history[1], sizeof(console->history[0]) * (CONSOLE_HISTORY - 1));
			console->history_count--;
		}
		SDL_strlcpy(console->history[console->history_count], text, sizeof(console->history[0]));
		console->history_count++;
	}
	SDL_strlcpy(console->command, text, sizeof(console->command));
	console->command_ready = true;
	set_input(console, "");
}

bool console_take_command(console_t *console, char *command_out, size_t command_out_size)
{
	if (!console->command_ready)
	{
		return false;
	}
	console->command_ready = false;
	SDL_strlcpy(command_out, console->command, command_out_size);
	return true;
}

// ---------------------------------------------------------------------------
// Tab

#define COMPLETION_MAX 512

typedef struct
{
	char names[COMPLETION_MAX][96];
	const char *keys[COMPLETION_MAX];  // a setting's key alone (NULL for commands)
	int count;
} completions_t;

static bool takes_setting(const char *command, size_t length)
{
	return (length == 3 && (SDL_strncmp(command, "set", 3) == 0 || SDL_strncmp(command, "get", 3) == 0))
		|| (length == 4 && SDL_strncmp(command, "list", 4) == 0);
}

static size_t common_prefix(const char *a, const char *b)
{
	size_t length = 0;

	while (a[length] != '\0' && a[length] == b[length])
	{
		length++;
	}
	return length;
}

static void complete(console_t *console)
{
	static completions_t found;
	int32_t word_start = console->cursor;
	int32_t word_index = 0;
	const char *first_word;
	size_t first_length = 0;
	char word[CONSOLE_INPUT_LENGTH];
	size_t word_length;
	char replacement[CONSOLE_INPUT_LENGTH];
	bool by_key = false;

	while (word_start > 0 && console->input[word_start - 1] != ' ')
	{
		word_start--;
	}
	first_word = console->input;
	while (*first_word == ' ')
	{
		first_word++;
	}
	while (first_word[first_length] != '\0' && first_word[first_length] != ' ')
	{
		first_length++;
	}
	// Which word the cursor is in
	{
		bool in_word = false;

		for (int32_t i = 0; i < word_start; i++)
		{
			bool space = console->input[i] == ' ';

			if (!space && !in_word)
			{
				word_index++;
			}
			in_word = !space;
		}
	}
	word_length = (size_t)(console->cursor - word_start);
	SDL_memcpy(word, console->input + word_start, word_length);
	word[word_length] = '\0';

	found.count = 0;
	if (word_index == 0)
	{
		for (int i = 0; i < console_command_count && found.count < COMPLETION_MAX; i++)
		{
			if (SDL_strncmp(console_commands[i].name, word, word_length) == 0)
			{
				SDL_strlcpy(found.names[found.count], console_commands[i].name, sizeof(found.names[0]));
				found.keys[found.count] = NULL;
				found.count++;
			}
		}
		for (int i = 0; i < console->added_count && found.count < COMPLETION_MAX; i++)
		{
			if (SDL_strncmp(console->added[i].name, word, word_length) == 0)
			{
				SDL_strlcpy(found.names[found.count], console->added[i].name, sizeof(found.names[0]));
				found.keys[found.count] = NULL;
				found.count++;
			}
		}
	}
	else if (word_index == 1 && takes_setting(first_word, first_length))
	{
		const char *section;
		const char *key;

		by_key = SDL_strchr(word, '.') == NULL;
		for (size_t i = 0; settings_field_name(i, &section, &key) && found.count < COMPLETION_MAX; i++)
		{
			char name[96];

			SDL_snprintf(name, sizeof(name), "%s.%s", section, key);
			if (SDL_strncmp(name, word, word_length) == 0 || (by_key && SDL_strncmp(key, word, word_length) == 0))
			{
				SDL_strlcpy(found.names[found.count], name, sizeof(found.names[0]));
				found.keys[found.count] = key;
				found.count++;
			}
		}
	}
	if (found.count == 0)
	{
		return;
	}
	replacement[0] = '\0';
	if (found.count == 1)
	{
		SDL_snprintf(replacement, sizeof(replacement), "%s ", found.names[0]);
	}
	else
	{
		size_t full = SDL_strlen(found.names[0]);
		size_t keys = found.keys[0] != NULL ? SDL_strlen(found.keys[0]) : 0;

		for (int i = 1; i < found.count; i++)
		{
			full = SDL_min(full, common_prefix(found.names[0], found.names[i]));
			if (found.keys[0] != NULL)
			{
				keys = SDL_min(keys, common_prefix(found.keys[0], found.keys[i]));
			}
		}
		if (full > word_length && SDL_strncmp(found.names[0], word, word_length) == 0)
		{
			SDL_strlcpy(replacement, found.names[0], SDL_min(full + 1, sizeof(replacement)));
		}
		else if (by_key && found.keys[0] != NULL && keys > word_length)
		{
			SDL_strlcpy(replacement, found.keys[0], SDL_min(keys + 1, sizeof(replacement)));
		}
		// What fits, a few to a line
		{
			char line[CONSOLE_LINE_LENGTH];
			size_t used = 0;
			int shown = SDL_min(found.count, 120);

			line[0] = '\0';
			for (int i = 0; i < shown; i++)
			{
				size_t length = SDL_strlen(found.names[i]);

				if (used > 0 && used + length + 2 > 110)
				{
					console_print(console, "  %s", line);
					used = 0;
					line[0] = '\0';
				}
				used += (size_t)SDL_snprintf(line + used, sizeof(line) - used, "%s%s", used > 0 ? "  " : "", found.names[i]);
				used = SDL_min(used, sizeof(line) - 1);
			}
			if (used > 0)
			{
				console_print(console, "  %s", line);
			}
			if (found.count > shown)
			{
				console_print(console, "  ... and %d more", found.count - shown);
			}
		}
	}
	if (replacement[0] != '\0')
	{
		erase(console, word_start, console->cursor);
		insert_text(console, replacement);
	}
}

// ---------------------------------------------------------------------------
// Events

static void scroll(console_t *console, int32_t lines)
{
	int32_t kept = (int32_t)SDL_min(console->line_count, (uint32_t)CONSOLE_LINES);

	console->scroll_lines = SDL_clamp(console->scroll_lines + lines, 0, SDL_max(kept - 1, 0));
}

static bool is_console_key(SDL_Scancode scancode)
{
	return scancode == SDL_SCANCODE_GRAVE;
}

bool console_handle_event(console_t *console, const SDL_Event *event)
{
	if (!console->open)
	{
		return false;
	}
	switch (event->type)
	{
		case SDL_EVENT_TEXT_INPUT:
		{
			// The key that closes it types nothing
			char text[64];
			size_t length = 0;

			for (const char *c = event->text.text; *c != '\0' && length + 1 < sizeof(text); c++)
			{
				if (*c != '`' && *c != '~' && (unsigned char)*c >= 32)
				{
					text[length++] = *c;
				}
			}
			text[length] = '\0';
			insert_text(console, text);
			return true;
		}
		case SDL_EVENT_KEY_DOWN:
		{
			SDL_Keymod modifiers = event->key.mod;
			bool control = (modifiers & SDL_KMOD_CTRL) != 0;

			if (is_console_key(event->key.scancode))
			{
				if (!event->key.repeat)
				{
					console_set_open(console, false);
				}
				return true;
			}
			switch (event->key.key)
			{
				case SDLK_ESCAPE:
					console_set_open(console, false);
					return true;
				case SDLK_RETURN:
				case SDLK_KP_ENTER:
					submit(console);
					return true;
				case SDLK_BACKSPACE:
					erase(console, previous_character(console, console->cursor), console->cursor);
					return true;
				case SDLK_DELETE:
					erase(console, console->cursor, next_character(console, console->cursor));
					return true;
				case SDLK_LEFT:
					console->cursor = previous_character(console, console->cursor);
					return true;
				case SDLK_RIGHT:
					console->cursor = next_character(console, console->cursor);
					return true;
				case SDLK_HOME:
					console->cursor = 0;
					return true;
				case SDLK_END:
					console->cursor = console->input_length;
					return true;
				case SDLK_UP:
					browse_history(console, -1);
					return true;
				case SDLK_DOWN:
					browse_history(console, 1);
					return true;
				case SDLK_TAB:
					complete(console);
					return true;
				case SDLK_PAGEUP:
					scroll(console, 10);
					return true;
				case SDLK_PAGEDOWN:
					scroll(console, -10);
					return true;
				case SDLK_L:
					if (control)
					{
						console_clear(console);
					}
					return true;
				case SDLK_U:
					if (control)
					{
						set_input(console, "");
					}
					return true;
				case SDLK_V:
					if (control)
					{
						char *clipboard = SDL_GetClipboardText();

						if (clipboard != NULL)
						{
							// One line: the rest of a pasted text is left out
							char *end = SDL_strpbrk(clipboard, "\r\n");

							if (end != NULL)
							{
								*end = '\0';
							}
							insert_text(console, clipboard);
							SDL_free(clipboard);
						}
					}
					return true;
				default:
					// Function keys still reach the game (F12 screenshots
					// with the console on screen); the rest is typing
					return !(event->key.key >= SDLK_F1 && event->key.key <= SDLK_F12);
			}
		}
		case SDL_EVENT_KEY_UP:
			return !(event->key.key >= SDLK_F1 && event->key.key <= SDLK_F12);
		case SDL_EVENT_MOUSE_WHEEL:
			scroll(console, (int32_t)SDL_lroundf(event->wheel.y * 3.0f));
			return true;
		default:
			return false;
	}
}
