// The game's interface on foot (hud.h): the compass, the notices, the
// conversation, the journal, the world map, the inventory and the loot,
// the prompt, the names over the actors.

#include "hud.h"

#include <SDL3/SDL.h>

#define INVENTORY_WINDOW "Inventory"
#define LOOT_WINDOW "Loot"
#define PROMPT_WINDOW "prompt"
#define COMPASS_WINDOW "compass"
#define MAP_WINDOW "Map"
#define JOURNAL_WINDOW "Journal"
#define NOTICES_WINDOW "notices"
#define DIALOG_WINDOW "dialog"
#define NAMES_WINDOW "names"
#define SHAPES_WINDOW "world shapes"
#define DEBUG_WINDOW "debug"

#define ROW_HEIGHT 26.0f
#define SMALL_ROW_HEIGHT 20.0f

void hud_init(hud_t *hud)
{
	SDL_zerop(hud);
	hud->inventory_bounds = nk_rect(120.0f, 90.0f, 620.0f, 440.0f);
	hud->loot_bounds = nk_rect(160.0f, 110.0f, 640.0f, 420.0f);
	hud->journal_bounds = nk_rect(100.0f, 80.0f, 720.0f, 460.0f);
	hud->inventory_selected = -1;
	hud->inventory_last_click_row = -1;
	hud->map_zoom = 1.0f;
	hud->requests.drop_item = -1;
}

bool hud_close_windows(hud_t *hud, bool shown)
{
	if (hud->dialog != NULL && hud->dialog->open)
	{
		dialog_cancel(hud->dialog);
		return true;
	}
	if (!shown)
	{
		return false;
	}
	// The topmost first: the map, the journal, the loot, the inventory
	if (hud->map_open)
	{
		hud->map_open = false;
		return true;
	}
	if (hud->journal_open)
	{
		hud->journal_open = false;
		return true;
	}
	if (hud->loot_container != 0)
	{
		hud->loot_container = 0;
		return true;
	}
	if (hud->inventory_open)
	{
		hud->inventory_open = false;
		return true;
	}
	return false;
}

static float text_width(hud_t *hud, const char *text)
{
	const struct nk_user_font *font = hud->backend->context.style.font;

	return font->width(font->userdata, font->height, text, nk_strlen(text));
}

// ---------------------------------------------------------------------------
// Inventory and loot

// A category's small sign, drawn (no image files): a rifle, rounds, a loaf,
// a pelt, a shirt, a tool, a star, a coin, a dot
static void category_sign(hud_t *hud, struct nk_rect bounds, item_category_t category)
{
	struct nk_context *context = &hud->backend->context;
	struct nk_command_buffer *canvas = nk_window_get_canvas(context);
	float size = SDL_min(bounds.w, bounds.h) - 4.0f;
	float x = bounds.x + 0.5f * (bounds.w - size);
	float y = bounds.y + 0.5f * (bounds.h - size);
	float u = size / 16.0f;
	struct nk_color steel = nk_rgb(170, 178, 188);
	struct nk_color brass = nk_rgb(214, 170, 72);
	struct nk_color bread = nk_rgb(205, 140, 70);
	struct nk_color pelt = nk_rgb(150, 105, 70);
	struct nk_color cloth = nk_rgb(120, 150, 210);
	struct nk_color quest = nk_rgb(240, 210, 90);
	struct nk_color green = nk_rgb(130, 190, 120);

	switch (category)
	{
		case ITEM_CATEGORY_WEAPON:
			nk_fill_rect(canvas, nk_rect(x, y + 6.0f * u, 16.0f * u, 3.0f * u), 0.0f, steel);
			nk_fill_rect(canvas, nk_rect(x + 9.0f * u, y + 9.0f * u, 2.5f * u, 5.0f * u), 0.0f, steel);
			nk_fill_rect(canvas, nk_rect(x, y + 7.0f * u, 4.0f * u, 5.0f * u), 0.0f, pelt);
			break;
		case ITEM_CATEGORY_AMMO:
			for (int i = 0; i < 3; i++)
			{
				float left = x + (2.0f + 4.5f * (float)i) * u;

				nk_fill_rect(canvas, nk_rect(left, y + 5.0f * u, 3.0f * u, 9.0f * u), 0.0f, brass);
				nk_fill_triangle(canvas, left, y + 5.0f * u, left + 3.0f * u, y + 5.0f * u, left + 1.5f * u, y + 1.5f * u, steel);
			}
			break;
		case ITEM_CATEGORY_FOOD:
			nk_fill_circle(canvas, nk_rect(x + 1.0f * u, y + 4.0f * u, 14.0f * u, 10.0f * u), bread);
			break;
		case ITEM_CATEGORY_MATERIAL:
			nk_fill_triangle(canvas, x + 1.0f * u, y + 14.0f * u, x + 8.0f * u, y + 2.0f * u, x + 15.0f * u, y + 14.0f * u, pelt);
			break;
		case ITEM_CATEGORY_CLOTHING:
			nk_fill_rect(canvas, nk_rect(x + 4.0f * u, y + 4.0f * u, 8.0f * u, 10.0f * u), 0.0f, cloth);
			nk_fill_rect(canvas, nk_rect(x + 1.0f * u, y + 4.0f * u, 14.0f * u, 4.0f * u), 0.0f, cloth);
			break;
		case ITEM_CATEGORY_TOOL:
			nk_fill_rect(canvas, nk_rect(x + 7.0f * u, y + 2.0f * u, 2.5f * u, 12.0f * u), 0.0f, pelt);
			nk_fill_rect(canvas, nk_rect(x + 3.0f * u, y + 2.0f * u, 10.0f * u, 3.0f * u), 0.0f, steel);
			break;
		case ITEM_CATEGORY_QUEST:
			nk_fill_triangle(canvas, x + 8.0f * u, y + 1.0f * u, x + 15.0f * u, y + 8.0f * u, x + 1.0f * u, y + 8.0f * u, quest);
			nk_fill_triangle(canvas, x + 8.0f * u, y + 15.0f * u, x + 15.0f * u, y + 8.0f * u, x + 1.0f * u, y + 8.0f * u, quest);
			break;
		case ITEM_CATEGORY_MONEY:
			nk_fill_circle(canvas, nk_rect(x + 3.0f * u, y + 3.0f * u, 10.0f * u, 10.0f * u), brass);
			break;
		case ITEM_CATEGORY_MISC:
		default:
			nk_fill_circle(canvas, nk_rect(x + 5.0f * u, y + 5.0f * u, 6.0f * u, 6.0f * u), green);
			break;
	}
}

// One row of a list: the sign, the name (and how many), the weight; true
// when clicked
static bool item_row(hud_t *hud, const inventories_t *inventories, const inventory_entry_t *entry, bool selected)
{
	struct nk_context *context = &hud->backend->context;
	const item_type_t *type = &inventories->items->types[entry->item];
	char name[96];
	char weight[32];
	struct nk_rect bounds;
	bool clicked;
	nk_bool chosen = selected;

	if (entry->count > 1)
	{
		SDL_snprintf(name, sizeof(name), "%s  x%d", type->shown_name, entry->count);
	}
	else
	{
		SDL_strlcpy(name, type->shown_name, sizeof(name));
	}
	SDL_snprintf(weight, sizeof(weight), "%.1f kg", type->weight_kilograms * (float)entry->count);
	nk_layout_row_template_begin(context, SMALL_ROW_HEIGHT + 2.0f);
	nk_layout_row_template_push_static(context, SMALL_ROW_HEIGHT + 2.0f);
	nk_layout_row_template_push_dynamic(context);
	nk_layout_row_template_push_static(context, 64.0f);
	nk_layout_row_template_end(context);
	if (nk_widget(&bounds, context) != NK_WIDGET_INVALID)
	{
		category_sign(hud, bounds, type->category);
	}
	clicked = nk_selectable_label(context, name, NK_TEXT_LEFT, &chosen);
	nk_label(context, weight, NK_TEXT_RIGHT);
	return clicked;
}

static void weight_title(const inventories_t *inventories, const container_t *container, const char *name, char *text, size_t size)
{
	float weight = inventory_weight(inventories, container->id);

	if (container->max_weight_kilograms > 0.0f)
	{
		SDL_snprintf(text, size, "%s   %.1f / %.0f kg", name, weight, container->max_weight_kilograms);
	}
	else
	{
		SDL_snprintf(text, size, "%s   %.1f kg", name, weight);
	}
}

static void inventory_window(hud_t *hud, const hud_view_t *view)
{
	struct nk_context *context = &hud->backend->context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR;
	container_t *container = view->inventories != NULL ? inventory_get(view->inventories, view->player_inventory) : NULL;
	const items_t *items;
	char title[96];

	if (!hud->inventory_open || container == NULL)
	{
		hud->inventory_open = false;
		return;
	}
	items = view->inventories->items;
	if (nk_window_is_hidden(context, INVENTORY_WINDOW) && nk_window_find(context, INVENTORY_WINDOW) != NULL)
	{
		nk_window_show(context, INVENTORY_WINDOW, NK_SHOWN);
	}
	if (nk_begin_titled(context, INVENTORY_WINDOW, "Inventory", hud->inventory_bounds, flags))
	{
		struct nk_rect content = nk_window_get_content_region(context);
		float spacing = context->style.window.spacing.y;
		// (the rows under the lists: the weight, the chosen one's line, its buttons)
		float list_height = content.h - 2.0f * SMALL_ROW_HEIGHT - ROW_HEIGHT - 4.0f * spacing - 16.0f;

		hud->inventory_bounds = nk_window_get_bounds(context);
		weight_title(view->inventories, container, "Carried", title, sizeof(title));
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		nk_label(context, title, NK_TEXT_LEFT);
		nk_layout_row_template_begin(context, SDL_max(list_height, 80.0f));
		nk_layout_row_template_push_static(context, 200.0f);
		nk_layout_row_template_push_dynamic(context);
		nk_layout_row_template_end(context);
		// The body: each slot and what is in it (a click takes it off)
		if (nk_group_begin(context, "inventory_body", NK_WINDOW_BORDER))
		{
			for (int s = 0; s < items->slot_count; s++)
			{
				int item = container->equipped[s];

				nk_layout_row_template_begin(context, SMALL_ROW_HEIGHT + 2.0f);
				nk_layout_row_template_push_static(context, 52.0f);
				nk_layout_row_template_push_dynamic(context);
				nk_layout_row_template_end(context);
				nk_label(context, items->slot_names[s], NK_TEXT_LEFT);
				if (item >= 0)
				{
					if (nk_button_label(context, items->types[item].shown_name))
					{
						inventory_unequip(view->inventories, container->id, s);
					}
				}
				else
				{
					nk_label(context, "-", NK_TEXT_CENTERED);
				}
			}
			nk_group_end(context);
		}
		// What is carried, a row each
		if (nk_group_begin(context, "inventory_list", NK_WINDOW_BORDER))
		{
			for (int i = 0; i < container->entry_count; i++)
			{
				if (item_row(hud, view->inventories, &container->entries[i], i == hud->inventory_selected))
				{
					uint64_t now = SDL_GetTicks();

					// A double click uses it (if it can be used)
					if (i == hud->inventory_last_click_row && now - hud->inventory_last_click_ticks < 400)
					{
						inventory_use(view->inventories, container->id, i);
						hud->inventory_last_click_row = -1;
						hud->inventory_selected = i;
					}
					else
					{
						hud->inventory_last_click_row = i;
						hud->inventory_last_click_ticks = now;
						hud->inventory_selected = i == hud->inventory_selected ? -1 : i;
					}
				}
			}
			if (container->entry_count == 0)
			{
				nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
				nk_label(context, "Nothing.", NK_TEXT_LEFT);
			}
			nk_group_end(context);
		}
		if (hud->inventory_selected >= container->entry_count)
		{
			hud->inventory_selected = -1;
		}
		// The chosen row: what it is, and what can be done with it
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		if (hud->inventory_selected >= 0)
		{
			const item_type_t *type = &items->types[container->entries[hud->inventory_selected].item];
			char line[256];

			SDL_snprintf(line, sizeof(line), "%s (%s, %.2f kg, worth %.0f)%s%s", type->shown_name, items_category_name(type->category),
				type->weight_kilograms, type->value, type->description[0] != '\0' ? ": " : "", type->description);
			nk_label(context, line, NK_TEXT_LEFT);
			nk_layout_row_dynamic(context, ROW_HEIGHT, 4);
			if (type->usable && nk_button_label(context, "Use"))
			{
				inventory_use(view->inventories, container->id, hud->inventory_selected);
			}
			if (type->slot >= 0 && nk_button_label(context, "Equip"))
			{
				inventory_equip(view->inventories, container->id, hud->inventory_selected);
				hud->inventory_selected = -1;
			}
			if (nk_button_label(context, "Drop"))
			{
				hud->requests.drop_item = container->entries[hud->inventory_selected].item;
				hud->requests.drop_count = 1;
			}
			if (container->entries[hud->inventory_selected].count > 1 && nk_button_label(context, "Drop all"))
			{
				hud->requests.drop_item = container->entries[hud->inventory_selected].item;
				hud->requests.drop_count = container->entries[hud->inventory_selected].count;
			}
		}
		else
		{
			nk_label(context, "Click a row to choose it.", NK_TEXT_LEFT);
		}
	}
	else
	{
		hud->inventory_open = false;
	}
	nk_end(context);
	if (!hud->inventory_open)
	{
		nk_window_close(context, INVENTORY_WINDOW);
	}
}

// A container's rows in a group; the row clicked (-1: none)
static int container_list(hud_t *hud, const inventories_t *inventories, const container_t *container, const char *group)
{
	struct nk_context *context = &hud->backend->context;
	int clicked = -1;

	if (nk_group_begin(context, group, NK_WINDOW_BORDER))
	{
		for (int i = 0; i < container->entry_count; i++)
		{
			if (item_row(hud, inventories, &container->entries[i], false))
			{
				clicked = i;
			}
		}
		if (container->entry_count == 0)
		{
			nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
			nk_label(context, "Nothing.", NK_TEXT_LEFT);
		}
		nk_group_end(context);
	}
	return clicked;
}

static void loot_window(hud_t *hud, const hud_view_t *view)
{
	struct nk_context *context = &hud->backend->context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR;
	container_t *loot = view->inventories != NULL ? inventory_get(view->inventories, hud->loot_container) : NULL;
	container_t *player = view->inventories != NULL ? inventory_get(view->inventories, view->player_inventory) : NULL;
	char title[96];

	if (hud->loot_container == 0 || loot == NULL || player == NULL)
	{
		if (hud->loot_container != 0)
		{
			nk_window_close(context, LOOT_WINDOW);
		}
		hud->loot_container = 0;
		return;
	}
	if (nk_window_is_hidden(context, LOOT_WINDOW) && nk_window_find(context, LOOT_WINDOW) != NULL)
	{
		nk_window_show(context, LOOT_WINDOW, NK_SHOWN);
	}
	if (nk_begin_titled(context, LOOT_WINDOW, loot->name[0] != '\0' ? loot->name : "Loot", hud->loot_bounds, flags))
	{
		struct nk_rect content = nk_window_get_content_region(context);
		float list_height = content.h - (ROW_HEIGHT + 8.0f) - (SMALL_ROW_HEIGHT + 8.0f) - 8.0f;
		uint32_t loot_id = loot->id;
		uint32_t player_id = player->id;
		int taken;
		int given;

		hud->loot_bounds = nk_window_get_bounds(context);
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 2);
		weight_title(view->inventories, loot, loot->name, title, sizeof(title));
		nk_label(context, title, NK_TEXT_LEFT);
		weight_title(view->inventories, player, "You", title, sizeof(title));
		nk_label(context, title, NK_TEXT_LEFT);
		nk_layout_row_dynamic(context, SDL_max(list_height, 80.0f), 2);
		// A click moves the whole row across
		taken = container_list(hud, view->inventories, loot, "loot_theirs");
		given = container_list(hud, view->inventories, player, "loot_yours");
		if (taken >= 0)
		{
			inventory_move(view->inventories, loot_id, player_id, loot->entries[taken].item, loot->entries[taken].count);
		}
		else if (given >= 0)
		{
			inventory_move(view->inventories, player_id, loot_id, player->entries[given].item, player->entries[given].count);
		}
		nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
		if (nk_button_label(context, "Take all"))
		{
			inventory_move_all(view->inventories, loot_id, player_id);
		}
		if (nk_button_label(context, "Close"))
		{
			hud->loot_container = 0;
		}
	}
	else
	{
		hud->loot_container = 0;
	}
	nk_end(context);
	if (hud->loot_container == 0)
	{
		nk_window_close(context, LOOT_WINDOW);
	}
}

// ---------------------------------------------------------------------------
// Compass and map

static struct nk_color marker_color(const marker_t *marker, nk_byte alpha)
{
	return nk_rgba((nk_byte)SDL_clamp((int)(marker->color[0] * 255.0f), 0, 255), (nk_byte)SDL_clamp((int)(marker->color[1] * 255.0f), 0, 255),
		(nk_byte)SDL_clamp((int)(marker->color[2] * 255.0f), 0, 255), alpha);
}

// From north (-Z) towards east (+X), degrees -180..180
static float bearing_degrees(float dx, float dz)
{
	return SDL_atan2f(dx, -dz) * 180.0f / SDL_PI_F;
}

static float wrap_degrees(float degrees)
{
	degrees = SDL_fmodf(degrees + 180.0f, 360.0f);
	if (degrees < 0.0f)
	{
		degrees += 360.0f;
	}
	return degrees - 180.0f;
}

// On foot, across the top: the heading's ticks and letters, the markers
static void compass_window(hud_t *hud, const hud_view_t *view)
{
	static const char *const letters[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	struct nk_context *context = &hud->backend->context;
	const float width = SDL_min(560.0f, hud->backend->width - 40.0f);
	const float height = 48.0f;
	const float half_view = 80.0f;
	struct nk_rect bounds = nk_rect(0.5f * (hud->backend->width - width), 8.0f, width, height);
	struct nk_color background = hud->colors[NK_COLOR_WINDOW];

	if (!view->on_foot || hud->map_open)
	{
		nk_window_show(context, COMPASS_WINDOW, NK_HIDDEN);
		return;
	}
	nk_window_show(context, COMPASS_WINDOW, NK_SHOWN);
	background.a = 0;
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	nk_style_push_vec2(context, &context->style.window.padding, nk_vec2(0.0f, 0.0f));
	if (nk_begin(context, COMPASS_WINDOW, bounds, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND))
	{
		struct nk_command_buffer *canvas = nk_window_get_canvas(context);
		const struct nk_user_font *font = context->style.font;
		struct nk_rect strip = nk_rect(bounds.x, bounds.y + 2.0f, bounds.w, 24.0f);
		float middle = strip.x + 0.5f * strip.w;
		float per_degree = 0.5f * strip.w / half_view;
		struct nk_color text = hud->colors[NK_COLOR_TEXT];
		struct nk_color tick = text;

		nk_window_set_bounds(context, COMPASS_WINDOW, bounds);
		tick.a = 150;
		nk_fill_rect(canvas, strip, 4.0f, nk_rgba(20, 22, 26, 150));
		// Ticks every 15 degrees, the letters every 45
		for (int degrees = 0; degrees < 360; degrees += 15)
		{
			float relative = wrap_degrees((float)degrees - view->heading_degrees);
			float x = middle + relative * per_degree;

			if (SDL_fabsf(relative) > half_view)
			{
				continue;
			}
			if (degrees % 45 == 0)
			{
				const char *letter = letters[degrees / 45];
				int length = nk_strlen(letter);
				float letter_width = font->width(font->userdata, font->height, letter, length);
				struct nk_color color = degrees == 0 ? nk_rgb(235, 95, 80) : text;

				nk_draw_text(canvas, nk_rect(x - 0.5f * letter_width, strip.y + 0.5f * (strip.h - font->height), letter_width + 2.0f, font->height),
					letter, length, font, nk_rgba(0, 0, 0, 0), color);
			}
			else
			{
				nk_stroke_line(canvas, x, strip.y + 7.0f, x, strip.y + strip.h - 7.0f, 1.5f, tick);
			}
		}
		// The middle
		nk_fill_triangle(canvas, middle - 5.0f, strip.y + strip.h + 6.0f, middle + 5.0f, strip.y + strip.h + 6.0f, middle, strip.y + strip.h,
			text);
		// The markers: a dot each (at the edge if it is out of view), the
		// active one bigger, ringed, with how far
		for (int i = 0; view->markers != NULL && i < view->markers->count; i++)
		{
			const marker_t *marker = &view->markers->list[i];
			float dx = marker->x - view->eye_x;
			float dz = marker->z - view->eye_z;
			float relative = wrap_degrees(bearing_degrees(dx, dz) - view->heading_degrees);
			bool active = marker->id == view->markers->active;
			bool beyond = SDL_fabsf(relative) > half_view;
			float x = middle + SDL_clamp(relative, -half_view, half_view) * per_degree;
			float radius = active ? 7.0f : 5.0f;
			float y = strip.y + 0.5f * strip.h;

			if (!marker->on_compass)
			{
				continue;
			}
			if (beyond)
			{
				// (an arrow at the edge: turn that way)
				float side = relative > 0.0f ? 1.0f : -1.0f;

				nk_fill_triangle(canvas, x - side * 2.0f, y - radius, x - side * 2.0f, y + radius, x + side * (radius + 2.0f), y,
					marker_color(marker, 230));
				continue;
			}
			nk_fill_circle(canvas, nk_rect(x - radius, y - radius, 2.0f * radius, 2.0f * radius), marker_color(marker, 235));
			if (active)
			{
				char distance[24];
				int length;
				float distance_width;
				float meters = SDL_sqrtf(dx * dx + dz * dz);

				nk_stroke_circle(canvas, nk_rect(x - radius - 3.0f, y - radius - 3.0f, 2.0f * radius + 6.0f, 2.0f * radius + 6.0f), 2.0f, text);
				if (meters >= 1000.0f)
				{
					SDL_snprintf(distance, sizeof(distance), "%.1f km", meters / 1000.0f);
				}
				else
				{
					SDL_snprintf(distance, sizeof(distance), "%.0f m", meters);
				}
				length = nk_strlen(distance);
				distance_width = font->width(font->userdata, font->height, distance, length);
				nk_draw_text(canvas, nk_rect(x - 0.5f * distance_width, strip.y + strip.h + 6.0f, distance_width + 2.0f, font->height), distance,
					length, font, nk_rgba(0, 0, 0, 0), text);
			}
		}
	}
	nk_end(context);
	nk_style_pop_vec2(context);
	nk_style_pop_style_item(context);
}

// M on foot: the world map, the markers, the player
static void map_window(hud_t *hud, const hud_view_t *view)
{
	struct nk_context *context = &hud->backend->context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR;
	float margin = 40.0f;
	struct nk_rect bounds = nk_rect(margin, margin, hud->backend->width - 2.0f * margin, hud->backend->height - 2.0f * margin);
	float extent = SDL_max(view->map_extent_meters, 1.0f);

	if (!hud->map_open || !view->on_foot)
	{
		if (hud->map_open)
		{
			nk_window_close(context, MAP_WINDOW);
		}
		hud->map_open = false;
		hud->map_centered = false;
		return;
	}
	if (!hud->map_centered)
	{
		hud->map_center_x = view->eye_x;
		hud->map_center_z = view->eye_z;
		hud->map_centered = true;
	}
	if (nk_window_is_hidden(context, MAP_WINDOW) && nk_window_find(context, MAP_WINDOW) != NULL)
	{
		nk_window_show(context, MAP_WINDOW, NK_SHOWN);
	}
	if (nk_begin_titled(context, MAP_WINDOW, view->map_from_artist ? "Map" : "Map (made from the land)", bounds, flags))
	{
		struct nk_command_buffer *canvas = nk_window_get_canvas(context);
		const struct nk_user_font *font = context->style.font;
		struct nk_rect content = nk_window_get_content_region(context);
		struct nk_rect area;
		const struct nk_input *input = &context->input;
		float fit;
		float scale;
		struct nk_color text = hud->colors[NK_COLOR_TEXT];

		nk_window_set_bounds(context, MAP_WINDOW, bounds);
		nk_layout_row_dynamic(context, content.h - SMALL_ROW_HEIGHT - 8.0f, 1);
		if (nk_widget(&area, context) == NK_WIDGET_INVALID)
		{
			nk_end(context);
			return;
		}
		fit = SDL_min(area.w, area.h) / extent;
		// The wheel zooms about the pointer, a drag moves it, a right click
		// puts a marker (or takes the one there away)
		if (nk_input_is_mouse_hovering_rect(input, area))
		{
			float wheel = input->mouse.scroll_delta.y;

			if (wheel != 0.0f)
			{
				float before = fit * hud->map_zoom;
				float pointer_x = hud->map_center_x + (input->mouse.pos.x - (area.x + 0.5f * area.w)) / before;
				float pointer_z = hud->map_center_z + (input->mouse.pos.y - (area.y + 0.5f * area.h)) / before;
				float after;

				hud->map_zoom = SDL_clamp(hud->map_zoom * SDL_powf(1.25f, wheel), 1.0f, 64.0f);
				after = fit * hud->map_zoom;
				hud->map_center_x = pointer_x - (input->mouse.pos.x - (area.x + 0.5f * area.w)) / after;
				hud->map_center_z = pointer_z - (input->mouse.pos.y - (area.y + 0.5f * area.h)) / after;
				context->input.mouse.scroll_delta.y = 0.0f;
			}
			if (nk_input_is_mouse_down(input, NK_BUTTON_LEFT))
			{
				hud->map_center_x -= input->mouse.delta.x / (fit * hud->map_zoom);
				hud->map_center_z -= input->mouse.delta.y / (fit * hud->map_zoom);
			}
			if (nk_input_is_mouse_pressed(input, NK_BUTTON_RIGHT))
			{
				hud->requests.map_marker = true;
				hud->requests.map_marker_x = hud->map_center_x + (input->mouse.pos.x - (area.x + 0.5f * area.w)) / (fit * hud->map_zoom);
				hud->requests.map_marker_z = hud->map_center_z + (input->mouse.pos.y - (area.y + 0.5f * area.h)) / (fit * hud->map_zoom);
			}
		}
		scale = fit * hud->map_zoom;
		// (the map kept in the window: no wandering off into nothing)
		{
			float half_w = 0.5f * area.w / scale;
			float half_h = 0.5f * area.h / scale;

			hud->map_center_x = half_w * 2.0f >= extent ? 0.5f * extent : SDL_clamp(hud->map_center_x, half_w, extent - half_w);
			hud->map_center_z = half_h * 2.0f >= extent ? 0.5f * extent : SDL_clamp(hud->map_center_z, half_h, extent - half_h);
		}
		nk_fill_rect(canvas, area, 0.0f, nk_rgb(28, 30, 34));
		{
			float left = area.x + 0.5f * area.w - hud->map_center_x * scale;
			float top = area.y + 0.5f * area.h - hud->map_center_z * scale;
			struct nk_rect old_clip = canvas->clip;

			nk_push_scissor(canvas, area);
			if (view->map_texture != NULL)
			{
				struct nk_image image = nk_image_ptr(view->map_texture);

				image.w = (nk_ushort)view->map_texture_width;
				image.h = (nk_ushort)view->map_texture_height;
				image.region[2] = image.w;
				image.region[3] = image.h;
				nk_draw_image(canvas, nk_rect(left, top, extent * scale, extent * scale), &image, nk_rgb(255, 255, 255));
			}
			// The markers: a dot and its name
			for (int i = 0; view->markers != NULL && i < view->markers->count; i++)
			{
				const marker_t *marker = &view->markers->list[i];
				float x = left + marker->x * scale;
				float y = top + marker->z * scale;
				float radius = marker->id == view->markers->active ? 8.0f : 6.0f;
				int length = nk_strlen(marker->label);
				float label_width = font->width(font->userdata, font->height, marker->label, length);

				if (!marker->on_map)
				{
					continue;
				}
				nk_fill_circle(canvas, nk_rect(x - radius - 2.0f, y - radius - 2.0f, 2.0f * radius + 4.0f, 2.0f * radius + 4.0f), nk_rgba(0, 0, 0, 170));
				nk_fill_circle(canvas, nk_rect(x - radius, y - radius, 2.0f * radius, 2.0f * radius), marker_color(marker, 255));
				if (length > 0)
				{
					nk_fill_rect(canvas, nk_rect(x + radius + 3.0f, y - 0.5f * font->height - 1.0f, label_width + 6.0f, font->height + 2.0f), 3.0f,
						nk_rgba(0, 0, 0, 150));
					nk_draw_text(canvas, nk_rect(x + radius + 6.0f, y - 0.5f * font->height, label_width + 2.0f, font->height), marker->label, length,
						font, nk_rgba(0, 0, 0, 0), text);
				}
			}
			// The player: an arrow the way the eye looks
			{
				float x = left + view->eye_x * scale;
				float y = top + view->eye_z * scale;
				float angle = view->heading_degrees * SDL_PI_F / 180.0f;
				float forward_x = SDL_sinf(angle);
				float forward_y = -SDL_cosf(angle);
				float side_x = -forward_y;
				float side_y = forward_x;

				nk_fill_triangle(canvas, x + forward_x * 13.0f, y + forward_y * 13.0f, x - forward_x * 8.0f + side_x * 8.0f,
					y - forward_y * 8.0f + side_y * 8.0f, x - forward_x * 8.0f - side_x * 8.0f, y - forward_y * 8.0f - side_y * 8.0f, nk_rgb(20, 20, 20));
				nk_fill_triangle(canvas, x + forward_x * 10.0f, y + forward_y * 10.0f, x - forward_x * 6.0f + side_x * 6.0f,
					y - forward_y * 6.0f + side_y * 6.0f, x - forward_x * 6.0f - side_x * 6.0f, y - forward_y * 6.0f - side_y * 6.0f, nk_rgb(250, 245, 230));
			}
			// North
			nk_draw_text(canvas, nk_rect(area.x + 10.0f, area.y + 8.0f, 40.0f, font->height), "N", 1, font, nk_rgba(0, 0, 0, 0), nk_rgb(235, 95, 80));
			nk_fill_triangle(canvas, area.x + 32.0f, area.y + 6.0f, area.x + 27.0f, area.y + 6.0f + font->height, area.x + 37.0f,
				area.y + 6.0f + font->height, nk_rgb(235, 95, 80));
			nk_push_scissor(canvas, old_clip);
		}
		nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
		nk_label(context, "Wheel: zoom   Drag: move   Right click: a marker of your own (again on it: away)   M or Escape: close", NK_TEXT_LEFT);
	}
	else
	{
		hud->map_open = false;
	}
	nk_end(context);
	if (!hud->map_open)
	{
		nk_window_close(context, MAP_WINDOW);
		hud->map_centered = false;
	}
}

// ---------------------------------------------------------------------------
// Notices and the journal

void hud_notify(hud_t *hud, const char *text, float seconds, hud_notify_style_t style)
{
	hud_notice_t *notice;

	if (hud->notice_count == HUD_NOTICES_MAX)
	{
		SDL_memmove(&hud->notices[0], &hud->notices[1], sizeof(hud_notice_t) * (HUD_NOTICES_MAX - 1));
		hud->notice_count--;
	}
	notice = &hud->notices[hud->notice_count++];
	SDL_strlcpy(notice->text, text != NULL ? text : "", sizeof(notice->text));
	notice->style = style;
	notice->start_seconds = hud->now_seconds;
	notice->until_seconds = hud->now_seconds + SDL_max(seconds, 0.5f);
}

void hud_show_map_at(hud_t *hud, float x, float z)
{
	hud->map_open = true;
	hud->map_center_x = x;
	hud->map_center_z = z;
	hud->map_centered = true;
	hud->map_zoom = SDL_max(hud->map_zoom, 4.0f);
}

// Under the compass: what the game said, newest last, fading out
static void notices_window(hud_t *hud)
{
	struct nk_context *context = &hud->backend->context;
	const float row = ROW_HEIGHT + 6.0f;
	int kept = 0;
	float width = 0.0f;
	struct nk_rect bounds;
	struct nk_color background = hud->colors[NK_COLOR_WINDOW];

	for (int i = 0; i < hud->notice_count; i++)
	{
		if (hud->now_seconds < hud->notices[i].until_seconds)
		{
			hud->notices[kept++] = hud->notices[i];
		}
	}
	hud->notice_count = kept;
	if (kept == 0)
	{
		nk_window_show(context, NOTICES_WINDOW, NK_HIDDEN);
		return;
	}
	for (int i = 0; i < kept; i++)
	{
		width = SDL_max(width, text_width(hud, hud->notices[i].text));
	}
	width += 40.0f;
	bounds = nk_rect(0.5f * (hud->backend->width - width), 76.0f, width, row * (float)kept + 4.0f);
	nk_window_show(context, NOTICES_WINDOW, NK_SHOWN);
	background.a = 0;
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	nk_style_push_vec2(context, &context->style.window.padding, nk_vec2(0.0f, 0.0f));
	if (nk_begin(context, NOTICES_WINDOW, bounds, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND))
	{
		struct nk_command_buffer *canvas = nk_window_get_canvas(context);
		const struct nk_user_font *font = context->style.font;

		nk_window_set_bounds(context, NOTICES_WINDOW, bounds);
		for (int i = 0; i < kept; i++)
		{
			const hud_notice_t *notice = &hud->notices[i];
			double left = notice->until_seconds - hud->now_seconds;
			double age = hud->now_seconds - notice->start_seconds;
			// (in quickly, out over the last second)
			float alpha = (float)SDL_min(SDL_min(left, 1.0), SDL_min(age * 4.0, 1.0));
			struct nk_color accent = notice->style == HUD_NOTIFY_QUEST ? nk_rgb(235, 190, 70)
				: (notice->style == HUD_NOTIFY_WARNING ? nk_rgb(225, 85, 70) : nk_rgb(140, 175, 220));
			struct nk_color text = hud->colors[NK_COLOR_TEXT];
			int length = nk_strlen(notice->text);
			float text_w = font->width(font->userdata, font->height, notice->text, length);
			struct nk_rect box = nk_rect(bounds.x + 0.5f * (bounds.w - text_w - 32.0f), bounds.y + 2.0f + row * (float)i, text_w + 32.0f, row - 4.0f);

			accent.a = (nk_byte)(255.0f * alpha);
			text.a = (nk_byte)(255.0f * alpha);
			nk_fill_rect(canvas, box, 4.0f, nk_rgba(20, 22, 26, (nk_byte)(190.0f * alpha)));
			nk_fill_rect(canvas, nk_rect(box.x, box.y, 5.0f, box.h), 2.0f, accent);
			nk_draw_text(canvas, nk_rect(box.x + 18.0f, box.y + 0.5f * (box.h - font->height), text_w + 2.0f, font->height), notice->text, length,
				font, nk_rgba(0, 0, 0, 0), notice->style == HUD_NOTIFY_QUEST ? accent : text);
		}
	}
	nk_end(context);
	nk_style_pop_vec2(context);
	nk_style_pop_style_item(context);
}

// An objective's sign: an empty box, a ticked one, a crossed one
static void objective_sign(hud_t *hud, struct nk_rect bounds, journal_objective_state_t state)
{
	struct nk_command_buffer *canvas = nk_window_get_canvas(&hud->backend->context);
	float size = SDL_min(bounds.w, bounds.h) - 8.0f;
	float x = bounds.x + 0.5f * (bounds.w - size);
	float y = bounds.y + 0.5f * (bounds.h - size);
	struct nk_color line = hud->colors[NK_COLOR_TEXT];

	if (state == JOURNAL_OBJECTIVE_DONE)
	{
		struct nk_color green = nk_rgb(110, 190, 110);

		nk_fill_rect(canvas, nk_rect(x, y, size, size), 2.0f, nk_rgba(110, 190, 110, 60));
		nk_stroke_rect(canvas, nk_rect(x, y, size, size), 2.0f, 1.5f, green);
		nk_stroke_line(canvas, x + 0.2f * size, y + 0.55f * size, x + 0.42f * size, y + 0.78f * size, 2.0f, green);
		nk_stroke_line(canvas, x + 0.42f * size, y + 0.78f * size, x + 0.82f * size, y + 0.25f * size, 2.0f, green);
	}
	else if (state == JOURNAL_OBJECTIVE_FAILED)
	{
		struct nk_color red = nk_rgb(215, 90, 80);

		nk_stroke_rect(canvas, nk_rect(x, y, size, size), 2.0f, 1.5f, red);
		nk_stroke_line(canvas, x + 0.25f * size, y + 0.25f * size, x + 0.75f * size, y + 0.75f * size, 2.0f, red);
		nk_stroke_line(canvas, x + 0.75f * size, y + 0.25f * size, x + 0.25f * size, y + 0.75f * size, 2.0f, red);
	}
	else
	{
		nk_stroke_rect(canvas, nk_rect(x, y, size, size), 2.0f, 1.5f, line);
	}
}

// Text wrapped in rows as wide as the layout: about how many rows it needs
static float wrapped_height(hud_t *hud, const char *text, float width)
{
	const struct nk_user_font *font = hud->backend->context.style.font;
	float lines = SDL_ceilf(text_width(hud, text) / SDL_max(width - 24.0f, 40.0f)) + 1.0f;

	for (const char *c = text; *c != '\0'; c++)
	{
		lines += *c == '\n' ? 1.0f : 0.0f;
	}
	return lines * (font->height + 3.0f);
}

static void journal_window(hud_t *hud, const hud_view_t *view)
{
	struct nk_context *context = &hud->backend->context;
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR;
	journal_t *journal = view->journal;
	journal_quest_t *selected;

	if (!hud->journal_open || journal == NULL || !view->on_foot)
	{
		if (hud->journal_open)
		{
			nk_window_close(context, JOURNAL_WINDOW);
		}
		hud->journal_open = false;
		return;
	}
	selected = journal_find(journal, hud->journal_selected);
	if (selected == NULL && journal->count > 0)
	{
		// (the newest active one first)
		uint32_t newest = 0;

		for (int i = 0; i < journal->count; i++)
		{
			if (journal->quests[i].state == JOURNAL_QUEST_ACTIVE && journal->quests[i].order > newest)
			{
				newest = journal->quests[i].order;
				selected = &journal->quests[i];
			}
		}
		selected = selected != NULL ? selected : &journal->quests[0];
		SDL_strlcpy(hud->journal_selected, selected->id, sizeof(hud->journal_selected));
	}
	if (nk_window_is_hidden(context, JOURNAL_WINDOW) && nk_window_find(context, JOURNAL_WINDOW) != NULL)
	{
		nk_window_show(context, JOURNAL_WINDOW, NK_SHOWN);
	}
	if (nk_begin_titled(context, JOURNAL_WINDOW, "Journal", hud->journal_bounds, flags))
	{
		struct nk_rect content = nk_window_get_content_region(context);

		hud->journal_bounds = nk_window_get_bounds(context);
		nk_layout_row_template_begin(context, SDL_max(content.h - 8.0f, 80.0f));
		nk_layout_row_template_push_static(context, 240.0f);
		nk_layout_row_template_push_dynamic(context);
		nk_layout_row_template_end(context);
		// The quests by their state, the newest first; the tracked ones with a dot
		if (nk_group_begin(context, "journal_list", NK_WINDOW_BORDER))
		{
			for (int state = 0; state < JOURNAL_QUEST_STATE_COUNT; state++)
			{
				bool heading = false;
				uint32_t last = UINT32_MAX;

				for (;;)
				{
					journal_quest_t *next = NULL;

					for (int i = 0; i < journal->count; i++)
					{
						journal_quest_t *quest = &journal->quests[i];

						if ((int)quest->state == state && quest->order < last && (next == NULL || quest->order > next->order))
						{
							next = quest;
						}
					}
					if (next == NULL)
					{
						break;
					}
					last = next->order;
					if (!heading)
					{
						nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
						nk_label_colored(context, journal_state_name((journal_quest_state_t)state), NK_TEXT_LEFT, nk_rgb(150, 160, 175));
						heading = true;
					}
					nk_layout_row_template_begin(context, SMALL_ROW_HEIGHT + 2.0f);
					nk_layout_row_template_push_static(context, 14.0f);
					nk_layout_row_template_push_dynamic(context);
					nk_layout_row_template_end(context);
					{
						struct nk_rect dot;

						if (nk_widget(&dot, context) != NK_WIDGET_INVALID && next->tracked)
						{
							nk_fill_circle(nk_window_get_canvas(context), nk_rect(dot.x + 3.0f, dot.y + 0.5f * dot.h - 4.0f, 8.0f, 8.0f),
								nk_rgb(235, 190, 70));
						}
					}
					{
						nk_bool chosen = selected == next;

						if (nk_selectable_label(context, next->title[0] != '\0' ? next->title : next->id, NK_TEXT_LEFT, &chosen))
						{
							SDL_strlcpy(hud->journal_selected, next->id, sizeof(hud->journal_selected));
							selected = next;
						}
					}
				}
			}
			if (journal->count == 0)
			{
				nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
				nk_label(context, "No quests yet.", NK_TEXT_LEFT);
			}
			nk_group_end(context);
		}
		// The chosen quest: its story, its objectives, what can be done
		if (nk_group_begin(context, "journal_quest", NK_WINDOW_BORDER))
		{
			if (selected != NULL)
			{
				struct nk_rect region = nk_window_get_content_region(context);
				char line[256];

				nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
				nk_label_colored(context, selected->title, NK_TEXT_LEFT, nk_rgb(235, 190, 70));
				nk_layout_row_dynamic(context, SMALL_ROW_HEIGHT, 1);
				SDL_snprintf(line, sizeof(line), "%s%s", journal_state_name(selected->state), selected->tracked ? ", tracked" : "");
				nk_label_colored(context, line, NK_TEXT_LEFT, nk_rgb(150, 160, 175));
				if (selected->description[0] != '\0')
				{
					nk_layout_row_dynamic(context, wrapped_height(hud, selected->description, region.w), 1);
					nk_label_wrap(context, selected->description);
				}
				nk_layout_row_dynamic(context, 6.0f, 1);
				nk_spacing(context, 1);
				for (int o = 0; o < selected->objective_count; o++)
				{
					journal_objective_t *objective = &selected->objectives[o];
					bool focused = SDL_strcmp(journal->focus_quest, selected->id) == 0 && SDL_strcmp(journal->focus_objective, objective->id) == 0;
					struct nk_rect sign;
					struct nk_color color = objective->state == JOURNAL_OBJECTIVE_OPEN ? (focused ? nk_rgb(245, 205, 95) : hud->colors[NK_COLOR_TEXT])
						: nk_rgb(140, 145, 150);

					nk_layout_row_template_begin(context, ROW_HEIGHT);
					nk_layout_row_template_push_static(context, ROW_HEIGHT);
					nk_layout_row_template_push_dynamic(context);
					nk_layout_row_template_push_static(context, 64.0f);
					nk_layout_row_template_push_static(context, 64.0f);
					nk_layout_row_template_end(context);
					if (nk_widget(&sign, context) != NK_WIDGET_INVALID)
					{
						objective_sign(hud, sign, objective->state);
					}
					if (objective->progress[0] != '\0')
					{
						SDL_snprintf(line, sizeof(line), "%s  (%s)", objective->text, objective->progress);
					}
					else
					{
						SDL_strlcpy(line, objective->text, sizeof(line));
					}
					nk_label_colored(context, line, NK_TEXT_LEFT, color);
					if (objective->state == JOURNAL_OBJECTIVE_OPEN && objective->has_place && selected->state == JOURNAL_QUEST_ACTIVE)
					{
						if (nk_button_label(context, focused ? "Focused" : "Focus"))
						{
							if (!journal_focus(journal, selected->id, objective->id))
							{
								hud_notify(hud, "Too many quests tracked: untrack one first", 3.0f, HUD_NOTIFY_WARNING);
							}
						}
						if (nk_button_label(context, "Map"))
						{
							journal_focus(journal, selected->id, objective->id);
							hud->journal_open = false;
							hud_show_map_at(hud, objective->x, objective->z);
						}
					}
					else
					{
						nk_spacing(context, 2);
					}
				}
				nk_layout_row_dynamic(context, 8.0f, 1);
				nk_spacing(context, 1);
				nk_layout_row_dynamic(context, ROW_HEIGHT, 3);
				if (selected->state == JOURNAL_QUEST_ACTIVE && nk_button_label(context, selected->tracked ? "Untrack" : "Track"))
				{
					if (!journal_track(journal, selected->id, !selected->tracked))
					{
						char message[96];

						SDL_snprintf(message, sizeof(message), "You can track at most %d quests", journal->track_limit);
						hud_notify(hud, message, 3.0f, HUD_NOTIFY_WARNING);
					}
				}
			}
			nk_group_end(context);
		}
	}
	else
	{
		hud->journal_open = false;
	}
	nk_end(context);
	if (!hud->journal_open)
	{
		nk_window_close(context, JOURNAL_WINDOW);
	}
}

// ---------------------------------------------------------------------------
// Names and the dialog

// Over the named actors near the eye
// The shapes drawn in the world (hud_world_shape_t): each with its own
// depth, hidden by what stands nearer
static void world_shapes_window(hud_t *hud, const hud_view_t *view)
{
	struct nk_context *context = &hud->backend->context;
	struct nk_rect bounds = nk_rect(0.0f, 0.0f, hud->backend->width, hud->backend->height);
	struct nk_color background = hud->colors[NK_COLOR_WINDOW];

	if (view->world_shape_count == 0 || view->world_shapes == NULL)
	{
		nk_window_show(context, SHAPES_WINDOW, NK_HIDDEN);
		return;
	}
	nk_window_show(context, SHAPES_WINDOW, NK_SHOWN);
	background.a = 0;
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	if (nk_begin(context, SHAPES_WINDOW, bounds, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND))
	{
		struct nk_command_buffer *canvas = nk_window_get_canvas(context);
		const struct nk_user_font *font = context->style.font;

		nk_window_set_bounds(context, SHAPES_WINDOW, bounds);
		for (int i = 0; i < view->world_shape_count; i++)
		{
			const hud_world_shape_t *shape = &view->world_shapes[i];
			struct nk_color color = nk_rgba(shape->color[0], shape->color[1], shape->color[2], shape->color[3]);

			nk_set_user_data(context, ui_backend_world_depth(shape->depth));
			if (shape->count == 4)
			{
				float points[8];

				SDL_memcpy(points, shape->points, sizeof(points));
				nk_fill_polygon(canvas, points, 4, color);
			}
			else if (shape->count == 2)
			{
				nk_stroke_line(canvas, shape->points[0], shape->points[1], shape->points[2], shape->points[3], shape->thickness, color);
			}
			if (shape->label[0] != '\0')
			{
				int length = nk_strlen(shape->label);
				float width = font->width(font->userdata, font->height, shape->label, length);

				nk_draw_text(canvas, nk_rect(shape->points[0] - 0.5f * width, shape->points[1] - 0.5f * font->height, width + 2.0f, font->height),
					shape->label, length, font, nk_rgba(0, 0, 0, 0), nk_rgb(255, 255, 255));
			}
		}
		nk_set_user_data(context, nk_handle_id(0));
	}
	nk_end(context);
	nk_style_pop_style_item(context);
}

static void names_window(hud_t *hud, const hud_view_t *view)
{
	struct nk_context *context = &hud->backend->context;
	struct nk_rect bounds = nk_rect(0.0f, 0.0f, hud->backend->width, hud->backend->height);
	struct nk_color background = hud->colors[NK_COLOR_WINDOW];

	if (view->nameplate_count == 0 || !view->on_foot)
	{
		nk_window_show(context, NAMES_WINDOW, NK_HIDDEN);
		return;
	}
	nk_window_show(context, NAMES_WINDOW, NK_SHOWN);
	background.a = 0;
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	if (nk_begin(context, NAMES_WINDOW, bounds, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND))
	{
		struct nk_command_buffer *canvas = nk_window_get_canvas(context);
		const struct nk_user_font *font = context->style.font;

		nk_window_set_bounds(context, NAMES_WINDOW, bounds);
		for (int i = 0; i < view->nameplate_count; i++)
		{
			const hud_nameplate_t *plate = &view->nameplates[i];
			int length = nk_strlen(plate->name);
			float width = font->width(font->userdata, font->height, plate->name, length);
			float x = (plate->screen_x * 0.5f + 0.5f) * hud->backend->width;
			float y = (0.5f - plate->screen_y * 0.5f) * hud->backend->height;
			struct nk_color color = plate->hostile ? nk_rgb(235, 90, 75) : nk_rgb(235, 235, 220);

			color.a = (nk_byte)(255.0f * plate->alpha);
			// (drawn in the world: what stands nearer hides it)
			nk_set_user_data(context, ui_backend_world_depth(plate->depth));
			nk_fill_rect(canvas, nk_rect(x - 0.5f * width - 6.0f, y - font->height - 4.0f, width + 12.0f, font->height + 4.0f), 3.0f,
				nk_rgba(0, 0, 0, (nk_byte)(130.0f * plate->alpha)));
			nk_draw_text(canvas, nk_rect(x - 0.5f * width, y - font->height - 2.0f, width + 2.0f, font->height), plate->name, length, font,
				nk_rgba(0, 0, 0, 0), color);
		}
		nk_set_user_data(context, nk_handle_id(0));
	}
	nk_end(context);
	nk_style_pop_style_item(context);
}

// The game's debug lines, in the top left corner (the game says what)
static void debug_window(hud_t *hud)
{
	struct nk_context *context = &hud->backend->context;
	const struct nk_user_font *font = context->style.font;
	struct nk_color background = hud->colors[NK_COLOR_WINDOW];
	float width = 0.0f;
	float row = font->height + 4.0f;
	struct nk_rect bounds;

	if (!hud->debug_open || hud->debug_line_count == 0)
	{
		nk_window_show(context, DEBUG_WINDOW, NK_HIDDEN);
		return;
	}
	for (int i = 0; i < hud->debug_line_count; i++)
	{
		width = SDL_max(width, font->width(font->userdata, font->height, hud->debug_lines[i], nk_strlen(hud->debug_lines[i])));
	}
	bounds = nk_rect(8.0f, 8.0f, width + 24.0f, (float)hud->debug_line_count * row + 16.0f);
	nk_window_show(context, DEBUG_WINDOW, NK_SHOWN);
	background.a = (nk_byte)((float)background.a * 0.75f);
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	if (nk_begin(context, DEBUG_WINDOW, bounds, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT))
	{
		nk_window_set_bounds(context, DEBUG_WINDOW, bounds);
		for (int i = 0; i < hud->debug_line_count; i++)
		{
			nk_layout_row_dynamic(context, font->height, 1);
			nk_label(context, hud->debug_lines[i], NK_TEXT_LEFT);
		}
	}
	nk_end(context);
	nk_style_pop_style_item(context);
}

// The conversation, low in the middle: who talks, the line, the answers
static void dialog_window(hud_t *hud, const hud_view_t *view)
{
	struct nk_context *context = &hud->backend->context;
	dialog_t *dialog = view->dialog;
	float width = SDL_min(780.0f, hud->backend->width - 40.0f);
	float text_height;
	float height;
	int answers;
	struct nk_rect bounds;

	float row;
	struct nk_color background = hud->colors[NK_COLOR_WINDOW];
	bool large = hud->backend->large_font != NULL;

	if (dialog == NULL || !dialog->open)
	{
		nk_window_show(context, DIALOG_WINDOW, NK_HIDDEN);
		return;
	}
	// In the bigger font, on a see-through ground (80%)
	if (large)
	{
		nk_style_push_font(context, &hud->backend->large_font->handle);
	}
	width = SDL_min(large ? 900.0f : 780.0f, hud->backend->width - 40.0f);
	row = ROW_HEIGHT * (large ? hud->backend->large_scale : 1.0f);
	answers = SDL_max(dialog->choice_count, 1);
	text_height = wrapped_height(hud, dialog->text, width - 40.0f);
	height = row + text_height + (float)answers * (row + 4.0f) + 48.0f;
	bounds = nk_rect(0.5f * (hud->backend->width - width), hud->backend->height - height - 48.0f, width, height);
	background.a = 204;
	nk_style_push_style_item(context, &context->style.window.fixed_background, nk_style_item_color(background));
	nk_window_show(context, DIALOG_WINDOW, NK_SHOWN);
	if (nk_begin(context, DIALOG_WINDOW, bounds, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
	{
		enum nk_text_align old_alignment = context->style.button.text_alignment;

		nk_window_set_bounds(context, DIALOG_WINDOW, bounds);
		nk_layout_row_dynamic(context, row, 1);
		nk_label_colored(context, dialog->speaker, NK_TEXT_LEFT, nk_rgb(235, 190, 70));
		nk_layout_row_dynamic(context, text_height, 1);
		nk_label_wrap(context, dialog->text);
		nk_layout_row_dynamic(context, 6.0f, 1);
		nk_spacing(context, 1);
		context->style.button.text_alignment = NK_TEXT_LEFT;
		for (int i = 0; i < answers; i++)
		{
			char line[DIALOG_CHOICE_LENGTH + 8];

			if (dialog->choice_count == 0)
			{
				SDL_strlcpy(line, "  (continue)", sizeof(line));
			}
			else
			{
				SDL_snprintf(line, sizeof(line), "  %d. %s", i + 1, dialog->choices[i]);
			}
			nk_layout_row_dynamic(context, row, 1);
			if (nk_button_label(context, line))
			{
				dialog_choose(dialog, i);
			}
		}
		context->style.button.text_alignment = old_alignment;
	}
	nk_end(context);
	nk_style_pop_style_item(context);
	if (large)
	{
		nk_style_pop_font(context);
	}
}

// On foot: what E would do, under the middle of the screen
static void prompt_window(hud_t *hud, const hud_view_t *view)
{
	struct nk_context *context = &hud->backend->context;
	float width;
	float height = ROW_HEIGHT + 10.0f;
	struct nk_rect bounds;

	if (view->interaction_prompt == NULL || view->interaction_prompt[0] == '\0' || hud->loot_container != 0)
	{
		nk_window_show(context, PROMPT_WINDOW, NK_HIDDEN);
		return;
	}
	width = text_width(hud, view->interaction_prompt) + 32.0f;
	bounds = nk_rect(0.5f * (hud->backend->width - width), 0.5f * hud->backend->height + 48.0f, width, height);
	nk_window_show(context, PROMPT_WINDOW, NK_SHOWN);
	if (nk_begin(context, PROMPT_WINDOW, bounds, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BORDER | NK_WINDOW_BACKGROUND))
	{
		nk_window_set_bounds(context, PROMPT_WINDOW, bounds);
		nk_layout_row_dynamic(context, ROW_HEIGHT, 1);
		nk_label(context, view->interaction_prompt, NK_TEXT_CENTERED);
	}
	nk_end(context);
}

// ---------------------------------------------------------------------------

void hud_update(hud_t *hud, ui_backend_t *backend, const struct nk_color *colors, const hud_view_t *view, double time_seconds)
{
	SDL_zero(hud->requests);
	hud->requests.drop_item = -1;
	hud->now_seconds = time_seconds;
	hud->dialog = view->dialog;
	if (backend == NULL)
	{
		return;
	}
	hud->backend = backend;
	hud->colors = colors;
	world_shapes_window(hud, view);
	names_window(hud, view);
	compass_window(hud, view);
	dialog_window(hud, view);
	notices_window(hud);
	journal_window(hud, view);
	map_window(hud, view);
	inventory_window(hud, view);
	loot_window(hud, view);
	prompt_window(hud, view);
	debug_window(hud);
}
