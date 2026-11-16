// The game's debug window (F3): the frame rate, where the player is, how
// lit (what the animals and the guards will go by). More lines as they are
// needed.

#include "game.h"
#include "system_monitor.h"

#include <SDL3/SDL.h>

// What is known of a part of the machine, after a line's start: each thing
// known appended, separated by commas (nothing known: the line not shown)
static void append_part(char *line, size_t size, bool *first, const char *part)
{
	SDL_strlcat(line, *first ? " " : ", ", size);
	SDL_strlcat(line, part, size);
	*first = false;
}

// The processor's and the graphics card's lines (Linux only: see
// system_monitor.h)
static void machine_lines(potop_t *potop)
{
	system_sample_t sample;
	char line[128];
	char part[64];
	bool first = true;

	if (!system_monitor_get(&sample))
	{
		return;
	}
	SDL_strlcpy(line, "CPU:", sizeof(line));
	if (sample.process_cpu_known)
	{
		SDL_snprintf(part, sizeof(part), "Potop %.0f%% of a core", sample.process_cpu_percent);
		append_part(line, sizeof(line), &first, part);
	}
	if (sample.system_cpu_known)
	{
		SDL_snprintf(part, sizeof(part), "all %.0f%% of %d", sample.system_cpu_percent, sample.cpu_threads);
		append_part(line, sizeof(line), &first, part);
	}
	if (sample.cpu_temperature_known)
	{
		SDL_snprintf(part, sizeof(part), "%.0f C", sample.cpu_celsius);
		append_part(line, sizeof(line), &first, part);
	}
	if (!first)
	{
		potop_debug_line(potop, line);
	}
	first = true;
	SDL_strlcpy(line, "GPU:", sizeof(line));
	if (sample.gpu_load_known)
	{
		SDL_snprintf(part, sizeof(part), "load %.0f%%", sample.gpu_load_percent);
		append_part(line, sizeof(line), &first, part);
	}
	if (sample.gpu_memory_known)
	{
		SDL_snprintf(part, sizeof(part), "VRAM %.0f / %.0f MB", sample.gpu_memory_used_megabytes, sample.gpu_memory_total_megabytes);
		append_part(line, sizeof(line), &first, part);
	}
	if (sample.gpu_power_known)
	{
		SDL_snprintf(part, sizeof(part), "%.0f W", sample.gpu_watts);
		append_part(line, sizeof(line), &first, part);
	}
	if (sample.gpu_temperature_known)
	{
		SDL_snprintf(part, sizeof(part), "%.0f C", sample.gpu_celsius);
		append_part(line, sizeof(line), &first, part);
	}
	if (!first)
	{
		SDL_snprintf(part, sizeof(part), " (%s)", sample.gpu_source != NULL ? sample.gpu_source : "?");
		SDL_strlcat(line, part, sizeof(line));
		potop_debug_line(potop, line);
	}
}

// The frame rate shown is the last frames' on average: this share of the
// newest each frame
static const float FRAME_AVERAGE_SHARE = 0.05f;

void game_update_debug(game_t *game, potop_t *potop, bool toggle, float seconds)
{
	potop_camera_info_t camera;
	potop_light_info_t light;
	char line[96];

	if (seconds > 0.0f)
	{
		game->frame_seconds_average = game->frame_seconds_average <= 0.0f ? seconds
			: game->frame_seconds_average + (seconds - game->frame_seconds_average) * FRAME_AVERAGE_SHARE;
	}
	if (toggle)
	{
		potop_debug_show(potop, !potop_debug_shown(potop));
	}
	if (!potop_debug_shown(potop))
	{
		return;
	}
	potop_debug_clear(potop);
	SDL_snprintf(line, sizeof(line), "FPS: %.0f (%.1f ms)", game->frame_seconds_average > 0.0f ? 1.0f / game->frame_seconds_average : 0.0f,
		1000.0f * game->frame_seconds_average);
	potop_debug_line(potop, line);
	machine_lines(potop);
	potop_camera_get(potop, &camera);
	SDL_snprintf(line, sizeof(line), "Position: %.1f, %.1f, %.1f", camera.eye_x, camera.eye_y, camera.eye_z);
	potop_debug_line(potop, line);
	potop_light_at(potop, camera.eye_x, camera.eye_y - 0.7f, camera.eye_z, &light);
	SDL_snprintf(line, sizeof(line), "Light: %.2f", light.total);
	potop_debug_line(potop, line);
	SDL_snprintf(line, sizeof(line), "  sky seen %.2f, day %.2f, lights near %.2f", light.sky_open, light.daylight, light.local);
	potop_debug_line(potop, line);
	SDL_snprintf(line, sizeof(line), "Occlusion culling: %s", camera.occlusion_culling ? "on" : "off (objects occlusion on)");
	potop_debug_line(potop, line);
	SDL_snprintf(line, sizeof(line), "Going: %s", camera.gait == POTOP_GAIT_CRAWL ? "crawling" : camera.gait == POTOP_GAIT_RUN ? "running"
		: camera.gait == POTOP_GAIT_WALK ? "walking" : (camera.crawling ? "lying low" : "standing"));
	potop_debug_line(potop, line);
	// On foot: what the feet stand on, what pushed the walker
	if (camera.first_person)
	{
		SDL_snprintf(line, sizeof(line), "Feet: %.2f over the terrain, ground %.2f over it%s", camera.walker_feet - camera.walker_terrain,
			camera.walker_ground - camera.walker_terrain, camera.walker_on_ground ? ", on it" : ", in the air");
		potop_debug_line(potop, line);
		SDL_snprintf(line, sizeof(line), "Pushed: %.3f m (x %.3f, z %.3f), planes %d (let be %d)", SDL_sqrtf(camera.push_x * camera.push_x
			+ camera.push_z * camera.push_z), camera.push_x, camera.push_z, camera.push_planes_kept, camera.push_planes_skipped);
		potop_debug_line(potop, line);
		if (camera.push_planes_kept > 0)
		{
			SDL_snprintf(line, sizeof(line), "  deepest: normal %.2f %.2f %.2f, %.2f m over the feet, %.3f m in", camera.push_normal[0],
				camera.push_normal[1], camera.push_normal[2], camera.push_height, camera.push_depth);
			potop_debug_line(potop, line);
		}
	}
	// The bears: how many, what they are about, what their minds cost
	{
		int states[BEAR_STATE_COUNT] = { 0 };
		float nearest = 1e30f;
		int nearest_index = -1;

		for (int i = 0; i < game->bear_count; i++)
		{
			potop_actor_info_t actor;

			states[game->bears[i].state]++;
			if (potop_actor_get(potop, game->bears[i].actor_id, &actor))
			{
				float squared = (actor.x - camera.eye_x) * (actor.x - camera.eye_x) + (actor.z - camera.eye_z) * (actor.z - camera.eye_z);

				if (squared < nearest)
				{
					nearest = squared;
					nearest_index = i;
				}
			}
		}
		SDL_snprintf(line, sizeof(line), "Bears: %d, minds %.2f ms", game->bear_count, game->bear_milliseconds);
		potop_debug_line(potop, line);
		SDL_snprintf(line, sizeof(line), "  wander %d, rest %d, alert %d, investigate %d, warn %d, attack %d, flee %d", states[BEAR_WANDER],
			states[BEAR_REST], states[BEAR_ALERT], states[BEAR_INVESTIGATE], states[BEAR_WARN], states[BEAR_ATTACK], states[BEAR_FLEE]);
		potop_debug_line(potop, line);
		if (nearest_index >= 0)
		{
			const bear_mind_t *bear = &game->bears[nearest_index];

			SDL_snprintf(line, sizeof(line), "  nearest: %.0f m, %s, sees you %.2f, suspicion %.2f", SDL_sqrtf(nearest),
				game_bear_state_name(bear->state), potop_actor_sees_player(potop, bear->actor_id), bear->suspicion);
			potop_debug_line(potop, line);
		}
	}
}
