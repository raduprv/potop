// The quest log. See journal.h.

#include "journal.h"

#include <SDL3/SDL.h>

static const char *const state_names[JOURNAL_QUEST_STATE_COUNT] = { "Active", "Inactive", "Completed", "Failed" };

const char *journal_state_name(journal_quest_state_t state)
{
	return state >= 0 && state < JOURNAL_QUEST_STATE_COUNT ? state_names[state] : "";
}

void journal_create(journal_t *journal)
{
	SDL_zerop(journal);
	journal->track_limit = 3;
	journal->next_order = 1;
}

void journal_clear(journal_t *journal)
{
	int limit = journal->track_limit;
	uint32_t owned[MARKERS_MAX];
	int owned_count = journal->owned_count;

	// (its markers still go at the next sync)
	SDL_memcpy(owned, journal->owned_markers, sizeof(owned));
	journal_create(journal);
	journal->track_limit = limit;
	SDL_memcpy(journal->owned_markers, owned, sizeof(owned));
	journal->owned_count = owned_count;
}

static void push_event(journal_t *journal, journal_event_type_t type, const char *quest, const char *objective)
{
	journal_event_t *event;

	if (journal->event_count == JOURNAL_EVENT_QUEUE)
	{
		journal->event_first = (journal->event_first + 1) % JOURNAL_EVENT_QUEUE;
		journal->event_count--;
	}
	event = &journal->events[(journal->event_first + journal->event_count) % JOURNAL_EVENT_QUEUE];
	event->type = type;
	SDL_strlcpy(event->quest, quest != NULL ? quest : "", sizeof(event->quest));
	SDL_strlcpy(event->objective, objective != NULL ? objective : "", sizeof(event->objective));
	journal->event_count++;
}

bool journal_poll_event(journal_t *journal, journal_event_t *event_out)
{
	if (journal->event_count == 0)
	{
		return false;
	}
	*event_out = journal->events[journal->event_first];
	journal->event_first = (journal->event_first + 1) % JOURNAL_EVENT_QUEUE;
	journal->event_count--;
	return true;
}

journal_quest_t *journal_find(journal_t *journal, const char *quest)
{
	for (int i = 0; i < journal->count && quest != NULL; i++)
	{
		if (SDL_strcmp(journal->quests[i].id, quest) == 0)
		{
			return &journal->quests[i];
		}
	}
	return NULL;
}

journal_objective_t *journal_find_objective(journal_t *journal, const char *quest, const char *objective)
{
	journal_quest_t *found = journal_find(journal, quest);

	for (int i = 0; found != NULL && objective != NULL && i < found->objective_count; i++)
	{
		if (SDL_strcmp(found->objectives[i].id, objective) == 0)
		{
			return &found->objectives[i];
		}
	}
	return NULL;
}

void journal_set_quest(journal_t *journal, const char *quest, const char *title, const char *description, journal_quest_state_t state)
{
	journal_quest_t *found = journal_find(journal, quest);

	if (found == NULL)
	{
		if (journal->count >= JOURNAL_QUESTS_MAX || quest == NULL)
		{
			return;
		}
		found = &journal->quests[journal->count++];
		SDL_zerop(found);
		SDL_strlcpy(found->id, quest, sizeof(found->id));
		found->order = journal->next_order++;
	}
	if (title != NULL)
	{
		SDL_strlcpy(found->title, title, sizeof(found->title));
	}
	if (description != NULL)
	{
		SDL_strlcpy(found->description, description, sizeof(found->description));
	}
	found->state = state;
}

void journal_set_objective(journal_t *journal, const char *quest, const char *objective, const char *text, journal_objective_state_t state,
	const char *progress)
{
	journal_quest_t *found = journal_find(journal, quest);
	journal_objective_t *line = journal_find_objective(journal, quest, objective);

	if (found == NULL || objective == NULL)
	{
		return;
	}
	if (line == NULL)
	{
		if (found->objective_count >= JOURNAL_OBJECTIVES_MAX)
		{
			return;
		}
		line = &found->objectives[found->objective_count++];
		SDL_zerop(line);
		SDL_strlcpy(line->id, objective, sizeof(line->id));
	}
	if (text != NULL)
	{
		SDL_strlcpy(line->text, text, sizeof(line->text));
	}
	if (progress != NULL)
	{
		SDL_strlcpy(line->progress, progress, sizeof(line->progress));
	}
	line->state = state;
}

void journal_set_place(journal_t *journal, const char *quest, const char *objective, float x, float z)
{
	journal_objective_t *line = journal_find_objective(journal, quest, objective);

	if (line != NULL)
	{
		line->has_place = true;
		line->x = x;
		line->z = z;
	}
}

void journal_clear_place(journal_t *journal, const char *quest, const char *objective)
{
	journal_objective_t *line = journal_find_objective(journal, quest, objective);

	if (line != NULL)
	{
		line->has_place = false;
	}
}

void journal_remove_quest(journal_t *journal, const char *quest)
{
	journal_quest_t *found = journal_find(journal, quest);

	if (found != NULL)
	{
		int index = (int)(found - journal->quests);

		if (SDL_strcmp(journal->focus_quest, quest) == 0)
		{
			journal->focus_quest[0] = '\0';
			journal->focus_objective[0] = '\0';
		}
		SDL_memmove(found, found + 1, sizeof(journal_quest_t) * (size_t)(journal->count - index - 1));
		journal->count--;
	}
}

void journal_remove_objective(journal_t *journal, const char *quest, const char *objective)
{
	journal_quest_t *found = journal_find(journal, quest);
	journal_objective_t *line = journal_find_objective(journal, quest, objective);

	if (found != NULL && line != NULL)
	{
		int index = (int)(line - found->objectives);

		if (SDL_strcmp(journal->focus_quest, quest) == 0 && SDL_strcmp(journal->focus_objective, objective) == 0)
		{
			journal->focus_objective[0] = '\0';
		}
		SDL_memmove(line, line + 1, sizeof(journal_objective_t) * (size_t)(found->objective_count - index - 1));
		found->objective_count--;
	}
}

void journal_set_track_limit(journal_t *journal, int limit)
{
	journal->track_limit = SDL_max(limit, 0);
}

int journal_tracked_count(const journal_t *journal)
{
	int count = 0;

	for (int i = 0; i < journal->count; i++)
	{
		count += journal->quests[i].tracked ? 1 : 0;
	}
	return count;
}

bool journal_track(journal_t *journal, const char *quest, bool tracked)
{
	journal_quest_t *found = journal_find(journal, quest);

	if (found == NULL)
	{
		return false;
	}
	if (found->tracked == tracked)
	{
		return true;
	}
	if (tracked && journal_tracked_count(journal) >= journal->track_limit)
	{
		return false;
	}
	found->tracked = tracked;
	if (!tracked && SDL_strcmp(journal->focus_quest, quest) == 0)
	{
		journal->focus_quest[0] = '\0';
		journal->focus_objective[0] = '\0';
	}
	push_event(journal, tracked ? JOURNAL_EVENT_TRACKED : JOURNAL_EVENT_UNTRACKED, quest, NULL);
	return true;
}

bool journal_focus(journal_t *journal, const char *quest, const char *objective)
{
	journal_quest_t *found = journal_find(journal, quest);
	const journal_objective_t *line = NULL;

	if (found == NULL)
	{
		return false;
	}
	if (objective != NULL)
	{
		line = journal_find_objective(journal, quest, objective);
	}
	for (int i = 0; line == NULL && objective == NULL && i < found->objective_count; i++)
	{
		if (found->objectives[i].state == JOURNAL_OBJECTIVE_OPEN && found->objectives[i].has_place)
		{
			line = &found->objectives[i];
		}
	}
	if (line == NULL || !journal_track(journal, quest, true))
	{
		return false;
	}
	SDL_strlcpy(journal->focus_quest, quest, sizeof(journal->focus_quest));
	SDL_strlcpy(journal->focus_objective, line->id, sizeof(journal->focus_objective));
	push_event(journal, JOURNAL_EVENT_FOCUSED, quest, line->id);
	return true;
}

void journal_sync_markers(journal_t *journal, markers_t *markers)
{
	static const float focus_color[3] = { 1.0f, 0.80f, 0.20f };
	static const float tracked_color[3] = { 0.95f, 0.93f, 0.85f };
	uint32_t claimed[MARKERS_MAX];
	int claimed_count = 0;
	uint32_t focus_marker = 0;

	for (int q = 0; q < journal->count; q++)
	{
		journal_quest_t *quest = &journal->quests[q];

		for (int o = 0; o < quest->objective_count; o++)
		{
			journal_objective_t *line = &quest->objectives[o];
			bool focused = SDL_strcmp(journal->focus_quest, quest->id) == 0 && SDL_strcmp(journal->focus_objective, line->id) == 0;
			bool shown = quest->tracked && quest->state == JOURNAL_QUEST_ACTIVE && line->state == JOURNAL_OBJECTIVE_OPEN && line->has_place;
			marker_t *marker = markers_get(markers, line->marker);

			if (!shown)
			{
				line->marker = 0;
				continue;
			}
			if (marker == NULL)
			{
				line->marker = markers_add(markers, line->x, line->z, focused ? focus_color : tracked_color, line->text);
				marker = markers_get(markers, line->marker);
			}
			if (marker == NULL)
			{
				continue;
			}
			marker->x = line->x;
			marker->z = line->z;
			SDL_memcpy(marker->color, focused ? focus_color : tracked_color, sizeof(marker->color));
			SDL_strlcpy(marker->label, line->text, sizeof(marker->label));
			if (claimed_count < MARKERS_MAX)
			{
				claimed[claimed_count++] = line->marker;
			}
			if (focused)
			{
				focus_marker = line->marker;
			}
		}
	}
	// What it put before and no longer claims goes
	for (int i = 0; i < journal->owned_count; i++)
	{
		bool still = false;

		for (int k = 0; k < claimed_count && !still; k++)
		{
			still = claimed[k] == journal->owned_markers[i];
		}
		if (!still)
		{
			markers_remove(markers, journal->owned_markers[i]);
		}
	}
	SDL_memcpy(journal->owned_markers, claimed, sizeof(uint32_t) * (size_t)claimed_count);
	journal->owned_count = claimed_count;
	if (focus_marker != 0)
	{
		markers_set_active(markers, focus_marker);
	}
}
