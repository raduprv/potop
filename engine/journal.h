// The quest log, as the player sees it. The engine only shows it: the game
// keeps its quests (their flags, why an objective is done) and tells the
// journal what to show: each quest's title, description and state, its
// objectives (a line each, open, done or failed, how far along: "3/10"),
// and where an objective is (a place on the map).
//
// The player chooses which quests are tracked (their open objectives'
// places are shown on the compass and the map: journal_sync_markers) and
// which objective is in focus (the compass shows how far it is). How many
// may be tracked at once is the game's (journal_set_track_limit; 3).
// What the player chose comes back as events, if the game cares.

#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "markers.h"

#define JOURNAL_QUESTS_MAX 128
#define JOURNAL_OBJECTIVES_MAX 16
#define JOURNAL_ID_LENGTH 32
#define JOURNAL_EVENT_QUEUE 32

typedef enum
{
	JOURNAL_QUEST_ACTIVE = 0,
	JOURNAL_QUEST_INACTIVE,                // put aside (it can go on later)
	JOURNAL_QUEST_COMPLETED,
	JOURNAL_QUEST_FAILED,
	JOURNAL_QUEST_STATE_COUNT
} journal_quest_state_t;

typedef enum
{
	JOURNAL_OBJECTIVE_OPEN = 0,
	JOURNAL_OBJECTIVE_DONE,
	JOURNAL_OBJECTIVE_FAILED
} journal_objective_state_t;

typedef struct
{
	char id[JOURNAL_ID_LENGTH];
	char text[160];
	journal_objective_state_t state;
	char progress[24];                     // "3/10" (empty: none)
	bool has_place;
	float x;                               // meters, on the map
	float z;
	uint32_t marker;                       // its marker while shown (journal_sync_markers)
} journal_objective_t;

typedef struct
{
	char id[JOURNAL_ID_LENGTH];
	char title[64];
	char description[512];
	journal_quest_state_t state;
	journal_objective_t objectives[JOURNAL_OBJECTIVES_MAX];
	int objective_count;
	bool tracked;
	uint32_t order;                        // when it came (the list: the newest first)
} journal_quest_t;

typedef enum
{
	JOURNAL_EVENT_TRACKED = 1,             // the player tracks it now
	JOURNAL_EVENT_UNTRACKED,
	JOURNAL_EVENT_FOCUSED                  // the player put this objective in focus
} journal_event_type_t;

typedef struct
{
	journal_event_type_t type;
	char quest[JOURNAL_ID_LENGTH];
	char objective[JOURNAL_ID_LENGTH];
} journal_event_t;

typedef struct
{
	journal_quest_t quests[JOURNAL_QUESTS_MAX];
	int count;
	uint32_t next_order;
	int track_limit;
	char focus_quest[JOURNAL_ID_LENGTH];   // the objective in focus (empty: none)
	char focus_objective[JOURNAL_ID_LENGTH];
	journal_event_t events[JOURNAL_EVENT_QUEUE];
	int event_first;
	int event_count;
	// The markers it put last time (taken away once nothing claims them: a
	// quest gone, untracked, done)
	uint32_t owned_markers[MARKERS_MAX];
	int owned_count;
} journal_t;

void journal_create(journal_t *journal);
void journal_clear(journal_t *journal);

// What the game tells it. A quest or objective named for the first time is
// added (objectives in the order they come); text NULL leaves it as it was
void journal_set_quest(journal_t *journal, const char *quest, const char *title, const char *description, journal_quest_state_t state);
void journal_set_objective(journal_t *journal, const char *quest, const char *objective, const char *text, journal_objective_state_t state,
	const char *progress);
void journal_set_place(journal_t *journal, const char *quest, const char *objective, float x, float z);
void journal_clear_place(journal_t *journal, const char *quest, const char *objective);
void journal_remove_quest(journal_t *journal, const char *quest);
void journal_remove_objective(journal_t *journal, const char *quest, const char *objective);

journal_quest_t *journal_find(journal_t *journal, const char *quest);
journal_objective_t *journal_find_objective(journal_t *journal, const char *quest, const char *objective);

// How many quests may be tracked at once (the game's; 3)
void journal_set_track_limit(journal_t *journal, int limit);
// Tracked or not (the player's choice, or the game's); false if the limit
// does not allow it (or there is no such quest)
bool journal_track(journal_t *journal, const char *quest, bool tracked);
int journal_tracked_count(const journal_t *journal);
// That objective in focus (objective NULL: the quest's first open one with
// a place); it tracks its quest too if it can. False if it cannot be
bool journal_focus(journal_t *journal, const char *quest, const char *objective);

bool journal_poll_event(journal_t *journal, journal_event_t *event_out);

// The tracked quests' open objectives with a place as markers (the one in
// focus the active one); the markers of the rest taken away. Every frame
// (cheap)
void journal_sync_markers(journal_t *journal, markers_t *markers);

const char *journal_state_name(journal_quest_state_t state);

#endif
