// potop_module: what is in a module (.pmod, engine/module_format.h).
//
//   tools/potop_module FILE.pmod...     every part and state, as text
//
// Modules are written by the tools that make them (tools/potop_house: a
// recipe with module = NAME), never by hand; this only reads them.

#include <stdio.h>
#include <string.h>

#include "module_file.h"

int main(int argument_count, char **arguments)
{
	int failed = 0;

	if (argument_count < 2 || strcmp(arguments[1], "--help") == 0 || strcmp(arguments[1], "-h") == 0)
	{
		printf("potop_module FILE.pmod...   what is in each module (its parts, its states)\n");
		return argument_count < 2 ? 1 : 0;
	}
	for (int i = 1; i < argument_count; i++)
	{
		module_file_t module;
		char error[128];

		if (strcmp(arguments[i], "--dump") == 0)
		{
			continue;
		}
		if (!module_file_read(arguments[i], &module, error, sizeof(error)))
		{
			fprintf(stderr, "potop_module: %s: %s\n", arguments[i], error);
			failed++;
			continue;
		}
		printf("%s: ", arguments[i]);
		module_file_dump(&module, stdout);
	}
	return failed > 0 ? 1 : 0;
}
