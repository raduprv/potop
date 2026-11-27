// Drawing a wreck model with the soft renderer: see damage_draw.h

#include "damage_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979f
// A log is a prism of this many flat sides, lying on a flat one (as the
// model makes its hull)
#define LOG_SIDE_COUNT 12
#define LOG_FIRST_CORNER_RADIANS (PI / (float)LOG_SIDE_COUNT)
#define BROKEN_SHOWN_FRAMES 90
#define HIT_SHOWN_FRAMES 30
#define ASLEEP_COLOR 0x707070
#define PROJECTILE_COLOR 0x505868
#define LINK_WHOLE_COLOR 0x30E040
#define LINK_HURT_COLOR 0xFFE040
#define LINK_BROKEN_COLOR 0xFF3020
#define LINK_OVERLOADED_COLOR 0xFF8000
#define CRUSHED_COLOR 0x303030
#define HIT_COLOR 0xFF2020
#define LABEL_COLOR 0xFFFFFF

static b3Vec3 make_vector(float x, float y, float z)
{
	b3Vec3 vector = {x, y, z};

	return vector;
}

// (A b3Vec3 is three floats and a b3Quat four, as the renderer takes them)
static const float *vector_floats(const b3Vec3 *vector)
{
	return (const float *)vector;
}

static const float *rotation_floats(const b3Quat *rotation)
{
	return (const float *)rotation;
}

// A body's color when it sleeps: half way to gray
static uint32_t asleep_color(uint32_t color)
{
	uint32_t red = (((color >> 16) & 0xFF) + ((ASLEEP_COLOR >> 16) & 0xFF)) / 2;
	uint32_t green = (((color >> 8) & 0xFF) + ((ASLEEP_COLOR >> 8) & 0xFF)) / 2;
	uint32_t blue = ((color & 0xFF) + (ASLEEP_COLOR & 0xFF)) / 2;

	return (red << 16) | (green << 8) | blue;
}

void damage_draw_solids(const wreck_model_t *model, soft_render_t *renderer, uint32_t first_object_id)
{
	int i;

	for (i = 0; i < model->piece_count; i++)
	{
		const wreck_piece_t *piece = &model->pieces[i];
		const wreck_group_t *group;
		b3Transform transform;
		uint32_t color;

		if (piece->group == WRECK_GONE)
		{
			continue;
		}
		group = &model->groups[piece->group];
		transform = wreck_model_piece_transform(model, i);
		// (What stands is shown in its own color; what lies still after
		// coming loose is grayed; what was crushed is dark for the frame
		// or two before it goes)
		color = (group->anchored || b3Body_IsAwake(group->body)) ? piece->color : asleep_color(piece->color);
		if (piece->crushed)
		{
			color = CRUSHED_COLOR;
		}

		soft_render_set_object(renderer, first_object_id + (uint32_t)i);
		if (piece->shape == WRECK_PIECE_BOX)
		{
			soft_render_box(renderer, vector_floats(&transform.p), rotation_floats(&transform.q), vector_floats(&piece->half_extents), color);
		}
		else
		{
			// The very prism its collision shape is
			b3Vec3 half_axis = b3RotateVector(transform.q, make_vector(piece->half_length, 0.0f, 0.0f));
			b3Vec3 end_a = b3Sub(transform.p, half_axis);
			b3Vec3 end_b = b3Add(transform.p, half_axis);
			b3Vec3 first_corner = b3RotateVector(transform.q, make_vector(0.0f, cosf(LOG_FIRST_CORNER_RADIANS), sinf(LOG_FIRST_CORNER_RADIANS)));

			soft_render_prism(renderer, vector_floats(&end_a), vector_floats(&end_b), vector_floats(&first_corner), piece->radius, LOG_SIDE_COUNT, color);
		}
	}
	for (i = 0; i < model->projectile_count; i++)
	{
		const wreck_projectile_t *projectile = &model->projectiles[i];
		b3Vec3 center = b3Body_GetPosition(projectile->body);

		soft_render_set_object(renderer, first_object_id + (uint32_t)(model->piece_count + i));
		soft_render_sphere(renderer, vector_floats(&center), projectile->radius,
			b3Body_IsAwake(projectile->body) ? PROJECTILE_COLOR : asleep_color(PROJECTILE_COLOR));
	}
}

void damage_draw_marks(const wreck_model_t *model, soft_render_t *renderer, int frame_index, bool draw_debug)
{
	int first_hit = model->hit_count > WRECK_RECENT_HITS_MAX ? model->hit_count - WRECK_RECENT_HITS_MAX : 0;
	int i;

	if (draw_debug)
	{
		// The links, through the solids: most are inside the building
		renderer->marks_on_top = true;
		for (i = 0; i < model->link_count; i++)
		{
			const wreck_link_t *link = &model->links[i];
			b3Transform frame;
			b3Vec3 point;

			if (model->pieces[link->piece_a].group == WRECK_GONE)
			{
				continue;
			}
			frame = b3Body_GetTransform(model->groups[model->pieces[link->piece_a].group].body);
			point = b3Add(frame.p, b3RotateVector(frame.q, link->local_point));

			if (link->broken)
			{
				if (frame_index - link->broken_frame < BROKEN_SHOWN_FRAMES)
				{
					soft_render_point(renderer, vector_floats(&point), 5, LINK_BROKEN_COLOR);
				}
			}
			else
			{
				bool hurt = link->hit_points_left < 1.0f;

				if (link->overload > 1.0f)
				{
					soft_render_point(renderer, vector_floats(&point), 6, LINK_OVERLOADED_COLOR);
				}
				else
				{
					soft_render_point(renderer, vector_floats(&point), hurt ? 4 : 3, hurt ? LINK_HURT_COLOR : LINK_WHOLE_COLOR);
				}
			}
		}
		renderer->marks_on_top = false;
	}

	// The hits of the last frames; with debug, each one's impulse
	for (i = first_hit; i < model->hit_count; i++)
	{
		const wreck_hit_t *hit = &model->recent_hits[i % WRECK_RECENT_HITS_MAX];
		float screen_x;
		float screen_y;

		if (hit->frame_index > frame_index || frame_index - hit->frame_index >= HIT_SHOWN_FRAMES)
		{
			continue;
		}
		soft_render_point(renderer, vector_floats(&hit->point), hit->broke_links ? 9 : 5, HIT_COLOR);
		if (draw_debug && soft_render_project(renderer, vector_floats(&hit->point), &screen_x, &screen_y))
		{
			char text[32];

			snprintf(text, sizeof(text), "%.0f NS", (double)hit->impulse);
			soft_render_text(renderer, (int)screen_x + 6, (int)screen_y - 4, 1, LABEL_COLOR, text);
		}
	}
}

void damage_draw_inspection(const wreck_model_t *model, soft_render_t *renderer, int piece_index, int text_x, int text_y, int text_scale)
{
	static const char *const material_names[WRECK_MATERIAL_COUNT] = {"WOOD", "BRICK", "GLASS"};
	static const char *const link_names[WRECK_LINK_TYPE_COUNT] = {"NAILS", "MORTAR", "FOUNDATION", "GLAZING", "NOTCH"};
	const wreck_piece_t *piece;
	const wreck_group_t *group;
	b3Transform frame;
	b3Vec3 center;
	char text[160];
	int line_height = (7 + 2) * text_scale;
	int whole_count = 0;
	int broken_count = 0;
	int k;

	if (piece_index < 0 || piece_index >= model->piece_count)
	{
		return;
	}
	piece = &model->pieces[piece_index];
	if (piece->group == WRECK_GONE)
	{
		snprintf(text, sizeof(text), "PIECE %d: %s, GONE", piece_index, material_names[piece->material]);
		soft_render_text(renderer, text_x, text_y, text_scale, LABEL_COLOR, text);
		return;
	}
	group = &model->groups[piece->group];
	frame = b3Body_GetTransform(group->body);
	center = b3Add(frame.p, b3RotateVector(frame.q, piece->local.p));

	// Its links, each as a line from its middle to the link and on to the
	// middle of what the link holds it to (straight down for the ground)
	renderer->marks_on_top = true;
	renderer->line_width_pixels = 2;
	for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
	{
		const wreck_link_t *link = &model->links[model->piece_links[k]];
		b3Vec3 point = b3Add(frame.p, b3RotateVector(frame.q, link->local_point));
		b3Vec3 other_end;
		uint32_t color = link->overload > 1.0f ? LINK_OVERLOADED_COLOR : (link->overload > 0.5f ? LINK_HURT_COLOR : LINK_WHOLE_COLOR);
		int other;

		if (link->broken)
		{
			broken_count++;
			continue;
		}
		whole_count++;
		other = link->piece_a == piece_index ? link->piece_b : link->piece_a;
		if (other == WRECK_GROUND)
		{
			other_end = b3Sub(point, make_vector(0.0f, 0.15f, 0.0f));
		}
		else
		{
			other_end = b3Add(frame.p, b3RotateVector(frame.q, model->pieces[other].local.p));
		}
		soft_render_line(renderer, vector_floats(&center), vector_floats(&point), color);
		soft_render_line(renderer, vector_floats(&point), vector_floats(&other_end), color);
		soft_render_point(renderer, vector_floats(&point), 6, color);
	}
	soft_render_point(renderer, vector_floats(&center), 8, LABEL_COLOR);
	renderer->line_width_pixels = 1;
	renderer->marks_on_top = false;

	snprintf(text, sizeof(text), "PIECE %d: %s, %.0f KG, %s (GROUP OF %d)", piece_index, material_names[piece->material], (double)piece->mass,
		group->anchored ? "STANDS" : (group->rubble ? "RUBBLE" : "LOOSE"), group->piece_count);
	soft_render_text(renderer, text_x, text_y, text_scale, LABEL_COLOR, text);
	text_y += line_height;
	snprintf(text, sizeof(text), "PRESSED %.0f N OF %.0f (%.0f%%)   LINKS: %d WHOLE, %d BROKEN", (double)piece->pressed, (double)model->settings.crush[piece->material],
		(double)(100.0f * piece->pressed / model->settings.crush[piece->material]), whole_count, broken_count);
	soft_render_text(renderer, text_x, text_y, text_scale, LABEL_COLOR, text);
	text_y += line_height;
	if (piece->long_axis >= 0)
	{
		snprintf(text, sizeof(text), "HIT POINTS %.0f/%.0f   LONG: BENT %.0f%% OF WHAT IT BEARS", (double)piece->hit_points_left,
			(double)model->settings.piece_hit_points[piece->material], (double)(100.0f * piece->bent));
	}
	else
	{
		snprintf(text, sizeof(text), "HIT POINTS %.0f/%.0f", (double)piece->hit_points_left, (double)model->settings.piece_hit_points[piece->material]);
	}
	soft_render_text(renderer, text_x, text_y, text_scale, LABEL_COLOR, text);
	text_y += line_height;

	// (What a link bears is known only for what stands: for a loose piece
	// the last pass left zeros)
	for (k = model->piece_link_start[piece_index]; k < model->piece_link_start[piece_index + 1]; k++)
	{
		const wreck_link_t *link = &model->links[model->piece_links[k]];
		int other = link->piece_a == piece_index ? link->piece_b : link->piece_a;
		char other_name[16];

		if (link->broken)
		{
			continue;
		}
		if (other == WRECK_GROUND)
		{
			snprintf(other_name, sizeof(other_name), "GROUND");
		}
		else
		{
			snprintf(other_name, sizeof(other_name), "%d", other);
		}
		snprintf(text, sizeof(text), "%s TO %s: PRESS %.0f  PULL %.0f  SIDE %.0f N  TURN %.0f NM  %.0f%% OF LIMIT  %.0f%% LEFT",
			link_names[link->type], other_name, (double)link->static_compression, (double)link->static_pull, (double)link->static_sideways,
			(double)link->static_moment, (double)(100.0f * link->overload), (double)(100.0f * link->hit_points_left));
		soft_render_text(renderer, text_x, text_y, text_scale, link->overload > 1.0f ? LINK_OVERLOADED_COLOR : LABEL_COLOR, text);
		text_y += line_height;
	}
}
