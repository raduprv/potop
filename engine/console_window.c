// The console drawn (console.h: console_draw): across the top, the log and
// the line being typed

#include "console.h"

#include <SDL3/SDL.h>

#include "ui_widgets.h"

#define CONSOLE_WINDOW "Console"

bool console_draw(console_t *console, ui_backend_t *backend, const ui_style_t *ui_style)
{
	struct nk_context *context = &backend->context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR;
	float height;
	struct nk_rect bounds;

	if (console == NULL || !console->open)
	{
		if (nk_window_find(context, CONSOLE_WINDOW) != NULL && !nk_window_is_closed(context, CONSOLE_WINDOW))
		{
			nk_window_close(context, CONSOLE_WINDOW);
		}
		return false;
	}
	if (nk_window_is_hidden(context, CONSOLE_WINDOW) && nk_window_find(context, CONSOLE_WINDOW) != NULL)
	{
		nk_window_show(context, CONSOLE_WINDOW, NK_SHOWN);
	}
	height = SDL_floorf(SDL_max(backend->height * 0.45f, 4.0f * UI_SMALL_ROW_HEIGHT + UI_ROW_HEIGHT));
	bounds = nk_rect(0.0f, 0.0f, backend->width, height);
	// The typing goes to the console (the application hands it the keys), and SDL
	// sends text only while some field wants it
	backend->wants_text_input = true;
	// Nearly solid: the log is read over a busy scene
	{
		struct nk_color background = ui_style->colors[NK_COLOR_WINDOW];

		background.a = 240;
		nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	}
	if (nk_begin(context, CONSOLE_WINDOW, bounds, flags))
	{
		const struct nk_style *style = &context->style;
		struct nk_color normal = style->text.color;
		struct nk_color warning = nk_rgb(240, 200, 90);
		struct nk_color error = nk_rgb(240, 110, 90);
		struct nk_color dim = nk_rgba(normal.r, normal.g, normal.b, 150);
		float row = UI_SMALL_ROW_HEIGHT + style->window.spacing.y;
		float room = height - 2.0f * style->window.padding.y - UI_ROW_HEIGHT - 2.0f * style->window.spacing.y;
		int32_t visible = SDL_max((int32_t)(room / row), 1);
		char text[CONSOLE_LINE_LENGTH + 32];
		char typed[CONSOLE_INPUT_LENGTH + 8];

		for (int32_t i = visible - 1; i >= 0; i--)
		{
			SDL_LogPriority priority = SDL_LOG_PRIORITY_INFO;

			nk_layout_row_dynamic(context, UI_SMALL_ROW_HEIGHT, 1);
			if (console_line(console, console->scroll_lines + i, text, sizeof(text), &priority))
			{
				struct nk_color color = priority >= SDL_LOG_PRIORITY_ERROR ? error : priority == SDL_LOG_PRIORITY_WARN ? warning : normal;

				nk_label_colored(context, text, NK_TEXT_LEFT, color);
			}
			else
			{
				nk_spacer(context);
			}
		}
		// The line, with the cursor where it is
		SDL_snprintf(typed, sizeof(typed), "> %.*s|%s", (int)console->cursor, console->input, console->input + console->cursor);
		if (console->scroll_lines > 0)
		{
			nk_layout_row(context, NK_DYNAMIC, UI_ROW_HEIGHT, 2, (const float[]){ 0.8f, 0.2f });
			nk_label(context, typed, NK_TEXT_LEFT);
			SDL_snprintf(text, sizeof(text), "%d lines up", (int)console->scroll_lines);
			nk_label_colored(context, text, NK_TEXT_RIGHT, dim);
		}
		else
		{
			nk_layout_row_dynamic(context, UI_ROW_HEIGHT, 1);
			nk_label(context, typed, NK_TEXT_LEFT);
		}
	}
	nk_end(context);
	nk_style_pop_style_item(context);
	return true;
}
