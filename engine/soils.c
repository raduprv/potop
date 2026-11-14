// Soil properties. See soils.h.

#include "soils.h"
#include "ini_parser.h"

#include <SDL3/SDL.h>

static const char *soil_names[SOIL_PROPERTY_COUNT] = { "sand", "earth", "clay", "rock", "silt" };

const char *soils_name(int soil)
{
	return (soil >= 0 && soil < SOIL_PROPERTY_COUNT) ? soil_names[soil] : "?";
}

static void set_soil(soil_properties_t *soil, float infiltration, float capacity, float friction, float critical_speed,
	float erodibility, float wet_talus)
{
	soil->infiltration_millimeters_per_second = infiltration;
	soil->capacity_millimeters = capacity;
	soil->friction_factor = friction;
	soil->critical_speed_meters_per_second = critical_speed;
	soil->erodibility = erodibility;
	soil->wet_talus_slope = wet_talus;
}

void soils_set_defaults(soil_settings_t *settings)
{
	SDL_zerop(settings);
	// These are game values, picked to look right at game speed: real
	// infiltration is one to two orders of magnitude slower
	set_soil(&settings->soils[SOIL_SAND], 2.0f, 60.0f, 1.0f, 0.25f, 0.8f, 0.6f);
	set_soil(&settings->soils[SOIL_EARTH], 0.5f, 80.0f, 1.3f, 0.45f, 0.4f, 0.8f);
	set_soil(&settings->soils[SOIL_CLAY], 0.05f, 100.0f, 1.1f, 0.8f, 0.2f, 1.0f);
	set_soil(&settings->soils[SOIL_ROCK], 0.0f, 0.0f, 0.8f, 100.0f, 0.0f, 100.0f);
	set_soil(&settings->soils[SOIL_SILT], 0.2f, 80.0f, 0.9f, 0.2f, 1.0f, 0.4f);

	settings->transport_capacity = 0.05f;
	settings->deposition_rate = 0.6f;
	settings->wet_critical_speed_factor = 0.6f;
	settings->drying_millimeters_per_second = 0.1f;
	settings->silt_visible_millimeters = 3.0f;
	settings->silt_full_millimeters = 40.0f;
	settings->minimum_soil_millimeters = 1.0f;
	settings->soil_depth_minimum_meters = 0.3f;
	settings->soil_depth_maximum_meters = 4.0f;
	settings->erosion_smoothing = 0.5f;
}

static bool soil_key_callback(void *user_data, const char *section, const char *key, const char *value)
{
	soil_settings_t *settings = user_data;
	soil_properties_t *soil = NULL;

	if (SDL_strcmp(section, "general") == 0)
	{
		if (SDL_strcmp(key, "transport_capacity") == 0) return ini_value_to_float(value, &settings->transport_capacity);
		if (SDL_strcmp(key, "deposition_rate") == 0) return ini_value_to_float(value, &settings->deposition_rate);
		if (SDL_strcmp(key, "wet_critical_speed_factor") == 0) return ini_value_to_float(value, &settings->wet_critical_speed_factor);
		if (SDL_strcmp(key, "drying_millimeters_per_second") == 0) return ini_value_to_float(value, &settings->drying_millimeters_per_second);
		if (SDL_strcmp(key, "silt_visible_millimeters") == 0) return ini_value_to_float(value, &settings->silt_visible_millimeters);
		if (SDL_strcmp(key, "silt_full_millimeters") == 0) return ini_value_to_float(value, &settings->silt_full_millimeters);
		if (SDL_strcmp(key, "minimum_soil_millimeters") == 0) return ini_value_to_float(value, &settings->minimum_soil_millimeters);
		if (SDL_strcmp(key, "soil_depth_minimum_meters") == 0) return ini_value_to_float(value, &settings->soil_depth_minimum_meters);
		if (SDL_strcmp(key, "soil_depth_maximum_meters") == 0) return ini_value_to_float(value, &settings->soil_depth_maximum_meters);
		if (SDL_strcmp(key, "erosion_smoothing") == 0) return ini_value_to_float(value, &settings->erosion_smoothing);
		return false;
	}

	for (int i = 0; i < SOIL_PROPERTY_COUNT; i++)
	{
		if (SDL_strcmp(section, soil_names[i]) == 0)
		{
			soil = &settings->soils[i];
		}
	}
	if (soil == NULL)
	{
		return false;
	}
	if (SDL_strcmp(key, "infiltration_millimeters_per_second") == 0) return ini_value_to_float(value, &soil->infiltration_millimeters_per_second);
	if (SDL_strcmp(key, "capacity_millimeters") == 0) return ini_value_to_float(value, &soil->capacity_millimeters);
	if (SDL_strcmp(key, "friction_factor") == 0) return ini_value_to_float(value, &soil->friction_factor);
	if (SDL_strcmp(key, "critical_speed_meters_per_second") == 0) return ini_value_to_float(value, &soil->critical_speed_meters_per_second);
	if (SDL_strcmp(key, "erodibility") == 0) return ini_value_to_float(value, &soil->erodibility);
	if (SDL_strcmp(key, "wet_talus_slope") == 0) return ini_value_to_float(value, &soil->wet_talus_slope);
	return false;
}

void soils_load(soil_settings_t *settings)
{
	ini_parse_data_file("soils.ini", soil_key_callback, settings);
}
