// Orbit camera: looks at a target point on the terrain from a distance,
// with free rotation around the vertical axis (yaw) and a limited elevation
// angle (pitch). Perspective projection with a narrow field of view, which
// gives an almost isometric look while keeping real depth.
//
// First person: a walker on the ground instead, eyes 1.7 m up, looking
// around with the mouse. It walks and runs on the terrain, jumps, wades
// (slower, pushed a little by the current) and swims in water deeper than
// its chest: floating at the surface, diving with ctrl, carried by the
// current. Its position doubles as the orbit target, so everything that
// follows the target (the near shadows, the camera when leaving first
// person) follows the walker.

#ifndef CAMERA_H
#define CAMERA_H

#include "math3d.h"
#include "input.h"
#include "settings.h"
#include "terrain.h"
#include "water.h"

typedef struct
{
	float target_x;                // world meters
	float target_z;
	float target_height;           // follows the terrain smoothly
	float distance_meters;
	float yaw_degrees;
	float pitch_degrees;

	// First person
	bool first_person;
	float walker_x;                // world meters
	float walker_z;
	float walker_feet;             // height of the feet
	float walker_vertical_speed;   // m/s, up is positive
	float walker_yaw_degrees;      // same convention as yaw_degrees
	float walker_pitch_degrees;    // up is positive
	bool walker_on_ground;
	bool walker_swimming;
	bool walker_crawling;          // Ctrl on dry ground: low and slow (unseen, unheard)
	bool noclip;                   // flying through everything, where it looks (console: noclip)
	float walker_eye_meters;       // the eye above the feet now (standing 1.7, crawling down to 0.5; eased)
	// The eye put somewhere (console: camera x y z): first person, it looks
	// from there (anywhere: in the grass, under a lid, in the air), turned
	// as the walker is (look, the mouse); the walker stays where it was
	bool fixed;
	float fixed_eye[3];
	float walker_velocity_x;       // m/s, how it meant to move this frame (what it pushes with)
	float walker_velocity_z;
	// Aiming: the field of view narrowed this many times (the mouse turns
	// the view as much slower); 1 (or 0): not
	float zoom;

	// Derived each frame by camera_compute_matrices
	vec3_t eye_position;
	mat4_t view;
	mat4_t projection;
	mat4_t view_projection;
	mat4_t inverse_view_projection;
	// The same from the eye (its translation left out: points given as
	// world minus eye_position). Drawing goes by these: a float32 world
	// position kilometers out times a matrix carrying the eye's as far
	// loses millimeters that change from frame to frame (the houses
	// trembled under the temporal anti-aliasing on a 4 km map)
	mat4_t view_projection_relative;
	mat4_t inverse_view_projection_relative;
	// The same with its far plane at the view distance (camera settings;
	// none: the far plane): what the terrain and the water draw is culled
	// by it, the rest as before (the sky, the clouds, far beyond)
	mat4_t cull_view_projection;
	float near_distance;
	float far_distance;

	// Set by the game each frame from the water at the eye (see
	// data/shaders/underwater.glsl)
	bool underwater;
	float water_level;             // the surface over the eye, meters
	float underwater_fog_per_meter;
	float underwater_color[3];     // linear: what far things fade into
} camera_t;

// Centers the camera over the terrain with a view of the whole map.
void camera_reset(camera_t *camera, const camera_settings_t *settings, const terrain_t *terrain);

// Applies the camera actions of this frame.
void camera_update(camera_t *camera, const input_state_t *input, const camera_settings_t *settings,
	const terrain_t *terrain, float frame_seconds, int viewport_height);

// Switches to first person, standing at the orbit target and facing the
// way the orbit camera looked; or back to the orbit camera, looking at
// where the walker stands.
void camera_enter_first_person(camera_t *camera, const terrain_t *terrain);
void camera_leave_first_person(camera_t *camera, const camera_settings_t *settings);

// What the walker stands on and runs into, when more than the terrain
// counts (objects: a bridge's deck, a rock's top, a house's wall)
typedef struct
{
	void *context;
	// The height of what is under (x, z) for feet at near_height
	float (*ground_height)(void *context, float x, float z, float near_height);
	// Moves the walker by (move_x, move_z) from (*x, *z), feet at
	// feet_height, as far as what stands in the way lets it (sliding along)
	void (*move)(void *context, float *x, float *z, float feet_height, float move_x, float move_z);
} camera_walker_world_t;

// First person: moves the walker by this frame's actions (instead of
// camera_update). world may be NULL: the bare terrain, nothing in the way.
void camera_update_first_person(camera_t *camera, const input_state_t *input, const camera_settings_t *settings,
	const terrain_t *terrain, const water_t *water, const camera_walker_world_t *world, float frame_seconds);

// The camera as an API (the game's scripts, the command line's screenshots).
// Angles in degrees: yaw as camera_t's (0 looks towards -Z, the view turns
// counterclockwise seen from above), pitch up positive.
//
// First person on or off (the walker stands where the orbit target was, or
// the orbit target goes where the walker stands)
void camera_set_first_person(camera_t *camera, const camera_settings_t *settings, const terrain_t *terrain, bool on);
// Moves the camera: in first person the walker's feet to (x, z) at height
// y (on the ground if y is NAN; above it the walker falls); orbiting, the
// target to (x, z), the view keeping its angles and distance (y ignored)
void camera_go_to(camera_t *camera, const terrain_t *terrain, float x, float y, float z);
// Turns towards a point: in first person the walker's eyes, orbiting the
// camera around its target so that it looks over the target to the point
// (pitch only within the orbit's limits)
void camera_look_at(camera_t *camera, const camera_settings_t *settings, float x, float y, float z);
void camera_set_heading(camera_t *camera, const camera_settings_t *settings, float yaw_degrees, float pitch_degrees);
void camera_get_heading(const camera_t *camera, float *yaw_degrees_out, float *pitch_degrees_out);

// Computes eye position and matrices for the given aspect ratio.
void camera_compute_matrices(camera_t *camera, const camera_settings_t *settings, const terrain_t *terrain, float aspect_ratio);


// World space ray through a window pixel. Requires up to date matrices.
void camera_pixel_ray(const camera_t *camera, float pixel_x, float pixel_y, int viewport_width, int viewport_height,
	vec3_t *origin_out, vec3_t *direction_out);

#endif
