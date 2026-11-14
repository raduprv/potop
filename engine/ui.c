// The game's user interface. See ui.h.

#include "ui.h"

#include "cloud_weather.h"
#include "file_io.h"
#include "ini_parser.h"
#include "season.h"
#include "stamps.h"

// (the maps window's, further down)
static bool map_name_is_baked(const char *name);
static void map_name_without_kind(const char *name, char *name_out, size_t name_out_size);

#define UI_STATE_FILE "ui_state.ini"

#define BRUSHES_WINDOW "Brushes"
#define ICON_BAR_WINDOW "icon_bar"
#define STATUS_WINDOW "status_line"
#define MESSAGE_WINDOW "message"
#define PICK_WINDOW "pick"
#define SETTINGS_WINDOW "Settings"
#define DIAGNOSTICS_WINDOW "Diagnostics"
#define INSPECTOR_WINDOW "Inspector"
#define MAPS_WINDOW "Maps"
#define SKY_WINDOW "Sky"
#define WORLD_WINDOW "World"
#define CONSOLE_WINDOW "Console"

#define BRUSHES_WIDTH 276.0f
#define ROW_HEIGHT 26.0f
#define SMALL_ROW_HEIGHT 20.0f

// ---------------------------------------------------------------------------
// Configuration: ui.ini, then ui_state.ini

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

static bool config_key(void *user_data, const char *section, const char *key, const char *value)
{
	ui_t *ui = user_data;

	if (SDL_strcmp(section, "ui") == 0)
	{
		if (SDL_strcmp(key, "font") == 0)
		{
			SDL_strlcpy(ui->font_path, value, sizeof(ui->font_path));
			return true;
		}
		if (SDL_strcmp(key, "font_size") == 0)
		{
			return ini_value_to_float(value, &ui->font_size);
		}
		if (SDL_strcmp(key, "dialog_text_scale") == 0)
		{
			return ini_value_to_float(value, &ui->dialog_text_scale);
		}
		if (SDL_strcmp(key, "scale") == 0)
		{
			return ini_value_to_float(value, &ui->scale);
		}
		if (SDL_strcmp(key, "panel_opacity") == 0)
		{
			return ini_value_to_float(value, &ui->panel_opacity);
		}
		if (SDL_strcmp(key, "show_status_line") == 0)
		{
			return ini_value_to_bool(value, &ui->show_status_line);
		}
		if (SDL_strcmp(key, "icon_labels") == 0)
		{
			return ini_value_to_bool(value, &ui->icon_labels);
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
				return parse_color(value, &ui->colors[color]);
			}
		}
		return false;
	}
	if (SDL_strcmp(section, "icons") == 0)
	{
		ui_icon_t *icon;

		for (int i = 0; i < ui->icon_count; i++)
		{
			if (SDL_strcmp(ui->icons[i].key, key) == 0)
			{
				SDL_strlcpy(ui->icons[i].path, value, sizeof(ui->icons[i].path));
				return true;
			}
		}
		if (ui->icon_count >= UI_ICONS_MAX)
		{
			return false;
		}
		icon = &ui->icons[ui->icon_count++];
		SDL_zerop(icon);
		SDL_strlcpy(icon->key, key, sizeof(icon->key));
		SDL_strlcpy(icon->path, value, sizeof(icon->path));
		return true;
	}
	return false;
}

static bool read_rect(const char *value, struct nk_rect *rect_out)
{
	float x;
	float y;
	float width;
	float height;

	if (SDL_sscanf(value, "%f , %f , %f , %f", &x, &y, &width, &height) != 4)
	{
		return false;
	}
	*rect_out = nk_rect(x, y, width, height);
	return true;
}

static bool state_key(void *user_data, const char *section, const char *key, const char *value)
{
	ui_t *ui = user_data;

	if (SDL_strcmp(section, "ui") == 0)
	{
		return config_key(user_data, section, key, value);
	}
	if (SDL_strcmp(section, "windows") != 0)
	{
		return false;
	}
	if (SDL_strcmp(key, "brushes") == 0)
	{
		return read_rect(value, &ui->brushes_bounds);
	}
	if (SDL_strcmp(key, "brushes_collapsed") == 0)
	{
		return ini_value_to_bool(value, &ui->brushes_collapsed);
	}
	if (SDL_strcmp(key, "settings") == 0)
	{
		return read_rect(value, &ui->settings_bounds);
	}
	if (SDL_strcmp(key, "settings_tab") == 0)
	{
		return ini_value_to_int(value, &ui->settings_tab);
	}
	if (SDL_strcmp(key, "diagnostics") == 0)
	{
		return read_rect(value, &ui->diagnostics_bounds);
	}
	if (SDL_strcmp(key, "inspector") == 0)
	{
		return read_rect(value, &ui->inspector_bounds);
	}
	if (SDL_strcmp(key, "diagnostics_open") == 0)
	{
		return ini_value_to_bool(value, &ui->diagnostics_open);
	}
	if (SDL_strcmp(key, "sky") == 0)
	{
		return read_rect(value, &ui->sky_bounds);
	}
	if (SDL_strcmp(key, "world") == 0)
	{
		return read_rect(value, &ui->world_bounds);
	}
	if (SDL_strcmp(key, "last_map") == 0)
	{
		SDL_strlcpy(ui->last_map_name, value, sizeof(ui->last_map_name));
		return true;
	}
	return false;
}

static void save_state(const ui_t *ui)
{
	char path[1024];
	char text[2048];
	const struct nk_rect *brushes = &ui->brushes_bounds;
	const struct nk_rect *settings = &ui->settings_bounds;
	const struct nk_rect *diagnostics = &ui->diagnostics_bounds;
	const struct nk_rect *inspector = &ui->inspector_bounds;
	const struct nk_rect *sky = &ui->sky_bounds;
	const struct nk_rect *world = &ui->world_bounds;

	if (!file_io_preference_path(UI_STATE_FILE, path, sizeof(path)))
	{
		return;
	}
	SDL_snprintf(text, sizeof(text),
		"; Where the windows of the interface are, and what was changed in it.\n"
		"; Delete this file to go back to data/ui.ini.\n"
		"[ui]\nscale = %g\nfont_size = %g\ndialog_text_scale = %g\npanel_opacity = %g\nshow_status_line = %s\n\n"
		"[windows]\nbrushes = %.0f, %.0f, %.0f, %.0f\nbrushes_collapsed = %s\n"
		"settings = %.0f, %.0f, %.0f, %.0f\nsettings_tab = %d\n"
		"diagnostics = %.0f, %.0f, %.0f, %.0f\ndiagnostics_open = %s\ninspector = %.0f, %.0f, %.0f, %.0f\n"
		"sky = %.0f, %.0f, %.0f, %.0f\nworld = %.0f, %.0f, %.0f, %.0f\nlast_map = %s\n",
		(double)ui->scale, (double)ui->font_size, (double)ui->dialog_text_scale, (double)ui->panel_opacity, ui->show_status_line ? "true" : "false",
		(double)brushes->x, (double)brushes->y, (double)brushes->w, (double)brushes->h, ui->brushes_collapsed ? "true" : "false",
		(double)settings->x, (double)settings->y, (double)settings->w, (double)settings->h, ui->settings_tab,
		(double)diagnostics->x, (double)diagnostics->y, (double)diagnostics->w, (double)diagnostics->h,
		ui->diagnostics_open ? "true" : "false", (double)inspector->x, (double)inspector->y, (double)inspector->w, (double)inspector->h,
		(double)sky->x, (double)sky->y, (double)sky->w, (double)sky->h,
		(double)world->x, (double)world->y, (double)world->w, (double)world->h, ui->last_map_name);
	file_io_save_file(path, text, SDL_strlen(text));
}

static void load_state(ui_t *ui)
{
	char path[1024];
	char *text;

	if (!file_io_preference_path(UI_STATE_FILE, path, sizeof(path)))
	{
		return;
	}
	text = file_io_load_file(path, NULL);
	if (text == NULL)
	{
		return;
	}
	ini_parse_buffer(text, UI_STATE_FILE, state_key, ui);
	SDL_free(text);
	ui->state_loaded = true;
}

// ---------------------------------------------------------------------------
// Style and icons

static void apply_style(ui_t *ui)
{
	struct nk_context *context = &ui->backend.context;
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

// The icon for a button, NULL if ui.ini names none or it cannot be read
static const struct nk_image *icon_for(ui_t *ui, const char *key)
{
	for (int i = 0; i < ui->icon_count; i++)
	{
		ui_icon_t *icon = &ui->icons[i];

		if (SDL_strcmp(icon->key, key) != 0)
		{
			continue;
		}
		if (!icon->tried)
		{
			icon->tried = true;
			icon->loaded = icon->path[0] != '\0' && ui_backend_load_image(icon->path, &icon->image);
		}
		return icon->loaded ? &icon->image : NULL;
	}
	return NULL;
}

static void free_icons(ui_t *ui)
{
	for (int i = 0; i < ui->icon_count; i++)
	{
		if (ui->icons[i].loaded)
		{
			ui_backend_free_image(&ui->icons[i].image);
		}
		ui->icons[i].loaded = false;
		ui->icons[i].tried = false;
	}
}

// ---------------------------------------------------------------------------
// Life cycle

bool ui_rebuild(ui_t *ui)
{
	free_icons(ui);
	if (ui->available)
	{
		ui_backend_destroy(&ui->backend);
	}
	ui->available = ui_backend_create(&ui->backend, ui->window, ui->font_path, ui->font_size, ui->dialog_text_scale, ui->scale);
	if (!ui->available)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The interface is off; the keys still work");
		return false;
	}
	apply_style(ui);
	ui->brushes_placed = false;
	return true;
}

bool ui_create(ui_t *ui, SDL_Window *window)
{
	SDL_zerop(ui);
	ui->window = window;
	SDL_strlcpy(ui->font_path, "ui/fonts/DejaVuSans.ttf", sizeof(ui->font_path));
	ui->font_size = 15.0f;
	ui->dialog_text_scale = 1.6f;
	ui->scale = 1.0f;
	ui->panel_opacity = 0.6f;
	ui->show_status_line = true;
	ui->icon_labels = true;
	SDL_memcpy(ui->colors, default_colors, sizeof(ui->colors));
	ui->brushes_bounds = nk_rect(12.0f, 12.0f, BRUSHES_WIDTH, 400.0f);
	ui->settings_bounds = nk_rect(320.0f, 60.0f, 520.0f, 560.0f);
	ui->diagnostics_bounds = nk_rect(320.0f, 80.0f, 640.0f, 470.0f);
	ui->sky_bounds = nk_rect(360.0f, 60.0f, 520.0f, 700.0f);
	ui->world_bounds = nk_rect(400.0f, 60.0f, 300.0f, 160.0f);
	ui->sky_brush_kilometers = 1.5f;
	ui->sky_brush_strength = 1.0f;
	ui->inspector_bounds = nk_rect(300.0f, 40.0f, 760.0f, 720.0f);
	ui->maps_bounds = nk_rect(360.0f, 90.0f, 440.0f, 420.0f);
	ui->selected_map = -1;
	ini_parse_data_file("ui.ini", config_key, ui);
	load_state(ui);
	ui->font_size = SDL_clamp(ui->font_size, 8.0f, 40.0f);
	ui->scale = SDL_clamp(ui->scale, 0.5f, 3.0f);
	ui->panel_opacity = SDL_clamp(ui->panel_opacity, 0.0f, 1.0f);
	ui->pending_scale = ui->scale;
	ui->pending_font_size = ui->font_size;
	ui->dialog_text_scale = SDL_clamp(ui->dialog_text_scale, 1.0f, 3.0f);
	ui->pending_dialog_text_scale = ui->dialog_text_scale;
	return ui_rebuild(ui);
}

void ui_destroy(ui_t *ui)
{
	if (ui->window != NULL)
	{
		save_state(ui);
	}
	free_icons(ui);
	if (ui->available)
	{
		ui_backend_destroy(&ui->backend);
	}
	SDL_zerop(ui);
}

void ui_input_begin(ui_t *ui)
{
	if (ui->available)
	{
		ui_backend_input_begin(&ui->backend);
	}
}

bool ui_handle_event(ui_t *ui, const SDL_Event *event)
{
	if (!ui->available || ui->hidden)
	{
		return false;
	}
	return ui_backend_handle_event(&ui->backend, event);
}

void ui_input_end(ui_t *ui)
{
	if (ui->available)
	{
		ui_backend_input_end(&ui->backend);
	}
}

void ui_show_message(ui_t *ui, const char *message, double time_seconds)
{
	SDL_strlcpy(ui->message, message, sizeof(ui->message));
	ui->message_until_seconds = time_seconds + 4.0;
}

bool ui_close_windows(ui_t *ui)
{
	bool any = ui->available && !ui->hidden
		&& (ui->settings_open || ui->diagnostics_open || ui->sky_open || ui->world_open || ui->maps_mode != UI_MAPS_CLOSED);

	if (any)
	{
		// The topmost first: maps, then the world, the sky, settings, then
		// diagnostics
		if (ui->maps_mode != UI_MAPS_CLOSED)
		{
			ui->maps_mode = UI_MAPS_CLOSED;
		}
		else if (ui->world_open)
		{
			ui->world_open = false;
		}
		else if (ui->sky_open)
		{
			ui->sky_open = false;
		}
		else if (ui->settings_open)
		{
			ui->settings_open = false;
		}
		else
		{
			ui->diagnostics_open = false;
		}
	}
	return any;
}

void ui_set_map_name(ui_t *ui, const char *name)
{
	SDL_strlcpy(ui->last_map_name, name, sizeof(ui->last_map_name));
}

// ---------------------------------------------------------------------------
// Small widgets

static float text_width(ui_t *ui, const char *text)
{
	const struct nk_user_font *font = ui->backend.context.style.font;

	return font->width(font->userdata, font->height, text, nk_strlen(text));
}

static void tooltip_if_hovered(struct nk_context *context, const char *tooltip)
{
	if (tooltip != NULL && tooltip[0] != '\0' && nk_widget_is_hovered(context))
	{
		nk_tooltip(context, tooltip);
	}
}

// Draws an icon into a widget's bounds: centered, or at the left followed
// by the label
static void draw_icon(ui_t *ui, struct nk_rect bounds, const struct nk_image *icon, const char *label, bool selected)
{
	struct nk_context *context = &ui->backend.context;
	struct nk_command_buffer *canvas = nk_window_get_canvas(context);
	float size = SDL_max(bounds.h - 6.0f, 4.0f);
	struct nk_rect place;

	if (label == NULL)
	{
		place = nk_rect(bounds.x + 0.5f * (bounds.w - size), bounds.y + 3.0f, size, size);
		nk_draw_image(canvas, place, icon, nk_rgb(255, 255, 255));
		return;
	}
	{
		const struct nk_user_font *font = context->style.font;
		int length = nk_strlen(label);
		float label_width = font->width(font->userdata, font->height, label, length);
		float total = size + 4.0f + label_width;
		float start = bounds.x + SDL_max(0.5f * (bounds.w - total), 3.0f);
		struct nk_color color = selected ? context->style.selectable.text_normal_active : context->style.selectable.text_normal;

		nk_draw_image(canvas, nk_rect(start, bounds.y + 3.0f, size, size), icon, nk_rgb(255, 255, 255));
		nk_draw_text(canvas, nk_rect(start + size + 4.0f, bounds.y + 0.5f * (bounds.h - font->height), label_width + 2.0f, font->height),
			label, length, font, nk_rgba(0, 0, 0, 0), color);
	}
}

// A tool button: its icon (with its name, if ui.ini says so), or just its
// name; highlighted while selected. Returns true when clicked.
static bool tool_button(ui_t *ui, const char *icon_key, const char *label, const char *tooltip, bool selected)
{
	struct nk_context *context = &ui->backend.context;
	const struct nk_image *icon = icon_for(ui, icon_key);
	nk_bool value = selected;
	struct nk_rect bounds = nk_widget_bounds(context);

	if (icon != NULL && tooltip == NULL && !ui->icon_labels)
	{
		// Without its name on it, the name goes into the tooltip
		tooltip = label;
	}
	tooltip_if_hovered(context, tooltip);
	nk_selectable_label(context, icon != NULL ? " " : label, NK_TEXT_CENTERED, &value);
	if (icon != NULL)
	{
		draw_icon(ui, bounds, icon, ui->icon_labels ? label : NULL, value);
	}
	return value != selected;
}

// A plain button with an icon or its name; tooltip on hover
static bool icon_button(ui_t *ui, const char *icon_key, const char *label, const char *tooltip)
{
	struct nk_context *context = &ui->backend.context;
	const struct nk_image *icon = icon_for(ui, icon_key);
	struct nk_rect bounds = nk_widget_bounds(context);
	bool clicked;

	tooltip_if_hovered(context, tooltip);
	clicked = nk_button_label(context, icon != NULL ? " " : label);
	if (icon != NULL)
	{
		draw_icon(ui, bounds, icon, NULL, false);
	}
	return clicked;
}

static void section_label(ui_t *ui, const char *text)
{
	struct nk_context *context = &ui->backend.context;
	struct nk_color color = ui->colors[NK_COLOR_TEXT];

	color.a = 150;
	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
	nk_label_colored(context, text, NK_TEXT_LEFT, color);
}

// label | slider | value; returns true if the value changed
static bool slider_row(ui_t *ui, const char *label, float *value, float minimum, float maximum, float step, const char *format,
	const char *tooltip)
{
	struct nk_context *context = &ui->backend.context;
	float before = *value;
	char text[48];

	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT - 4.0f, 3, (const float[]){ 0.38f, 0.42f, 0.20f });
	tooltip_if_hovered(context, tooltip);
	nk_label(context, label, NK_TEXT_LEFT);
	tooltip_if_hovered(context, tooltip);
	nk_slider_float(context, minimum, value, maximum, step);
	SDL_snprintf(text, sizeof(text), format, (double)*value);
	nk_label(context, text, NK_TEXT_RIGHT);
	return *value != before;
}

static bool slider_row_int(ui_t *ui, const char *label, int *value, int minimum, int maximum, int step, const char *tooltip)
{
	struct nk_context *context = &ui->backend.context;
	int before = *value;
	char text[32];

	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT - 4.0f, 3, (const float[]){ 0.38f, 0.42f, 0.20f });
	tooltip_if_hovered(context, tooltip);
	nk_label(context, label, NK_TEXT_LEFT);
	tooltip_if_hovered(context, tooltip);
	nk_slider_int(context, minimum, value, maximum, step);
	SDL_snprintf(text, sizeof(text), "%d", *value);
	nk_label(context, text, NK_TEXT_RIGHT);
	return *value != before;
}

static bool checkbox_row(ui_t *ui, const char *label, bool *value, const char *tooltip)
{
	struct nk_context *context = &ui->backend.context;
	nk_bool checked = *value;

	nk_layout_row_dynamic(context, ROW_HEIGHT - 4.0f, 1);
	tooltip_if_hovered(context, tooltip);
	nk_checkbox_label(context, label, &checked);
	if ((bool)checked != *value)
	{
		*value = checked;
		return true;
	}
	return false;
}

// "homestead" -> "Homestead", "desert_camp" -> "Desert camp"
static void display_name(const char *name, char *text_out, size_t text_out_size)
{
	size_t length = 0;

	for (; *name != '\0' && length + 1 < text_out_size; name++)
	{
		char character = *name == '_' ? ' ' : *name;

		text_out[length] = length == 0 ? (char)SDL_toupper((unsigned char)character) : character;
		length++;
	}
	text_out[length] = '\0';
}

// ---------------------------------------------------------------------------
// The brush panel

static void tool_row_begin(struct nk_context *context)
{
	nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
}

// (the chosen tool's button again: no tool)
static void select_tool(tool_state_t *tools, tool_type_t tool)
{
	tools->current_tool = tools->current_tool == tool ? TOOL_NONE : tool;
	tools->stamp_dragging = false;
}

// The object tool: a category, the types in it, the drop height
static void object_options(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	const object_types_t *types = view->object_types;
	tool_state_t *tools = view->tools;
	char names[OBJECT_CATEGORIES_MAX][OBJECT_CATEGORY_NAME_LENGTH];
	const char *items[OBJECT_CATEGORIES_MAX];
	int category;
	int column = 0;

	if (types->category_count == 0)
	{
		return;
	}
	for (int i = 0; i < types->category_count; i++)
	{
		display_name(types->categories[i], names[i], sizeof(names[i]));
		items[i] = names[i];
	}
	tools->object_category = SDL_clamp(tools->object_category, 0, types->category_count - 1);
	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT - 4.0f, 2, (const float[]){ 0.38f, 0.62f });
	nk_label(context, "Category", NK_TEXT_LEFT);
	category = nk_combo(context, items, types->category_count, tools->object_category, (int)(ROW_HEIGHT - 4.0f),
		nk_vec2(nk_widget_width(context), 320.0f));
	if (category != tools->object_category || tools->object_type < 0 || tools->object_type >= types->type_count
		|| types->types[tools->object_type].category != category)
	{
		// A new category: its first type
		tools->object_category = category;
		for (int t = 0; t < types->type_count; t++)
		{
			if (types->types[t].category == category && types->types[t].model >= 0 && types->types[t].module_type < 0)
			{
				tools->object_type = t;
				break;
			}
		}
	}
	for (int t = 0; t < types->type_count; t++)
	{
		char label[OBJECT_TYPE_NAME_LENGTH];
		char tooltip[96];
		const char *physics_names[] = { "only drawn", "plant (no body)", "fixed body", "loose body" };

		if (types->types[t].category != tools->object_category || types->types[t].model < 0 || types->types[t].module_type >= 0)
		{
			continue;
		}
		if (column % 2 == 0)
		{
			nk_layout_row_dynamic(context, ROW_HEIGHT, 2);
		}
		column++;
		display_name(types->types[t].name, label, sizeof(label));
		SDL_snprintf(tooltip, sizeof(tooltip), "%s: %s", types->types[t].name, physics_names[types->types[t].physics]);
		if (tool_button(ui, "", label, tooltip, tools->object_type == t))
		{
			tools->object_type = t;
		}
	}
	slider_row(ui, "Drop from", &tools->object_drop_meters, 0.0f, 15.0f, 0.5f, "%.1f m",
		"Loose things fall from this high (0: put down, asleep until something moves them)");
}

static void brush_options(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	tool_state_t *tools = view->tools;
	settings_t *settings = view->settings;
	tool_type_t tool = tools->current_tool;
	bool uses_soil = tool == TOOL_RAISE || tool == TOOL_PAINT_SOIL || tool == TOOL_STAMP;

	if (tool == TOOL_LAYOUT)
	{
		if (view->layouts->count > 0)
		{
			char text[64];

			SDL_snprintf(text, sizeof(text), "Size: %.0f m across", (double)(2.0f * view->layouts->layouts[tools->layout].footprint_meters));
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, text, NK_TEXT_LEFT);
		}
	}
	else if (tool != TOOL_SPRING && tool != TOOL_REMOVE_LAYOUT && tool != TOOL_OBJECT && tool != TOOL_INSPECT && tool != TOOL_EFFECT
		&& tool != TOOL_LIGHT && tool != TOOL_NONE)
	{
		float radius = tools->brush_radius_meters;

		if (slider_row(ui, "Size", &radius, settings->tools.brush_minimum_radius_meters, settings->tools.brush_maximum_radius_meters, 0.5f,
			"%.1f m", "Brush radius ([ and ])"))
		{
			tools->brush_radius_meters = radius;
		}
	}

	if (uses_soil)
	{
		static const char *soil_labels[SOIL_TYPE_COUNT] = { "Sand", "Earth", "Clay", "Rock" };
		static const char *soil_icons[SOIL_TYPE_COUNT] = { "soil_sand", "soil_earth", "soil_clay", "soil_rock" };
		static const char *soil_tooltips[SOIL_TYPE_COUNT] = { "Sand (5): washes away easily", "Earth (6)", "Clay (7): holds water back",
			"Rock (8): does not erode" };

		nk_layout_row_dynamic(context, ROW_HEIGHT, SOIL_TYPE_COUNT);
		for (int soil = 0; soil < SOIL_TYPE_COUNT; soil++)
		{
			if (tool_button(ui, soil_icons[soil], soil_labels[soil], soil_tooltips[soil], tools->current_soil == (soil_type_t)soil))
			{
				tools->current_soil = (soil_type_t)soil;
			}
		}
	}

	switch (tool)
	{
		case TOOL_RAISE:
		case TOOL_DIG:
			slider_row(ui, "Speed", &settings->tools.raise_meters_per_second, 0.5f, 30.0f, 0.5f, "%.1f m/s", "How fast the ground rises or sinks");
			break;
		case TOOL_SMOOTH:
			slider_row(ui, "Strength", &settings->tools.smooth_strength_per_second, 0.5f, 10.0f, 0.1f, "%.1f", NULL);
			break;
		case TOOL_STAMP:
		{
			nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
			for (int stamp = 0; stamp < STAMP_COUNT; stamp++)
			{
				char label[32];
				char icon_key[48];

				display_name(stamps_name((stamp_type_t)stamp), label, sizeof(label));
				SDL_snprintf(icon_key, sizeof(icon_key), "stamp_%s", stamps_name((stamp_type_t)stamp));
				if (tool_button(ui, icon_key, label, stamps_is_line((stamp_type_t)stamp) ? "Drag from one end to the other" : "Click to place",
					tools->current_stamp == (stamp_type_t)stamp))
				{
					tools->current_stamp = (stamp_type_t)stamp;
					tools->stamp_dragging = false;
				}
			}
			slider_row(ui, "Height", &tools->stamp_height_meters, 0.5f, 30.0f, 0.5f, "%.1f m", "Height or depth of the stamp (- and =)");
			break;
		}
		case TOOL_WATER_ADD:
		case TOOL_WATER_REMOVE:
			slider_row(ui, "Rate", &settings->tools.water_meters_per_second, 0.1f, 10.0f, 0.1f, "%.1f m/s",
				"Water added or taken at the brush center, meters of depth per second");
			break;
		case TOOL_SPRING:
		{
			char text[64];

			SDL_snprintf(text, sizeof(text), "Click: place or remove (%d springs)", view->water->spring_count);
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, text, NK_TEXT_LEFT);
			slider_row(ui, "Flow", &settings->water.spring_cubic_meters_per_second, 0.1f, 20.0f, 0.1f, "%.1f m3/s", "Water per spring");
			break;
		}
		case TOOL_PLANT:
		{
			const object_types_t *types = view->object_types;
			int column = 0;

			for (int category = 0; category < types->category_count; category++)
			{
				char label[48];
				char icon_key[64];

				if (!object_types_category_plantable(types, category))
				{
					continue;
				}
				if (column % 3 == 0)
				{
					nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
				}
				column++;
				display_name(types->categories[category], label, sizeof(label));
				SDL_snprintf(icon_key, sizeof(icon_key), "plant_%s", types->categories[category]);
				if (tool_button(ui, icon_key, label, NULL, tools->plant_category == category))
				{
					tools->plant_category = category;
				}
			}
			break;
		}
		case TOOL_LAYOUT:
		{
			const layouts_t *layouts = view->layouts;

			for (int layout = 0; layout < layouts->count; layout++)
			{
				char label[64];
				char icon_key[80];

				if (layout % 2 == 0)
				{
					nk_layout_row_dynamic(context, ROW_HEIGHT, 2);
				}
				display_name(layouts->layouts[layout].name, label, sizeof(label));
				SDL_snprintf(icon_key, sizeof(icon_key), "layout_%s", layouts->layouts[layout].name);
				if (tool_button(ui, icon_key, label, layouts->layouts[layout].needs_water ? "Needs water in front of it" : NULL,
					tools->layout == layout))
				{
					tools->layout = layout;
				}
			}
			break;
		}
		case TOOL_REMOVE_LAYOUT:
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, "Click a village part to remove it", NK_TEXT_LEFT);
			break;
		case TOOL_OBJECT:
			object_options(ui, view);
			break;
		case TOOL_INSPECT:
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, "Click: pin the cell; click it again: let go", NK_TEXT_LEFT);
			break;
		case TOOL_EFFECT:
		{
			particles_t *particles = view->particles;
			char text[64];

			if (particles == NULL || !particles->created)
			{
				nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
				nk_label(context, "No particles (see the log)", NK_TEXT_LEFT);
				break;
			}
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, "Click: on an actor, an object or the ground", NK_TEXT_LEFT);
			for (int effect = 0; effect < particles->effect_count; effect++)
			{
				char label[64];

				if (effect % 2 == 0)
				{
					nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 2);
				}
				display_name(particles->effects[effect].name, label, sizeof(label));
				if (tool_button(ui, "effect", label, particles->effects[effect].burst ? "Plays once" : "Goes on until stopped",
					tools->effect == effect))
				{
					tools->effect = effect;
				}
			}
			SDL_snprintf(text, sizeof(text), "Playing: %d", particles_emitter_count(particles));
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, text, NK_TEXT_LEFT);
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 2);
			if (nk_button_label(context, "Stop all"))
			{
				particles_stop_all(particles);
			}
			if (nk_button_label(context, "Reload ini"))
			{
				particles_reload(particles);
				tools->effect = SDL_clamp(tools->effect, 0, SDL_max(particles->effect_count - 1, 0));
			}
			break;
		}
		case TOOL_LIGHT:
		{
			static const char *const kinds[TOOL_LIGHT_KIND_COUNT] = { "Lamp", "Fire", "Cold", "White" };
			static const char *const kind_tips[TOOL_LIGHT_KIND_COUNT] =
			{
				"Warm, steady, a glowing core; lit at dusk",
				"Orange, dancing; burns day and night",
				"Blue-white (magic, moonstone); lit at dusk",
				"Plain white; lit at dusk",
			};
			char text[64];

			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, "Click: a light; click by one: remove it", NK_TEXT_LEFT);
			for (int kind = 0; kind < TOOL_LIGHT_KIND_COUNT; kind++)
			{
				if (kind % 2 == 0)
				{
					nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 2);
				}
				if (tool_button(ui, "light_kind", kinds[kind], kind_tips[kind], tools->light_kind == kind))
				{
					tools->light_kind = kind;
				}
			}
			slider_row(ui, "Bright", &tools->light_intensity, 0.1f, 6.0f, 0.1f, "%.1f", "1: about the noon sun a meter away");
			slider_row(ui, "Reach", &tools->light_radius_meters, 1.0f, 40.0f, 0.5f, "%.1f m", "Nothing beyond this");
			slider_row(ui, "Height", &tools->light_height_meters, 0.0f, 10.0f, 0.1f, "%.1f m", "Above the ground");
			if (view->lights != NULL)
			{
				SDL_snprintf(text, sizeof(text), "Placed: %d, lighting now: %d", view->lights->placed_count, view->lights->count);
				nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
				nk_label(context, text, NK_TEXT_LEFT);
				nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
				if (nk_button_label(context, "Remove all"))
				{
					lights_clear_placed(view->lights);
				}
			}
			break;
		}
		default:
			break;
	}
}

static void brushes_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	tool_state_t *tools = view->tools;
	tool_type_t tool = tools->current_tool;
	bool hovered = nk_input_is_mouse_hovering_rect(&context->input, ui->brushes_bounds) || nk_item_is_any_active(context);
	struct nk_color background = ui->colors[NK_COLOR_WINDOW];
	struct nk_color header = ui->colors[NK_COLOR_HEADER];
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE;
	float content_bottom = 0.0f;

	if (!hovered)
	{
		background.a = (nk_byte)((float)background.a * ui->panel_opacity);
		header.a = (nk_byte)((float)header.a * SDL_min(1.0f, ui->panel_opacity + 0.2f));
	}
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	nk_style_push_style_item(context, &context->style.window.header.normal, nk_style_item_color(header));
	nk_style_push_style_item(context, &context->style.window.header.hover, nk_style_item_color(header));
	nk_style_push_style_item(context, &context->style.window.header.active, nk_style_item_color(header));

	if (nk_begin(context, BRUSHES_WINDOW, ui->brushes_bounds, flags))
	{
		section_label(ui, "TERRAIN");
		tool_row_begin(context);
		if (tool_button(ui, "raise", "Raise", "Raise the ground (1)", tool == TOOL_RAISE))
		{
			select_tool(tools, TOOL_RAISE);
		}
		if (tool_button(ui, "dig", "Dig", "Dig the ground away (2)", tool == TOOL_DIG))
		{
			select_tool(tools, TOOL_DIG);
		}
		if (tool_button(ui, "paint", "Paint", "Paint the soil type (3)", tool == TOOL_PAINT_SOIL))
		{
			select_tool(tools, TOOL_PAINT_SOIL);
		}
		tool_row_begin(context);
		if (tool_button(ui, "smooth", "Smooth", "Smooth the ground (4)", tool == TOOL_SMOOTH))
		{
			select_tool(tools, TOOL_SMOOTH);
		}
		if (tool_button(ui, "stamp", "Stamp", "Craters, dams, hills, channels, lakes, plateaus (T)", tool == TOOL_STAMP))
		{
			select_tool(tools, TOOL_STAMP);
		}
		nk_spacer(context);

		section_label(ui, "WATER");
		tool_row_begin(context);
		if (tool_button(ui, "water_add", "Water", "Pour water (or shift + click with any tool)", tool == TOOL_WATER_ADD))
		{
			select_tool(tools, TOOL_WATER_ADD);
		}
		if (tool_button(ui, "sponge", "Sponge", "Take water away (or ctrl + click with any tool)", tool == TOOL_WATER_REMOVE))
		{
			select_tool(tools, TOOL_WATER_REMOVE);
		}
		if (tool_button(ui, "spring", "Spring", "Place or remove a spring (F)", tool == TOOL_SPRING))
		{
			select_tool(tools, TOOL_SPRING);
		}
		nk_layout_row_dynamic(context, ROW_HEIGHT - 4.0f, 2);
		{
			nk_bool raining = view->water->raining;
			nk_bool draining = view->water->drain_edges;

			tooltip_if_hovered(context, "Rain over the whole map (R)");
			if (nk_checkbox_label(context, "Rain", &raining))
			{
				view->input->toggle_rain = true;
			}
			tooltip_if_hovered(context, "Water leaves the map at its borders; off: the borders are walls (B)");
			if (nk_checkbox_label(context, "Drain at edges", &draining))
			{
				view->input->toggle_borders = true;
			}
		}

		section_label(ui, "NATURE");
		tool_row_begin(context);
		if (tool_button(ui, "plant", "Plant", "Plant trees, bushes, grass... (G)", tool == TOOL_PLANT))
		{
			select_tool(tools, TOOL_PLANT);
			if (!object_types_category_plantable(view->object_types, tools->plant_category))
			{
				for (int category = 0; category < view->object_types->category_count; category++)
				{
					if (object_types_category_plantable(view->object_types, category))
					{
						tools->plant_category = category;
						break;
					}
				}
			}
		}
		if (tool_button(ui, "clear", "Clear", "Remove the objects under the brush (H)", tool == TOOL_CLEAR))
		{
			select_tool(tools, TOOL_CLEAR);
		}
		if (tool_button(ui, "object", "Object", "Place one object of a chosen type (O: one of the plant category)", tool == TOOL_OBJECT))
		{
			select_tool(tools, TOOL_OBJECT);
		}

		section_label(ui, "VILLAGES");
		tool_row_begin(context);
		if (tool_button(ui, "layout", "Place", "Place homesteads, camps, mills... (L)", tool == TOOL_LAYOUT))
		{
			select_tool(tools, TOOL_LAYOUT);
		}
		if (tool_button(ui, "remove_layout", "Remove", "Remove a whole homestead, camp... (Delete)", tool == TOOL_REMOVE_LAYOUT))
		{
			select_tool(tools, TOOL_REMOVE_LAYOUT);
		}
		nk_spacer(context);

		section_label(ui, "DEBUG");
		tool_row_begin(context);
		if (tool_button(ui, "inspect", "Inspect", "Everything about the water and ground of one cell", tool == TOOL_INSPECT))
		{
			select_tool(tools, TOOL_INSPECT);
		}
		if (tool_button(ui, "effects", "Effects", "Particle effects: fire, smoke, magic (data/particles.ini)", tool == TOOL_EFFECT))
		{
			select_tool(tools, TOOL_EFFECT);
		}
		if (tool_button(ui, "light", "Light", "Place a light (a click by one removes it); kept with the map", tool == TOOL_LIGHT))
		{
			select_tool(tools, TOOL_LIGHT);
		}

		section_label(ui, "BRUSH");
		brush_options(ui, view);

		// Where the content ends: the window is fitted to it
		nk_layout_row_dynamic(context, 1.0f, 1);
		content_bottom = nk_widget_bounds(context).y;
		ui->brushes_bounds = nk_window_get_bounds(context);
	}
	ui->brushes_collapsed = nk_window_is_collapsed(context, BRUSHES_WINDOW);
	nk_end(context);
	nk_style_pop_style_item(context);
	nk_style_pop_style_item(context);
	nk_style_pop_style_item(context);
	nk_style_pop_style_item(context);

	if (!ui->brushes_placed)
	{
		// The first frame: the stored state
		ui->brushes_placed = true;
		if (ui->brushes_collapsed)
		{
			nk_window_collapse(context, BRUSHES_WINDOW, NK_MINIMIZED);
		}
	}
	if (content_bottom > 0.0f && !ui->brushes_collapsed)
	{
		struct nk_rect bounds = ui->brushes_bounds;
		float wanted = content_bottom - bounds.y + context->style.window.padding.y + 2.0f * context->style.window.spacing.y + 6.0f;
		float room = ui->backend.height - bounds.y - 8.0f;
		float height = SDL_max(SDL_min(wanted, room), 80.0f);

		// Kept on screen after the window got smaller
		bounds.x = SDL_clamp(bounds.x, 0.0f, SDL_max(ui->backend.width - 60.0f, 0.0f));
		bounds.y = SDL_clamp(bounds.y, 0.0f, SDL_max(ui->backend.height - 40.0f, 0.0f));
		if (SDL_fabsf(height - bounds.h) > 0.5f || bounds.x != ui->brushes_bounds.x || bounds.y != ui->brushes_bounds.y)
		{
			bounds.h = height;
			nk_window_set_bounds(context, BRUSHES_WINDOW, bounds);
			ui->brushes_bounds = bounds;
		}
	}
}

// ---------------------------------------------------------------------------
// The icon bar and the status line

typedef struct
{
	const char *icon;
	const char *label;
	const char *tooltip;
} icon_bar_item_t;

static void icon_bar_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	char speed_text[32];
	icon_bar_item_t items[] = {
		{ "settings", "Settings", "Settings" },
		{ "save", "Save", "Save the map (F5: quick save)" },
		{ "load", "Load", "Load a map (F9: quick load)" },
		{ "new_map", "New", "A new map (N: with a random seed at once)" },
		{ view->paused ? "play" : "pause", view->paused ? "Run" : "Pause", "Pause / run the simulation (space)" },
		{ "slower", "\xc2\xab", "Slower (,)" },
		{ NULL, speed_text, "Simulation speed" },
		{ "faster", "\xc2\xbb", "Faster (.)" },
		{ "sky", "Sky", "Paint the clouds on their map" },
		{ "world", "World", "What is shown of the world (objects...)" },
		{ "diagnostics", "Diagnostics", "What the frame and the simulation cost" },
		{ "status", "Status", "Show or hide the status line" },
	};
	int item_count = (int)(sizeof(items) / sizeof(items[0]));
	float widths[16];
	float total = 0.0f;
	float row_height = ROW_HEIGHT;
	struct nk_vec2 padding = context->style.window.padding;
	float spacing = context->style.window.spacing.x;

	if (view->paused)
	{
		SDL_strlcpy(speed_text, "paused", sizeof(speed_text));
	}
	else if (view->reached_time_scale < 0.95f * view->time_scale)
	{
		SDL_snprintf(speed_text, sizeof(speed_text), "x%g (%.1f)", (double)view->time_scale, (double)view->reached_time_scale);
	}
	else
	{
		SDL_snprintf(speed_text, sizeof(speed_text), "x%g", (double)view->time_scale);
	}
	for (int i = 0; i < item_count; i++)
	{
		const struct nk_image *icon = items[i].icon != NULL ? icon_for(ui, items[i].icon) : NULL;

		widths[i] = icon != NULL ? row_height : text_width(ui, items[i].label) + 2.0f * context->style.button.padding.x + 8.0f;
		if (items[i].icon == NULL)
		{
			// The speed label: room for its longest text
			widths[i] = text_width(ui, "x16 (15.9)") + 6.0f;
		}
		total += widths[i] + spacing;
	}
	total += 2.0f * padding.x;
	{
		struct nk_rect bounds = nk_rect(ui->backend.width - total - 8.0f, 8.0f, total, row_height + 2.0f * padding.y + 2.0f);

		if (nk_begin(context, ICON_BAR_WINDOW, bounds, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER))
		{
			nk_window_set_bounds(context, ICON_BAR_WINDOW, bounds);
			nk_layout_row_begin(context, NK_STATIC, row_height, item_count);
			for (int i = 0; i < item_count; i++)
			{
				nk_layout_row_push(context, widths[i]);
				if (items[i].icon == NULL)
				{
					tooltip_if_hovered(context, items[i].tooltip);
					nk_label(context, items[i].label, NK_TEXT_CENTERED);
					continue;
				}
				if (!icon_button(ui, items[i].icon, items[i].label, items[i].tooltip))
				{
					continue;
				}
				switch (i)
				{
					case 0:
						ui->settings_open = !ui->settings_open;
						break;
					case 1:
					case 2:
					case 3:
					{
						ui_maps_mode_t mode = i == 1 ? UI_MAPS_SAVE : (i == 2 ? UI_MAPS_LOAD : UI_MAPS_NEW);

						if (ui->maps_mode == mode)
						{
							ui->maps_mode = UI_MAPS_CLOSED;
							break;
						}
						ui->maps_mode = mode;
						ui->map_entry_count = map_file_list(ui->map_entries, MAP_FILE_LIST_MAX);
						ui->selected_map = -1;
						ui->confirm_delete = false;
						ui->confirm_overwrite = false;
						if (mode == UI_MAPS_SAVE)
						{
							// (the name without its kind; the kind it was, or must be)
							ui->save_baked = view->map_baked || map_name_is_baked(ui->last_map_name);
							if (ui->last_map_name[0] != '\0')
							{
								map_name_without_kind(ui->last_map_name, ui->map_name_input, sizeof(ui->map_name_input));
							}
							else
							{
								SDL_snprintf(ui->map_name_input, sizeof(ui->map_name_input), "map %u", view->seed);
							}
						}
						break;
					}
					case 4:
						view->input->toggle_pause = true;
						break;
					case 5:
						view->input->time_scale_steps--;
						break;
					case 7:
						view->input->time_scale_steps++;
						break;
					case 8:
						ui->sky_open = !ui->sky_open;
						break;
					case 9:
						ui->world_open = !ui->world_open;
						break;
					case 10:
						ui->diagnostics_open = !ui->diagnostics_open;
						break;
					case 11:
						ui->show_status_line = !ui->show_status_line;
						break;
					default:
						break;
				}
			}
			nk_layout_row_end(context);
		}
		nk_end(context);
	}
}

static void status_window(ui_t *ui)
{
	struct nk_context *context = &ui->backend.context;
	float height = SMALL_ROW_HEIGHT + 8.0f;
	struct nk_color background = ui->colors[NK_COLOR_WINDOW];

	if (!ui->show_status_line)
	{
		return;
	}
	background.a = (nk_byte)((float)background.a * 0.7f);
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	nk_style_push_vec2(context, &context->style.window.padding, nk_vec2(8.0f, 4.0f));
	if (nk_begin(context, STATUS_WINDOW, nk_rect(0.0f, ui->backend.height - height, ui->backend.width, height),
		NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND))
	{
		nk_window_set_bounds(context, STATUS_WINDOW, nk_rect(0.0f, ui->backend.height - height, ui->backend.width, height));
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		nk_label(context, ui->status_text, NK_TEXT_LEFT);
	}
	nk_end(context);
	nk_style_pop_vec2(context);
	nk_style_pop_style_item(context);
}

// Alt held: the list of what the ray under the pointer goes through,
// in the bottom left corner
static void pick_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	float width = 0.0f;
	float height = (float)(view->pick_line_count + 1) * (SMALL_ROW_HEIGHT + 4.0f) + 16.0f;
	float bottom = ui->show_status_line ? SMALL_ROW_HEIGHT + 8.0f : 0.0f;
	struct nk_rect bounds;

	if (view->pick_line_count == 0)
	{
		nk_window_show(context, PICK_WINDOW, NK_HIDDEN);
		return;
	}
	for (int i = 0; i < view->pick_line_count; i++)
	{
		width = SDL_max(width, text_width(ui, view->pick_lines[i]));
	}
	width += 32.0f;
	bounds = nk_rect(12.0f, ui->backend.height - bottom - height - 12.0f, width, height);
	nk_window_show(context, PICK_WINDOW, NK_SHOWN);
	if (nk_begin(context, PICK_WINDOW, bounds, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BORDER))
	{
		nk_window_set_bounds(context, PICK_WINDOW, bounds);
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		nk_label(context, "Under the pointer (nearest first):", NK_TEXT_LEFT);
		for (int i = 0; i < view->pick_line_count; i++)
		{
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, view->pick_lines[i], NK_TEXT_LEFT);
		}
	}
	nk_end(context);
}

static void message_window(ui_t *ui, double time_seconds)
{
	struct nk_context *context = &ui->backend.context;
	float width;
	float height = ROW_HEIGHT + 12.0f;
	float bottom = ui->show_status_line ? SMALL_ROW_HEIGHT + 8.0f : 0.0f;

	if (ui->message[0] == '\0' || time_seconds > ui->message_until_seconds)
	{
		nk_window_show(context, MESSAGE_WINDOW, NK_HIDDEN);
		return;
	}
	width = text_width(ui, ui->message) + 32.0f;
	nk_window_show(context, MESSAGE_WINDOW, NK_SHOWN);
	if (nk_begin(context, MESSAGE_WINDOW, nk_rect(0.5f * (ui->backend.width - width), ui->backend.height - bottom - height - 12.0f, width, height),
		NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BORDER))
	{
		nk_window_set_bounds(context, MESSAGE_WINDOW,
			nk_rect(0.5f * (ui->backend.width - width), ui->backend.height - bottom - height - 12.0f, width, height));
		nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
		nk_label(context, ui->message, NK_TEXT_CENTERED);
	}
	nk_end(context);
}

// ---------------------------------------------------------------------------
// Settings

typedef enum
{
	TAB_SKY = 0,
	TAB_CLOUDS,
	TAB_GROUND,
	TAB_WATER,
	TAB_OBJECTS,
	TAB_RENDER,
	TAB_TERRAIN,
	TAB_CAMERA,
	TAB_INTERFACE,
	TAB_DEBUG,
	TAB_COUNT
} settings_tab_t;

static const char *tab_names[TAB_COUNT] = { "Sky & light", "Clouds", "Ground", "Water", "Objects", "Graphics", "New map", "Camera", "Interface",
	"Debug" };

typedef enum
{
	EFFECT_LIVE = 0,               // used as it changes
	EFFECT_RENDER,                 // rebuilds textures or shadow maps: Apply
	EFFECT_OBJECTS,                // grows the vegetation anew: Apply
	EFFECT_GENERATE                // used by the next new map
} setting_effect_t;

typedef struct
{
	settings_tab_t tab;
	const char *section;           // NULL: a heading
	const char *key;
	const char *label;
	float minimum;
	float maximum;
	float step;
	setting_effect_t effect;
	const char *format;            // for floats; NULL: "%.2f"
	const char *choices;           // integers chosen from a list: "512|1024|2048", or named: "0:Off|1:On"
	const char *tooltip;
} ui_setting_t;

#define SETTINGS_TABS_PER_ROW 5

#define HEADING(tab, text) { tab, NULL, NULL, text, 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL }

static const ui_setting_t ui_settings[] = {
	HEADING(TAB_SKY, "Sun and sky"),
	{ TAB_SKY, "render", "sun_azimuth_degrees", "Sun direction", 0, 360, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, "0 = from +X, 90 = from +Z (with the day cycle: at noon)" },
	{ TAB_SKY, "render", "sun_elevation_degrees", "Sun height", 5, 85, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL },
	{ TAB_SKY, "render", "sun_intensity", "Sunlight", 0, 3, 0.05f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_SKY, "render", "sun_color", "Sun color", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_SKY, "render", "ambient_intensity", "Sky light", 0, 1.5f, 0.01f, EFFECT_LIVE, NULL, NULL, "Light from the sky in the shadows" },
	{ TAB_SKY, "render", "sky_color", "Sky color", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "At the horizon; distant land fades into it" },
	{ TAB_SKY, "render", "sky_zenith_color", "Sky overhead", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "The sky straight up" },
	{ TAB_SKY, "render", "fog_density", "Haze", 0, 0.003f, 0.00005f, EFFECT_LIVE, "%.5f", NULL, "Fog density per meter, at the ground" },
	{ TAB_SKY, "render", "fog_height_meters", "Haze height", 100, 5000, 50, EFFECT_LIVE, "%.0f m", NULL,
		"The haze thins upwards: this much higher it is a third as thick (low: clear mountain tops and clouds)" },
	HEADING(TAB_SKY, "Day and night"),
	{ TAB_SKY, "render", "day_cycle", "Day cycle", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The sun crosses the sky, evening, night with the moon and the stars (off: the sun stays where it is set). The sun settings are noon's" },
	{ TAB_SKY, "render", "daylight_hours", "Daylight", 2, 21, 0.5f, EFFECT_LIVE, "%.1f h", NULL,
		"Hours of the 24 with the sun up, about noon (14: a summer's day); an hour of dawn, one of dusk, the rest night" },
	{ TAB_SKY, "render", "day_length_minutes", "Day length", 0, 720, 5, EFFECT_LIVE, "%.0f min", NULL,
		"A whole day in real minutes (0: the hour stays; the console's hour sets it, freeze stops it)" },
	{ TAB_SKY, "render", "golden_hour", "Golden hour", 0, 2, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The low sun's deep gold (the last and first degrees above the horizon), the shadows cooler against it" },
	{ TAB_SKY, "render", "blue_hour_on", "Blue hour", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Dusk and dawn with a blue hour: all the sky deep blue, its light stronger (off: the dusky twilight with its warm glow)" },
	{ TAB_SKY, "render", "blue_hour", "Blue hour strength", 0, 2, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The deep blue light while the sun is a few degrees under the horizon, before the night" },
	{ TAB_SKY, "render", "moon_cycle_days", "Moon cycle", 2, 60, 0.5f, EFFECT_LIVE, "%.1f days", NULL,
		"From new moon to new moon, in days of the game (29.5: as the real one; the console's moon sets its age)" },
	{ TAB_SKY, "render", "moon_size_degrees", "Moon size", 0, 4.5f, 0.05f, EFFECT_LIVE, "%.2f\xc2\xb0", NULL,
		"How wide the moon is on the sky (0: none; the real one is half a degree, 1.5 the default)" },
	{ TAB_SKY, "render", "aurora_strength", "Aurora", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright the northern lights are when there are some (World window, the console's aurora)" },
	HEADING(TAB_SKY, "Rain"),
	{ TAB_SKY, "render", "rain_intensity", "Rain", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How hard it rains when it rains (World window, R, the console's rain): the drops, the dimmer light, the rings on the water" },
	{ TAB_SKY, "render", "rain_streak_meters", "Drop streaks", 0.05f, 1.5f, 0.05f, EFFECT_LIVE, "%.2f m", NULL,
		"How long a falling drop looks" },
	{ TAB_SKY, "render", "rain_splashes", "Splashes", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How many splashes where the drops land (0: none)" },
	{ TAB_SKY, "render", "rainbow_strength", "Rainbow", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright a rainbow is (World window, the console's rainbow); it shows only with the sun low, opposite it" },
	HEADING(TAB_SKY, "Valley fog"),
	{ TAB_SKY, "render", "fog_mode", "Valley fog", 0, 0, 0, EFFECT_LIVE, NULL, "0:Off|1:Simple|2:Volumetric",
		"Low fog in the valleys and over the water, forming in the evening, lifting in the morning. Volumetric: lit through "
		"the shadow maps (shafts between the trees); simple: cheaper, no shadows" },
	{ TAB_SKY, "render", "fog_quality", "Fog detail", 0, 0, 0, EFFECT_LIVE, NULL, "0:Low|1:Medium|2:High",
		"The volumetric fog's cells (froxels): 96 x 54 x 64, 160 x 90 x 96, 240 x 135 x 128" },
	{ TAB_SKY, "render", "fog_valley_density", "Fog thickness", 0, 0.5f, 0.005f, EFFECT_LIVE, "%.3f", NULL,
		"How thick a full valley's fog is (per meter)" },
	{ TAB_SKY, "render", "fog_valley_height_meters", "Fog height", 2, 100, 1, EFFECT_LIVE, "%.0f m", NULL,
		"How high it rises in the deepest valleys (and two thirds of that over water)" },
	{ TAB_SKY, "render", "fog_valley_reach_meters", "Valley width", 20, 1000, 10, EFFECT_LIVE, "%.0f m", NULL,
		"How far around a place is looked at to tell whether it lies in a valley" },
	{ TAB_SKY, "render", "fog_softness_meters", "Fog top", 0.5f, 30, 0.5f, EFFECT_LIVE, "%.1f m", NULL, "How soft its top is" },
	{ TAB_SKY, "render", "fog_glow", "Fog glow", 0, 0.9f, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How much brighter it is looking towards the sun (0: alike every way)" },
	{ TAB_SKY, "render", "fog_shafts", "Fog shafts", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How much the shadows darken the fog: the sun's shafts through the trees (0: none)" },
	{ TAB_SKY, "render", "fog_night_brightness", "Fog at night", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright the fog is at night, of its full light (lit evenly all round, it outshines the dark ground)" },
	{ TAB_SKY, "render", "fog_rays", "Fog sun rays", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The soft rays round the sun, through the fog, between what stands in front of it (0: none)" },
	{ TAB_SKY, "render", "fog_form_hour", "Fog forms", 0, 24, 0.25f, EFFECT_LIVE, "%.2f h", NULL,
		"The hour it begins to form (thick four hours later)" },
	{ TAB_SKY, "render", "fog_clear_hour", "Fog lifted", 0, 24, 0.25f, EFFECT_LIVE, "%.2f h", NULL,
		"The hour it has gone (lifting over the two hours before)" },
	{ TAB_SKY, "render", "fog_distance_meters", "Fog distance", 50, 2000, 10, EFFECT_LIVE, "%.0f m", NULL, "How far out it is drawn" },
	{ TAB_SKY, "render", "fog_always", "Fog always", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Whatever the hour (to look at it)" },
	HEADING(TAB_SKY, "Light (HDR)"),
	{ TAB_SKY, "render", "hdr", "HDR", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Exposure, bloom and tone mapping: light brighter than white keeps its shape (off: cut at white, as before)" },
	{ TAB_SKY, "render", "tone_mapper", "Tone mapping", 0, 0, 0, EFFECT_LIVE, NULL, "0:AgX|1:AgX punchy|2:ACES|3:Reinhard",
		"AgX: bright colors go towards white (punchy: more contrast and color); ACES: saturated, filmic; Reinhard: the plainest" },
	{ TAB_SKY, "render", "auto_exposure", "Auto exposure", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The exposure follows the picture, as the eye adapts (off: Exposure alone)" },
	{ TAB_SKY, "render", "exposure_ev", "Exposure", -4, 4, 0.1f, EFFECT_LIVE, "%+.1f EV", NULL,
		"Stops brighter (+) or darker (-)" },
	{ TAB_SKY, "render", "exposure_adapt_seconds", "Adaptation", 0, 10, 0.1f, EFFECT_LIVE, "%.1f s", NULL,
		"How long the eye takes to get used to brighter or darker (0: at once)" },
	{ TAB_SKY, "render", "bloom", "Bloom", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Bright light glows" },
	{ TAB_SKY, "render", "bloom_strength", "Bloom strength", 0, 0.3f, 0.005f, EFFECT_LIVE, "%.3f", NULL, NULL },
	{ TAB_SKY, "render", "night_vision", "Night vision", 0, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"Where the picture is dim, colors fade and turn blue, as the eye sees in the dark (0: off)" },
	{ TAB_SKY, "render", "night_vision_luminance", "Night vision under", 0.002f, 0.5f, 0.002f, EFFECT_LIVE, "%.3f", NULL,
		"How dim (the scene's light) before colors start to fade; all gone at a tenth of it" },
	HEADING(TAB_SKY, "Local lights"),
	{ TAB_SKY, "render", "local_lights", "Fires and torches", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Fires, torches and candles light what is around them (particles.ini and objects.ini: light)" },
	{ TAB_SKY, "render", "local_light_intensity", "Brightness", 0, 4, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"All the local lights, times this" },
	{ TAB_SKY, "render", "lamps_with_the_day", "Lamps with the day", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Lanterns and candles are lit at dusk and put out at dawn, each at its own moment (off: the console's lights on/off)" },
	HEADING(TAB_CLOUDS, "Clouds"),
	{ TAB_CLOUDS, "render", "cloud_type", "Type", 0, 0, 0, EFFECT_LIVE, NULL, "1:Small|2:Medium|3:Storm|4:Overcast|0:Custom",
		"Sets the values below; changing any of them makes the clouds custom" },
	{ TAB_CLOUDS, "render", "cloud_coverage", "Coverage", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "0 clear sky, 1 overcast" },
	{ TAB_CLOUDS, "render", "cloud_tiling_meters", "Spread", 300, 8000, 50, EFFECT_LIVE, "%.0f m", NULL, "How broad the clouds are and how far apart" },
	{ TAB_CLOUDS, "render", "cloud_height_meters", "Height", CLOUD_MINIMUM_HEIGHT_METERS, 3000, 10, EFFECT_LIVE, "%.0f m", NULL, "Bottom of the clouds" },
	{ TAB_CLOUDS, "render", "cloud_thickness_meters", "Thickness", 50, 2000, 10, EFFECT_LIVE, "%.0f m", NULL, "Volumetric clouds only" },
	{ TAB_CLOUDS, "render", "cloud_density", "Density", 0.1f, 5, 0.05f, EFFECT_LIVE, NULL, NULL, "Volumetric clouds only" },
	{ TAB_CLOUDS, "render", "cloud_size", "Size", 0.5f, 4, 0.05f, EFFECT_LIVE, NULL, NULL, "Volumetric clouds: size of the billows" },
	{ TAB_CLOUDS, "render", "cloud_towers", "Towers", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "Volumetric clouds: how tall the thick parts grow" },
	{ TAB_CLOUDS, "render", "cloud_base_darkness", "Dark bases", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, "Volumetric clouds: how dark the bottoms of thick clouds get" },
	{ TAB_CLOUDS, "render", "cloud_softness", "Softness", 0.01f, 0.5f, 0.01f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_CLOUDS, "render", "cloud_shadow_strength", "Shadows", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL },
	HEADING(TAB_CLOUDS, "Life and wind"),
	{ TAB_CLOUDS, "render", "cloud_density_variation", "Variety", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"Volumetric clouds: how much they differ, white and puffy to heavy and dark" },
	{ TAB_CLOUDS, "render", "cloud_lifetime_minutes", "Lifetime", 0, 60, 0.5f, EFFECT_LIVE, "%.1f min", NULL,
		"Volumetric clouds: each forms, lives and dissolves over about this long (0: they stay)" },
	{ TAB_CLOUDS, "render", "wind_direction_degrees", "Wind direction", 0, 360, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL },
	{ TAB_CLOUDS, "render", "wind_speed_meters_per_second", "Wind speed", 0, 30, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL, NULL },
	{ TAB_CLOUDS, "render", "cloud_horizon_drop_meters", "Horizon drop", 0, CLOUD_MAXIMUM_HORIZON_DROP_METERS, 50, EFFECT_LIVE, "%.0f m", NULL,
		"The planet the clouds go round, let down under you: clouds a few degrees under the horizon too (0: they stop at its line)" },
	HEADING(TAB_CLOUDS, "Quality"),
	{ TAB_CLOUDS, "render", "volumetric_clouds", "Volumetric", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Real 3D clouds (off: a flat sheet, for weak devices)" },
	{ TAB_CLOUDS, "render", "cloud_steps", "Quality", CLOUD_MINIMUM_STEPS, 192, 8, EFFECT_LIVE, "%.0f steps", NULL, "Volumetric clouds: steps along each ray (cost)" },
	{ TAB_CLOUDS, "render", "cloud_dome_size", "Sharpness", 0, 0, 0, EFFECT_LIVE, NULL, "0:Auto|1024:1024|1536:1536|2048:2048|3072:3072",
		"Volumetric clouds: texels a side of the sky they are drawn into (memory and cost; Auto: from the screen)" },
	{ TAB_CLOUDS, "render", "cloud_refresh_seconds", "Refresh", 0.02f, 1, 0.01f, EFFECT_LIVE, "%.2f s", NULL,
		"Volumetric clouds: the sky is drawn all over again in this long (longer: cheaper, moving clouds step)" },
	{ TAB_CLOUDS, "render", "cloud_memory_seconds", "Smoothing", 0.02f, 2, 0.01f, EFFECT_LIVE, "%.2f s", NULL,
		"Volumetric clouds: how long the old sky lingers (longer: smoother, trails behind moving clouds)" },
	HEADING(TAB_CLOUDS, "Flocks: small clouds high up (painted in Sky)"),
	{ TAB_CLOUDS, "render", "flock_size_meters", "Size", 100, 1500, 10, EFFECT_LIVE, "%.0f m", NULL,
		"How big each one is: grains (100 m) to big heaps (1500 m)" },
	{ TAB_CLOUDS, "render", "flock_amount", "Amount", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"How many: a few strays to packed in rows" },
	{ TAB_CLOUDS, "render", "flock_trend", "Trend", -1, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"All of them at once: dissolving (-1), steady (0), growing (1); at 1, from none to all in five minutes of the sky's clock" },
	{ TAB_CLOUDS, "render", "flock_height_meters", "Height", 2000, 10000, 50, EFFECT_LIVE, "%.0f m", NULL,
		"The bottom of their layer (altocumulus 2-6 km, cirrocumulus higher)" },
	{ TAB_CLOUDS, "render", "flock_density", "Density", 0.05f, 2, 0.01f, EFFECT_LIVE, NULL, NULL,
		"How opaque they are: a veil (low) to solid little heaps" },
	{ TAB_CLOUDS, "render", "flock_shade", "Shade", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL,
		"How gray their middles get in their own shadow (0: all white)" },
	HEADING(TAB_GROUND, "Look"),
	{ TAB_GROUND, "render", "normal_strength", "Relief", 0, 5, 0.1f, EFFECT_RENDER, NULL, NULL, "Depth of the texture relief" },
	{ TAB_GROUND, "render", "snow_height_fraction", "Snow line", 0, 1.2f, 0.01f, EFFECT_LIVE, NULL, NULL, "Fraction of the highest point" },
	{ TAB_GROUND, "render", "variation_strength", "Variation", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_GROUND, "render", "variation_hue_strength", "Hue variation", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_GROUND, "render", "detail_fade_start_meters", "Detail from", 10, 1000, 5, EFFECT_LIVE, "%.0f m", NULL, NULL },
	{ TAB_GROUND, "render", "detail_fade_end_meters", "Detail to", 20, 2000, 10, EFFECT_LIVE, "%.0f m", NULL, NULL },
	HEADING(TAB_GROUND, "Textures"),
	{ TAB_GROUND, "render", "texture_size", "Texture size", 0, 0, 0, EFFECT_RENDER, NULL, "256|512|1024|2048", NULL },
	{ TAB_GROUND, "render", "anisotropy", "Anisotropy", 1, 16, 1, EFFECT_RENDER, "%.0f", NULL, "Sharpness of textures seen at a slant" },
	HEADING(TAB_GROUND, "Material map"),
	{ TAB_GROUND, "render", "material_map", "Material map", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The ground shows the material map: the materials worked out once, the covers under the trees, the snow and the petals (off: worked out per pixel, as before)" },
	{ TAB_GROUND, "render", "ground_overlay", "Snow and petals", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Snow settles where it snows and melts after the winter, petals fall under blooming trees (with the material map; off: only the snow line's)" },
	HEADING(TAB_WATER, "Look"),
	{ TAB_WATER, "render", "water_shallow_color", "Shallow color", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_WATER, "render", "water_deep_color", "Deep color", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_WATER, "render", "water_clarity_meters", "Clarity", 0.1f, 20, 0.1f, EFFECT_LIVE, "%.1f m", NULL, NULL },
	{ TAB_WATER, "render", "water_opacity_per_meter", "Opacity", 0, 5, 0.05f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_WATER, "render", "water_reflections", "Reflections", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The water mirrors what is on screen (the land, the trees) and the volumetric clouds (off: the sky alone)" },
	{ TAB_WATER, "render", "water_reflections_half_size", "Reflections at half size", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The reflections found for a quarter of the pixels and spread over the rest (faster, a little softer; off: every pixel)" },
	{ TAB_WATER, "render", "terrain_from_gpu", "Terrain from the GPU", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The ground's heights, normals and surface made on the GPU from the simulation (off: by the CPU from its copy, slower; to compare)" },
	{ TAB_WATER, "render", "underwater_visibility_meters", "Underwater sight", 5, 300, 1, EFFECT_LIVE, "%.0f m", NULL,
		"How far you see with the camera under clear water (mud cuts it down)" },
	{ TAB_WATER, "render", "underwater_brightness", "Underwater light", 0.2f, 5, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How bright the water around the camera is" },
	{ TAB_WATER, "render", "foam_speed_meters_per_second", "Foam from", 0, 10, 0.1f, EFFECT_LIVE, "%.1f m/s", NULL, "Flow speed where foam starts" },
	HEADING(TAB_WATER, "Water on things"),
	{ TAB_WATER, "render", "water_ice", "Ice", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The water freezes through the winter, from the shore out (not where it runs fast), and holds still under the ice; a flood breaks it" },
	{ TAB_WATER, "render", "water_marks", "Flood marks", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"What the water leaves on things: the mud up to where it stood, the drying, the algae (off: only the water there now)" },
	{ TAB_WATER, "render", "moss_shells", "Moss shells", 0, 16, 1, EFFECT_LIVE, NULL, NULL,
		"The moss under water on the rocks stands up in this many layers near the camera (0: painted on alone)" },
	{ TAB_WATER, "render", "moss_shell_meters", "Moss height", 0.005f, 0.05f, 0.001f, EFFECT_LIVE, "%.3f m", NULL, NULL },
	{ TAB_WATER, "render", "moss_shell_distance_meters", "Moss shell distance", 0, 40, 1, EFFECT_LIVE, "%.0f m", NULL,
		"How far from the camera the moss's layers are drawn" },
	HEADING(TAB_WATER, "Sources"),
	{ TAB_WATER, "water", "rain_millimeters_per_second", "Rain", 0, 5, 0.05f, EFFECT_LIVE, "%.2f mm/s", NULL, NULL },
	{ TAB_WATER, "water", "spring_cubic_meters_per_second", "Spring flow", 0, 20, 0.1f, EFFECT_LIVE, "%.1f m3/s", NULL, NULL },
	{ TAB_WATER, "tools", "water_meters_per_second", "Brush rate", 0.1f, 10, 0.1f, EFFECT_LIVE, "%.1f m/s", NULL, NULL },
	HEADING(TAB_WATER, "Flow"),
	{ TAB_WATER, "water", "friction", "Bed friction", 0, 0.2f, 0.001f, EFFECT_LIVE, "%.3f", NULL, "Slows shallow water most" },
	{ TAB_WATER, "water", "manning_roughness", "Roughness", 0, 0.1f, 0.001f, EFFECT_LIVE, "%.3f", NULL, "Manning: sets the top speed of fast water" },
	{ TAB_WATER, "water", "linear_damping", "Damping", 0, 0.5f, 0.005f, EFFECT_LIVE, "%.3f", NULL, NULL },
	{ TAB_WATER, "water", "maximum_substeps", "Most substeps", 1, 32, 1, EFFECT_LIVE, NULL, NULL, "For fast water; more is steadier and slower" },
	{ TAB_WATER, "water", "substep_speed_meters_per_second", "Substeps up to", 2, 40, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL,
		"Faster water adds no more substeps; it is held back instead (cheaper, waterfalls a little slower)" },
	HEADING(TAB_WATER, "Thin water"),
	{ TAB_WATER, "water", "film_millimeters", "Wet film", 0, 5, 0.05f, EFFECT_LIVE, "%.2f mm", NULL, "Thinner water clings to the ground" },
	{ TAB_WATER, "water", "film_evaporation_millimeters_per_second", "Drying", 0, 0.5f, 0.005f, EFFECT_LIVE, "%.3f mm/s", NULL, NULL },
	{ TAB_WATER, "water", "meniscus_millimeters", "Surface tension", 0, 10, 0.1f, EFFECT_LIVE, "%.1f mm", NULL, NULL },
	{ TAB_WATER, "water", "lip_millimeters", "Lip", 0, 100, 1, EFFECT_LIVE, "%.0f mm", NULL,
		"Deep water spills into thin water only this much above it; stops ponds draining through trickles" },
	{ TAB_WATER, "water", "thin_water_millimeters", "Thin water", 0, 100, 1, EFFECT_LIVE, "%.0f mm", NULL, "Up to this deep it drains fast; 0: off" },
	{ TAB_WATER, "water", "thin_water_superfluid", "Superfluid", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Thin water flows without the shallow friction" },
	{ TAB_WATER, "water", "still_water_speed_meters_per_second", "Mud settles below", 0, 0.5f, 0.01f, EFFECT_LIVE, "%.2f m/s", NULL, NULL },
	{ TAB_WATER, "water", "thin_water_evaporation_millimeters_per_second", "Thin drying", 0, 5, 0.05f, EFFECT_LIVE, "%.2f mm/s", NULL,
		"Thin water left behind dries this fast (not while it rains)" },
	{ TAB_WATER, "water", "still_level_millimeters", "Still below", 0, 20, 0.5f, EFFECT_LIVE, "%.1f mm", NULL,
		"Water does not turn back for a smaller level difference (stops rocking)" },
	{ TAB_WATER, "water", "shore_damping_depth_meters", "Shore depth", 0, 1, 0.01f, EFFECT_LIVE, "%.2f m", NULL, "Slow water shallower than this is braked" },
	{ TAB_WATER, "water", "shore_damping_per_second", "Shore brake", 0, 10, 0.1f, EFFECT_LIVE, "%.1f /s", NULL, "So waves die on the shore" },
	{ TAB_WATER, "water", "erosion_minimum_depth_millimeters", "Erodes from", 0, 100, 1, EFFECT_LIVE, "%.0f mm", NULL,
		"Shallower water digs less, nothing below half of this" },
	{ TAB_WATER, "water", "still_water_deposition_per_second", "Mud settling", 0, 10, 0.1f, EFFECT_LIVE, "%.1f /s", NULL, "Fraction of the mud dropped per second in still water" },
	HEADING(TAB_WATER, "Freeze"),
	{ TAB_WATER, "water", "freeze_after_seconds", "Freezes after", 0, 600, 5, EFFECT_LIVE, "%.0f s", NULL,
		"The water stops this long after you last touched terrain or water (F7 too); drawn flowing, not simulated (0: only F7)" },
	HEADING(TAB_WATER, "Simulation"),
	{ TAB_WATER, "simulation", "maximum_milliseconds_per_frame", "Time per frame", 5, 60, 1, EFFECT_LIVE, "%.0f ms",
		NULL, "Most simulation time in a frame; beyond it the game slows down" },
	{ TAB_WATER, "simulation", "pause_in_background", "Pause in background", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL },
	HEADING(TAB_OBJECTS, "Vegetation"),
	{ TAB_OBJECTS, "objects", "density_scale", "Density", 0, 3, 0.05f, EFFECT_OBJECTS, NULL, NULL, "Multiplies every plant's number per hectare" },
	{ TAB_OBJECTS, "objects", "brush_fill_seconds", "Brush fill time", 0.1f, 5, 0.1f, EFFECT_LIVE, "%.1f s", NULL, NULL },
	HEADING(TAB_OBJECTS, "Wind in the plants"),
	{ TAB_OBJECTS, "render", "plant_wind_strength", "Wind", 0, 1, 0.01f, EFFECT_LIVE, "%.2f", NULL,
		"How hard the wind blows in the grass and the trees (0: still, 1: a gale); objects.ini says how much each kind sways" },
	{ TAB_OBJECTS, "render", "plant_wind_direction_degrees", "Wind direction", 0, 360, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL,
		"Where it blows towards in the plants (not the clouds' wind): 0 = +X, 90 = +Z" },
	{ TAB_OBJECTS, "render", "plant_wind_gust_meters", "Gusts apart", 5, 200, 1, EFFECT_LIVE, "%.0f m", NULL,
		"How far apart the gusts are, rolling over the grass and through the woods" },
	{ TAB_OBJECTS, "render", "grass_gust_sheen", "Gust waves", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"How pale the grass a gust bows turns: the gusts seen rolling over a meadow (0: none)" },
	HEADING(TAB_OBJECTS, "Distance"),
	{ TAB_OBJECTS, "objects", "draw_distance_scale", "Object distance", 0.2f, 3, 0.05f, EFFECT_LIVE, NULL, NULL, "Multiplies every object's draw distance" },
	{ TAB_OBJECTS, "objects", "impostors", "Far objects as pictures", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Trees and houses far away are drawn as pictures of themselves (impostors): faster, the same at that size" },
	{ TAB_OBJECTS, "objects", "impostor_pixels", "Pictures from", 16, 512, 4, EFFECT_LIVE, "%.0f px", NULL,
		"Objects smaller than this on screen are drawn as pictures (their pictures are 64 px: above that they are a little soft)" },
	{ TAB_OBJECTS, "objects", "lod", "Fewer branches far away", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Trees made by potop_plant lose their finest branches, then the next, the smaller they are on screen (only the trunk and the foliage stay)" },
	{ TAB_OBJECTS, "objects", "lod_distance_scale", "Branches as far as", 0.25f, 4, 0.05f, EFFECT_LIVE, "x%.2f", NULL,
		"Higher: the branches stay to farther away (finer, slower); lower: they go sooner" },
	{ TAB_OBJECTS, "objects", "lod_foliage_kept", "Far foliage kept", 0.1f, 1, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The share of a far tree's twigs and leaves drawn (1: all); thinned out evenly" },
	{ TAB_OBJECTS, "objects", "prop_lod_pixels", "Rocks' detail off by", 0.1f, 4, 0.05f, EFFECT_LIVE, "%.2f px", NULL,
		"Rocks and walls made by potop_prop switch to a simpler level once it would be off by less than this on screen (lower: finer, the simpler ones only farther away)" },
	HEADING(TAB_OBJECTS, "Grass"),
	{ TAB_OBJECTS, "objects", "grass", "Grass blades", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Blades of grass made on the graphics card every frame, on the ground that shows grass" },
	{ TAB_OBJECTS, "objects", "grass_distance_meters", "Grass as far as", 10, 300, 5, EFFECT_LIVE, "%.0f m", NULL,
		"How far from the eye the blades go (farther they thin out and widen; past this the ground's texture alone)" },
	{ TAB_OBJECTS, "objects", "grass_density", "Grass density", 10, 1500, 10, EFFECT_LIVE, "%.0f /m\xc2\xb2", NULL,
		"Blades a square meter near the eye" },
	{ TAB_OBJECTS, "objects", "grass_height_meters", "Grass height", 0.05f, 1.5f, 0.01f, EFFECT_LIVE, "%.2f m", NULL,
		"How tall the blades are, give or take (shorter where less grows)" },
	{ TAB_OBJECTS, "objects", "grass_width_meters", "Blade width", 0.004f, 0.08f, 0.001f, EFFECT_LIVE, "%.3f m", NULL,
		"How wide a blade is at its foot, near the eye" },
	{ TAB_OBJECTS, "objects", "grass_tint", "Grass tint", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Multiplies the ground's grass color, which the blades take" },
	{ TAB_OBJECTS, "objects", "grass_dry_share", "Dry blades", 0, 0.6f, 0.01f, EFFECT_LIVE, "%.2f", NULL,
		"The share of yellowed, dry blades" },
	HEADING(TAB_OBJECTS, "Physics"),
	{ TAB_OBJECTS, "physics", "enabled", "Physics on", 0, 0, 0, EFFECT_LIVE, NULL, NULL, "Off: objects stay where they are" },
	{ TAB_OBJECTS, "physics", "drag_coefficient", "Water drag", 0.1f, 3, 0.05f, EFFECT_LIVE, NULL, NULL, "How hard flowing water pushes things" },
	{ TAB_OBJECTS, "physics", "water_minimum_meters", "Water from", 0, 0.2f, 0.005f, EFFECT_LIVE, "%.3f m", NULL, "Shallower water does not push things" },
	{ TAB_OBJECTS, "physics", "float_damping_per_second", "Bobbing brake", 0, 10, 0.1f, EFFECT_LIVE, "%.1f /s", NULL, "Stops floating things bobbing" },
	{ TAB_OBJECTS, "physics", "wake_flow_meters_per_second", "Wakes at", 0.02f, 1, 0.01f, EFFECT_LIVE, "%.2f m/s", NULL,
		"Water flowing this fast wakes sleeping objects" },
	{ TAB_OBJECTS, "physics", "hit_seconds", "Hit length", 0.02f, 0.5f, 0.01f, EFFECT_LIVE, "%.2f s", NULL,
		"Shorter hits are harder: they tear trees loose sooner" },
	{ TAB_OBJECTS, "physics", "terrain_resync_meters", "Ground follows", 0.05f, 2, 0.05f, EFFECT_LIVE, "%.2f m", NULL,
		"Erosion this deep makes the physics ground follow" },
	HEADING(TAB_OBJECTS, "Terrain brushes"),
	{ TAB_OBJECTS, "tools", "raise_meters_per_second", "Raise speed", 0.5f, 30, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL, NULL },
	{ TAB_OBJECTS, "tools", "smooth_strength_per_second", "Smoothing", 0.5f, 10, 0.1f, EFFECT_LIVE, "%.1f", NULL, NULL },
	{ TAB_OBJECTS, "tools", "brush_maximum_radius_meters", "Largest brush", 20, 300, 5, EFFECT_LIVE, "%.0f m", NULL, NULL },
	HEADING(TAB_RENDER, "Shading"),
	{ TAB_RENDER, "render", "pbr_shading", "PBR shading", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Objects and actors lit physically: normal maps, roughness, metal, their own light (off: plain diffuse, as before)" },
	HEADING(TAB_RENDER, "Shadows"),
	{ TAB_RENDER, "render", "shadow_strength", "Strength", 0, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_RENDER, "render", "shadow_map_size", "Map shadows", 0, 0, 0, EFFECT_RENDER, NULL, "0|1024|2048|4096|8192",
		"Texels per side of the shadow map over the whole map (0: no shadows)" },
	{ TAB_RENDER, "render", "near_shadow_map_size", "Near shadows", 0, 0, 0, EFFECT_RENDER, NULL, "0|1024|2048|4096",
		"The sharp shadows around the camera's target" },
	{ TAB_RENDER, "render", "shadow_depth_16_bit", "16 bit shadows", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The shadow maps' depth in 16 bits: half their memory and bandwidth (takes effect at the next start)" },
	{ TAB_RENDER, "render", "near_shadow_meters", "Near area", 20, 400, 5, EFFECT_LIVE, "%.0f m", NULL, NULL },
	{ TAB_RENDER, "render", "contact_shadows", "Contact shadows", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The small shadows the maps are too coarse for (stones on the ground, under a door), from the scene's depth near the eye" },
	{ TAB_RENDER, "render", "contact_shadow_meters", "Contact reach", 0.1f, 1.5f, 0.05f, EFFECT_LIVE, "%.2f m", NULL, NULL },
	{ TAB_RENDER, "render", "contact_shadow_strength", "Contact strength", 0, 1.5f, 0.05f, EFFECT_LIVE, "%.2f", NULL, NULL },
	{ TAB_RENDER, "render", "ambient_occlusion", "Ambient occlusion", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The small scale shade of the sky's light (a house's foot, under the eaves, round a barrel), from the scene's depth (GTAO)" },
	{ TAB_RENDER, "render", "ambient_occlusion_meters", "Occlusion radius", 0.25f, 3.0f, 0.05f, EFFECT_LIVE, "%.2f m", NULL, NULL },
	{ TAB_RENDER, "render", "ambient_occlusion_strength", "Occlusion strength", 0, 2.0f, 0.05f, EFFECT_LIVE, "%.2f", NULL, NULL },
	{ TAB_RENDER, "render", "ambient_occlusion_distance", "Occlusion distance", 20, 1000, 10, EFFECT_LIVE, "%.0f m", NULL, NULL },
	{ TAB_RENDER, "render", "indirect_light", "Bounced light", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The light bounced off what is near (sunlit grass on a wall, under the eaves), from the scene on the screen; with the ambient occlusion" },
	{ TAB_RENDER, "render", "indirect_light_strength", "Bounced strength", 0, 3.0f, 0.05f, EFFECT_LIVE, "%.2f", NULL, NULL },
	{ TAB_RENDER, "render", "indirect_light_meters", "Bounced reach", 0.25f, 10.0f, 0.25f, EFFECT_LIVE, "%.2f m", NULL, NULL },
	HEADING(TAB_RENDER, "Sky occlusion"),
	{ TAB_RENDER, "render", "sky_occlusion", "Sky occlusion", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Hollows, the ground under trees and round rocks see less of the sky (worked out once per map, kept with it)" },
	{ TAB_RENDER, "render", "sky_occlusion_intensity", "Strength", 0, 1.5f, 0.05f, EFFECT_LIVE, "%.2f", NULL, NULL },
	{ TAB_RENDER, "render", "sky_occlusion_directions", "Directions", 0, 0, 0, EFFECT_LIVE, NULL, "16|32|64|128|256",
		"From how many directions of the sky (more: smoother, slower to work out; Work it out)" },
	{ TAB_RENDER, "render", "sky_occlusion_texel_meters", "Detail", 0.1f, 4, 0.05f, EFFECT_LIVE, "%.2f m", NULL,
		"Texel of each direction's depth map (smaller: small things shade finer; at most 8192 texels over the map; Work it out)" },
	{ TAB_RENDER, "render", "bounce_light", "Bounce light", 0, 3, 0.05f, EFFECT_LIVE, "%.2f", NULL,
		"The light a sunlit slope or wall sends back onto what faces it (a crater's shady side, a cliff's foot)" },
	{ TAB_RENDER, "render", "sky_occlusion_least_height", "Smallest caster", 0, 10, 0.1f, EFFECT_LIVE, "%.1f m", NULL,
		"Things lower than this cast none: ferns, flowers, small bushes would shade themselves (Work it out)" },
	{ TAB_RENDER, "render", "sky_occlusion_lowest_degrees", "Lowest", 0, 45, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL,
		"The lowest direction above the horizon (Work it out)" },
	{ TAB_RENDER, "render", "sky_occlusion_directions_per_frame", "Per frame", 1, 32, 1, EFFECT_LIVE, "%.0f", NULL,
		"Directions worked out each frame while it is worked out (more: done sooner, the frames slower meanwhile)" },
	HEADING(TAB_RENDER, "Speed"),
	{ TAB_RENDER, "render", "render_scale", "Resolution", 0.25f, 1, 0.05f, EFFECT_LIVE, NULL, NULL, "Fraction of the window resolution the scene is drawn at" },
	{ TAB_RENDER, "render", "lod_distance_meters", "Terrain detail", 0, 400, 5, EFFECT_LIVE, "%.0f m", NULL, "Full detail up to here; 0: everywhere" },
	{ TAB_RENDER, "render", "far_terrain_upload_seconds", "Far ground updates", 0, 5, 0.1f, EFFECT_LIVE, "%.1f s", NULL,
		"Ground changed beyond full detail or off screen is redrawn at most this often (0: at once)" },
	{ TAB_RENDER, "render", "far_water_upload_seconds", "Far water updates", 0, 2, 0.05f, EFFECT_LIVE, "%.2f s", NULL,
		"Water beyond full detail or off screen is redrawn at most this often (0: every step)" },
	{ TAB_RENDER, "render", "far_upload_meters", "Far from", 0, 2000, 10, EFFECT_LIVE, "%.0f m", NULL,
		"Ground and water nearer than this are always redrawn at once (if on screen)" },
	{ TAB_RENDER, "render", "vsync", "Vertical sync", 0, 0, 0, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_RENDER, "render", "start_fullscreen", "Start fullscreen", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The game starts fullscreen (from the next start; F11 still switches)" },
	{ TAB_RENDER, "render", "start_maximized", "Start maximized", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"The game starts in a maximized window (from the next start; fullscreen wins if both are on)" },
	HEADING(TAB_TERRAIN, "Map"),
	{ TAB_TERRAIN, "terrain", "size", "Cells per side", 0, 0, 0, EFFECT_GENERATE, NULL, "512|1024|2048|4096", NULL },
	{ TAB_TERRAIN, "terrain", "cell_size_meters", "Cell size", 0.25f, 4, 0.25f, EFFECT_GENERATE, "%.2f m", NULL, NULL },
	{ TAB_TERRAIN, "terrain", "max_height_meters", "Highest point", 10, 400, 5, EFFECT_GENERATE, "%.0f m", NULL, NULL },
	HEADING(TAB_TERRAIN, "Shape"),
	{ TAB_TERRAIN, "terrain", "feature_count", "Big features", 1, 10, 0.1f, EFFECT_GENERATE, "%.1f", NULL, "Hills and valleys across the map" },
	{ TAB_TERRAIN, "terrain", "minimum_feature_meters", "Finest detail", 1, 64, 1, EFFECT_GENERATE, "%.0f m", NULL, NULL },
	{ TAB_TERRAIN, "terrain", "persistence", "Roughness", 0.2f, 0.7f, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL },
	{ TAB_TERRAIN, "terrain", "warp_amount", "Warp", 0, 1, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL },
	{ TAB_TERRAIN, "terrain", "mountain_amount", "Mountains", 0, 1, 0.01f, EFFECT_GENERATE, NULL, NULL, "0 rolling hills, 1 ridged mountains" },
	{ TAB_TERRAIN, "terrain", "mountain_coverage", "Mountain area", 0, 1, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL },
	{ TAB_TERRAIN, "terrain", "thermal_iterations", "Slides", 0, 200, 1, EFFECT_GENERATE, NULL, NULL, "Thermal erosion passes" },
	{ TAB_TERRAIN, "terrain", "talus_slope", "Steepest slope", 0.2f, 2, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL },
	HEADING(TAB_TERRAIN, "Soils"),
	{ TAB_TERRAIN, "terrain", "rock_slope", "Rock from slope", 0.3f, 2, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL },
	{ TAB_TERRAIN, "terrain", "rock_height_fraction", "Rock above", 0, 1, 0.01f, EFFECT_GENERATE, NULL, NULL, NULL },
	{ TAB_TERRAIN, "terrain", "sand_height_fraction", "Sand below", 0, 0.3f, 0.005f, EFFECT_GENERATE, "%.3f", NULL, NULL },
	{ TAB_TERRAIN, "terrain", "clay_height_fraction", "Clay below", 0, 0.5f, 0.005f, EFFECT_GENERATE, "%.3f", NULL, NULL },
	{ TAB_TERRAIN, "objects", "populate", "Grow vegetation", 0, 0, 0, EFFECT_GENERATE, NULL, NULL, "New maps get trees, grass and villages" },
	HEADING(TAB_CAMERA, "Camera"),
	{ TAB_CAMERA, "camera", "field_of_view_degrees", "Field of view", 15, 90, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL },
	{ TAB_CAMERA, "camera", "rotate_degrees_per_pixel", "Turn speed", 0.05f, 1, 0.01f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_CAMERA, "camera", "keyboard_pan_speed", "Key pan speed", 0.1f, 3, 0.05f, EFFECT_LIVE, NULL, NULL, "Screen heights per second" },
	{ TAB_CAMERA, "camera", "zoom_step_factor", "Zoom step", 1.02f, 1.5f, 0.01f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_CAMERA, "camera", "maximum_distance_meters", "Farthest zoom", 200, 6000, 50, EFFECT_LIVE, "%.0f m", NULL, NULL },
	{ TAB_CAMERA, "camera", "maximum_pitch_degrees", "Steepest view", 30, 89, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL },
	HEADING(TAB_CAMERA, "On foot (C)"),
	{ TAB_CAMERA, "camera", "first_person_field_of_view_degrees", "Field of view", 15, 110, 1, EFFECT_LIVE, "%.0f\xc2\xb0", NULL, NULL },
	{ TAB_CAMERA, "camera", "first_person_look_degrees_per_pixel", "Mouse speed", 0.02f, 0.5f, 0.01f, EFFECT_LIVE, NULL, NULL, NULL },
	{ TAB_CAMERA, "camera", "first_person_invert_mouse", "Invert mouse", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Mouse forward looks down, like a flight stick" },
	{ TAB_CAMERA, "camera", "walk_meters_per_second", "Walk speed", 1, 10, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL, NULL },
	{ TAB_CAMERA, "camera", "run_meters_per_second", "Run speed", 2, 30, 0.5f, EFFECT_LIVE, "%.1f m/s", NULL, "With shift" },
	{ TAB_CAMERA, "tools", "interact_highlight", "Glow what is in reach", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"What you look at, within reach, glows if the game can do something with it" },
	HEADING(TAB_DEBUG, "Water drawing"),
	{ TAB_DEBUG, "render", "water_depth_test", "Water depth test", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Off: the water is drawn over everything, hills in front of it included (debugging)" },
	{ TAB_DEBUG, "render", "water_depth_write", "Water depth write", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"On: the water hides water behind it (debugging)" },
	{ TAB_DEBUG, "render", "water_cull_back_faces", "Water back face culling", 0, 0, 0, EFFECT_LIVE, NULL, NULL,
		"Off: the faces of the water turned away from the camera are drawn too (debugging)" },
	HEADING(TAB_DEBUG, "Water sleep"),
	{ TAB_DEBUG, "water", "sleep_speed_meters_per_second", "Sleeps below", 0.005f, 0.5f, 0.005f, EFFECT_LIVE, "%.3f m/s", NULL,
		"A tile sleeps once all its water is slower than this" },
	{ TAB_DEBUG, "water", "sleep_change_millimeters_per_second", "Level still below", 0.5f, 10000, 1, EFFECT_LIVE, "%.1f mm/s", NULL,
		"A tile sleeps once no cell's level changes faster than this (small waves keep it awake below it)" },
	{ TAB_DEBUG, "water", "sleep_after_substeps", "Sleeps after", 1, 60, 1, EFFECT_LIVE, NULL, NULL, "Substeps of still water in a row" },
	{ TAB_DEBUG, "water", "asleep_wake_millimeters_per_second", "Wakes for", 1, 300, 1, EFFECT_LIVE, "%.0f mm/s", NULL,
		"Water pouring into a sleeping tile faster wakes it; slower is thrown away" },
	{ TAB_DEBUG, "water", "asleep_level_wake_millimeters", "Wakes for drop", 5, 500, 5, EFFECT_LIVE, "%.0f mm", NULL,
		"A neighbor this much lower wakes a sleeping tile (a breaking dam)" },
	HEADING(TAB_DEBUG, "Physics"),
	{ TAB_DEBUG, "physics", "substeps", "Substeps", 1, 8, 1, EFFECT_LIVE, NULL, NULL, "More is steadier and slower" },
};

#undef HEADING

static bool color_row(ui_t *ui, const char *label, float color[3], const char *tooltip)
{
	struct nk_context *context = &ui->backend.context;
	struct nk_colorf value = { color[0], color[1], color[2], 1.0f };
	bool changed = false;

	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT - 4.0f, 2, (const float[]){ 0.38f, 0.62f });
	tooltip_if_hovered(context, tooltip);
	nk_label(context, label, NK_TEXT_LEFT);
	if (nk_combo_begin_color(context, nk_rgb_cf(value), nk_vec2(240.0f, 300.0f)))
	{
		struct nk_colorf picked;

		nk_layout_row_dynamic(context, 160.0f, 1);
		picked = nk_color_picker(context, value, NK_RGB);
		nk_layout_row_dynamic(context, ROW_HEIGHT - 4.0f, 1);
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

static bool choice_row(ui_t *ui, const char *label, int32_t *value, const char *choices, const char *tooltip)
{
	struct nk_context *context = &ui->backend.context;
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
	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT - 4.0f, 2, (const float[]){ 0.38f, 0.62f });
	tooltip_if_hovered(context, tooltip);
	nk_label(context, label, NK_TEXT_LEFT);
	if (selected < 0)
	{
		// A value from the ini file that is not in the list: shown as is
		char current[16];

		SDL_snprintf(current, sizeof(current), "%d", *value);
		if (nk_combo_begin_label(context, current, nk_vec2(nk_widget_width(context), 200.0f)))
		{
			nk_layout_row_dynamic(context, ROW_HEIGHT - 4.0f, 1);
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
		int chosen = nk_combo(context, items, count, selected, (int)(ROW_HEIGHT - 4.0f), nk_vec2(nk_widget_width(context), 200.0f));

		if (chosen != selected)
		{
			*value = values[chosen];
			return true;
		}
	}
	return false;
}

// One row of the table; returns true if the value changed
static bool setting_row(ui_t *ui, settings_t *settings, const ui_setting_t *entry)
{
	setting_type_t type;
	void *value = settings_find(settings, entry->section, entry->key, &type);

	if (value == NULL)
	{
		return false;
	}
	switch (type)
	{
		case SETTING_FLOAT:
			return slider_row(ui, entry->label, value, entry->minimum, entry->maximum, entry->step,
				entry->format != NULL ? entry->format : "%.2f", entry->tooltip);
		case SETTING_INT:
			if (entry->choices != NULL)
			{
				return choice_row(ui, entry->label, value, entry->choices, entry->tooltip);
			}
			return slider_row_int(ui, entry->label, value, (int)entry->minimum, (int)entry->maximum, SDL_max((int)entry->step, 1),
				entry->tooltip);
		case SETTING_BOOL:
			return checkbox_row(ui, entry->label, value, entry->tooltip);
		case SETTING_COLOR:
			return color_row(ui, entry->label, value, entry->tooltip);
		case SETTING_UINT:
		default:
			return false;
	}
}

// What follows a setting changed in the window
static void setting_changed(ui_t *ui, settings_t *settings, const ui_setting_t *entry)
{
	// A cloud type sets the clouds' values; changing one of them makes the
	// clouds custom
	if (SDL_strcmp(entry->key, "cloud_type") == 0)
	{
		settings_apply_cloud_type(&settings->render);
	}
	else if (SDL_strcmp(entry->section, "render") == 0 && settings_is_cloud_type_key(entry->key))
	{
		settings->render.cloud_type = CLOUD_TYPE_CUSTOM;
	}
	if (entry->effect == EFFECT_RENDER)
	{
		ui->render_pending = true;
	}
	else if (entry->effect == EFFECT_OBJECTS)
	{
		ui->objects_pending = true;
	}
}

static void heading_row(ui_t *ui, const char *text, bool first)
{
	struct nk_context *context = &ui->backend.context;

	if (!first)
	{
		nk_layout_row_dynamic(context, 4.0f, 1);
		nk_spacer(context);
	}
	section_label(ui, text);
}

static void new_map_controls(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	nk_flags state;

	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 3, (const float[]){ 0.38f, 0.32f, 0.30f });
	tooltip_if_hovered(context, "Empty: a random seed");
	nk_label(context, "Seed", NK_TEXT_LEFT);
	state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, ui->seed_input, sizeof(ui->seed_input), nk_filter_decimal);
	if (state & NK_EDIT_ACTIVE)
	{
		ui->backend.wants_text_input = true;
	}
	if (nk_button_label(context, "This map's"))
	{
		SDL_snprintf(ui->seed_input, sizeof(ui->seed_input), "%u", view->seed);
	}
	nk_layout_row_dynamic(context, ROW_HEIGHT + 2.0f, 1);
	if (nk_button_label(context, "Generate a new map") || (state & NK_EDIT_COMMITED))
	{
		ui->requests.new_map = true;
		ui->requests.new_map_seed = (uint32_t)SDL_strtoul(ui->seed_input, NULL, 10);
		if (ui->maps_mode == UI_MAPS_NEW)
		{
			ui->maps_mode = UI_MAPS_CLOSED;
		}
	}
}

static void interface_tab(ui_t *ui)
{
	slider_row(ui, "Size", &ui->pending_scale, 0.75f, 2.5f, 0.05f, "%.2f", "Scale of the whole interface (Apply)");
	slider_row(ui, "Font size", &ui->pending_font_size, 10.0f, 28.0f, 1.0f, "%.0f", "In interface units (Apply)");
	slider_row(ui, "Dialog text", &ui->pending_dialog_text_scale, 1.0f, 3.0f, 0.05f, "%.2f",
		"The conversations' text, times the font size (a television across the room: 2 or more; Apply)");
	ui->interface_pending = ui->pending_scale != ui->scale || ui->pending_font_size != ui->font_size
		|| ui->pending_dialog_text_scale != ui->dialog_text_scale;
	slider_row(ui, "Brush panel", &ui->panel_opacity, 0.0f, 1.0f, 0.05f, "%.2f", "How see-through the brush panel is while the pointer is elsewhere");
	checkbox_row(ui, "Status line", &ui->show_status_line, NULL);
	section_label(ui, "F10 hides the interface; the keys keep working.");
}

static void settings_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	settings_t *settings = view->settings;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR;

	if (!ui->settings_open)
	{
		return;
	}
	if (nk_window_is_hidden(context, SETTINGS_WINDOW) && nk_window_find(context, SETTINGS_WINDOW) != NULL)
	{
		nk_window_show(context, SETTINGS_WINDOW, NK_SHOWN);
	}
	if (nk_begin(context, SETTINGS_WINDOW, ui->settings_bounds, flags))
	{
		struct nk_rect region;
		float footer = 2.0f * (ROW_HEIGHT + context->style.window.spacing.y) + 8.0f;

		ui->settings_bounds = nk_window_get_bounds(context);
		ui->settings_tab = SDL_clamp(ui->settings_tab, 0, TAB_COUNT - 1);
		// The tabs in two rows, then the search
		for (int tab = 0; tab < TAB_COUNT; tab++)
		{
			if (tab % SETTINGS_TABS_PER_ROW == 0)
			{
				nk_layout_row_dynamic(context, ROW_HEIGHT, SETTINGS_TABS_PER_ROW);
			}
			if (tool_button(ui, "", tab_names[tab], NULL, ui->settings_tab == tab && ui->settings_search[0] == '\0'))
			{
				ui->settings_tab = tab;
				ui->settings_search[0] = '\0';
			}
		}
		nk_layout_row_begin(context, NK_DYNAMIC, ROW_HEIGHT, 2);
		nk_layout_row_push(context, 0.15f);
		nk_label(context, "Search", NK_TEXT_LEFT);
		nk_layout_row_push(context, 0.85f);
		tooltip_if_hovered(context, "Shows the settings of every tab whose name (or key) has this");
		nk_edit_string_zero_terminated(context, NK_EDIT_FIELD, ui->settings_search, (int)sizeof(ui->settings_search), nk_filter_default);
		nk_layout_row_end(context);
		region = nk_window_get_content_region(context);
		nk_layout_row_dynamic(context, SDL_max(region.h - 3.0f * ROW_HEIGHT - footer - 16.0f, 60.0f), 1);
		if (ui->settings_search[0] != '\0')
		{
			// Searching: the rows of every tab that match, under their tab's name
			if (nk_group_begin(context, "Search results", NK_WINDOW_BORDER))
			{
				int last_tab = -1;
				int found = 0;

				for (size_t i = 0; i < sizeof(ui_settings) / sizeof(ui_settings[0]); i++)
				{
					const ui_setting_t *entry = &ui_settings[i];

					if (entry->section == NULL || (SDL_strcasestr(entry->label, ui->settings_search) == NULL
						&& SDL_strcasestr(entry->key, ui->settings_search) == NULL))
					{
						continue;
					}
					if ((int)entry->tab != last_tab)
					{
						heading_row(ui, tab_names[entry->tab], last_tab < 0);
						last_tab = (int)entry->tab;
					}
					found++;
					if (setting_row(ui, settings, entry))
					{
						setting_changed(ui, settings, entry);
					}
				}
				if (found == 0)
				{
					nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
					nk_label(context, "Nothing has that in its name", NK_TEXT_LEFT);
				}
				nk_group_end(context);
			}
		}
		else if (nk_group_begin(context, tab_names[ui->settings_tab], NK_WINDOW_BORDER))
		{
			bool first = true;

			if (ui->settings_tab == TAB_WATER)
			{
				nk_bool pipes = view->water->depth_scaled_pipes;

				heading_row(ui, "Now", true);
				first = false;
				nk_layout_row_dynamic(context, ROW_HEIGHT - 4.0f, 1);
				tooltip_if_hovered(context, "Thin water flows slowly; off: the classic fixed pipes (M)");
				if (nk_checkbox_label(context, "Depth scaled pipes", &pipes))
				{
					view->input->toggle_water_model = true;
				}
			}
			if (ui->settings_tab == TAB_INTERFACE)
			{
				interface_tab(ui);
			}
			for (size_t i = 0; i < sizeof(ui_settings) / sizeof(ui_settings[0]); i++)
			{
				const ui_setting_t *entry = &ui_settings[i];

				if ((int)entry->tab != ui->settings_tab)
				{
					continue;
				}
				if (entry->section == NULL)
				{
					heading_row(ui, entry->label, first);
					first = false;
					continue;
				}
				first = false;
				if (setting_row(ui, settings, entry))
				{
					setting_changed(ui, settings, entry);
				}
			}
			if (ui->settings_tab == TAB_RENDER)
			{
				heading_row(ui, "Sky occlusion now", false);
				nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
				tooltip_if_hovered(context, "Over the whole map, with the values above, a few directions a frame (U)");
				if (nk_button_label(context, "Work it out"))
				{
					view->input->bake_sky_occlusion = true;
				}
			}
			if (ui->settings_tab == TAB_TERRAIN)
			{
				heading_row(ui, "Generate", false);
				new_map_controls(ui, view);
			}
			if (ui->settings_tab == TAB_OBJECTS)
			{
				heading_row(ui, "Whole map", false);
				nk_layout_row_dynamic(context, ROW_HEIGHT, 2);
				tooltip_if_hovered(context, "Removes the vegetation and grows it again (V)");
				if (nk_button_label(context, "Grow anew"))
				{
					ui->requests.regrow = true;
					ui->objects_pending = false;
				}
				tooltip_if_hovered(context, "Every object, villages too (shift + V)");
				if (nk_button_label(context, "Remove all"))
				{
					ui->requests.remove_objects = true;
				}
			}
			nk_group_end(context);
		}

		// Footer: what waits for Apply, and the buttons
		{
			bool pending = ui->render_pending || ui->objects_pending || ui->interface_pending;
			char note[128] = "";
			struct nk_color dim = ui->colors[NK_COLOR_TEXT];

			dim.a = 160;
			if (pending)
			{
				SDL_snprintf(note, sizeof(note), "Apply: %s%s%s", ui->render_pending ? "graphics " : "",
					ui->objects_pending ? "vegetation " : "", ui->interface_pending ? "interface" : "");
			}
			else if (ui->settings_tab == TAB_TERRAIN)
			{
				SDL_strlcpy(note, "These shape the next new map.", sizeof(note));
			}
			nk_layout_row_dynamic(context, ROW_HEIGHT - 6.0f, 1);
			nk_label_colored(context, note, NK_TEXT_LEFT, dim);
			nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
			if (!pending)
			{
				nk_widget_disable_begin(context);
			}
			if (nk_button_label(context, "Apply"))
			{
				ui->requests.apply_render = ui->render_pending;
				ui->requests.apply_objects = ui->objects_pending;
				ui->render_pending = false;
				ui->objects_pending = false;
				if (ui->interface_pending)
				{
					ui->scale = ui->pending_scale;
					ui->font_size = ui->pending_font_size;
					ui->dialog_text_scale = ui->pending_dialog_text_scale;
					ui->interface_pending = false;
					// Rebuilt after this frame is drawn (see ui_render)
				}
			}
			if (!pending)
			{
				nk_widget_disable_end(context);
			}
			tooltip_if_hovered(context, "Keep these settings for the next start (settings_changed.ini)");
			if (nk_button_label(context, "Save"))
			{
				ui->requests.save_settings = true;
			}
			tooltip_if_hovered(context, "Back to the values of data/settings.ini");
			if (nk_button_label(context, "Restore"))
			{
				ui->requests.restore_settings = true;
				ui->render_pending = false;
				ui->objects_pending = false;
			}
		}
	}
	else
	{
		// Closed with its X
		ui->settings_open = false;
	}
	nk_end(context);
	if (!ui->settings_open)
	{
		nk_window_close(context, SETTINGS_WINDOW);
	}
}

// ---------------------------------------------------------------------------
// The sky: the clouds' map, painted

static void sky_window(ui_t *ui, const ui_view_t *view)
{
	static const char *const channel_names[CLOUD_PAINT_CHANNELS] = { "Coverage", "Heaviness", "Kind", "Flocks" };
	static const char *const channel_legends[CLOUD_PAINT_CHANNELS] =
	{
		"Blue: clear sky, white: cloudy (middle: the Coverage setting)",
		"White: puffy and bright, gray: heavy and dark",
		"Green: fair weather puffs, yellow: heaps, red: towers",
		"Blue: none, white: small clouds high up (their settings: Clouds tab)",
	};
	struct nk_context *context = &ui->backend.context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE;
	cloud_paint_t *paint = view->cloud_paint;
	uint64_t now = SDL_GetTicks();
	float elapsed = ui->sky_last_ticks != 0 ? SDL_min((float)(now - ui->sky_last_ticks) * 0.001f, 0.1f) : 0.0f;

	ui->sky_last_ticks = now;
	if (!ui->sky_open || paint == NULL)
	{
		return;
	}
	if (nk_window_is_hidden(context, SKY_WINDOW) && nk_window_find(context, SKY_WINDOW) != NULL)
	{
		nk_window_show(context, SKY_WINDOW, NK_SHOWN);
	}
	ui->sky_channel = SDL_clamp(ui->sky_channel, 0, CLOUD_PAINT_CHANNELS - 1);
	if (nk_begin(context, SKY_WINDOW, ui->sky_bounds, flags))
	{
		cloud_paint_channel_t channel = (cloud_paint_channel_t)ui->sky_channel;
		struct nk_rect region;
		struct nk_rect map_bounds;
		SDL_GPUTexture *picture;
		struct nk_input *input = &context->input;
		float side;
		float eye[2];
		float corner[2];

		ui->sky_bounds = nk_window_get_bounds(context);
		// The time of day (render.day_cycle): the hour, and whether it
		// stands still
		if (view->day_hour != NULL)
		{
			float hour = (float)*view->day_hour;
			char text[32];
			nk_bool frozen = view->day_frozen != NULL && *view->day_frozen;

			nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT - 4.0f, 4, (const float[]){ 0.18f, 0.50f, 0.14f, 0.18f });
			nk_label(context, "Hour", NK_TEXT_LEFT);
			if (nk_slider_float(context, 0.0f, &hour, 23.99f, 0.05f))
			{
				*view->day_hour = (double)hour;
			}
			SDL_snprintf(text, sizeof(text), "%02d:%02d", (int)hour, (int)(SDL_fmodf(hour, 1.0f) * 60.0f));
			nk_label(context, text, NK_TEXT_RIGHT);
			tooltip_if_hovered(context, "The day stands still at this hour (the console's freeze)");
			if (view->day_frozen != NULL && nk_checkbox_label(context, "Stop", &frozen))
			{
				*view->day_frozen = frozen;
			}
		}
		nk_layout_row_dynamic(context, ROW_HEIGHT, CLOUD_PAINT_CHANNELS);
		for (int c = 0; c < CLOUD_PAINT_CHANNELS; c++)
		{
			if (nk_option_label(context, channel_names[c], ui->sky_channel == c))
			{
				ui->sky_channel = c;
			}
		}
		channel = (cloud_paint_channel_t)ui->sky_channel;
		section_label(ui, channel_legends[channel]);
		slider_row(ui, "Brush", &ui->sky_brush_kilometers, 0.2f, 8.0f, 0.1f, "%.1f km", "How far the brush reaches");
		slider_row(ui, "Strength", &ui->sky_brush_strength, 0.1f, 4.0f, 0.1f, "%.1f", "How fast it paints");
		checkbox_row(ui, "Back to normal", &ui->sky_brush_neutral, "The brush brings back the sky the settings make");
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		nk_label(context, "Left: more, right: less. The dot: you; the line: the wind", NK_TEXT_LEFT);

		// The map, square, as wide as the window
		region = nk_window_get_content_region(context);
		side = SDL_max(SDL_min(region.w, region.h - 8.0f * ROW_HEIGHT), 64.0f);
		nk_layout_row_static(context, side, (int)side, 1);
		map_bounds = nk_widget_bounds(context);
		{
			float eye_u = SDL_fmodf(view->sky_eye[0] / CLOUD_WEATHER_METERS, 1.0f);
			float eye_v = SDL_fmodf(view->sky_eye[1] / CLOUD_WEATHER_METERS, 1.0f);

			eye[0] = eye_u < 0.0f ? eye_u + 1.0f : eye_u;
			eye[1] = eye_v < 0.0f ? eye_v + 1.0f : eye_v;
		}
		// Centered on the eye (the map goes round)
		picture = cloud_paint_picture(paint, channel, eye[0], eye[1], corner);
		if (picture != NULL)
		{
			nk_image(context, nk_image_ptr(picture));
		}
		else
		{
			nk_spacing(context, 1);
		}
		{
			bool more = nk_input_is_mouse_down(input, NK_BUTTON_LEFT);
			bool less = nk_input_is_mouse_down(input, NK_BUTTON_RIGHT);
			bool over = nk_input_is_mouse_hovering_rect(input, map_bounds);
			float u = corner[0] + (input->mouse.pos.x - map_bounds.x) / SDL_max(map_bounds.w, 1.0f);
			float v = corner[1] + (input->mouse.pos.y - map_bounds.y) / SDL_max(map_bounds.h, 1.0f);
			float radius = ui->sky_brush_kilometers * 1000.0f / CLOUD_WEATHER_METERS;
			struct nk_command_buffer *canvas = nk_window_get_canvas(context);
			float wind = view->settings->render.wind_direction_degrees * (3.14159265f / 180.0f);
			if (over && (more || less) && elapsed > 0.0f)
			{
				cloud_paint_brush(paint, channel, u, v, radius, (more ? 1.0f : -1.0f) * ui->sky_brush_strength * elapsed,
					ui->sky_brush_neutral);
			}
			// The eye, the wind, the brush
			{
				float x = map_bounds.x + (eye[0] - corner[0]) * map_bounds.w;
				float y = map_bounds.y + (eye[1] - corner[1]) * map_bounds.h;
				float arrow = side * 0.06f;

				nk_fill_circle(canvas, nk_rect(x - 4.0f, y - 4.0f, 8.0f, 8.0f), nk_rgb(230, 40, 40));
				nk_stroke_line(canvas, x, y, x + SDL_cosf(wind) * arrow, y + SDL_sinf(wind) * arrow, 2.0f, nk_rgb(230, 40, 40));
			}
			if (over)
			{
				float reach = radius * map_bounds.w;

				nk_stroke_circle(canvas, nk_rect(input->mouse.pos.x - reach, input->mouse.pos.y - reach, reach * 2.0f, reach * 2.0f), 1.5f,
					nk_rgb(20, 20, 20));
			}
		}
		nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
		if (nk_button_label(context, "Normal sky"))
		{
			cloud_paint_reset(paint);
		}
	}
	else
	{
		ui->sky_open = false;
	}
	nk_end(context);
	if (!ui->sky_open)
	{
		nk_window_close(context, SKY_WINDOW);
	}
}

// ---------------------------------------------------------------------------
// World: what is shown of the world (more to come)

static void world_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE;

	if (!ui->world_open)
	{
		return;
	}
	if (nk_window_is_hidden(context, WORLD_WINDOW) && nk_window_find(context, WORLD_WINDOW) != NULL)
	{
		nk_window_show(context, WORLD_WINDOW, NK_SHOWN);
	}
	if (nk_begin(context, WORLD_WINDOW, ui->world_bounds, flags))
	{
		ui->world_bounds = nk_window_get_bounds(context);
		if (view->objects_hidden != NULL)
		{
			bool shown = !*view->objects_hidden;

			if (checkbox_row(ui, "Objects", &shown,
				"Trees, rocks, houses... drawn (off: not drawn, nor their shadows and lights; they stay and go on, unseen)"))
			{
				*view->objects_hidden = !shown;
			}
		}
		if (view->no_terrain_occlusion != NULL)
		{
			bool hiding = !*view->no_terrain_occlusion;

			if (checkbox_row(ui, "Hidden objects culled", &hiding,
				"Objects behind a mountain or a house are not drawn (off: all are drawn, slower; to compare)"))
			{
				*view->no_terrain_occlusion = !hiding;
			}
		}
		if (view->aurora_on != NULL)
		{
			checkbox_row(ui, "Aurora", view->aurora_on, "The northern lights, on dark nights (until switched off)");
		}
		if (view->raining != NULL)
		{
			checkbox_row(ui, "Rain", view->raining, "It rains (R): drops, splashes, wet ground (how hard: Settings, Sky)");
		}
		if (view->rainbow_on != NULL)
		{
			checkbox_row(ui, "Rainbow", view->rainbow_on,
				"A rainbow opposite the sun, when the sun is low enough (under 42 degrees; until switched off)");
		}
		if (view->month != NULL)
		{
			char month_label[32];

			SDL_snprintf(month_label, sizeof(month_label), "Month (%s)", season_month_name(*view->month));
			slider_row(ui, month_label, view->month, 1.0f, 12.99f, 0.01f, "%.2f",
				"The year in the plants: 1 the start of January .. 12.99 the end of December. Each month shows the plants' "
				"pictures for it; through a plant's autumn months its leaves turn and fall (potop_plant's plants)");
		}
		if (view->snow_first_month != NULL && view->snow_last_month != NULL)
		{
			slider_row(ui, "Snow from month", view->snow_first_month, 1.0f, 12.0f, 1.0f, "%.0f",
				"The winter's snow on the bark, and the branches sagging under it: from the start of this month");
			slider_row(ui, "Snow to month", view->snow_last_month, 1.0f, 12.0f, 1.0f, "%.0f",
				"To the end of this one (round the year)");
		}
		if (view->tips_first_month != NULL && view->tips_last_month != NULL)
		{
			slider_row(ui, "New growth from month", view->tips_first_month, 1.0f, 12.0f, 1.0f, "%.0f",
				"The foliage's lighter tips (the new growth): growing through this month");
			slider_row(ui, "New growth to month", view->tips_last_month, 1.0f, 12.0f, 1.0f, "%.0f",
				"To the end of this one, going in its last half (round the year)");
		}
		if (view->bloom_first_month != NULL && view->bloom_last_month != NULL)
		{
			slider_row(ui, "Petals from month", view->bloom_first_month, 1.0f, 12.0f, 1.0f, "%.0f",
				"The blooming trees' petals on the ground (objects.ini: cover = petals): falling from the start of this month");
			slider_row(ui, "Petals to month", view->bloom_last_month, 1.0f, 12.0f, 1.0f, "%.0f",
				"Gone by the end of this one");
		}
		if (view->impostors_tinted != NULL)
		{
			checkbox_row(ui, "Show impostors", view->impostors_tinted,
				"Debugging: objects drawn as pictures of themselves (far away) are tinted magenta");
		}
		if (view->physics_off != NULL)
		{
			bool running = !*view->physics_off;

			if (checkbox_row(ui, "Physics", &running, "Debugging: the bodies move (off: no physics steps, everything stays where it is)"))
			{
				*view->physics_off = !running;
			}
		}
		if (view->far_shadow_off != NULL && view->near_shadow_off != NULL)
		{
			bool far = !*view->far_shadow_off;
			bool near = !*view->near_shadow_off;

			if (checkbox_row(ui, "Far shadow map", &far, "Debugging: the shadows over the whole map (off: neither drawn nor used)"))
			{
				*view->far_shadow_off = !far;
			}
			if (checkbox_row(ui, "Near shadow map", &near, "Debugging: the fine shadows around the camera (off: neither drawn nor used)"))
			{
				*view->near_shadow_off = !near;
			}
		}
		if (view->fireflies != NULL)
		{
			slider_row(ui, "Fireflies", view->fireflies, 0.0f, 1.0f, 0.05f, "%.2f",
				"On dark nights, swarms of them near the eye, on grass, most near water (0: none)");
		}
		if (view->actor_name != NULL && view->actor_game_id != NULL && view->actor_hostile != NULL)
		{
			section_label(ui, "Actors K puts");
			nk_layout_row_template_begin(context, ROW_HEIGHT);
			nk_layout_row_template_push_static(context, 80.0f);
			nk_layout_row_template_push_dynamic(context);
			nk_layout_row_template_end(context);
			nk_label(context, "Name", NK_TEXT_LEFT);
			nk_edit_string_zero_terminated(context, NK_EDIT_FIELD, view->actor_name, 32, nk_filter_default);
			nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
			nk_property_int(context, "Game id", 0, view->actor_game_id, 1000000, 1, 1.0f);
			tooltip_if_hovered(context, "The game's number for the actor (a quest's wolf); 0: none. Kept with the map");
			checkbox_row(ui, "Enemy", view->actor_hostile, "Its name over its head in red");
		}
	}
	else
	{
		ui->world_open = false;
	}
	nk_end(context);
	if (!ui->world_open)
	{
		nk_window_close(context, WORLD_WINDOW);
	}
}

// ---------------------------------------------------------------------------
// Diagnostics

static void diagnostics_window(ui_t *ui)
{
	struct nk_context *context = &ui->backend.context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE;

	if (!ui->diagnostics_open)
	{
		return;
	}
	if (nk_window_is_hidden(context, DIAGNOSTICS_WINDOW) && nk_window_find(context, DIAGNOSTICS_WINDOW) != NULL)
	{
		nk_window_show(context, DIAGNOSTICS_WINDOW, NK_SHOWN);
	}
	if (nk_begin(context, DIAGNOSTICS_WINDOW, ui->diagnostics_bounds, flags))
	{
		ui->diagnostics_bounds = nk_window_get_bounds(context);
		for (int i = 0; i < ui->diagnostic_count; i++)
		{
			const char *line = ui->diagnostics[i];

			if (line[0] == '#')
			{
				// A heading
				section_label(ui, line + 1);
				continue;
			}
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, line, NK_TEXT_LEFT);
		}
	}
	else
	{
		ui->diagnostics_open = false;
	}
	nk_end(context);
	if (!ui->diagnostics_open)
	{
		nk_window_close(context, DIAGNOSTICS_WINDOW);
	}
}

// ---------------------------------------------------------------------------
// Inspector

// A small line graph of the history, oldest first
static void history_chart(ui_t *ui, const char *title, const float *values, const inspector_t *inspector, const char *unit)
{
	struct nk_context *context = &ui->backend.context;
	float lowest = 1e30f;
	float highest = -1e30f;
	char text[96];
	int first = inspector->history_count < INSPECTOR_HISTORY ? 0 : inspector->history_next;

	for (int i = 0; i < inspector->history_count; i++)
	{
		lowest = SDL_min(lowest, values[i]);
		highest = SDL_max(highest, values[i]);
	}
	SDL_snprintf(text, sizeof(text), "%s: %.3f .. %.3f %s", title, (double)lowest, (double)highest, unit);
	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
	nk_label(context, text, NK_TEXT_LEFT);
	if (highest - lowest < 1e-4f)
	{
		highest = lowest + 1e-4f;
	}
	nk_layout_row_dynamic(context, 56.0f, 1);
	if (nk_chart_begin(context, NK_CHART_LINES, inspector->history_count, lowest, highest))
	{
		for (int i = 0; i < inspector->history_count; i++)
		{
			nk_chart_push(context, values[(first + i) % INSPECTOR_HISTORY]);
		}
		nk_chart_end(context);
	}
}

static void inspector_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	inspector_t *inspector = view->inspector;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE;
	// Dry, film, thin, deep, wall
	struct nk_color kind_colors[] = { nk_rgb(110, 110, 110), nk_rgb(170, 140, 100), nk_rgb(210, 200, 110), nk_rgb(100, 170, 255),
		nk_rgb(230, 110, 110) };

	if (inspector == NULL || (view->tools->current_tool != TOOL_INSPECT && !inspector->pinned))
	{
		return;
	}
	if (nk_begin(context, INSPECTOR_WINDOW, ui->inspector_bounds, flags))
	{
		ui->inspector_bounds = nk_window_get_bounds(context);
		nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
		nk_label(context, inspector->pinned ? "Pinned" : "Under the pointer", NK_TEXT_LEFT);
		if (nk_button_label(context, "Let go"))
		{
			inspector->unpin_requested = true;
		}
		if (nk_button_label(context, "Log"))
		{
			inspector->log_requested = true;
		}
		for (int i = 0; i < inspector->line_count; i++)
		{
			const char *line = inspector->lines[i];

			if (line[0] == '#')
			{
				section_label(ui, line + 1);
				continue;
			}
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, line, NK_TEXT_LEFT);
		}
		if (inspector->valid)
		{
			section_label(ui, "LEVELS AROUND IT, MM (YOURS IN THE MIDDLE)");
			for (int row = 0; row < INSPECTOR_GRID; row++)
			{
				nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, INSPECTOR_GRID);
				for (int column = 0; column < INSPECTOR_GRID; column++)
				{
					char text[32];
					uint8_t kind = inspector->grid_kind[row][column];

					if (kind == INSPECTOR_CELL_OUTSIDE)
					{
						SDL_strlcpy(text, "-", sizeof(text));
					}
					else
					{
						SDL_snprintf(text, sizeof(text), row == INSPECTOR_GRID / 2 && column == INSPECTOR_GRID / 2 ? "[%+.2f]" : "%+.2f",
							(double)inspector->grid[row][column]);
					}
					nk_label_colored(context, text, NK_TEXT_CENTERED, kind_colors[SDL_max((int)kind - 1, 0)]);
				}
			}
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, "gray dry, brown film, yellow thin, blue deep, red wall", NK_TEXT_LEFT);
			if (inspector->history_count > 1)
			{
				section_label(ui, "LAST 10 SECONDS");
				history_chart(ui, "level", inspector->level_history, inspector, "mm");
				history_chart(ui, "busiest pipe", inspector->speed_history, inspector, "m/s");
			}
		}
	}
	nk_end(context);
}

// ---------------------------------------------------------------------------
// Saving and loading maps

static void format_date(int64_t nanoseconds, char *text_out, size_t text_out_size)
{
	SDL_DateTime date;

	if (!SDL_TimeToDateTime((SDL_Time)nanoseconds, &date, true))
	{
		SDL_strlcpy(text_out, "?", text_out_size);
		return;
	}
	SDL_snprintf(text_out, text_out_size, "%04d-%02d-%02d %02d:%02d", date.year, date.month, date.day, date.hour, date.minute);
}

static bool map_name_ends_with(const char *name, const char *end)
{
	size_t length = SDL_strlen(name);
	size_t end_length = SDL_strlen(end);

	return length >= end_length && SDL_strcmp(name + length - end_length, end) == 0;
}

static bool map_name_is_baked(const char *name)
{
	return map_name_ends_with(name, MAP_FILE_BAKED_SUFFIX);
}

static void map_name_without_kind(const char *name, char *name_out, size_t name_out_size)
{
	SDL_strlcpy(name_out, name, name_out_size);
	if (map_name_ends_with(name, MAP_FILE_BAKED_SUFFIX))
	{
		name_out[SDL_min(SDL_strlen(name) - SDL_strlen(MAP_FILE_BAKED_SUFFIX), name_out_size - 1)] = '\0';
	}
	else if (map_name_ends_with(name, MAP_FILE_ACTIVE_SUFFIX))
	{
		name_out[SDL_min(SDL_strlen(name) - SDL_strlen(MAP_FILE_ACTIVE_SUFFIX), name_out_size - 1)] = '\0';
	}
}

static bool map_exists(const ui_t *ui, const char *name)
{
	for (int i = 0; i < ui->map_entry_count; i++)
	{
		if (SDL_strcmp(ui->map_entries[i].name, name) == 0)
		{
			return true;
		}
	}
	return false;
}

static void map_list(ui_t *ui, float height)
{
	struct nk_context *context = &ui->backend.context;

	nk_layout_row_dynamic(context, height, 1);
	if (nk_group_begin(context, "saved_maps", NK_WINDOW_BORDER))
	{
		if (ui->map_entry_count == 0)
		{
			nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
			nk_label(context, "No saved maps yet", NK_TEXT_CENTERED);
		}
		for (int i = 0; i < ui->map_entry_count; i++)
		{
			const map_file_entry_t *entry = &ui->map_entries[i];
			char date[48];
			char size[32];
			nk_bool selected = ui->selected_map == i;

			format_date(entry->modified_nanoseconds, date, sizeof(date));
			SDL_snprintf(size, sizeof(size), "%.1f MB", (double)entry->size_bytes / (1024.0 * 1024.0));
			nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT - 4.0f, 4, (const float[]){ 0.38f, 0.13f, 0.32f, 0.17f });
			if (nk_selectable_label(context, entry->shown_name, NK_TEXT_LEFT, &selected))
			{
				ui->selected_map = i;
				ui->confirm_delete = false;
				ui->confirm_overwrite = false;
				SDL_strlcpy(ui->map_name_input, entry->shown_name, sizeof(ui->map_name_input));
				// (saving over it: as its kind, if this map may be saved so)
				if (!ui->save_locked)
				{
					ui->save_baked = entry->baked;
				}
			}
			nk_label_colored(context, entry->baked ? "baked" : "active", NK_TEXT_LEFT,
				entry->baked ? nk_rgb(150, 200, 255) : nk_rgb(255, 210, 140));
			nk_label(context, date, NK_TEXT_LEFT);
			nk_label(context, size, NK_TEXT_RIGHT);
		}
		nk_group_end(context);
	}
}

static void maps_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR;
	const char *title = ui->maps_mode == UI_MAPS_SAVE ? "Save the map" : (ui->maps_mode == UI_MAPS_LOAD ? "Load a map" : "New map");
	float height = ui->maps_mode == UI_MAPS_NEW ? 170.0f : 420.0f;

	if (ui->maps_mode == UI_MAPS_CLOSED)
	{
		return;
	}
	if (nk_window_is_hidden(context, MAPS_WINDOW) && nk_window_find(context, MAPS_WINDOW) != NULL)
	{
		nk_window_show(context, MAPS_WINDOW, NK_SHOWN);
	}
	ui->maps_bounds.h = height;
	if (nk_begin_titled(context, MAPS_WINDOW, title, ui->maps_bounds, flags))
	{
		struct nk_rect bounds = nk_window_get_bounds(context);

		if (bounds.h != height)
		{
			bounds.h = height;
			nk_window_set_bounds(context, MAPS_WINDOW, bounds);
		}
		ui->maps_bounds = bounds;
		if (ui->maps_mode == UI_MAPS_NEW)
		{
			new_map_controls(ui, view);
			section_label(ui, "The map's shape: Settings, New map.");
		}
		else if (ui->maps_mode == UI_MAPS_SAVE)
		{
			nk_flags state;
			char clean[MAP_FILE_NAME_LENGTH];
			char file_name[MAP_FILE_NAME_LENGTH + 16];
			nk_bool baked;

			nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 2, (const float[]){ 0.2f, 0.8f });
			nk_label(context, "Name", NK_TEXT_LEFT);
			state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, ui->map_name_input, sizeof(ui->map_name_input),
				nk_filter_default);
			if (state & NK_EDIT_ACTIVE)
			{
				ui->backend.wants_text_input = true;
			}
			// Baked (for play) or active (to go on making it); a baked map
			// can only be saved baked
			ui->save_locked = view->map_baked;
			if (view->map_baked)
			{
				ui->save_baked = true;
			}
			baked = ui->save_baked;
			nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 2, (const float[]){ 0.45f, 0.55f });
			if (view->map_baked)
			{
				nk_widget_disable_begin(context);
			}
			if (nk_checkbox_label(context, "Baked (for play)", &baked))
			{
				ui->save_baked = baked;
				ui->confirm_overwrite = false;
			}
			if (view->map_baked)
			{
				nk_widget_disable_end(context);
			}
			nk_label(context, view->map_baked ? "this map is baked" : (ui->save_baked ? "still: no water, no digging" : "everything goes on"),
				NK_TEXT_LEFT);
			map_list(ui, height - 3.0f * ROW_HEIGHT - 80.0f);
			map_file_clean_name(ui->map_name_input, clean, sizeof(clean));
			map_file_kind_name(clean, ui->save_baked, file_name, sizeof(file_name));
			if (ui->confirm_overwrite)
			{
				char question[128];

				SDL_snprintf(question, sizeof(question), "Replace \"%s\" (%s)?", clean, ui->save_baked ? "baked" : "active");
				nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
				nk_label(context, question, NK_TEXT_LEFT);
				if (nk_button_label(context, "Replace"))
				{
					ui->requests.save_map = true;
					ui->requests.map_baked = ui->save_baked;
					SDL_strlcpy(ui->requests.map_name, clean, sizeof(ui->requests.map_name));
					ui->maps_mode = UI_MAPS_CLOSED;
				}
				if (nk_button_label(context, "Cancel"))
				{
					ui->confirm_overwrite = false;
				}
			}
			else
			{
				nk_layout_row_dynamic(context, ROW_HEIGHT, 2);
				if (clean[0] == '\0')
				{
					nk_widget_disable_begin(context);
				}
				if (nk_button_label(context, ui->save_baked ? "Bake" : "Save") || ((state & NK_EDIT_COMMITED) && clean[0] != '\0'))
				{
					if (map_exists(ui, file_name))
					{
						ui->confirm_overwrite = true;
					}
					else
					{
						ui->requests.save_map = true;
						ui->requests.map_baked = ui->save_baked;
						SDL_strlcpy(ui->requests.map_name, clean, sizeof(ui->requests.map_name));
						ui->maps_mode = UI_MAPS_CLOSED;
					}
				}
				if (clean[0] == '\0')
				{
					nk_widget_disable_end(context);
				}
				if (nk_button_label(context, "Cancel"))
				{
					ui->maps_mode = UI_MAPS_CLOSED;
				}
			}
		}
		else
		{
			bool has_selection = ui->selected_map >= 0 && ui->selected_map < ui->map_entry_count;

			map_list(ui, height - ROW_HEIGHT - 72.0f);
			if (ui->confirm_delete && has_selection)
			{
				char question[128];

				SDL_snprintf(question, sizeof(question), "Delete \"%s\" (%s)?", ui->map_entries[ui->selected_map].shown_name,
					ui->map_entries[ui->selected_map].baked ? "baked" : "active");
				nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
				nk_label(context, question, NK_TEXT_LEFT);
				if (nk_button_label(context, "Delete"))
				{
					map_file_delete(ui->map_entries[ui->selected_map].name);
					ui->map_entry_count = map_file_list(ui->map_entries, MAP_FILE_LIST_MAX);
					ui->selected_map = -1;
					ui->confirm_delete = false;
				}
				if (nk_button_label(context, "Keep"))
				{
					ui->confirm_delete = false;
				}
			}
			else
			{
				nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
				if (!has_selection)
				{
					nk_widget_disable_begin(context);
				}
				if (nk_button_label(context, "Load") && has_selection)
				{
					ui->requests.load_map = true;
					SDL_strlcpy(ui->requests.map_name, ui->map_entries[ui->selected_map].name, sizeof(ui->requests.map_name));
					ui->maps_mode = UI_MAPS_CLOSED;
				}
				if (nk_button_label(context, "Delete") && has_selection)
				{
					ui->confirm_delete = true;
				}
				if (!has_selection)
				{
					nk_widget_disable_end(context);
				}
				if (nk_button_label(context, "Cancel"))
				{
					ui->maps_mode = UI_MAPS_CLOSED;
				}
			}
		}
	}
	else
	{
		ui->maps_mode = UI_MAPS_CLOSED;
	}
	nk_end(context);
	if (ui->maps_mode == UI_MAPS_CLOSED)
	{
		nk_window_close(context, MAPS_WINDOW);
	}
}

// ---------------------------------------------------------------------------

// The console across the top: the newest lines of the log over the line
// being typed (console.c edits it; this only draws it)
static void console_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	console_t *console = view->console;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR;
	float height;
	struct nk_rect bounds;

	ui->console_shown = console != NULL && console->open;
	if (!ui->console_shown)
	{
		if (nk_window_find(context, CONSOLE_WINDOW) != NULL && !nk_window_is_closed(context, CONSOLE_WINDOW))
		{
			nk_window_close(context, CONSOLE_WINDOW);
		}
		return;
	}
	if (nk_window_is_hidden(context, CONSOLE_WINDOW) && nk_window_find(context, CONSOLE_WINDOW) != NULL)
	{
		nk_window_show(context, CONSOLE_WINDOW, NK_SHOWN);
	}
	height = SDL_floorf(SDL_max(ui->backend.height * 0.45f, 4.0f * SMALL_ROW_HEIGHT + ROW_HEIGHT));
	bounds = nk_rect(0.0f, 0.0f, ui->backend.width, height);
	// The typing goes to the console (main.c hands it the keys), and SDL
	// sends text only while some field wants it
	ui->backend.wants_text_input = true;
	// Nearly solid: the log is read over a busy scene
	{
		struct nk_color background = ui->colors[NK_COLOR_WINDOW];

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
		float row = SMALL_ROW_HEIGHT + style->window.spacing.y;
		float room = height - 2.0f * style->window.padding.y - ROW_HEIGHT - 2.0f * style->window.spacing.y;
		int32_t visible = SDL_max((int32_t)(room / row), 1);
		char text[CONSOLE_LINE_LENGTH + 32];
		char typed[CONSOLE_INPUT_LENGTH + 8];

		for (int32_t i = visible - 1; i >= 0; i--)
		{
			SDL_LogPriority priority = SDL_LOG_PRIORITY_INFO;

			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
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
			nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 2, (const float[]){ 0.8f, 0.2f });
			nk_label(context, typed, NK_TEXT_LEFT);
			SDL_snprintf(text, sizeof(text), "%d lines up", (int)console->scroll_lines);
			nk_label_colored(context, text, NK_TEXT_RIGHT, dim);
		}
		else
		{
			nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
			nk_label(context, typed, NK_TEXT_LEFT);
		}
	}
	nk_end(context);
	nk_style_pop_style_item(context);
}

void ui_update(ui_t *ui, const ui_view_t *view, double time_seconds)
{
	SDL_zero(ui->requests);
	if (!ui->available)
	{
		return;
	}
	if (ui->hidden)
	{
		// F10 hides the interface, not the console
		console_window(ui, view);
		ui_backend_update_capture(&ui->backend);
		return;
	}
	brushes_window(ui, view);
	icon_bar_window(ui, view);
	status_window(ui);
	settings_window(ui, view);
	diagnostics_window(ui);
	sky_window(ui, view);
	world_window(ui, view);
	inspector_window(ui, view);
	maps_window(ui, view);
	message_window(ui, time_seconds);
	pick_window(ui, view);
	console_window(ui, view);
	ui_backend_update_capture(&ui->backend);
}

void ui_show_loading(ui_t *ui, int pixel_width, int pixel_height, const char *what, float fraction)
{
	struct nk_context *context = &ui->backend.context;
	float width = 420.0f;
	float height = 110.0f;
	nk_size done = (nk_size)(SDL_clamp(fraction, 0.0f, 1.0f) * 1000.0f);

	if (!ui->available)
	{
		return;
	}
	// (the input frame sizes it to the window)
	ui_backend_input_begin(&ui->backend);
	ui_backend_input_end(&ui->backend);
	if (nk_begin(context, "Loading", nk_rect(0.5f * (ui->backend.width - width), 0.5f * (ui->backend.height - height), width, height),
		NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT))
	{
		nk_layout_row_dynamic(context, 26.0f, 1);
		nk_label(context, "Potop", NK_TEXT_CENTERED);
		nk_label(context, what, NK_TEXT_CENTERED);
		nk_layout_row_dynamic(context, 18.0f, 1);
		nk_progress(context, &done, 1000, nk_false);
	}
	nk_end(context);
	ui_backend_render(&ui->backend, pixel_width, pixel_height);
}

void ui_render(ui_t *ui, int pixel_width, int pixel_height)
{
	if (!ui->available)
	{
		return;
	}
	if (ui->hidden && !ui->console_shown)
	{
		nk_clear(&ui->backend.context);
	}
	else
	{
		ui_backend_render(&ui->backend, pixel_width, pixel_height);
	}
	if (ui->scale != ui->backend.scale || ui->font_size != ui->backend.font->handle.height || ui->dialog_text_scale != ui->backend.large_scale)
	{
		// Apply in the interface tab: everything baked anew
		ui_rebuild(ui);
	}
}
