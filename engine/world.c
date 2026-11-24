// The world. See world.h.

#include "world.h"

#include <SDL3/SDL.h>

#include "material_map.h"
#include "populate.h"
#include "profile.h"
#include "terrain_cache.h"
#include "terrain_gen.h"
#include "session.h"
#include "water_gpu.h"

// The player changed terrain or water (a tool, a spring, rain, a new map):
// the water runs again, and the freeze timer starts over. Objects falling
// into the water do not count; only the player does.
void world_touch_water(world_t *world)
{
	if (world->water_frozen)
	{
		SDL_Log("Water simulation running again");
	}
	world->water_frozen = false;
	world->water_quiet_seconds = 0.0f;
}

// A map of another size (or cell size): terrain, water, erosion and the
// objects' grid are made anew, empty. The renderers follow on their own
// (their textures are sized at the next upload). Returns false if memory
// ran out; the game cannot go on then.
bool world_resize(world_t *world, int32_t size, float cell_size_meters, bool baked)
{
	bool drain_edges = world->water.drain_edges;
	bool depth_scaled_pipes = world->water.depth_scaled_pipes;
	bool created;

	if (world->terrain.size == size && world->terrain.cell_size_meters == cell_size_meters && world->terrain.baked == baked)
	{
		return true;
	}
	SDL_Log("Map size %dx%d cells of %.2f m%s -> %dx%d cells of %.2f m%s", world->terrain.size, world->terrain.size,
		(double)world->terrain.cell_size_meters, world->terrain.baked ? ", baked" : "", size, size, (double)cell_size_meters, baked ? ", baked" : "");
	erosion_destroy(&world->erosion);
	water_destroy(&world->water);
	terrain_destroy(&world->terrain);
	if (baked)
	{
		created = terrain_create_baked(&world->terrain, size, SDL_max(cell_size_meters, 0.01f))
			&& water_create_baked(&world->water, world->terrain.size, world->terrain.cell_size_meters);
	}
	else
	{
		created = terrain_create(&world->terrain, size, SDL_max(cell_size_meters, 0.01f))
			&& water_create(&world->water, world->terrain.size, world->terrain.cell_size_meters, drain_edges, depth_scaled_pipes);
	}
	if (!created || !erosion_create(&world->erosion, &world->water))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory making a %dx%d map", size, size);
		return false;
	}
	if (world->objects.pipeline != NULL && !objects_resize_map(&world->objects, &world->terrain))
	{
		return false;
	}
	world->shadow.needs_render = true;
	world->near_shadow.needs_render = true;
	return true;
}

// What was put on the map, for saving it: the placed lights (as they are),
// the effects placed (copies; free effects), and for a baked one its
// waterfalls (a baked map's as they are; an active one's looked for over
// all of it now, into search, which the caller destroys)
// The actors for the map's file: their type, place, name, the game's
// number for them, dead or not
void world_gather_actors(world_t *world, map_features_t *features)
{
	features->actors_saved = true;
	features->actor_count = 0;
	features->actors = world->actors.count > 0 ? SDL_calloc((size_t)world->actors.count, sizeof(map_actor_t)) : NULL;
	for (int i = 0; i < world->actors.count && features->actors != NULL; i++)
	{
		const actor_t *actor = &world->actors.actors[i];
		map_actor_t *record = &features->actors[features->actor_count++];

		SDL_strlcpy(record->type, world->actors.types[actor->type].name, sizeof(record->type));
		record->x = actor->x;
		record->z = actor->z;
		record->yaw_radians = actor->yaw_radians;
		SDL_strlcpy(record->name, actor->name, sizeof(record->name));
		record->game_id = actor->game_id;
		record->hostile = actor->hostile ? 1 : 0;
		record->dead = actors_is_dead(&world->actors, actor->id) ? 1 : 0;
	}
}

// The areas for the map's file, as they are
void world_gather_areas(world_t *world, map_features_t *features)
{
	features->areas_saved = true;
	features->area_count = 0;
	features->areas = world->areas.count > 0 ? SDL_calloc((size_t)world->areas.count, sizeof(map_area_t)) : NULL;
	for (int i = 0; i < world->areas.count && features->areas != NULL; i++)
	{
		const area_t *area = &world->areas.areas[i];
		map_area_t *record = &features->areas[features->area_count++];

		record->id = area->id;
		SDL_strlcpy(record->name, area->name, sizeof(record->name));
		record->x = area->x;
		record->z = area->z;
		record->half_width_meters = area->half_width_meters;
		record->half_depth_meters = area->half_depth_meters;
		record->yaw_radians = area->yaw_radians;
		record->bottom_meters = area->bottom_meters;
		record->top_meters = area->top_meters;
		record->triggers = area->triggers;
		record->blocks = area->blocks;
	}
}

// A loaded map's areas instead of the ones there were (none, if it has
// none), with their ids
void world_apply_areas(world_t *world, const map_features_t *features)
{
	areas_clear(&world->areas);
	for (int i = 0; i < features->area_count; i++)
	{
		const map_area_t *record = &features->areas[i];

		if (areas_add(&world->areas, record->name, record->x, record->z, 2.0f * record->half_width_meters, 2.0f * record->half_depth_meters,
			record->yaw_radians, (uint8_t)record->triggers) == 0)
		{
			break;
		}
		if (!areas_set_id(&world->areas, world->areas.count - 1, record->id))
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: area %s has an id another has: given a new one", record->name);
		}
		world->areas.areas[world->areas.count - 1].bottom_meters = record->bottom_meters;
		world->areas.areas[world->areas.count - 1].top_meters = record->top_meters;
		world->areas.areas[world->areas.count - 1].blocks = (uint8_t)record->blocks;
	}
}

// A loaded map's actors instead of the ones there were (a map from before
// they were kept: left as they are). Their ids are new: the game finds its
// own by their numbers (actors_find_by_game_id)
void world_apply_actors(world_t *world, const map_features_t *features)
{
	if (!features->actors_saved)
	{
		return;
	}
	actors_clear(&world->actors);
	for (int i = 0; i < features->actor_count; i++)
	{
		const map_actor_t *record = &features->actors[i];
		int type = actors_find_type(&world->actors, record->type);
		uint32_t id;

		if (type < 0 || world->actors.types[type].model < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: actors.ini has no %s any more: left out", record->type);
			continue;
		}
		id = actors_add(&world->actors, &world->terrain, type, record->x, record->z, record->yaw_radians);
		if (id == 0)
		{
			continue;
		}
		actors_set_name(&world->actors, id, record->name, record->hostile != 0);
		actors_set_game_id(&world->actors, id, record->game_id);
		if (record->dead)
		{
			actors_set_dead(&world->actors, id);
		}
	}
	{
		actor_event_t event;

		// (what the load did is no news)
		while (actors_poll_event(&world->actors, &event))
		{
		}
	}
}

// What was picked off the objects, for the map's file
void world_gather_harvest(world_t *world, map_features_t *features)
{
	int count = 0;

	for (int i = 0; i < world->objects.instance_count; i++)
	{
		count += world->objects.instances[i].harvested > 0 || world->objects.instances[i].harvest_original_type >= 0 ? 1 : 0;
	}
	features->harvest = count > 0 ? SDL_calloc((size_t)count, sizeof(map_harvest_t)) : NULL;
	features->harvest_count = 0;
	for (int i = 0; i < world->objects.instance_count && features->harvest != NULL; i++)
	{
		const object_instance_t *instance = &world->objects.instances[i];
		map_harvest_t *record;

		if (instance->harvested == 0 && instance->harvest_original_type < 0)
		{
			continue;
		}
		record = &features->harvest[features->harvest_count++];
		SDL_strlcpy(record->type, world->objects.types.types[instance->type].name, sizeof(record->type));
		record->x = instance->x;
		record->z = instance->z;
		record->harvested = instance->harvested;
		record->regrow_hours = instance->regrow_hours;
		if (instance->harvest_original_type >= 0)
		{
			SDL_strlcpy(record->original, world->objects.types.types[instance->harvest_original_type].name, sizeof(record->original));
		}
	}
}

// A loaded map's picking: each record's object found by its type and place
void world_apply_harvest(world_t *world, const map_features_t *features)
{
	int found = 0;

	for (int r = 0; r < features->harvest_count; r++)
	{
		const map_harvest_t *record = &features->harvest[r];
		int type = object_types_find(&world->objects.types, record->type);

		for (int i = 0; i < world->objects.instance_count && type >= 0; i++)
		{
			object_instance_t *instance = &world->objects.instances[i];

			if (instance->type == type && SDL_fabsf(instance->x - record->x) < 0.01f && SDL_fabsf(instance->z - record->z) < 0.01f)
			{
				instance->harvested = (uint8_t)SDL_clamp(record->harvested, 0, 255);
				instance->regrow_hours = record->regrow_hours;
				instance->harvest_original_type = record->original[0] != '\0' ? object_types_find(&world->objects.types, record->original) : -1;
				found++;
				break;
			}
		}
	}
	// (the objects waiting to grow again: found among them all at the next look)
	world->objects.regrow_known = false;
	if (features->harvest_count > 0)
	{
		SDL_Log("Map: %d of %d picked objects found again", found, features->harvest_count);
	}
}

// The sketch's lakes (terrain_sketch.h, found when the land was made):
// water up to each one's surface wherever the land under it is lower and
// joined to its middle (from where its hint is over a half, out through
// the land under its surface, no farther than its own texels): the shore
// where the land meets the water, no puddles past its rim
static void fill_sketch_lakes(world_t *world)
{
	const terrain_sketch_t *sketch = world->generation_sketch;
	terrain_t *terrain = &world->terrain;
	int32_t size = terrain->size;
	size_t cell_count = (size_t)size * (size_t)size;
	uint8_t *seen = SDL_calloc(cell_count, 1);
	uint32_t *stack = NULL;
	size_t stack_capacity = 0;
	int64_t filled = 0;
	static const int32_t steps[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

	if (seen == NULL)
	{
		return;
	}
	for (int32_t y = 0; y < size; y++)
	{
		for (int32_t x = 0; x < size; x++)
		{
			size_t start = (size_t)y * (size_t)size + (size_t)x;
			float u = ((float)x + 0.5f) / (float)size;
			float v = ((float)y + 0.5f) / (float)size;
			int lake;
			int32_t surface;
			size_t stack_count = 0;

			if (seen[start] || terrain_sketch_lake_hint(sketch, u, v) < 0.5f)
			{
				continue;
			}
			lake = terrain_sketch_lake_at(sketch, u, v);
			if (lake < 0)
			{
				continue;
			}
			surface = (int32_t)(sketch->lake_surfaces[lake] * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
			if (terrain->heights[terrain_index(terrain, x, y)] >= surface)
			{
				continue;
			}
			seen[start] = 1;
			if (stack_capacity == 0)
			{
				stack_capacity = 65536;
				stack = SDL_malloc(stack_capacity * sizeof(uint32_t));
				if (stack == NULL)
				{
					SDL_free(seen);
					return;
				}
			}
			stack[stack_count++] = (uint32_t)start;
			while (stack_count > 0)
			{
				uint32_t cell = stack[--stack_count];
				int32_t cell_x = (int32_t)(cell % (uint32_t)size);
				int32_t cell_y = (int32_t)(cell / (uint32_t)size);
				int32_t ground = terrain->heights[terrain_index(terrain, cell_x, cell_y)];

				water_add_units(&world->water, cell_x, cell_y, surface - ground);
				filled++;
				for (int k = 0; k < 4; k++)
				{
					int32_t next_x = cell_x + steps[k][0];
					int32_t next_y = cell_y + steps[k][1];
					size_t next;

					if (next_x < 0 || next_y < 0 || next_x >= size || next_y >= size)
					{
						continue;
					}
					next = (size_t)next_y * (size_t)size + (size_t)next_x;
					if (seen[next] || terrain->heights[terrain_index(terrain, next_x, next_y)] >= surface
						|| terrain_sketch_lake_at(sketch, ((float)next_x + 0.5f) / (float)size, ((float)next_y + 0.5f) / (float)size) != lake
						|| terrain_sketch_lake_hint(sketch, ((float)next_x + 0.5f) / (float)size, ((float)next_y + 0.5f) / (float)size)
							< TERRAIN_SKETCH_LAKE_HOLDS)
					{
						continue;
					}
					seen[next] = 1;
					if (stack_count == stack_capacity)
					{
						uint32_t *grown = SDL_realloc(stack, stack_capacity * 2 * sizeof(uint32_t));

						if (grown == NULL)
						{
							continue;
						}
						stack = grown;
						stack_capacity *= 2;
					}
					stack[stack_count++] = (uint32_t)next;
				}
			}
		}
	}
	SDL_free(stack);
	SDL_free(seen);
	SDL_Log("Sketch: %d lakes, %lld cells under water", sketch->lake_count, (long long)filled);
}

bool world_generate(world_t *world, uint32_t seed)
{
	bool loaded = false;
	bool sketched = terrain_sketch_painted(world->generation_sketch);

	// (a new map has no areas, nothing heard)
	areas_clear(&world->areas);
	noises_clear(&world->noises);
	// (a new map is an active one)
	if (world->terrain.baked && !world_resize(world, world->terrain.size, world->terrain.cell_size_meters, false))
	{
		return false;
	}

	// (a sketched map is its own: not kept with the seed's, nor taken from them)
	if (world->settings->cache.use_cache && !sketched)
	{
		loaded = terrain_cache_load(&world->terrain, &world->settings->terrain, world->soils, seed);
	}
	if (!loaded)
	{
		terrain_generate(&world->terrain, &world->settings->terrain, world->soils, seed, world->generation_sketch);
		if (world->settings->cache.use_cache && !sketched)
		{
			terrain_cache_save(&world->terrain, &world->settings->terrain, world->soils, &world->settings->cache);
		}
	}
	// (no cover, no overlay: the objects that put them there are new too;
	// nothing placed on it yet, its waterfalls looked for)
	material_map_clear(&world->terrain);
	if (world->particles.created)
	{
		map_features_t none;

		SDL_zero(none);
		world_apply_features(world, &none);
	}
	world->ground_overlay.cleared = false;
	water_ice_clear(&world->ice);
	if (world->water.depth != NULL)
	{
		water_clear(&world->water);
		if (sketched && world->generation_sketch->lake_count > 0 && !world->terrain.baked)
		{
			fill_sketch_lakes(world);
		}
	}
	erosion_reset(&world->erosion);
	world->shadow.needs_render = true;
	// A new map: the old objects have no place on it (the objects exist
	// only once the window does; the first map is populated after that)
	if (world->objects.pipeline != NULL)
	{
		objects_clear(&world->objects, false);
		if (world->settings->objects.populate)
		{
			populate_map(&world->objects, &world->terrain, &world->water, &world->settings->objects);
			layouts_populate_map(&world->layouts, &world->objects, &world->terrain, &world->water, &world->settings->objects);
		}
		material_map_restamp(&world->terrain, &world->objects);
		physics_reset(&world->physics, &world->objects, &world->terrain);
	}
	return true;
}

void world_gather_features(world_t *world, bool baked, map_features_t *features, waterfalls_t *search)
{
	SDL_zerop(features);
	features->lights = world->lights.placed;
	features->light_count = world->lights.placed_count;
	features->effects = SDL_calloc(PARTICLES_EMITTERS_MAX, sizeof(map_effect_t));
	for (int i = 0; i < PARTICLES_EMITTERS_MAX && features->effects != NULL; i++)
	{
		const particles_emitter_t *emitter = &world->particles.emitters[i];
		map_effect_t *effect = &features->effects[features->effect_count];

		if (!emitter->used || !emitter->kept || emitter->effect < 0 || emitter->effect >= world->particles.effect_count)
		{
			continue;
		}
		SDL_strlcpy(effect->effect, world->particles.effects[emitter->effect].name, sizeof(effect->effect));
		SDL_memcpy(effect->position, emitter->position, sizeof(effect->position));
		SDL_memcpy(effect->rotation, emitter->rotation, sizeof(effect->rotation));
		SDL_memcpy(effect->extents, emitter->extents, sizeof(effect->extents));
		features->effect_count++;
	}
	features->sky_occlusion = world->sky_occlusion.map_size == world->terrain.size
		? sky_occlusion_download(&world->sky_occlusion, &features->sky_occlusion_layers, &features->sky_occlusion_bounce) : NULL;
	if (features->sky_occlusion == NULL || features->sky_occlusion_layers == NULL || features->sky_occlusion_bounce == NULL)
	{
		SDL_free(features->sky_occlusion);
		SDL_free(features->sky_occlusion_layers);
		SDL_free(features->sky_occlusion_bounce);
		features->sky_occlusion = NULL;
		features->sky_occlusion_layers = NULL;
		features->sky_occlusion_bounce = NULL;
	}
	features->sky_occlusion_size = features->sky_occlusion != NULL ? world->terrain.size : 0;
	features->sky_occlusion_layer_size = features->sky_occlusion != NULL ? world->sky_occlusion.layer_size : 0;
	features->sky_occlusion_bounce_size = features->sky_occlusion != NULL ? world->sky_occlusion.bounce_size : 0;
	world_gather_harvest(world, features);
	world_gather_actors(world, features);
	world_gather_areas(world, features);
	waterfalls_create(search);
	if (baked)
	{
		if (world->terrain.baked)
		{
			features->waterfalls = world->waterfalls.segments;
			features->waterfall_count = world->waterfalls.count;
		}
		else
		{
			waterfalls_search(search, &world->terrain, &world->water, 0.0f, 0.0f, -1.0f);
			features->waterfalls = search->segments;
			features->waterfall_count = search->count;
		}
	}
}

// What a loaded map brought: its lights and effects instead of the ones
// placed before, its waterfalls (an active map: looked for)
void world_apply_features(world_t *world, const map_features_t *features)
{
	lights_clear_placed(&world->lights);
	// (the old map's marks go with it)
	decals_clear(&world->decals);
	for (int i = 0; i < features->light_count; i++)
	{
		lights_place(&world->lights, features->lights[i].position, &features->lights[i].source);
	}
	for (int i = 0; i < PARTICLES_EMITTERS_MAX; i++)
	{
		if (world->particles.emitters[i].used && world->particles.emitters[i].kept)
		{
			particles_stop(&world->particles, world->particles.emitters[i].id);
		}
	}
	for (int i = 0; i < features->effect_count; i++)
	{
		int effect = particles_find_effect(&world->particles, features->effects[i].effect);
		particles_emitter_t *emitter;

		if (effect < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Map: particles.ini has no effect %s any more: left out", features->effects[i].effect);
			continue;
		}
		emitter = particles_find_emitter(&world->particles, particles_play_on(&world->particles, effect, features->effects[i].position,
			features->effects[i].rotation, features->effects[i].extents));
		if (emitter != NULL)
		{
			emitter->kept = true;
		}
	}
	waterfalls_set(&world->waterfalls, &world->particles, features->waterfalls, features->waterfall_count, world->terrain.baked);
	world_apply_harvest(world, features);
	world_apply_actors(world, features);
	world_apply_areas(world, features);
	// Its sky occlusion; without it worked out (render_frame) when it is used
	sky_occlusion_forget(&world->sky_occlusion, false);
	if (features->sky_occlusion != NULL && features->sky_occlusion_size == world->terrain.size && features->sky_occlusion_layers != NULL
		&& features->sky_occlusion_bounce != NULL)
	{
		sky_occlusion_upload(&world->sky_occlusion, features->sky_occlusion_size, features->sky_occlusion, features->sky_occlusion_layer_size,
			features->sky_occlusion_layers, features->sky_occlusion_bounce_size, features->sky_occlusion_bounce);
	}
}

void world_step(world_t *world, float step_seconds)
{
	uint64_t water_start = SDL_GetPerformanceCounter();
	uint64_t erosion_start;
	uint64_t erosion_end;
	double milliseconds_per_tick = 1000.0 / (double)SDL_GetPerformanceFrequency();

	// Rain keeps the water running; otherwise it stops once the player
	// has left it alone long enough
	if (world->water.raining)
	{
		world_touch_water(world);
	}
	world->water_stepped = false;
	world->water_quiet_seconds += step_seconds;
	if (!world->water_frozen && world->settings->water.freeze_after_seconds > 0.0f
		&& world->water_quiet_seconds >= world->settings->water.freeze_after_seconds)
	{
		world->water_frozen = true;
		SDL_Log("Water simulation frozen after %.0f s without changes (F7 or any tool starts it again)",
			(double)world->settings->water.freeze_after_seconds);
	}
	if (!world->water_frozen && !world->water_iced)
	{
		uint64_t section_start = SDL_GetPerformanceCounter();

		water_step(&world->water, &world->terrain, &world->settings->water, world->soils, step_seconds);
		frame_profile_end(FRAME_WATER, section_start);
		world->water_stepped = true;
	}
	erosion_start = SDL_GetPerformanceCounter();
	if (!world->water_frozen && !world->water_iced && !world->no_erosion)
	{
		// On the GPU, after the water's step (the ground's moisture on the
		// CPU)
		erosion_step(&world->erosion, &world->terrain, &world->water, world->soils, &world->settings->water, step_seconds,
			(int64_t)(world->settings->render.shadow_erosion_meters * (float)TERRAIN_HEIGHT_UNITS_PER_METER));
	}
	erosion_end = SDL_GetPerformanceCounter();
	if (!world->physics_off)
	{
		physics_step(&world->physics, &world->objects, &world->terrain, &world->water, &world->settings->physics, step_seconds);
	}
	frame_profile_end(FRAME_PHYSICS, erosion_end);
	world->water_seconds_sum += (double)(erosion_start - water_start) * milliseconds_per_tick * 0.001;
	world->erosion_seconds_sum += (double)(erosion_end - erosion_start) * milliseconds_per_tick * 0.001;
	world->physics_seconds_sum += (double)(SDL_GetPerformanceCounter() - erosion_end) * milliseconds_per_tick * 0.001;
	world->diagnostics.water_milliseconds_sum += (double)(erosion_start - water_start) * milliseconds_per_tick;
	world->diagnostics.water_milliseconds_max = SDL_max(world->diagnostics.water_milliseconds_max,
		(double)(erosion_start - water_start) * milliseconds_per_tick);
	world->diagnostics.erosion_milliseconds_sum += (double)(erosion_end - erosion_start) * milliseconds_per_tick;
	world->diagnostics.erosion_milliseconds_max = SDL_max(world->diagnostics.erosion_milliseconds_max,
		(double)(erosion_end - erosion_start) * milliseconds_per_tick);
	world->diagnostics.step_count++;
	world->diagnostics.simulated_seconds += step_seconds;
	world->simulation_step_count++;
}

bool world_save(world_t *world, session_t *session, const char *path, bool baked, const camera_t *camera)
{
	map_features_t features;
	waterfalls_t search;
	bool saved;

	if (world->terrain.baked && !baked)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A baked map can only be saved baked");
		return false;
	}
	// The water as it is now, all of it, from the GPU; the material map as
	// it would be drawn
	water_gpu_sync(&world->water, &world->terrain);
	terrain_renderer_bake_materials(&world->renderer, &world->terrain, &world->lit_render);
	// The objects gone in the game (OBJECT_STATE_DEAD) taken out of the list
	// now, and only now (the editor's save, the game's): sorted again after
	objects_compact_dead(&world->objects);
	world_gather_features(world, baked, &features, &search);
	if (session != NULL)
	{
		session_gather_items(session, world, &features);
	}
	if (baked)
	{
		saved = map_file_save_baked(path, &world->terrain, &world->water, &world->objects, camera, &world->cloud_paint, world->soils, &features);
	}
	else
	{
		saved = map_file_save(path, &world->terrain, &world->water, &world->objects, camera, &world->cloud_paint, &features);
	}
	SDL_free(features.effects);
	SDL_free(features.sky_occlusion);
	SDL_free(features.sky_occlusion_layers);
	SDL_free(features.sky_occlusion_bounce);
	SDL_free(features.items);
	SDL_free(features.harvest);
	SDL_free(features.actors);
	SDL_free(features.areas);
	waterfalls_destroy(&search, NULL);
	return saved;
}

bool world_load(world_t *world, session_t *session, const char *path, map_file_info_t *info_out, bool *complete_out)
{
	map_file_reader_t *reader;
	map_file_info_t info;
	bool complete;

	*complete_out = false;
	reader = map_file_open(path, &info);
	if (reader == NULL)
	{
		return false;
	}
	if (!world_resize(world, info.size, info.cell_size_meters, info.baked))
	{
		map_file_close(reader);
		return false;
	}
	{
		map_features_t features;

		SDL_zero(features);
		complete = map_file_read(reader, &world->terrain, &world->water, &world->objects, &world->cloud_paint, &features);
		world_apply_features(world, &features);
		if (session != NULL)
		{
			session_apply_items(session, world, &features);
		}
		map_features_free(&features);
	}
	map_file_close(reader);
	// (the ice as the season says)
	water_ice_clear(&world->ice);
	// (a map from before the material map: the cover from its objects)
	if (world->terrain.materials_dirty)
	{
		material_map_restamp(&world->terrain, &world->objects);
	}
	// (what the game can do something with, it says again for this map)
	objects_clear_interactive(&world->objects);
	physics_reset(&world->physics, &world->objects, &world->terrain);
	navigation_forget(&world->navigation, &world->terrain, &world->objects);
	fog_forget(&world->fog);
	erosion_reset(&world->erosion);
	// A saved map starts with still water: let it settle again
	world_touch_water(world);
	world->shadow.needs_render = true;
	world->near_shadow.needs_render = true;
	world->simulation_accumulator_seconds = 0.0;
	world->frames_since_load = 0;
	noises_clear(&world->noises);
	SDL_Log("Load: the map takes %.0f MB on the CPU (the ground %.0f, the water %.0f): %s", (double)(terrain_memory_bytes(&world->terrain)
		+ water_memory_bytes(&world->water)) / (1024.0 * 1024.0), (double)terrain_memory_bytes(&world->terrain) / (1024.0 * 1024.0),
		(double)water_memory_bytes(&world->water) / (1024.0 * 1024.0), world->terrain.baked ? "baked" : "active");
	*info_out = info;
	*complete_out = complete;
	return true;
}
