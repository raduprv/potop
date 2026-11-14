// How the interface looks (ui_style.h)

#include "ui_style.h"

#include <SDL3/SDL.h>

#include "ini_parser.h"

static const struct nk_color default_colors[NK_COLOR_COUNT] = {
	[NK_COLOR_TEXT] = { 226, 229, 233, 255 },
	[NK_COLOR_WINDOW] = { 31, 35, 41, 242 },
	[NK_COLOR_HEADER] = { 43, 49, 57, 255 },
	[NK_COLOR_BORDER] = { 66, 74, 85, 255 },
	[NK_COLOR_BUTTON] = { 52, 59, 69, 255 },
	[NK_COLOR_BUTTON_HOVER] = { 65, 74, 86, 255 },
	[NK_COLOR_BUTTON_ACTIVE] = { 74, 118, 168, 255 },
	[NK_COLOR_TOGGLE] = { 58, 65, 76, 255 },
	[NK_COLOR_TOGGLE_HOVER] = { 72, 81, 94, 255 },
	[NK_COLOR_TOGGLE_CURSOR] = { 108, 168, 228, 255 },
	[NK_COLOR_SELECT] = { 46, 52, 61, 255 },
	[NK_COLOR_SELECT_ACTIVE] = { 70, 116, 168, 255 },
	[NK_COLOR_SLIDER] = { 50, 56, 66, 255 },
	[NK_COLOR_SLIDER_CURSOR] = { 108, 163, 222, 255 },
	[NK_COLOR_SLIDER_CURSOR_HOVER] = { 128, 182, 238, 255 },
	[NK_COLOR_SLIDER_CURSOR_ACTIVE] = { 150, 198, 248, 255 },
	[NK_COLOR_PROPERTY] = { 48, 54, 64, 255 },
	[NK_COLOR_EDIT] = { 38, 43, 50, 255 },
	[NK_COLOR_EDIT_CURSOR] = { 226, 229, 233, 255 },
	[NK_COLOR_COMBO] = { 48, 54, 64, 255 },
	[NK_COLOR_CHART] = { 48, 54, 64, 255 },
	[NK_COLOR_CHART_COLOR] = { 108, 163, 222, 255 },
	[NK_COLOR_CHART_COLOR_HIGHLIGHT] = { 255, 200, 90, 255 },
	[NK_COLOR_SCROLLBAR] = { 35, 39, 46, 255 },
	[NK_COLOR_SCROLLBAR_CURSOR] = { 68, 76, 87, 255 },
	[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = { 84, 93, 106, 255 },
	[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = { 100, 110, 124, 255 },
	[NK_COLOR_TAB_HEADER] = { 43, 49, 57, 255 },
	[NK_COLOR_KNOB] = { 50, 56, 66, 255 },
	[NK_COLOR_KNOB_CURSOR] = { 108, 163, 222, 255 },
	[NK_COLOR_KNOB_CURSOR_HOVER] = { 128, 182, 238, 255 },
	[NK_COLOR_KNOB_CURSOR_ACTIVE] = { 150, 198, 248, 255 },
};

// "NK_COLOR_BUTTON_HOVER" -> "button_hover"
static void color_key(enum nk_style_colors color, char *key_out, size_t key_out_size)
{
	const char *name = nk_style_get_color_by_name(color);
	size_t length = 0;

	if (SDL_strncmp(name, "NK_COLOR_", 9) == 0)
	{
		name += 9;
	}
	for (; *name != '\0' && length + 1 < key_out_size; name++)
	{
		key_out[length++] = (char)SDL_tolower((unsigned char)*name);
	}
	key_out[length] = '\0';
}

static bool parse_color(const char *value, struct nk_color *color_out)
{
	int red;
	int green;
	int blue;
	int alpha = 255;
	int count = SDL_sscanf(value, "%d , %d , %d , %d", &red, &green, &blue, &alpha);

	if (count < 3)
	{
		return false;
	}
	*color_out = nk_rgba(SDL_clamp(red, 0, 255), SDL_clamp(green, 0, 255), SDL_clamp(blue, 0, 255), SDL_clamp(alpha, 0, 255));
	return true;
}

void ui_style_defaults(ui_style_t *style)
{
	SDL_zerop(style);
	SDL_strlcpy(style->font_path, "ui/fonts/DejaVuSans.ttf", sizeof(style->font_path));
	style->font_size = 15.0f;
	style->dialog_text_scale = 1.6f;
	style->scale = 1.0f;
	style->panel_opacity = 0.6f;
	SDL_memcpy(style->colors, default_colors, sizeof(style->colors));
}

bool ui_style_key(ui_style_t *style, const char *section, const char *key, const char *value)
{
	if (SDL_strcmp(section, "ui") == 0)
	{
		if (SDL_strcmp(key, "font") == 0)
		{
			SDL_strlcpy(style->font_path, value, sizeof(style->font_path));
			return true;
		}
		if (SDL_strcmp(key, "font_size") == 0)
		{
			return ini_value_to_float(value, &style->font_size);
		}
		if (SDL_strcmp(key, "dialog_text_scale") == 0)
		{
			return ini_value_to_float(value, &style->dialog_text_scale);
		}
		if (SDL_strcmp(key, "scale") == 0)
		{
			return ini_value_to_float(value, &style->scale);
		}
		if (SDL_strcmp(key, "panel_opacity") == 0)
		{
			return ini_value_to_float(value, &style->panel_opacity);
		}
		return false;
	}
	if (SDL_strcmp(section, "colors") == 0)
	{
		for (int color = 0; color < NK_COLOR_COUNT; color++)
		{
			char name[48];

			color_key((enum nk_style_colors)color, name, sizeof(name));
			if (SDL_strcmp(name, key) == 0)
			{
				return parse_color(value, &style->colors[color]);
			}
		}
	}
	return false;
}

static bool style_file_key(void *user_data, const char *section, const char *key, const char *value)
{
	ui_style_key(user_data, section, key, value);
	// (the rest of ui.ini is the editor's)
	return true;
}

void ui_style_clamp(ui_style_t *style)
{
	style->font_size = SDL_clamp(style->font_size, 8.0f, 40.0f);
	style->scale = SDL_clamp(style->scale, 0.5f, 3.0f);
	style->panel_opacity = SDL_clamp(style->panel_opacity, 0.0f, 1.0f);
	style->dialog_text_scale = SDL_clamp(style->dialog_text_scale, 1.0f, 3.0f);
}

void ui_style_load(ui_style_t *style)
{
	ini_parse_data_file("ui.ini", style_file_key, style);
	ui_style_clamp(style);
}

void ui_style_apply(const ui_style_t *ui, struct nk_context *context)
{
	struct nk_style *style = &context->style;

	nk_style_from_table(context, ui->colors);
	style->window.padding = nk_vec2(8.0f, 6.0f);
	style->window.spacing = nk_vec2(4.0f, 4.0f);
	style->window.rounding = 5.0f;
	style->window.border = 1.0f;
	style->window.header.padding = nk_vec2(8.0f, 5.0f);
	style->window.group_padding = nk_vec2(4.0f, 4.0f);
	style->button.rounding = 3.0f;
	style->button.padding = nk_vec2(6.0f, 3.0f);
	style->selectable.rounding = 3.0f;
	style->selectable.padding = nk_vec2(4.0f, 3.0f);
	style->slider.cursor_size = nk_vec2(14.0f, 14.0f);
	style->slider.bar_height = 6.0f;
	style->edit.rounding = 3.0f;
	style->combo.rounding = 3.0f;
	style->property.rounding = 3.0f;
	// The selected tool in the accent color, with light text
	style->selectable.text_normal_active = nk_rgb(255, 255, 255);
	style->selectable.text_hover_active = nk_rgb(255, 255, 255);
	style->selectable.normal = nk_style_item_color(ui->colors[NK_COLOR_BUTTON]);
	style->selectable.hover = nk_style_item_color(ui->colors[NK_COLOR_BUTTON_HOVER]);
	style->selectable.pressed = nk_style_item_color(ui->colors[NK_COLOR_BUTTON_ACTIVE]);
	style->selectable.normal_active = nk_style_item_color(ui->colors[NK_COLOR_SELECT_ACTIVE]);
	style->selectable.hover_active = nk_style_item_color(ui->colors[NK_COLOR_SELECT_ACTIVE]);
	style->selectable.pressed_active = nk_style_item_color(ui->colors[NK_COLOR_BUTTON_ACTIVE]);
}
