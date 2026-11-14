// Tiny INI parser. See ini_parser.h.

#include "ini_parser.h"
#include "file_io.h"

#include <stdlib.h>
#include <SDL3/SDL.h>

static char *trim_in_place(char *text)
{
	char *end;

	while (*text == ' ' || *text == '\t')
	{
		text++;
	}
	end = text + SDL_strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
	{
		end--;
	}
	*end = '\0';
	return text;
}

void ini_parse_buffer(char *buffer, const char *file_name, ini_key_callback_t callback, void *user_data)
{
	char section[128] = "";
	char *line = buffer;
	int line_number = 0;

	while (line != NULL && *line != '\0')
	{
		char *next_line = SDL_strchr(line, '\n');
		char *content;

		if (next_line != NULL)
		{
			*next_line = '\0';
			next_line++;
		}
		line_number++;
		content = trim_in_place(line);

		if (content[0] == '\0' || content[0] == ';' || content[0] == '#')
		{
			// Empty line or comment
		}
		else if (content[0] == '[')
		{
			char *closing = SDL_strchr(content, ']');

			if (closing == NULL)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s:%d: missing ']'", file_name, line_number);
			}
			else
			{
				*closing = '\0';
				SDL_strlcpy(section, trim_in_place(content + 1), sizeof(section));
			}
		}
		else
		{
			char *equals = SDL_strchr(content, '=');

			if (equals == NULL)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s:%d: expected 'key = value'", file_name, line_number);
			}
			else
			{
				char *key;
				char *value;

				*equals = '\0';
				key = trim_in_place(content);
				value = trim_in_place(equals + 1);
				if (!callback(user_data, section, key, value))
				{
					SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s:%d: unknown or invalid key [%s] %s = %s",
						file_name, line_number, section, key, value);
				}
			}
		}
		line = next_line;
	}
}

bool ini_parse_data_file(const char *relative_path, ini_key_callback_t callback, void *user_data)
{
	char *buffer = file_io_load_data_file(relative_path, NULL);

	if (buffer == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot read %s: %s", relative_path, SDL_GetError());
		return false;
	}
	ini_parse_buffer(buffer, relative_path, callback, user_data);
	SDL_free(buffer);
	return true;
}

bool ini_value_to_float(const char *text, float *value_out)
{
	char *end = NULL;
	double value = SDL_strtod(text, &end);

	if (end == text || *end != '\0')
	{
		return false;
	}
	*value_out = (float)value;
	return true;
}

bool ini_value_to_int(const char *text, int *value_out)
{
	char *end = NULL;
	long value = SDL_strtol(text, &end, 0);

	if (end == text || *end != '\0')
	{
		return false;
	}
	*value_out = (int)value;
	return true;
}

bool ini_value_to_bool(const char *text, bool *value_out)
{
	if (SDL_strcasecmp(text, "1") == 0 || SDL_strcasecmp(text, "true") == 0 || SDL_strcasecmp(text, "yes") == 0 || SDL_strcasecmp(text, "on") == 0)
	{
		*value_out = true;
		return true;
	}
	if (SDL_strcasecmp(text, "0") == 0 || SDL_strcasecmp(text, "false") == 0 || SDL_strcasecmp(text, "no") == 0 || SDL_strcasecmp(text, "off") == 0)
	{
		*value_out = false;
		return true;
	}
	return false;
}

bool ini_value_to_color(const char *text, float color_out[3])
{
	char copy[128];
	char *items[3];
	float components[3];

	SDL_strlcpy(copy, text, sizeof(copy));
	if (ini_split_list(copy, items, 3) != 3)
	{
		return false;
	}
	for (int i = 0; i < 3; i++)
	{
		if (!ini_value_to_float(items[i], &components[i]))
		{
			return false;
		}
	}
	for (int i = 0; i < 3; i++)
	{
		color_out[i] = components[i] / 255.0f;
	}
	return true;
}

int ini_split_list(char *text, char **items_out, int max_items)
{
	int item_count = 0;
	char *cursor = text;

	while (cursor != NULL && item_count < max_items)
	{
		char *comma = SDL_strchr(cursor, ',');
		char *item;

		if (comma != NULL)
		{
			*comma = '\0';
		}
		item = trim_in_place(cursor);
		if (item[0] != '\0')
		{
			items_out[item_count++] = item;
		}
		cursor = comma != NULL ? comma + 1 : NULL;
	}
	return item_count;
}
