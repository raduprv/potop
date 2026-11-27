// A small reader of ini files: see damage_ini.h

#include "damage_ini.h"

#include <stdio.h>
#include <string.h>

#define LINE_LENGTH 256
#define SECTION_LENGTH 64

// Cuts the spaces off both ends of a text, in place; returns its start
static char *trim(char *text)
{
	size_t length;

	text += strspn(text, " \t\r\n");
	length = strlen(text);
	while (length > 0 && strchr(" \t\r\n", text[length - 1]) != NULL)
	{
		text[--length] = '\0';
	}
	return text;
}

bool damage_ini_read(const char *path, damage_ini_key_function_t *key_function, void *user)
{
	FILE *file = fopen(path, "r");
	char line[LINE_LENGTH];
	char section[SECTION_LENGTH] = "";
	int line_number = 0;

	if (file == NULL)
	{
		return false;
	}
	while (fgets(line, sizeof(line), file) != NULL)
	{
		char *text;
		char *equals;

		line_number++;
		line[strcspn(line, ";#")] = '\0';
		text = trim(line);
		if (text[0] == '\0')
		{
			continue;
		}
		if (text[0] == '[')
		{
			char *closing = strchr(text, ']');

			if (closing != NULL)
			{
				*closing = '\0';
				snprintf(section, sizeof(section), "%s", trim(text + 1));
				continue;
			}
		}
		equals = strchr(text, '=');
		if (equals == NULL)
		{
			fprintf(stderr, "%s, line %d: neither a section nor a key = value\n", path, line_number);
			continue;
		}
		*equals = '\0';
		if (!key_function(user, section, trim(text), trim(equals + 1)))
		{
			fprintf(stderr, "%s, line %d: unknown key or bad value in [%s]\n", path, line_number, section);
		}
	}
	fclose(file);
	return true;
}
