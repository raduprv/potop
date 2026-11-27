// The console's commands (console.h): the same in the game and in the
// editor. Answers go through SDL_Log: into the console and the terminal
// alike.

#include "console.h"
#include "profile.h"

#include <SDL3/SDL.h>

#include "potop_engine.h"
#include "map_file.h"
#include "season.h"


// The next word of a command line; false at the end
static bool next_word(const char **cursor, char *word_out, size_t word_out_size)
{
	const char *start = *cursor;
	size_t length = 0;

	while (*start == ' ' || *start == '\t')
	{
		start++;
	}
	if (*start == '\0')
	{
		*cursor = start;
		return false;
	}
	while (start[length] != '\0' && start[length] != ' ' && start[length] != '\t')
	{
		length++;
	}
	SDL_strlcpy(word_out, start, SDL_min(length + 1, word_out_size));
	*cursor = start + length;
	return true;
}

static bool read_number(const char *text, float *value_out)
{
	char *end = NULL;
	double value = SDL_strtod(text, &end);

	if (end == text || (end != NULL && *end != '\0'))
	{
		return false;
	}
	*value_out = (float)value;
	return true;
}

// section.key, or a key alone where only one section has it
static bool find_setting(const char *name, char *section_out, size_t section_out_size, const char **key_out)
{
	const char *dot = SDL_strchr(name, '.');
	const char *section;
	const char *key;
	int found = 0;
	char options[512];

	options[0] = '\0';
	for (size_t i = 0; settings_field_name(i, &section, &key); i++)
	{
		bool match;

		if (dot != NULL)
		{
			match = SDL_strlen(section) == (size_t)(dot - name) && SDL_strncmp(section, name, (size_t)(dot - name)) == 0
				&& SDL_strcmp(key, dot + 1) == 0;
		}
		else
		{
			match = SDL_strcmp(key, name) == 0;
		}
		if (match)
		{
			if (found == 0)
			{
				SDL_strlcpy(section_out, section, section_out_size);
				*key_out = key;
			}
			SDL_snprintf(options + SDL_strlen(options), sizeof(options) - SDL_strlen(options), "%s%s.%s", found > 0 ? ", " : "", section,
				key);
			found++;
		}
	}
	if (found == 1)
	{
		return true;
	}
	if (found == 0)
	{
		SDL_Log("No setting '%s' (list shows them, Tab completes)", name);
	}
	else
	{
		SDL_Log("'%s' is in several sections: %s", name, options);
	}
	return false;
}

static void say_where(potop_t *potop)
{
	const camera_t *camera = &potop->camera;
	float yaw;
	float pitch;

	camera_get_heading(camera, &yaw, &pitch);
	if (camera->first_person)
	{
		SDL_Log("On foot at x %.1f z %.1f, feet at %.1f m; yaw %.1f, pitch %.1f", (double)camera->walker_x, (double)camera->walker_z,
			(double)camera->walker_feet, (double)yaw, (double)pitch);
	}
	else
	{
		SDL_Log("Looking at x %.1f z %.1f (ground %.1f m) from %.1f m; yaw %.1f, pitch %.1f", (double)camera->target_x,
			(double)camera->target_z, (double)camera->target_height, (double)camera->distance_meters, (double)yaw, (double)pitch);
	}
}

void console_run_command(console_t *console, potop_t *potop, const char *line, console_actions_t *actions)
{
	const char *cursor = line;
	char command[32];
	char first[96];
	char second[96];
	char third[96];
	bool has_first;
	bool has_second;
	bool has_third;
	const char *after_first;
	const char *after_command;

	SDL_zerop(actions);
	SDL_Log("> %s", line);
	if (!next_word(&cursor, command, sizeof(command)))
	{
		return;
	}
	after_command = cursor;
	has_first = next_word(&cursor, first, sizeof(first));
	after_first = cursor;
	has_second = next_word(&cursor, second, sizeof(second));
	has_third = next_word(&cursor, third, sizeof(third));

	if (SDL_strcmp(command, "help") == 0)
	{
		for (int i = 0; i < console_command_count; i++)
		{
			const console_command_t *entry = &console_commands[i];

			if (!has_first || SDL_strcmp(first, entry->name) == 0)
			{
				SDL_Log("  %s %s  -  %s", entry->name, entry->usage, entry->help);
			}
		}
		for (int i = 0; i < console->added_count; i++)
		{
			const console_added_command_t *entry = &console->added[i];

			if (!has_first || SDL_strcmp(first, entry->name) == 0)
			{
				SDL_Log("  %s %s  -  %s", entry->name, entry->usage, entry->help);
			}
		}
		if (!has_first)
		{
			SDL_Log("  Tab completes, Up and Down bring back what was typed, Page Up and Down scroll, ` or Escape closes");
		}
	}
	else if (SDL_strcmp(command, "list") == 0)
	{
		const char *section;
		const char *key;
		int shown = 0;

		for (size_t i = 0; settings_field_name(i, &section, &key); i++)
		{
			char name[128];
			char value[64];

			SDL_snprintf(name, sizeof(name), "%s.%s", section, key);
			if (has_first && SDL_strstr(name, first) == NULL)
			{
				continue;
			}
			settings_get_text(&potop->settings, section, key, value, sizeof(value));
			SDL_Log("  %s = %s", name, value);
			shown++;
		}
		if (shown == 0)
		{
			SDL_Log("No setting has '%s' in its name", has_first ? first : "");
		}
	}
	else if (SDL_strcmp(command, "get") == 0 || SDL_strcmp(command, "set") == 0)
	{
		bool set = command[0] == 's';
		char section[32];
		const char *key;
		char value[64];

		if (!has_first || (set && !has_second))
		{
			SDL_Log(set ? "set <setting> <value>" : "get <setting>");
			return;
		}
		if (!find_setting(first, section, sizeof(section), &key))
		{
			return;
		}
		if (set)
		{
			// The value is the rest of the line (a color is "r, g, b")
			char text[CONSOLE_INPUT_LENGTH];
			size_t length;

			while (*after_first == ' ' || *after_first == '\t')
			{
				after_first++;
			}
			SDL_strlcpy(text, after_first, sizeof(text));
			length = SDL_strlen(text);
			while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t'))
			{
				text[--length] = '\0';
			}
			if (!settings_set_text(&potop->settings, section, key, text))
			{
				SDL_Log("'%s' does not read as a value of %s.%s", text, section, key);
				return;
			}
		}
		settings_get_text(&potop->settings, section, key, value, sizeof(value));
		SDL_Log("  %s.%s = %s", section, key, value);
	}
	else if (SDL_strcmp(command, "mapset") == 0 || SDL_strcmp(command, "mapunset") == 0)
	{
		bool set = command[3] == 's';
		char section[32];
		const char *key;
		char full_key[MAP_SETTING_KEY_LENGTH];
		char text[CONSOLE_INPUT_LENGTH];
		char value[64];

		if (!has_first || (set && !has_second))
		{
			SDL_Log(set ? "mapset <setting> <value>" : "mapunset <setting>");
			return;
		}
		if (!find_setting(first, section, sizeof(section), &key))
		{
			return;
		}
		SDL_snprintf(full_key, sizeof(full_key), "%s.%s", section, key);
		text[0] = '\0';
		if (set)
		{
			// The value is the rest of the line (a color is "r, g, b")
			size_t length;

			while (*after_first == ' ' || *after_first == '\t')
			{
				after_first++;
			}
			SDL_strlcpy(text, after_first, sizeof(text));
			length = SDL_strlen(text);
			while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t'))
			{
				text[--length] = '\0';
			}
		}
		if (!map_settings_set(&potop->world.map_settings, full_key, text))
		{
			SDL_Log(set ? "'%s' does not read as a value of %s (or the map has %d of its own already)" : "%s: cannot", set ? text : full_key,
				full_key, MAP_SETTINGS_MAX);
			return;
		}
		world_refresh_settings(&potop->world);
		settings_get_text(potop->world.settings, section, key, value, sizeof(value));
		SDL_Log("  the map's %s %s; the world goes by %s (saved with the map)", full_key, set ? text : "taken away", value);
	}
	else if (SDL_strcmp(command, "mapsettings") == 0)
	{
		const map_settings_t *map = &potop->world.map_settings;

		if (map->count == 0)
		{
			SDL_Log("The map has no settings of its own");
		}
		for (int i = 0; i < map->count; i++)
		{
			const char *dot = SDL_strchr(map->entries[i].key, '.');
			char section[32];
			char mine[64];
			char used[64];

			mine[0] = used[0] = '\0';
			if (dot != NULL)
			{
				SDL_strlcpy(section, map->entries[i].key, SDL_min(sizeof(section), (size_t)(dot - map->entries[i].key) + 1));
				settings_get_text(&potop->settings, section, dot + 1, mine, sizeof(mine));
				settings_get_text(potop->world.settings, section, dot + 1, used, sizeof(used));
			}
			SDL_Log("  %s = %s (the player's %s: the world goes by %s)", map->entries[i].key, map->entries[i].value, mine, used);
		}
	}
	else if (SDL_strcmp(command, "apply") == 0)
	{
		actions->apply_render = true;
		SDL_Log("Applying the render settings");
	}
	else if (SDL_strcmp(command, "where") == 0)
	{
		say_where(potop);
	}
	else if (SDL_strcmp(command, "goto") == 0)
	{
		float x;
		float z;
		float height = NAN;

		if (!has_second || !read_number(first, &x) || !read_number(second, &z) || (has_third && !read_number(third, &height)))
		{
			SDL_Log("goto <x> <z> [height]  (meters)");
			return;
		}
		camera_go_to(&potop->camera, &potop->world.terrain, x, height, z);
		say_where(potop);
	}
	else if (SDL_strcmp(command, "bake") == 0 && has_first && SDL_strcmp(first, "paths") == 0)
	{
		potop_bake_paths(potop);
	}
	else if (SDL_strcmp(command, "log") == 0 && has_first && SDL_strcmp(first, "paths") == 0)
	{
		// (the walkers' ways into the log, as the tests have them: each
		// search, its waypoints, where one is stuck and why)
		// (with a number: only of those within so many meters of the eye)
		float meters = 0.0f;

		if (has_second && read_number(second, &meters) && meters > 0.0f)
		{
			potop->world.actors.log_paths = true;
			potop->world.actors.log_paths_meters = meters;
			SDL_Log("The ways of the walkers within %.0f m of you are logged", (double)meters);
		}
		else
		{
			potop->world.actors.log_paths = !potop->world.actors.log_paths;
			potop->world.actors.log_paths_meters = 0.0f;
			SDL_Log("The walkers' ways are %s", potop->world.actors.log_paths ? "logged" : "not logged");
		}
	}
	else if (SDL_strcmp(command, "navmesh") == 0)
	{
		char text[768];

		navigation_mesh_describe(&potop->world.navigation, text, sizeof(text));
		SDL_Log("%s", text);
	}
	else if (SDL_strcmp(command, "path") == 0)
	{
		// (from where you stand to x z, for an actor type: the points its
		// walk would take)
		float x;
		float z;
		float points[256][2];
		bool complete = false;
		int count;

		if (!has_third || !read_number(second, &x) || !read_number(third, &z))
		{
			SDL_Log("path <actor type> <x> <z>  (from where you stand)");
			return;
		}
		count = potop_find_path(potop, first, potop->camera.walker_x, potop->camera.walker_z, x, z, points, 256, &complete);
		SDL_Log("path for %s from %.2f %.2f to %.2f %.2f: %d points%s", first, potop->camera.walker_x, potop->camera.walker_z, x, z, count,
			complete ? "" : " (not all the way)");
		for (int k = 0; k < count; k++)
		{
			int type = actors_find_type(&potop->world.actors, first);
			navigation_agent_t agent = actors_agent(&potop->world.actors, type);
			float ground = terrain_sample_height_meters(&potop->world.terrain, points[k][0], points[k][1]);

			SDL_Log("  %.2f %.2f  (ground %.2f, its surface %.2f)", points[k][0], points[k][1], ground,
				navigation_surface_height(&potop->world.navigation, &potop->world.terrain, &agent, points[k][0], points[k][1], ground + 0.5f));
		}
	}
	else if (SDL_strcmp(command, "water") == 0)
	{
		// (the CPU's copy of the water: read back from the GPU as tiles
		// change, a step or so behind)
		water_t *water = &potop->world.water;
		float x;
		float z;
		int32_t cell_x;
		int32_t cell_y;
		int32_t tile;
		char line[256];

		if (has_second)
		{
			if (!read_number(first, &x) || !read_number(second, &z))
			{
				SDL_Log("water [x z]");
				return;
			}
		}
		else
		{
			// Where the middle of the screen looks: the nearer of the
			// objects (a roof) and the ground
			int pixel_width = 1;
			int pixel_height = 1;
			vec3_t origin;
			vec3_t direction;
			vec3_t ground_hit;
			object_ray_hit_t hit;
			float nearest = potop->camera.far_distance;
			bool found = false;

			SDL_GetWindowSizeInPixels(potop->window, &pixel_width, &pixel_height);
			camera_pixel_ray(&potop->camera, (float)pixel_width * 0.5f, (float)pixel_height * 0.5f, pixel_width, pixel_height, &origin,
				&direction);
			if (terrain_raycast(&potop->world.terrain, origin, direction, nearest, &ground_hit))
			{
				nearest = vec3_length(vec3_subtract(ground_hit, origin));
				x = ground_hit.x;
				z = ground_hit.z;
				found = true;
			}
			if (objects_raycast(&potop->world.objects, origin, direction, nearest, OBJECTS_RAY_ALL, &hit, 1) > 0 && hit.distance < nearest)
			{
				x = hit.point.x;
				z = hit.point.z;
				found = true;
			}
			if (!found)
			{
				SDL_Log("water: the middle of the screen looks at nothing (water x z)");
				return;
			}
		}
		if (water->depth == NULL)
		{
			SDL_Log("water: no water simulation on this map");
			return;
		}
		cell_x = (int32_t)SDL_floorf(x / water->cell_size_meters + 0.5f);
		cell_y = (int32_t)SDL_floorf(z / water->cell_size_meters + 0.5f);
		if (cell_x < 0 || cell_y < 0 || cell_x >= water->size || cell_y >= water->size)
		{
			SDL_Log("water: %.1f %.1f is off the map", (double)x, (double)z);
			return;
		}
		tile = water_tile_index_of_cell(water, cell_x, cell_y);
		{
			static const char *const state_names[] = { "dry", "sleeping", "active" };
			int index = cell_y * water->size + cell_x;
			int state = water->tile_state[tile];

			SDL_Log("water at %.2f %.2f: cell %d %d, ground %.3f m, depth %.1f mm, wall %u cm%s; tile %d (%s, max depth %.1f mm)",
				(double)x, (double)z, cell_x, cell_y, (double)terrain_sample_height_meters(&potop->world.terrain,
				(float)cell_x * water->cell_size_meters, (float)cell_y * water->cell_size_meters),
				(double)((float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT * 1000.0f),
				water->obstacle_centimeters != NULL ? (unsigned)water->obstacle_centimeters[index] : 0u, water->baked ? " (baked)" : "",
				tile, state >= 0 && state < 3 ? state_names[state] : "?",
				(double)((float)water->tile_max_depth[tile] * TERRAIN_METERS_PER_HEIGHT_UNIT * 1000.0f));
		}
		// The cells round it: depth in mm (. dry), the wall's height in cm
		// after a slash where there is one; north (smaller z) on top
		for (int dy = -2; dy <= 2; dy++)
		{
			int length = SDL_snprintf(line, sizeof(line), "  z %5d:", cell_y + dy);

			for (int dx = -2; dx <= 2; dx++)
			{
				int at_x = cell_x + dx;
				int at_y = cell_y + dy;
				char part[32];

				if (at_x < 0 || at_y < 0 || at_x >= water->size || at_y >= water->size)
				{
					SDL_strlcpy(part, "       -    ", sizeof(part));
				}
				else
				{
					int at = at_y * water->size + at_x;
					float millimeters = (float)water->depth[at] * TERRAIN_METERS_PER_HEIGHT_UNIT * 1000.0f;
					unsigned wall = water->obstacle_centimeters != NULL ? water->obstacle_centimeters[at] : 0u;
					char depth_text[16];

					if (millimeters <= 0.0f)
					{
						SDL_strlcpy(depth_text, ".", sizeof(depth_text));
					}
					else
					{
						SDL_snprintf(depth_text, sizeof(depth_text), "%.1f", (double)millimeters);
					}
					if (wall > 0u)
					{
						SDL_snprintf(part, sizeof(part), " %7s/%-4u%s", depth_text, wall, dx == 0 && dy == 0 ? "*" : " ");
					}
					else
					{
						SDL_snprintf(part, sizeof(part), " %7s     %s", depth_text, dx == 0 && dy == 0 ? "*" : " ");
					}
				}
				if (length > 0 && length < (int)sizeof(line))
				{
					length += SDL_snprintf(line + length, sizeof(line) - (size_t)length, "%s", part);
				}
			}
			SDL_Log("%s", line);
		}
	}
	else if (SDL_strcmp(command, "cell") == 0)
	{
		// (what an actor type finds at x z: navigation_describe)
		float x;
		float z;
		char text[512];
		int type;
		navigation_agent_t agent;

		if (!has_third || !read_number(second, &x) || !read_number(third, &z) || (type = actors_find_type(&potop->world.actors, first)) < 0)
		{
			SDL_Log("cell <actor type> <x> <z>");
			return;
		}
		agent = actors_agent(&potop->world.actors, type);
		// (the cell as its surfaces have it, there again if they were let go)
		navigation_ensure_cells(&potop->world.navigation, x, z);
		// (the surface nearest the ground, then any above it, a meter at a time)
		for (int up = 0; up <= 4; up++)
		{
			float near = terrain_sample_height_meters(&potop->world.terrain, x, z) + (float)up;

			navigation_describe(&potop->world.navigation, &potop->world.terrain, &potop->world.water, &potop->world.objects, &agent, x, z, near,
				text, sizeof(text));
			SDL_Log("cell %.2f %.2f near %.1f: %s", x, z, near, text);
		}
	}
	else if (SDL_strcmp(command, "module") == 0)
	{
		objects_t *objects = &potop->world.objects;
		const camera_t *camera = &potop->camera;
		float x = camera->first_person ? camera->walker_x : camera->target_x;
		float z = camera->first_person ? camera->walker_z : camera->target_z;
		int index = objects_nearest_module(objects, x, z, 40.0f);
		const module_file_t *module = objects_module_of(objects, index);
		const object_instance_t *instance;

		if (module == NULL)
		{
			SDL_Log("module: no module within 40 m of (%.1f, %.1f)", (double)x, (double)z);
			return;
		}
		instance = &objects->instances[index];
		if (has_first)
		{
			if (!objects_set_module_state(objects, instance->id, first, -1))
			{
				SDL_Log("module: '%s' has no state '%s'", objects->types.types[instance->type].name, first);
				return;
			}
		}
		SDL_Log("module '%s' (id %u) at (%.1f, %.1f): in '%s'%s%s", objects->types.types[instance->type].name, (unsigned)instance->id,
			(double)instance->x, (double)instance->z, module->states[instance->module_state].name,
			instance->module_target != instance->module_state ? ", going to " : "",
			instance->module_target != instance->module_state ? module->states[instance->module_target].name : "");
		for (int s = 0; s < module->state_count; s++)
		{
			SDL_Log("  state '%s': %.1f s to get there%s%s", module->states[s].name, (double)module->states[s].seconds,
				module->states[s].next >= 0 ? ", then on to " : "", module->states[s].next >= 0 ? module->states[module->states[s].next].name : "");
		}
	}
	else if (SDL_strcmp(command, "camera") == 0)
	{
		float values[6];
		int count = 0;

		if (has_first && SDL_strcmp(first, "off") == 0)
		{
			potop->camera.fixed = false;
			SDL_Log("Camera: back to the walker");
			return;
		}
		// (the first number is the first word; the rest after it)
		if (has_first && read_number(first, &values[0]))
		{
			count = 1 + SDL_sscanf(after_first, "%f %f %f %f %f", &values[1], &values[2], &values[3], &values[4], &values[5]);
		}
		if (count != 3 && count != 6)
		{
			SDL_Log("camera <x> <y> <z> [<at x> <at y> <at z>] | off  (meters; first person)");
			return;
		}
		if (!potop->camera.first_person)
		{
			camera_set_first_person(&potop->camera, &potop->settings.camera, &potop->world.terrain, true);
		}
		potop->camera.fixed = true;
		potop->camera.fixed_eye[0] = values[0];
		potop->camera.fixed_eye[1] = values[1];
		potop->camera.fixed_eye[2] = values[2];
		if (count == 6)
		{
			camera_look_at(&potop->camera, &potop->settings.camera, values[3], values[4], values[5]);
		}
		SDL_Log("Camera: the eye at (%.2f, %.2f, %.2f)%s", (double)values[0], (double)values[1], (double)values[2], count == 6 ? ", looking at the point" : "");
	}
	else if (SDL_strcmp(command, "look") == 0)
	{
		float yaw;
		float pitch;

		camera_get_heading(&potop->camera, &yaw, &pitch);
		if (!has_first || !read_number(first, &yaw) || (has_second && !read_number(second, &pitch)))
		{
			SDL_Log("look <yaw> [pitch]  (degrees)");
			return;
		}
		camera_set_heading(&potop->camera, &potop->settings.camera, yaw, pitch);
		say_where(potop);
	}
	else if (SDL_strcmp(command, "distance") == 0)
	{
		float distance;

		if (!has_first || !read_number(first, &distance))
		{
			SDL_Log("distance <meters>");
			return;
		}
		potop->camera.distance_meters = SDL_clamp(distance, potop->settings.camera.minimum_distance_meters,
			potop->settings.camera.maximum_distance_meters);
		say_where(potop);
	}
	else if (SDL_strcmp(command, "time") == 0)
	{
		float speed;

		if (has_first)
		{
			if (!read_number(first, &speed) || speed < 0.0f)
			{
				SDL_Log("time [speed]  (0 or more: 1 as the game, 60 a minute a second)");
				return;
			}
			potop->world.sky_time_scale = speed;
		}
		SDL_Log("The sky's clock goes %g times as fast as the game's (at %.0f s)", (double)potop->world.sky_time_scale, potop->world.sky_seconds);
	}
	else if (SDL_strcmp(command, "hour") == 0)
	{
		float hour;

		if (has_first)
		{
			if (!read_number(first, &hour) || hour < 0.0f || hour > 24.0f)
			{
				SDL_Log("hour [0..24]  (18.5 is half past six in the evening)");
				return;
			}
			potop->world.day_hour = SDL_fmod((double)hour, 24.0);
			daylight_apply(&potop->settings.render, potop->world.day_hour, potop->world.moon_age_days, &potop->world.lit_render, &potop->world.daylight);
		}
		SDL_Log("It is %02d:%02d%s%s", (int)potop->world.day_hour, (int)(SDL_fmod(potop->world.day_hour, 1.0) * 60.0),
			potop->world.day_frozen ? " (frozen)" : "", potop->settings.render.day_cycle ? "" : " (the day cycle is off: render.day_cycle)");
	}
	else if (SDL_strcmp(command, "moon") == 0)
	{
		static const char *const phases[8] = { "new", "a waxing crescent", "first quarter", "waxing gibbous", "full",
			"waning gibbous", "last quarter", "a waning crescent" };
		float age;
		float cycle = SDL_max(potop->settings.render.moon_cycle_days, 1.0f);

		if (has_first)
		{
			if (!read_number(first, &age))
			{
				SDL_Log("moon [age]  (days since new: 0 new, %.1f full)", (double)(0.5f * cycle));
				return;
			}
			potop->world.moon_age_days = SDL_fmod(SDL_fmod((double)age, (double)cycle) + (double)cycle, (double)cycle);
			daylight_apply(&potop->settings.render, potop->world.day_hour, potop->world.moon_age_days, &potop->world.lit_render, &potop->world.daylight);
		}
		SDL_Log("The moon is %.1f days old of %.1f: %s, %.0f%% lit, %s (azimuth %.0f, elevation %.0f degrees)", potop->world.moon_age_days,
			(double)cycle, phases[(int)SDL_floorf(potop->world.daylight.moon_phase * 8.0f + 0.5f) % 8], (double)(potop->world.daylight.moon_lit * 100.0f),
			potop->world.daylight.moon_elevation_degrees > 0.0f ? "up" : "down", (double)potop->world.daylight.moon_azimuth_degrees,
			(double)potop->world.daylight.moon_elevation_degrees);
	}
	else if (SDL_strcmp(command, "objects") == 0)
	{
		if (has_first && SDL_strcmp(first, "impostors") == 0)
		{
			// To look at them: every object that has one drawn as its
			// impostor, near or far, or as far as they should be
			if (has_second && (SDL_strcmp(second, "all") == 0 || SDL_strcmp(second, "auto") == 0))
			{
				potop->world.objects.impostors_everywhere = SDL_strcmp(second, "all") == 0;
			}
			else if (has_second && (SDL_strcmp(second, "show") == 0 || SDL_strcmp(second, "hide") == 0))
			{
				potop->settings.objects.show_impostors = SDL_strcmp(second, "show") == 0;
			}
			SDL_Log("Impostors: %s%s", potop->world.objects.impostors_everywhere ? "everywhere" : "far away",
				potop->settings.objects.impostors ? "" : " (but off: objects.impostors)");
			return;
		}
		if (has_first && SDL_strcmp(first, "windows") == 0)
		{
			// The houses' windows (their emissive groups named "window"): by
			// the hour, on their own (auto), or all lit, or all dark (on,
			// off: the hour left alone till auto again)
			objects_t *objects = &potop->world.objects;

			if (has_second && SDL_strcmp(second, "auto") == 0)
			{
				objects->windows_by_hour = true;
				objects->windows_minute = -1;
			}
			else if (has_second && (SDL_strcmp(second, "on") == 0 || SDL_strcmp(second, "off") == 0))
			{
				bool lit = SDL_strcmp(second, "on") == 0;
				int houses = 0;

				objects->windows_by_hour = false;
				for (int i = 0; i < objects->instance_count; i++)
				{
					int count = objects_emissive_group_count(objects, i);

					for (int g = 1; g <= count; g++)
					{
						if (SDL_strncmp(objects_emissive_group(objects, i, g)->name, "window", 6) == 0)
						{
							objects_set_emissive(objects, i, g, lit);
						}
					}
					houses += count > 0 ? 1 : 0;
				}
				SDL_Log("Windows: all %s (%d objects with emissive groups)", lit ? "lit" : "dark", houses);
				return;
			}
			SDL_Log("Windows: %s", objects->windows_by_hour ? "by the hour" : "as last set (objects windows auto: by the hour)");
			return;
		}
		if (has_first && SDL_strcmp(first, "age") == 0)
		{
			// Every building as old as asked (0 new .. 1 the oldest: its
			// streaks, its worn edges, the moss under its jetties), none
			// more or less by where it stands; off: each its type's again
			if (has_second && SDL_strcmp(second, "off") == 0)
			{
				potop->world.objects.age_overridden = false;
			}
			else if (has_second)
			{
				potop->world.objects.age_overridden = true;
				potop->world.objects.age_override = SDL_clamp((float)SDL_atof(second), 0.0f, 1.0f);
			}
			if (potop->world.objects.age_overridden)
			{
				SDL_Log("Buildings' age: %.2f for all", (double)potop->world.objects.age_override);
			}
			else
			{
				SDL_Log("Buildings' age: each its type's (objects.ini)");
			}
			return;
		}
		if (has_first && SDL_strcmp(first, "relief") == 0)
		{
			// A thatch's relief (parallax) marched into, or the surface flat
			// with its normals alone (to compare, and what it costs)
			if (has_second && (SDL_strcmp(second, "on") == 0 || SDL_strcmp(second, "off") == 0))
			{
				potop->world.objects.relief_off = SDL_strcmp(second, "off") == 0;
			}
			SDL_Log("Relief (thatch parallax): %s", potop->world.objects.relief_off ? "off" : "on");
			return;
		}
		if (has_first && SDL_strcmp(first, "occlusion") == 0)
		{
			// What the last frame's depth hid (the terrain, the houses, the
			// rest: hiz.h) left out, or drawn all the same (to see whether
			// the occlusion culling hides what it should not)
			// (show: the boxes it hid outlined over the scene, in red, the
			// pyramid's texels read for each in yellow; depth: the pyramid
			// itself under them too; all: those it let be drawn too, in
			// green; hide: none)
			if (has_second && (SDL_strcmp(second, "on") == 0 || SDL_strcmp(second, "off") == 0))
			{
				potop->world.objects.no_terrain_occlusion = SDL_strcmp(second, "off") == 0;
			}
			else if (has_second && (SDL_strcmp(second, "show") == 0 || SDL_strcmp(second, "depth") == 0 || SDL_strcmp(second, "all") == 0
				|| SDL_strcmp(second, "hide") == 0))
			{
				potop->world.objects.occlusion_shown = SDL_strcmp(second, "hide") != 0;
				potop->world.objects.occlusion_depth_shown = SDL_strcmp(second, "depth") == 0;
				potop->world.objects.occlusion_all_shown = SDL_strcmp(second, "all") == 0;
			}
			SDL_Log("Occlusion culling: %s, %s", potop->world.objects.no_terrain_occlusion ? "off" : "on",
				!potop->world.objects.occlusion_shown ? "not shown" : (potop->world.objects.occlusion_depth_shown ? "shown with the pyramid"
				: (potop->world.objects.occlusion_all_shown ? "shown, the drawn too" : "shown")));
			return;
		}
		SDL_Log("objects impostors [all|auto|show|hide], objects occlusion [on|off|show|depth|all|hide], objects relief [on|off]");
	}
	else if (SDL_strcmp(command, "aurora") == 0)
	{
		// The game's say (aurora_set): on (with a strength) or off
		float strength = potop->world.aurora.strength > 0.0f ? potop->world.aurora.strength : 1.0f;

		if (has_first && (SDL_strcmp(first, "on") == 0 || SDL_strcmp(first, "off") == 0))
		{
			if (has_second && !read_number(second, &strength))
			{
				SDL_Log("aurora [on|off] [strength]");
				return;
			}
			aurora_set(&potop->world.aurora, SDL_strcmp(first, "on") == 0, strength);
		}
		else if (has_first)
		{
			SDL_Log("aurora [on|off] [strength]");
			return;
		}
		SDL_Log("Aurora: %s (strength %.2f%s)", potop->world.aurora.on ? "on" : "off", (double)potop->world.aurora.strength,
			potop->world.aurora.on && potop->world.daylight.sun_elevation_degrees > -4.0f ? ", waiting for the dark" : "");
	}
	else if (SDL_strcmp(command, "rain") == 0)
	{
		// The simulation's rain (water.raining, the R key's), and how hard
		// it looks (render.rain_intensity)
		float intensity = potop->settings.render.rain_intensity;

		if (has_first && (SDL_strcmp(first, "on") == 0 || SDL_strcmp(first, "off") == 0))
		{
			if (has_second && (!read_number(second, &intensity) || intensity < 0.0f || intensity > 1.0f))
			{
				SDL_Log("rain [on|off] [intensity 0..1]");
				return;
			}
			world_touch_water(&potop->world);
			potop->world.water.raining = SDL_strcmp(first, "on") == 0;
			potop->settings.render.rain_intensity = intensity;
		}
		else if (has_first)
		{
			SDL_Log("rain [on|off] [intensity 0..1]");
			return;
		}
		SDL_Log("Rain: %s (intensity %.2f)", potop->world.water.raining ? "on" : "off", (double)potop->settings.render.rain_intensity);
	}
	else if (SDL_strcmp(command, "rainbow") == 0)
	{
		// The game's say (rainbow_set): on (with a strength) or off
		float strength = potop->world.rain.rainbow_strength > 0.0f ? potop->world.rain.rainbow_strength : 1.0f;

		if (has_first && (SDL_strcmp(first, "on") == 0 || SDL_strcmp(first, "off") == 0))
		{
			if (has_second && !read_number(second, &strength))
			{
				SDL_Log("rainbow [on|off] [strength]");
				return;
			}
			rainbow_set(&potop->world.rain, SDL_strcmp(first, "on") == 0, strength);
		}
		else if (has_first)
		{
			SDL_Log("rainbow [on|off] [strength]");
			return;
		}
		SDL_Log("Rainbow: %s (strength %.2f)%s", potop->world.rain.rainbow_on ? "on" : "off", (double)potop->world.rain.rainbow_strength,
			potop->world.rain.rainbow_on && (potop->world.daylight.sun_elevation_degrees < 0.0f || potop->world.daylight.sun_elevation_degrees > 42.0f)
				? ", but the sun is not where one shows (above the horizon, under 42 degrees)" : "");
	}
	else if (SDL_strcmp(command, "decal") == 0)
	{
		// A decal where the middle of the screen looks (the ground or what
		// stands there), for trying them out
		decals_t *decals = &potop->world.decals;
		int kind = has_first ? decals_find_kind(decals, first) : -1;
		float size = 0.0f;
		vec3_t origin;
		vec3_t direction;
		float from[3];
		float to[3];
		physics_cast_t cast;

		if (has_first && SDL_strcmp(first, "clear") == 0)
		{
			decals_clear(decals);
			SDL_Log("Decals: all gone");
		}
		else if (kind < 0)
		{
			SDL_Log("decal <kind> [size meters], or decal clear; the kinds (data/decals.ini):");
			for (int i = 0; i < decals->kind_count; i++)
			{
				SDL_Log("  %s", decals->kinds[i].name);
			}
		}
		else
		{
			if (has_second)
			{
				read_number(second, &size);
			}
			camera_pixel_ray(&potop->camera, 0.5f, 0.5f, 1, 1, &origin, &direction);
			from[0] = origin.x;
			from[1] = origin.y;
			from[2] = origin.z;
			to[0] = origin.x + direction.x * 500.0f;
			to[1] = origin.y + direction.y * 500.0f;
			to[2] = origin.z + direction.z * 500.0f;
			// (what stands there, or the ground, whichever is nearer)
			{
				vec3_t ground_hit;
				bool on_ground = terrain_raycast(&potop->world.terrain, origin, direction, 500.0f, &ground_hit);
				bool on_body = physics_cast_segment(&potop->world.physics, from, to, &cast);
				float ground_distance = on_ground ? vec3_length(vec3_subtract(ground_hit, origin)) : 1e30f;

				if (on_ground && (!on_body || ground_distance < cast.fraction * 500.0f))
				{
					const terrain_t *terrain = &potop->world.terrain;
					float step = terrain->cell_size_meters;
					float slope_x = terrain_sample_height_meters(terrain, ground_hit.x + step, ground_hit.z)
						- terrain_sample_height_meters(terrain, ground_hit.x - step, ground_hit.z);
					float slope_z = terrain_sample_height_meters(terrain, ground_hit.x, ground_hit.z + step)
						- terrain_sample_height_meters(terrain, ground_hit.x, ground_hit.z - step);
					float length = SDL_sqrtf(slope_x * slope_x + 4.0f * step * step + slope_z * slope_z);

					cast.point[0] = ground_hit.x;
					cast.point[1] = ground_hit.y;
					cast.point[2] = ground_hit.z;
					cast.normal[0] = -slope_x / length;
					cast.normal[1] = 2.0f * step / length;
					cast.normal[2] = -slope_z / length;
					on_body = true;
				}
				if (on_body)
				{
					decals_add(decals, kind, cast.point, cast.normal, size, potop->world.elapsed_seconds);
					SDL_Log("Decal '%s' at x %.2f y %.2f z %.2f (%d now)", decals->kinds[kind].name, (double)cast.point[0], (double)cast.point[1],
						(double)cast.point[2], decals->count);
				}
				else
				{
					SDL_Log("Decal: nothing there (look at the ground or at something standing)");
				}
			}
		}
	}
	else if (SDL_strcmp(command, "wreck") == 0)
	{
		vec3_t origin;
		vec3_t direction;
		vec3_t ground_hit;

		if (!has_first)
		{
			SDL_Log("wreck <name> | clear  (data/wrecks/NAME.wreck, where the middle of the screen looks at the ground)");
			return;
		}
		if (SDL_strcmp(first, "clear") == 0)
		{
			wrecks_clear(&potop->world.wrecks, &potop->world.objects);
			SDL_Log("Wrecks: all taken away");
			return;
		}
		camera_pixel_ray(&potop->camera, 0.5f, 0.5f, 1, 1, &origin, &direction);
		// The ball gun's ball (explosives.ini's ball) thrown from the eye
		// where the screen's middle looks, as the weapon throws it
		if (SDL_strcmp(first, "ball") == 0)
		{
			float from[3] = { origin.x + direction.x * 0.9f, origin.y + direction.y * 0.9f, origin.z + direction.z * 0.9f };
			float towards[3] = { direction.x, direction.y, direction.z };

			explosives_throw(&potop->session.explosives, &potop->world, explosives_find(&potop->session.explosives, "ball"), from, towards, -1);
			return;
		}
		// A blast of so many kilograms where the middle of the screen looks
		// (the first thing there: a piece, the ground), on the buildings alone
		if (SDL_strcmp(first, "blast") == 0)
		{
			float from[3] = { origin.x, origin.y, origin.z };
			float to[3] = { origin.x + direction.x * 300.0f, origin.y + direction.y * 300.0f, origin.z + direction.z * 300.0f };
			float kilograms = SDL_max((float)SDL_atof(after_first), 0.0f);
			physics_cast_t cast;

			if (kilograms <= 0.0f)
			{
				kilograms = 3.0f;
			}
			if (!physics_cast_segment(&potop->world.physics, from, to, &cast))
			{
				// (the ground beyond the physics' tiles)
				if (!terrain_raycast(&potop->world.terrain, origin, direction, 300.0f, &ground_hit))
				{
					SDL_Log("Wrecks: nothing there for a blast (300 m at most)");
					return;
				}
				cast.point[0] = ground_hit.x;
				cast.point[1] = ground_hit.y + 0.5f;
				cast.point[2] = ground_hit.z;
			}
			{
				// (a little back from what it hit, towards the eye)
				float at[3] = { cast.point[0] - direction.x * 0.2f, cast.point[1] - direction.y * 0.2f, cast.point[2] - direction.z * 0.2f };

				wrecks_blast(&potop->world.wrecks, at, kilograms);
			}
			return;
		}
		if (!terrain_raycast(&potop->world.terrain, origin, direction, 300.0f, &ground_hit))
		{
			SDL_Log("Wrecks: look at the ground where it should stand (300 m at most)");
			return;
		}
		// (its front, +Z, towards the one placing it)
		wrecks_place(&potop->world.wrecks, potop->world.physics.world, &potop->world.objects, &potop->world.terrain, first, ground_hit.x,
			ground_hit.z, SDL_atan2f(origin.x - ground_hit.x, origin.z - ground_hit.z));
	}
	else if (SDL_strcmp(command, "burn") == 0)
	{
		vec3_t origin;
		vec3_t direction;
		object_ray_hit_t hit;

		if (has_first && SDL_strcmp(first, "clear") == 0)
		{
			burning_clear(&potop->world.burning, &potop->world.objects, &potop->world.particles);
			SDL_Log("Burning: every fire out");
			return;
		}
		camera_pixel_ray(&potop->camera, 0.5f, 0.5f, 1, 1, &origin, &direction);
		if (objects_raycast(&potop->world.objects, origin, direction, 60.0f, OBJECTS_RAY_ALL, &hit, 1) < 1)
		{
			SDL_Log("Burning: no object there (look at one, 60 m at most)");
			return;
		}
		{
			float point[3] = { hit.point.x, hit.point.y, hit.point.z };
			const object_instance_t *instance = &potop->world.objects.instances[hit.instance];

			SDL_Log("Burning: '%s' (object %u) at (%.2f, %.2f, %.2f)", potop->world.objects.types.types[instance->type].name, (unsigned)instance->id,
				(double)point[0], (double)point[1], (double)point[2]);
			if (!burning_ignite(&potop->world.burning, &potop->world.objects, &potop->world.particles, hit.instance, point))
			{
				SDL_Log("Burning: it is burning already, or there are as many fires as there can be");
			}
		}
	}
	else if (SDL_strcmp(command, "effect") == 0)
	{
		char name[PARTICLES_EFFECT_NAME_LENGTH];
		float x;
		float y;
		float z;
		int effect;

		if (has_first && SDL_strcmp(first, "stop") == 0)
		{
			particles_stop_all(&potop->world.particles);
			SDL_Log("Effects: all stopped");
			return;
		}
		if (!has_first)
		{
			SDL_Log("effect <name> [<x> <y> <z>] | stop  (a particles.ini effect where the middle of the screen looks, or at a point: meters, y its height)");
			return;
		}
		if (SDL_sscanf(after_first, "%f %f %f", &x, &y, &z) != 3)
		{
			// (where the middle of the screen looks: the ground or what
			// stands there, whichever is nearer)
			vec3_t origin;
			vec3_t direction;
			vec3_t ground_hit;
			float from[3];
			float to[3];
			physics_cast_t cast;
			bool on_ground;
			bool on_body;
			float ground_distance;

			camera_pixel_ray(&potop->camera, 0.5f, 0.5f, 1, 1, &origin, &direction);
			from[0] = origin.x;
			from[1] = origin.y;
			from[2] = origin.z;
			to[0] = origin.x + direction.x * 500.0f;
			to[1] = origin.y + direction.y * 500.0f;
			to[2] = origin.z + direction.z * 500.0f;
			on_ground = terrain_raycast(&potop->world.terrain, origin, direction, 500.0f, &ground_hit);
			on_body = physics_cast_segment(&potop->world.physics, from, to, &cast);
			ground_distance = on_ground ? vec3_length(vec3_subtract(ground_hit, origin)) : 1e30f;
			if (on_body && ground_distance > cast.fraction * 500.0f)
			{
				x = cast.point[0];
				y = cast.point[1];
				z = cast.point[2];
			}
			else if (on_ground)
			{
				x = ground_hit.x;
				y = ground_hit.y;
				z = ground_hit.z;
			}
			else
			{
				SDL_Log("Effects: nothing there (look at the ground or at something standing, or give a point)");
				return;
			}
		}
		SDL_strlcpy(name, first, sizeof(name));
		effect = particles_find_effect(&potop->world.particles, name);
		if (effect < 0)
		{
			SDL_Log("Effects: particles.ini has no [%s]", name);
			return;
		}
		SDL_Log("Effects: [%s] at (%.2f, %.2f, %.2f): emitter %u", name, (double)x, (double)y, (double)z,
			particles_play(&potop->world.particles, effect, x, y, z));
	}
	else if (SDL_strcmp(command, "torch") == 0)
	{
		player_t *player = &potop->session.player;

		player->light_on = has_first ? SDL_strcmp(first, "on") == 0 : !player->light_on;
		SDL_Log("The torch: %s%s", player->light_on ? "lit" : "out", player->light_on && !potop->camera.first_person ? " (shown on foot)" : "");
	}
	else if (SDL_strcmp(command, "lights") == 0)
	{
		// The lamps: on, off, or with the day (the game's API: lights_set_lamps)
		if (has_first && SDL_strcmp(first, "on") == 0)
		{
			potop->settings.render.lamps_with_the_day = false;
			lights_set_lamps(&potop->world.lights, LIGHTS_LAMPS_ON);
		}
		else if (has_first && SDL_strcmp(first, "off") == 0)
		{
			potop->settings.render.lamps_with_the_day = false;
			lights_set_lamps(&potop->world.lights, LIGHTS_LAMPS_OFF);
		}
		else if (has_first && SDL_strcmp(first, "auto") == 0)
		{
			potop->settings.render.lamps_with_the_day = true;
			lights_set_lamps(&potop->world.lights, LIGHTS_LAMPS_AUTOMATIC);
		}
		else if (has_first)
		{
			SDL_Log("lights [on|off|auto]");
			return;
		}
		SDL_Log("Lamps: %s; %d lights lighting, %d placed", potop->settings.render.lamps_with_the_day ? "with the day"
			: (potop->world.lights.lamps == LIGHTS_LAMPS_ON ? "on" : "off"), potop->world.lights.count, potop->world.lights.placed_count);
	}
	else if (SDL_strcmp(command, "awake") == 0)
	{
		// The bodies the physics keeps awake: which, and what the water
		// does to each (the first few; awake N: that many)
		physics_log_awake(&potop->world.physics, &potop->world.objects, has_first ? SDL_max(SDL_atoi(first), 0) : 12);
	}
	else if (SDL_strcmp(command, "stutters") == 0)
	{
		// The shortest frame told as a stutter (stutters 15; stutters 0:
		// any the frame's usual length makes one)
		if (has_first)
		{
			frame_profile_set_stutter_floor(SDL_atof(first));
		}
		else
		{
			double floor_milliseconds = frame_profile_set_stutter_floor(0.0);

			frame_profile_set_stutter_floor(floor_milliseconds);
			SDL_Log("Stutters: told from %.1f ms (stutters N sets it; 0: by the usual frame alone)", floor_milliseconds);
		}
	}
	else if (SDL_strcmp(command, "profile") == 0)
	{
		// Where the frames' time went on the CPU since the last time
		// (profile.h): a big "acquire" is the CPU waiting for a busy GPU
		// (or vsync); shadows, objects cull and terrain upload are inside
		// render
		char sections[1024];
		int frames;

		// (profile frame: the next frame, step by step)
		if (has_first && SDL_strcmp(first, "frame") == 0)
		{
			int later = has_second ? SDL_atoi(second) : 0;

			trace_next_frame(later);
			SDL_Log("Profile: a frame step by step in %d frames, in the log", later + 1);
			return;
		}
		frames = frame_profile_take_averages(sections, sizeof(sections));
		if (frames > 0)
		{
			SDL_Log("Profile, ms a frame over %d frames: %s", frames, sections);
		}
		else
		{
			SDL_Log("Profile: no frames yet (profile again in a while)");
		}
	}
	else if (SDL_strcmp(command, "noclip") == 0)
	{
		potop->camera.noclip = !potop->camera.noclip;
		SDL_Log(potop->camera.noclip ? "Noclip: flying through everything (on foot: W A S D where you look, Space up, Ctrl down, Shift fast)"
			: "Noclip: off, walking again");
	}
	else if (SDL_strcmp(command, "freeze") == 0)
	{
		potop->world.day_frozen = !potop->world.day_frozen;
		SDL_Log(potop->world.day_frozen ? "The day stands still at %02d:%02d (freeze again lets it go)" : "The day goes on from %02d:%02d",
			(int)potop->world.day_hour, (int)(SDL_fmod(potop->world.day_hour, 1.0) * 60.0));
	}
	else if (SDL_strcmp(command, "paint") == 0)
	{
		static const char *const channels[CLOUD_PAINT_CHANNELS] = { "coverage", "heaviness", "kind", "flocks" };
		int channel = -1;
		float value;

		for (int c = 0; c < CLOUD_PAINT_CHANNELS && has_first; c++)
		{
			channel = SDL_strcmp(first, channels[c]) == 0 ? c : channel;
		}
		if (channel < 0 || !has_second || !read_number(second, &value))
		{
			SDL_Log("paint <coverage|heaviness|kind|flocks> <0..1> [km]");
			return;
		}
		if (has_third)
		{
			// A brush around the eye, on the clouds' map (the wind taken off)
			float kilometers;
			float parameters[4];
			float motion[3];
			float eye_x = potop->camera.first_person ? potop->camera.walker_x : potop->camera.target_x;
			float eye_z = potop->camera.first_person ? potop->camera.walker_z : potop->camera.target_z;

			if (!read_number(third, &kilometers) || kilometers <= 0.0f)
			{
				SDL_Log("paint <coverage|heaviness|kind|flocks> <-1..1> <km>");
				return;
			}
			terrain_renderer_cloud_uniforms(&potop->settings.render, (float)potop->world.sky_seconds, parameters, motion);
			cloud_paint_brush(&potop->world.cloud_paint, (cloud_paint_channel_t)channel, (eye_x - motion[0]) / CLOUD_WEATHER_METERS,
				(eye_z - motion[1]) / CLOUD_WEATHER_METERS, kilometers * 1000.0f / CLOUD_WEATHER_METERS, clamp_float(value, -1.0f, 1.0f), false);
			SDL_Log("Painted %s %+.2f, %.1f km around you", channels[channel], (double)value, (double)kilometers);
			return;
		}
		{
			uint8_t byte = (uint8_t)SDL_lroundf(clamp_float(value, 0.0f, 1.0f) * 255.0f);

			for (size_t t = 0; t < (size_t)CLOUD_PAINT_SIZE * CLOUD_PAINT_SIZE; t++)
			{
				potop->world.cloud_paint.values[t * 4 + (size_t)channel] = byte;
			}
			potop->world.cloud_paint.revision++;
			if (channel != CLOUD_PAINT_FLOCKS)
			{
				potop->world.cloud_paint.stamp_revision++;
			}
			potop->world.cloud_paint.changed_ticks = SDL_GetTicks();
			SDL_Log("The whole sky's %s at %.2f", channels[channel], (double)byte / 255.0);
		}
	}
	else if (SDL_strcmp(command, "load") == 0 || SDL_strcmp(command, "save") == 0 || SDL_strcmp(command, "bake") == 0)
	{
		// The rest of the line is the name (it may have spaces); done with
		// the interface's requests, this frame
		char name[MAP_FILE_NAME_LENGTH];
		size_t length;

		while (*after_command == ' ' || *after_command == '\t')
		{
			after_command++;
		}
		SDL_strlcpy(name, after_command, sizeof(name));
		length = SDL_strlen(name);
		while (length > 0 && (name[length - 1] == ' ' || name[length - 1] == '\t'))
		{
			name[--length] = '\0';
		}
		if (length == 0)
		{
			SDL_Log("%s <name>: which map? (maps lists them)", command);
		}
		else if (command[0] == 'l')
		{
			actions->load_map = true;
			SDL_strlcpy(actions->map_name, name, sizeof(actions->map_name));
		}
		else
		{
			char clean[MAP_FILE_NAME_LENGTH];

			// (save: as the kind the map is; bake: baked)
			map_file_clean_name(name, clean, sizeof(clean));
			actions->save_map = true;
			actions->map_baked = command[0] == 'b' || potop->world.terrain.baked;
			SDL_strlcpy(actions->map_name, clean, sizeof(actions->map_name));
		}
	}
	else if (SDL_strcmp(command, "maps") == 0)
	{
		map_file_entry_t *entries = SDL_malloc(sizeof(map_file_entry_t) * MAP_FILE_LIST_MAX);
		int count = entries != NULL ? map_file_list(entries, MAP_FILE_LIST_MAX) : 0;

		for (int i = 0; i < count; i++)
		{
			SDL_Log("  %s  (%s, %.1f MB)", entries[i].name, entries[i].baked ? "baked" : "active", (double)entries[i].size_bytes / (1024.0 * 1024.0));
		}
		if (count == 0)
		{
			SDL_Log("No saved maps yet");
		}
		SDL_free(entries);
	}
	else if (SDL_strcmp(command, "screenshot") == 0)
	{
		actions->screenshot = true;
	}
	else if (SDL_strcmp(command, "clear") == 0)
	{
		console_clear(console);
	}
	else if (SDL_strcmp(command, "quit") == 0)
	{
		actions->quit = true;
	}
	else if (!console_run_added(console, command, after_command))
	{
		SDL_Log("Unknown command '%s' (help lists them)", command);
	}
}

bool console_add_command(console_t *console, const char *name, const char *usage, const char *help, console_command_function_t function, void *user)
{
	console_added_command_t *entry;

	if (console->added_count >= CONSOLE_ADDED_MAX)
	{
		return false;
	}
	entry = &console->added[console->added_count++];
	SDL_strlcpy(entry->name, name, sizeof(entry->name));
	SDL_strlcpy(entry->usage, usage != NULL ? usage : "", sizeof(entry->usage));
	SDL_strlcpy(entry->help, help != NULL ? help : "", sizeof(entry->help));
	entry->function = function;
	entry->user = user;
	return true;
}

bool console_run_added(console_t *console, const char *command, const char *arguments)
{
	for (int i = 0; i < console->added_count; i++)
	{
		if (SDL_strcmp(console->added[i].name, command) == 0)
		{
			while (*arguments == ' ' || *arguments == '\t')
			{
				arguments++;
			}
			console->added[i].function(console->added[i].user, arguments);
			return true;
		}
	}
	return false;
}
