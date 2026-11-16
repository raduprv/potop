// The world drawn (world.h: world_render): the shadow maps, the sky, the
// land, the water, the things on it, the weather, then exposure and tone
// into the swapchain. The application's interface goes over it after.

#include "world.h"

#include <SDL3/SDL.h>

#include "gpu.h"
#include "material_map.h"
#include "profile.h"
#include "scene_uniforms.h"
#include "season.h"
#include "water_gpu.h"

// F8: what the first actor type with a model finds in each navigation
// cell, and every actor's path on top; filled four times a second
static void update_navigation_overlay(world_t *world)
{
	navigation_t *navigation = &world->navigation;
	int type = -1;
	navigation_agent_t agent;

	if (!world->renderer.navigation_overlay || world->elapsed_seconds - world->navigation_overlay_seconds < 0.25)
	{
		return;
	}
	world->navigation_overlay_seconds = world->elapsed_seconds;
	for (int t = 0; t < world->actors.type_count && type < 0; t++)
	{
		if (world->actors.types[t].model >= 0)
		{
			type = t;
		}
	}
	if (type < 0)
	{
		SDL_zero(agent);
		agent.height_meters = 1.0f;
		agent.step_meters = 0.4f;
		agent.maximum_slope = 0.84f;
		agent.wade_depth_meters = 0.3f;
		agent.swim_depth_meters = 0.8f;
		agent.can_swim = true;
		agent.swim_meters_per_second = 1.0f;
	}
	else
	{
		agent = actors_agent(&world->actors, type);
	}
	if (world->navigation_overlay_size != navigation->size)
	{
		SDL_free(world->navigation_overlay);
		world->navigation_overlay = SDL_malloc((size_t)navigation->size * (size_t)navigation->size);
		world->navigation_overlay_size = world->navigation_overlay != NULL ? navigation->size : 0;
		if (world->navigation_overlay == NULL)
		{
			return;
		}
	}
	navigation_fill_overlay(navigation, &world->terrain, &world->water, &agent, world->navigation_overlay);
	// The paths: lines from each moving actor through its waypoints
	for (int i = 0; i < world->actors.count; i++)
	{
		const actor_t *actor = &world->actors.actors[i];
		float from_x = actor->x;
		float from_z = actor->z;

		if (!actor->moving)
		{
			continue;
		}
		for (int k = actor->path_index; k < actor->path_count; k++)
		{
			float to_x = actor->path[k][0];
			float to_z = actor->path[k][1];
			float length = SDL_sqrtf((to_x - from_x) * (to_x - from_x) + (to_z - from_z) * (to_z - from_z));
			int steps = SDL_max((int)(length / (0.5f * NAVIGATION_CELL_METERS)), 1);

			for (int step = 0; step <= steps; step++)
			{
				float amount = (float)step / (float)steps;
				int32_t cell_x = (int32_t)((from_x + (to_x - from_x) * amount) / NAVIGATION_CELL_METERS);
				int32_t cell_z = (int32_t)((from_z + (to_z - from_z) * amount) / NAVIGATION_CELL_METERS);

				if (cell_x >= 0 && cell_z >= 0 && cell_x < navigation->size && cell_z < navigation->size)
				{
					world->navigation_overlay[cell_z * navigation->size + cell_x] = step == steps ? NAVIGATION_OVERLAY_WAYPOINT
						: NAVIGATION_OVERLAY_PATH;
				}
			}
			from_x = to_x;
			from_z = to_z;
		}
	}
	terrain_renderer_upload_navigation(&world->renderer, navigation->size, NAVIGATION_CELL_METERS, world->navigation_overlay);
}

// Color settings are sRGB; shaders work in linear light (same as the
// renderers do)
static float color_to_linear(float value)
{
	return SDL_powf(value, 2.2f);
}

// Is the eye under water? If so, the color and thickness of the water
// around it, for data/shaders/underwater.glsl: the water's deep color lit by
// the sun and the sky, darker the deeper the eye, and murky brown with mud.
static void update_underwater(world_t *world, camera_t *camera)
{
	const water_t *water = &world->water;
	const render_settings_t *render = &world->lit_render;
	int32_t cell_x = (int32_t)SDL_floorf(camera->eye_position.x / water->cell_size_meters + 0.5f);
	int32_t cell_y = (int32_t)SDL_floorf(camera->eye_position.z / water->cell_size_meters + 0.5f);
	int32_t index;
	float depth;
	float level;
	float below;
	float mud;
	float concentration;
	float clarity = SDL_max(render->water_clarity_meters, 0.1f);
	float visibility = SDL_max(render->underwater_visibility_meters, 1.0f);
	vec3_t sun_direction = terrain_renderer_sun_direction(render);
	float light[3];
	float mud_color[3] = { color_to_linear(0.42f), color_to_linear(0.32f), color_to_linear(0.20f) };

	camera->underwater = false;
	if (cell_x < 0 || cell_y < 0 || cell_x >= water->size || cell_y >= water->size)
	{
		return;
	}
	index = cell_y * water->size + cell_x;
	depth = (float)water->depth[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;
	level = (float)world->terrain.heights[index] * TERRAIN_METERS_PER_HEIGHT_UNIT + depth;
	if (depth < 0.1f || camera->eye_position.y > level - 0.02f)
	{
		return;
	}
	camera->underwater = true;
	camera->water_level = level;
	below = level - camera->eye_position.y;
	concentration = water_concentration_at(water, index);
	mud = clamp_float(concentration / 0.04f, 0.0f, 1.0f);
	for (int k = 0; k < 3; k++)
	{
		float sun = color_to_linear(render->sun_color[k]) * render->sun_intensity * SDL_max(sun_direction.y, 0.0f);
		float sky = color_to_linear(render->sky_color[k]) * render->ambient_intensity;

		// Near the surface the water glows in its shallow color; deeper
		// down it slowly turns the deep color and gets a little darker
		float depth_blend = 1.0f - SDL_expf(-below / (clarity * 6.0f));
		float water_color = lerp_float(color_to_linear(render->water_shallow_color[k]), color_to_linear(render->water_deep_color[k]), depth_blend);

		light[k] = (0.9f * sun + 2.0f * sky) * render->underwater_brightness * SDL_expf(-below / (visibility * 0.5f));
		camera->underwater_color[k] = lerp_float(water_color, mud_color[k], mud) * light[k];
	}
	// Things fade to 95% water color at the visibility distance
	camera->underwater_fog_per_meter = 3.0f * (1.0f + mud * 8.0f) / visibility;
}

// What the grass needs of the world (grass.h), as the renderers have it
static void grass_world_of(const world_t *world, const scene_uniforms_t *scene, grass_world_t *grass)
{
	const material_set_t *materials = &world->renderer.materials;

	SDL_zerop(grass);
	grass->height_texture = world->renderer.height_texture;
	grass->water_texture = water_gpu_depth_texture(&world->water);
	grass->flow_texture = water_gpu_flow_texture(&world->water);
	grass->soil_weight_texture = world->renderer.soil_weight_texture;
	grass->normal_texture = world->renderer.normal_texture;
	grass->surface_texture = world->renderer.surface_texture;
	grass->material_texture = world->renderer.material_texture;
	grass->material_map = world->settings->render.material_map;
	grass->overlay_texture = world->renderer.overlay_texture;
	grass->variation_texture = materials->variation_texture;
	grass->material_color_array = materials->color_height_array;
	grass->cloud_shadow_texture = clouds_shadow_texture();
	grass->sky_occlusion_texture = world->renderer.sky_occlusion_texture;
	grass->sky_layers_texture = world->renderer.sky_layers_texture;
	grass->sky_bounce_texture = world->renderer.sky_bounce_texture;
	grass->scene = scene;
	grass->far_shadow_texture = world->shadow.depth_texture;
	grass->near_shadow_texture = world->near_shadow.size > 0 && world->shadow.size > 0 ? world->near_shadow.depth_texture : world->shadow.depth_texture;
	grass->grass_layer = materials->first_layer[MATERIAL_GRASS];
	for (int i = 0; i < MATERIAL_COUNT; i++)
	{
		grass->material_patches[i][0] = materials->patch_repeats_per_meter[i];
		grass->material_patches[i][1] = materials->patch_strength[i];
		grass->material_patches[i][2] = materials->patch_edge_meters[i];
		grass->material_patches[i][3] = 0.0f;
	}
	grass->grass_macro_repeats_per_meter = materials->macro_repeats_per_meter[MATERIAL_GRASS];
	grass->soil_blend_cells = world->settings->render.soil_blend_meters / SDL_max(world->terrain.cell_size_meters, 0.01f);
	grass->map_size = world->terrain.size;
	grass->cell_size_meters = world->terrain.cell_size_meters;
	grass->max_height_meters = world->terrain.max_height_meters;
	SDL_memcpy(grass->sun_direction, scene->sun_direction, sizeof(grass->sun_direction));
	grass->wind_uniform = world->objects.wind_uniform;
}

// Snow and petals on the ground (ground_overlay.h): the weather, the season
static void update_ground_overlay(world_t *world, const scene_uniforms_t *scene)
{
	if (!world->settings->render.ground_overlay || !world->settings->render.material_map)
	{
		// (switched on again: as the season says at once)
		world->ground_overlay.cleared = false;
		world->ground_overlay_seconds = world->elapsed_seconds;
		world->renderer.overlay_texture = NULL;
		return;
	}
	ground_overlay_inputs_t inputs;
	float month = world->objects.month;
	float month_jump = SDL_fabsf(month - world->ground_overlay_month);

	SDL_zero(inputs);
	inputs.material_texture = world->renderer.material_texture;
	inputs.normal_texture = world->renderer.normal_texture;
	inputs.sky_texture = world->sky_occlusion.texture != NULL && world->sky_occlusion.map_size == world->terrain.size ? world->sky_occlusion.texture
		: NULL;
	inputs.water_texture = water_gpu_depth_texture(&world->water);
	inputs.map_size = world->terrain.size;
	inputs.cell_size_meters = world->terrain.cell_size_meters;
	inputs.seconds = (float)SDL_max(world->elapsed_seconds - world->ground_overlay_seconds, 0.0);
	inputs.winter = season_snow(month, world->objects.snow_first_month, world->objects.snow_last_month);
	// (as rain_draw has it: snow as soon as the snow's months have begun)
	inputs.snowfall = rain_intensity(&world->rain, &world->settings->render)
		* SDL_min(4.0f * inputs.winter, 1.0f);
	inputs.bloom = season_snow(month, SDL_clamp(SDL_roundf(world->settings->render.bloom_first_month), 1.0f, 12.0f),
		SDL_clamp(SDL_roundf(world->settings->render.bloom_last_month), 1.0f, 12.0f));
	inputs.sun_x = scene->sun_direction[0];
	inputs.sun_z = scene->sun_direction[2];
	inputs.sun_height = scene->sun_direction[1];
	inputs.reset = SDL_min(month_jump, 12.0f - month_jump) > 0.25f;
	inputs.snow_amount = world->objects.snow_amount;
	world->ground_overlay_seconds = world->elapsed_seconds;
	world->ground_overlay_month = month;
	ground_overlay_update(&world->ground_overlay, &inputs);
	world->renderer.overlay_texture = world->ground_overlay.texture;
}

// The ice on the water (water_ice.h): it grows and melts, the water's
// simulation holds the water still under it, the renderer draws it
static void update_ice(world_t *world)
{
	water_ice_inputs_t inputs;
	float month = world->objects.month;
	float month_jump = SDL_fabsf(month - world->ice_month);

	if (!world->settings->render.water_ice)
	{
		world->water_iced = false;
		world->water.iced = false;
		if (world->ice.texture != NULL)
		{
			water_ice_release(&world->ice);
		}
		world->ice_seconds = world->elapsed_seconds;
		water_gpu_set_ice(&world->water, NULL);
		world->water_renderer.ice_texture = NULL;
		return;
	}
	SDL_zero(inputs);
	inputs.water_texture = water_gpu_depth_texture(&world->water);
	inputs.flow_texture = water_gpu_flow_texture(&world->water);
	inputs.height_texture = world->renderer.height_texture;
	inputs.map_size = world->terrain.size;
	inputs.seconds = (float)SDL_max(world->elapsed_seconds - world->ice_seconds, 0.0);
	inputs.winter = season_snow(month, world->objects.snow_first_month, world->objects.snow_last_month);
	inputs.snowfall = rain_intensity(&world->rain, &world->settings->render) * SDL_min(4.0f * inputs.winter, 1.0f);
	inputs.reset = SDL_min(month_jump, 12.0f - month_jump) > 0.25f;
	// Cold enough, all of the water held still: the ice over all of it
	world->water_iced = inputs.winter >= WATER_ICE_HOLDS_WINTER && world->ice.pipeline != NULL;
	world->water.iced = world->water_iced;
	inputs.frozen = world->water_iced;
	world->ice_seconds = world->elapsed_seconds;
	world->ice_month = month;
	water_ice_update(&world->ice, &inputs);
	water_gpu_set_ice(&world->water, water_ice_texture(&world->ice));
	world->water_renderer.ice_texture = water_ice_texture(&world->ice);
}

// The moon's disc on the sky: how bright its lit part (linear light, white
// albedo), and the faint earthshine on its dark part, of that
#define MOON_DISC_BRIGHTNESS 1.6f
// The light turning this much draws the far shadow map again
#define FAR_SHADOW_TURN_DEGREES 3.0f
#define MOON_EARTHSHINE 0.015f

static void render_frame(world_t *world, camera_t *camera, const world_render_options_t *options, int pixel_width, int pixel_height)
{
	int scene_width;
	int scene_height;

	uint64_t section_start = SDL_GetPerformanceCounter();
	int64_t chunks_before = world->renderer.uploaded_chunk_count;

	gpu_timer_begin_frame(&world->gpu_timer);
	terrain_update_chunk_bounds(&world->terrain);
	frame_profile_end(FRAME_CHUNK_BOUNDS, section_start);
	objects_follow_terrain(&world->objects, &world->terrain);
	{
		uint64_t water_start = SDL_GetPerformanceCounter();

		objects_follow_water(&world->objects, &world->water);
		frame_profile_end(FRAME_FOLLOW_WATER, water_start);
	}
	frame_profile_end(FRAME_OBJECTS_FOLLOW, section_start);
	trace_mark("objects follow the ground and the water");
	section_start = SDL_GetPerformanceCounter();
	// The areas that block paths (a locked gate), as they are now
	{
		navigation_blocker_t blockers[NAVIGATION_BLOCKERS_MAX];
		int blocker_count = 0;

		for (int i = 0; i < world->areas.count && blocker_count < NAVIGATION_BLOCKERS_MAX; i++)
		{
			const area_t *area = &world->areas.areas[i];

			if (area->blocks != 0)
			{
				navigation_blocker_t *blocker = &blockers[blocker_count++];

				SDL_zerop(blocker);
				blocker->x = area->x;
				blocker->z = area->z;
				blocker->half_width_meters = area->half_width_meters;
				blocker->half_depth_meters = area->half_depth_meters;
				blocker->yaw_radians = area->yaw_radians;
				blocker->groups = area->blocks;
			}
		}
		navigation_set_blockers(&world->navigation, blockers, blocker_count);
	}
	// Before the upload, which clears the terrain's change flags
	navigation_update(&world->navigation, &world->terrain, &world->objects, world->elapsed_seconds);
	update_navigation_overlay(world);
	frame_profile_end(FRAME_NAVIGATION, section_start);
	trace_mark("paths follow the map");
	section_start = SDL_GetPerformanceCounter();
	// (a flood takes the cover away, silt buries it)
	{
		uint64_t part_start = SDL_GetPerformanceCounter();

		material_map_clear_flooded_cover(&world->terrain, &world->water,
			world->soils->silt_visible_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER);
		frame_profile_end(FRAME_FLOODED_COVER, part_start);
		terrain_renderer_upload(&world->renderer, &world->terrain, &world->water, world->soils, camera, &world->lit_render,
			world->elapsed_seconds);
		part_start = SDL_GetPerformanceCounter();
		terrain_renderer_upload_tiles(&world->renderer, &world->water);
		frame_profile_end(FRAME_TERRAIN_TILES, part_start);
	}
	frame_profile_end(FRAME_TERRAIN_UPLOAD, section_start);
	trace_mark("terrain textures and chunks");
	frame_profile_count(FRAME_CHUNK_UPLOADS, (int32_t)(world->renderer.uploaded_chunk_count - chunks_before));
	section_start = SDL_GetPerformanceCounter();
	// The objects shown or hidden (the World window): their shadows go too
	{
		if (world->objects.hidden != world->objects_were_hidden)
		{
			world->objects_were_hidden = world->objects.hidden;
			world->objects.far_shadow_dirty = true;
			world->objects.near_shadow_dirty = true;
		}
	}
	// Objects and the ground changing: the near map (see below); the far one
	// only in the editor (far_shadow_follows_edits), else it follows the
	// light alone
	if (world->objects.far_shadow_dirty)
	{
		world->near_shadow.needs_render = true;
		world->shadow.needs_render = world->shadow.needs_render || world->far_shadow_follows_edits;
		world->objects.far_shadow_dirty = false;
	}
	if (world->objects.near_shadow_dirty)
	{
		// Grass and such only show in the near map
		world->near_shadow.needs_render = true;
		world->objects.near_shadow_dirty = false;
	}
	// (actors are only in the near map's moving layer: below)
	if (world->terrain.shadow_dirty)
	{
		world->shadow.needs_render = world->shadow.needs_render || world->far_shadow_follows_edits;
		world->near_shadow.needs_render = true;
		world->terrain.shadow_dirty = false;
	}
	// The light turned (the sun along its path, the moon taking over at
	// night): the far map again
	{
		vec3_t light = terrain_renderer_sun_direction(&world->lit_render);

		if (vec3_dot(light, world->far_shadow_light) < SDL_cosf(FAR_SHADOW_TURN_DEGREES * SDL_PI_F / 180.0f))
		{
			world->shadow.needs_render = true;
		}
	}
	// Erosion: only the near map, and only if the ground changed in its
	// square
	if (world->terrain.eroded_shadow_pending)
	{
		const int32_t *rectangle = world->terrain.eroded_rectangle;
		float cell = world->terrain.cell_size_meters;
		float half = 0.5f * world->near_shadow.extent_meters;

		if (world->near_shadow.size > 0 && world->terrain.eroded_rectangle_valid
			&& (float)(rectangle[2] + 1) * cell >= world->near_shadow.center.x - half && (float)rectangle[0] * cell <= world->near_shadow.center.x + half
			&& (float)(rectangle[3] + 1) * cell >= world->near_shadow.center.z - half && (float)rectangle[1] * cell <= world->near_shadow.center.z + half)
		{
			world->near_shadow.needs_render = true;
		}
		world->terrain.eroded_shadow_pending = false;
		world->terrain.eroded_rectangle_valid = false;
	}

	// The sky's occlusion: worked out a few directions a frame when asked
	// (U), or when the map has none, whether it is drawn with or not (the
	// snow goes by it, the game asks it where it is dark or covered)
	if (!world->sky_occlusion.ready && !world->sky_occlusion.working
		&& !world->sky_occlusion.given_up)
	{
		world->sky_occlusion.requested = true;
	}
	if (world->sky_occlusion.map_size != 0 && world->sky_occlusion.map_size != world->terrain.size)
	{
		sky_occlusion_forget(&world->sky_occlusion, true);
	}
	if (world->sky_occlusion.requested || world->sky_occlusion.working)
	{
		sky_occlusion_step(&world->sky_occlusion, &world->terrain, &world->objects, &world->lit_render, world->renderer.height_texture,
			world->renderer.vertex_buffer, world->renderer.index_buffer, world->renderer.index_count);
	}

	// The sun does not move, so the shadow map only changes with the
	// terrain; while the terrain keeps changing (a brush held down) it is
	// redrawn at most every shadow_update_interval_seconds
	// (a map switched back on: drawn again, it was left as it was)
	{
		bool far_off = world->far_shadow_off || world->settings->render.debug_far_shadow_off;
		bool near_off = world->near_shadow_off || world->settings->render.debug_near_shadow_off;

		if (world->shadows_were_off[0] && !far_off)
		{
			world->shadow.needs_render = true;
		}
		if (world->shadows_were_off[1] && !near_off)
		{
			world->near_shadow.needs_render = true;
		}
		world->shadows_were_off[0] = far_off;
		world->shadows_were_off[1] = near_off;
	}
	// (the turning ones, a windmill's sails, to where the clock has them)
	objects_update_spin(&world->objects, world->elapsed_seconds);
	if (world->shadow.needs_render && !(world->far_shadow_off || world->settings->render.debug_far_shadow_off)
		&& (world->elapsed_seconds - world->last_shadow_render_seconds >= world->lit_render.shadow_update_interval_seconds
			|| world->last_shadow_render_seconds == 0.0))
	{
		world->last_shadow_render_seconds = SDL_max(world->elapsed_seconds, 1e-6);
		world->far_shadow_light = terrain_renderer_sun_direction(&world->lit_render);
		world->objects.far_shadow_small_meters = SDL_max(world->settings->render.far_shadow_least_meters, 0.0f);
		frame_profile_count(FRAME_SHADOW_REDRAWS, 1);
		shadow_map_update_projection(&world->shadow, &world->terrain, terrain_renderer_sun_direction(&world->lit_render));
		gpu_timer_begin(&world->gpu_timer, GPU_PASS_SHADOW);
		objects_prepare_shadow(&world->objects, &world->shadow, NULL);
		{
			SDL_GPURenderPass *pass = shadow_map_begin(&world->shadow);

			if (pass != NULL)
			{
				shadow_map_render_terrain(&world->shadow, pass, &world->terrain, world->renderer.vertex_buffer, world->renderer.index_buffer,
					world->renderer.index_count, world->renderer.height_texture, false);
				objects_render_shadow(&world->objects, pass);
				shadow_map_end(&world->shadow, pass);
			}
		}
		gpu_timer_end(&world->gpu_timer, GPU_PASS_SHADOW);
		// The near map shares the far one's view: redo it too
		world->near_shadow.needs_render = true;
		world->last_near_shadow_render_seconds = 0.0;
	}

	// The near cascade follows the camera's target, in two layers. The still
	// one (the terrain, what stands) is drawn again when the target has
	// moved a sixth of the square (the shadows near its edge fade into the
	// far map anyway), or when something changed (as often as the far map).
	// The moving one (loose objects falling or floating, the actors) goes
	// over a copy of it each frame something moves: a stone's shadow falls
	// with it, a bear's walks with it, the forest is not drawn again
	if (world->near_shadow.size > 0 && world->shadow.size > 0 && !(world->near_shadow_off || world->settings->render.debug_near_shadow_off))
	{
		vec3_t target = vec3_make(camera->target_x, camera->target_height, camera->target_z);
		float extent = SDL_max(world->lit_render.near_shadow_meters, 10.0f);
		float moved_x = target.x - world->near_shadow.center.x;
		float moved_z = target.z - world->near_shadow.center.z;
		bool moved = moved_x * moved_x + moved_z * moved_z > (extent / 6.0f) * (extent / 6.0f) || world->near_shadow.extent_meters != extent;
		bool changed = world->near_shadow.needs_render
			&& (world->elapsed_seconds - world->last_near_shadow_render_seconds >= world->lit_render.shadow_update_interval_seconds
				|| world->last_near_shadow_render_seconds == 0.0);
		// Actors in the square move all the time: their shadows follow at
		// their own pace (0: every frame)
		bool actors_moved = !options->paused && world->elapsed_seconds - world->last_moving_shadow_render_seconds
			>= world->lit_render.actor_shadow_update_seconds && actors_any_near(&world->actors, target.x, target.z, extent * 0.75f);
		bool still_again = moved || changed;
		bool moving_again = still_again || world->objects.moving_shadows || world->actors.shadow_dirty || actors_moved;

		if (still_again || moving_again)
		{
			gpu_timer_begin(&world->gpu_timer, GPU_PASS_NEAR_SHADOW);
		}
		if (still_again)
		{
			SDL_GPURenderPass *pass;

			world->last_near_shadow_render_seconds = SDL_max(world->elapsed_seconds, 1e-6);
			frame_profile_count(FRAME_NEAR_SHADOW_REDRAWS, 1);
			shadow_map_update_near_projection(&world->near_shadow, &world->shadow, target, extent);
			objects_prepare_shadow_layer(&world->objects, &world->near_shadow, camera, OBJECTS_SHADOW_STILL);
			pass = shadow_map_begin_still(&world->near_shadow);
			if (pass != NULL)
			{
				shadow_map_render_terrain(&world->near_shadow, pass, &world->terrain, world->renderer.vertex_buffer,
					world->renderer.index_buffer, world->renderer.index_count, world->renderer.height_texture, true);
				objects_render_shadow(&world->objects, pass);
				shadow_map_end(&world->near_shadow, pass);
			}
		}
		if (moving_again)
		{
			SDL_GPURenderPass *pass;

			world->last_moving_shadow_render_seconds = SDL_max(world->elapsed_seconds, 1e-6);
			world->objects.moving_shadows = false;
			world->actors.shadow_dirty = false;
			frame_profile_count(FRAME_MOVING_SHADOW_REDRAWS, 1);
			objects_prepare_shadow_layer(&world->objects, &world->near_shadow, camera, OBJECTS_SHADOW_LOOSE);
			pass = shadow_map_begin_moving(&world->near_shadow);
			if (pass != NULL)
			{
				objects_render_shadow(&world->objects, pass);
				actors_render_shadow(&world->actors, pass, &world->near_shadow);
				SDL_EndGPURenderPass(pass);
			}
		}
		if (still_again || moving_again)
		{
			gpu_timer_end(&world->gpu_timer, GPU_PASS_NEAR_SHADOW);
		}
	}
	frame_profile_end(FRAME_SHADOWS, section_start);
	trace_mark("shadow maps");

	update_underwater(world, camera);
	{
		// Linear light, as the scene is drawn
		float clear_color[4] = { SDL_powf(world->lit_render.sky_color[0], 2.2f), SDL_powf(world->lit_render.sky_color[1], 2.2f),
			SDL_powf(world->lit_render.sky_color[2], 2.2f), 1.0f };
		scene_uniforms_t scene;
		grass_world_t grass_world;
		SDL_GPURenderPass *pass;

		if (camera->underwater)
		{
			// What is not drawn is far away: the water's color, not the sky
			for (int i = 0; i < 3; i++)
			{
				clear_color[i] = camera->underwater_color[i];
			}
		}
		world->water_renderer.film_meters = world->settings->water.film_millimeters * 0.001f;
		world->water_renderer.thin_meters = world->settings->water.thin_water_millimeters * 0.001f;
		world->water_renderer.moving_speed = world->settings->water.sleep_speed_meters_per_second;
		scene_uniforms_fill(&scene, camera, &world->terrain, &world->lit_render, &world->shadow, &world->near_shadow,
			(float)world->elapsed_seconds);
		scene.temporal_jitter[0] = world->temporal_jitter[0];
		scene.temporal_jitter[1] = world->temporal_jitter[1];
		// The leaves' cut outs smoothed by alpha to coverage (scene_depth.glsl)
		scene.temporal_jitter[2] = gpu.scene_samples > 1 ? 1.0f : 0.0f;
		// The shadow maps switched off in the World window (debugging)
		if (world->far_shadow_off || world->settings->render.debug_far_shadow_off)
		{
			scene.shadow_texel_size = 0.0f;
		}
		if (world->near_shadow_off || world->settings->render.debug_near_shadow_off)
		{
			scene.near_shadow_texel_size = 0.0f;
		}
		// The clouds go by the sky's clock (the console's time)
		terrain_renderer_cloud_uniforms(&world->lit_render, (float)world->sky_seconds, scene.cloud_parameters, scene.cloud_motion);
		scene.padding[2] = (float)SDL_fmod(world->sky_seconds, 1.0e6);
		// The stars: how much they show, and the hour they turn with
		scene.fog_parameters[1] = world->daylight.star_visibility;
		// The moon on the sky (whatever lights the scene)
		{
			float azimuth = world->daylight.moon_azimuth_degrees * SDL_PI_F / 180.0f;
			float elevation = world->daylight.moon_elevation_degrees * SDL_PI_F / 180.0f;
			float phase = world->daylight.moon_phase;

			scene.moon[0] = SDL_cosf(elevation) * SDL_cosf(azimuth);
			scene.moon[1] = SDL_sinf(elevation);
			scene.moon[2] = SDL_cosf(elevation) * SDL_sinf(azimuth);
			scene.moon[3] = world->settings->render.day_cycle && world->daylight.moon_lights ? 1.0f : 0.0f;
			scene.moon_shape[0] = phase;
			scene.moon_shape[1] = SDL_cosf(0.5f * SDL_clamp(world->settings->render.moon_size_degrees, 0.1f, 20.0f) * SDL_PI_F / 180.0f);
			// Size 0: no moon on the sky (its light stays)
			scene.moon_shape[2] = world->settings->render.day_cycle && world->settings->render.moon_size_degrees > 0.0f ? MOON_DISC_BRIGHTNESS : 0.0f;
			// The earth lights the dark part a little, most near new moon
			{
				float near_new = 0.5f * (1.0f + SDL_cosf(2.0f * SDL_PI_F * phase));

				scene.moon_shape[3] = MOON_EARTHSHINE * near_new * near_new;
			}
		}
		scene.fog_parameters[2] = (float)world->day_hour;
		// The aurora over the north (away from the noon sun), and its faint
		// green in the light from the sky
		{
			float azimuth = world->settings->render.sun_azimuth_degrees * SDL_PI_F / 180.0f;
			float shown;

			aurora_update(&world->aurora, (float)(world->elapsed_seconds - world->aurora_seconds));
			world->aurora_seconds = world->elapsed_seconds;
			shown = aurora_intensity(&world->aurora, world->settings->render.aurora_strength, world->daylight.sun_elevation_degrees);
			scene.aurora[0] = -SDL_cosf(azimuth);
			scene.aurora[1] = -SDL_sinf(azimuth);
			scene.aurora[2] = shown;
			scene.aurora[3] = (float)SDL_fmod(world->elapsed_seconds, 10000.0);
			scene.sky_color[0] += AURORA_SKY_TINT_RED * shown;
			scene.sky_color[1] += AURORA_SKY_TINT_GREEN * shown;
			scene.sky_color[2] += AURORA_SKY_TINT_BLUE * shown;
		}
		// The rain (rain.h): how hard, how wet things are, its clock
		// (snow does not wet things nor ring the water)
		scene.weather[0] = camera->underwater ? 0.0f : rain_intensity(&world->rain, &world->settings->render)
			* (1.0f - SDL_min(4.0f * season_snow(world->objects.month, world->objects.snow_first_month, world->objects.snow_last_month), 1.0f));
		// (snow does not wet things: as much of the wetness as falls as rain)
		scene.weather[1] = world->rain.wetness
			* (1.0f - SDL_min(4.0f * season_snow(world->objects.month, world->objects.snow_first_month, world->objects.snow_last_month), 1.0f));
		scene.weather[3] = world->objects.snow_amount;
		// (what lies under water takes no snow: ground_water.glsl)
		scene_ground_set(world->renderer.height_texture, water_gpu_depth_texture(&world->water));
		// (the one there is, while a new one is worked out too)
		world->renderer.sky_occlusion_texture = world->settings->render.sky_occlusion && world->sky_occlusion.texture != NULL
			&& world->sky_occlusion.map_size == world->terrain.size ? world->sky_occlusion.texture : NULL;
		scene.sky_occlusion[0] = world->lit_render.sky_occlusion_intensity;
		scene.sky_occlusion[1] = world->renderer.sky_occlusion_texture != NULL ? 1.0f : 0.0f;
		scene.sky_occlusion[2] = (float)world->sky_occlusion.layer_size;
		// (the gusts' waves over the meadow, on the ground too: terrain.frag)
		SDL_memcpy(scene.wind, world->objects.wind_uniform, sizeof(scene.wind));
		scene.gusts[0] = world->objects.wind_uniform[4];
		scene.gusts[1] = world->objects.wind_uniform[5];
		scene.gusts[3] = world->settings->render.grass_gust_sheen;
		world->renderer.sky_bounce_texture = world->renderer.sky_occlusion_texture != NULL ? world->sky_occlusion.bounce_texture : NULL;
		scene.sky_bounce[0] = world->lit_render.bounce_light;
		scene.sky_bounce[1] = world->renderer.sky_bounce_texture != NULL ? 1.0f : 0.0f;
		world->renderer.sky_layers_texture = world->renderer.sky_occlusion_texture != NULL ? world->sky_occlusion.layers_texture : NULL;
		scene_ground_set_sky_occlusion(world->renderer.sky_occlusion_texture,
			world->renderer.sky_occlusion_texture != NULL ? world->sky_occlusion.layers_texture : NULL,
			world->renderer.sky_occlusion_texture != NULL ? world->sky_occlusion.bounce_texture : NULL);
		scene.ground_map[0] = (float)world->terrain.size;
		scene.ground_map[1] = world->terrain.cell_size_meters;
		scene.ground_map[2] = scene_ground_ready() ? 1.0f : 0.0f;
		scene.ground_map[3] = 0.0f;
		scene.weather[2] = (float)SDL_fmod(world->elapsed_seconds, 3600.0);
		{
			float azimuth = world->daylight.sun_azimuth_degrees * SDL_PI_F / 180.0f;

			scene.twilight[0] = SDL_cosf(azimuth);
			scene.twilight[1] = SDL_sinf(azimuth);
			scene.twilight[2] = world->daylight.twilight_glow;
			scene.twilight[3] = world->daylight.twilight_brightness;
		}
		// The volumetric clouds' dome, for the sky to hide the stars behind
		// them: 1 the whole sphere, 2 its upper half (see clouds_volume.glsl)
		scene.fog_parameters[3] = world->lit_render.volumetric_clouds && !world->clouds.failed && world->clouds.dome_texture != NULL
			? (world->clouds.dome_half ? 2.0f : 1.0f) : 0.0f;
		// The sky leaves its flat clouds to the volumetric ones
		scene.padding[0] = world->lit_render.volumetric_clouds && !world->clouds.failed ? 1.0f : 0.0f;
		// ... and the land takes their shadows from them, once they are there
		scene.padding[1] = scene.padding[0] > 0.5f && clouds_shadow_texture() != NULL ? 1.0f : 0.0f;
		// Where the eye is on the clouds' map, for the sky window
		world->sky_eye[0] = scene.eye_position[0] - scene.cloud_motion[0];
		world->sky_eye[1] = scene.eye_position[2] - scene.cloud_motion[1];
		// The fires and torches near the eye, for this camera (uploaded
		// before the scene's pass begins)
		{
			lights_frame_t frame;

			frame.enabled = world->lit_render.local_lights;
			frame.intensity_scale = world->lit_render.local_light_intensity;
			frame.darkness = lights_darkness(world->daylight.sun_elevation_degrees);
			SDL_memcpy(frame.view_projection, scene.view_projection, sizeof(frame.view_projection));
			SDL_memcpy(frame.eye, scene.eye_position, sizeof(frame.eye));
			// (as render_target_begin will make it)
			frame.scene_pixels[0] = (float)SDL_max((int)SDL_lroundf((float)pixel_width * SDL_clamp(world->lit_render.render_scale, 0.25f, 2.0f)), 1);
			frame.scene_pixels[1] = (float)SDL_max((int)SDL_lroundf((float)pixel_height * SDL_clamp(world->lit_render.render_scale, 0.25f, 2.0f)), 1);
			frame.seconds = world->elapsed_seconds;
			frame.enabled_decals = world->settings->render.decals;
			frame.window_lights = world->lit_render.window_lights;
			frame.window_light_intensity = world->lit_render.window_light_intensity;
			// The lamps go with the day, until the game (or the console)
			// says otherwise
			if (world->settings->render.lamps_with_the_day)
			{
				lights_set_lamps(&world->lights, LIGHTS_LAMPS_AUTOMATIC);
			}
			else if (world->lights.lamps == LIGHTS_LAMPS_AUTOMATIC)
			{
				lights_set_lamps(&world->lights, frame.darkness > 0.5f ? LIGHTS_LAMPS_ON : LIGHTS_LAMPS_OFF);
			}
			decals_update(&world->decals, world->elapsed_seconds);
			lights_update(&world->lights, &world->particles, &world->objects, &world->decals, &frame);
		}
		trace_mark("scene uniforms, sky, lights");
		// The objects' culling, before the scene's pass (with the depth
		// pyramid of the frame before, see hiz.h). Far objects become
		// their impostors once smaller on screen than objects.impostor_pixels:
		// the focal length (the projection's) times the scene's height in
		// pixels, over that
		{
			uint64_t cull_start = SDL_GetPerformanceCounter();
			float scene_pixels = (float)pixel_height * SDL_clamp(world->lit_render.render_scale, 0.25f, 1.0f);

			world->objects.impostor_factor = world->objects.impostors_everywhere ? 1e-6f
				: camera->projection.m[5] * scene_pixels / SDL_max(world->settings->objects.impostor_pixels, 1.0f);
			// The levels of detail: a model's height on screen in pixels is
			// its height times this over its distance (half the focal length
			// times the scene's height: m[5] is over half the screen)
			world->objects.lod_factor = 0.5f * camera->projection.m[5] * scene_pixels;

			objects_prepare(&world->objects, camera);
			frame_profile_end(FRAME_OBJECTS_CULL, cull_start);
		}
		trace_mark("objects culled");
		// The grass's blades in view, made anew (grass.h)
		update_ground_overlay(world, &scene);
		trace_mark("ground overlay (snow, petals)");
		update_ice(world);
		waterfalls_update(&world->waterfalls, &world->particles, &world->terrain, &world->water,
			camera->first_person ? camera->walker_x : camera->target_x,
			camera->first_person ? camera->walker_z : camera->target_z, world->elapsed_seconds, world->water_iced);
		trace_mark("ice, waterfalls");
		grass_world_of(world, &scene, &grass_world);
		// (what was drawn the frame before hides the blades behind it, as
		// it hides the objects: objects occlusion off, neither)
		grass_world.occlusion = world->objects.no_terrain_occlusion ? NULL : world->objects.occlusion;
		grass_prepare(&world->grass, camera, &world->lit_render, &world->settings->objects, &grass_world);
		trace_mark("grass");
		terrain_renderer_prepare_draw(&world->renderer, &world->terrain, camera, &world->lit_render);
		water_renderer_prepare(&world->water_renderer, &world->water, &world->terrain, camera);
		gpu_flush_uploads();
		trace_mark("uploads flushed");
		world->scene_target.world_scene = true;
		pass = render_target_begin(&world->scene_target, pixel_width, pixel_height, world->lit_render.render_scale, clear_color,
			&scene_width, &scene_height);
		if (pass != NULL)
		{
			SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)scene_width, (float)scene_height, 0.0f, 1.0f };

			SDL_SetGPUViewport(pass, &viewport);
			scene_uniforms_push(&scene);
		}
		if (pass == NULL)
		{
			return;
		}
		gpu_timer_begin(&world->gpu_timer, GPU_PASS_OBJECTS);
		objects_draw(&world->objects, pass, &world->lit_render, &world->shadow, &world->near_shadow,
			world->renderer.materials.variation_texture, (float)world->elapsed_seconds);
		{
			char objects_drawn[64];

			SDL_snprintf(objects_drawn, sizeof(objects_drawn), "objects drawn (%d draws)", world->objects.draw_call_count);
			trace_mark(objects_drawn);
		}
		grass_draw(&world->grass, pass, &world->lit_render, &world->settings->objects, &grass_world);
		trace_mark("grass drawn");
		world->actors.pbr_shading = world->lit_render.pbr_shading;
		actors_draw(&world->actors, pass, camera, &world->shadow, &world->near_shadow, world->renderer.materials.variation_texture,
			(float)world->elapsed_seconds);
		gpu_timer_end(&world->gpu_timer, GPU_PASS_OBJECTS);
		trace_mark("actors drawn");
		// The terrain last of the opaque things: its shading is the dearest
		// per pixel, and what stands on it (houses, trees, grass, animals)
		// is drawn already: the depth test turns its hidden pixels away
		// before they are shaded
		gpu_timer_begin(&world->gpu_timer, GPU_PASS_TERRAIN);
		terrain_renderer_draw(&world->renderer, pass, &world->terrain, camera, &world->lit_render, &options->brush, &world->shadow,
			&world->near_shadow);
		gpu_timer_end(&world->gpu_timer, GPU_PASS_TERRAIN);
		trace_mark("terrain drawn");
		// Under water there is no sky to see: what is not drawn is the
		// water's color (the clear color)
		if (!camera->underwater)
		{
			gpu_timer_begin(&world->gpu_timer, GPU_PASS_SKY);
			sky_renderer_draw(&world->sky_renderer, pass, world->renderer.materials.variation_texture,
				scene.fog_parameters[3] > 0.0f ? world->clouds.dome_texture : NULL);
			gpu_timer_end(&world->gpu_timer, GPU_PASS_SKY);
		}
		gpu_timer_begin(&world->gpu_timer, GPU_PASS_WATER);
		// A pause before the water: the depth so far (terrain, objects) into
		// the pyramid the next frame's culling uses (hiz.h), and the
		// reflections march over the scene drawn so far: a copy of it (the
		// water is drawn into the same target)
		{
			bool capture = world->lit_render.water_reflections && !camera->underwater
				&& water_renderer_visible(&world->water, &world->terrain, camera);
			const clouds_t *mirrored_clouds = world->lit_render.volumetric_clouds && !world->clouds.failed ? &world->clouds : NULL;
			bool drawn = false;

			render_target_pause(&world->scene_target, pass);
			// (the ambient occlusion and the contact shadows (one pass) over
			// what is drawn so far: before the water, which takes the scene's
			// color as it is for its reflections)
			if (world->settings->render.ambient_occlusion && !camera->underwater)
			{
				bool shadows = world->lit_render.shadow_strength > 0.0f;

				ambient_occlusion_draw(&world->ambient_occlusion, world->scene_target.color_texture, world->scene_target.depth_texture,
					(uint32_t)world->scene_target.width, (uint32_t)world->scene_target.height, &scene, &world->settings->render,
					shadows ? world->shadow.depth_texture : NULL, shadows ? world->near_shadow.depth_texture : NULL);
			}
			hiz_build(&world->hiz, world->scene_target.depth_texture, (uint32_t)world->scene_target.width,
				(uint32_t)world->scene_target.height, camera->view_projection.m);
			trace_mark("sky drawn, depth pyramid");
			if (capture)
			{
				water_renderer_capture(&world->water_renderer, world->scene_target.color_texture, world->scene_target.depth_texture,
					world->scene_target.width, world->scene_target.height);
				// The reflections at half size: the water in passes of its own
				if (world->lit_render.water_reflections_half_size)
				{
					drawn = water_renderer_draw_split(&world->water_renderer, world->scene_target.color_texture,
						world->scene_target.depth_texture, world->scene_target.width, world->scene_target.height, &scene, &world->water,
						&world->terrain, &world->renderer, camera, &world->lit_render, &world->shadow, &world->near_shadow,
						mirrored_clouds);
				}
			}
			pass = render_target_resume(&world->scene_target);
			if (pass == NULL)
			{
				return;
			}
			scene_uniforms_push(&scene);
			if (!drawn)
			{
				water_renderer_draw(&world->water_renderer, pass, &world->water, &world->terrain, &world->renderer, camera,
					&world->lit_render, &world->shadow, &world->near_shadow, mirrored_clouds);
			}
		}
		gpu_timer_end(&world->gpu_timer, GPU_PASS_WATER);
		trace_mark("water drawn");
		// The volumetric clouds over the sky and the land, up to the terrain
		// (they read the scene's depth: a pass of their own, without it;
		// the scene's own again after). Before the valley fog: it lies over
		// them where the eye looks through it
		if (world->lit_render.volumetric_clouds && !world->clouds.failed && !camera->underwater)
		{
			SDL_GPURenderPass *cloud_pass;

			render_target_pause(&world->scene_target, pass);
			cloud_pass = clouds_draw(&world->clouds, world->scene_target.color_texture, world->scene_target.depth_texture,
				(uint32_t)world->scene_target.width, (uint32_t)world->scene_target.height, world->renderer.materials.variation_texture,
				&scene, &world->lit_render, &world->cloud_paint);
			if (cloud_pass != NULL)
			{
				render_target_pause(&world->scene_target, cloud_pass);
			}
			pass = render_target_resume(&world->scene_target);
			if (pass == NULL)
			{
				return;
			}
			scene_uniforms_push(&scene);
		}
		else
		{
			// Their dome would be stale when they come back
			clouds_forget(&world->clouds);
		}
		trace_mark("clouds");
		// The valley fog over it all (fog.h: a pass of its own)
		if (world->lit_render.fog_mode != FOG_MODE_OFF && !camera->underwater)
		{
			float wind_radians = world->lit_render.wind_direction_degrees * SDL_PI_F / 180.0f;
			float wind[2] =
			{
				SDL_cosf(wind_radians) * world->lit_render.wind_speed_meters_per_second,
				SDL_sinf(wind_radians) * world->lit_render.wind_speed_meters_per_second,
			};

			fog_update(&world->fog, &world->terrain, &world->water, &world->lit_render, world->elapsed_seconds);
			render_target_pause(&world->scene_target, pass);
			fog_draw(&world->fog, world->scene_target.color_texture, world->scene_target.depth_texture, (uint32_t)world->scene_target.width,
				(uint32_t)world->scene_target.height, &scene, &world->lit_render, world->shadow.depth_texture, world->near_shadow.depth_texture,
				(float)world->day_hour, wind, world->elapsed_seconds, (float)world->terrain.size * world->terrain.cell_size_meters);
			pass = render_target_resume(&world->scene_target);
			if (pass == NULL)
			{
				return;
			}
			scene_uniforms_push(&scene);
		}
		trace_mark("fog");
		lights_draw_cores(&world->lights, pass, camera);
		particles_draw(&world->particles, pass, camera, &world->lit_render, scene_height);
		trace_mark("lights, particles");
		// The rainbow and the rain over everything (the clouds too), with
		// the depth test (rain.h)
		if (!camera->underwater
			&& (rain_intensity(&world->rain, &world->settings->render) > 0.0f
				|| rainbow_intensity(&world->rain, &world->settings->render, world->daylight.sun_elevation_degrees) > 0.0f))
		{
			rain_world_t rain_world;

			render_target_pause(&world->scene_target, pass);
			pass = render_target_resume(&world->scene_target);
			if (pass == NULL)
			{
				return;
			}
			scene_uniforms_push(&scene);
			SDL_zero(rain_world);
			rain_world.height_texture = world->renderer.height_texture;
			rain_world.water_texture = water_gpu_depth_texture(&world->water);
			rain_world.map_size = world->terrain.size;
			rain_world.cell_size_meters = world->terrain.cell_size_meters;
			{
				float wind_radians = world->lit_render.wind_direction_degrees * SDL_PI_F / 180.0f;

				rain_world.wind[0] = SDL_cosf(wind_radians) * world->lit_render.wind_speed_meters_per_second * 0.4f;
				rain_world.wind[1] = SDL_sinf(wind_radians) * world->lit_render.wind_speed_meters_per_second * 0.4f;
			}
			rain_world.seconds = (float)world->elapsed_seconds;
			rain_world.screen_height = (float)world->scene_target.height;
			// What falls: snow as soon as the snow's months have begun (a
			// week or so of sleet), rain again as they end
			rain_world.snow = SDL_min(4.0f * season_snow(world->objects.month, world->objects.snow_first_month, world->objects.snow_last_month), 1.0f);
			rain_draw(&world->rain, pass, camera, &world->lit_render, &rain_world, world->daylight.sun_elevation_degrees);
		}
		trace_mark("rain");
		// What the application draws over everything of the world (the
		// weapon in hand)
		if (options->draw_in_scene != NULL)
		{
			options->draw_in_scene(options->user, pass, scene_width, scene_height);
		}
		// The occlusion culling seen, over everything (objects.h)
		objects_draw_occlusion_debug(&world->objects, pass, camera->near_distance, camera->far_distance);
		gpu_timer_begin(&world->gpu_timer, GPU_PASS_UPSCALE);
		render_target_end(&world->scene_target, pass);
		{
			// The temporal anti-aliasing: the pixels averaged with where
			// they were (the camera's motion), the weapon in hand held still
			SDL_GPUTexture *picture = world->scene_target.color_texture;

			if (world->temporal_active)
			{
				// (this frame's clip space to the world from this eye, on by
				// how far the eye went since, into last frame's)
				mat4_t moved = mat4_identity();
				mat4_t reprojection;
				float still_depth = 0.0f;

				moved.m[12] = world->temporal_eye.x - world->temporal_previous_eye.x;
				moved.m[13] = world->temporal_eye.y - world->temporal_previous_eye.y;
				moved.m[14] = world->temporal_eye.z - world->temporal_previous_eye.z;
				reprojection = mat4_multiply(world->temporal_previous_view_projection, mat4_multiply(moved, world->temporal_inverse));

				if (!world->temporal_previous_valid)
				{
					post_temporal_reset(&world->post);
				}
				if (camera->first_person)
				{
					// (the depth, 0..1, of 0.6 m off)
					const float still_meters = 0.6f;
					float n = camera->near_distance;
					float f = camera->far_distance;

					still_depth = f * (still_meters - n) / (still_meters * (f - n));
				}
				picture = post_temporal(&world->post, world->scene_target.color_texture, world->scene_target.depth_texture, world->scene_target.width,
					world->scene_target.height, reprojection.m, world->lit_render.taa_history_weight, still_depth, world->lit_render.taa_sharpness);
				world->temporal_previous_view_projection = world->temporal_view_projection;
				world->temporal_previous_eye = world->temporal_eye;
				world->temporal_previous_valid = true;
			}
			else
			{
				post_temporal_reset(&world->post);
				world->temporal_previous_valid = false;
			}
			post_draw(&world->post, picture, world->scene_target.width, world->scene_target.height, &world->lit_render, world->elapsed_seconds,
				options->capture);
		}
		gpu_timer_end(&world->gpu_timer, GPU_PASS_UPSCALE);
		trace_mark("post (exposure, bloom, tone)");
	}
}

// One of the Halton sequence's numbers (0..1): the jitter's pattern
static float halton(uint32_t index, uint32_t base)
{
	float result = 0.0f;
	float fraction = 1.0f;

	while (index > 0)
	{
		fraction /= (float)base;
		result += fraction * (float)(index % base);
		index /= base;
	}
	return result;
}

void world_render(world_t *world, camera_t *camera, const world_render_options_t *options, int pixel_width, int pixel_height)
{
	// The temporal anti-aliasing: the buildings drawn a fraction of a pixel
	// off (eight places in turn; prop_vertex.glsl), the rest as it is; the
	// camera's view kept for the reprojection
	world->temporal_jitter[0] = 0.0f;
	world->temporal_jitter[1] = 0.0f;
	// (only while the eye goes forward or back, when asked: a millimeter a
	// frame along where it looks, more that way than sideways, and a
	// quarter of a second after it stops; turning round, going sideways or
	// standing is not: Radu, things far off flickered with it standing)
	{
		vec3_t eye = camera->eye_position;
		float dx = eye.x - world->temporal_last_eye.x;
		float dy = eye.y - world->temporal_last_eye.y;
		float dz = eye.z - world->temporal_last_eye.z;
		vec4_t near_point = mat4_transform(camera->inverse_view_projection, (vec4_t){ 0.0f, 0.0f, -1.0f, 1.0f });
		vec4_t far_point = mat4_transform(camera->inverse_view_projection, (vec4_t){ 0.0f, 0.0f, 1.0f, 1.0f });
		float fx = far_point.x / far_point.w - near_point.x / near_point.w;
		float fy = far_point.y / far_point.w - near_point.y / near_point.w;
		float fz = far_point.z / far_point.w - near_point.z / near_point.w;
		float length = SDL_sqrtf(fx * fx + fy * fy + fz * fz);
		float along = length > 1e-6f ? (dx * fx + dy * fy + dz * fz) / length : 0.0f;
		float moved_squared = dx * dx + dy * dy + dz * dz;

		if (along * along > 1e-6f && along * along > 0.5f * moved_squared)
		{
			world->temporal_moving_until = world->elapsed_seconds + 0.25;
		}
		world->temporal_last_eye = eye;
		world->temporal_active = world->lit_render.taa
			&& (!world->settings->render.taa_when_moving || world->elapsed_seconds < world->temporal_moving_until);
	}
	if (world->temporal_active)
	{
		float scale = SDL_clamp(world->lit_render.render_scale, 0.25f, 2.0f);
		int width = SDL_max((int)SDL_lroundf((float)pixel_width * scale), 1);
		int height = SDL_max((int)SDL_lroundf((float)pixel_height * scale), 1);
		uint32_t index = world->temporal_frame % 8u + 1u;

		world->temporal_frame++;
		world->temporal_view_projection = camera->view_projection_relative;
		world->temporal_inverse = camera->inverse_view_projection_relative;
		world->temporal_eye = camera->eye_position;
		// (in clip space: a pixel is 2 / the size)
		world->temporal_jitter[0] = (halton(index, 2u) - 0.5f) * 2.0f / (float)width;
		world->temporal_jitter[1] = (halton(index, 3u) - 0.5f) * 2.0f / (float)height;
	}
	render_frame(world, camera, options, pixel_width, pixel_height);
}
