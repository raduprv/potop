// The editor's tools. See tools.h.

#include "tools.h"

#include <SDL3/SDL.h>

void tools_initialize(tool_state_t *tools, const tool_settings_t *settings)
{
	SDL_zerop(tools);
	tools->current_tool = TOOL_NONE;
	tools->current_soil = SOIL_EARTH;
	tools->brush_radius_meters = settings->brush_radius_meters;
	tools->current_stamp = STAMP_DAM;
	tools->stamp_height_meters = settings->stamp_height_meters;
	tools->light_kind = TOOL_LIGHT_LAMP;
	tools->light_intensity = 1.5f;
	tools->light_radius_meters = 8.0f;
	tools->light_height_meters = 1.8f;
}

void tools_shutdown(tool_state_t *tools)
{
	(void)tools;
}

const char *tools_tool_name(tool_type_t tool)
{
	static const char *names[TOOL_COUNT] = { "raise", "dig", "paint soil", "smooth", "stamp", "plant", "clear", "layout", "add water",
		"sponge", "spring", "remove layout", "object", "inspect", "effect", "light", "area", "no tool" };

	return (tool >= 0 && tool < TOOL_COUNT) ? names[tool] : "?";
}

const char *tools_soil_name(soil_type_t soil)
{
	static const char *names[SOIL_TYPE_COUNT] = { "sand", "earth", "clay", "rock" };

	return (soil >= 0 && soil < SOIL_TYPE_COUNT) ? names[soil] : "?";
}
