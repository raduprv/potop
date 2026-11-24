// Phase profiler. See profile.h.

#include "profile.h"

#include <SDL3/SDL.h>

profile_t profile;

uint64_t profile_begin(void)
{
	return SDL_GetPerformanceCounter();
}

void profile_end(profile_phase_t phase, uint64_t start)
{
	profile.ticks[phase] += SDL_GetPerformanceCounter() - start;
}

const char *profile_phase_name(profile_phase_t phase)
{
	static const char *names[PROFILE_PHASE_COUNT] = { "sources", "fluxes", "depths", "bookkeeping", "evaporation",
		"erode", "collapse", "refresh", "soak" };

	return (phase >= 0 && phase < PROFILE_PHASE_COUNT) ? names[phase] : "?";
}

// ---------------------------------------------------------------------------
// The frame profiler

frame_profile_t frame_profile;

// A frame this much longer than the usual one is a stutter (both must hold)
// (a frame of 7 ms among ones of 4 is felt, and was under both)
#define SPIKE_FACTOR 1.5
#define SPIKE_EXTRA_MILLISECONDS 2.0
// No more lines than this a second, whatever happens
#define SPIKES_LOGGED_PER_SECOND 4

void frame_profile_end(frame_section_t section, uint64_t start)
{
	frame_profile.ticks[section] += SDL_GetPerformanceCounter() - start;
}

static const char *section_name(frame_section_t section)
{
	static const char *names[FRAME_SECTION_COUNT] = { "update", "poll", "water", "erosion", "wetting", "physics", "particles",
		"water end", "copy wait", "acquire", "objects follow", "navigation", "terrain upload", "shadows", "objects cull", "render", "present",
		"game", "events", "hud", "player", "actors", "paths", "weapons", "ui", "sky occlusion", "frame limit",
		"follow water", "flooded cover", "terrain setup", "terrain materials", "chunk prepare", "chunk send", "soil blur", "water tiles", "chunk bounds",
		"physics sync", "physics terrain", "physics water", "physics flood", "box3d step", "physics hits", "physics follow", "physics wake", "physics remove" };

	return names[section];
}

static const char *counter_name(frame_counter_t counter)
{
	static const char *names[FRAME_COUNTER_COUNT] = { "steps", "substeps", "evaporations", "collapses", "water tiles in",
		"ground tiles in", "wet tiles", "chunk uploads", "physics rebuilds", "path regions built", "path tiles built", "path build microseconds", "shadow redraws", "near shadow redraws", "moving shadow redraws",
		"path searches", "path surfaces", "objects checked", "objects moved",
		"awake bodies", "bodies pushed", "body moves", "objects removed", "gpu water tiles", "terrain chunks drawn", "water chunks drawn",
		"shadow draw calls", "far shadow for the ground", "far shadow for objects", "far shadow for the light", "far shadow for the camera",
		"object sorts", "full body syncs", "bodies made" };

	return names[counter];
}

void frame_profile_path(uint64_t ticks, uint32_t actor, const char *type_name, int32_t expansions, const char *result, float distance)
{
	frame_profile.ticks[FRAME_PATHS] += ticks;
	frame_profile.counters[FRAME_PATH_SEARCHES]++;
	frame_profile.counters[FRAME_PATH_EXPANSIONS] += expansions;
	if (ticks > frame_profile.slowest_path_ticks)
	{
		frame_profile.slowest_path_ticks = ticks;
		frame_profile.slowest_path_actor = actor;
		frame_profile.slowest_path_type = type_name;
		frame_profile.slowest_path_expansions = expansions;
		frame_profile.slowest_path_result = result;
		frame_profile.slowest_path_distance = distance;
	}
}

// The sections inside others (not added twice)
static bool section_nested(int section)
{
	return section == FRAME_COPY_WAIT || section == FRAME_OBJECTS_FOLLOW || section == FRAME_NAVIGATION || section == FRAME_TERRAIN_UPLOAD
		|| section == FRAME_SHADOWS || section == FRAME_OBJECTS_CULL || section == FRAME_PATHS || section >= FRAME_FOLLOW_WATER;
}

static void log_spike(double milliseconds, double frequency)
{
	char line[1024];
	size_t length = 0;
	double accounted = 0.0;

	length += (size_t)SDL_snprintf(line + length, sizeof(line) - length, "Stutter: %.1f ms (usual %.1f):", milliseconds,
		frame_profile.usual_milliseconds);
	for (int section = 0; section < FRAME_SECTION_COUNT && length < sizeof(line); section++)
	{
		double section_milliseconds = 1000.0 * (double)frame_profile.ticks[section] / frequency;

		if (!section_nested(section))
		{
			accounted += section_milliseconds;
		}
		if (section_milliseconds >= 0.1)
		{
			length += (size_t)SDL_snprintf(line + length, sizeof(line) - length, " %s %.1f,", section_name((frame_section_t)section),
				section_milliseconds);
		}
	}
	if (length < sizeof(line))
	{
		length += (size_t)SDL_snprintf(line + length, sizeof(line) - length, " other %.1f |", SDL_max(milliseconds - accounted, 0.0));
	}
	for (int counter = 0; counter < FRAME_COUNTER_COUNT && length < sizeof(line); counter++)
	{
		if (frame_profile.counters[counter] != 0)
		{
			length += (size_t)SDL_snprintf(line + length, sizeof(line) - length, " %s %d,", counter_name((frame_counter_t)counter),
				(int)frame_profile.counters[counter]);
		}
	}
	if (frame_profile.slowest_path_ticks > 0 && length < sizeof(line))
	{
		length += (size_t)SDL_snprintf(line + length, sizeof(line) - length, " slowest path: %s %u, %.1f ms, %d surfaces, %.0f m away, %s,",
			frame_profile.slowest_path_type != NULL ? frame_profile.slowest_path_type : "?", frame_profile.slowest_path_actor,
			1000.0 * (double)frame_profile.slowest_path_ticks / frequency, (int)frame_profile.slowest_path_expansions,
			(double)frame_profile.slowest_path_distance, frame_profile.slowest_path_result != NULL ? frame_profile.slowest_path_result : "?");
	}
	if (length < sizeof(line))
	{
		length += (size_t)SDL_snprintf(line + length, sizeof(line) - length, " | 2 frames before:");
	}
	for (int counter = 0; counter < FRAME_COUNTER_COUNT && length < sizeof(line); counter++)
	{
		int32_t before = frame_profile.previous_counters[counter] + frame_profile.earlier_counters[counter];

		if (before != 0)
		{
			length += (size_t)SDL_snprintf(line + length, sizeof(line) - length, " %s %d,", counter_name((frame_counter_t)counter), (int)before);
		}
	}
	SDL_Log("%s", line);
}

void frame_profile_finish(void)
{
	uint64_t now = SDL_GetPerformanceCounter();
	double frequency = (double)SDL_GetPerformanceFrequency();

	if (frame_profile.last_finish != 0)
	{
		double milliseconds = 1000.0 * (double)(now - frame_profile.last_finish) / frequency;

		frame_profile.worst_milliseconds = SDL_max(frame_profile.worst_milliseconds, milliseconds);
		frame_profile.second_timer += milliseconds * 0.001;
		if (frame_profile.second_timer >= 1.0)
		{
			frame_profile.second_timer = 0.0;
			frame_profile.spikes_logged_this_second = 0;
		}
		// Long stalls (a window dragged, loading a map) are not stutters
		if (frame_profile.usual_milliseconds > 0.0 && milliseconds < 250.0
			&& milliseconds > frame_profile.usual_milliseconds * SPIKE_FACTOR
			&& milliseconds > frame_profile.usual_milliseconds + SPIKE_EXTRA_MILLISECONDS)
		{
			frame_profile.spike_count++;
			if (frame_profile.spikes_logged_this_second < SPIKES_LOGGED_PER_SECOND)
			{
				frame_profile.spikes_logged_this_second++;
				log_spike(milliseconds, frequency);
			}
		}
		if (milliseconds < 250.0)
		{
			frame_profile.usual_milliseconds = frame_profile.usual_milliseconds == 0.0 ? milliseconds
				: frame_profile.usual_milliseconds * 0.97 + milliseconds * 0.03;
		}
	}
	frame_profile.last_finish = now;
	SDL_memcpy(frame_profile.earlier_counters, frame_profile.previous_counters, sizeof(frame_profile.earlier_counters));
	SDL_memcpy(frame_profile.previous_counters, frame_profile.counters, sizeof(frame_profile.previous_counters));
	for (int section = 0; section < FRAME_SECTION_COUNT; section++)
	{
		frame_profile.sum_ticks[section] += frame_profile.ticks[section];
	}
	frame_profile.sum_frames++;
	SDL_zeroa(frame_profile.ticks);
	SDL_zeroa(frame_profile.counters);
	frame_profile.slowest_path_ticks = 0;
}

int frame_profile_take_averages(char *text, size_t size)
{
	double frequency = (double)SDL_GetPerformanceFrequency();
	int frames = frame_profile.sum_frames;
	size_t length = 0;

	if (size == 0)
	{
		return 0;
	}
	text[0] = '\0';
	for (int section = 0; section < FRAME_SECTION_COUNT && frames > 0 && length < size; section++)
	{
		double milliseconds = 1000.0 * (double)frame_profile.sum_ticks[section] / frequency / (double)frames;

		if (milliseconds >= 0.01)
		{
			length += (size_t)SDL_snprintf(text + length, size - length, "%s%s %.2f", length > 0 ? ", " : "",
				section_name((frame_section_t)section), milliseconds);
		}
	}
	SDL_zeroa(frame_profile.sum_ticks);
	frame_profile.sum_frames = 0;
	return frames;
}

double frame_profile_take_worst(void)
{
	double worst = frame_profile.worst_milliseconds;

	frame_profile.worst_milliseconds = 0.0;
	return worst;
}

// ---------------------------------------------------------------------------
// The trace

bool trace_active;

static struct
{
	char line[4096];
	size_t length;
	uint64_t start;
	uint64_t mark;
	double rest_milliseconds;
	double threshold_milliseconds; // steps under it into "the rest"
	double log_over_milliseconds;  // logged only if it took this long in all (0: always)
	double live_milliseconds;      // steps this long logged at once (0: none)
	bool frame_asked;              // trace_next_frame: a frame traced, every step
	int frames_to_wait;            // frames before it (time to close the console)
} trace;

void trace_begin(const char *label)
{
	trace.length = (size_t)SDL_snprintf(trace.line, sizeof(trace.line), "%s:", label);
	trace.start = SDL_GetPerformanceCounter();
	trace.mark = trace.start;
	trace.rest_milliseconds = 0.0;
	trace.threshold_milliseconds = 0.5;
	trace.log_over_milliseconds = 0.0;
	trace.live_milliseconds = 0.0;
	trace_active = true;
}

void trace_log_steps_over(double live_milliseconds)
{
	trace.live_milliseconds = live_milliseconds;
}

void trace_begin_if_slow(const char *label, double log_over_milliseconds, double step_milliseconds)
{
	trace_begin(label);
	trace.log_over_milliseconds = log_over_milliseconds;
	trace.threshold_milliseconds = step_milliseconds;
}

void trace_next_frame(int frames_later)
{
	trace.frame_asked = true;
	trace.frames_to_wait = SDL_max(frames_later, 0);
}

void trace_frame_begin(void)
{
	if (trace.frame_asked && trace.frames_to_wait > 0)
	{
		trace.frames_to_wait--;
		return;
	}
	if (trace.frame_asked && (!trace_active || trace.log_over_milliseconds > 0.0))
	{
		trace.frame_asked = false;
		trace_begin("One frame, step by step (ms; the steps are in the order done)");
		trace.threshold_milliseconds = 0.0;
		return;
	}
	// Every frame traced, and logged if it is a stutter (as log_spike's
	// line, which tells the sections: this one the steps in the order
	// done, the time in none of the sections among them)
	if (!trace_active && frame_profile.usual_milliseconds > 0.0 && frame_profile.spikes_logged_this_second < SPIKES_LOGGED_PER_SECOND)
	{
		trace_begin_if_slow("Stutter, step by step (ms)",
			SDL_max(frame_profile.usual_milliseconds * SPIKE_FACTOR, frame_profile.usual_milliseconds + SPIKE_EXTRA_MILLISECONDS), 0.3);
	}
}

void trace_mark_slow(const char *name)
{
	uint64_t now = SDL_GetPerformanceCounter();
	double milliseconds = 1000.0 * (double)(now - trace.mark) / (double)SDL_GetPerformanceFrequency();

	trace.mark = now;
	if (trace.live_milliseconds > 0.0 && milliseconds >= trace.live_milliseconds)
	{
		SDL_Log("  ... %s: %.0f ms", name, milliseconds);
	}
	if (milliseconds < trace.threshold_milliseconds)
	{
		trace.rest_milliseconds += milliseconds;
		return;
	}
	if (trace.length < sizeof(trace.line))
	{
		trace.length += (size_t)SDL_snprintf(trace.line + trace.length, sizeof(trace.line) - trace.length,
			trace.threshold_milliseconds > 0.0 ? " %s %.1f," : " %s %.3f,", name, milliseconds);
	}
}

void trace_end(void)
{
	double total;

	if (!trace_active)
	{
		return;
	}
	trace_mark_slow("(unmarked)");
	total = 1000.0 * (double)(trace.mark - trace.start) / (double)SDL_GetPerformanceFrequency();
	trace_active = false;
	if (total < trace.log_over_milliseconds)
	{
		return;
	}
	SDL_Log(trace.threshold_milliseconds > 0.0 ? "%s the rest %.1f; %.1f ms in all" : "%s the rest %.3f; %.3f ms in all", trace.line,
		trace.rest_milliseconds, total);
	trace_active = false;
}
