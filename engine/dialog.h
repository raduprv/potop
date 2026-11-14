// Talking: the engine shows a conversation, the game holds it. The game
// opens it with someone (an actor, a name), says their line and offers the
// player's answers; what the player picks comes back as an event, and the
// game says the next line (its dialog trees, conditions and flags are its
// own: C code; flags that last only the conversation too). Each answer has
// the game's id, so the answers can change while it goes on. While it is
// open the pointer is free, the player stays where they are, the one
// talking looks at them.

#ifndef DIALOG_H
#define DIALOG_H

#include <stdbool.h>
#include <stdint.h>

#define DIALOG_CHOICES_MAX 8
#define DIALOG_TEXT_LENGTH 1024
#define DIALOG_CHOICE_LENGTH 160
#define DIALOG_EVENT_QUEUE 16

typedef enum
{
	DIALOG_EVENT_CHOSEN = 1,               // the player picked an answer (choice: its id)
	DIALOG_EVENT_CLOSED                    // the player walked off or pressed Escape
} dialog_event_type_t;

typedef struct
{
	dialog_event_type_t type;
	uint32_t actor;
	uint32_t choice;                       // the answer's id (the game's)
} dialog_event_t;

// An answer: the game's id for it (unique in the conversation), its text
typedef struct
{
	uint32_t id;
	const char *text;
} dialog_choice_t;

typedef struct
{
	bool open;
	uint32_t actor;                        // who talks (0: no actor: a sign, a letter)
	char speaker[48];
	char text[DIALOG_TEXT_LENGTH];
	char choices[DIALOG_CHOICES_MAX][DIALOG_CHOICE_LENGTH];
	uint32_t choice_ids[DIALOG_CHOICES_MAX];
	int choice_count;
	dialog_event_t events[DIALOG_EVENT_QUEUE];
	int event_first;
	int event_count;
} dialog_t;

void dialog_create(dialog_t *dialog);

// What the game does
void dialog_open(dialog_t *dialog, uint32_t actor, const char *speaker);
void dialog_say(dialog_t *dialog, const char *text);
// The player's answers, each with the game's id (none: a line to read,
// then Continue: choice id 0). Shown in this order; added at the end,
// taken away by id (an id given again: its text replaced)
void dialog_set_choices(dialog_t *dialog, const dialog_choice_t *choices, int count);
void dialog_add_choice(dialog_t *dialog, uint32_t id, const char *text);
void dialog_remove_choice(dialog_t *dialog, uint32_t id);
void dialog_clear_choices(dialog_t *dialog);
void dialog_close(dialog_t *dialog);

// What the player does (the interface, the keys): the answer shown at that
// place (0 the first), told as its id
void dialog_choose(dialog_t *dialog, int place);
void dialog_cancel(dialog_t *dialog);

bool dialog_poll_event(dialog_t *dialog, dialog_event_t *event_out);

#endif
