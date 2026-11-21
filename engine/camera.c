// Orbit camera. See camera.h.

#include "camera.h"

#include <SDL3/SDL.h>

// Keep the eye at least this high above the ground under it
#define CAMERA_MINIMUM_CLEARANCE_METERS 3.0f

// The walker (first person)
#define WALKER_EYE_HEIGHT_METERS 1.7f
#define WALKER_GRAVITY 20.0f              // m/s^2, a bit more than real: jumps feel less floaty
#define WALKER_JUMP_SPEED 6.0f            // m/s up
#define WALKER_STEP_DOWN_METERS 0.6f      // walking downhill keeps the feet on the ground up to this drop per frame
#define WALKER_WADE_DEPTH_METERS 0.3f     // deeper water slows walking down...
#define WALKER_SWIM_DEPTH_METERS 1.3f     // ...and deeper than this the walker swims
#define WALKER_FLOAT_EYE_METERS 0.25f     // swimming: the eyes this far above the surface
#define WALKER_SWIM_VERTICAL_SPEED 2.5f   // diving and swimming up, m/s
#define WALKER_RISE_SPEED 0.5f            // drifting back up when not swimming anywhere, m/s

// Crawling (Ctrl on dry ground): the eye this high, this fast, getting
// down and up at this many meters a second
static const float WALKER_CRAWL_EYE_METERS = 0.5f;
static const float WALKER_CRAWL_METERS_PER_SECOND = 1.0f;
static const float WALKER_CROUCH_METERS_PER_SECOND = 3.0f;

// The eye above the feet now (a camera from before crawling: standing)
static float walker_eye(const camera_t *camera)
{
	return camera->walker_eye_meters > 0.0f ? camera->walker_eye_meters : WALKER_EYE_HEIGHT_METERS;
}

void camera_reset(camera_t *camera, const camera_settings_t *settings, const terrain_t *terrain)
{
	float extent = (float)terrain->size * terrain->cell_size_meters;

	SDL_zerop(camera);
	camera->target_x = extent * 0.5f;
	camera->target_z = extent * 0.5f;
	camera->target_height = terrain_sample_height_meters(terrain, camera->target_x, camera->target_z);
	camera->distance_meters = clamp_float(extent * 1.1f, settings->minimum_distance_meters, settings->maximum_distance_meters);
	camera->yaw_degrees = 35.0f;
	camera->pitch_degrees = settings->default_pitch_degrees;
}

// Height of the visible area at the target distance, used to make panning
// speed proportional to what is on screen.
static float visible_height_meters(const camera_t *camera, const camera_settings_t *settings)
{
	return 2.0f * camera->distance_meters * SDL_tanf(degrees_to_radians(settings->field_of_view_degrees) * 0.5f);
}

void camera_update(camera_t *camera, const input_state_t *input, const camera_settings_t *settings,
	const terrain_t *terrain, float frame_seconds, int viewport_height)
{
	float extent = (float)terrain->size * terrain->cell_size_meters;
	float yaw_radians;
	float forward_x;
	float forward_z;
	float right_x;
	float right_z;
	float meters_per_pixel;
	float pan_right_meters;
	float pan_forward_meters;
	float ground_height;

	// Rotation: mouse drag and Q/E
	camera->yaw_degrees -= input->rotate_drag_x_pixels * settings->rotate_degrees_per_pixel;
	camera->yaw_degrees += input->rotate_keyboard * 90.0f * frame_seconds;
	camera->yaw_degrees = SDL_fmodf(camera->yaw_degrees + 360.0f, 360.0f);
	camera->pitch_degrees += input->rotate_drag_y_pixels * settings->rotate_degrees_per_pixel;
	camera->pitch_degrees = clamp_float(camera->pitch_degrees, settings->minimum_pitch_degrees, settings->maximum_pitch_degrees);

	// Zoom: each wheel step scales the distance by a fixed factor
	if (input->zoom_steps != 0.0f)
	{
		camera->distance_meters *= SDL_powf(settings->zoom_step_factor, -input->zoom_steps);
		camera->distance_meters = clamp_float(camera->distance_meters, settings->minimum_distance_meters, settings->maximum_distance_meters);
	}

	// Pan along the ground, relative to the view direction. The eye sits at
	// target + (sin(yaw), ., cos(yaw)) * distance, so "forward" on the ground
	// points the other way.
	yaw_radians = degrees_to_radians(camera->yaw_degrees);
	forward_x = -SDL_sinf(yaw_radians);
	forward_z = -SDL_cosf(yaw_radians);
	right_x = -forward_z;
	right_z = forward_x;

	meters_per_pixel = visible_height_meters(camera, settings) / (float)SDL_max(viewport_height, 1);
	// Dragging moves the ground with the cursor, so the target goes the
	// opposite way of the drag
	pan_right_meters = -input->pan_drag_x_pixels * meters_per_pixel;
	pan_forward_meters = input->pan_drag_y_pixels * meters_per_pixel;
	pan_right_meters += input->pan_keyboard_x * settings->keyboard_pan_speed * visible_height_meters(camera, settings) * frame_seconds;
	pan_forward_meters += input->pan_keyboard_y * settings->keyboard_pan_speed * visible_height_meters(camera, settings) * frame_seconds;

	camera->target_x += right_x * pan_right_meters + forward_x * pan_forward_meters;
	camera->target_z += right_z * pan_right_meters + forward_z * pan_forward_meters;
	camera->target_x = clamp_float(camera->target_x, 0.0f, extent);
	camera->target_z = clamp_float(camera->target_z, 0.0f, extent);

	// Let the target glide to the ground height instead of jumping, so the
	// view does not jitter while panning over rough terrain
	ground_height = terrain_sample_height_meters(terrain, camera->target_x, camera->target_z);
	camera->target_height = lerp_float(camera->target_height, ground_height, clamp_float(frame_seconds * 6.0f, 0.0f, 1.0f));
}

void camera_enter_first_person(camera_t *camera, const terrain_t *terrain)
{
	camera->first_person = true;
	camera->walker_x = camera->target_x;
	camera->walker_z = camera->target_z;
	camera->walker_feet = terrain_sample_height_meters(terrain, camera->walker_x, camera->walker_z);
	camera->walker_vertical_speed = 0.0f;
	camera->walker_yaw_degrees = camera->yaw_degrees;
	camera->walker_pitch_degrees = 0.0f;
	camera->walker_on_ground = true;
	camera->walker_swimming = false;
}

void camera_leave_first_person(camera_t *camera, const camera_settings_t *settings)
{
	camera->first_person = false;
	camera->target_x = camera->walker_x;
	camera->target_z = camera->walker_z;
	camera->yaw_degrees = camera->walker_yaw_degrees;
	camera->distance_meters = clamp_float(40.0f, settings->minimum_distance_meters, settings->maximum_distance_meters);
}

// The walker's ground: the world's if it has one, else the terrain; on
// water the ice bears, its surface
static float walker_ground(const camera_walker_world_t *world, const terrain_t *terrain, const water_t *water, float x, float z,
	float near_height)
{
	float ground = world != NULL && world->ground_height != NULL ? world->ground_height(world->context, x, z, near_height)
		: terrain_sample_height_meters(terrain, x, z);
	float ice;

	if (water != NULL && water->iced && water_ice_surface(water, terrain, x, z, &ice))
	{
		ground = SDL_max(ground, ice);
	}
	return ground;
}

void camera_update_first_person(camera_t *camera, const input_state_t *input, const camera_settings_t *settings,
	const terrain_t *terrain, const water_t *water, const camera_walker_world_t *world, float frame_seconds)
{
	float extent = (float)terrain->size * terrain->cell_size_meters;
	float zoom = SDL_max(camera->zoom, 1.0f);
	float look_per_pixel = settings->first_person_look_degrees_per_pixel / zoom;
	float look_up_per_pixel = settings->first_person_invert_mouse ? look_per_pixel : -look_per_pixel;
	float yaw_radians;
	float forward_x;
	float forward_z;
	float move_x;
	float move_z;
	float move_length;
	float speed;
	float ground;
	float depth = 0.0f;
	float current_x = 0.0f;
	float current_z = 0.0f;
	float level;
	float dt = SDL_min(frame_seconds, 0.1f);

	// Looking around
	camera->walker_yaw_degrees -= input->look_x_pixels * look_per_pixel;
	camera->walker_yaw_degrees = SDL_fmodf(camera->walker_yaw_degrees + 360.0f, 360.0f);
	camera->walker_pitch_degrees += input->look_y_pixels * look_up_per_pixel;
	camera->walker_pitch_degrees = clamp_float(camera->walker_pitch_degrees, -89.0f, 89.0f);

	// Flying through everything (noclip): where it looks, Space up and Ctrl
	// down, no ground, no gravity, nothing in the way
	if (camera->noclip)
	{
		float yaw = degrees_to_radians(camera->walker_yaw_degrees);
		float pitch = degrees_to_radians(camera->walker_pitch_degrees);
		float look_x = -SDL_sinf(yaw) * SDL_cosf(pitch);
		float look_y = SDL_sinf(pitch);
		float look_z = -SDL_cosf(yaw) * SDL_cosf(pitch);
		float side_x = SDL_cosf(yaw);
		float side_z = -SDL_sinf(yaw);
		float fly_speed = settings->walk_meters_per_second * (input->run_held ? 6.0f : 2.0f);
		float up = (input->jump_held ? 1.0f : 0.0f) - (input->dive_held ? 1.0f : 0.0f);

		camera->walker_x = clamp_float(camera->walker_x + (look_x * input->pan_keyboard_y + side_x * input->pan_keyboard_x) * fly_speed * dt, 0.0f, extent);
		camera->walker_z = clamp_float(camera->walker_z + (look_z * input->pan_keyboard_y + side_z * input->pan_keyboard_x) * fly_speed * dt, 0.0f, extent);
		camera->walker_feet += (look_y * input->pan_keyboard_y + up) * fly_speed * dt;
		camera->walker_vertical_speed = 0.0f;
		camera->walker_velocity_x = 0.0f;
		camera->walker_velocity_z = 0.0f;
		camera->walker_on_ground = false;
		camera->walker_swimming = false;
		camera->walker_crawling = false;
		camera->target_x = camera->walker_x;
		camera->target_z = camera->walker_z;
		camera->target_height = terrain_sample_height_meters(terrain, camera->walker_x, camera->walker_z);
		return;
	}

	// Walking: W A S D along the ground, relative to where the walker faces
	yaw_radians = degrees_to_radians(camera->walker_yaw_degrees);
	forward_x = -SDL_sinf(yaw_radians);
	forward_z = -SDL_cosf(yaw_radians);
	move_x = -forward_z * input->pan_keyboard_x + forward_x * input->pan_keyboard_y;
	move_z = forward_x * input->pan_keyboard_x + forward_z * input->pan_keyboard_y;
	move_length = SDL_sqrtf(move_x * move_x + move_z * move_z);
	if (move_length > 1.0f)
	{
		move_x /= move_length;
		move_z /= move_length;
	}

	water_sample(water, camera->walker_x, camera->walker_z, &depth, &current_x, &current_z);
	// (on the ice: no water to wade or swim in)
	if (water->iced)
	{
		depth = 0.0f;
	}
	ground = terrain_sample_height_meters(terrain, camera->walker_x, camera->walker_z);
	level = ground + depth;
	ground = walker_ground(world, terrain, water, camera->walker_x, camera->walker_z, camera->walker_feet);
	camera->walker_swimming = depth > WALKER_SWIM_DEPTH_METERS && camera->walker_feet < level - WALKER_EYE_HEIGHT_METERS * 0.5f;

	// Crawling: Ctrl on dry ground, on until pressed again (or running,
	// jumping, the water: in it Ctrl dives)
	if (camera->walker_swimming || (depth > WALKER_WADE_DEPTH_METERS && camera->walker_feet < level) || input->run_held
		|| input->jump_pressed)
	{
		camera->walker_crawling = false;
	}
	else if (input->dive_pressed)
	{
		camera->walker_crawling = !camera->walker_crawling;
	}
	{
		float wanted_eye = camera->walker_crawling ? WALKER_CRAWL_EYE_METERS : WALKER_EYE_HEIGHT_METERS;
		float eye = walker_eye(camera);
		float step = WALKER_CROUCH_METERS_PER_SECOND * dt;

		camera->walker_eye_meters = eye < wanted_eye ? SDL_min(eye + step, wanted_eye) : SDL_max(eye - step, wanted_eye);
	}
	speed = camera->walker_crawling ? WALKER_CRAWL_METERS_PER_SECOND
		: (input->run_held ? settings->run_meters_per_second : settings->walk_meters_per_second);
	if (camera->walker_swimming)
	{
		// Swimming: half the speed, and the river carries the walker along
		speed *= 0.5f;
		move_x = move_x * speed + current_x * 0.8f;
		move_z = move_z * speed + current_z * 0.8f;
	}
	else if (depth > WALKER_WADE_DEPTH_METERS && camera->walker_feet < level)
	{
		// Wading: slower, and the current pushes a little
		speed *= 0.6f;
		move_x = move_x * speed + current_x * 0.3f;
		move_z = move_z * speed + current_z * 0.3f;
	}
	else
	{
		move_x *= speed;
		move_z *= speed;
	}
	{
		// Houses, trees and big rocks stop it; along them it slides
		float new_x = camera->walker_x;
		float new_z = camera->walker_z;

		if (world != NULL && world->move != NULL)
		{
			world->move(world->context, &new_x, &new_z, camera->walker_feet, move_x * dt, move_z * dt);
		}
		else
		{
			new_x += move_x * dt;
			new_z += move_z * dt;
		}
		camera->walker_x = clamp_float(new_x, 0.0f, extent);
		camera->walker_z = clamp_float(new_z, 0.0f, extent);
		camera->walker_velocity_x = move_x;
		camera->walker_velocity_z = move_z;
	}
	ground = walker_ground(world, terrain, water, camera->walker_x, camera->walker_z, camera->walker_feet);

	if (camera->walker_swimming)
	{
		// Afloat. Ctrl dives straight down and Space swims straight up;
		// swimming forward goes where the walker looks (down into the
		// water, or back up to the surface); otherwise it drifts back up,
		// slowly while deep, and settles at the surface
		float float_feet = level + WALKER_FLOAT_EYE_METERS - WALKER_EYE_HEIGHT_METERS;
		float settle_speed = clamp_float((float_feet - camera->walker_feet) * 2.0f, -WALKER_SWIM_VERTICAL_SPEED, WALKER_SWIM_VERTICAL_SPEED);
		float heading_climb = SDL_sinf(degrees_to_radians(camera->walker_pitch_degrees)) * input->pan_keyboard_y;

		if (input->dive_held)
		{
			camera->walker_vertical_speed = -WALKER_SWIM_VERTICAL_SPEED;
		}
		else if (input->jump_held)
		{
			camera->walker_vertical_speed = WALKER_SWIM_VERTICAL_SPEED;
		}
		else if (heading_climb < -0.05f || (heading_climb > 0.05f && camera->walker_feet < float_feet))
		{
			// Never above the floating height by swimming up
			camera->walker_vertical_speed = heading_climb * speed;
			if (heading_climb > 0.0f)
			{
				camera->walker_vertical_speed = SDL_min(camera->walker_vertical_speed, SDL_max(settle_speed, 0.0f));
			}
		}
		else
		{
			camera->walker_vertical_speed = SDL_min(settle_speed, WALKER_RISE_SPEED + SDL_max(0.0f, 0.5f - (float_feet - camera->walker_feet))
				* 2.0f * WALKER_SWIM_VERTICAL_SPEED);
		}
		camera->walker_feet += camera->walker_vertical_speed * dt;
		camera->walker_on_ground = false;
		if (camera->walker_feet <= ground)
		{
			camera->walker_feet = ground;
			camera->walker_vertical_speed = SDL_max(camera->walker_vertical_speed, 0.0f);
		}
	}
	else
	{
		if (camera->walker_on_ground && input->jump_pressed)
		{
			// (the height goes with the speed squared)
			camera->walker_vertical_speed = WALKER_JUMP_SPEED * SDL_sqrtf(SDL_clamp(settings->jump_height_scale, 0.1f, 100.0f));
			camera->walker_on_ground = false;
		}
		camera->walker_vertical_speed -= WALKER_GRAVITY * dt;
		camera->walker_feet += camera->walker_vertical_speed * dt;
		// Downhill the feet stay on the ground instead of hopping off it
		if (camera->walker_on_ground && camera->walker_vertical_speed <= 0.0f && camera->walker_feet - ground < WALKER_STEP_DOWN_METERS)
		{
			camera->walker_feet = ground;
		}
		if (camera->walker_feet <= ground)
		{
			camera->walker_feet = ground;
			camera->walker_vertical_speed = 0.0f;
			camera->walker_on_ground = true;
		}
		else if (camera->walker_feet > ground + 0.01f)
		{
			camera->walker_on_ground = false;
		}
	}

	// The walker is the orbit target too (near shadows follow it)
	camera->target_x = camera->walker_x;
	camera->target_z = camera->walker_z;
	camera->target_height = ground;
}

bool camera_box_within_view_distance(const camera_t *camera, vec3_t box_minimum, vec3_t box_maximum)
{
	float center_x = camera->first_person ? camera->eye_position.x : camera->target_x;
	float center_z = camera->first_person ? camera->eye_position.z : camera->target_z;
	float outside_x;
	float outside_z;

	if (camera->view_distance_meters <= 0.0f)
	{
		return true;
	}
	outside_x = SDL_max(SDL_max(box_minimum.x - center_x, center_x - box_maximum.x), 0.0f);
	outside_z = SDL_max(SDL_max(box_minimum.z - center_z, center_z - box_maximum.z), 0.0f);
	return outside_x * outside_x + outside_z * outside_z <= camera->view_distance_meters * camera->view_distance_meters;
}

void camera_compute_matrices(camera_t *camera, const camera_settings_t *settings, const terrain_t *terrain, float aspect_ratio)
{
	camera->view_distance_meters = settings->view_distance_meters;
	if (camera->first_person)
	{
		float yaw_radians = degrees_to_radians(camera->walker_yaw_degrees);
		float pitch_radians = degrees_to_radians(camera->walker_pitch_degrees);
		vec3_t eye = camera->fixed ? vec3_make(camera->fixed_eye[0], camera->fixed_eye[1], camera->fixed_eye[2])
			: vec3_make(camera->walker_x, camera->walker_feet + walker_eye(camera), camera->walker_z);
		vec3_t direction = vec3_make(-SDL_cosf(pitch_radians) * SDL_sinf(yaw_radians), SDL_sinf(pitch_radians),
			-SDL_cosf(pitch_radians) * SDL_cosf(yaw_radians));

		camera->eye_position = eye;
		camera->near_distance = SDL_clamp(settings->first_person_near_meters, 0.02f, 1.0f);
		camera->far_distance = (float)terrain->size * terrain->cell_size_meters * 1.5f;
		camera->view = mat4_look_at(eye, vec3_add(eye, direction), vec3_make(0.0f, 1.0f, 0.0f));
		// (aiming: narrower, as many times as it zooms)
		float half_angle = SDL_atanf(SDL_tanf(degrees_to_radians(settings->first_person_field_of_view_degrees) * 0.5f) / SDL_max(camera->zoom, 1.0f));

		camera->projection = mat4_perspective(2.0f * half_angle, aspect_ratio, camera->near_distance, camera->far_distance);
		camera->view_projection = mat4_multiply(camera->projection, camera->view);
		mat4_inverse(camera->view_projection, &camera->inverse_view_projection);
		camera->view_projection_relative = mat4_multiply(camera->projection,
			mat4_look_at(vec3_make(0.0f, 0.0f, 0.0f), direction, vec3_make(0.0f, 1.0f, 0.0f)));
		mat4_inverse(camera->view_projection_relative, &camera->inverse_view_projection_relative);
		return;
	}

	float yaw_radians = degrees_to_radians(camera->yaw_degrees);
	float pitch_radians = degrees_to_radians(camera->pitch_degrees);
	vec3_t target = vec3_make(camera->target_x, camera->target_height, camera->target_z);
	vec3_t offset = vec3_make(
		SDL_cosf(pitch_radians) * SDL_sinf(yaw_radians),
		SDL_sinf(pitch_radians),
		SDL_cosf(pitch_radians) * SDL_cosf(yaw_radians));
	vec3_t eye = vec3_add(target, vec3_scale(offset, camera->distance_meters));
	float ground_under_eye = terrain_sample_height_meters(terrain, eye.x, eye.z);
	float map_diagonal = (float)terrain->size * terrain->cell_size_meters * 1.5f;

	// Never let the eye dip below a hill between it and the target
	if (eye.y < ground_under_eye + CAMERA_MINIMUM_CLEARANCE_METERS)
	{
		eye.y = ground_under_eye + CAMERA_MINIMUM_CLEARANCE_METERS;
	}

	camera->eye_position = eye;
	// Near plane proportional to distance keeps depth precision usable at
	// every zoom level; far plane covers the whole map plus the orbit.
	camera->near_distance = SDL_max(0.5f, camera->distance_meters * 0.02f);
	// Under water (as of the last frame) the surface can be a hand's width
	// above the eye: pull the near plane in so it is not cut away
	if (camera->underwater)
	{
		camera->near_distance = 0.1f;
	}
	camera->far_distance = map_diagonal + camera->distance_meters * 2.0f;

	camera->view = mat4_look_at(eye, target, vec3_make(0.0f, 1.0f, 0.0f));
	camera->projection = mat4_perspective(degrees_to_radians(settings->field_of_view_degrees), aspect_ratio,
		camera->near_distance, camera->far_distance);
	camera->view_projection = mat4_multiply(camera->projection, camera->view);
	mat4_inverse(camera->view_projection, &camera->inverse_view_projection);
	camera->view_projection_relative = mat4_multiply(camera->projection,
		mat4_look_at(vec3_make(0.0f, 0.0f, 0.0f), vec3_subtract(target, eye), vec3_make(0.0f, 1.0f, 0.0f)));
	mat4_inverse(camera->view_projection_relative, &camera->inverse_view_projection_relative);
}

void camera_pixel_ray(const camera_t *camera, float pixel_x, float pixel_y, int viewport_width, int viewport_height,
	vec3_t *origin_out, vec3_t *direction_out)
{
	float ndc_x = 2.0f * pixel_x / (float)SDL_max(viewport_width, 1) - 1.0f;
	float ndc_y = 1.0f - 2.0f * pixel_y / (float)SDL_max(viewport_height, 1);
	vec4_t near_point = mat4_transform(camera->inverse_view_projection, (vec4_t){ ndc_x, ndc_y, -1.0f, 1.0f });
	vec4_t far_point = mat4_transform(camera->inverse_view_projection, (vec4_t){ ndc_x, ndc_y, 1.0f, 1.0f });
	vec3_t near_world = vec3_make(near_point.x / near_point.w, near_point.y / near_point.w, near_point.z / near_point.w);
	vec3_t far_world = vec3_make(far_point.x / far_point.w, far_point.y / far_point.w, far_point.z / far_point.w);

	*origin_out = near_world;
	*direction_out = vec3_normalize(vec3_subtract(far_world, near_world));
}

// ---------------------------------------------------------------------------
// The camera as an API

void camera_set_first_person(camera_t *camera, const camera_settings_t *settings, const terrain_t *terrain, bool on)
{
	if (on && !camera->first_person)
	{
		camera_enter_first_person(camera, terrain);
	}
	else if (!on && camera->first_person)
	{
		camera_leave_first_person(camera, settings);
	}
}

void camera_go_to(camera_t *camera, const terrain_t *terrain, float x, float y, float z)
{
	float extent = (float)terrain->size * terrain->cell_size_meters;

	x = clamp_float(x, 0.0f, extent);
	z = clamp_float(z, 0.0f, extent);
	if (camera->first_person)
	{
		float ground = terrain_sample_height_meters(terrain, x, z);

		camera->walker_x = x;
		camera->walker_z = z;
		camera->walker_feet = SDL_isnan(y) ? ground : SDL_max(y, ground);
		camera->walker_vertical_speed = 0.0f;
		camera->walker_on_ground = camera->walker_feet <= ground + 0.01f;
		camera->target_x = x;
		camera->target_z = z;
		camera->target_height = ground;
	}
	else
	{
		camera->target_x = x;
		camera->target_z = z;
		camera->target_height = terrain_sample_height_meters(terrain, x, z);
	}
}

void camera_set_heading(camera_t *camera, const camera_settings_t *settings, float yaw_degrees, float pitch_degrees)
{
	yaw_degrees = SDL_fmodf(SDL_fmodf(yaw_degrees, 360.0f) + 360.0f, 360.0f);
	if (camera->first_person)
	{
		camera->walker_yaw_degrees = yaw_degrees;
		camera->walker_pitch_degrees = clamp_float(pitch_degrees, -89.0f, 89.0f);
	}
	else
	{
		// The orbit's pitch is the angle down to the target
		camera->yaw_degrees = yaw_degrees;
		camera->pitch_degrees = clamp_float(-pitch_degrees, settings->minimum_pitch_degrees, settings->maximum_pitch_degrees);
	}
}

void camera_get_heading(const camera_t *camera, float *yaw_degrees_out, float *pitch_degrees_out)
{
	*yaw_degrees_out = camera->first_person ? camera->walker_yaw_degrees : camera->yaw_degrees;
	*pitch_degrees_out = camera->first_person ? camera->walker_pitch_degrees : -camera->pitch_degrees;
}

void camera_look_at(camera_t *camera, const camera_settings_t *settings, float x, float y, float z)
{
	vec3_t from = camera->first_person && camera->fixed ? vec3_make(camera->fixed_eye[0], camera->fixed_eye[1], camera->fixed_eye[2])
		: (camera->first_person ? vec3_make(camera->walker_x, camera->walker_feet + walker_eye(camera), camera->walker_z) : camera->eye_position);
	float dx = x - from.x;
	float dy = y - from.y;
	float dz = z - from.z;
	float flat = SDL_sqrtf(dx * dx + dz * dz);

	if (flat < 1e-4f && SDL_fabsf(dy) < 1e-4f)
	{
		return;
	}
	// Looking along (-sin yaw, -cos yaw) at yaw (see camera_compute_matrices)
	camera_set_heading(camera, settings, radians_to_degrees(SDL_atan2f(-dx, -dz)), radians_to_degrees(SDL_atan2f(dy, flat)));
}
