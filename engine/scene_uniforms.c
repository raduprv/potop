// The scene block. See scene_uniforms.h.

#include "scene_uniforms.h"

#include "gpu.h"
#include "sky_renderer.h"
#include "terrain_renderer.h"

_Static_assert(sizeof(scene_uniforms_t) == 6 * 16 * 4 + 16 * 22, "scene_uniforms_t must match scene_block in scene.glsl");

static float srgb_to_linear(float value)
{
	return SDL_powf(value, 2.2f);
}

void scene_uniforms_fill(scene_uniforms_t *scene, const camera_t *camera, const terrain_t *terrain, const render_settings_t *settings,
	const shadow_map_t *far_shadow, const shadow_map_t *near_shadow, float time_seconds)
{
	vec3_t sun = terrain_renderer_sun_direction(settings);
	bool near_used = near_shadow != NULL && near_shadow->size > 0 && far_shadow->size > 0;

	SDL_zerop(scene);
	gpu_clip_matrix(camera->view_projection.m, scene->view_projection);
	gpu_clip_matrix(camera->view_projection_relative.m, scene->view_projection_relative);
	SDL_memcpy(scene->inverse_view_projection_relative, camera->inverse_view_projection_relative.m, sizeof(scene->inverse_view_projection_relative));
	SDL_memcpy(scene->inverse_view_projection, camera->inverse_view_projection.m, sizeof(scene->inverse_view_projection));
	SDL_memcpy(scene->light_view_projection, far_shadow->light_view_projection.m, sizeof(scene->light_view_projection));
	SDL_memcpy(scene->near_light_view_projection,
		near_used ? near_shadow->light_view_projection.m : far_shadow->light_view_projection.m, sizeof(scene->near_light_view_projection));
	scene->eye_position[0] = camera->eye_position.x;
	scene->eye_position[1] = camera->eye_position.y;
	scene->eye_position[2] = camera->eye_position.z;
	scene->time_seconds = time_seconds;
	scene->sun_direction[0] = sun.x;
	scene->sun_direction[1] = sun.y;
	scene->sun_direction[2] = sun.z;
	scene->ambient_intensity = settings->ambient_intensity;
	for (int i = 0; i < 3; i++)
	{
		scene->sun_color[i] = srgb_to_linear(settings->sun_color[i]) * settings->sun_intensity;
		scene->sky_color[i] = srgb_to_linear(settings->sky_color[i]);
		scene->sky_zenith_color[i] = srgb_to_linear(settings->sky_zenith_color[i]);
	}
	scene->fog_density = settings->fog_density;
	scene->fog_parameters[0] = SDL_max(settings->fog_height_meters, 10.0f);
	scene->fog_parameters[1] = 0.0f;   // the stars (the game sets them: daylight.h)
	scene->fog_parameters[2] = 0.0f;   // the hour
	scene->fog_parameters[3] = 0.0f;   // the clouds' dome (the game sets it)
	SDL_zero(scene->twilight);         // the game sets it
	scene->shadow_strength = far_shadow->size > 0 ? settings->shadow_strength : 0.0f;
	scene->shadow_texel_size = far_shadow->size > 0 ? 1.0f / (float)far_shadow->size : 0.0f;
	scene->near_shadow_texel_size = near_used ? 1.0f / (float)near_shadow->size : 0.0f;
	scene->sky_horizon_drop = sky_horizon_drop(camera, terrain, settings);
	terrain_renderer_cloud_uniforms(settings, time_seconds, scene->cloud_parameters, scene->cloud_motion);
	scene->underwater_color[0] = camera->underwater_color[0];
	scene->underwater_color[1] = camera->underwater_color[1];
	scene->underwater_color[2] = camera->underwater_color[2];
	scene->underwater_color[3] = camera->underwater ? 1.0f : 0.0f;
	scene->underwater_parameters[0] = camera->water_level;
	scene->underwater_parameters[1] = camera->underwater_fog_per_meter;
	scene->underwater_parameters[2] = time_seconds;
}

void scene_uniforms_push(const scene_uniforms_t *scene)
{
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
}

