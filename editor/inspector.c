// The inspector. See inspector.h.

#include "inspector.h"
#include "materials.h"

#include <stdarg.h>
#include <SDL3/SDL.h>

static const char *direction_names[WATER_DIRECTION_COUNT] = { "left (-x)", "right (+x)", "back (-z)", "front (+z)" };

static float units_to_millimeters(int64_t units)
{
	return (float)units * 1000.0f / (float)TERRAIN_HEIGHT_UNITS_PER_METER;
}

static int32_t millimeters_to_units(float millimeters)
{
	return (int32_t)(millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
}

// Ground plus whatever wall stands there: what the water lies on
static int32_t bed_units(const water_t *water, const terrain_t *terrain, int32_t index)
{
	return terrain->heights[index]
		+ (water->obstacle_centimeters != NULL ? (int32_t)water->obstacle_centimeters[index] * WATER_UNITS_PER_OBSTACLE_CENTIMETER : 0);
}

static void add_line(inspector_t *inspector, const char *format, ...)
{
	va_list arguments;

	if (inspector->line_count >= INSPECTOR_LINES)
	{
		return;
	}
	va_start(arguments, format);
	SDL_vsnprintf(inspector->lines[inspector->line_count], INSPECTOR_LINE_LENGTH, format, arguments);
	va_end(arguments);
	inspector->line_count++;
}

void inspector_set_cell(inspector_t *inspector, int32_t cell_x, int32_t cell_y)
{
	inspector->valid = cell_x >= 0 && cell_y >= 0;
	inspector->cell_x = cell_x;
	inspector->cell_y = cell_y;
	if (cell_x != inspector->history_cell_x || cell_y != inspector->history_cell_y)
	{
		inspector->history_cell_x = cell_x;
		inspector->history_cell_y = cell_y;
		inspector->history_count = 0;
		inspector->history_next = 0;
	}
}

// The speed of the cell's busiest pipe, m/s
static float fastest_pipe_speed(const water_t *water, int32_t index)
{
	float depth_meters = (float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;
	float largest = 0.0f;

	if (water->baked)
	{
		return 0.0f;
	}
	for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
	{
		largest = SDL_max(largest, SDL_fabsf((float)water->flux[k][index]));
	}
	return largest / (SDL_max(depth_meters, water->speed_depth_meters) * water->cell_size_meters);
}

void inspector_record(inspector_t *inspector, const water_t *water, const terrain_t *terrain)
{
	int32_t index;
	int32_t level;

	if (!inspector->valid || inspector->cell_x >= water->size || inspector->cell_y >= water->size)
	{
		return;
	}
	index = inspector->cell_y * water->size + inspector->cell_x;
	level = bed_units(water, terrain, index) + water->depth[index];
	if (inspector->history_count == 0)
	{
		inspector->history_base_level = level;
	}
	inspector->level_history[inspector->history_next] = units_to_millimeters(level - inspector->history_base_level);
	inspector->speed_history[inspector->history_next] = fastest_pipe_speed(water, index);
	inspector->history_next = (inspector->history_next + 1) % INSPECTOR_HISTORY;
	inspector->history_count = SDL_min(inspector->history_count + 1, INSPECTOR_HISTORY);
}

static inspector_cell_kind_t cell_kind(const water_t *water, int32_t index, int32_t film_units, int32_t thin_units)
{
	int32_t units = water->depth[index];

	if (units <= 0)
	{
		return water->obstacle_centimeters[index] > 0 ? INSPECTOR_CELL_WALL : INSPECTOR_CELL_DRY;
	}
	if (units <= film_units)
	{
		return INSPECTOR_CELL_FILM;
	}
	return units <= thin_units ? INSPECTOR_CELL_THIN : INSPECTOR_CELL_DEEP;
}

static const char *kind_name(inspector_cell_kind_t kind)
{
	static const char *names[] = { "outside", "dry", "film (does not flow)", "thin", "deep", "wall" };

	return names[kind];
}

static const char *wake_reason_text(water_wake_reason_t reason)
{
	switch (reason)
	{
		case WATER_WAKE_SOURCE:
			return "a source (brush, spring, rain)";
		case WATER_WAKE_NEIGHBOR:
			return "a neighbor tile (inflow or a higher level)";
		case WATER_WAKE_EDIT:
			return "the ground was edited";
		case WATER_WAKE_SOAK:
			return "the ground soaked water away";
		case WATER_WAKE_EROSION:
			return "erosion changed the bed";
		default:
			return "?";
	}
}

static void describe_cell(inspector_t *inspector, const inspector_world_t *world, int32_t index, int32_t film_units, int32_t thin_units)
{
	const water_t *water = world->water;
	const terrain_t *terrain = world->terrain;
	int32_t units = water->depth[index];
	int32_t bed = bed_units(water, terrain, index);
	int32_t tile = water_tile_index_of_cell(water, inspector->cell_x, inspector->cell_y);
	int32_t silt_visible_units = SDL_max(1, millimeters_to_units(world->soils->silt_visible_millimeters));
	int surface = terrain->silt_thickness[index] >= silt_visible_units ? SOIL_SILT : terrain->soil_types[index];
	int32_t capacity_units = erosion_moisture_capacity_units(terrain, world->soils, index);

	add_line(inspector, "#CELL %d, %d  (x %.2f m, z %.2f m), tile %d, %d", inspector->cell_x, inspector->cell_y,
		(double)((float)inspector->cell_x * water->cell_size_meters), (double)((float)inspector->cell_y * water->cell_size_meters),
		tile % water->tiles_per_side, tile / water->tiles_per_side);
	add_line(inspector, "water %.2f mm, %s; level %.4f m", (double)units_to_millimeters(units),
		kind_name(cell_kind(water, index, film_units, thin_units)), (double)((float)(bed + units) * TERRAIN_METERS_PER_HEIGHT_UNIT));
	add_line(inspector, "ground %.4f m (soil %.2f m over the bedrock), wall %d cm", (double)((float)terrain->heights[index] * TERRAIN_METERS_PER_HEIGHT_UNIT),
		(double)((float)(terrain->heights[index] - terrain->bedrock_heights[index]) * TERRAIN_METERS_PER_HEIGHT_UNIT),
		(int)water->obstacle_centimeters[index]);
	add_line(inspector, "surface %s (silt %.2f mm), moisture %.1f of %.1f mm", soils_name(surface),
		(double)units_to_millimeters(terrain->silt_thickness[index]), (double)units_to_millimeters(water->moisture[index]),
		(double)units_to_millimeters(capacity_units));
	add_line(inspector, "mud in the water %.3f mm (%.2f%% of the water)", (double)units_to_millimeters(water->sediment[index]),
		units > 0 ? 100.0 * (double)water->sediment[index] / (double)units : 0.0);
	// The material map (material_map.h)
	{
		const uint8_t *cell = &terrain->materials[(size_t)index * TERRAIN_MATERIAL_BYTES];

		add_line(inspector, "shows %s, cover %s, overlay %s, sheltered %.0f%%", materials_name((material_type_t)cell[0]),
			cell[1] == MATERIAL_NONE ? "none" : materials_name((material_type_t)cell[1]),
			cell[2] == MATERIAL_NONE ? "snow only" : materials_name((material_type_t)cell[2]), 100.0 * (double)cell[3] / 255.0);
	}
}

static void describe_pipes(inspector_t *inspector, const inspector_world_t *world, int32_t index, const water_cell_report_t *report)
{
	const water_t *water = world->water;
	const terrain_t *terrain = world->terrain;
	int32_t size = water->size;
	int32_t level = bed_units(water, terrain, index) + water->depth[index];
	float depth_meters = (float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;
	float section = SDL_max(depth_meters, water->speed_depth_meters) * water->cell_size_meters;
	bool inside[WATER_DIRECTION_COUNT] = { inspector->cell_x > 0, inspector->cell_x < size - 1, inspector->cell_y > 0, inspector->cell_y < size - 1 };
	int32_t neighbor[WATER_DIRECTION_COUNT] = { index - 1, index + 1, index - size, index + size };
	// The pipe of the neighbor that points back at us
	water_direction_t opposite[WATER_DIRECTION_COUNT] = { WATER_DIRECTION_RIGHT, WATER_DIRECTION_LEFT, WATER_DIRECTION_FRONT, WATER_DIRECTION_BACK };

	add_line(inspector, "#PIPES (substep %.4f s)", (double)report->substep_seconds);
	for (int k = 0; k < WATER_DIRECTION_COUNT; k++)
	{
		float out = (float)water->flux[k][index];
		float in = inside[k] ? (float)water->flux[opposite[k]][neighbor[k]] : 0.0f;
		char rules[96] = "";

		if (!inside[k])
		{
			add_line(inspector, "%s: map edge (%s), out %.5f m3/s", direction_names[k], water->drain_edges ? "drains" : "wall", (double)out);
			continue;
		}
		if (report->flows)
		{
			const water_pipe_report_t *pipe = &report->pipes[k];

			SDL_snprintf(rules, sizeof(rules), "pipe %.0f%%%s%s%s%s, next %.5f", (double)(pipe->push_factor * 100.0f),
				pipe->superfluid ? ", superfluid" : "", pipe->meniscus_holds ? ", SHUT: meniscus" : "",
				pipe->lip_holds ? ", SHUT: lip" : "", pipe->still_holds ? ", SHUT: still" : "", (double)pipe->next_flux);
		}
		else
		{
			SDL_strlcpy(rules, "cannot flow (film or dry)", sizeof(rules));
		}
		add_line(inspector, "%s: out %.5f m3/s = %.2f mm (%.3f m/s), in %.5f | level %+.2f mm vs %s | %s", direction_names[k], (double)out,
			(double)units_to_millimeters((int64_t)(out * report->units_per_flux)), (double)(out / section), (double)in,
			(double)units_to_millimeters(level - (bed_units(water, terrain, neighbor[k]) + water->depth[neighbor[k]])),
			water->depth[neighbor[k]] > report->film_units ? "wet" : "dry", rules);
	}
	if (report->flows)
	{
		add_line(inspector, "friction sees %.1f mm (into dry) / %.1f mm (over wet); shore brake x%.3f; %s",
			(double)(report->dry_friction_depth * 1000.0f), (double)(report->wet_friction_depth * 1000.0f), (double)report->shore_factor,
			report->outflow_limited ? "pipes want more than the cell holds" : "the cell holds enough");
	}
}

static void describe_tile(inspector_t *inspector, const inspector_world_t *world, int32_t tile, const water_cell_report_t *report)
{
	const water_t *water = world->water;
	const water_settings_t *settings = &world->settings->water;
	float cell_area = water->cell_size_meters * water->cell_size_meters;
	static const char *state_names[] = { "DRY", "STILL (sleeping)", "ACTIVE" };
	int state = water->tile_state[tile];

	add_line(inspector, "#TILE");
	if (water->tile_wake_step[tile] < 0)
	{
		add_line(inspector, "%s; %d of %d substeps still in a row; never woke", state_names[state], (int)water->tile_idle_steps[tile],
			SDL_max(1, settings->sleep_after_substeps));
	}
	else
	{
		add_line(inspector, "%s; %d of %d substeps still in a row; woke %.1f s ago, because of %s", state_names[state],
			(int)water->tile_idle_steps[tile], SDL_max(1, settings->sleep_after_substeps),
			(double)((float)(water->step_count - water->tile_wake_step[tile]) * world->step_seconds),
			wake_reason_text((water_wake_reason_t)water->tile_wake_reason[tile]));
	}
	if (state == WATER_TILE_ACTIVE)
	{
		bool too_fast = water->tile_max_speed[tile] >= settings->sleep_speed_meters_per_second;
		bool changing = water->tile_max_change[tile] > report->sleep_change_units;

		// Level changes per substep, shown per second like the setting
		float per_second = 1.0f / SDL_max(report->substep_seconds, 1e-6f);

		add_line(inspector, "fastest flow %.3f m/s (still below %.3f)%s; fastest level change %.1f mm/s (still up to %.1f)%s",
			(double)water->tile_max_speed[tile], (double)settings->sleep_speed_meters_per_second, too_fast ? " AWAKE" : "",
			(double)(units_to_millimeters(water->tile_max_change[tile]) * per_second),
			(double)(units_to_millimeters(report->sleep_change_units) * per_second), changing ? " AWAKE" : "");
	}
	add_line(inspector, "water %.2f m3, deepest %.3f m, mud %.3f m3, ground moisture %.2f m3",
		(double)((float)water->tile_total_depth[tile] * TERRAIN_METERS_PER_HEIGHT_UNIT * cell_area),
		(double)((float)water->tile_max_depth[tile] * TERRAIN_METERS_PER_HEIGHT_UNIT),
		(double)((float)water->tile_sediment[tile] * TERRAIN_METERS_PER_HEIGHT_UNIT * cell_area),
		(double)((float)water->tile_moisture[tile] * TERRAIN_METERS_PER_HEIGHT_UNIT * cell_area));
	add_line(inspector, "map: %d substeps last step, fastest deep flow %.2f m/s", water->substeps_last_step, (double)water->fastest_flow_last_step);
}

static void describe_ground(inspector_t *inspector, const inspector_world_t *world, int32_t index, int32_t tile, int32_t film_units,
	int32_t thin_units)
{
	const water_t *water = world->water;
	const terrain_t *terrain = world->terrain;
	const water_settings_t *settings = &world->settings->water;
	const soil_settings_t *soils = world->soils;
	int32_t size = water->size;
	int32_t x = inspector->cell_x;
	int32_t y = inspector->cell_y;
	int32_t units = water->depth[index];
	float depth_meters = (float)units * TERRAIN_METERS_PER_HEIGHT_UNIT;
	int32_t silt_visible_units = SDL_max(1, millimeters_to_units(soils->silt_visible_millimeters));
	int surface = terrain->silt_thickness[index] >= silt_visible_units ? SOIL_SILT : terrain->soil_types[index];
	int eroding_layer = terrain->silt_thickness[index] > 0 ? SOIL_SILT : terrain->soil_types[index];
	const soil_properties_t *properties = &soils->soils[eroding_layer];
	int32_t capacity_units = erosion_moisture_capacity_units(terrain, soils, index);
	float flow_x;
	float flow_y;
	float speed;
	float critical;
	float transport_units;
	const char *erosion_state;

	// Erosion, as erosion.c sees the cell
	flow_x = (float)water->flux[WATER_DIRECTION_RIGHT][index] - (float)water->flux[WATER_DIRECTION_LEFT][index];
	flow_y = (float)water->flux[WATER_DIRECTION_FRONT][index] - (float)water->flux[WATER_DIRECTION_BACK][index];
	if (x > 0)
	{
		flow_x += (float)water->flux[WATER_DIRECTION_RIGHT][index - 1];
	}
	if (x < size - 1)
	{
		flow_x -= (float)water->flux[WATER_DIRECTION_LEFT][index + 1];
	}
	if (y > 0)
	{
		flow_y += (float)water->flux[WATER_DIRECTION_FRONT][index - size];
	}
	if (y < size - 1)
	{
		flow_y -= (float)water->flux[WATER_DIRECTION_BACK][index + size];
	}
	speed = units > 0 ? 0.5f * SDL_sqrtf(flow_x * flow_x + flow_y * flow_y) / (SDL_max(depth_meters, water->speed_depth_meters) * water->cell_size_meters)
		: 0.0f;
	speed = SDL_min(speed, 20.0f);
	// Under flowing water the ground counts as soaked (water_erode.comp)
	critical = properties->critical_speed_meters_per_second * soils->wet_critical_speed_factor;
	// Erosion never lets water carry more than a fifth of its volume
	transport_units = SDL_min(soils->transport_capacity * depth_meters * speed * (float)TERRAIN_HEIGHT_UNITS_PER_METER, (float)units * 0.2f);
	if (units <= film_units)
	{
		erosion_state = water->sediment[index] > 0 ? "drops all its mud (too thin to carry it)" : "nothing (no flowing water)";
	}
	else if (speed < settings->still_water_speed_meters_per_second && water->sediment[index] > 0)
	{
		erosion_state = "mud settles fast (still water)";
	}
	else if (properties->erodibility > 0.0f && speed > critical
		&& (float)units * 1000.0f / (float)TERRAIN_HEIGHT_UNITS_PER_METER <= 0.5f * settings->erosion_minimum_depth_millimeters)
	{
		erosion_state = "would erode, but too shallow to dig";
	}
	else if (properties->erodibility > 0.0f && speed > critical)
	{
		erosion_state = (float)units * 1000.0f / (float)TERRAIN_HEIGHT_UNITS_PER_METER < settings->erosion_minimum_depth_millimeters
			? "ERODING (weakly, shallow)" : "ERODING";
	}
	else if ((float)water->sediment[index] > transport_units)
	{
		erosion_state = "depositing (carries more than it can)";
	}
	else
	{
		erosion_state = "nothing";
	}
	add_line(inspector, "#GROUND");
	add_line(inspector, "erosion: speed %.3f m/s, %s erodes above %.3f m/s, can carry %.3f mm, carries %.3f mm: %s", (double)speed,
		soils_name(eroding_layer), (double)critical, (double)units_to_millimeters((int64_t)transport_units),
		(double)units_to_millimeters(water->sediment[index]), erosion_state);

	// Wetting, as wet_tile in erosion.c does it (the water stays)
	if (units > 0 && water->moisture[index] < capacity_units)
	{
		add_line(inspector, "ground getting wet at %.3f mm/s (%s)", (double)soils->soils[surface].infiltration_millimeters_per_second,
			soils_name(surface));
	}
	else if (units > 0)
	{
		add_line(inspector, "ground soaked");
	}
	else
	{
		add_line(inspector, "ground drying at %.3f mm/s", (double)soils->drying_millimeters_per_second);
	}

	// Drying of the water itself, as evaporate_films does it
	if (units > 0 && units <= film_units)
	{
		add_line(inspector, "evaporating: film, %.3f mm/s", (double)settings->film_evaporation_millimeters_per_second);
	}
	else if (units > film_units && units <= thin_units)
	{
		bool shore = (x > 0 && water->depth[index - 1] > thin_units) || (x < size - 1 && water->depth[index + 1] > thin_units)
			|| (y > 0 && water->depth[index - size] > thin_units) || (y < size - 1 && water->depth[index + size] > thin_units);

		if (shore)
		{
			add_line(inspector, "evaporating: no, deeper water next to it (shore)");
		}
		else if (water->raining)
		{
			add_line(inspector, "evaporating: no, it rains");
		}
		else if (water->tile_state[tile] != WATER_TILE_SLEEPING)
		{
			add_line(inspector, "evaporating: not while the tile is active");
		}
		else
		{
			add_line(inspector, "evaporating: thin water left behind, %.2f mm/s", (double)settings->thin_water_evaporation_millimeters_per_second);
		}
	}
	else if (units > thin_units)
	{
		add_line(inspector, "evaporating: no, deeper than %.0f mm", (double)settings->thin_water_millimeters);
	}
}

static void describe_nearest_body(inspector_t *inspector, const inspector_world_t *world)
{
	const physics_t *physics = world->physics;
	const objects_t *objects = world->objects;
	float x = (float)inspector->cell_x * world->water->cell_size_meters;
	float z = (float)inspector->cell_y * world->water->cell_size_meters;
	const physics_body_t *nearest = NULL;
	float nearest_distance = 3.0f;
	static const char *kind_names[] = { "fixed", "anchored", "moving" };

	for (int slot = 0; slot < physics->body_capacity; slot++)
	{
		const physics_body_t *body = &physics->bodies[slot];
		float distance;

		if (!body->used)
		{
			continue;
		}
		distance = SDL_sqrtf((body->x - x) * (body->x - x) + (body->z - z) * (body->z - z));
		if (distance < nearest_distance)
		{
			nearest_distance = distance;
			nearest = body;
		}
	}
	add_line(inspector, "#NEAREST BODY (within 3 m)");
	if (nearest == NULL || nearest->instance < 0 || nearest->instance >= objects->instance_count)
	{
		add_line(inspector, "none");
		return;
	}
	add_line(inspector, "%s at %.1f m: %s, %s, %.0f kg (weight %.0f N), damage %d",
		objects->types.types[objects->instances[nearest->instance].type].name, (double)nearest_distance, kind_names[nearest->kind],
		nearest->awake ? "awake" : "asleep", (double)nearest->mass, (double)(nearest->mass * 9.81f), (int)objects->instances[nearest->instance].damage);
	add_line(inspector, "last water push: %.0f%% under water, buoyancy %.0f N, drag %.0f N, fastest flow %.2f m/s",
		(double)(nearest->water_submerged * 100.0f), (double)nearest->water_buoyancy_newtons, (double)nearest->water_drag_newtons,
		(double)nearest->water_flow_meters_per_second);
}

static void fill_grid(inspector_t *inspector, const inspector_world_t *world, int32_t index, int32_t film_units, int32_t thin_units)
{
	const water_t *water = world->water;
	const terrain_t *terrain = world->terrain;
	int32_t level = bed_units(water, terrain, index) + water->depth[index];
	int half = INSPECTOR_GRID / 2;

	for (int row = 0; row < INSPECTOR_GRID; row++)
	{
		for (int column = 0; column < INSPECTOR_GRID; column++)
		{
			int32_t x = inspector->cell_x + column - half;
			int32_t y = inspector->cell_y + row - half;
			int32_t other;

			if (x < 0 || y < 0 || x >= water->size || y >= water->size)
			{
				inspector->grid_kind[row][column] = INSPECTOR_CELL_OUTSIDE;
				inspector->grid[row][column] = 0.0f;
				continue;
			}
			other = y * water->size + x;
			inspector->grid_kind[row][column] = (uint8_t)cell_kind(water, other, film_units, thin_units);
			inspector->grid[row][column] = units_to_millimeters(bed_units(water, terrain, other) + water->depth[other] - level);
		}
	}
}

void inspector_describe(inspector_t *inspector, const inspector_world_t *world)
{
	const water_t *water = world->water;
	water_cell_report_t report;
	int32_t index;
	int32_t tile;
	float substep_seconds;

	inspector->line_count = 0;
	if (!inspector->valid || inspector->cell_x >= water->size || inspector->cell_y >= water->size)
	{
		add_line(inspector, "Point at the map; click to pin a cell");
		return;
	}
	index = inspector->cell_y * water->size + inspector->cell_x;
	tile = water_tile_index_of_cell(water, inspector->cell_x, inspector->cell_y);
	if (water->baked)
	{
		// A baked map: no layers, no simulation; what it shows
		const terrain_t *terrain = world->terrain;
		const uint8_t *cell = &terrain->materials[(size_t)index * TERRAIN_MATERIAL_BYTES];
		float depth;
		float velocity_x;
		float velocity_z;

		water_sample(water, (float)inspector->cell_x * water->cell_size_meters, (float)inspector->cell_y * water->cell_size_meters, &depth,
			&velocity_x, &velocity_z);
		add_line(inspector, "#CELL %d, %d  (x %.2f m, z %.2f m), tile %d, %d (a baked map)", inspector->cell_x, inspector->cell_y,
			(double)((float)inspector->cell_x * water->cell_size_meters), (double)((float)inspector->cell_y * water->cell_size_meters),
			tile % water->tiles_per_side, tile / water->tiles_per_side);
		add_line(inspector, "ground %.4f m, water %.3f m, flowing %.2f m/s (x %.2f, z %.2f), mud %.2f%%",
			(double)((float)terrain->heights[index] * TERRAIN_METERS_PER_HEIGHT_UNIT), (double)depth,
			(double)SDL_sqrtf(velocity_x * velocity_x + velocity_z * velocity_z), (double)velocity_x, (double)velocity_z,
			100.0 * (double)water_concentration_at(water, index));
		add_line(inspector, "shows %s, cover %s, overlay %s, sheltered %.0f%%", materials_name((material_type_t)cell[0]),
			cell[1] == MATERIAL_NONE ? "none" : materials_name((material_type_t)cell[1]),
			cell[2] == MATERIAL_NONE ? "snow only" : materials_name((material_type_t)cell[2]), 100.0 * (double)cell[3] / 255.0);
		describe_nearest_body(inspector, world);
		return;
	}
	substep_seconds = world->step_seconds / (float)SDL_max(1, water->substeps_last_step);
	water_inspect_cell(water, world->terrain, &world->settings->water, world->soils, substep_seconds, inspector->cell_x, inspector->cell_y,
		&report);

	describe_cell(inspector, world, index, report.film_units, report.thin_units);
	describe_pipes(inspector, world, index, &report);
	describe_tile(inspector, world, tile, &report);
	describe_ground(inspector, world, index, tile, report.film_units, report.thin_units);
	describe_nearest_body(inspector, world);
	fill_grid(inspector, world, index, report.film_units, report.thin_units);
}

void inspector_log(const inspector_t *inspector)
{
	static const char kind_marks[] = { ' ', '.', 'f', 't', 'w', '#' };

	SDL_Log("---- inspector ----");
	for (int i = 0; i < inspector->line_count; i++)
	{
		SDL_Log("%s", inspector->lines[i]);
	}
	SDL_Log("levels around it, mm (. dry, f film, t thin, w deep, # wall):");
	for (int row = 0; row < INSPECTOR_GRID; row++)
	{
		char text[256] = "";

		for (int column = 0; column < INSPECTOR_GRID; column++)
		{
			char entry[32];

			SDL_snprintf(entry, sizeof(entry), "%c%+9.2f ", kind_marks[inspector->grid_kind[row][column]], (double)inspector->grid[row][column]);
			SDL_strlcat(text, entry, sizeof(text));
		}
		SDL_Log("%s", text);
	}
	if (inspector->history_count > 0)
	{
		float lowest = 1e30f;
		float highest = -1e30f;
		float fastest = 0.0f;

		for (int i = 0; i < inspector->history_count; i++)
		{
			lowest = SDL_min(lowest, inspector->level_history[i]);
			highest = SDL_max(highest, inspector->level_history[i]);
			fastest = SDL_max(fastest, inspector->speed_history[i]);
		}
		SDL_Log("last %d steps: level moved over %.2f mm, fastest pipe %.3f m/s", inspector->history_count, (double)(highest - lowest),
			(double)fastest);
	}
	SDL_Log("-------------------");
}
