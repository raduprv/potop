// The editor's tools. See tools.h.

#include "tools.h"

#include <SDL3/SDL.h>

void tools_initialize(tool_state_t *tools, const tool_settings_t *settings)
{
	SDL_zerop(tools);
	tools->current_tool = TOOL_NONE;
	tools->current_soil = SOIL_EARTH;
	tools->brush_radius_meters = settings->brush_radius_meters;
	tools->sculpt_mode = 0;
	tools->sculpt_shape = 0;
	tools->sculpt_radius_meters = 200.0f;
	tools->sculpt_meters_per_second = 10.0f;
	tools->sculpt_share_per_second = 1.0f;
	tools->sculpt_feature_meters = 300.0f;
	tools->current_stamp = STAMP_DAM;
	tools->stamp_height_meters = settings->stamp_height_meters;
	tools->light_kind = TOOL_LIGHT_LAMP;
	tools->light_intensity = 1.5f;
	tools->light_radius_meters = 8.0f;
	tools->light_height_meters = 1.8f;
	for (int i = 0; i < (int)SDL_arraysize(tools->clear_categories); i++)
	{
		tools->clear_categories[i] = true;
	}
}

void tools_shutdown(tool_state_t *tools)
{
	(void)tools;
}

const char *tools_tool_name(tool_type_t tool)
{
	static const char *names[TOOL_COUNT] = { "raise", "dig", "paint soil", "smooth", "stamp", "plant", "clear", "layout", "add water",
		"sponge", "spring", "remove layout", "object", "inspect", "effect", "light", "area", "sculpt", "no tool" };

	return (tool >= 0 && tool < TOOL_COUNT) ? names[tool] : "?";
}

const char *tools_soil_name(soil_type_t soil)
{
	static const char *names[SOIL_TYPE_COUNT] = { "sand", "earth", "clay", "rock" };

	return (soil >= 0 && soil < SOIL_TYPE_COUNT) ? names[soil] : "?";
}
