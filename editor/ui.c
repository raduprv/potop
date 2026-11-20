// The game's user interface. See ui.h.

#include "ui.h"

#include "ui_widgets.h"

#include "cloud_weather.h"
#include "file_io.h"
#include "ini_parser.h"
#include "season.h"
#include "stamps.h"

#define UI_STATE_FILE "ui_state.ini"

#define BRUSHES_WINDOW "Brushes"
#define ICON_BAR_WINDOW "icon_bar"
#define STATUS_WINDOW "status_line"
#define MESSAGE_WINDOW "message"
#define PICK_WINDOW "pick"
#define DIAGNOSTICS_WINDOW "Diagnostics"
#define INSPECTOR_WINDOW "Inspector"
#define MAPS_WINDOW "Maps"
#define SKY_WINDOW "Sky"
#define WORLD_WINDOW "World"
#define SKETCH_WINDOW "Sketch"

#define BRUSHES_WIDTH 276.0f
#define ROW_HEIGHT 26.0f
#define SMALL_ROW_HEIGHT 20.0f

// ---------------------------------------------------------------------------
// Configuration: ui.ini, then ui_state.ini

static bool config_key(void *user_data, const char *section, const char *key, const char *value)
{
	ui_t *ui = user_data;

	if (ui_style_key(&ui->style, section, key, value))
	{
		return true;
	}
	if (SDL_strcmp(section, "ui") == 0)
	{
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
		return read_rect(value, &ui->settings.bounds);
	}
	if (SDL_strcmp(key, "settings_tab") == 0)
	{
		return ini_value_to_int(value, &ui->settings.tab);
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
	const struct nk_rect *settings = &ui->settings.bounds;
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
		(double)ui->style.scale, (double)ui->style.font_size, (double)ui->style.dialog_text_scale, (double)ui->style.panel_opacity, ui->show_status_line ? "true" : "false",
		(double)brushes->x, (double)brushes->y, (double)brushes->w, (double)brushes->h, ui->brushes_collapsed ? "true" : "false",
		(double)settings->x, (double)settings->y, (double)settings->w, (double)settings->h, ui->settings.tab,
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
	ui_style_apply(&ui->style, &ui->backend.context);
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
	ui->available = ui_backend_create(&ui->backend, ui->window, ui->style.font_path, ui->style.font_size, ui->style.dialog_text_scale, ui->style.scale);
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
	ui_style_defaults(&ui->style);
	ui->show_status_line = true;
	ui->icon_labels = true;
	ui->brushes_bounds = nk_rect(12.0f, 12.0f, BRUSHES_WIDTH, 400.0f);
	ui->settings.bounds = nk_rect(320.0f, 60.0f, 520.0f, 560.0f);
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
	ui_style_clamp(&ui->style);
	settings_window_init(&ui->settings, &ui->style);
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
		&& (ui->settings.open || ui->diagnostics_open || ui->sky_open || ui->world_open || ui->maps_mode != UI_MAPS_CLOSED);

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
		else if (ui->settings.open)
		{
			ui->settings.open = false;
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
	ui_widget_tooltip(context, tooltip);
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
	ui_widget_section(&ui->backend.context, text);
}

// label | slider | value; returns true if the value changed
static bool slider_row(ui_t *ui, const char *label, float *value, float minimum, float maximum, float step, const char *format,
	const char *tooltip)
{
	return ui_widget_slider(&ui->backend.context, label, value, minimum, maximum, step, format, tooltip);
}

static bool checkbox_row(ui_t *ui, const char *label, bool *value, const char *tooltip)
{
	return ui_widget_checkbox(&ui->backend.context, label, value, tooltip);
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
	tools->area_dragging = false;
}

// The area tool's panel: the area picked, all of it to change
static void area_options(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	tool_state_t *tools = view->tools;
	int index = view->areas != NULL ? areas_find(view->areas, tools->area_selected) : -1;
	area_t *area;
	char text[64];
	float width;
	float depth;
	float turn_degrees;
	nk_bool player;
	nk_bool actors;

	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
	nk_label(context, "Drag: a new area; click in one: pick it", NK_TEXT_LEFT);
	SDL_snprintf(text, sizeof(text), "On the map: %d", view->areas != NULL ? view->areas->count : 0);
	nk_label(context, text, NK_TEXT_LEFT);
	if (index < 0)
	{
		return;
	}
	area = &view->areas->areas[index];
	SDL_snprintf(text, sizeof(text), "Id: %u", area->id);
	nk_label(context, text, NK_TEXT_LEFT);
	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT + 4.0f, 1);
	nk_edit_string_zero_terminated(context, NK_EDIT_FIELD, area->name, sizeof(area->name), nk_filter_default);
	player = (area->triggers & AREA_TRIGGER_PLAYER) != 0;
	actors = (area->triggers & AREA_TRIGGER_ACTORS) != 0;
	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 2);
	nk_checkbox_label(context, "Player", &player);
	nk_checkbox_label(context, "Actors", &actors);
	area->triggers = (uint8_t)((area->triggers & ~(AREA_TRIGGER_PLAYER | AREA_TRIGGER_ACTORS)) | (player ? AREA_TRIGGER_PLAYER : 0)
		| (actors ? AREA_TRIGGER_ACTORS : 0));
	// The paths it blocks: the actors' path groups (actors.ini), four of them
	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
	nk_label(context, "Blocks the paths of groups:", NK_TEXT_LEFT);
	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 4);
	for (int group = 0; group < 4; group++)
	{
		char label[8];
		nk_bool blocked = (area->blocks & (1u << group)) != 0;

		SDL_snprintf(label, sizeof(label), "%d", group + 1);
		if (nk_checkbox_label(context, label, &blocked))
		{
			area->blocks = (uint8_t)(blocked ? (area->blocks | (1u << group)) : (area->blocks & ~(1u << group)));
		}
	}
	width = 2.0f * area->half_width_meters;
	depth = 2.0f * area->half_depth_meters;
	turn_degrees = area->yaw_radians * 180.0f / MATH3D_PI;
	if (slider_row(ui, "Width", &width, 1.0f, 500.0f, 0.5f, "%.1f m", "Along its own x"))
	{
		area->half_width_meters = 0.5f * width;
	}
	if (slider_row(ui, "Depth", &depth, 1.0f, 500.0f, 0.5f, "%.1f m", "Along its own z"))
	{
		area->half_depth_meters = 0.5f * depth;
	}
	if (slider_row(ui, "Turn", &turn_degrees, -180.0f, 180.0f, 1.0f, "%.0f deg", "Turned round its middle"))
	{
		area->yaw_radians = turn_degrees * MATH3D_PI / 180.0f;
	}
	slider_row(ui, "Bottom", &area->bottom_meters, -50.0f, 500.0f, 0.5f, "%.1f m", "How low it goes (only with a top above it)");
	slider_row(ui, "Top", &area->top_meters, -50.0f, 500.0f, 0.5f, "%.1f m", "How high it goes; at or under the bottom: from the ground to the sky");
	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT + 4.0f, 1);
	if (nk_button_label(context, "Remove the area"))
	{
		areas_remove(view->areas, area->id);
		tools->area_selected = 0;
	}
}

// The areas on the ground, while the area tool is chosen: their outlines
// (the picked one bright, the ones for no one grey), their names
static void area_overlay(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	const tool_state_t *tools = view->tools;
	const float *m = view->view_projection.m;
	bool shown = tools->current_tool == TOOL_AREA && view->areas != NULL && view->terrain != NULL;

	if (!shown)
	{
		nk_window_show(context, "area_overlay", NK_HIDDEN);
		return;
	}
	nk_window_show(context, "area_overlay", NK_SHOWN);
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
	if (nk_begin(context, "area_overlay", nk_rect(0.0f, 0.0f, ui->backend.width, ui->backend.height),
		NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND))
	{
		struct nk_command_buffer *canvas = nk_window_get_canvas(context);
		int count = view->areas->count + (tools->area_dragging && tools->hover_valid ? 1 : 0);

		nk_window_set_bounds(context, "area_overlay", nk_rect(0.0f, 0.0f, ui->backend.width, ui->backend.height));
		for (int i = 0; i < count; i++)
		{
			area_t drawn;
			float corners[4][2];
			struct nk_color color;
			float label_x = 0.0f;
			float label_y = 0.0f;
			bool label_seen = false;

			if (i < view->areas->count)
			{
				drawn = view->areas->areas[i];
			}
			else
			{
				// The one being drawn
				SDL_zero(drawn);
				drawn.x = 0.5f * (tools->hover_point.x + tools->area_drag_start.x);
				drawn.z = 0.5f * (tools->hover_point.z + tools->area_drag_start.z);
				drawn.half_width_meters = 0.5f * SDL_fabsf(tools->hover_point.x - tools->area_drag_start.x);
				drawn.half_depth_meters = 0.5f * SDL_fabsf(tools->hover_point.z - tools->area_drag_start.z);
				drawn.triggers = AREA_TRIGGER_PLAYER;
			}
			color = drawn.id != 0 && drawn.id == tools->area_selected ? nk_rgba(255, 220, 60, 255)
				: (drawn.blocks != 0 ? nk_rgba(255, 90, 70, 230) : (drawn.triggers == 0 ? nk_rgba(160, 160, 160, 200) : nk_rgba(80, 220, 255, 220)));
			areas_corners(&drawn, corners);
			for (int edge = 0; edge < 4; edge++)
			{
				const float *from = corners[edge];
				const float *to = corners[(edge + 1) % 4];
				float length = SDL_sqrtf((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]));
				int pieces = SDL_clamp((int)(length / 2.0f) + 1, 1, 400);
				bool previous_seen = false;
				float previous[2] = { 0.0f, 0.0f };

				// Along the ground, a piece every 2 m or so
				for (int piece = 0; piece <= pieces; piece++)
				{
					float t = (float)piece / (float)pieces;
					float x = from[0] + (to[0] - from[0]) * t;
					float z = from[1] + (to[1] - from[1]) * t;
					float y = terrain_sample_height_meters(view->terrain, x, z) + 0.3f;
					float clip_x = m[0] * x + m[4] * y + m[8] * z + m[12];
					float clip_y = m[1] * x + m[5] * y + m[9] * z + m[13];
					float clip_w = m[3] * x + m[7] * y + m[11] * z + m[15];
					bool seen = clip_w > 0.1f;
					float screen[2];

					screen[0] = seen ? (clip_x / clip_w * 0.5f + 0.5f) * ui->backend.width : 0.0f;
					screen[1] = seen ? (0.5f - clip_y / clip_w * 0.5f) * ui->backend.height : 0.0f;
					if (seen && previous_seen)
					{
						nk_stroke_line(canvas, previous[0], previous[1], screen[0], screen[1], 2.0f, color);
					}
					previous_seen = seen;
					previous[0] = screen[0];
					previous[1] = screen[1];
				}
			}
			{
				float y = terrain_sample_height_meters(view->terrain, drawn.x, drawn.z) + 1.5f;
				float clip_x = m[0] * drawn.x + m[4] * y + m[8] * drawn.z + m[12];
				float clip_y = m[1] * drawn.x + m[5] * y + m[9] * drawn.z + m[13];
				float clip_w = m[3] * drawn.x + m[7] * y + m[11] * drawn.z + m[15];

				label_seen = clip_w > 0.1f && drawn.name[0] != '\0';
				label_x = (clip_x / SDL_max(clip_w, 0.1f) * 0.5f + 0.5f) * ui->backend.width;
				label_y = (0.5f - clip_y / SDL_max(clip_w, 0.1f) * 0.5f) * ui->backend.height;
			}
			if (label_seen)
			{
				const struct nk_user_font *font = context->style.font;
				float text_width = font->width(font->userdata, font->height, drawn.name, (int)SDL_strlen(drawn.name));

				nk_draw_text(canvas, nk_rect(label_x - 0.5f * text_width, label_y - 0.5f * font->height, text_width + 4.0f, font->height + 2.0f),
					drawn.name, (int)SDL_strlen(drawn.name), font, nk_rgba(0, 0, 0, 0), color);
			}
		}
	}
	nk_end(context);
	nk_style_pop_style_item(context);
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
	{
		nk_bool awake = tools->object_awake;

		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		nk_checkbox_label(context, "Loose (settles, rolls)", &awake);
		tools->object_awake = awake;
	}
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
		&& tool != TOOL_LIGHT && tool != TOOL_AREA && tool != TOOL_NONE)
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
		case TOOL_AREA:
			area_options(ui, view);
			break;
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
	struct nk_color background = ui->style.colors[NK_COLOR_WINDOW];
	struct nk_color header = ui->style.colors[NK_COLOR_HEADER];
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE;
	float content_bottom = 0.0f;

	if (!hovered)
	{
		background.a = (nk_byte)((float)background.a * ui->style.panel_opacity);
		header.a = (nk_byte)((float)header.a * SDL_min(1.0f, ui->style.panel_opacity + 0.2f));
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
		if (tool_button(ui, "area", "Area", "Areas: who comes in and goes out tells the game; kept with the map", tool == TOOL_AREA))
		{
			select_tool(tools, TOOL_AREA);
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
		{ "save", "Save", "Save or bake the project (F5: save it at once)" },
		{ "load", "Open", "Open a project, or bring in a saved map (F9: the project as last saved)" },
		{ "new_map", "New", "A new map: its land sketched (or left to the seed), made into a project (N: a new map at once, outside any project)" },
		{ view->paused ? "play" : "pause", view->paused ? "Run" : "Pause", "Pause / run the simulation (space)" },
		{ "slower", "\xc2\xab", "Slower (,)" },
		{ NULL, speed_text, "Simulation speed" },
		{ "faster", "\xc2\xbb", "Faster (.)" },
		{ "sky", "Sky", "Paint the clouds on their map" },
		{ "sketch", "Sketch", "The sketch again: paint the land some more and make the map again" },
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
						ui->settings.open = !ui->settings.open;
						break;
					case 3:
						// A new map: the sketch, its fields afresh (a new name,
						// a new seed; the open project's sketch is its own)
						ui_open_new_map(ui, view);
						break;
					case 1:
					case 2:
					{
						ui_maps_mode_t mode = i == 1 ? UI_MAPS_SAVE : UI_MAPS_LOAD;

						if (ui->maps_mode == mode)
						{
							ui->maps_mode = UI_MAPS_CLOSED;
							break;
						}
						ui->maps_mode = mode;
						ui->map_entry_count = map_file_list(ui->map_entries, MAP_FILE_LIST_MAX);
						ui->project_count = project_list(ui->projects, PROJECT_LIST_MAX);
						ui->selected_map = -1;
						ui->selected_project = -1;
						ui->confirm_delete = false;
						ui->confirm_project_delete = false;
						ui->project_name_input[0] = '\0';
						if (mode == UI_MAPS_SAVE && view->project_name[0] == '\0')
						{
							SDL_snprintf(ui->project_name_input, sizeof(ui->project_name_input), "map %u", view->seed);
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
						ui->sketch_open = true;
						break;
					case 10:
						ui->world_open = !ui->world_open;
						break;
					case 11:
						ui->diagnostics_open = !ui->diagnostics_open;
						break;
					case 12:
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
	struct nk_color background = ui->style.colors[NK_COLOR_WINDOW];

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

// The status line, under the settings' interface tab
static void interface_extra(void *user, struct nk_context *context)
{
	ui_t *ui = user;

	ui_widget_checkbox(context, "Status line", &ui->show_status_line, NULL);
	ui_widget_section(context, "F10 hides the interface; the keys keep working.");
}

// The settings (settings_window.h: every row), and what they ask
static void settings_window(ui_t *ui, const ui_view_t *view)
{
	settings_window_view_t settings_view;
	const settings_window_requests_t *asked = &ui->settings.requests;

	SDL_zero(settings_view);
	settings_view.editor = true;
	settings_view.depth_scaled_pipes = view->water->depth_scaled_pipes;
	settings_view.seed = view->seed;
	settings_view.interface_extra = interface_extra;
	settings_view.user = ui;
	settings_window_draw(&ui->settings, &ui->backend, &ui->style, view->settings, &settings_view);
	ui->requests.apply_render = ui->requests.apply_render || asked->apply_render;
	ui->requests.apply_objects = ui->requests.apply_objects || asked->apply_objects;
	ui->requests.regrow = ui->requests.regrow || asked->regrow;
	ui->requests.remove_objects = ui->requests.remove_objects || asked->remove_objects;
	ui->requests.save_settings = ui->requests.save_settings || asked->save_settings;
	ui->requests.restore_settings = ui->requests.restore_settings || asked->restore_settings;
	if (asked->new_map)
	{
		ui->requests.new_map = true;
		ui->requests.new_map_seed = asked->new_map_seed;
	}
	if (asked->toggle_water_model)
	{
		view->input->toggle_water_model = true;
	}
	if (asked->bake_sky_occlusion)
	{
		view->input->bake_sky_occlusion = true;
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
				// (brought in as a project: by its name, the kind left off)
				SDL_strlcpy(ui->project_name_input, entry->shown_name, sizeof(ui->project_name_input));
			}
			nk_label_colored(context, entry->baked ? "baked" : "active", NK_TEXT_LEFT,
				entry->baked ? nk_rgb(150, 200, 255) : nk_rgb(255, 210, 140));
			nk_label(context, date, NK_TEXT_LEFT);
			nk_label(context, size, NK_TEXT_RIGHT);
		}
		nk_group_end(context);
	}
}

// A name typed into a field (its own row, with a label)
static void sketch_fill_fields(ui_t *ui, const ui_view_t *view);

static nk_flags name_field(ui_t *ui, const char *label, char *text, int size)
{
	struct nk_context *context = &ui->backend.context;
	nk_flags state;

	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 2, (const float[]){ 0.3f, 0.7f });
	nk_label(context, label, NK_TEXT_LEFT);
	state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, text, size, nk_filter_default);
	if (state & NK_EDIT_ACTIVE)
	{
		ui->backend.wants_text_input = true;
	}
	return state;
}

// A project's name as typed, cleaned as the map files' are (no slashes,
// no leading dots)
static void clean_project_name(const char *typed, char *name_out, size_t name_out_size)
{
	char clean[MAP_FILE_NAME_LENGTH];

	map_file_clean_name(typed, clean, sizeof(clean));
	SDL_strlcpy(name_out, clean, name_out_size);
}

static bool project_listed(const ui_t *ui, const char *name)
{
	for (int i = 0; i < ui->project_count; i++)
	{
		if (SDL_strcmp(ui->projects[i].name, name) == 0)
		{
			return true;
		}
	}
	return false;
}

static void project_list_group(ui_t *ui, float height)
{
	struct nk_context *context = &ui->backend.context;

	nk_layout_row_dynamic(context, height, 1);
	if (nk_group_begin(context, "projects", NK_WINDOW_BORDER))
	{
		if (ui->project_count == 0)
		{
			nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
			nk_label(context, "No projects yet", NK_TEXT_CENTERED);
		}
		for (int i = 0; i < ui->project_count; i++)
		{
			const project_info_t *info = &ui->projects[i];
			char date[48];
			char size[48];
			nk_bool selected = ui->selected_project == i;

			format_date(info->modified_nanoseconds, date, sizeof(date));
			SDL_snprintf(size, sizeof(size), "%.1f km, %.1f MB", (double)((float)info->size_cells * info->cell_size_meters / 1000.0f),
				(double)info->map_bytes / (1024.0 * 1024.0));
			nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT - 4.0f, 3, (const float[]){ 0.36f, 0.24f, 0.40f });
			tooltip_if_hovered(context, size);
			if (nk_selectable_label(context, info->name, NK_TEXT_LEFT, &selected))
			{
				ui->selected_project = i;
				ui->confirm_project_delete = false;
			}
			tooltip_if_hovered(context, info->has_bake ? "Baked for play too (Save, Bake)" : "Not baked for play yet");
			nk_label_colored(context, info->baked ? "baked" : (info->has_bake ? "active +bake" : "active"), NK_TEXT_LEFT,
				info->baked ? nk_rgb(150, 200, 255) : nk_rgb(255, 210, 140));
			nk_label(context, date, NK_TEXT_RIGHT);
		}
		nk_group_end(context);
	}
}

// New: a name and a seed (empty: a random one); the map made as the
// settings say (their size), saved as the project's
static void new_project_controls(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	char clean[PROJECT_NAME_LENGTH];
	nk_flags name_state = name_field(ui, "Name", ui->project_name_input, sizeof(ui->project_name_input));
	nk_flags seed_state;
	bool taken;

	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 3, (const float[]){ 0.3f, 0.4f, 0.3f });
	tooltip_if_hovered(context, "Empty: a random seed");
	nk_label(context, "Seed", NK_TEXT_LEFT);
	seed_state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, ui->seed_input, sizeof(ui->seed_input),
		nk_filter_decimal);
	if (seed_state & NK_EDIT_ACTIVE)
	{
		ui->backend.wants_text_input = true;
	}
	if (nk_button_label(context, "This map's"))
	{
		SDL_snprintf(ui->seed_input, sizeof(ui->seed_input), "%u", view->seed);
	}
	clean_project_name(ui->project_name_input, clean, sizeof(clean));
	taken = project_listed(ui, clean);
	nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
	nk_label(context, clean[0] == '\0' ? "It needs a name." : (taken ? "There is a project of that name." : "The map as Settings, New map say."),
		NK_TEXT_LEFT);
	nk_layout_row_dynamic(context, ROW_HEIGHT + 2.0f, 2);
	if (clean[0] == '\0' || taken)
	{
		nk_widget_disable_begin(context);
	}
	if ((nk_button_label(context, "Make the project") || ((name_state | seed_state) & NK_EDIT_COMMITED)) && clean[0] != '\0' && !taken)
	{
		ui->requests.project_new = true;
		ui->requests.project_seed = (uint32_t)SDL_strtoul(ui->seed_input, NULL, 10);
		SDL_strlcpy(ui->requests.project_name, clean, sizeof(ui->requests.project_name));
		ui->maps_mode = UI_MAPS_CLOSED;
	}
	if (clean[0] == '\0' || taken)
	{
		nk_widget_disable_end(context);
	}
	if (nk_button_label(context, "Cancel"))
	{
		ui->maps_mode = UI_MAPS_CLOSED;
	}
	nk_layout_row_dynamic(context, ROW_HEIGHT + 2.0f, 1);
	tooltip_if_hovered(context, "Paint its land roughly first (mountains, plains, lakes), then make it");
	if (nk_button_label(context, "Sketch it first..."))
	{
		if (!ui->sketch_known)
		{
			sketch_fill_fields(ui, view);
		}
		if (clean[0] != '\0')
		{
			SDL_strlcpy(ui->sketch_name_input, clean, sizeof(ui->sketch_name_input));
		}
		if (ui->seed_input[0] != '\0')
		{
			SDL_strlcpy(ui->sketch_seed_input, ui->seed_input, sizeof(ui->sketch_seed_input));
		}
		ui->sketch_open = true;
		ui->maps_mode = UI_MAPS_CLOSED;
	}
}

// Save: the project open saved (or baked); or saved as a new project
static void save_project_controls(ui_t *ui, const ui_view_t *view, float height)
{
	struct nk_context *context = &ui->backend.context;
	char clean[PROJECT_NAME_LENGTH];
	char line[160];
	nk_flags state;
	bool taken;

	nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
	if (view->project_name[0] != '\0')
	{
		SDL_snprintf(line, sizeof(line), "Project \"%s\"%s", view->project_name, view->map_baked ? " (a baked map: it saves baked)" : "");
		nk_label(context, line, NK_TEXT_LEFT);
		nk_layout_row_dynamic(context, ROW_HEIGHT + 2.0f, 2);
		tooltip_if_hovered(context, "The map being made, into the project (F5)");
		if (nk_button_label(context, "Save"))
		{
			ui->requests.project_save = true;
			ui->maps_mode = UI_MAPS_CLOSED;
		}
		tooltip_if_hovered(context, "A baked copy for play: into the project, and into the saves for the game");
		if (nk_button_label(context, "Bake (for play)"))
		{
			ui->requests.project_bake = true;
			ui->maps_mode = UI_MAPS_CLOSED;
		}
		section_label(ui, "Or as a new project:");
	}
	else
	{
		nk_label(context, "No project open: save it as one.", NK_TEXT_LEFT);
	}
	state = name_field(ui, "Name", ui->project_name_input, sizeof(ui->project_name_input));
	clean_project_name(ui->project_name_input, clean, sizeof(clean));
	taken = project_listed(ui, clean);
	project_list_group(ui, height - (view->project_name[0] != '\0' ? 6.0f : 4.0f) * (ROW_HEIGHT + 4.0f) - 56.0f);
	if (ui->selected_project >= 0 && ui->selected_project < ui->project_count)
	{
		// (a project picked from the list: its name, to see it is taken)
		SDL_strlcpy(ui->project_name_input, ui->projects[ui->selected_project].name, sizeof(ui->project_name_input));
		ui->selected_project = -1;
	}
	nk_layout_row_dynamic(context, ROW_HEIGHT, 2);
	if (clean[0] == '\0' || taken)
	{
		nk_widget_disable_begin(context);
	}
	if ((nk_button_label(context, taken ? "That name is taken" : "Save as a new project") || (state & NK_EDIT_COMMITED)) && clean[0] != '\0'
		&& !taken)
	{
		ui->requests.project_save_as = true;
		SDL_strlcpy(ui->requests.project_name, clean, sizeof(ui->requests.project_name));
		ui->maps_mode = UI_MAPS_CLOSED;
	}
	if (clean[0] == '\0' || taken)
	{
		nk_widget_disable_end(context);
	}
	if (nk_button_label(context, "Cancel"))
	{
		ui->maps_mode = UI_MAPS_CLOSED;
	}
}

// Open: a project; or a map saved before projects, brought in as one
static void open_project_controls(ui_t *ui, float height)
{
	struct nk_context *context = &ui->backend.context;
	bool has_selection = ui->selected_project >= 0 && ui->selected_project < ui->project_count;
	bool has_map = ui->selected_map >= 0 && ui->selected_map < ui->map_entry_count;
	float list_height = (height - 6.0f * (ROW_HEIGHT + 4.0f) - 56.0f) * 0.5f;
	char clean[PROJECT_NAME_LENGTH];
	bool taken;

	project_list_group(ui, list_height);
	if (ui->confirm_project_delete && has_selection)
	{
		char question[128];

		SDL_snprintf(question, sizeof(question), "Delete \"%s\" and all in it?", ui->projects[ui->selected_project].name);
		nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
		nk_label(context, question, NK_TEXT_LEFT);
		if (nk_button_label(context, "Delete"))
		{
			project_delete(ui->projects[ui->selected_project].name);
			ui->project_count = project_list(ui->projects, PROJECT_LIST_MAX);
			ui->selected_project = -1;
			ui->confirm_project_delete = false;
		}
		if (nk_button_label(context, "Keep"))
		{
			ui->confirm_project_delete = false;
		}
	}
	else
	{
		nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
		if (!has_selection)
		{
			nk_widget_disable_begin(context);
		}
		if (nk_button_label(context, "Open") && has_selection)
		{
			ui->requests.project_open = true;
			SDL_strlcpy(ui->requests.project_name, ui->projects[ui->selected_project].name, sizeof(ui->requests.project_name));
			ui->maps_mode = UI_MAPS_CLOSED;
		}
		if (nk_button_label(context, "Delete") && has_selection)
		{
			ui->confirm_project_delete = true;
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
	section_label(ui, "Maps saved before projects (or by the game):");
	map_list(ui, list_height);
	name_field(ui, "As project", ui->project_name_input, sizeof(ui->project_name_input));
	clean_project_name(ui->project_name_input, clean, sizeof(clean));
	taken = project_listed(ui, clean);
	nk_layout_row_dynamic(context, ROW_HEIGHT, 2);
	if (!has_map || clean[0] == '\0' || taken)
	{
		nk_widget_disable_begin(context);
	}
	if (nk_button_label(context, taken ? "That name is taken" : "Bring it in as a project") && has_map && clean[0] != '\0' && !taken)
	{
		ui->requests.project_import = true;
		SDL_strlcpy(ui->requests.import_map, ui->map_entries[ui->selected_map].name, sizeof(ui->requests.import_map));
		SDL_strlcpy(ui->requests.project_name, clean, sizeof(ui->requests.project_name));
		ui->maps_mode = UI_MAPS_CLOSED;
	}
	if (!has_map || clean[0] == '\0' || taken)
	{
		nk_widget_disable_end(context);
	}
	tooltip_if_hovered(context, "Load it as it is, outside any project");
	if (!has_map)
	{
		nk_widget_disable_begin(context);
	}
	if (nk_button_label(context, "Just load it") && has_map)
	{
		ui->requests.load_map = true;
		SDL_strlcpy(ui->requests.map_name, ui->map_entries[ui->selected_map].name, sizeof(ui->requests.map_name));
		ui->maps_mode = UI_MAPS_CLOSED;
	}
	if (!has_map)
	{
		nk_widget_disable_end(context);
	}
}

// The projects' window: new, save (or bake), open (see project.h)
static void maps_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR;
	const char *title = ui->maps_mode == UI_MAPS_SAVE ? "Save" : (ui->maps_mode == UI_MAPS_LOAD ? "Open a project" : "New project");
	float height = ui->maps_mode == UI_MAPS_NEW ? 226.0f : (ui->maps_mode == UI_MAPS_SAVE ? 380.0f : 440.0f);

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
			new_project_controls(ui, view);
		}
		else if (ui->maps_mode == UI_MAPS_SAVE)
		{
			save_project_controls(ui, view, height);
		}
		else
		{
			open_project_controls(ui, height);
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
// Sketch: a new map's land painted roughly, over the whole screen

static const int32_t sketch_sizes[4] = { 1024, 2048, 4096, 8192 };

int32_t ui_sketch_size(const ui_t *ui)
{
	return sketch_sizes[SDL_clamp(ui->sketch_size_index, 0, 3)];
}

uint32_t ui_sketch_seed(const ui_t *ui)
{
	return (uint32_t)SDL_strtoul(ui->sketch_seed_input, NULL, 10);
}

// Its fields from the map and the settings, when first opened
static void sketch_fill_fields(ui_t *ui, const ui_view_t *view)
{
	const terrain_settings_t *terrain = &view->settings->terrain;
	int32_t size = view->terrain != NULL && view->terrain->size > 0 ? view->terrain->size : terrain->size;

	ui->sketch_size_index = 2;
	for (int i = 0; i < 4; i++)
	{
		if (sketch_sizes[i] >= size)
		{
			ui->sketch_size_index = i;
			break;
		}
	}
	ui->sketch_cell_meters = view->terrain != NULL && view->terrain->size > 0 ? view->terrain->cell_size_meters : terrain->cell_size_meters;
	ui->sketch_max_height = terrain->max_height_meters;
	ui->sketch_erosion = terrain->erosion_droplets;
	SDL_snprintf(ui->sketch_seed_input, sizeof(ui->sketch_seed_input), "%u", view->seed);
	if (view->project_name[0] != '\0')
	{
		SDL_strlcpy(ui->sketch_name_input, view->project_name, sizeof(ui->sketch_name_input));
	}
	else
	{
		SDL_snprintf(ui->sketch_name_input, sizeof(ui->sketch_name_input), "map %u", view->seed);
	}
	ui->sketch_brush = 0;
	ui->sketch_brush_meters = SDL_max((float)ui_sketch_size(ui) * ui->sketch_cell_meters * 0.06f, 20.0f);
	ui->project_count = project_list(ui->projects, PROJECT_LIST_MAX);
	ui->sketch_known = true;
}

void ui_open_new_map(ui_t *ui, const ui_view_t *view)
{
	char name[PROJECT_NAME_LENGTH];

	sketch_fill_fields(ui, view);
	SDL_snprintf(ui->sketch_seed_input, sizeof(ui->sketch_seed_input), "%u", (uint32_t)SDL_rand_bits() % 1000000u + 1u);
	for (int number = 1; number < 10000; number++)
	{
		SDL_snprintf(name, sizeof(name), "map %d", number);
		if (!project_listed(ui, name))
		{
			break;
		}
	}
	SDL_strlcpy(ui->sketch_name_input, name, sizeof(ui->sketch_name_input));
	// (a blank sheet, the picture showing it)
	if (view->sketch != NULL)
	{
		terrain_sketch_clear(view->sketch);
	}
	if (view->sketch_view != NULL)
	{
		view->sketch_view->show_result = false;
	}
	ui->sketch_confirm_replace = false;
	ui->maps_mode = UI_MAPS_CLOSED;
	ui->sketch_open = true;
}

// The region the brush chosen paints (erase: none)
static terrain_sketch_region_t sketch_region_of(const ui_t *ui)
{
	return ui->sketch_brush < SKETCH_PRESET_COUNT ? sketch_presets[ui->sketch_brush].region : TERRAIN_SKETCH_NONE;
}

// The tools, at the left
static void sketch_tools(ui_t *ui, const ui_view_t *view, float map_meters)
{
	struct nk_context *context = &ui->backend.context;
	static const char *const size_names[4] = { "1024", "2048", "4096", "8192" };
	char clean[PROJECT_NAME_LENGTH];
	char line[160];
	bool taken;
	bool replaces_open;
	nk_flags state;

	section_label(ui, "The map");
	name_field(ui, "Project", ui->sketch_name_input, sizeof(ui->sketch_name_input));
	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 3, (const float[]){ 0.3f, 0.4f, 0.3f });
	nk_label(context, "Seed", NK_TEXT_LEFT);
	state = nk_edit_string_zero_terminated(context, NK_EDIT_FIELD, ui->sketch_seed_input, sizeof(ui->sketch_seed_input), nk_filter_decimal);
	if (state & NK_EDIT_ACTIVE)
	{
		ui->backend.wants_text_input = true;
	}
	tooltip_if_hovered(context, "Another seed: the land's detail over what you paint (nothing painted: all of the land)");
	if (nk_button_label(context, "Random"))
	{
		SDL_snprintf(ui->sketch_seed_input, sizeof(ui->sketch_seed_input), "%u", (uint32_t)SDL_rand_bits() % 1000000u + 1u);
	}
	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 2, (const float[]){ 0.3f, 0.7f });
	nk_label(context, "Size", NK_TEXT_LEFT);
	{
		int old_size = ui_sketch_size(ui);

		ui->sketch_size_index = nk_combo(context, size_names, 4, SDL_clamp(ui->sketch_size_index, 0, 3), (int)ROW_HEIGHT, nk_vec2(200.0f, 160.0f));
		// (the brush the same share of the map)
		ui->sketch_brush_meters *= (float)ui_sketch_size(ui) / (float)old_size;
	}
	slider_row(ui, "Cell", &ui->sketch_cell_meters, 0.25f, 4.0f, 0.25f, "%.2f m", "How big a cell of the map is");
	slider_row(ui, "Highest", &ui->sketch_max_height, 20.0f, 1500.0f, 10.0f, "%.0f m", "How high the highest ground may be");
	slider_row(ui, "Erosion", &ui->sketch_erosion, 0.0f, 4.0f, 0.1f, "%.1f", "How much the rain wears the land when it is made (0: none)");
	nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
	SDL_snprintf(line, sizeof(line), "%.1f x %.1f km", (double)(map_meters * 0.001f), (double)(map_meters * 0.001f));
	nk_label(context, line, NK_TEXT_LEFT);

	section_label(ui, "Brushes (left: paint, right: erase)");
	// (each a swatch of its color on the sheet, then its name)
	nk_layout_row(context, NK_DYNAMIC, ROW_HEIGHT, 4, (const float[]){ 0.08f, 0.42f, 0.08f, 0.42f });
	for (int i = 0; i <= SKETCH_PRESET_COUNT; i++)
	{
		const char *name = i < SKETCH_PRESET_COUNT ? sketch_presets[i].name : "Erase";
		const char *tooltip = i < SKETCH_PRESET_COUNT ? sketch_presets[i].tooltip : "Back to the blank sheet";
		const uint8_t *color = sketch_region_color(i < SKETCH_PRESET_COUNT ? sketch_presets[i].region : TERRAIN_SKETCH_NONE);
		nk_bool selected = ui->sketch_brush == i;

		if (nk_button_color(context, nk_rgb(color[0], color[1], color[2])))
		{
			ui->sketch_brush = i;
		}
		tooltip_if_hovered(context, tooltip);
		if (nk_selectable_label(context, name, NK_TEXT_CENTERED, &selected))
		{
			ui->sketch_brush = i;
		}
	}
	slider_row(ui, "Brush", &ui->sketch_brush_meters, 10.0f, SDL_max(map_meters * 0.3f, 20.0f), 10.0f, "%.0f m", "How far the brush reaches");
	if (view->sketch_view != NULL)
	{
		nk_layout_row_dynamic(context, ROW_HEIGHT, 2);
		tooltip_if_hovered(context, "What you painted, on its blank sheet");
		if (nk_option_label(context, "The sketch", !view->sketch_view->show_result))
		{
			view->sketch_view->show_result = false;
		}
		tooltip_if_hovered(context, "The land the generator makes from it (worked out small; the rain's erosion once you stop)");
		if (nk_option_label(context, "What it makes", view->sketch_view->show_result))
		{
			view->sketch_view->show_result = true;
		}
	}

	section_label(ui, "Make it");
	clean_project_name(ui->sketch_name_input, clean, sizeof(clean));
	taken = project_listed(ui, clean);
	replaces_open = clean[0] != '\0' && SDL_strcmp(clean, view->project_name) == 0;
	nk_layout_row_dynamic(context, ROW_HEIGHT + 2.0f, 2);
	tooltip_if_hovered(context, "The land in 3D a quarter as big, quickly, outside any project");
	if (nk_button_label(context, "Preview"))
	{
		ui->requests.sketch_preview = true;
		ui->sketch_open = false;
	}
	if (clean[0] == '\0' || (taken && !ui->sketch_confirm_replace))
	{
		nk_widget_disable_begin(context);
	}
	tooltip_if_hovered(context, "The map at its size, into the project (its map replaced)");
	if (nk_button_label(context, "Make the map") && clean[0] != '\0' && (!taken || ui->sketch_confirm_replace))
	{
		ui->requests.sketch_generate = true;
		ui->sketch_open = false;
		ui->sketch_confirm_replace = false;
	}
	if (clean[0] == '\0' || (taken && !ui->sketch_confirm_replace))
	{
		nk_widget_disable_end(context);
	}
	if (taken)
	{
		nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
		SDL_snprintf(line, sizeof(line), replaces_open ? "Replace this project's map (everything on it)" : "Replace \"%s\"'s map", clean);
		checkbox_row(ui, line, &ui->sketch_confirm_replace, "Its map is made again from the sketch: what was done on it is lost");
	}
	else
	{
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		nk_label(context, clean[0] == '\0' ? "The project needs a name." : "A new project.", NK_TEXT_LEFT);
	}
	if (view->sketch_view != NULL && !view->sketch_view->painted)
	{
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		nk_label(context, "Nothing painted: the map is the seed's.", NK_TEXT_LEFT);
	}
	nk_layout_row_dynamic(context, ROW_HEIGHT, 2);
	if (nk_button_label(context, "Clear the sketch") && view->sketch != NULL)
	{
		terrain_sketch_clear(view->sketch);
	}
	if (nk_button_label(context, "Close"))
	{
		ui->sketch_open = false;
	}
}

static void sketch_window(ui_t *ui, const ui_view_t *view)
{
	struct nk_context *context = &ui->backend.context;
	// (no close box, nor nk_window_close: closing the active window while
	// another comes after it in Nuklear's list (the console, drawn after
	// it) cuts the list short and crashes it a frame later; closed, it is
	// simply not begun, and Nuklear lets it go cleanly)
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR;
	struct nk_rect bounds = nk_rect(0.0f, 0.0f, ui->backend.width, ui->backend.height);

	if (!ui->sketch_open || view->sketch == NULL)
	{
		return;
	}
	if (ui->sketch_new_pending)
	{
		ui->sketch_new_pending = false;
		ui_open_new_map(ui, view);
	}
	if (!ui->sketch_known)
	{
		sketch_fill_fields(ui, view);
	}
	if (nk_begin_titled(context, SKETCH_WINDOW, "Sketch: the new map's land", bounds, flags))
	{
		struct nk_rect region;
		float tools_width = SDL_min(340.0f, ui->backend.width * 0.4f);
		float side;
		float map_meters = (float)ui_sketch_size(ui) * ui->sketch_cell_meters;
		struct nk_rect picture_bounds;

		nk_window_set_bounds(context, SKETCH_WINDOW, bounds);
		region = nk_window_get_content_region(context);
		side = SDL_max(SDL_min(region.w - tools_width - 3.0f * context->style.window.spacing.x, region.h - 4.0f), 64.0f);
		nk_layout_row(context, NK_STATIC, region.h - 4.0f, 2, (const float[]){ tools_width, side });
		if (nk_group_begin(context, "sketch_tools", NK_WINDOW_NO_SCROLLBAR))
		{
			sketch_tools(ui, view, map_meters);
			nk_group_end(context);
		}
		picture_bounds = nk_widget_bounds(context);
		picture_bounds.h = side;
		if (view->sketch_view != NULL && view->sketch_view->texture != NULL)
		{
			nk_image(context, nk_image_ptr(view->sketch_view->texture));
		}
		else
		{
			nk_spacing(context, 1);
		}
		{
			struct nk_input *input = &context->input;
			bool paint = nk_input_is_mouse_down(input, NK_BUTTON_LEFT);
			bool erase = nk_input_is_mouse_down(input, NK_BUTTON_RIGHT);
			struct nk_rect square = nk_rect(picture_bounds.x, picture_bounds.y, side, side);
			// (not while a slider at the left is dragged over it)
			bool over = nk_input_is_mouse_hovering_rect(input, square) && !(context->last_widget_state & NK_WIDGET_STATE_MODIFIED);
			float u = (input->mouse.pos.x - square.x) / SDL_max(square.w, 1.0f);
			float v = (input->mouse.pos.y - square.y) / SDL_max(square.h, 1.0f);
			float radius = ui->sketch_brush_meters / SDL_max(map_meters, 1.0f);
			struct nk_command_buffer *canvas = nk_window_get_canvas(context);

			// A stroke: from where the mouse was the frame before, no gaps
			if (over && (paint || erase))
			{
				terrain_sketch_region_t region = erase ? TERRAIN_SKETCH_NONE : sketch_region_of(ui);

				if (ui->sketch_stroke)
				{
					terrain_sketch_paint_line(view->sketch, region, ui->sketch_last_u, ui->sketch_last_v, u, v, radius);
				}
				else
				{
					terrain_sketch_paint(view->sketch, region, u, v, radius);
				}
				ui->sketch_last_u = u;
				ui->sketch_last_v = v;
				ui->sketch_stroke = true;
			}
			else
			{
				ui->sketch_stroke = false;
			}
			if (over)
			{
				float reach = radius * square.w;

				nk_stroke_circle(canvas, nk_rect(input->mouse.pos.x - reach, input->mouse.pos.y - reach, reach * 2.0f, reach * 2.0f), 1.5f,
					nk_rgb(250, 250, 250));
				nk_stroke_circle(canvas, nk_rect(input->mouse.pos.x - reach - 1.0f, input->mouse.pos.y - reach - 1.0f, reach * 2.0f + 2.0f,
					reach * 2.0f + 2.0f), 1.0f, nk_rgb(20, 20, 20));
			}
		}
	}
	else
	{
		ui->sketch_open = false;
	}
	nk_end(context);
}

// ---------------------------------------------------------------------------

// The console across the top: the newest lines of the log over the line
// being typed (console.c edits it; this only draws it)
static void console_window(ui_t *ui, const ui_view_t *view)
{
	ui->console_shown = console_draw(view->console, &ui->backend, &ui->style);
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
	// (the sketch over everything: the rest waits while it is open)
	if (ui->sketch_open)
	{
		sketch_window(ui, view);
		message_window(ui, time_seconds);
		console_window(ui, view);
		ui_backend_update_capture(&ui->backend);
		return;
	}
	area_overlay(ui, view);
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
	if (ui->style.scale != ui->backend.scale || ui->style.font_size != ui->backend.font->handle.height || ui->style.dialog_text_scale != ui->backend.large_scale)
	{
		// Apply in the interface tab: everything baked anew
		ui_rebuild(ui);
	}
}
