// A small ini reader: see wb_ini.h

#include "wb_ini.h"

#include <SDL3/SDL.h>

static char *trim(char *text)
{
	size_t length;

	while (*text == ' ' || *text == '\t')
	{
		text++;
	}
	length = SDL_strlen(text);
	while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' || text[length - 1] == '\n'))
	{
		text[--length] = '\0';
	}
	return text;
}

bool wb_ini_parse_file(const char *path, wb_ini_callback_t callback, void *user)
{
	size_t size = 0;
	char *contents = SDL_LoadFile(path, &size);
	char section[64] = "";
	char *line;
	char *next;
	int line_number = 0;

	if (contents == NULL)
	{
		return false;
	}
	for (line = contents; line != NULL; line = next)
	{
		char *comment;
		char *equals;
		char *text;

		line_number++;
		next = SDL_strchr(line, '\n');
		if (next != NULL)
		{
			*next++ = '\0';
		}
		comment = SDL_strpbrk(line, ";#");
		if (comment != NULL)
		{
			*comment = '\0';
		}
		text = trim(line);
		if (*text == '\0')
		{
			continue;
		}
		if (*text == '[')
		{
			char *end = SDL_strchr(text, ']');

			if (end != NULL)
			{
				*end = '\0';
			}
			SDL_strlcpy(section, trim(text + 1), sizeof(section));
			continue;
		}
		equals = SDL_strchr(text, '=');
		if (equals == NULL)
		{
			SDL_Log("%s:%d: not a key = value line", path, line_number);
			continue;
		}
		*equals = '\0';
		if (!callback(user, section, trim(text), trim(equals + 1)))
		{
			SDL_Log("%s:%d: [%s] %s is not known", path, line_number, section, trim(text));
		}
	}
	SDL_free(contents);
	return true;
}
