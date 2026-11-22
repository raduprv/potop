// The small widgets (ui_widgets.h)

#include "ui_widgets.h"

#include <SDL3/SDL.h>

void ui_widget_tooltip(struct nk_context *context, const char *tooltip)
{
	// (a tooltip is a popup, and a popup cannot have one: inside a menu, a
	// combo's list or a tooltip none is shown; Nuklear asserts, the program
	// stops)
	if (context->current != NULL && context->current->layout != NULL && ((int)context->current->layout->type & (int)NK_PANEL_SET_POPUP) != 0)
	{
		return;
	}
	if (tooltip != NULL && tooltip[0] != '\0' && nk_widget_is_hovered(context))
	{
		nk_tooltip(context, tooltip);
	}
}

bool ui_widget_slider(struct nk_context *context, const char *label, float *value, float minimum, float maximum, float step, const char *format,
	const char *tooltip)
{
	float before = *value;
	char text[48];

	nk_layout_row(context, NK_DYNAMIC, UI_ROW_HEIGHT - 4.0f, 3, (const float[]){ 0.38f, 0.42f, 0.20f });
	ui_widget_tooltip(context, tooltip);
	nk_label(context, label, NK_TEXT_LEFT);
	ui_widget_tooltip(context, tooltip);
	nk_slider_float(context, minimum, value, maximum, step);
	SDL_snprintf(text, sizeof(text), format, (double)*value);
	nk_label(context, text, NK_TEXT_RIGHT);
	return *value != before;
}

bool ui_widget_slider_int(struct nk_context *context, const char *label, int *value, int minimum, int maximum, int step, const char *tooltip)
{
	int before = *value;
	char text[32];

	nk_layout_row(context, NK_DYNAMIC, UI_ROW_HEIGHT - 4.0f, 3, (const float[]){ 0.38f, 0.42f, 0.20f });
	ui_widget_tooltip(context, tooltip);
	nk_label(context, label, NK_TEXT_LEFT);
	ui_widget_tooltip(context, tooltip);
	nk_slider_int(context, minimum, value, maximum, step);
	SDL_snprintf(text, sizeof(text), "%d", *value);
	nk_label(context, text, NK_TEXT_RIGHT);
	return *value != before;
}

bool ui_widget_checkbox(struct nk_context *context, const char *label, bool *value, const char *tooltip)
{
	nk_bool checked = *value;

	nk_layout_row_dynamic(context, UI_ROW_HEIGHT - 4.0f, 1);
	ui_widget_tooltip(context, tooltip);
	nk_checkbox_label(context, label, &checked);
	if ((bool)checked != *value)
	{
		*value = checked;
		return true;
	}
	return false;
}

bool ui_widget_color(struct nk_context *context, const char *label, float color[3], const char *tooltip)
{
	struct nk_colorf value = { color[0], color[1], color[2], 1.0f };
	bool changed = false;

	nk_layout_row(context, NK_DYNAMIC, UI_ROW_HEIGHT - 4.0f, 2, (const float[]){ 0.38f, 0.62f });
	ui_widget_tooltip(context, tooltip);
	nk_label(context, label, NK_TEXT_LEFT);
	if (nk_combo_begin_color(context, nk_rgb_cf(value), nk_vec2(240.0f, 300.0f)))
	{
		struct nk_colorf picked;

		nk_layout_row_dynamic(context, 160.0f, 1);
		picked = nk_color_picker(context, value, NK_RGB);
		nk_layout_row_dynamic(context, UI_ROW_HEIGHT - 4.0f, 1);
		picked.r = nk_propertyf(context, "#R:", 0.0f, picked.r, 1.0f, 0.01f, 0.005f);
		picked.g = nk_propertyf(context, "#G:", 0.0f, picked.g, 1.0f, 0.01f, 0.005f);
		picked.b = nk_propertyf(context, "#B:", 0.0f, picked.b, 1.0f, 0.01f, 0.005f);
		if (picked.r != value.r || picked.g != value.g || picked.b != value.b)
		{
			color[0] = picked.r;
			color[1] = picked.g;
			color[2] = picked.b;
			changed = true;
		}
		nk_combo_end(context);
	}
	return changed;
}

bool ui_widget_choice(struct nk_context *context, const char *label, int32_t *value, const char *choices, const char *tooltip)
{
	char texts[16][16];
	const char *items[16];
	int values[16];
	int count = 0;
	int selected = -1;
	const char *cursor = choices;

	while (*cursor != '\0' && count < 16)
	{
		const char *end = SDL_strchr(cursor, '|');
		size_t length = end != NULL ? (size_t)(end - cursor) : SDL_strlen(cursor);

		SDL_strlcpy(texts[count], cursor, SDL_min(length + 1, sizeof(texts[count])));
		values[count] = SDL_atoi(texts[count]);
		items[count] = texts[count];
		if (SDL_strchr(texts[count], ':') != NULL)
		{
			// Named: the value, then its name
			items[count] = SDL_strchr(texts[count], ':') + 1;
		}
		if (values[count] == *value)
		{
			selected = count;
		}
		count++;
		cursor += length;
		if (*cursor == '|')
		{
			cursor++;
		}
	}
	nk_layout_row(context, NK_DYNAMIC, UI_ROW_HEIGHT - 4.0f, 2, (const float[]){ 0.38f, 0.62f });
	ui_widget_tooltip(context, tooltip);
	nk_label(context, label, NK_TEXT_LEFT);
	// Inside a drop-down (the editor's bar): the choices as buttons, as a
	// drop-down cannot hold another
	if (context->current != NULL && context->current->layout != NULL && ((int)context->current->layout->type & (int)NK_PANEL_SET_POPUP) != 0)
	{
		int chosen = selected;

		for (int i = 0; i < count; i++)
		{
			if (i % 3 == 0)
			{
				nk_layout_row_dynamic(context, UI_ROW_HEIGHT - 4.0f, 3);
			}
			if (nk_select_label(context, items[i], NK_TEXT_CENTERED, i == selected))
			{
				chosen = i;
			}
		}
		if (chosen != selected)
		{
			*value = values[chosen];
			return true;
		}
		return false;
	}
	if (selected < 0)
	{
		// A value from the ini file that is not in the list: shown as is
		char current[16];

		SDL_snprintf(current, sizeof(current), "%d", *value);
		if (nk_combo_begin_label(context, current, nk_vec2(nk_widget_width(context), 200.0f)))
		{
			nk_layout_row_dynamic(context, UI_ROW_HEIGHT - 4.0f, 1);
			for (int i = 0; i < count; i++)
			{
				if (nk_combo_item_label(context, items[i], NK_TEXT_LEFT))
				{
					selected = i;
				}
			}
			nk_combo_end(context);
		}
		if (selected >= 0)
		{
			*value = values[selected];
			return true;
		}
		return false;
	}
	{
		int chosen = nk_combo(context, items, count, selected, (int)(UI_ROW_HEIGHT - 4.0f), nk_vec2(nk_widget_width(context), 200.0f));

		if (chosen != selected)
		{
			*value = values[chosen];
			return true;
		}
	}
	return false;
}

void ui_widget_section(struct nk_context *context, const char *text)
{
	struct nk_color color = context->style.text.color;

	color.a = 150;
	nk_layout_row_dynamic(context, UI_SMALL_ROW_HEIGHT, 1);
	nk_label_colored(context, text, NK_TEXT_LEFT, color);
}

bool ui_widget_toggle_button(struct nk_context *context, const char *label, const char *tooltip, bool selected)
{
	nk_bool value = selected;

	ui_widget_tooltip(context, tooltip);
	nk_selectable_label(context, label, NK_TEXT_CENTERED, &value);
	return value != selected;
}
