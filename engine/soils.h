// How each soil behaves: water absorption, friction, erosion, collapse.
// Read from data/soils.ini, one section per soil (sand, earth, clay, rock,
// silt), on top of compiled-in defaults. materials.ini says what the ground
// looks like; soils.ini says what it does.

#ifndef SOILS_H
#define SOILS_H

#include "terrain.h"

typedef struct
{
	// Infiltration: how fast dry soil drinks water standing on it, and how
	// much water a column of it can hold before it is saturated
	float infiltration_millimeters_per_second;
	float capacity_millimeters;
	// Multiplies the water's bed friction
	float friction_factor;
	// Erosion: below this flow speed the water does not pick anything up;
	// above it, a cell loses this fraction of its carrying shortfall per
	// second (higher = erodes faster)
	float critical_speed_meters_per_second;
	float erodibility;
	// Steepest slope (meters per meter) the soil holds under water before
	// banks collapse
	float wet_talus_slope;
} soil_properties_t;

typedef struct
{
	soil_properties_t soils[SOIL_PROPERTY_COUNT];

	// How much sediment flowing water can carry: capacity (meters of
	// material per cell) = transport_capacity * depth * (speed - critical)
	float transport_capacity;
	// Fraction of the excess sediment settling per second where the water
	// carries more than it can
	float deposition_rate;
	// Wet soil starts eroding at this fraction of its critical speed
	float wet_critical_speed_factor;
	// Soil moisture drains away (where no water stands on it) at this rate
	float drying_millimeters_per_second;
	// Silt starts showing at this thickness and covers the ground fully at
	// silt_full_millimeters
	float silt_visible_millimeters;
	float silt_full_millimeters;
	// Erosion stops this close to the bedrock
	float minimum_soil_millimeters;
	// Generated soil depth over the bedrock: deep in flat valleys, thin on
	// steep slopes and peaks
	float soil_depth_minimum_meters;
	float soil_depth_maximum_meters;
	// Under water, a cell standing above (or below) both its neighbors on a
	// line, the one cell sawtooth erosion leaves behind, gives this fraction
	// of the difference to them (or takes it) per bank check; 0 disables
	float erosion_smoothing;
} soil_settings_t;

void soils_set_defaults(soil_settings_t *settings);

// Applies data/soils.ini (if present) over the current values.
void soils_load(soil_settings_t *settings);

const char *soils_name(int soil);

#endif
