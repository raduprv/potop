// Talking. See dialog.h.

#include "dialog.h"

#include <SDL3/SDL.h>

void dialog_create(dialog_t *dialog)
{
	SDL_zerop(dialog);
}

static void push_event(dialog_t *dialog, dialog_event_type_t type, uint32_t choice)
{
	dialog_event_t *event;

	if (dialog->event_count == DIALOG_EVENT_QUEUE)
	{
		dialog->event_first = (dialog->event_first + 1) % DIALOG_EVENT_QUEUE;
		dialog->event_count--;
	}
	event = &dialog->events[(dialog->event_first + dialog->event_count) % DIALOG_EVENT_QUEUE];
	event->type = type;
	event->actor = dialog->actor;
	event->choice = choice;
	dialog->event_count++;
}

bool dialog_poll_event(dialog_t *dialog, dialog_event_t *event_out)
{
	if (dialog->event_count == 0)
	{
		return false;
	}
	*event_out = dialog->events[dialog->event_first];
	dialog->event_first = (dialog->event_first + 1) % DIALOG_EVENT_QUEUE;
	dialog->event_count--;
	return true;
}

void dialog_open(dialog_t *dialog, uint32_t actor, const char *speaker)
{
	dialog->open = true;
	dialog->actor = actor;
	SDL_strlcpy(dialog->speaker, speaker != NULL ? speaker : "", sizeof(dialog->speaker));
	dialog->text[0] = '\0';
	dialog->choice_count = 0;
}

void dialog_say(dialog_t *dialog, const char *text)
{
	SDL_strlcpy(dialog->text, text != NULL ? text : "", sizeof(dialog->text));
	dialog->choice_count = 0;
}

void dialog_clear_choices(dialog_t *dialog)
{
	dialog->choice_count = 0;
}

void dialog_add_choice(dialog_t *dialog, uint32_t id, const char *text)
{
	int place = dialog->choice_count;

	for (int i = 0; i < dialog->choice_count; i++)
	{
		if (dialog->choice_ids[i] == id)
		{
			place = i;
		}
	}
	if (place >= DIALOG_CHOICES_MAX)
	{
		return;
	}
	dialog->choice_ids[place] = id;
	SDL_strlcpy(dialog->choices[place], text != NULL ? text : "", sizeof(dialog->choices[place]));
	if (place == dialog->choice_count)
	{
		dialog->choice_count++;
	}
}

void dialog_remove_choice(dialog_t *dialog, uint32_t id)
{
	for (int i = 0; i < dialog->choice_count; i++)
	{
		if (dialog->choice_ids[i] == id)
		{
			SDL_memmove(&dialog->choices[i], &dialog->choices[i + 1], sizeof(dialog->choices[0]) * (size_t)(dialog->choice_count - i - 1));
			SDL_memmove(&dialog->choice_ids[i], &dialog->choice_ids[i + 1], sizeof(uint32_t) * (size_t)(dialog->choice_count - i - 1));
			dialog->choice_count--;
			return;
		}
	}
}

void dialog_set_choices(dialog_t *dialog, const dialog_choice_t *choices, int count)
{
	dialog->choice_count = 0;
	for (int i = 0; i < count && choices != NULL; i++)
	{
		dialog_add_choice(dialog, choices[i].id, choices[i].text);
	}
}

void dialog_close(dialog_t *dialog)
{
	dialog->open = false;
	dialog->choice_count = 0;
}

void dialog_choose(dialog_t *dialog, int place)
{
	if (!dialog->open || place < 0 || place >= SDL_max(dialog->choice_count, 1))
	{
		return;
	}
	push_event(dialog, DIALOG_EVENT_CHOSEN, dialog->choice_count > 0 ? dialog->choice_ids[place] : 0u);
}

void dialog_cancel(dialog_t *dialog)
{
	if (dialog->open)
	{
		push_event(dialog, DIALOG_EVENT_CLOSED, 0u);
		dialog->open = false;
	}
}
