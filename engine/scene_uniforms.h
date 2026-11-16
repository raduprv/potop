// What every lit shader knows of the scene (data/shaders/scene.glsl): one
// block, filled once a frame and pushed to both shader stages at the start
// of each pass that draws lit things.

#ifndef SCENE_UNIFORMS_H
#define SCENE_UNIFORMS_H

#include "camera.h"
#include "settings.h"
#include "shadow_map.h"
#include "terrain.h"

// std140, as scene_block in scene.glsl: vec3 + float pairs share 16 bytes
typedef struct
{
	float view_projection[16];
	float inverse_view_projection[16];
	float light_view_projection[16];
	float near_light_view_projection[16];
	float eye_position[3];
	float time_seconds;
	float sun_direction[3];
	float ambient_intensity;
	float sun_color[3];
	float fog_density;
	float sky_color[3];
	float shadow_strength;
	float sky_zenith_color[3];
	float sky_horizon_drop;
	float cloud_parameters[4];
	float cloud_motion[3];
	float shadow_texel_size;
	float underwater_color[4];
	float underwater_parameters[4];
	float near_shadow_texel_size;
	float padding[3];
	float fog_parameters[4];       // x: the haze's scale height (meters)
	float twilight[4];             // the glow where the sun rose or set: xy its direction (x, z), z how much, w how bright (daylight.h)
	float moon[4];                 // xyz: towards the moon; w: 1 when the scene's light is not the sun (no sun disc at sun_direction)
	float moon_shape[4];           // x: its phase (0 new .. 0.5 full .. 1), y: the cosine of its angular radius, z: how bright its disc, w: earthshine
	float aurora[4];               // xy: towards the north (x, z); z: how much it shows (0: none, aurora.h); w: its clock (seconds)
	float weather[4];              // x: how hard it rains (0..1, rain.h), y: how wet things are (0..1), z: the rain's clock (seconds), w: the snow on things (0..1)
	float ground_map[4];           // the ground and water textures (scene_ground_bind): cells per side, cell size (m), 1 if they are there, unused
	float sky_occlusion[4];        // x: how strong, y: 1 if there is one (sky_occlusion.h), z: its layers' texels per side (0: none)
	float wind[4];                 // the plants' wind (objects_t.wind_uniform's first four)
	float gusts[4];                // its gusts (wind_uniform's 4, 5), -, how pale a gust's wave makes the grass
	float sky_bounce[4];           // the light the land round sends back: how much, 1 if it is there (sky_occlusion.h)
	float temporal_jitter[4];      // xy: the buildings' shift this frame (clip space, times w: the temporal anti-aliasing's); z: 1 multisampled (alpha to coverage: scene_depth.glsl); w: -
	float view_projection_relative[16]; // camera_t's (depth 0..1): world minus eye_position onto the screen (scene.glsl world_to_clip)
	float inverse_view_projection_relative[16]; // camera_t's (OpenGL style): back to world minus eye_position (scene.glsl clip_to_eye)
} scene_uniforms_t;

void scene_uniforms_fill(scene_uniforms_t *scene, const camera_t *camera, const terrain_t *terrain, const render_settings_t *settings,
	const shadow_map_t *far_shadow, const shadow_map_t *near_shadow, float time_seconds);

// Pushed to uniform slot 0 of both stages (every lit shader reads it there)
void scene_uniforms_push(const scene_uniforms_t *scene);

// The ground's heights (R32F) and the water on it (RG32F: depth) the
// objects' shaders read (ground_water.glsl); NULL: none (stand ins are bound)
void scene_ground_set(SDL_GPUTexture *height_texture, SDL_GPUTexture *water_texture);
// Bound to fragment sampler slots first .. first + 4 (the sky's below)
void scene_ground_bind(SDL_GPURenderPass *pass, uint32_t first);
// The sky's occlusion (sky_occlusion.h; NULL: none), its layers and its
// bounce texture, the third, fourth and fifth of them
void scene_ground_set_sky_occlusion(SDL_GPUTexture *sky_occlusion_texture, SDL_GPUTexture *layers_texture, SDL_GPUTexture *bounce_texture);
// The ground's heights, the sky's occlusion, its layers and its bounce
// texture alone at first and the next three (the impostors' shader)
void scene_ground_bind_sky(SDL_GPURenderPass *pass, uint32_t first);
// The sky's occlusion and its layers as vertex samplers at first and the
// next (the particles' shader)
void scene_ground_bind_sky_vertex(SDL_GPURenderPass *pass, uint32_t first);
// The sky's occlusion, its layers, its bounce texture and the far shadow map
// as vertex samplers at first and the next three (sky_vertex.glsl)
void scene_ground_bind_sky_vertex4(SDL_GPURenderPass *pass, uint32_t first, SDL_GPUTexture *far_shadow);
// The ground's heights, the sky's occlusion and its layers for a compute
// shader at first and the next two (the fog's)
void scene_ground_bind_sky_compute(SDL_GPUComputePass *compute, uint32_t first);
// Whether the ground and the water are there (scene_uniforms_t.ground_map's
// z)
bool scene_ground_ready(void);

#endif
