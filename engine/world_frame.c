// A frame of the world (world.h: world_begin_frame, world_update_actors,
// world_simulate, world_end_frame)

#include "world.h"

#include <SDL3/SDL.h>

#include "profile.h"
#include "water_gpu.h"

static double milliseconds_since(uint64_t start)
{
	return 1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();
}

void world_begin_frame(world_t *world, double frame_seconds)
{
	const render_settings_t *render = &world->settings->render;

	world->elapsed_seconds += frame_seconds;
	world->sky_seconds += frame_seconds * (double)world->sky_time_scale;
	if (render->day_cycle && !world->day_frozen && render->day_length_minutes > 0.0f)
	{
		double days = frame_seconds / (render->day_length_minutes * 60.0);

		world->day_hour = SDL_fmod(world->day_hour + days * 24.0, 24.0);
		world->harvest_hours_pending += days * 24.0;
		world->moon_age_days = SDL_fmod(world->moon_age_days + days, (double)SDL_max(render->moon_cycle_days, 1.0f));
	}
	daylight_apply(&world->settings->render, world->day_hour, world->moon_age_days, &world->lit_render, &world->daylight);
	// The houses' windows by the hour (objects_light_windows)
	objects_light_windows(&world->objects, world->day_hour, world->lit_render.night_amount);
	// The modules' moving parts, by their states (a mill's sails, a crane)
	objects_update_modules(&world->objects, (float)frame_seconds);
	// The rain follows the simulation's; it dims the light the frame is
	// drawn with
	world->rain.raining = world->water.raining;
	rain_update(&world->rain, (float)frame_seconds);
	rain_dim_light(&world->rain, &world->settings->render, &world->lit_render);
	trace_mark("the light");
	// Models and textures written while the game runs (the converter,
	// potop_plant) show at once
	if (world->elapsed_seconds - world->reload_check_seconds >= 1.0)
	{
		world->reload_check_seconds = world->elapsed_seconds;
		if (objects_reload_changed(&world->objects))
		{
			world->terrain.shadow_dirty = true;
		}
	}
	trace_mark("models changed on disk");
	// The ground under the buildings (what stands changed, or the map)
	ground_footprints_update(&world->ground_footprints, &world->terrain, &world->objects, (float)frame_seconds);
	trace_mark("the ground under the buildings");
}

void world_update_actors(world_t *world, float seconds)
{
	actors_move(&world->actors, &world->navigation, &world->terrain, &world->water, seconds);
	actors_update(&world->actors, seconds);
}

int world_simulate(world_t *world, double game_seconds, float focus_x, float focus_z, bool exactly_one, void (*step)(void *user, float step_seconds),
	void *user)
{
	const simulation_settings_t *simulation = &world->settings->simulation;
	const float step_seconds = 1.0f / (float)SDL_max(1, simulation->steps_per_second);
	uint64_t start = SDL_GetPerformanceCounter();
	int steps = 0;

	// (the bodies near the eye are made first; round the actors too: they
	// stand on them, their straight walks stop at them)
	physics_set_focus(&world->physics, focus_x, focus_z);
	for (int a = 0; a < world->actors.count; a++)
	{
		physics_request_bodies(&world->physics, &world->objects, world->actors.actors[a].x, world->actors.actors[a].z, PHYSICS_ACTOR_BODIES_METERS);
	}
	// The water the GPU sent back since the last frame
	{
		uint64_t poll_start = SDL_GetPerformanceCounter();

		water_gpu_poll(&world->water);
		frame_profile_end(FRAME_POLL, poll_start);
		world->water_copy_milliseconds = milliseconds_since(poll_start);
	}
	trace_mark("water copies in");
	world->simulation_accumulator_seconds += game_seconds;
	if (exactly_one)
	{
		world->simulation_accumulator_seconds = step_seconds;
	}
	// As many steps as the time asks for, never more than the most per
	// frame, and none once the frame's budget is spent: a machine that
	// cannot keep up slows the game down (the time left is dropped, not owed)
	while (world->simulation_accumulator_seconds >= step_seconds)
	{
		if (steps >= simulation->maximum_steps_per_frame
			|| (steps > 0 && !exactly_one && milliseconds_since(start) >= simulation->maximum_milliseconds_per_frame))
		{
			world->simulation_accumulator_seconds = 0.0;
			break;
		}
		if (step != NULL)
		{
			step(user, step_seconds);
		}
		else
		{
			world_step(world, step_seconds);
		}
		world->simulation_accumulator_seconds -= step_seconds;
		steps++;
	}
	trace_mark("simulation steps");
	return steps;
}

// The lights the actors carry follow them (a dead or gone actor's go out)
static void follow_actor_lights(world_t *world)
{
	const uint32_t first_key = 0x40000000u;

	for (int i = 0; i < LIGHTS_MOVING_MAX; i++)
	{
		moving_light_t *moving = &world->lights.moving[i];
		int index;
		const actor_t *actor;
		const actor_type_t *type;
		float top;

		if (!moving->used || moving->key < first_key)
		{
			continue;
		}
		index = actors_find(&world->actors, moving->key - first_key);
		if (index < 0 || actors_is_dead(&world->actors, moving->key - first_key))
		{
			moving->used = false;
			continue;
		}
		actor = &world->actors.actors[index];
		type = &world->actors.types[actor->type];
		top = type->model >= 0 ? world->actors.models[type->model].bounds_maximum.y * actor->scale : 1.6f;
		// (held about at the shoulder, a step to the side)
		moving->position[0] = actor->x + SDL_cosf(actor->yaw_radians) * 0.35f;
		moving->position[1] = actor->y - actor->root_lift + top * 0.8f;
		moving->position[2] = actor->z - SDL_sinf(actor->yaw_radians) * 0.35f;
	}
}

void world_end_frame(world_t *world, const camera_t *camera, float seconds, bool held, bool exact)
{
	follow_actor_lights(world);
	const float step_seconds = 1.0f / (float)SDL_max(1, world->settings->simulation.steps_per_second);
	float blend = exact ? 1.0f : (float)(world->simulation_accumulator_seconds / step_seconds);
	uint64_t particles_start = SDL_GetPerformanceCounter();
	// Around the walker, or where the camera looks from above
	float eye[3] = { camera->first_person ? camera->eye_position.x : camera->target_x, camera->eye_position.y,
		camera->first_person ? camera->eye_position.z : camera->target_z };

	fireflies_update(&world->fireflies, &world->particles, &world->terrain, &world->water, eye, world->settings->render.fireflies,
		world->settings->render.day_cycle ? lights_darkness(world->daylight.sun_elevation_degrees) : 0.0f, seconds);
	object_effects_update(&world->object_effects, &world->particles, &world->objects, &world->water, eye, world->settings->objects.effects_meters,
		seconds);
	particles_update(&world->particles, seconds, world->renderer.height_texture, water_gpu_depth_texture(&world->water), world->terrain.size,
		world->terrain.cell_size_meters);
	frame_profile_end(FRAME_PARTICLES, particles_start);
	trace_mark("fireflies, particles");
	// The water's textures, and its copy back to the CPU
	{
		uint64_t end_start = SDL_GetPerformanceCounter();

		water_gpu_end_frame(&world->water, &world->terrain, camera->target_x, camera->target_z);
		frame_profile_end(FRAME_WATER_END, end_start);
		world->water_copy_milliseconds += milliseconds_since(end_start);
	}
	trace_mark("water textures");
	// Moving bodies and the water's surface drawn between the last two
	// steps (they go at a fixed rate)
	if (!world->physics_off)
	{
		physics_interpolate(&world->physics, &world->objects, blend);
	}
	world->water_renderer.step_blend = world->water_frozen || held ? 1.0f : blend;
	// New or changed models' impostors (baked or from the cache; between
	// frames, as baking waits for the GPU)
	objects_update_impostors(&world->objects);
	trace_mark("physics drawn between steps, impostors");
}
