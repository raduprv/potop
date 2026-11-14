// Terrain stamps. See stamps.h.

#include "stamps.h"

#include <SDL3/SDL.h>

// Dam shape: half the crest width, and the side slope (rise over run)
#define DAM_CREST_HALF_WIDTH_METERS 1.0f
#define DAM_SIDE_SLOPE 0.5f
// Channel shape: bottom width as a fraction of the brush radius, side slope
#define CHANNEL_BOTTOM_HALF_WIDTH_FRACTION 0.25f
#define CHANNEL_SIDE_SLOPE 1.0f
// Plateau: flat top over this fraction of the radius, then 1:1 slopes
#define PLATEAU_TOP_FRACTION 0.6f
#define PLATEAU_SIDE_SLOPE 1.0f
// Lakes are filled this far below their lowest rim point
#define LAKE_FREEBOARD_METERS 0.05f
// No stamp reaches further than this from its point or line
#define STAMP_MAXIMUM_FOOTPRINT_METERS 250.0f

const char *stamps_name(stamp_type_t type)
{
	static const char *names[STAMP_COUNT] = { "crater", "dam", "hill", "channel", "lake", "plateau" };

	return (type >= 0 && type < STAMP_COUNT) ? names[type] : "?";
}

bool stamps_is_line(stamp_type_t type)
{
	return type == STAMP_DAM || type == STAMP_CHANNEL;
}

static int32_t meters_to_units(float meters)
{
	return (int32_t)SDL_lroundf(meters * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
}

// Distance (meters, horizontal) from a point to the segment start-end, and
// how far along the segment the closest point lies (0..1)
static float distance_to_segment(float point_x, float point_z, vec3_t start, vec3_t end, float *along_out)
{
	float segment_x = end.x - start.x;
	float segment_z = end.z - start.z;
	float length_squared = segment_x * segment_x + segment_z * segment_z;
	float along = 0.0f;
	float closest_x;
	float closest_z;

	if (length_squared > 1e-6f)
	{
		along = clamp_float(((point_x - start.x) * segment_x + (point_z - start.z) * segment_z) / length_squared, 0.0f, 1.0f);
	}
	closest_x = start.x + segment_x * along;
	closest_z = start.z + segment_z * along;
	if (along_out != NULL)
	{
		*along_out = along;
	}
	return SDL_sqrtf((point_x - closest_x) * (point_x - closest_x) + (point_z - closest_z) * (point_z - closest_z));
}

// Lowest and highest ground along the segment
static void ground_range_along(const terrain_t *terrain, vec3_t start, vec3_t end, float *lowest_out, float *highest_out)
{
	float length = SDL_sqrtf((end.x - start.x) * (end.x - start.x) + (end.z - start.z) * (end.z - start.z));
	int sample_count = SDL_max(2, (int)(length / (terrain->cell_size_meters * 0.5f)) + 1);

	*lowest_out = 1e30f;
	*highest_out = -1e30f;
	for (int i = 0; i < sample_count; i++)
	{
		float along = (float)i / (float)(sample_count - 1);
		float height = terrain_sample_height_meters(terrain, lerp_float(start.x, end.x, along), lerp_float(start.z, end.z, along));

		*lowest_out = SDL_min(*lowest_out, height);
		*highest_out = SDL_max(*highest_out, height);
	}
}

static float dam_crest_meters(const terrain_t *terrain, const stamp_t *stamp)
{
	float start_ground = terrain_sample_height_meters(terrain, stamp->start.x, stamp->start.z);
	float end_ground = terrain_sample_height_meters(terrain, stamp->end.x, stamp->end.z);

	return SDL_max(start_ground, end_ground) + stamp->height_meters;
}

// A dam's crest lies above the ground at both of its ends, so water would
// simply walk around them. Like a real dam it is keyed into the banks: each
// end is extended outwards along the dam's line until the ground there
// reaches the crest (or a limit is hit).
static void extend_dam_into_banks(const terrain_t *terrain, const stamp_t *stamp, float crest, vec3_t *start_out, vec3_t *end_out)
{
	float direction_x = stamp->end.x - stamp->start.x;
	float direction_z = stamp->end.z - stamp->start.z;
	float length = SDL_sqrtf(direction_x * direction_x + direction_z * direction_z);
	float step = terrain->cell_size_meters * 0.5f;
	float limit = SDL_min(STAMP_MAXIMUM_FOOTPRINT_METERS, SDL_max(length, 20.0f));

	*start_out = stamp->start;
	*end_out = stamp->end;
	if (length < 1e-3f)
	{
		return;
	}
	direction_x /= length;
	direction_z /= length;
	for (int side = 0; side < 2; side++)
	{
		vec3_t *point = side == 0 ? start_out : end_out;
		float sign = side == 0 ? -1.0f : 1.0f;

		for (float travelled = 0.0f; travelled < limit; travelled += step)
		{
			float x = point->x + sign * direction_x * step;
			float z = point->z + sign * direction_z * step;

			if (terrain_sample_height_meters(terrain, point->x, point->z) >= crest
				|| x < 0.0f || z < 0.0f || x > (float)terrain->size * terrain->cell_size_meters || z > (float)terrain->size * terrain->cell_size_meters)
			{
				break;
			}
			point->x = x;
			point->z = z;
		}
	}
}

float stamps_footprint_meters(const terrain_t *terrain, const stamp_t *stamp)
{
	float radius = SDL_max(stamp->radius_meters, terrain->cell_size_meters);
	float height = SDL_max(stamp->height_meters, 0.0f);
	float footprint = radius;

	switch (stamp->type)
	{
		case STAMP_CRATER:
			footprint = radius * 1.5f;
			break;
		case STAMP_DAM:
		{
			float lowest;
			float highest;

			ground_range_along(terrain, stamp->start, stamp->end, &lowest, &highest);
			footprint = DAM_CREST_HALF_WIDTH_METERS + (dam_crest_meters(terrain, stamp) - lowest) / DAM_SIDE_SLOPE;
			break;
		}
		case STAMP_CHANNEL:
		{
			float lowest;
			float highest;
			float start_ground = terrain_sample_height_meters(terrain, stamp->start.x, stamp->start.z);
			float end_ground = terrain_sample_height_meters(terrain, stamp->end.x, stamp->end.z);

			// The sides climb from the lowest bottom point to the highest
			// ground the channel crosses
			ground_range_along(terrain, stamp->start, stamp->end, &lowest, &highest);
			footprint = radius * CHANNEL_BOTTOM_HALF_WIDTH_FRACTION
				+ (highest - (SDL_min(start_ground, end_ground) - height)) / CHANNEL_SIDE_SLOPE;
			break;
		}
		case STAMP_PLATEAU:
			footprint = radius * PLATEAU_TOP_FRACTION + height / PLATEAU_SIDE_SLOPE;
			break;
		default:
			break;
	}
	return clamp_float(footprint, terrain->cell_size_meters, STAMP_MAXIMUM_FOOTPRINT_METERS);
}

// Fills the lake basin around the stamp point with water up to just below
// its lowest rim point
static void fill_lake(const terrain_t *terrain, water_t *water, const stamp_t *stamp, int32_t bounds[4])
{
	float cell_size = terrain->cell_size_meters;
	float radius = SDL_max(stamp->radius_meters, cell_size * 2.0f);
	int32_t rim_units = INT32_MAX;
	int32_t level_units;

	for (int32_t y = bounds[1]; y <= bounds[3]; y++)
	{
		for (int32_t x = bounds[0]; x <= bounds[2]; x++)
		{
			float distance = SDL_sqrtf(((float)x * cell_size - stamp->start.x) * ((float)x * cell_size - stamp->start.x)
				+ ((float)y * cell_size - stamp->start.z) * ((float)y * cell_size - stamp->start.z));

			if (distance >= radius - 1.5f * cell_size && distance <= radius)
			{
				rim_units = SDL_min(rim_units, terrain->heights[terrain_index(terrain, x, y)]);
			}
		}
	}
	if (rim_units == INT32_MAX || water == NULL)
	{
		return;
	}
	level_units = rim_units - meters_to_units(LAKE_FREEBOARD_METERS);
	for (int32_t y = bounds[1]; y <= bounds[3]; y++)
	{
		for (int32_t x = bounds[0]; x <= bounds[2]; x++)
		{
			float distance = SDL_sqrtf(((float)x * cell_size - stamp->start.x) * ((float)x * cell_size - stamp->start.x)
				+ ((float)y * cell_size - stamp->start.z) * ((float)y * cell_size - stamp->start.z));
			int32_t index = terrain_index(terrain, x, y);
			int32_t surface = terrain->heights[index] + water->depth[index];

			if (distance < radius && surface < level_units)
			{
				water_add_units(water, x, y, level_units - surface);
			}
		}
	}
}

void stamps_apply(terrain_t *terrain, water_t *water, const stamp_t *stamp)
{
	float cell_size = terrain->cell_size_meters;
	float radius = SDL_max(stamp->radius_meters, cell_size * 2.0f);
	float height = SDL_max(stamp->height_meters, 0.0f);
	float footprint = stamps_footprint_meters(terrain, stamp);
	vec3_t start = stamp->start;
	vec3_t end = stamps_is_line(stamp->type) ? stamp->end : stamp->start;
	float center_ground = terrain_sample_height_meters(terrain, stamp->start.x, stamp->start.z);
	float start_ground = center_ground;
	float end_ground = terrain_sample_height_meters(terrain, end.x, end.z);
	float dam_crest = stamp->type == STAMP_DAM ? dam_crest_meters(terrain, stamp) : 0.0f;
	float lake_rim = 1e30f;
	int32_t bounds[4];
	int32_t changed_cells = 0;

	if (stamp->type == STAMP_DAM)
	{
		extend_dam_into_banks(terrain, stamp, dam_crest, &start, &end);
	}
	if (stamp->type == STAMP_LAKE)
	{
		// The lake is dug relative to the lowest point of its rim, so that
		// on a slope it still holds water (the uphill side is cut down)
		for (int i = 0; i < 64; i++)
		{
			float angle = (float)i * (2.0f * MATH3D_PI / 64.0f);

			lake_rim = SDL_min(lake_rim, terrain_sample_height_meters(terrain, stamp->start.x + SDL_cosf(angle) * radius,
				stamp->start.z + SDL_sinf(angle) * radius));
		}
	}
	bounds[0] = clamp_int((int32_t)SDL_floorf((SDL_min(start.x, end.x) - footprint) / cell_size), 0, terrain->size - 1);
	bounds[1] = clamp_int((int32_t)SDL_floorf((SDL_min(start.z, end.z) - footprint) / cell_size), 0, terrain->size - 1);
	bounds[2] = clamp_int((int32_t)SDL_ceilf((SDL_max(start.x, end.x) + footprint) / cell_size), 0, terrain->size - 1);
	bounds[3] = clamp_int((int32_t)SDL_ceilf((SDL_max(start.z, end.z) + footprint) / cell_size), 0, terrain->size - 1);

	for (int32_t y = bounds[1]; y <= bounds[3]; y++)
	{
		for (int32_t x = bounds[0]; x <= bounds[2]; x++)
		{
			int32_t index = terrain_index(terrain, x, y);
			float along;
			float distance = distance_to_segment((float)x * cell_size, (float)y * cell_size, start, end, &along);
			int32_t old_height = terrain->heights[index];
			int32_t raise_to = INT32_MIN;  // the shape, where it lies above the ground
			int32_t lower_to = INT32_MAX;  // the shape, where it lies below the ground
			int32_t add = 0;

			switch (stamp->type)
			{
				case STAMP_CRATER:
				{
					// A raised rim around the bowl (the material is not
					// counted: craters are for fun, not bookkeeping)
					float rim = (distance - radius) / (0.5f * radius);

					if (SDL_fabsf(rim) < 1.0f)
					{
						add = meters_to_units(0.25f * height * (1.0f - rim * rim) * (1.0f - rim * rim));
					}
					if (distance < radius)
					{
						float fraction = distance / radius;

						lower_to = meters_to_units(center_ground - height * (1.0f - fraction * fraction));
					}
					break;
				}
				case STAMP_DAM:
					raise_to = meters_to_units(dam_crest - SDL_max(0.0f, distance - DAM_CREST_HALF_WIDTH_METERS) * DAM_SIDE_SLOPE);
					break;
				case STAMP_HILL:
					if (distance < radius)
					{
						add = meters_to_units(height * 0.5f * (1.0f + SDL_cosf(MATH3D_PI * distance / radius)));
					}
					break;
				case STAMP_CHANNEL:
				{
					float bottom = lerp_float(start_ground, end_ground, along) - height;

					lower_to = meters_to_units(bottom + SDL_max(0.0f, distance - radius * CHANNEL_BOTTOM_HALF_WIDTH_FRACTION) * CHANNEL_SIDE_SLOPE);
					break;
				}
				case STAMP_LAKE:
					if (distance < radius)
					{
						lower_to = meters_to_units(lake_rim - height * (1.0f - smoothstep_float(0.55f * radius, radius, distance)));
					}
					break;
				case STAMP_PLATEAU:
					raise_to = meters_to_units(center_ground + height
						- SDL_max(0.0f, distance - radius * PLATEAU_TOP_FRACTION) * PLATEAU_SIDE_SLOPE);
					break;
				default:
					break;
			}

			if (add > 0)
			{
				terrain->heights[index] += add;
			}
			if (raise_to > terrain->heights[index])
			{
				terrain->heights[index] = raise_to;
			}
			if (lower_to < terrain->heights[index])
			{
				terrain->heights[index] = SDL_max(lower_to, 0);
			}
			if (terrain->heights[index] > old_height)
			{
				// New material of the selected soil; silt that was there is
				// buried and becomes part of it
				terrain->soil_types[index] = (uint8_t)stamp->soil;
				terrain->silt_thickness[index] = 0;
			}
			if (terrain->heights[index] != old_height)
			{
				changed_cells++;
			}
		}
	}

	terrain_fix_layers(terrain, bounds[0], bounds[1], bounds[2], bounds[3]);
	if (stamp->type == STAMP_LAKE)
	{
		fill_lake(terrain, water, stamp, bounds);
	}
	terrain_mark_cells_changed(terrain, bounds[0], bounds[1], bounds[2], bounds[3]);
	if (water != NULL)
	{
		water_wake_cells(water, bounds[0], bounds[1], bounds[2], bounds[3]);
	}
	SDL_Log("Stamp %s: %d cells changed, height %.1f m, footprint %.1f m", stamps_name(stamp->type), changed_cells,
		(double)height, (double)footprint);
}
