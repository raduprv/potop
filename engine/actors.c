// Actors: types, placement and drawing (see actors.h).

#include "lights.h"
#include "actors.h"
#include "physics.h"
#include "object_model.h"
#include "clouds.h"
#include "ini_parser.h"
#include "profile.h"
#include "gpu.h"
#include "terrain_renderer.h"
#include "scene_uniforms.h"

#include <SDL3/SDL.h>

// MAX_BONES in data/shaders/actor_skinning.glsl
#define ACTOR_MAX_BONES_DRAWN 64
#define PIVOT_DEGREES_PER_SECOND 90.0f     // pivot_degrees_per_second not given
#define SPOT_TURN_FOUR_LEGS_DEGREES 20.0f  // spot_turn_degrees not given, on four legs
#define SPOT_TURN_WALK_FRACTION 0.4f        // its arc's walk then, if it has none
#define SPOT_TURN_GOING_SPEED 0.1f          // m/s: going at least this fast (forward or back), it turns freely

// ---------------------------------------------------------------------------
// data/actors.ini

typedef struct
{
	actors_t *actors;
	actor_type_t *current;
	char current_section[ACTOR_TYPE_NAME_LENGTH];
} load_state_t;

static void set_type_defaults(actor_type_t *type, const char *name)
{
	SDL_zerop(type);
	SDL_strlcpy(type->name, name, sizeof(type->name));
	SDL_snprintf(type->model_path, sizeof(type->model_path), "actors/%s.pact", name);
	type->model = -1;
	type->scale = 1.0f;
	type->walk_meters_per_second = 1.5f;
	type->run_meters_per_second = 4.0f;
	type->blend_seconds = 0.25f;
	type->ground_align = 1.0f;
	SDL_strlcpy(type->idle_action_name, "idle", sizeof(type->idle_action_name));
	type->idle_action = -1;
	SDL_strlcpy(type->walk_action_name, "walk", sizeof(type->walk_action_name));
	type->walk_action = -1;
	type->turn_degrees_per_second = 180.0f;
	type->spot_turn_degrees = -1.0f;
	type->pivot_degrees_per_second = -1.0f;
	type->jump_up_meters = 0.0f;
	type->jump_down_meters = 0.0f;
	type->jump_actions[0] = type->jump_actions[1] = type->jump_actions[2] = -1;
	SDL_strlcpy(type->turn_left_action_name, "turn_left", sizeof(type->turn_left_action_name));
	type->turn_left_action = -1;
	SDL_strlcpy(type->turn_right_action_name, "turn_right", sizeof(type->turn_right_action_name));
	type->turn_right_action = -1;
	SDL_strlcpy(type->walk_left_action_name, "walk_left", sizeof(type->walk_left_action_name));
	type->walk_left_action = -1;
	SDL_strlcpy(type->walk_right_action_name, "walk_right", sizeof(type->walk_right_action_name));
	type->walk_right_action = -1;
	type->walk_turn_degrees = 15.0f;
	for (int g = 0; g < ACTOR_GAIT_COUNT; g++)
	{
		type->gait_actions[g] = -1;
	}
	type->walk_start_meters = 0.366f;
	type->walk_start_180_meters = 0.4f;
	type->walk_start_180_delay = 0.33f;
	type->walk_stop_meters = 0.327f;
	type->walk_stop_left_phase = 0.0f;
	type->walk_stop_right_phase = 0.5f;
	type->sidestep_meters_per_second = 0.833f;
	type->step_back_meters_per_second = 0.625f;
	type->maximum_slope_degrees = 40.0f;
	type->vision_meters = 20.0f;
	type->sight_meters = 40.0f;
	type->sight_degrees = 220.0f;
	type->can_swim = true;
	type->path_groups = 1;
	type->swim_meters_per_second = 1.0f;
	type->wade_depth_meters = 0.3f;
	type->swim_depth_meters = 0.8f;
	type->step_meters = 0.4f;
	type->fly_meters_per_second = 12.0f;
	type->fly_height_minimum_meters = 15.0f;
	type->fly_height_maximum_meters = 60.0f;
	type->fly_ceiling_meters = 150.0f;
	type->circle_radius_meters = 12.0f;
	type->climb_meters_per_second = 4.0f;
	SDL_strlcpy(type->glide_action_name, "glide", sizeof(type->glide_action_name));
	type->glide_action = -1;
	type->ik = true;
}

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	load_state_t *state = user_data;
	actors_t *actors = state->actors;
	actor_type_t *type;

	if (SDL_strcmp(section, state->current_section) != 0)
	{
		SDL_strlcpy(state->current_section, section, sizeof(state->current_section));
		state->current = NULL;
		if (actors->type_count == ACTOR_TYPES_MAX)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "actors.ini: more than %d types, [%s] ignored", ACTOR_TYPES_MAX, section);
			return true;
		}
		state->current = &actors->types[actors->type_count++];
		set_type_defaults(state->current, section);
	}
	type = state->current;
	if (type == NULL)
	{
		return true;
	}
	if (SDL_strcmp(key, "model") == 0)
	{
		SDL_snprintf(type->model_path, sizeof(type->model_path), "actors/%s.pact", value);
		return true;
	}
	if (SDL_strcmp(key, "scale") == 0)
	{
		return ini_value_to_float(value, &type->scale);
	}
	if (SDL_strcmp(key, "front_degrees") == 0)
	{
		return ini_value_to_float(value, &type->front_degrees);
	}
	if (SDL_strcmp(key, "walk_meters_per_second") == 0)
	{
		return ini_value_to_float(value, &type->walk_meters_per_second);
	}
	if (SDL_strcmp(key, "run_meters_per_second") == 0)
	{
		return ini_value_to_float(value, &type->run_meters_per_second);
	}
	if (SDL_strcmp(key, "blend_seconds") == 0)
	{
		return ini_value_to_float(value, &type->blend_seconds);
	}
	if (SDL_strcmp(key, "ground_align") == 0)
	{
		return ini_value_to_float(value, &type->ground_align);
	}
	if (SDL_strcmp(key, "idle_action") == 0)
	{
		SDL_strlcpy(type->idle_action_name, value, sizeof(type->idle_action_name));
		return true;
	}
	if (SDL_strcmp(key, "walk_action") == 0)
	{
		SDL_strlcpy(type->walk_action_name, value, sizeof(type->walk_action_name));
		return true;
	}
	if (SDL_strcmp(key, "walk_cycle_meters") == 0)
	{
		return ini_value_to_float(value, &type->walk_cycle_meters);
	}
	if (SDL_strcmp(key, "turn_degrees_per_second") == 0)
	{
		return ini_value_to_float(value, &type->turn_degrees_per_second);
	}
	if (SDL_strcmp(key, "pivot_degrees_per_second") == 0)
	{
		return ini_value_to_float(value, &type->pivot_degrees_per_second);
	}
	if (SDL_strcmp(key, "jump_up_meters") == 0)
	{
		return ini_value_to_float(value, &type->jump_up_meters);
	}
	if (SDL_strcmp(key, "jump_down_meters") == 0)
	{
		return ini_value_to_float(value, &type->jump_down_meters);
	}
	if (SDL_strcmp(key, "walk_left_action") == 0)
	{
		SDL_strlcpy(type->walk_left_action_name, value, sizeof(type->walk_left_action_name));
		return true;
	}
	if (SDL_strcmp(key, "walk_right_action") == 0)
	{
		SDL_strlcpy(type->walk_right_action_name, value, sizeof(type->walk_right_action_name));
		return true;
	}
	if (SDL_strcmp(key, "walk_turn_degrees") == 0)
	{
		return ini_value_to_float(value, &type->walk_turn_degrees);
	}
	if (SDL_strcmp(key, "turn_left_action") == 0)
	{
		SDL_strlcpy(type->turn_left_action_name, value, sizeof(type->turn_left_action_name));
		return true;
	}
	if (SDL_strcmp(key, "turn_right_action") == 0)
	{
		SDL_strlcpy(type->turn_right_action_name, value, sizeof(type->turn_right_action_name));
		return true;
	}
	if (SDL_strcmp(key, "walk_start_meters") == 0)
	{
		return ini_value_to_float(value, &type->walk_start_meters);
	}
	if (SDL_strcmp(key, "walk_start_180_meters") == 0)
	{
		return ini_value_to_float(value, &type->walk_start_180_meters);
	}
	if (SDL_strcmp(key, "walk_start_180_delay") == 0)
	{
		return ini_value_to_float(value, &type->walk_start_180_delay);
	}
	if (SDL_strcmp(key, "walk_stop_meters") == 0)
	{
		return ini_value_to_float(value, &type->walk_stop_meters);
	}
	if (SDL_strcmp(key, "walk_stop_left_phase") == 0)
	{
		return ini_value_to_float(value, &type->walk_stop_left_phase);
	}
	if (SDL_strcmp(key, "walk_stop_right_phase") == 0)
	{
		return ini_value_to_float(value, &type->walk_stop_right_phase);
	}
	if (SDL_strcmp(key, "sidestep_meters_per_second") == 0)
	{
		return ini_value_to_float(value, &type->sidestep_meters_per_second);
	}
	if (SDL_strcmp(key, "step_back_meters_per_second") == 0)
	{
		return ini_value_to_float(value, &type->step_back_meters_per_second);
	}
	if (SDL_strcmp(key, "turn_walk_fraction") == 0)
	{
		return ini_value_to_float(value, &type->turn_walk_fraction);
	}
	if (SDL_strcmp(key, "spot_turn_degrees") == 0)
	{
		return ini_value_to_float(value, &type->spot_turn_degrees);
	}
	if (SDL_strcmp(key, "maximum_slope_degrees") == 0)
	{
		return ini_value_to_float(value, &type->maximum_slope_degrees);
	}
	if (SDL_strcmp(key, "vision_meters") == 0)
	{
		return ini_value_to_float(value, &type->vision_meters);
	}
	if (SDL_strcmp(key, "sight_meters") == 0)
	{
		return ini_value_to_float(value, &type->sight_meters);
	}
	if (SDL_strcmp(key, "sight_degrees") == 0)
	{
		return ini_value_to_float(value, &type->sight_degrees);
	}
	if (SDL_strcmp(key, "can_swim") == 0)
	{
		return ini_value_to_bool(value, &type->can_swim);
	}
	if (SDL_strcmp(key, "opens_doors") == 0)
	{
		return ini_value_to_bool(value, &type->opens_doors);
	}
	if (SDL_strcmp(key, "path_groups") == 0)
	{
		int groups = 0;

		if (SDL_sscanf(value, " %d", &groups) != 1)
		{
			return false;
		}
		type->path_groups = (uint8_t)SDL_clamp(groups, 0, 255);
		return true;
	}
	if (SDL_strcmp(key, "swim_meters_per_second") == 0)
	{
		return ini_value_to_float(value, &type->swim_meters_per_second);
	}
	if (SDL_strcmp(key, "wade_depth_meters") == 0)
	{
		return ini_value_to_float(value, &type->wade_depth_meters);
	}
	if (SDL_strcmp(key, "swim_depth_meters") == 0)
	{
		return ini_value_to_float(value, &type->swim_depth_meters);
	}
	if (SDL_strcmp(key, "height_meters") == 0)
	{
		return ini_value_to_float(value, &type->height_meters);
	}
	if (SDL_strcmp(key, "width_meters") == 0)
	{
		return ini_value_to_float(value, &type->width_meters);
	}
	if (SDL_strcmp(key, "step_meters") == 0)
	{
		return ini_value_to_float(value, &type->step_meters);
	}
	if (SDL_strcmp(key, "flies") == 0)
	{
		return ini_value_to_bool(value, &type->flies);
	}
	if (SDL_strcmp(key, "fly_meters_per_second") == 0)
	{
		return ini_value_to_float(value, &type->fly_meters_per_second);
	}
	if (SDL_strcmp(key, "fly_height_minimum_meters") == 0)
	{
		return ini_value_to_float(value, &type->fly_height_minimum_meters);
	}
	if (SDL_strcmp(key, "fly_height_maximum_meters") == 0)
	{
		return ini_value_to_float(value, &type->fly_height_maximum_meters);
	}
	if (SDL_strcmp(key, "fly_ceiling_meters") == 0)
	{
		return ini_value_to_float(value, &type->fly_ceiling_meters);
	}
	if (SDL_strcmp(key, "circle_radius_meters") == 0)
	{
		return ini_value_to_float(value, &type->circle_radius_meters);
	}
	if (SDL_strcmp(key, "climb_meters_per_second") == 0)
	{
		return ini_value_to_float(value, &type->climb_meters_per_second);
	}
	if (SDL_strcmp(key, "ik") == 0)
	{
		return ini_value_to_bool(value, &type->ik);
	}
	if (SDL_strcmp(key, "ik_feet") == 0)
	{
		SDL_strlcpy(type->ik_feet, value, sizeof(type->ik_feet));
		return true;
	}
	if (SDL_strcmp(key, "glide_action") == 0)
	{
		SDL_strlcpy(type->glide_action_name, value, sizeof(type->glide_action_name));
		return true;
	}
	return false;
}

// Is the name one of a comma separated list (spaces around ignored)?
static bool name_in_list(const char *list, const char *name)
{
	size_t length = SDL_strlen(name);

	while (*list != '\0')
	{
		const char *end;

		while (*list == ' ' || *list == ',')
		{
			list++;
		}
		end = list;
		while (*end != '\0' && *end != ',')
		{
			end++;
		}
		{
			const char *last = end;

			while (last > list && last[-1] == ' ')
			{
				last--;
			}
			if ((size_t)(last - list) == length && SDL_strncmp(list, name, length) == 0)
			{
				return true;
			}
		}
		list = end;
	}
	return false;
}

// The legs whose feet stand on the ground: each foot bone (named in
// ik_feet, or starting with foot), its parent and grandparent bending
static void find_legs(actor_type_t *type, const actor_model_t *model)
{
	actor_file_key_t bind_local[ACTOR_FILE_MAX_BONES];
	actor_file_key_t bind_global[ACTOR_FILE_MAX_BONES];

	type->leg_count = 0;
	if (!type->ik || type->flies)
	{
		return;
	}
	for (int b = 0; b < model->bone_count; b++)
	{
		SDL_memcpy(bind_local[b].translation, model->bones[b].translation, sizeof(float) * 3);
		SDL_memcpy(bind_local[b].rotation, model->bones[b].rotation, sizeof(float) * 4);
	}
	actor_model_compose_global(model, bind_local, bind_global);
	for (int b = 0; b < model->bone_count && type->leg_count < ACTOR_MAX_LEGS; b++)
	{
		const char *name = model->bones[b].name;
		bool wanted = type->ik_feet[0] != '\0' ? name_in_list(type->ik_feet, name) : SDL_strncasecmp(name, "foot", 4) == 0;
		int lower;
		int upper;
		actor_leg_t *leg;

		if (!wanted)
		{
			continue;
		}
		lower = model->bones[b].parent;
		upper = lower >= 0 ? model->bones[lower].parent : -1;
		if (upper < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "actors.ini: [%s] foot '%s' has no two bones above it to bend: left out",
				type->name, name);
			continue;
		}
		leg = &type->legs[type->leg_count++];
		leg->upper = upper;
		leg->lower = lower;
		leg->foot = b;
		leg->bind_foot_height = bind_global[b].translation[1];
	}
	if (type->leg_count > 0)
	{
		SDL_Log("Actor type %s: %d feet on the ground", type->name, type->leg_count);
	}
}

// Loads each type's model; types sharing a file share the model
static void load_models(actors_t *actors)
{
	for (int t = 0; t < actors->type_count; t++)
	{
		actor_type_t *type = &actors->types[t];

		type->scale = SDL_max(type->scale, 0.01f);
		for (int m = 0; m < actors->model_count; m++)
		{
			if (SDL_strcmp(actors->models[m].path, type->model_path) == 0)
			{
				type->model = m;
				break;
			}
		}
		if (type->model < 0 && actor_model_load(&actors->models[actors->model_count], type->model_path))
		{
			type->model = actors->model_count++;
		}
		if (type->model >= 0)
		{
			type->idle_action = actor_model_find_action(&actors->models[type->model], type->idle_action_name);
			if (type->idle_action < 0 && actors->models[type->model].action_count > 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "actors.ini: [%s] has no action '%s', standing still in the first one",
					type->name, type->idle_action_name);
				type->idle_action = 0;
			}
			if (type->height_meters <= 0.0f)
			{
				const actor_model_t *model = &actors->models[type->model];

				type->height_meters = (model->bounds_maximum.y - SDL_min(model->bounds_minimum.y, 0.0f)) * type->scale;
			}
			if (type->width_meters <= 0.0f)
			{
				// Side to side (its front is +Z)
				const actor_model_t *model = &actors->models[type->model];

				type->width_meters = (model->bounds_maximum.x - model->bounds_minimum.x) * type->scale;
			}
			type->walk_action = actor_model_find_action(&actors->models[type->model], type->walk_action_name);
			if (type->walk_action < 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "actors.ini: [%s] has no action '%s': it will glide", type->name,
					type->walk_action_name);
			}
			find_legs(type, &actors->models[type->model]);
			// Optional: without them, the walk while it turns on the spot
			type->turn_left_action = actor_model_find_action(&actors->models[type->model], type->turn_left_action_name);
			type->turn_right_action = actor_model_find_action(&actors->models[type->model], type->turn_right_action_name);
			type->walk_left_action = actor_model_find_action(&actors->models[type->model], type->walk_left_action_name);
			type->walk_right_action = actor_model_find_action(&actors->models[type->model], type->walk_right_action_name);
			// Optional: its steps between standing and walking (people)
			{
				static const char *const gait_names[ACTOR_GAIT_COUNT] = { "walk_start", "walk_start_180", "walk_stop_l", "walk_stop_r", "step_left", "step_right",
					"step_back", "shuffle" };

				for (int g = 0; g < ACTOR_GAIT_COUNT; g++)
				{
					type->gait_actions[g] = actor_model_find_action(&actors->models[type->model], gait_names[g]);
				}
			}
			// Optional: its jump's clips
			{
				static const char *const jump_names[3] = { "jump_start", "jump_air", "jump_land" };

				for (int j = 0; j < 3; j++)
				{
					type->jump_actions[j] = actor_model_find_action(&actors->models[type->model], jump_names[j]);
				}
			}
			// Optional: without it, a flyer flaps on the way down too
			type->glide_action = type->flies ? actor_model_find_action(&actors->models[type->model], type->glide_action_name) : -1;
		}
		type->walk_meters_per_second = SDL_max(type->walk_meters_per_second, 0.05f);
		type->swim_meters_per_second = SDL_max(type->swim_meters_per_second, 0.05f);
		type->turn_degrees_per_second = SDL_max(type->turn_degrees_per_second, 1.0f);
		type->turn_walk_fraction = SDL_clamp(type->turn_walk_fraction, 0.0f, 1.0f);
		if (type->pivot_degrees_per_second < 0.0f)
		{
			type->pivot_degrees_per_second = PIVOT_DEGREES_PER_SECOND;
		}
		type->pivot_degrees_per_second = SDL_max(type->pivot_degrees_per_second, type->turn_degrees_per_second);
		if (type->spot_turn_degrees < 0.0f)
		{
			type->spot_turn_degrees = type->ground_align > 0.0f ? SPOT_TURN_FOUR_LEGS_DEGREES : 0.0f;
		}
		// (limited on the spot, it must be able to turn going: an arc of
		// its own if none is given)
		if (type->spot_turn_degrees > 0.0f && type->turn_walk_fraction <= 0.0f)
		{
			type->turn_walk_fraction = SPOT_TURN_WALK_FRACTION;
		}
		type->maximum_slope_degrees = SDL_clamp(type->maximum_slope_degrees, 1.0f, 89.0f);
		type->swim_depth_meters = SDL_max(type->swim_depth_meters, 0.05f);
		type->wade_depth_meters = SDL_clamp(type->wade_depth_meters, 0.0f, type->swim_depth_meters);
		type->step_meters = SDL_max(type->step_meters, 0.0f);
		type->blend_seconds = SDL_max(type->blend_seconds, 0.0f);
		type->walk_turn_degrees = SDL_clamp(type->walk_turn_degrees, 1.0f, 90.0f);
		type->ground_align = SDL_clamp(type->ground_align, 0.0f, 1.0f);
		type->fly_meters_per_second = SDL_max(type->fly_meters_per_second, 0.5f);
		type->fly_height_minimum_meters = SDL_max(type->fly_height_minimum_meters, 1.0f);
		type->fly_height_maximum_meters = SDL_max(type->fly_height_maximum_meters, type->fly_height_minimum_meters);
		type->fly_ceiling_meters = SDL_max(type->fly_ceiling_meters, type->fly_height_maximum_meters);
		type->circle_radius_meters = SDL_max(type->circle_radius_meters, 2.0f);
		type->climb_meters_per_second = SDL_max(type->climb_meters_per_second, 0.5f);
	}
}

// ---------------------------------------------------------------------------

static bool create_pipelines(actors_t *actors)
{
	// The vertices, and (for the camera's pipelines, not the shadows') the
	// tangents
	static const SDL_GPUVertexBufferDescription vertex_buffers[2] =
	{
		{ 0, sizeof(actor_file_vertex_t), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
		{ 1, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	};
	// Bone indices as integers, weights as 0..1
	static const SDL_GPUVertexAttribute attributes[6] =
	{
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(actor_file_vertex_t, position) },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(actor_file_vertex_t, normal) },
		{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(actor_file_vertex_t, uv) },
		{ 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, offsetof(actor_file_vertex_t, bones) },
		{ 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(actor_file_vertex_t, weights) },
		{ 5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	};
	SDL_GPUShader *vertex = gpu_load_shader("actor.vert", 0, 0, 2);
	SDL_GPUShader *fragment = gpu_load_shader("object.frag", OBJECT_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);
	SDL_GPUShader *shadow_vertex = gpu_load_shader("actor_shadow.vert", 0, 0, 2);
	SDL_GPUShader *shadow_fragment = gpu_load_shader("object_shadow.frag", 1, 0, 2);
	gpu_pipeline_info_t info;
	bool ok = vertex != NULL && fragment != NULL && shadow_vertex != NULL && shadow_fragment != NULL;

	if (ok)
	{
		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = vertex_buffers;
		info.vertex_buffer_count = 2;
		info.vertex_attributes = attributes;
		info.vertex_attribute_count = 6;
		info.cull = SDL_GPU_CULLMODE_BACK;
		info.depth_test = true;
		info.depth_write = true;
		info.multisampled = true;
		info.alpha_to_coverage = true;
		actors->pipeline = gpu_make_pipeline(&info, "actors");
		info.cull = SDL_GPU_CULLMODE_NONE;
		actors->two_sided_pipeline = gpu_make_pipeline(&info, "actors (two sided)");
		info.vertex_shader = shadow_vertex;
		info.fragment_shader = shadow_fragment;
		info.vertex_buffer_count = 1;
		info.vertex_attribute_count = 5;
		info.depth_only = true;
		info.depth_bias_constant = 4.0f;
		info.depth_bias_slope = 2.0f;
		actors->shadow_pipeline = gpu_make_pipeline(&info, "actor shadows");
		ok = actors->pipeline != NULL && actors->two_sided_pipeline != NULL && actors->shadow_pipeline != NULL;
	}
	SDL_ReleaseGPUShader(gpu.device, vertex);
	SDL_ReleaseGPUShader(gpu.device, fragment);
	SDL_ReleaseGPUShader(gpu.device, shadow_vertex);
	SDL_ReleaseGPUShader(gpu.device, shadow_fragment);
	return ok;
}

bool actors_create(actors_t *actors, const settings_t *settings)
{
	load_state_t state;

	(void)settings;
	SDL_zerop(actors);
	actors->next_id = 1;
	if (!create_pipelines(actors))
	{
		return false;
	}

	SDL_zero(state);
	state.actors = actors;
	if (!ini_parse_data_file("actors.ini", load_key, &state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "data/actors.ini not found: no actors");
	}
	load_models(actors);
	return true;
}

void actors_destroy(actors_t *actors)
{
	for (int m = 0; m < actors->model_count; m++)
	{
		actor_model_destroy(&actors->models[m]);
	}
	if (actors->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, actors->pipeline);
	}
	if (actors->two_sided_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, actors->two_sided_pipeline);
	}
	if (actors->shadow_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, actors->shadow_pipeline);
	}
	SDL_free(actors->actors);
	SDL_zerop(actors);
}

int actors_find_type(const actors_t *actors, const char *name)
{
	for (int t = 0; t < actors->type_count; t++)
	{
		if (SDL_strcmp(actors->types[t].name, name) == 0)
		{
			return t;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Standing on the ground

#define ACTOR_LEAN_MAXIMUM_RADIANS 0.6f // about 35 degrees
// Feet on the ground
#define IK_NORMAL_METERS 0.3f          // the ground's slope under a foot, over this much
#define IK_TILT_MAXIMUM_RADIANS 0.6f   // a foot turns with the slope at most this much
#define IK_LIFTED_METERS 0.15f         // a foot this far up (a step) no longer turns with the ground
#define PLACE_EASE_SECONDS 0.08f       // the body and feet follow the ground with this time constant
#define PLACE_REACH_EXTRA_METERS 0.3f  // what it stands on: within its step and this much of the body
#define PLACE_SNAP_METERS 1.5f         // a bigger change is taken at once (put somewhere else)

// left * right, quaternions (x, y, z, w)
static void quaternion_multiply(const float left[4], const float right[4], float out[4])
{
	float result[4];

	result[0] = left[3] * right[0] + left[0] * right[3] + left[1] * right[2] - left[2] * right[1];
	result[1] = left[3] * right[1] - left[0] * right[2] + left[1] * right[3] + left[2] * right[0];
	result[2] = left[3] * right[2] + left[0] * right[1] - left[1] * right[0] + left[2] * right[3];
	result[3] = left[3] * right[3] - left[0] * right[0] - left[1] * right[1] - left[2] * right[2];
	SDL_memcpy(out, result, sizeof(result));
}

static void quaternion_conjugate(const float rotation[4], float out[4])
{
	out[0] = -rotation[0];
	out[1] = -rotation[1];
	out[2] = -rotation[2];
	out[3] = rotation[3];
}

static void quaternion_rotate(const float rotation[4], const float vector[3], float out[3])
{
	float twice_cross[3] = {
		2.0f * (rotation[1] * vector[2] - rotation[2] * vector[1]),
		2.0f * (rotation[2] * vector[0] - rotation[0] * vector[2]),
		2.0f * (rotation[0] * vector[1] - rotation[1] * vector[0]) };
	float result[3];

	result[0] = vector[0] + rotation[3] * twice_cross[0] + (rotation[1] * twice_cross[2] - rotation[2] * twice_cross[1]);
	result[1] = vector[1] + rotation[3] * twice_cross[1] + (rotation[2] * twice_cross[0] - rotation[0] * twice_cross[2]);
	result[2] = vector[2] + rotation[3] * twice_cross[2] + (rotation[0] * twice_cross[1] - rotation[1] * twice_cross[0]);
	SDL_memcpy(out, result, sizeof(result));
}

static float vector_length(const float vector[3])
{
	return SDL_sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
}

// The shortest turn that takes the direction from onto the direction to
static void quaternion_between(const float from[3], const float to[3], float out[4])
{
	float from_length = vector_length(from);
	float to_length = vector_length(to);
	float a[3];
	float b[3];
	float dot;
	float length;

	if (from_length < 1e-6f || to_length < 1e-6f)
	{
		out[0] = out[1] = out[2] = 0.0f;
		out[3] = 1.0f;
		return;
	}
	for (int k = 0; k < 3; k++)
	{
		a[k] = from[k] / from_length;
		b[k] = to[k] / to_length;
	}
	dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	if (dot < -0.9999f)
	{
		// Opposite: half a turn around any axis across them
		float axis[3] = { 0.0f, -a[2], a[1] };

		if (vector_length(axis) < 1e-3f)
		{
			axis[0] = a[2];
			axis[1] = 0.0f;
			axis[2] = -a[0];
		}
		length = vector_length(axis);
		out[0] = axis[0] / length;
		out[1] = axis[1] / length;
		out[2] = axis[2] / length;
		out[3] = 0.0f;
		return;
	}
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
	out[3] = 1.0f + dot;
	length = SDL_sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
	for (int k = 0; k < 4; k++)
	{
		out[k] /= length;
	}
}

// That much of the turn (0: none, 1: all of it)
static void quaternion_scale(float rotation[4], float amount)
{
	float length;

	if (rotation[3] < 0.0f)
	{
		for (int k = 0; k < 4; k++)
		{
			rotation[k] = -rotation[k];
		}
	}
	for (int k = 0; k < 3; k++)
	{
		rotation[k] *= amount;
	}
	rotation[3] = 1.0f - amount + amount * rotation[3];
	length = SDL_sqrtf(rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] + rotation[3] * rotation[3]);
	for (int k = 0; k < 4; k++)
	{
		rotation[k] /= length;
	}
}

// The model's turn: in its own axes the nose up by the pitch (around -X,
// which lifts +Z), the +X side up by the roll (around +Z); then the yaw
// (turn: the yaw less the model's front)
static void compose_rotation(float turn, float pitch, float roll, float out[4])
{
	float yaw_rotation[4] = { 0.0f, SDL_sinf(0.5f * turn), 0.0f, SDL_cosf(0.5f * turn) };
	float lean_pitch[4] = { -SDL_sinf(0.5f * pitch), 0.0f, 0.0f, SDL_cosf(0.5f * pitch) };
	float lean_roll[4] = { 0.0f, 0.0f, SDL_sinf(0.5f * roll), SDL_cosf(0.5f * roll) };
	float lean[4];

	quaternion_multiply(lean_pitch, lean_roll, lean);
	quaternion_multiply(yaw_rotation, lean, out);
}

// Height and lean from the ground under the feet. Actors with legs found
// (see find_legs), once their feet are known: the ground under each foot,
// a plane fitted through those (the lean, as much as the type's
// ground_align says), and the body as low as its lowest foot needs (the
// legs bend for the others, see place_feet), though not more than a
// quarter of its height under the plane (a foot over a hole dangles).
// Others, and every actor's first moment: the ground sampled at the front,
// back, left and right of the feet's extent (in the model's own directions,
// after its yaw), the lean by the slopes between them, standing on the
// plane they make; upright ones (ground_align 0) on the lowest sample, so
// no foot floats. Every point keeps to the surface it was on (a rock, a
// deck): its last height is what it looks near. The body's height and lean
// and the feet's ground follow what is found within a few hundredths of a
// second, not at once: a foot stepping onto a rock does not jerk the body.
static navigation_agent_t agent_of(const actor_type_t *type);

static void place_on_ground(const actors_t *actors, actor_t *actor, const terrain_t *terrain, const water_t *water,
	const navigation_t *navigation, float seconds)
{
	const actor_type_t *type = &actors->types[actor->type];
	navigation_agent_t agent = agent_of(type);
	const actor_model_t *model = &actors->models[type->model];
	float turn = actor->yaw_radians - degrees_to_radians(type->front_degrees);
	float cosine = SDL_cosf(turn);
	float sine = SDL_sinf(turn);
	// Model space (x, z) to the map: turned around Y by turn
	float forward_x = sine * actor->scale;
	float forward_z = cosine * actor->scale;
	float right_x = cosine * actor->scale;
	float right_z = -sine * actor->scale;
	float back = model->feet_minimum_z;
	float front = model->feet_maximum_z;
	float middle_z = 0.5f * (back + front);
	// Narrow feet still give the sideways slope over a useful distance
	float half_width = SDL_max(0.5f * (model->feet_maximum_x - model->feet_minimum_x), 0.15f);
	float middle_x = 0.5f * (model->feet_minimum_x + model->feet_maximum_x);
	float length = SDL_max(front - back, 0.15f);
	float samples[4];
	float sample_offsets[4][2] = {
		{ middle_x, front }, { middle_x, back }, { middle_x + half_width, middle_z }, { middle_x - half_width, middle_z } };
	float lowest = 1e30f;
	float slope_forward;
	float slope_right;
	float plane_at_origin;
	float pitch;
	float roll;
	float align = type->ground_align;
	float depth = 0.0f;
	float velocity_x;
	float velocity_z;
	bool by_feet;
	float previous_y = actor->y;
	float center_height;
	// How much of the way to what it finds it goes this frame (1: all, the
	// first time and when paused)
	float easing = seconds > 0.0f && actor->placed ? 1.0f - SDL_expf(-seconds / PLACE_EASE_SECONDS) : 1.0f;

	float ice_top = -1e30f;

	if (water != NULL && !water_sample(water, actor->x, actor->z, &depth, &velocity_x, &velocity_z))
	{
		depth = 0.0f;
	}
	// On the ice: it stands on its surface, no water to wade or swim in
	if (water != NULL && water->iced && water_ice_surface(water, terrain, actor->x, actor->z, &ice_top))
	{
		depth = 0.0f;
	}
	else
	{
		ice_top = -1e30f;
	}
	// The deeper the water, the less the ground matters: afloat, it is level
	align *= 1.0f - SDL_clamp(depth / type->swim_depth_meters, 0.0f, 1.0f);
	actor->swimming = depth > type->swim_depth_meters;
	// Feet on the ground let go as the water lifts it
	actor->ik_weight = type->leg_count > 0 && navigation != NULL ? 1.0f - SDL_clamp(depth / type->swim_depth_meters, 0.0f, 1.0f)
		: 0.0f;
	// (its feet not on the ground under the ice)
	if (ice_top > -1e29f)
	{
		actor->ik_weight = 0.0f;
		align = 0.0f;
	}

	if (model->feet_maximum_z < model->feet_minimum_z || type->ground_align <= 0.0f)
	{
		// No feet found, or upright whatever it stands on (people): stand
		// on the point under the origin (not up on an anvil's stump its
		// front foot's reach is over)
		back = front = middle_z = middle_x = 0.0f;
		sample_offsets[0][1] = sample_offsets[1][1] = 0.0f;
		sample_offsets[2][1] = sample_offsets[3][1] = 0.0f;
	}
	// What it stands on under its middle first (within a little more than
	// its step of the body: a body tilted at a terrace's edge, its back
	// feet below, still finds the terrace under it), then each sample
	// against that: one that once fell to the ground beside a tread no
	// longer drags the body down into the terrace it walks on
	{
		navigation_agent_t placing = agent;

		placing.step_meters += PLACE_REACH_EXTRA_METERS;
		// (the navigation's alone: the physics' shapes only refine the
		// samples round it, within a step of it, so a wall of logs, each a
		// ledge, is not climbed log by log onto the roof)
		center_height = navigation != NULL && actor->placed ? navigation_surface_height(navigation, terrain, &placing, actor->x, actor->z,
			actor->center_ground) : actor->y;
		actor->center_ground = center_height;
		agent = placing;
	}
	for (int k = 0; k < 4; k++)
	{
		float x = actor->x + right_x * sample_offsets[k][0] + forward_x * sample_offsets[k][1];
		float z = actor->z + right_z * sample_offsets[k][0] + forward_z * sample_offsets[k][1];
		// (what it can be on there is what is within its step of the body:
		// a sample's own last height let one that once fell to the ground
		// beside a stair's tread stay there, the body sunk in the terrace
		// it walked on)
		float near_height = actor->placed ? center_height : (actor->ground_samples_known ? actor->ground_samples[k] : actor->y);

		samples[k] = navigation != NULL ? navigation_surface_height(navigation, terrain, &agent, x, z, near_height)
			: terrain_sample_height_meters(terrain, x, z);
		// What the physics has there too, by the objects' real shapes: a
		// door's step, an inn's terrace, a stair no wider than a navigation
		// cell's quarter; anything up to a step over the body (people, and
		// the animals: on a terrace, up its stair)
		if (actors->physics != NULL && actor->placed)
		{
			float top;

			if (physics_walker_ground(actors->physics, x, z, near_height, type->step_meters, &top) && top > samples[k])
			{
				samples[k] = top;
			}
		}
		actor->ground_samples[k] = samples[k];
		lowest = SDL_min(lowest, samples[k]);
	}
	actor->ground_samples_known = navigation != NULL;
	// Rise per meter of the map along the model's front and right
	slope_forward = (samples[0] - samples[1]) / (length * actor->scale);
	slope_right = (samples[2] - samples[3]) / (2.0f * half_width * actor->scale);
	plane_at_origin = 0.25f * (samples[0] + samples[1] + samples[2] + samples[3])
		- slope_forward * middle_z * actor->scale - slope_right * middle_x * actor->scale;

	// The ground under each foot, where the animation put it last frame,
	// each foot looking near its own last height; only an object's top (a
	// deck, a boulder) bends the leg: on the terrain the animation stays as
	// it is (the body leans with the slope already)
	by_feet = actor->ik_weight > 0.0f && actor->feet_known;
	if (by_feet)
	{
		for (int i = 0; i < type->leg_count; i++)
		{
			float foot_x = actor->foot_x[i];
			float foot_z = actor->foot_z[i];
			bool on_object = false;
			float ground = navigation_surface_height_on(navigation, terrain, &agent, foot_x, foot_z, actor->placed ? center_height : actor->foot_ground[i],
				&on_object);
			float ground_x = navigation_surface_height(navigation, terrain, &agent, foot_x + IK_NORMAL_METERS, foot_z, ground);
			float ground_z = navigation_surface_height(navigation, terrain, &agent, foot_x, foot_z + IK_NORMAL_METERS, ground);
			float normal[3];

			// (and the physics' real shapes under the foot: a tread, a
			// terrace's flags; then level)
			if (actors->physics != NULL)
			{
				float top;

				if (physics_walker_ground(actors->physics, foot_x, foot_z, center_height, type->step_meters, &top) && top > ground + 0.01f)
				{
					ground = top;
					ground_x = top;
					ground_z = top;
					on_object = true;
				}
			}
			normal[0] = -(ground_x - ground) / IK_NORMAL_METERS;
			normal[1] = 1.0f;
			normal[2] = -(ground_z - ground) / IK_NORMAL_METERS;
			float normal_length = vector_length(normal);
			float wanted_weight = on_object ? 1.0f : 0.0f;

			actor->foot_ground[i] = ground;
			// What the foot stands on follows the ground found, eased, and so
			// does how much the leg bends for it
			actor->foot_height[i] = easing < 1.0f && SDL_fabsf(ground - actor->foot_height[i]) < PLACE_SNAP_METERS
				? actor->foot_height[i] + (ground - actor->foot_height[i]) * easing : ground;
			actor->foot_weight[i] = easing < 1.0f ? actor->foot_weight[i] + (wanted_weight - actor->foot_weight[i]) * easing
				: wanted_weight;
			for (int k = 0; k < 3; k++)
			{
				actor->foot_normal[i][k] = normal[k] / normal_length;
			}
		}
	}

	pitch = SDL_clamp(SDL_atanf(slope_forward), -ACTOR_LEAN_MAXIMUM_RADIANS, ACTOR_LEAN_MAXIMUM_RADIANS) * align;
	roll = SDL_clamp(SDL_atanf(slope_right), -ACTOR_LEAN_MAXIMUM_RADIANS, ACTOR_LEAN_MAXIMUM_RADIANS) * align;
	if (easing < 1.0f)
	{
		pitch = actor->lean_pitch + (pitch - actor->lean_pitch) * easing;
		roll = actor->lean_roll + (roll - actor->lean_roll) * easing;
	}
	actor->lean_pitch = pitch;
	actor->lean_roll = roll;
	compose_rotation(turn, pitch, roll, actor->rotation);

	actor->y = lerp_float(lowest, plane_at_origin, align);
	actor->y = SDL_max(actor->y, ice_top);
	if (depth > 0.0f)
	{
		// Afloat once the water is deeper than its swimming depth: the body
		// that deep under the surface (continuous with standing at that depth)
		float surface = terrain_sample_height_meters(terrain, actor->x, actor->z) + depth;

		actor->y = SDL_max(actor->y, surface - type->swim_depth_meters);
	}
	if (easing < 1.0f && SDL_fabsf(actor->y - previous_y) < PLACE_SNAP_METERS)
	{
		actor->y = previous_y + (actor->y - previous_y) * easing;
	}
	actor->placed = navigation != NULL;
}

// ---------------------------------------------------------------------------
// Animation

static const actor_model_t *model_of(const actors_t *actors, const actor_t *actor)
{
	return &actors->models[actors->types[actor->type].model];
}

// How long the action plays, in real seconds
static float action_length(const actor_model_t *model, int action)
{
	const actor_file_action_t *info = &model->actions[action];

	return info->duration_seconds > 0.0f ? info->duration_seconds : model->clips[info->clip].info.duration_seconds;
}

static void sample_action(const actor_model_t *model, int action, float seconds, actor_file_key_t *pose_out)
{
	const actor_file_action_t *info = &model->actions[action];
	float length = action_length(model, action);
	float clip_length = model->clips[info->clip].info.duration_seconds;

	if ((info->flags & ACTOR_ACTION_LOOP) != 0)
	{
		seconds = SDL_fmodf(seconds, length);
	}
	actor_model_sample_clip(model, info->clip, SDL_clamp(seconds, 0.0f, length) / length * clip_length, pose_out);
}

// Two bones from hip through knee to ankle, bent so the ankle gets to the
// target (or as near as the legs reach), the knee staying on the side it
// was on: the new knee and ankle
static void solve_two_bones(const float hip[3], const float knee[3], const float ankle[3], const float target[3], float knee_out[3],
	float ankle_out[3])
{
	float upper[3] = { knee[0] - hip[0], knee[1] - hip[1], knee[2] - hip[2] };
	float lower[3] = { ankle[0] - knee[0], ankle[1] - knee[1], ankle[2] - knee[2] };
	float to_target[3] = { target[0] - hip[0], target[1] - hip[1], target[2] - hip[2] };
	float upper_length = vector_length(upper);
	float lower_length = vector_length(lower);
	float distance = vector_length(to_target);
	float direction[3];
	float bend[3];
	float along;
	float bend_length;
	float cosine;
	float sine;

	if (distance < 1e-5f || upper_length < 1e-5f || lower_length < 1e-5f)
	{
		SDL_memcpy(knee_out, knee, sizeof(float) * 3);
		SDL_memcpy(ankle_out, ankle, sizeof(float) * 3);
		return;
	}
	for (int k = 0; k < 3; k++)
	{
		direction[k] = to_target[k] / distance;
	}
	distance = SDL_clamp(distance, SDL_fabsf(upper_length - lower_length) + 1e-3f, 0.999f * (upper_length + lower_length));
	// Which way the knee bends: its side of the line to the target
	along = upper[0] * direction[0] + upper[1] * direction[1] + upper[2] * direction[2];
	for (int k = 0; k < 3; k++)
	{
		bend[k] = upper[k] - direction[k] * along;
	}
	bend_length = vector_length(bend);
	if (bend_length < 1e-5f)
	{
		// Straight as animated: bend it forward (+Z), across the line
		float forward[3] = { 0.0f, 0.0f, 1.0f };

		along = direction[2];
		for (int k = 0; k < 3; k++)
		{
			bend[k] = forward[k] - direction[k] * along;
		}
		bend_length = SDL_max(vector_length(bend), 1e-5f);
	}
	cosine = (upper_length * upper_length + distance * distance - lower_length * lower_length) / (2.0f * upper_length * distance);
	cosine = SDL_clamp(cosine, -1.0f, 1.0f);
	sine = SDL_sqrtf(1.0f - cosine * cosine);
	for (int k = 0; k < 3; k++)
	{
		knee_out[k] = hip[k] + direction[k] * upper_length * cosine + bend[k] / bend_length * upper_length * sine;
		ankle_out[k] = hip[k] + direction[k] * distance;
	}
}

// Feet on the ground: each leg of the animated pose bent so its foot stands
// on what is under it (actor->foot_ground, from the move), as high above it
// as the animation lifts it above the body's ground plane, the foot turned
// with the slope while it is down. Then the skinning matrices. Where the
// animation put the feet is kept for the next move.
static void place_feet(const actors_t *actors, actor_t *actor, const actor_model_t *model, actor_file_key_t *pose)
{
	static const float model_up[3] = { 0.0f, 1.0f, 0.0f };
	const actor_type_t *type = &actors->types[actor->type];
	actor_file_key_t global[ACTOR_FILE_MAX_BONES];
	float inverse[4];
	float up[3];
	float weight = actor->feet_known ? actor->ik_weight : 0.0f;
	float placement[3] = { actor->x, actor->y - actor->root_lift, actor->z };
	float new_foot_x[ACTOR_MAX_LEGS];
	float new_foot_z[ACTOR_MAX_LEGS];

	float turn = actor->yaw_radians - degrees_to_radians(type->front_degrees);
	float yaw_only[4] = { 0.0f, SDL_sinf(0.5f * turn), 0.0f, SDL_cosf(0.5f * turn) };

	actor_model_compose_global(model, pose, global);
	quaternion_conjugate(actor->rotation, inverse);
	quaternion_rotate(actor->rotation, model_up, up);
	for (int i = 0; i < type->leg_count; i++)
	{
		const actor_leg_t *leg = &type->legs[i];
		float *hip = global[leg->upper].translation;
		float *knee = global[leg->lower].translation;
		float *ankle = global[leg->foot].translation;
		float scaled[3] = { ankle[0] * actor->scale, ankle[1] * actor->scale, ankle[2] * actor->scale };
		float ankle_world[3];
		float plane;
		float delta;
		float reach;
		float target[3];
		float new_knee[3];
		float new_ankle[3];
		float turn_upper[4];
		float turn_lower[4];
		float turn_foot[4];
		float moved[3];
		float old_lower[3];
		float new_lower[3];
		float upper_rotation[4];
		float lower_rotation[4];
		float foot_rotation[4];
		float temporary[4];
		float planted;
		float leg_weight;

		// Where the foot is over the map, by the yaw alone: the lean (which
		// comes from the ground under the feet) must not move the feet over
		// it, or a foot by a rock's edge goes on and off it every frame
		quaternion_rotate(yaw_only, scaled, ankle_world);
		new_foot_x[i] = ankle_world[0] + placement[0];
		new_foot_z[i] = ankle_world[2] + placement[2];
		quaternion_rotate(actor->rotation, scaled, ankle_world);
		for (int k = 0; k < 3; k++)
		{
			ankle_world[k] += placement[k];
		}
		// Where the animation thinks the ground under the foot is: the body's
		// ground plane, at the point the ground was looked up at (the same
		// point for both: on an even slope they agree, and the leg is left
		// as animated)
		plane = actor->feet_known ? placement[1] - (up[0] * (actor->foot_x[i] - placement[0]) + up[2] * (actor->foot_z[i] - placement[2])) / up[1]
			: placement[1] - (up[0] * (new_foot_x[i] - placement[0]) + up[2] * (new_foot_z[i] - placement[2])) / up[1];
		if (!actor->feet_known)
		{
			// The first look for the ground under it starts there
			actor->foot_ground[i] = plane;
			actor->foot_height[i] = plane;
			actor->foot_weight[i] = 0.0f;
		}
		leg_weight = weight * actor->foot_weight[i];
		if (leg_weight <= 0.001f)
		{
			continue;
		}
		// The foot moves by how far the ground under it is from that
		reach = (vector_length((float[3]){ knee[0] - hip[0], knee[1] - hip[1], knee[2] - hip[2] })
			+ vector_length((float[3]){ ankle[0] - knee[0], ankle[1] - knee[1], ankle[2] - knee[2] })) * actor->scale;
		delta = SDL_clamp((actor->foot_height[i] - plane) * leg_weight, -0.5f * reach, reach);
		ankle_world[1] += delta;
		for (int k = 0; k < 3; k++)
		{
			ankle_world[k] -= placement[k];
		}
		quaternion_rotate(inverse, ankle_world, target);
		for (int k = 0; k < 3; k++)
		{
			target[k] /= actor->scale;
		}
		solve_two_bones(hip, knee, ankle, target, new_knee, new_ankle);

		// The thigh turns the knee where it goes; the calf, turned along,
		// turns the ankle where it goes
		quaternion_between((float[3]){ knee[0] - hip[0], knee[1] - hip[1], knee[2] - hip[2] },
			(float[3]){ new_knee[0] - hip[0], new_knee[1] - hip[1], new_knee[2] - hip[2] }, turn_upper);
		for (int k = 0; k < 3; k++)
		{
			old_lower[k] = ankle[k] - knee[k];
			new_lower[k] = new_ankle[k] - new_knee[k];
		}
		quaternion_rotate(turn_upper, old_lower, moved);
		quaternion_between(moved, new_lower, turn_lower);
		quaternion_multiply(turn_upper, global[leg->upper].rotation, upper_rotation);
		quaternion_multiply(turn_lower, turn_upper, temporary);
		quaternion_multiply(temporary, global[leg->lower].rotation, lower_rotation);
		// The foot keeps its turn in model space (the legs' bending does not
		// twist it), and turns with the slope while it is down
		{
			float normal[3];
			float lifted = ankle[1] - leg->bind_foot_height;

			quaternion_rotate(inverse, actor->foot_normal[i], normal);
			quaternion_between(model_up, normal, turn_foot);
			{
				float angle = 2.0f * SDL_acosf(SDL_clamp(SDL_fabsf(turn_foot[3]), 0.0f, 1.0f));

				planted = 1.0f - SDL_clamp(lifted * actor->scale / IK_LIFTED_METERS, 0.0f, 1.0f);
				quaternion_scale(turn_foot, leg_weight * planted * (angle > IK_TILT_MAXIMUM_RADIANS ? IK_TILT_MAXIMUM_RADIANS / angle : 1.0f));
			}
			quaternion_multiply(turn_foot, global[leg->foot].rotation, foot_rotation);
		}

		// Back to turns relative to the parents
		{
			float parent_inverse[4];

			int parent = model->bones[leg->upper].parent;

			if (parent >= 0)
			{
				quaternion_conjugate(global[parent].rotation, parent_inverse);
				quaternion_multiply(parent_inverse, upper_rotation, pose[leg->upper].rotation);
			}
			else
			{
				SDL_memcpy(pose[leg->upper].rotation, upper_rotation, sizeof(upper_rotation));
			}
			quaternion_conjugate(upper_rotation, parent_inverse);
			quaternion_multiply(parent_inverse, lower_rotation, pose[leg->lower].rotation);
			quaternion_conjugate(lower_rotation, parent_inverse);
			quaternion_multiply(parent_inverse, foot_rotation, pose[leg->foot].rotation);
		}
	}
	for (int i = 0; i < type->leg_count; i++)
	{
		actor->foot_x[i] = new_foot_x[i];
		actor->foot_z[i] = new_foot_z[i];
	}
	actor->feet_known = true;
	if (weight > 0.0f)
	{
		actor_model_compose_global(model, pose, global);
	}
	actor_model_global_rows(model, global, actor->bone_rows);
}

// Two poses mixed: from a toward b by amount (0..1)
static void mix_poses(int bone_count, actor_file_key_t *a, const actor_file_key_t *b, float amount)
{
	for (int bone = 0; bone < bone_count; bone++)
	{
		const float *to = b[bone].rotation;
		float *from = a[bone].rotation;
		float sign = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3] < 0.0f ? -1.0f : 1.0f;
		float length_squared = 0.0f;

		for (int k = 0; k < 3; k++)
		{
			a[bone].translation[k] += (b[bone].translation[k] - a[bone].translation[k]) * amount;
		}
		for (int k = 0; k < 4; k++)
		{
			from[k] += (sign * to[k] - from[k]) * amount;
			length_squared += from[k] * from[k];
		}
		for (int k = 0; k < 4; k++)
		{
			from[k] /= SDL_max(SDL_sqrtf(length_squared), 1e-6f);
		}
	}
}

static bool walking_action(const actor_type_t *type, int action);

// Its walk's pose bent round its curve (walk_curve): walk_left or
// walk_right at the same point of the step, mixed in so much
static void bend_walk(const actors_t *actors, const actor_t *actor, int action, float seconds, actor_file_key_t *pose)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);
	actor_file_key_t side_pose[ACTOR_FILE_MAX_BONES];
	int side = actor->walk_curve > 0.0f ? type->walk_left_action : type->walk_right_action;
	float length = action_length(model, action);
	float phase;

	if (action != type->walk_action || side < 0 || SDL_fabsf(actor->walk_curve) < 0.01f || length <= 0.0f)
	{
		return;
	}
	phase = SDL_fmodf(seconds, length) / length;
	phase = phase < 0.0f ? phase + 1.0f : phase;
	sample_action(model, side, phase * action_length(model, side), side_pose);
	mix_poses(model->bone_count, pose, side_pose, SDL_fabsf(actor->walk_curve));
}

// The pose's skinning matrices from the action and the crossfade
static void update_pose(const actors_t *actors, actor_t *actor)
{
	const actor_model_t *model = model_of(actors, actor);
	actor_file_key_t pose[ACTOR_FILE_MAX_BONES];
	actor_file_key_t old_pose[ACTOR_FILE_MAX_BONES];
	float blend_seconds = actors->types[actor->type].blend_seconds;

	actor->root_lift = 0.0f;
	if (actor->action < 0)
	{
		SDL_memcpy(actor->bone_rows, model->bind_rows, sizeof(float) * 12 * (size_t)model->bone_count);
		return;
	}
	sample_action(model, actor->action, actor->action_seconds, pose);
	bend_walk(actors, actor, actor->action, actor->action_seconds, pose);
	if (actor->previous_action >= 0 && actor->blend_left_seconds > 0.0f && blend_seconds > 0.0f)
	{
		// From the old pose to the new one, smoothly
		float amount = smoothstep_float(0.0f, 1.0f, 1.0f - actor->blend_left_seconds / blend_seconds);

		sample_action(model, actor->previous_action, actor->previous_seconds, old_pose);
		bend_walk(actors, actor, actor->previous_action, actor->previous_seconds, old_pose);
		mix_poses(model->bone_count, old_pose, pose, amount);
		SDL_memcpy(pose, old_pose, sizeof(actor_file_key_t) * (size_t)model->bone_count);
	}
	if (actors->types[actor->type].flies && model->bone_count > 0)
	{
		actor->root_lift = (pose[0].translation[1] - model->bones[0].translation[1]) * actor->scale;
	}
	if (actors->types[actor->type].leg_count > 0)
	{
		place_feet(actors, actor, model, pose);
		return;
	}
	actor_model_pose_rows(model, pose, actor->bone_rows);
}

static void start_action(const actors_t *actors, actor_t *actor, int action)
{
	if (action == actor->action)
	{
		return;
	}
	actor->previous_action = actor->action;
	actor->previous_seconds = actor->action_seconds;
	actor->previous_rate = actor->action_rate;
	actor->blend_left_seconds = actor->previous_action >= 0 ? actors->types[actor->type].blend_seconds : 0.0f;
	actor->action = action;
	actor->action_seconds = 0.0f;
	actor->action_rate = 1.0f;
}

#define WALK_CURVE_PER_SECOND 2.5f      // its walk's bend eased this fast (all straight to all round a curve: 0.4 s)

static float walk_turn_radians_per_second(const actors_t *actors, const actor_t *actor, float speed);

// Debug: POTOP_ANIMATION_LOG=file and POTOP_ANIMATION_TYPE=name: every
// frame, each actor of that type's actions as they play (for
// tools/animation_check.py: the pose as drawn, its jumps)
static SDL_IOStream *animation_log;
static const char *animation_log_type;
static bool animation_log_tried;
static double animation_log_clock;

static void log_animation(const actors_t *actors, const actor_t *actor)
{
	const actor_model_t *model = model_of(actors, actor);

	SDL_IOprintf(animation_log, "%.4f %u %s %.4f %.3f %s %.4f %.4f %.3f %.3f\n", animation_log_clock, actor->id,
		actor->action >= 0 ? model->actions[actor->action].name : "-", (double)actor->action_seconds, (double)actor->action_rate,
		actor->previous_action >= 0 ? model->actions[actor->previous_action].name : "-", (double)actor->previous_seconds,
		(double)actor->blend_left_seconds, (double)actor->speed, (double)actor->walk_curve);
}

void actors_update(actors_t *actors, float seconds)
{
	if (!animation_log_tried)
	{
		const char *path = SDL_getenv("POTOP_ANIMATION_LOG");

		animation_log_tried = true;
		animation_log_type = SDL_getenv("POTOP_ANIMATION_TYPE");
		animation_log = path != NULL && animation_log_type != NULL ? SDL_IOFromFile(path, "w") : NULL;
	}
	animation_log_clock += seconds;
	actors->pose_clock++;
	for (int i = 0; i < actors->count; i++)
	{
		actor_t *actor = &actors->actors[i];
		const actor_model_t *model = model_of(actors, actor);

		if (actor->action >= 0)
		{
			const actor_file_action_t *info = &model->actions[actor->action];
			float length = action_length(model, actor->action);

			actor->action_seconds += seconds * actor->action_rate;
			if ((info->flags & ACTOR_ACTION_LOOP) == 0 && actor->action_seconds >= length)
			{
				int idle = actors->types[actor->type].idle_action;

				actor->action_seconds = length;
				// Once is once; dying is for good
				if (SDL_strncmp(info->name, "die", 3) != 0 && idle >= 0 && idle != actor->action)
				{
					start_action(actors, actor, idle);
				}
			}
			else if ((info->flags & ACTOR_ACTION_LOOP) != 0 && actor->action_seconds >= length)
			{
				// Kept small, so the float keeps its precision
				actor->action_seconds = SDL_fmodf(actor->action_seconds, length);
			}
			else if ((info->flags & ACTOR_ACTION_LOOP) != 0 && actor->action_seconds < 0.0f && length > 0.0f)
			{
				// (played backwards: backing out)
				actor->action_seconds = length + SDL_fmodf(actor->action_seconds, length);
			}
		}
		if (actor->previous_action >= 0)
		{
			actor->previous_seconds += seconds * actor->previous_rate;
			actor->blend_left_seconds -= seconds;
			if (actor->blend_left_seconds <= 0.0f)
			{
				actor->previous_action = -1;
			}
		}
		// Its walk's bend: toward how fast it turns against how fast its
		// walk_left / walk_right turn at its speed (straight when it does
		// not walk), eased (a turn coming and going bends it smoothly)
		{
			const actor_type_t *type = &actors->types[actor->type];
			float wanted = 0.0f;

			if (actor->action == type->walk_action && type->walk_left_action >= 0 && type->walk_right_action >= 0 && actor->speed > 0.02f)
			{
				wanted = SDL_clamp(actor->turn_rate / SDL_max(walk_turn_radians_per_second(actors, actor, actor->speed), 1e-3f), -1.0f, 1.0f);
			}
			if (actor->walk_curve < wanted)
			{
				actor->walk_curve = SDL_min(actor->walk_curve + WALK_CURVE_PER_SECOND * seconds, wanted);
			}
			else
			{
				actor->walk_curve = SDL_max(actor->walk_curve - WALK_CURVE_PER_SECOND * seconds, wanted);
			}
		}
		// The pose itself only when it is drawn (ensure_pose); a flyer's
		// now (its pose lifts it: where it is, its box, come from it)
		if (actors->types[actor->type].flies)
		{
			update_pose(actors, actor);
			actor->pose_frame = actors->pose_clock;
		}
		if (animation_log != NULL && SDL_strcmp(actors->types[actor->type].name, animation_log_type) == 0)
		{
			log_animation(actors, actor);
		}
	}
}

int actors_find(const actors_t *actors, uint32_t id)
{
	for (int i = 0; i < actors->count; i++)
	{
		if (actors->actors[i].id == id)
		{
			return i;
		}
	}
	return -1;
}

void actors_set_running(actors_t *actors, uint32_t id, bool running)
{
	int index = actors_find(actors, id);

	if (index >= 0)
	{
		actors->actors[index].running = running;
	}
}

bool actors_play(actors_t *actors, uint32_t id, const char *action_name)
{
	int index = actors_find(actors, id);
	int action;

	if (index < 0)
	{
		return false;
	}
	action = actor_model_find_action(model_of(actors, &actors->actors[index]), action_name);
	if (action < 0)
	{
		return false;
	}
	// (a clip of its own, not its walk: a wanderer stops for it, or it
	// went on wandering once the clip was over)
	if (!walking_action(&actors->types[actors->actors[index].type], action))
	{
		actors->actors[index].wandering = false;
	}
	start_action(actors, &actors->actors[index], action);
	return true;
}

const char *actors_cycle_actions(actors_t *actors, int step)
{
	const char *first_name = NULL;

	for (int i = 0; i < actors->count; i++)
	{
		actor_t *actor = &actors->actors[i];
		const actor_model_t *model = model_of(actors, actor);
		int action;

		if (model->action_count == 0)
		{
			continue;
		}
		action = ((actor->action < 0 ? 0 : actor->action) + step + model->action_count) % model->action_count;
		start_action(actors, actor, action);
		if (first_name == NULL)
		{
			first_name = model->actions[action].name;
		}
	}
	return first_name;
}

bool actors_any_near(const actors_t *actors, float x, float z, float radius)
{
	for (int i = 0; i < actors->count; i++)
	{
		float dx = actors->actors[i].x - x;
		float dz = actors->actors[i].z - z;

		if (dx * dx + dz * dz <= radius * radius)
		{
			return true;
		}
	}
	return false;
}

// Flying (below)
static void pick_cruise_height(const actor_type_t *type, actor_t *actor);
static bool is_dying(const actors_t *actors, const actor_t *actor);
static bool plan_flight(actors_t *actors, actor_t *actor, navigation_t *navigation, const terrain_t *terrain, const char *why);

uint32_t actors_add(actors_t *actors, const terrain_t *terrain, int type, float x, float z, float yaw_radians)
{
	actor_t *actor;

	if (type < 0 || type >= actors->type_count || actors->types[type].model < 0)
	{
		return 0;
	}
	if (actors->count == actors->capacity)
	{
		int capacity = SDL_max(16, actors->capacity * 2);
		actor_t *grown = SDL_realloc(actors->actors, sizeof(actor_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return 0;
		}
		actors->actors = grown;
		actors->capacity = capacity;
	}
	actor = &actors->actors[actors->count++];
	SDL_zerop(actor);
	actor->id = actors->next_id++;
	actor->interactive = true;
	if (actors->next_id == 0)
	{
		actors->next_id = 1;
	}
	actor->type = type;
	actor->x = x;
	actor->z = z;
	actor->yaw_radians = yaw_radians;
	actor->spot_yaw_radians = yaw_radians;
	actor->scale = actors->types[type].scale;
	actor->y = terrain_sample_height_meters(terrain, x, z);
	place_on_ground(actors, actor, terrain, NULL, NULL, 0.0f);
	actor->action = actors->types[type].idle_action;
	actor->action_rate = 1.0f;
	actor->previous_action = -1;
	actor->gait = -1;
	actor->stride_scale = 1.0f;
	actor->random_state = actor->id * 2654435761u;
	pick_cruise_height(&actors->types[type], actor);
	update_pose(actors, actor);
	actor->pose_frame = actors->pose_clock;
	actors->shadow_dirty = true;
	return actor->id;
}

bool actors_remove(actors_t *actors, uint32_t id)
{
	int index = actors_find(actors, id);

	if (index < 0)
	{
		return false;
	}
	actors->actors[index] = actors->actors[actors->count - 1];
	actors->count--;
	actors->shadow_dirty = true;
	return true;
}

bool actors_remove_nearest(actors_t *actors, float x, float z, float radius)
{
	int nearest = -1;
	float nearest_squared = radius * radius;

	for (int i = 0; i < actors->count; i++)
	{
		float dx = actors->actors[i].x - x;
		float dz = actors->actors[i].z - z;
		float squared = dx * dx + dz * dz;

		if (squared <= nearest_squared)
		{
			nearest = i;
			nearest_squared = squared;
		}
	}
	if (nearest < 0)
	{
		return false;
	}
	actors->actors[nearest] = actors->actors[actors->count - 1];
	actors->count--;
	actors->shadow_dirty = true;
	return true;
}

void actors_clear(actors_t *actors)
{
	if (actors->count > 0)
	{
		actors->shadow_dirty = true;
	}
	actors->count = 0;
}

// ---------------------------------------------------------------------------
// Moving about

#define ARRIVE_METERS 0.3f            // at the goal
#define EXACT_ARRIVE_METERS 0.04f     // at the goal of an exact walk
#define EXACT_SHUFFLE_METERS 0.16f    // this near it, shuffled onto it
#define EXACT_SHUFFLE_SPEED 0.25f     // meters a second
#define SIDESTEP_METERS 0.7f          // an exact walk this short: turned onto it on the spot
#define SIDESTEP_LEAST_SECONDS 0.6f   // its slide no quicker than this
#define SIDESTEP_STEPS_RADIANS 0.5f   // a step or two with less than this to turn: its own steps (aside, back), not its turn's
#define SHUFFLE_METERS 0.12f          // nearer: a shuffle on the spot
#define SHUFFLE_SPEED 0.15f           // meters a second
#define GAIT_START_MORE_METERS 0.9f   // a walk's start only for a walk this much longer than it and its stop (shorter: as any walk)
#define GAIT_HOLD_MOST_SECONDS 2.5f   // turning on the spot for its start no longer than this
#define GAIT_START_RADIANS 0.8f       // facing the way this near: the walk's start straight on
#define GAIT_TURN_ABOUT_RADIANS 2.4f  // the way this far round behind: the start turning about
#define GAIT_PLAN_METERS 3.0f         // a stop planned from this near the goal
#define GAIT_STRIDE_LEAST 0.8f        // the stride stretched no more than this either way
#define GAIT_STRIDE_MOST 1.25f
#define WAYPOINT_METERS 0.6f          // past a waypoint on the way
// Its turns' inertia (inertial_turn): from still to its fastest turn in
// this long, and as long to stop
#define TURN_RAMP_SECONDS 0.2f
// Where a walk steers for (steer_point): this far ahead along the path at
// a standstill, more the faster (up to the most), found again this often
#define STEER_AHEAD_METERS 1.5f
#define STEER_AHEAD_PER_SPEED 0.8f    // meters more per meter a second
#define STEER_AHEAD_MOST_METERS 4.5f
#define STEER_SECONDS 0.2f
#define STEER_DIRECT_SHARE 0.6f       // the point ahead at least this much of that far off straight (a right corner's: 0.7), or it is the waypoint
#define STRAIGHT_WAYPOINT_METERS 0.25f
#define BODY_BRUSHES 2               // of its body's 8 points (navigation_quarter_tightness): its sides brushing, let be
#define BODY_WAY_METERS 5.0f         // its body's straight way looked at this far (body_way_clear); the rest as it gets there
#define ARC_SECONDS 0.1f             // its turn's arc looked at this often (actor_t.arc_clear)
#define ARC_FREE_RADIANS 0.15f       // its way this little off its nose: no arc to speak of
#define ARC_RADIUS_LEAST_METERS 0.4f // an arc tighter than this is none: it turns on the spot (round a point so near it went for ever)
#define CORNER_SLACK_METERS 0.3f     // round a path's corner this far off it at the most: its pace there by that
#define FINE_AFTER_SECONDS 0.2f      // steps refused this long: a fine way round (navigation_fine_path)
#define FINE_AGAIN_SECONDS 1.0f      // and not again sooner
#define FINE_REACH_METERS 3.0f       // towards its waypoint at most this far
#define FINE_BY_WAYPOINT_METERS 0.55f // a fine way's end this near its waypoint: the waypoint's own, reached with it
#define FINE_FAILURES_MOST 2         // no fine way found this often where it stands: none there is, the walk given up
#define FINE_POINT_METERS 0.15f      // a fine way's point reached this near // past a straight walker's waypoint (its mind's spot)
// Its jump (jump_move): the phases; faced this near before it goes; no
// longer turning to it than this; without clips, gathered and landed this
// long; its arc's top this far over the higher of its ends; how much of
// the way it flies its body tilts by, and at the most; its body's middle
// (the tilt's center) this much of its height up
#define JUMP_NONE 0
#define JUMP_TURN 1
#define JUMP_OFF 2
#define JUMP_AIR 3
#define JUMP_LAND 4
#define JUMP_FACE_RADIANS 0.12f
#define JUMP_TURN_MOST_SECONDS 4.0f
#define JUMP_OFF_SECONDS 0.25f
#define JUMP_LAND_SECONDS 0.2f
#define JUMP_OVER_METERS 0.25f
#define JUMP_GRAVITY 9.81f
#define JUMP_PITCH_SHARE 0.6f
#define JUMP_PITCH_MOST_RADIANS 0.5f
#define JUMP_MIDDLE_SHARE 0.5f
#define LOOK_SECONDS 0.5f             // how often it looks at the path ahead
#define STUCK_SECONDS 3.0f            // this long without getting half a meter: search again
#define STUCK_METERS 0.5f
#define STUCK_TURN_RADIANS 1.0f       // its way this far round: turning about, not stuck (its time counts a quarter)
#define MAXIMUM_REPLANS 3             // searches in a row that got nowhere: give up
#define ACCELERATION 3.0f             // meters per second, per second

static navigation_agent_t agent_of(const actor_type_t *type)
{
	navigation_agent_t agent;

	agent.height_meters = type->height_meters;
	agent.step_meters = type->step_meters;
	agent.maximum_slope = SDL_tanf(degrees_to_radians(type->maximum_slope_degrees));
	agent.wade_depth_meters = type->wade_depth_meters;
	agent.swim_depth_meters = type->swim_depth_meters;
	agent.can_swim = type->can_swim;
	agent.swim_meters_per_second = type->swim_meters_per_second;
	agent.radius_meters = 0.5f * type->width_meters;
	// (a shut door stops one that does not open doors: navigation.h)
	agent.groups = (uint8_t)((type->path_groups & ~NAVIGATION_GROUP_SHUT_DOOR) | (type->opens_doors ? 0u : NAVIGATION_GROUP_SHUT_DOOR));
	agent.opens_doors = type->opens_doors;
	agent.line_center_only = false;
	agent.jump_up_meters = type->jump_up_meters;
	agent.jump_down_meters = type->jump_down_meters;
	return agent;
}

navigation_agent_t actors_agent(const actors_t *actors, int type)
{
	return agent_of(&actors->types[type]);
}

static void push_event(actors_t *actors, actor_event_type_t type, const actor_t *actor)
{
	actor_event_t *event;

	if (actors->event_count == ACTOR_EVENT_QUEUE_SIZE)
	{
		// Nobody reads them: the oldest goes
		actors->event_first = (actors->event_first + 1) % ACTOR_EVENT_QUEUE_SIZE;
		actors->event_count--;
	}
	event = &actors->events[(actors->event_first + actors->event_count) % ACTOR_EVENT_QUEUE_SIZE];
	actors->event_count++;
	event->type = type;
	event->id = actor->id;
	event->game_id = actor->game_id;
	event->x = actor->x;
	event->z = actor->z;
}

bool actors_poll_event(actors_t *actors, actor_event_t *event_out)
{
	if (actors->event_count == 0)
	{
		return false;
	}
	*event_out = actors->events[actors->event_first];
	actors->event_first = (actors->event_first + 1) % ACTOR_EVENT_QUEUE_SIZE;
	actors->event_count--;
	return true;
}

uint32_t actors_nearest(const actors_t *actors, float x, float z, float radius)
{
	uint32_t nearest = 0;
	float nearest_squared = radius * radius;

	for (int i = 0; i < actors->count; i++)
	{
		float dx = actors->actors[i].x - x;
		float dz = actors->actors[i].z - z;
		float squared = dx * dx + dz * dz;

		if (squared <= nearest_squared)
		{
			nearest = actors->actors[i].id;
			nearest_squared = squared;
		}
	}
	return nearest;
}

static void stop_moving(actor_t *actor)
{
	actor->wandering = false;
	actor->moving = false;
	actor->path_count = 0;
	actor->path_index = 0;
	actor->steer_seconds = 0.0f;
}

// The log, for one actor: what it finds at a point
static void log_point(const actors_t *actors, const actor_t *actor, navigation_t *navigation, const terrain_t *terrain,
	const water_t *water, const char *what, float x, float z, float near_height)
{
	navigation_agent_t agent = agent_of(&actors->types[actor->type]);
	char text[512];
	bool standable;

	if (!actors->log_paths || actors->objects == NULL)
	{
		return;
	}
	standable = navigation_describe(navigation, terrain, water, actors->objects, &agent, x, z, near_height, text, sizeof(text));
	SDL_Log("  %s (%.1f, %.1f): %s%s", what, (double)x, (double)z, text, standable ? "" : " -- CANNOT STAND THERE");
}

// Searches a path from where the actor is to its goal (why: for the log;
// quiet: a failure is not logged). False if not even a step can be made
// (the actor stops; what then is the caller's)
static bool plan_path_logged(const actors_t *actors, actor_t *actor, const actor_type_t *type, navigation_t *navigation,
	const terrain_t *terrain, const water_t *water, const char *why, bool quiet);

static bool plan_path(const actors_t *actors, actor_t *actor, const actor_type_t *type, navigation_t *navigation, const terrain_t *terrain,
	const water_t *water, const char *why)
{
	return plan_path_logged(actors, actor, type, navigation, terrain, water, why, false);
}

static float water_depth_at(const water_t *water, float x, float z)
{
	float depth = 0.0f;
	float velocity_x;
	float velocity_z;

	if (water == NULL || !water_sample(water, x, z, &depth, &velocity_x, &velocity_z))
	{
		return 0.0f;
	}
	return depth;
}

static void push_event(actors_t *actors, actor_event_type_t type, const actor_t *actor);

// A search found no way: in water it waits and tries again every second
// (the flood may calm down), on dry ground it gives up
static void path_failed(actors_t *actors, actor_t *actor, const water_t *water)
{
	stop_moving(actor);
	if (water_depth_at(water, actor->x, actor->z) > 0.05f)
	{
		if (!actor->waiting && actors->log_paths)
		{
			SDL_Log("Actor %u (%s): no way out of the water now; tries again every second", actor->id,
				actors->types[actor->type].name);
		}
		actor->waiting = true;
		actor->wait_seconds = 1.0f;
		return;
	}
	actor->waiting = false;
	push_event(actors, ACTOR_EVENT_NO_PATH, actor);
}

static bool plan_path_logged(const actors_t *actors, actor_t *actor, const actor_type_t *type, navigation_t *navigation,
	const terrain_t *terrain, const water_t *water, const char *why, bool quiet)
{
	navigation_agent_t agent = agent_of(type);
	uint64_t start = SDL_GetPerformanceCounter();
	navigation_result_t result = navigation_find_path(navigation, terrain, water, &agent, actor->x, actor->z, actor->y, actor->goal_x,
		actor->goal_z, actor->path, &actor->path_count, &actor->path_complete);

	// (the waypoints it gets to by a jump)
	for (int k = 0; k < NAVIGATION_MAX_WAYPOINTS; k++)
	{
		actor->path_jump[k] = k < actor->path_count && navigation_path_jump(navigation, k, &actor->path_jump_height[k]) ? 1 : 0;
	}
	// (about to jump: not now, the new path says whether; in the air: it
	// lands first, and searches again from there)
	if (actor->jump_phase == JUMP_TURN || actor->jump_phase == JUMP_OFF)
	{
		actor->jump_phase = JUMP_NONE;
	}
	else if (actor->jump_phase != JUMP_NONE)
	{
		actor->jump_replan = true;
	}

	{
		float dx = actor->goal_x - actor->x;
		float dz = actor->goal_z - actor->z;

		frame_profile_path(SDL_GetPerformanceCounter() - start, actor->id, type->name, navigation->last_expansions,
			result == NAVIGATION_PATH_FOUND ? "found" : result == NAVIGATION_PATH_PARTIAL ? "partial" : "none", SDL_sqrtf(dx * dx + dz * dz));
	}

	if (actors->log_paths && !(quiet && result == NAVIGATION_PATH_NONE))
	{
		SDL_Log("Actor %u (%s), %s: from (%.1f, %.1f, height %.2f) to (%.1f, %.1f): %s, %d waypoints%s, %d surfaces searched in %.2f ms",
			actor->id, type->name, why, (double)actor->x, (double)actor->z, (double)actor->y, (double)actor->goal_x, (double)actor->goal_z,
			result == NAVIGATION_PATH_FOUND ? "found" : result == NAVIGATION_PATH_PARTIAL ? "PARTIAL (as near as it gets)" : "NONE",
			actor->path_count, actor->path_complete ? "" : " (more after the last)", navigation->last_expansions,
			(double)(SDL_GetPerformanceCounter() - start) * 1000.0 / (double)SDL_GetPerformanceFrequency());
		for (int k = 0; k < actor->path_count; k++)
		{
			SDL_Log("  waypoint %d: (%.1f, %.1f)", k, (double)actor->path[k][0], (double)actor->path[k][1]);
		}
		if (result != NAVIGATION_PATH_FOUND)
		{
			log_point(actors, actor, navigation, terrain, water, "start", actor->x, actor->z, actor->y);
			log_point(actors, actor, navigation, terrain, water, "goal", actor->goal_x, actor->goal_z,
				navigation_surface_height(navigation, terrain, NULL, actor->goal_x, actor->goal_z, actor->y));
			if (actor->path_count > 0)
			{
				float dx = actor->goal_x - actor->path[actor->path_count - 1][0];
				float dz = actor->goal_z - actor->path[actor->path_count - 1][1];

				SDL_Log("  the nearest it gets is %.1f m from the goal", (double)SDL_sqrtf(dx * dx + dz * dz));
			}
		}
	}

	actor->path_index = 0;
	actor->steer_seconds = 0.0f;
	actor->path_generation = navigation->generation;
	actor->look_seconds = LOOK_SECONDS;
	actor->path_partial = result == NAVIGATION_PATH_PARTIAL;
	if (result == NAVIGATION_PATH_NONE || actor->path_count == 0)
	{
		stop_moving(actor);
		return false;
	}
	actor->moving = true;
	return true;
}

bool actors_move_to(actors_t *actors, navigation_t *navigation, const terrain_t *terrain, const water_t *water, uint32_t id,
	float x, float z)
{
	int index = actors_find(actors, id);
	actor_t *actor;

	if (index < 0)
	{
		return false;
	}
	actor = &actors->actors[index];
	actor->straight = false;
	actor->wandering = false;
	actor->goal_x = x;
	actor->goal_z = z;
	actor->replans = 0;
	actor->stuck_seconds = 0.0f;
	actor->progress_x = actor->x;
	actor->progress_z = actor->z;
	actor->waiting = false;
	if (is_dying(actors, actor))
	{
		return false;
	}
	if (actors->types[actor->type].flies)
	{
		if (!plan_flight(actors, actor, navigation, terrain, "sent"))
		{
			push_event(actors, ACTOR_EVENT_NO_PATH, actor);
			return false;
		}
		return true;
	}
	if (!plan_path(actors, actor, &actors->types[actor->type], navigation, terrain, water, "sent"))
	{
		path_failed(actors, actor, water);
		return actor->waiting;
	}
	return true;
}

bool actors_walk_straight(actors_t *actors, uint32_t id, const float (*points)[2], int count)
{
	int index = actors_find(actors, id);
	actor_t *actor;

	if (index < 0 || count <= 0)
	{
		return false;
	}
	actor = &actors->actors[index];
	if (is_dying(actors, actor) || actors->types[actor->type].flies)
	{
		return false;
	}
	count = SDL_min(count, NAVIGATION_MAX_WAYPOINTS);
	for (int k = 0; k < count; k++)
	{
		actor->path[k][0] = points[k][0];
		actor->path[k][1] = points[k][1];
	}
	SDL_memset(actor->path_jump, 0, sizeof(actor->path_jump));
	actor->path_count = count;
	actor->path_index = 0;
	actor->steer_seconds = 0.0f;
	actor->path_complete = true;
	actor->path_partial = false;
	actor->straight = true;
	actor->arrive_meters = 0.0f;
	actor->arrive_face = false;
	actor->sidestep = false;
	actor->stride_scale = 1.0f;
	actor->start_held_seconds = 0.0f;
	actor->avoid_seconds = 0.0f;
	actor->crowd_seconds = 0.0f;
	actor->backing_meters = 0.0f;
	actor->goal_x = points[count - 1][0];
	actor->goal_z = points[count - 1][1];
	actor->replans = 0;
	actor->stuck_seconds = 0.0f;
	actor->progress_x = actor->x;
	actor->progress_z = actor->z;
	actor->waiting = false;
	actor->path_dirty = false;
	actor->wandering = false;
	actor->moving = true;
	return true;
}

bool actors_walk_straight_exact(actors_t *actors, uint32_t id, const float (*points)[2], int count, float face_yaw_radians)
{
	int index = actors_find(actors, id);
	actor_t *actor;

	if (index < 0 || count < 1)
	{
		return false;
	}
	actor = &actors->actors[index];
	// (a step or two away: no walk, turned on the spot onto it)
	if (count == 1 && SDL_sqrtf((points[0][0] - actor->x) * (points[0][0] - actor->x) + (points[0][1] - actor->z) * (points[0][1] - actor->z))
		< SIDESTEP_METERS)
	{
		stop_moving(actor);
		actor->facing = false;
		actor->sidestep = true;
		actor->sidestep_x = points[0][0];
		actor->sidestep_z = points[0][1];
		actor->arrive_face = true;
		actor->arrive_yaw = face_yaw_radians;
		return true;
	}
	if (!actors_walk_straight(actors, id, points, count))
	{
		return false;
	}
	actor->arrive_meters = EXACT_ARRIVE_METERS;
	actor->arrive_face = true;
	actor->arrive_yaw = face_yaw_radians;
	actor->sidestep = false;
	return true;
}

static float wrap_angle(float radians);

bool actors_wander(actors_t *actors, uint32_t id, float heading_radians, float share)
{
	int index = actors_find(actors, id);
	actor_t *actor;

	if (index < 0)
	{
		return false;
	}
	actor = &actors->actors[index];
	if (is_dying(actors, actor) || actors->types[actor->type].flies)
	{
		return false;
	}
	if (!actor->wandering)
	{
		stop_moving(actor);
		actor->sidestep = false;
		actor->facing = false;
		actor->waiting = false;
		actor->backing_meters = 0.0f;
		actor->straight_free_meters = 0.0f;
	}
	// (a straight walker: its body's checks, its standing turn limit)
	actor->straight = true;
	actor->wandering = true;
	actor->wander_heading = wrap_angle(heading_radians);
	actor->wander_share = SDL_clamp(share, -1.0f, 1.0f);
	return true;
}

void actors_mark_path_dirty(actors_t *actors, uint32_t id)
{
	int index = actors_find(actors, id);

	if (index >= 0)
	{
		actors->actors[index].path_dirty = true;
	}
}

void actors_stop(actors_t *actors, uint32_t id)
{
	int index = actors_find(actors, id);

	if (index >= 0)
	{
		actor_t *actor = &actors->actors[index];

		if (actors->types[actor->type].flies && actor->flight == ACTOR_FLIGHT_FLYING)
		{
			// A flyer cannot stop in the air: it circles where it is
			actor->goal_x = actor->x;
			actor->goal_z = actor->z;
			actor->path_partial = false;
			actor->moving = false;
			actor->flight = ACTOR_FLIGHT_CIRCLING;
			actor->circle_floor_height = actor->y;
			actor->circle_direction = 1.0f;
			return;
		}
		stop_moving(actor);
		actor->waiting = false;
	}
}

// Looks at the stretch of path it can see: if something changed there and
// the way is blocked now, searches again. False if it had to give up.
// A turn of turn radians wanted (wrapped), at most most_per_second: how
// much of it this frame, with inertia. The turning speed gains and loses
// at most its most in TURN_RAMP_SECONDS (no turn from still to full and
// back in a frame: that read as a robot), and is never more than it can
// lose again before it faces the way (no swinging past and back)
static float inertial_turn(actor_t *actor, float turn, float most_per_second, float seconds)
{
	float acceleration = most_per_second / TURN_RAMP_SECONDS;
	float can_stop = SDL_sqrtf(2.0f * acceleration * SDL_fabsf(turn));
	float wanted = (turn >= 0.0f ? 1.0f : -1.0f) * SDL_min(most_per_second, can_stop);
	float change = acceleration * seconds;
	float applied;

	actor->angular_speed = SDL_clamp(actor->angular_speed, -most_per_second, most_per_second);
	actor->angular_speed += SDL_clamp(wanted - actor->angular_speed, -change, change);
	applied = actor->angular_speed * seconds;
	// (the last bit: onto the way exactly, the turn over)
	if ((applied >= 0.0f) == (turn >= 0.0f) && SDL_fabsf(applied) >= SDL_fabsf(turn))
	{
		applied = turn;
		actor->angular_speed = 0.0f;
	}
	return applied;
}

// On a path, how fast it may turn at a speed (radians a second): standing,
// its pivot; from its walking pace up, its walk's curve; between the two,
// between (the slower it goes the tighter it turns, as one does)
static float path_turn_radians_per_second(const actor_type_t *type, float speed)
{
	float pivot = degrees_to_radians(type->pivot_degrees_per_second);
	float turning = degrees_to_radians(type->turn_degrees_per_second);
	float share = SDL_clamp(SDL_fabsf(speed) / type->walk_meters_per_second, 0.0f, 1.0f);

	return pivot + (turning - pivot) * share;
}

// The fastest it goes round a circle of radius meters turning no faster
// than it may at that pace (path_turn_radians_per_second): from its
// walking pace up, its walk's curve; under it, the pace at which its
// turning (quicker the slower it goes) just makes the circle
static float arc_speed(const actor_type_t *type, float radius)
{
	float pivot = degrees_to_radians(type->pivot_degrees_per_second);
	float turning = degrees_to_radians(type->turn_degrees_per_second);
	float walk = type->walk_meters_per_second;

	if (turning * radius >= walk)
	{
		return turning * radius;
	}
	return pivot * radius / (1.0f + (pivot - turning) * radius / walk);
}

// Whether its body can go straight from where it is to a point: the
// path finding's line there (the cells' rectangles: no wall, no drop, no
// deep water between) and its body's along it over the quarter meters
// (navigation_body_line, the first BODY_WAY_METERS of it: by its step
// guard's own measure, so the steps of a way this lets through are not
// refused). Every way it takes off its path's own lines is asked here
// first: the point ahead it steers for, a corner cut, a turn's arc
static bool body_way_clear(const actor_t *actor, const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z)
{
	float dx = x - actor->x;
	float dz = z - actor->z;
	float distance = SDL_sqrtf(dx * dx + dz * dz);
	float share = distance > BODY_WAY_METERS ? BODY_WAY_METERS / distance : 1.0f;

	return navigation_line_passable(navigation, terrain, water, agent, actor->x, actor->z, actor->y, x, z, NULL, NULL, NULL)
		&& navigation_body_line(navigation, terrain, water, agent, actor->x, actor->z, actor->center_ground, actor->x + dx * share,
			actor->z + dz * share, BODY_BRUSHES);
}

// Where a walk on its path steers for (not a straight walker's: its
// waypoints are its mind's exact spots): a point a little ahead along the
// path (more the faster it goes), so it starts round a corner before it
// is at it, as one does; the waypoints it has cut past (one it is near and
// can walk straight past to the next) left behind. Only where its body
// can go straight to it (body_way_clear); else its waypoint, and whether
// its body has the straight way to that (actor_t.steer_blocked: not, off
// its path's line or a post by the corner: a fine way, move_actor).
// Found again every STEER_SECONDS and when its waypoint changes
static void steer_point(actors_t *actors, actor_t *actor, const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	float seconds, float *target_x, float *target_z)
{
	const actor_type_t *type = &actors->types[actor->type];
	navigation_agent_t agent = agent_of(type);
	float ahead = SDL_min(STEER_AHEAD_METERS + STEER_AHEAD_PER_SPEED * SDL_fabsf(actor->speed), STEER_AHEAD_MOST_METERS);

	actor->steer_seconds -= seconds;
	if (actor->steer_seconds > 0.0f && actor->steer_index == actor->path_index)
	{
		*target_x = actor->steer_x;
		*target_z = actor->steer_z;
		return;
	}
	actor->steer_seconds = STEER_SECONDS;
	// (waypoints cut past: near one, the next straight ahead walkable)
	for (int k = 0; k < 3 && actor->path_index < actor->path_count - 1; k++)
	{
		float dx = actor->path[actor->path_index][0] - actor->x;
		float dz = actor->path[actor->path_index][1] - actor->z;

		// (not past where it leaves the ground for a jump: that one it
		// stands on)
		if (dx * dx + dz * dz > ahead * ahead * 0.25f || actor->path_jump[actor->path_index + 1] != 0
			|| !body_way_clear(actor, navigation, terrain, water, &agent, actor->path[actor->path_index + 1][0],
				actor->path[actor->path_index + 1][1]))
		{
			break;
		}
		actor->path_index++;
	}
	actor->steer_index = actor->path_index;
	actor->steer_x = actor->path[actor->path_index][0];
	actor->steer_z = actor->path[actor->path_index][1];
	actor->steer_blocked = false;
	// Ahead along the path from where it is: past its waypoint as far as
	// is left
	{
		float from_x = actor->x;
		float from_z = actor->z;
		float left = ahead;
		float point_x = from_x;
		float point_z = from_z;

		for (int k = actor->path_index; k < actor->path_count && left > 0.0f; k++)
		{
			float dx = actor->path[k][0] - from_x;
			float dz = actor->path[k][1] - from_z;
			float length = SDL_sqrtf(dx * dx + dz * dz);
			float amount = length > left ? left / length : 1.0f;

			// (no further than where it leaves the ground for a jump)
			if (k > actor->path_index && actor->path_jump[k] != 0)
			{
				break;
			}
			point_x = from_x + dx * amount;
			point_z = from_z + dz * amount;
			left -= length;
			from_x = actor->path[k][0];
			from_z = actor->path[k][1];
		}
		// (only past its waypoint: short of it, the waypoint is on the way
		// anyway)
		// (and not round a corner so sharp that the point is as good as
		// under its own feet, its way doubling back by it: it turned
		// after that point round and round, the waypoint never reached)
		if (left < ahead - 1e-3f
			&& (point_x - actor->x) * (point_x - actor->x) + (point_z - actor->z) * (point_z - actor->z) >= STEER_DIRECT_SHARE * STEER_DIRECT_SHARE * ahead * ahead
			&& body_way_clear(actor, navigation, terrain, water, &agent, point_x, point_z))
		{
			float to_waypoint_x = actor->path[actor->path_index][0] - actor->x;
			float to_waypoint_z = actor->path[actor->path_index][1] - actor->z;

			if (to_waypoint_x * to_waypoint_x + to_waypoint_z * to_waypoint_z < ahead * ahead)
			{
				actor->steer_x = point_x;
				actor->steer_z = point_z;
			}
		}
	}
	// (its waypoint itself: whether its body has the straight way to it)
	if (!actor->swimming && actor->steer_x == actor->path[actor->path_index][0] && actor->steer_z == actor->path[actor->path_index][1])
	{
		float dx = actor->steer_x - actor->x;
		float dz = actor->steer_z - actor->z;
		float distance = SDL_sqrtf(dx * dx + dz * dz);
		float share = distance > BODY_WAY_METERS ? BODY_WAY_METERS / distance : 1.0f;

		actor->steer_blocked = !navigation_body_line(navigation, terrain, water, &agent, actor->x, actor->z, actor->center_ground,
			actor->x + dx * share, actor->z + dz * share, BODY_BRUSHES);
	}
	*target_x = actor->steer_x;
	*target_z = actor->steer_z;
}

static bool look_ahead(actors_t *actors, actor_t *actor, navigation_t *navigation, const terrain_t *terrain, const water_t *water)
{
	const actor_type_t *type = &actors->types[actor->type];
	navigation_agent_t agent = agent_of(type);
	float points[NAVIGATION_MAX_WAYPOINTS + 1][2];
	int point_count = 1;
	float left = type->vision_meters;
	float minimum_x = actor->x;
	float minimum_z = actor->z;
	float maximum_x = actor->x;
	float maximum_z = actor->z;
	bool blocked = false;

	points[0][0] = actor->x;
	points[0][1] = actor->z;
	for (int k = actor->path_index; k < actor->path_count && left > 0.0f; k++)
	{
		float dx = actor->path[k][0] - points[point_count - 1][0];
		float dz = actor->path[k][1] - points[point_count - 1][1];
		float length = SDL_sqrtf(dx * dx + dz * dz);
		float amount = length > left ? left / length : 1.0f;

		points[point_count][0] = points[point_count - 1][0] + dx * amount;
		points[point_count][1] = points[point_count - 1][1] + dz * amount;
		minimum_x = SDL_min(minimum_x, points[point_count][0]);
		minimum_z = SDL_min(minimum_z, points[point_count][1]);
		maximum_x = SDL_max(maximum_x, points[point_count][0]);
		maximum_z = SDL_max(maximum_z, points[point_count][1]);
		point_count++;
		left -= length;
	}
	if (!navigation_changed_since(navigation, water, minimum_x, minimum_z, maximum_x, maximum_z, actor->path_generation))
	{
		return true;
	}
	actor->path_generation = navigation->generation;
	// From where it is, along the path: one line, so the surfaces carry on
	// from each to the next (on a bridge, it stays on the bridge)
	{
		float height = actor->y;
		float blocked_x = 0.0f;
		float blocked_z = 0.0f;
		float blocked_height = 0.0f;

		for (int k = 1; k < point_count && !blocked; k++)
		{
			blocked = !navigation_line_passable(navigation, terrain, water, &agent, points[k - 1][0], points[k - 1][1], height,
				points[k][0], points[k][1], &blocked_x, &blocked_z, &blocked_height);
			height = navigation_surface_height(navigation, terrain, &agent, points[k][0], points[k][1], height);
		}
		if (!blocked)
		{
			return true;
		}
		if (actors->log_paths)
		{
			SDL_Log("Actor %u (%s): the path ahead is blocked", actor->id, type->name);
			log_point(actors, actor, navigation, terrain, water, "blocked at", blocked_x, blocked_z, blocked_height);
		}
	}
	return plan_path(actors, actor, type, navigation, terrain, water, "blocked ahead, searching again");
}

static float wrap_angle(float radians)
{
	while (radians > MATH3D_PI)
	{
		radians -= 2.0f * MATH3D_PI;
	}
	while (radians < -MATH3D_PI)
	{
		radians += 2.0f * MATH3D_PI;
	}
	return radians;
}

// Turning on the spot slower than this (radians a second) is standing
#define TURN_ON_SPOT_RADIANS 0.2f
#define TURN_START_RADIANS 0.5f       // its turn's steps start at this many a second (a few degrees' turn: none), go on down to the one above

static float body_radius(const actors_t *actors, const actor_t *actor);

// Whether an action is one of its walking ones: the walk, the idle, the
// turns on the spot (the others the game plays are left alone)
static bool walking_action(const actor_type_t *type, int action)
{
	return action == type->walk_action || action == type->idle_action || (action >= 0 && (action == type->turn_left_action
		|| action == type->turn_right_action || action == type->walk_left_action || action == type->walk_right_action));
}

// One of its walks: straight on, or round a curve
static bool walk_group_action(const actor_type_t *type, int action)
{
	return action >= 0 && (action == type->walk_action || action == type->walk_left_action || action == type->walk_right_action);
}

// Into one of its walks; from another of them at the same point of its
// step (they are made alike: the crossfade then only bends the curve, no
// legs crossed over)
static void start_walk(const actors_t *actors, actor_t *actor, int action)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);
	float phase = -1.0f;

	if (action == actor->action)
	{
		return;
	}
	if (walk_group_action(type, actor->action) && walk_group_action(type, action))
	{
		float length = action_length(model, actor->action);

		phase = length > 0.0f ? SDL_fmodf(actor->action_seconds, length) / length : 0.0f;
		phase = phase < 0.0f ? phase + 1.0f : phase;
	}
	start_action(actors, actor, action);
	if (phase >= 0.0f)
	{
		actor->action_seconds = phase * action_length(model, action);
	}
}

// How fast it turns walking round its curve at this speed (radians a
// second): walk_turn_degrees a cycle of the walk
static float walk_turn_radians_per_second(const actors_t *actors, const actor_t *actor, float speed)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);
	float cycle_meters = type->walk_cycle_meters;

	if (cycle_meters <= 0.0f)
	{
		cycle_meters = type->walk_meters_per_second * (type->walk_action >= 0 ? action_length(model, type->walk_action) : 1.0f);
	}
	return degrees_to_radians(type->walk_turn_degrees) * SDL_fabsf(speed) / SDL_max(cycle_meters, 0.05f);
}

// Walk (or swim) animation at the pace of its speed; turning on the spot,
// its turn (or the walk at its feet's pace round it); idle when it stands
static void choose_animation(actors_t *actors, actor_t *actor)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);
	// (walking already: on till it is all but still, no walk-idle-walk
	// flicker at the edge; round a curve the walk is bent: walk_curve)
	bool going = actor->speed > (walk_group_action(type, actor->action) ? 0.02f : 0.05f);
	float turning = SDL_fabsf(actor->turn_rate);
	float clip_seconds;
	float pace;

	if (type->walk_action < 0 || is_dying(actors, actor) || actor->gait >= 0 || actor->animation_held)
	{
		return;
	}
	// (a flinch plays out; then idle, and the walk again if it goes on)
	if (actor->action >= 0 && !walking_action(type, actor->action) && (model->actions[actor->action].flags & ACTOR_ACTION_LOOP) == 0)
	{
		return;
	}
	clip_seconds = action_length(model, type->walk_action);
	pace = type->walk_cycle_meters > 0.0f ? type->walk_cycle_meters / clip_seconds : type->walk_meters_per_second;
	if (actor->backing_meters > 0.0f && actor->speed < -0.02f)
	{
		// Stepping back: the walk backwards
		start_walk(actors, actor, type->walk_action);
		actor->action_rate = -SDL_clamp(-actor->speed / pace, 0.3f, 2.0f);
	}
	else if (going || actor->swimming)
	{
		start_walk(actors, actor, type->walk_action);
		// Treading water when it is not going anywhere
		// (its stride stretched to end on a foot: the feet so much slower;
		// at least so fast, so a start does not glide, but not while it
		// slows to a stop: its last steps as slow as it goes, no stepping
		// in place)
		bool stopping = (!actor->moving && !actor->wandering) || (actor->moving && actor->path_index == actor->path_count - 1);
		float slowest = stopping ? 0.05f : 0.3f;

		actor->action_rate = going ? SDL_clamp(actor->speed / (pace * actor->stride_scale), slowest, 3.0f) : 0.5f;
	}
	else if (turning > (actor->action >= 0 && (actor->action == type->turn_left_action || actor->action == type->turn_right_action) ? TURN_ON_SPOT_RADIANS
			: TURN_START_RADIANS)
		&& (actor->action < 0 || walking_action(type, actor->action)))
	{
		int turn_action = actor->turn_rate > 0.0f ? type->turn_left_action : type->turn_right_action;

		if (turn_action >= 0)
		{
			// (its own turn, at the pace it turns: played as made at the
			// type's turning speed)
			start_action(actors, actor, turn_action);
			actor->action_rate = SDL_clamp(turning / degrees_to_radians(type->turn_degrees_per_second), 0.3f, 2.0f);
		}
		else
		{
			// The walk, its feet going round the body's middle: a four
			// legged one steps round on the spot
			start_action(actors, actor, type->walk_action);
			actor->action_rate = SDL_clamp(turning * body_radius(actors, actor) / pace, 0.3f, 1.5f);
		}
	}
	else if (actor->action >= 0 && actor->action != type->idle_action && walking_action(type, actor->action) && type->idle_action >= 0)
	{
		start_action(actors, actor, type->idle_action);
	}
}

// How far round itself another is kept off: its body's size, the larger
// of across and along (a hen is longer than wide), a little in (a circle
// round a longish body)
static float body_radius(const actors_t *actors, const actor_t *actor)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);
	float along = (model->bounds_maximum.z - model->bounds_minimum.z) * type->scale;

	return 0.4f * SDL_max(type->width_meters, along);
}

#define STRAIGHT_NEAR_END_METERS 0.8f   // stopped this near the end of a straight walk: got there

// Whether a body stands just ahead of a creature walking straight: a line
// from it, as far as its step and its body's reach, low (its feet) and at
// its middle; the ground does not count (a slope is walked up)
static bool straight_blocked(const actors_t *actors, const actor_t *actor, float step_x, float step_z)
{
	const actor_type_t *type = &actors->types[actor->type];
	float length = SDL_sqrtf(step_x * step_x + step_z * step_z);
	// (out to its beak: its body's reach, the larger of across and along)
	float reach = length + body_radius(actors, actor) * 1.25f + 0.03f;
	// (people step up what is lower than a stride, a stair: looked for over it)
	float low = type->ground_align <= 0.0f ? type->step_meters + 0.03f : SDL_min(0.08f, 0.3f * type->height_meters);
	float heights[2] = { low, 0.6f * type->height_meters };

	if (actors->physics == NULL || length <= 0.0f)
	{
		return false;
	}
	for (int k = 0; k < 2; k++)
	{
		float from[3] = { actor->x, actor->y + heights[k], actor->z };
		float to[3] = { actor->x + step_x / length * reach, actor->y + heights[k], actor->z + step_z / length * reach };
		physics_cast_t hit;

		if (physics_cast_segment(actors->physics, from, to, &hit) && hit.body >= 0)
		{
			return true;
		}
	}
	return false;
}

#define BIG_BODY_HALF_METERS 0.9f     // a body longer than this each side of its middle swings wide turning

// Whether it is big enough that turning on the spot swings its body out
// past what its line ahead saw (a cow; not a sheep or a pig, which in a
// strunga or a sty would only be kept from turning)
static bool big_body(const actors_t *actors, const actor_t *actor)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);

	return SDL_max(-model->bounds_minimum.z, model->bounds_maximum.z) * type->scale > BIG_BODY_HALF_METERS;
}

// Whether its whole body fits standing at (x, z) turned to yaw: lines
// from its middle out to its four corners (its bounds a little in, as
// scaled), at its feet and at its middle, hit no body. A big one (a cow)
// turning on the spot swings its rump through what the line ahead never
// saw (a hay rack), and its legs then climb it
static bool body_fits(const actors_t *actors, const actor_t *actor, float x, float z, float yaw)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);
	float heights[2] = { SDL_min(0.08f, 0.3f * type->height_meters), 0.6f * type->height_meters };
	float across[2] = { model->bounds_minimum.x * type->scale * 0.85f, model->bounds_maximum.x * type->scale * 0.85f };
	float along[2] = { model->bounds_minimum.z * type->scale * 0.9f, model->bounds_maximum.z * type->scale * 0.9f };
	float c = SDL_cosf(yaw);
	float s = SDL_sinf(yaw);

	if (actors->physics == NULL)
	{
		return true;
	}
	for (int corner = 0; corner < 4; corner++)
	{
		float u = across[corner & 1];
		float v = along[corner >> 1];
		// (its own x to the world: right is (cos, -sin), ahead (sin, cos))
		float corner_x = x + u * c + v * s;
		float corner_z = z - u * s + v * c;

		for (int k = 0; k < 2; k++)
		{
			float from[3] = { x, actor->y + heights[k], z };
			float to[3] = { corner_x, actor->y + heights[k], corner_z };
			physics_cast_t hit;

			if (physics_cast_segment(actors->physics, from, to, &hit) && hit.body >= 0)
			{
				return false;
			}
		}
	}
	return true;
}

// Whether another one on the ground stands in the way of a creature
// walking straight: ahead of it within a little more than their bodies'
// reach, and not off to the side past them (it goes elsewhere rather than
// shove it: the shoved one sliding where it stands looks wrong)
static bool straight_actor_ahead(const actors_t *actors, const actor_t *actor, float step_x, float step_z)
{
	float length = SDL_sqrtf(step_x * step_x + step_z * step_z);
	float own = body_radius(actors, actor);
	float direction_x;
	float direction_z;

	if (length <= 0.0f)
	{
		return false;
	}
	direction_x = step_x / length;
	direction_z = step_z / length;
	for (int i = 0; i < actors->count; i++)
	{
		const actor_t *other = &actors->actors[i];
		float reach;
		float dx;
		float dz;
		float along;
		float aside;

		if (other == actor || actors->types[other->type].flies || is_dying(actors, other) || SDL_fabsf(other->y - actor->y) > 1.0f)
		{
			continue;
		}
		reach = own + body_radius(actors, other);
		dx = other->x - actor->x;
		dz = other->z - actor->z;
		along = dx * direction_x + dz * direction_z;
		aside = SDL_fabsf(dx * direction_z - dz * direction_x);
		if (along > 0.0f && along < reach + length + 0.1f && aside < reach * 0.9f)
		{
			return true;
		}
	}
	return false;
}

#define STEER_STEP_METERS 0.3f         // how far ahead a heading is tried (on top of its body's reach)
#define STEER_ANGLE_RADIANS 0.436f     // the headings tried: this apart (25 degrees), each side in turn
#define STEER_TRIES 5                  // so many each side (out to 125 degrees)
#define STEER_KEEP_SECONDS 1.5f        // the side it goes round on, kept this long
#define CROWD_WAIT_SECONDS 3.0f        // nothing free but others in the way: it waits this long
#define BACK_TURN_RADIANS 0.87f        // its way this far round (50 degrees) and its nose against something: back out first
#define BACK_NOSE_METERS 0.1f          // against something: a body this near past its reach ahead
#define BACK_CLEAR_METERS 0.35f        // clear: nothing this far past its reach ahead
#define BACK_MOST_METERS 1.0f          // backing out, at most so far
#define BACK_SPEED_SHARE 0.35f         // backing out at this share of its walk

// Whether a heading is free for a straight walker (no body, no other just
// ahead along it); by_body_out: what was in the way, if anything
static bool heading_free(const actors_t *actors, const actor_t *actor, float heading, bool *by_body_out)
{
	float step_x = SDL_sinf(heading) * STEER_STEP_METERS;
	float step_z = SDL_cosf(heading) * STEER_STEP_METERS;

	if (straight_blocked(actors, actor, step_x, step_z))
	{
		*by_body_out = true;
		return false;
	}
	if (straight_actor_ahead(actors, actor, step_x, step_z))
	{
		return false;
	}
	return true;
}

// The heading a straight walker takes towards where it goes: that one if
// it is free, else the nearest free one round it, on the side it took
// last if it still goes round something (no swinging from side to side);
// false if none is free (all_bodies_out: only bodies were in the way, no
// others it could wait for)
static bool steer(const actors_t *actors, actor_t *actor, float wanted, float seconds, float *heading_out, bool *all_bodies_out)
{
	bool by_body = false;
	bool any_other = false;

	actor->avoid_seconds = SDL_max(actor->avoid_seconds - seconds, 0.0f);
	if (actor->avoid_side == 0)
	{
		actor->avoid_side = (actor->id & 1u) != 0 ? 1 : -1;
	}
	*heading_out = wanted;
	if (heading_free(actors, actor, wanted, &by_body))
	{
		return true;
	}
	any_other = any_other || !by_body;
	for (int k = 1; k <= STEER_TRIES; k++)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			// (going round something: its side kept, the other only after)
			int side = pass == 0 ? actor->avoid_side : -actor->avoid_side;
			float heading = wanted + (float)side * (float)k * STEER_ANGLE_RADIANS;

			if (pass == 1 && actor->avoid_seconds > 0.0f)
			{
				continue;
			}
			by_body = false;
			if (heading_free(actors, actor, heading, &by_body))
			{
				actor->avoid_side = side;
				actor->avoid_seconds = STEER_KEEP_SECONDS;
				*heading_out = heading;
				return true;
			}
			any_other = any_other || !by_body;
		}
	}
	// (its side gave nothing: the other side, before it gives up)
	if (actor->avoid_seconds > 0.0f)
	{
		actor->avoid_seconds = 0.0f;
		actor->avoid_side = -actor->avoid_side;
	}
	*all_bodies_out = !any_other;
	return false;
}

// ---------------------------------------------------------------------------
// Wandering (actors_wander): free ahead, and the step itself

#define WANDER_PREDICT_SECONDS 0.5f    // another that goes: where it will be in this long counts too
#define WANDER_SIDE_SHARE 0.85f        // its sides' lines: this share of its half width out
#define WANDER_HOLD_SECONDS 0.5f       // its step blocked: it stands this long before it tries again

// Whether a point (another's middle, of radius) is across the stretch
// from (x, z) along (direction_x, direction_z), length long, half_width
// each side: ahead of it only (one behind or beside its middle is not in
// its way)
static bool across_stretch(float x, float z, float direction_x, float direction_z, float length, float half_width, float other_x, float other_z,
	float radius)
{
	float dx = other_x - x;
	float dz = other_z - z;
	float along = dx * direction_x + dz * direction_z;
	float aside = SDL_fabsf(dx * direction_z - dz * direction_x);

	return along > 0.0f && along < length + radius && aside < half_width + radius;
}

static bool heading_free_for(const actors_t *actors, const actor_t *actor, float heading_radians, float meters);

bool actors_heading_free(const actors_t *actors, uint32_t id, float heading_radians, float meters)
{
	int index = actors_find(actors, id);

	return index >= 0 && heading_free_for(actors, &actors->actors[index], heading_radians, meters);
}

static bool heading_free_for(const actors_t *actors, const actor_t *actor, float heading_radians, float meters)
{
	const actor_type_t *type;
	const actor_model_t *model;
	float direction_x = SDL_sinf(heading_radians);
	float direction_z = SDL_cosf(heading_radians);
	float half_width;
	float nose;
	float length;
	float heights[2];

	type = &actors->types[actor->type];
	model = model_of(actors, actor);
	half_width = 0.5f * (model->bounds_maximum.x - model->bounds_minimum.x) * type->scale;
	// (its nose: the far end of its bounds the way it goes, ahead or back)
	nose = SDL_max(model->bounds_maximum.z, -model->bounds_minimum.z) * type->scale;
	length = nose + SDL_max(meters, 0.0f);
	heights[0] = type->ground_align <= 0.0f ? type->step_meters + 0.03f : SDL_min(0.08f, 0.3f * type->height_meters);
	heights[1] = 0.6f * type->height_meters;
	// Bodies: lines along its middle and both its sides, at its feet and
	// at its middle (one line slips between two fence posts)
	if (actors->physics != NULL)
	{
		for (int side = -1; side <= 1; side++)
		{
			float offset = (float)side * half_width * WANDER_SIDE_SHARE;
			// (its side: right of the way is (cos, -sin) of the heading)
			float start_x = actor->x + direction_z * offset;
			float start_z = actor->z - direction_x * offset;

			for (int k = 0; k < 2; k++)
			{
				float from[3] = { start_x, actor->y + heights[k], start_z };
				float to[3] = { start_x + direction_x * length, actor->y + heights[k], start_z + direction_z * length };
				physics_cast_t hit;

				if (physics_cast_segment(actors->physics, from, to, &hit) && hit.body >= 0)
				{
					return false;
				}
			}
		}
	}
	// Others on the ground: where they are, and where they will be
	for (int i = 0; i < actors->count; i++)
	{
		const actor_t *other = &actors->actors[i];
		float radius;
		float later_x;
		float later_z;

		if (other == actor || actors->types[other->type].flies || is_dying(actors, other) || SDL_fabsf(other->y - actor->y) > 1.0f)
		{
			continue;
		}
		radius = body_radius(actors, other);
		later_x = other->x + SDL_sinf(other->yaw_radians) * other->speed * WANDER_PREDICT_SECONDS;
		later_z = other->z + SDL_cosf(other->yaw_radians) * other->speed * WANDER_PREDICT_SECONDS;
		if (across_stretch(actor->x, actor->z, direction_x, direction_z, length, half_width, other->x, other->z, radius)
			|| across_stretch(actor->x, actor->z, direction_x, direction_z, length, half_width, later_x, later_z, radius))
		{
			return false;
		}
	}
	return true;
}

// A wanderer's frame: turned toward its heading at its turning speed (a
// big body's turn it does not fit through not made), on at its share of
// the walk (eased), the step not taken if a body or another is just ahead
// of it; its speed for choose_animation (below 0: backing)
static void wander_move(actors_t *actors, actor_t *actor, const navigation_t *navigation, const terrain_t *terrain, float seconds)
{
	const actor_type_t *type = &actors->types[actor->type];
	navigation_agent_t agent = agent_of(type);
	float map_meters = (float)terrain->size * terrain->cell_size_meters;
	float turn = wrap_angle(actor->wander_heading - actor->yaw_radians);
	// (walking: round its own curve, as its walk_left / walk_right turn;
	// standing: on the spot at its turning speed, its turn clips)
	float most = (SDL_fabsf(actor->speed) > 0.05f ? SDL_min(walk_turn_radians_per_second(actors, actor, actor->speed),
		degrees_to_radians(type->turn_degrees_per_second)) : degrees_to_radians(type->turn_degrees_per_second)) * seconds;
	float new_yaw = wrap_angle(actor->yaw_radians + inertial_turn(actor, turn, most / seconds, seconds));
	float wanted_speed;
	float step_x;
	float step_z;

	if (big_body(actors, actor) && !body_fits(actors, actor, actor->x, actor->z, new_yaw) && body_fits(actors, actor, actor->x, actor->z, actor->yaw_radians))
	{
		new_yaw = actor->yaw_radians;
		actor->angular_speed = 0.0f;
	}
	actor->yaw_radians = new_yaw;
	actor->spot_yaw_radians = new_yaw;
	turn = wrap_angle(actor->wander_heading - actor->yaw_radians);
	if (actor->wander_share >= 0.0f)
	{
		float ahead_x = actor->x + SDL_sinf(actor->yaw_radians) * 0.5f;
		float ahead_z = actor->z + SDL_cosf(actor->yaw_radians) * 0.5f;
		float rise = (navigation_surface_height(navigation, terrain, &agent, ahead_x, ahead_z, actor->y)
			- navigation_surface_height(navigation, terrain, &agent, actor->x, actor->z, actor->y)) / 0.5f;

		// (turning too: its walk kept, round an arc; slowed in a turn it
		// looked odd, a mind's turns are small)
		wanted_speed = actor->wander_share * type->walk_meters_per_second;
		wanted_speed /= 1.0f + 2.0f * SDL_max(rise, 0.0f);
		actor->backing_meters = 0.0f;
	}
	else
	{
		wanted_speed = actor->wander_share * type->walk_meters_per_second;
		// (choose_animation's walk backwards)
		actor->backing_meters = 1.0f;
	}
	if (actor->wander_hold_seconds > 0.0f)
	{
		actor->wander_hold_seconds -= seconds;
		wanted_speed = 0.0f;
	}
	// (a one-shot of its own playing: still, as move_actor holds it)
	if (actor->action >= 0 && !walking_action(type, actor->action) && (model_of(actors, actor)->actions[actor->action].flags & ACTOR_ACTION_LOOP) == 0)
	{
		wanted_speed = 0.0f;
	}
	if (actor->speed < wanted_speed)
	{
		actor->speed = SDL_min(actor->speed + ACCELERATION * seconds, wanted_speed);
	}
	else
	{
		actor->speed = SDL_max(actor->speed - 2.0f * ACCELERATION * seconds, wanted_speed);
	}
	step_x = SDL_sinf(actor->yaw_radians) * actor->speed * seconds;
	step_z = SDL_cosf(actor->yaw_radians) * actor->speed * seconds;
	// (the same look as its mind's (actors_heading_free), just past its
	// nose: a stricter one here walked it in place where its mind saw the
	// way free)
	if ((step_x != 0.0f || step_z != 0.0f)
		&& !heading_free_for(actors, actor, actor->speed >= 0.0f ? actor->yaw_radians : actor->yaw_radians + 3.14159265f,
			SDL_fabsf(actor->speed) * seconds + 0.02f))
	{
		// (it stands a while before it tries again: not a start and a stop
		// every frame against what is at its nose)
		actor->wander_hold_seconds = WANDER_HOLD_SECONDS;
		step_x = 0.0f;
		step_z = 0.0f;
		actor->speed = 0.0f;
	}
	actor->x = SDL_clamp(actor->x + step_x, 0.5f, map_meters - 0.5f);
	actor->z = SDL_clamp(actor->z + step_z, 0.5f, map_meters - 0.5f);
}

// ---------------------------------------------------------------------------
// Steps between standing and walking (people's: their models' walk_start,
// walk_start_180, walk_stop_l / _r, step_left / _right / _back, shuffle):
// each clip moves it as far as it was made to, eased as its feet go, the
// walk's stride stretched a little before a stop so the stop begins as a
// foot lands just its length from the goal

// A clip started at once, no crossfade (its first pose the one before)
static void start_action_now(const actors_t *actors, actor_t *actor, int action, float seconds_into)
{
	(void)actors;
	actor->previous_action = -1;
	actor->blend_left_seconds = 0.0f;
	actor->action = action;
	actor->action_seconds = seconds_into;
	actor->action_rate = 1.0f;
}

// How far along its way a step is (0..1) at t of its length: a start
// gathering pace to the walk's, a stop losing it
static float gait_share(int gait, float t)
{
	if (gait == ACTOR_GAIT_START || gait == ACTOR_GAIT_START_180)
	{
		return t * t * (1.5f - 0.5f * t);
	}
	return 1.0f - (1.0f - t) * (1.0f - t);
}

static void begin_gait(actors_t *actors, actor_t *actor, int gait, float meters, float yaw, float turn_after)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);

	actor->gait = gait;
	actor->gait_seconds = 0.0f;
	actor->gait_length_seconds = SDL_max(action_length(model, type->gait_actions[gait]), 0.05f);
	actor->gait_meters = meters;
	actor->gait_done_meters = 0.0f;
	actor->gait_yaw = yaw;
	actor->gait_delay = gait == ACTOR_GAIT_START_180 ? SDL_clamp(type->walk_start_180_delay, 0.0f, 0.9f) : 0.0f;
	actor->gait_turn_after = turn_after;
	actor->speed = 0.0f;
	start_action_now(actors, actor, type->gait_actions[gait], 0.0f);
	if (actors->log_paths)
	{
		SDL_Log("Actor %u (%s): %s, %.2f m%s", actor->id, type->name, model_of(actors, actor)->actions[type->gait_actions[gait]].name, (double)meters,
			actor->stride_scale != 1.0f ? " (its stride stretched)" : "");
	}
}

// The walk's pace, meters a second, as made
static float walk_pace(const actors_t *actors, const actor_t *actor)
{
	const actor_type_t *type = &actors->types[actor->type];
	float clip_seconds = SDL_max(action_length(model_of(actors, actor), type->walk_action), 0.05f);

	return type->walk_cycle_meters > 0.0f ? type->walk_cycle_meters / clip_seconds : type->walk_meters_per_second;
}

// A step under way: moved by it this frame (true), done at its end
static bool gait_move(actors_t *actors, actor_t *actor, float seconds)
{
	const actor_type_t *type = &actors->types[actor->type];
	float t;
	float u;
	float wanted;
	float step;

	if (actor->gait < 0)
	{
		return false;
	}
	actor->gait_seconds += seconds;
	t = SDL_clamp(actor->gait_seconds / actor->gait_length_seconds, 0.0f, 1.0f);
	u = actor->gait_delay > 0.0f ? SDL_clamp((t - actor->gait_delay) / (1.0f - actor->gait_delay), 0.0f, 1.0f) : t;
	wanted = actor->gait_meters * gait_share(actor->gait, u);
	step = wanted - actor->gait_done_meters;
	actor->x += SDL_sinf(actor->gait_yaw) * step;
	actor->z += SDL_cosf(actor->gait_yaw) * step;
	actor->gait_done_meters = wanted;
	actor->speed = 0.0f;
	if (t < 1.0f)
	{
		return true;
	}
	// Done: walking on from the walk's start (as the start ends), or there
	if (actor->gait == ACTOR_GAIT_START || actor->gait == ACTOR_GAIT_START_180)
	{
		actor->yaw_radians = wrap_angle(actor->yaw_radians + actor->gait_turn_after);
		actor->gait = -1;
		if (type->walk_action >= 0)
		{
			start_action_now(actors, actor, type->walk_action, 0.0f);
		}
		actor->speed = walk_pace(actors, actor);
		actor->walk_phase_last = 0.0f;
		return true;
	}
	actor->gait = -1;
	if (actor->moving && actor->path_count > 0)
	{
		actor->x = actor->path[actor->path_count - 1][0];
		actor->z = actor->path[actor->path_count - 1][1];
	}
	stop_moving(actor);
	actor->stride_scale = 1.0f;
	push_event(actors, ACTOR_EVENT_ARRIVED, actor);
	if (actor->arrive_face)
	{
		actor->facing = true;
		actor->face_x = actor->x + SDL_sinf(actor->arrive_yaw) * 2.0f;
		actor->face_z = actor->z + SDL_cosf(actor->arrive_yaw) * 2.0f;
	}
	if (type->idle_action >= 0)
	{
		start_action(actors, actor, type->idle_action);
	}
	return true;
}

// Off from standing: a start clip straight on (facing near enough the
// way), or turning about (the way behind it); between, it turns on the
// spot first (true: held still for that). Only for a walk long enough to
// start and stop in
static bool gait_start(actors_t *actors, actor_t *actor)
{
	const actor_type_t *type = &actors->types[actor->type];
	float dx;
	float dz;
	float off;
	float length = 0.0f;
	float from_x = actor->x;
	float from_z = actor->z;
	bool can_start = type->gait_actions[ACTOR_GAIT_START] >= 0;
	bool can_turn_about = type->gait_actions[ACTOR_GAIT_START_180] >= 0;

	if (actor->speed > 0.05f)
	{
		actor->start_held_seconds = 0.0f;
	}
	if (!can_start || actor->speed > 0.05f || actor->backing_meters > 0.0f || actor->swimming
		|| (actor->action >= 0 && actor->action == type->walk_action))
	{
		return false;
	}
	for (int k = actor->path_index; k < actor->path_count; k++)
	{
		length += SDL_sqrtf((actor->path[k][0] - from_x) * (actor->path[k][0] - from_x) + (actor->path[k][1] - from_z) * (actor->path[k][1] - from_z));
		from_x = actor->path[k][0];
		from_z = actor->path[k][1];
	}
	dx = actor->path[actor->path_index][0] - actor->x;
	dz = actor->path[actor->path_index][1] - actor->z;
	off = wrap_angle(SDL_atan2f(dx, dz) - actor->yaw_radians);
	if (can_turn_about && SDL_fabsf(off) > GAIT_TURN_ABOUT_RADIANS && length > type->walk_start_180_meters + type->walk_stop_meters + GAIT_START_MORE_METERS)
	{
		begin_gait(actors, actor, ACTOR_GAIT_START_180, type->walk_start_180_meters, wrap_angle(actor->yaw_radians + 3.14159265f), 3.14159265f);
		return true;
	}
	if (length < type->walk_start_meters + type->walk_stop_meters + GAIT_START_MORE_METERS)
	{
		return false;
	}
	if (SDL_fabsf(off) < GAIT_START_RADIANS)
	{
		begin_gait(actors, actor, ACTOR_GAIT_START, type->walk_start_meters, actor->yaw_radians, 0.0f);
		return true;
	}
	// (turned on the spot till it faces near enough; turning so too long,
	// its way swinging about: off as any walk)
	if (actor->start_held_seconds > GAIT_HOLD_MOST_SECONDS)
	{
		return false;
	}
	actor->start_held = true;
	return false;
}

// Walking to an exact goal: the stride stretched (or shortened) a little so
// a foot lands just a stop's length from it, and the stop begun as it
// does; true when it began
static bool gait_plan_stop(actors_t *actors, actor_t *actor)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);
	float clip_seconds;
	float cycle;
	float phase;
	float last;
	float dx;
	float dz;
	float distance;
	const float feet[2] = { type->walk_stop_left_phase, type->walk_stop_right_phase };
	const int stops[2] = { ACTOR_GAIT_STOP_LEFT, ACTOR_GAIT_STOP_RIGHT };

	if (actor->arrive_meters <= 0.0f || actor->path_index < actor->path_count - 2 || type->walk_action < 0 || actor->action != type->walk_action
		|| actor->speed < 0.3f)
	{
		actor->stride_scale = 1.0f;
		return false;
	}
	clip_seconds = SDL_max(action_length(model, type->walk_action), 0.05f);
	cycle = type->walk_cycle_meters > 0.0f ? type->walk_cycle_meters : type->walk_meters_per_second * clip_seconds;
	phase = SDL_fmodf(actor->action_seconds / clip_seconds, 1.0f);
	last = actor->walk_phase_last;
	actor->walk_phase_last = phase;
	dx = actor->path[actor->path_count - 1][0] - actor->x;
	dz = actor->path[actor->path_count - 1][1] - actor->z;
	// (along what is left of its way: by a point before the goal too)
	distance = 0.0f;
	{
		float from_x = actor->x;
		float from_z = actor->z;

		for (int k = actor->path_index; k < actor->path_count; k++)
		{
			distance += SDL_sqrtf((actor->path[k][0] - from_x) * (actor->path[k][0] - from_x) + (actor->path[k][1] - from_z) * (actor->path[k][1] - from_z));
			from_x = actor->path[k][0];
			from_z = actor->path[k][1];
		}
	}
	// A foot landing now, the goal about a stop away (nearer it than the
	// next foot's landing would leave it): stopping on it
	for (int f = 0; f < 2; f++)
	{
		bool crossed = phase >= last ? (last < feet[f] && feet[f] <= phase) : (feet[f] > last || feet[f] <= phase);
		float next_left = distance - SDL_fabsf(feet[1 - f] - feet[f]) * cycle * actor->stride_scale;
		bool best_now = SDL_fabsf(distance - type->walk_stop_meters) <= SDL_fabsf(next_left - type->walk_stop_meters) || next_left < type->walk_stop_meters * 0.45f;

		if (crossed && best_now && type->gait_actions[stops[f]] >= 0 && distance > type->walk_stop_meters * 0.45f && distance < type->walk_stop_meters * 2.2f)
		{
			distance = SDL_sqrtf(dx * dx + dz * dz);
			float heading = SDL_atan2f(dx, dz);

			if (SDL_fabsf(wrap_angle(heading - actor->yaw_radians)) < 0.35f)
			{
				actor->yaw_radians = heading;
			}
			begin_gait(actors, actor, stops[f], distance, heading, 0.0f);
			actor->stride_scale = 1.0f;
			return true;
		}
	}
	// Nearing: the landing that leaves the stop's length best, and the
	// stride stretched to it
	actor->stride_scale = 1.0f;
	if (distance < GAIT_PLAN_METERS)
	{
		float best = 1e30f;
		float best_meters = 0.0f;

		for (int k = 0; k < 4; k++)
		{
			for (int f = 0; f < 2; f++)
			{
				float ahead = SDL_fmodf(feet[f] - phase + 2.0f, 1.0f) + (float)k;
				float meters = ahead * cycle;
				float left = distance - meters;

				if (type->gait_actions[stops[f]] < 0 || meters < 0.05f || left < type->walk_stop_meters * 0.45f)
				{
					continue;
				}
				if (SDL_fabsf(left - type->walk_stop_meters) < best)
				{
					best = SDL_fabsf(left - type->walk_stop_meters);
					best_meters = meters;
				}
			}
		}
		if (best_meters > 0.0f)
		{
			actor->stride_scale = SDL_clamp((distance - type->walk_stop_meters) / best_meters, GAIT_STRIDE_LEAST, GAIT_STRIDE_MOST);
		}
		if (SDL_getenv("POTOP_DEBUG_GAIT") != NULL)
		{
			SDL_Log("GAIT %u d %.3f phase %.3f last %.3f best_m %.3f scale %.3f speed %.2f rate %.2f", actor->id, (double)distance, (double)phase, (double)last,
				(double)best_meters, (double)actor->stride_scale, (double)actor->speed, (double)actor->action_rate);
		}
	}
	return false;
}

// Whether it is on a fine way (still to the waypoint it was made for)
static bool fine_active(const actor_t *actor)
{
	return actor->fine_index < actor->fine_count && actor->path_index == actor->fine_for && actor->path_index < actor->path_count
		&& actor->path[actor->path_index][0] == actor->fine_for_x && actor->path[actor->path_index][1] == actor->fine_for_z;
}

// No straight way for its body to its waypoint, or its steps refused
// (round a post, a cell's corner; why, for the log): a fine way over the
// quarter meters to its waypoint, or towards it if that is far
static void plan_fine(actors_t *actors, actor_t *actor, const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, const char *why)
{
	float to_x = actor->path[actor->path_index][0];
	float to_z = actor->path[actor->path_index][1];
	float dx = to_x - actor->x;
	float dz = to_z - actor->z;
	float distance = SDL_sqrtf(dx * dx + dz * dz);

	if (distance > FINE_REACH_METERS)
	{
		to_x = actor->x + dx / distance * FINE_REACH_METERS;
		to_z = actor->z + dz / distance * FINE_REACH_METERS;
	}
	actor->fine_count = navigation_fine_path(navigation, terrain, water, agent, actor->x, actor->z, actor->center_ground, to_x, to_z,
		actor->fine_path, ACTOR_FINE_POINTS);
	actor->fine_index = 0;
	actor->fine_for = actor->path_index;
	actor->fine_for_x = actor->path[actor->path_index][0];
	actor->fine_for_z = actor->path[actor->path_index][1];
	// (to the waypoint itself, by it: the clear quarter nearest it, the
	// waypoint a corner of the cells' rectangles, by a post as often as not)
	actor->fine_to_waypoint = false;
	if (actor->fine_count > 0)
	{
		float end_x = actor->fine_path[actor->fine_count - 1][0] - actor->fine_for_x;
		float end_z = actor->fine_path[actor->fine_count - 1][1] - actor->fine_for_z;

		actor->fine_to_waypoint = end_x * end_x + end_z * end_z < FINE_BY_WAYPOINT_METERS * FINE_BY_WAYPOINT_METERS;
	}
	// (none: the quarter meters have no way where the cells' rectangles
	// found one, up a stair's treads too high for it, under a beam too
	// low; none again a little later: it gives up, move_actor)
	actor->fine_failures = actor->fine_count > 0 ? 0 : actor->fine_failures + 1;
	if (actors->log_paths)
	{
		SDL_Log("Actor %u (%s): %s at (%.2f, %.2f), a fine way to (%.2f, %.2f): %d points", actor->id, actors->types[actor->type].name, why,
			(double)actor->x, (double)actor->z, (double)to_x, (double)to_z, actor->fine_count);
	}
}

// Whether a step of a walker on its path keeps to where the path finding
// lets it be: its center's line to the step's end and a radius on is
// passable and the quarter meter there free (inside a cell: a balustrade's
// post on a stair's top); or, its cell one it cannot be in (pushed there),
// the quarter ahead is free: out it goes
static bool step_passable(const navigation_t *navigation, const terrain_t *terrain, const water_t *water, const navigation_agent_t *agent,
	const actor_t *actor, float step_x, float step_z, bool fine)
{
	float length = SDL_sqrtf(step_x * step_x + step_z * step_z);
	// (a radius on ahead; on a fine way, that keeps its body off what is
	// round it already, half a quarter: it squeezes past a post)
	float reach = length + (fine ? 0.125f : agent->radius_meters);
	// (its center's line only, a radius on ahead: its sides brush along
	// what its path keeps close to, a stair's edge)
	navigation_agent_t center = *agent;
	float ahead_x;
	float ahead_z;
	bool ahead_free;

	center.line_center_only = true;
	if (length < 1e-6f)
	{
		return true;
	}
	ahead_x = actor->x + step_x / length * reach;
	ahead_z = actor->z + step_z / length * reach;
	ahead_free = navigation_quarter_free(navigation, terrain, water, &center, ahead_x, ahead_z, actor->center_ground);
	// (and its body room where the step ends: a brush or two of its
	// sides, or no tighter than where it is; not onto the ledge outside a
	// balustrade, a strip the cells' rectangles take in whole)
	if (ahead_free && !fine)
	{
		int tight_after = navigation_quarter_tightness(navigation, terrain, water, agent, actor->x + step_x, actor->z + step_z, actor->center_ground);

		if (tight_after < 0 || (tight_after > BODY_BRUSHES && tight_after > navigation_quarter_tightness(navigation, terrain, water, agent, actor->x,
			actor->z, actor->center_ground)))
		{
			ahead_free = false;
		}
	}
	// (on a fine way the quarters are the judge: the cells' line may see a
	// wall in a gap the cells are too coarse for)
	if (ahead_free && (fine || navigation_line_passable(navigation, terrain, water, &center, actor->x, actor->z, actor->center_ground, ahead_x,
		ahead_z, NULL, NULL, NULL)))
	{
		return true;
	}
	return ahead_free && !navigation_passable(navigation, terrain, water, &center, actor->x, actor->z, actor->center_ground);
}

// Whether, short of its waypoint, it can go straight on to the next one
// from where it is (the corner cut)
static bool corner_cut_passable(const actors_t *actors, const actor_t *actor, const navigation_t *navigation, const terrain_t *terrain,
	const water_t *water)
{
	navigation_agent_t agent = agent_of(&actors->types[actor->type]);

	if (actor->path_index + 1 >= actor->path_count)
	{
		return true;
	}
	return body_way_clear(actor, navigation, terrain, water, &agent, actor->path[actor->path_index + 1][0], actor->path[actor->path_index + 1][1]);
}

// Whether its body has the room for its turn's arc to a point: the
// circle from the way it faces round to the point (its radius the point's
// distance over twice the sine of how far round it is), in two straight
// halves (body_way_clear's measure, the body's alone: the arc's middle is
// off the path's line). Further round than a quarter turn, or a circle
// tighter than ARC_RADIUS_LEAST_METERS (a point half a step off to its
// side: it went round and round it, never onto it): no arc, it turns on
// the spot
static bool turn_arc_clear(const actor_t *actor, const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	const navigation_agent_t *agent, float x, float z)
{
	float dx = x - actor->x;
	float dz = z - actor->z;
	float distance = SDL_sqrtf(dx * dx + dz * dz);
	float turn = wrap_angle(SDL_atan2f(dx, dz) - actor->yaw_radians);
	float radius;
	float middle_x;
	float middle_z;
	float side_x;
	float side_z;

	if (SDL_fabsf(turn) >= MATH3D_PI * 0.5f || distance < 1e-3f)
	{
		return false;
	}
	if (SDL_fabsf(SDL_sinf(turn)) < 1e-3f)
	{
		return true;
	}
	radius = distance / (2.0f * SDL_fabsf(SDL_sinf(turn)));
	if (radius < ARC_RADIUS_LEAST_METERS)
	{
		return false;
	}
	// (the way it turns: its side there)
	side_x = SDL_cosf(actor->yaw_radians) * (turn > 0.0f ? 1.0f : -1.0f);
	side_z = -SDL_sinf(actor->yaw_radians) * (turn > 0.0f ? 1.0f : -1.0f);
	// (half the arc: turned as far as the point is round)
	middle_x = actor->x + radius * (SDL_sinf(SDL_fabsf(turn)) * SDL_sinf(actor->yaw_radians) + (1.0f - SDL_cosf(turn)) * side_x);
	middle_z = actor->z + radius * (SDL_sinf(SDL_fabsf(turn)) * SDL_cosf(actor->yaw_radians) + (1.0f - SDL_cosf(turn)) * side_z);
	return navigation_body_line(navigation, terrain, water, agent, actor->x, actor->z, actor->center_ground, middle_x, middle_z, BODY_BRUSHES)
		&& navigation_body_line(navigation, terrain, water, agent, middle_x, middle_z, actor->center_ground, x, z, BODY_BRUSHES);
}

// Where its way goes on from the point it steers for, if that is one of
// its way's corners (a fine way's point, its waypoint: not a point ahead
// past it, nor its path's end)
static bool next_corner(const actor_t *actor, float target_x, float target_z, float *next_x, float *next_z)
{
	if (fine_active(actor))
	{
		if (actor->fine_index + 1 < actor->fine_count)
		{
			*next_x = actor->fine_path[actor->fine_index + 1][0];
			*next_z = actor->fine_path[actor->fine_index + 1][1];
		}
		else
		{
			*next_x = actor->path[actor->path_index][0];
			*next_z = actor->path[actor->path_index][1];
		}
		return (*next_x - target_x) * (*next_x - target_x) + (*next_z - target_z) * (*next_z - target_z) > 0.05f * 0.05f;
	}
	if (actor->path_index + 1 >= actor->path_count || target_x != actor->path[actor->path_index][0] || target_z != actor->path[actor->path_index][1])
	{
		return false;
	}
	*next_x = actor->path[actor->path_index + 1][0];
	*next_z = actor->path[actor->path_index + 1][1];
	return true;
}

// Its jump to its waypoint (one its path gets to by a jump: an edge it
// does not step, a terrace's): turned to face it on the spot, gathered
// (its "jump_start" clip's length), through the air on an arc that tops
// JUMP_OVER_METERS over the higher of its ends, falling as things fall,
// its body tilted by how it rises and falls round its middle, its feet
// let go of the ground (no IK); landed ("jump_land"), its feet taken up
// again as that plays, then on along its path. Without the clips it goes
// over as it walks. A path asked for while it is in the air is searched
// again from where it lands
static void jump_move(actors_t *actors, actor_t *actor, navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	float seconds)
{
	const actor_type_t *type = &actors->types[actor->type];
	const actor_model_t *model = model_of(actors, actor);

	actor->turn_rate = 0.0f;
	actor->jump_seconds += seconds;
	if (actor->jump_phase == JUMP_TURN)
	{
		float turn = wrap_angle(SDL_atan2f(actor->jump_to[0] - actor->x, actor->jump_to[2] - actor->z) - actor->yaw_radians);

		actor->speed = 0.0f;
		actor->yaw_radians = wrap_angle(actor->yaw_radians + inertial_turn(actor, turn, degrees_to_radians(type->pivot_degrees_per_second),
			seconds));
		place_on_ground(actors, actor, terrain, water, navigation, seconds);
		choose_animation(actors, actor);
		if (SDL_fabsf(turn) < JUMP_FACE_RADIANS || actor->jump_seconds > JUMP_TURN_MOST_SECONDS)
		{
			actor->jump_phase = JUMP_OFF;
			actor->jump_seconds = 0.0f;
			actor->angular_speed = 0.0f;
			actor->jump_phase_seconds = type->jump_actions[0] >= 0 ? SDL_max(action_length(model, type->jump_actions[0]), 0.05f) : JUMP_OFF_SECONDS;
			if (type->jump_actions[0] >= 0)
			{
				start_action(actors, actor, type->jump_actions[0]);
			}
		}
		return;
	}
	if (actor->jump_phase == JUMP_OFF)
	{
		place_on_ground(actors, actor, terrain, water, navigation, seconds);
		if (type->jump_actions[0] >= 0)
		{
			actor->animation_held = true;
		}
		else
		{
			choose_animation(actors, actor);
		}
		if (actor->jump_seconds >= actor->jump_phase_seconds)
		{
			float top = SDL_max(actor->y, actor->jump_to[1]) + JUMP_OVER_METERS;
			float up_seconds = SDL_sqrtf(2.0f * (top - actor->y) / JUMP_GRAVITY);
			float down_seconds = SDL_sqrtf(2.0f * (top - actor->jump_to[1]) / JUMP_GRAVITY);

			actor->jump_from[0] = actor->x;
			actor->jump_from[1] = actor->y;
			actor->jump_from[2] = actor->z;
			actor->jump_rise_speed = JUMP_GRAVITY * up_seconds;
			actor->jump_air_seconds = SDL_max(up_seconds + down_seconds, 0.1f);
			actor->jump_phase = JUMP_AIR;
			actor->jump_seconds = 0.0f;
			if (type->jump_actions[1] >= 0)
			{
				start_action(actors, actor, type->jump_actions[1]);
			}
			if (actors->log_paths)
			{
				SDL_Log("Actor %u (%s): jumps from (%.2f, %.2f, height %.2f) to (%.2f, %.2f, height %.2f), %.2f s in the air", actor->id, type->name,
					(double)actor->x, (double)actor->z, (double)actor->y, (double)actor->jump_to[0], (double)actor->jump_to[2], (double)actor->jump_to[1],
					(double)actor->jump_air_seconds);
			}
		}
		return;
	}
	if (actor->jump_phase == JUMP_AIR)
	{
		float t = SDL_min(actor->jump_seconds, actor->jump_air_seconds);
		float share = t / actor->jump_air_seconds;
		float across_x = actor->jump_to[0] - actor->jump_from[0];
		float across_z = actor->jump_to[2] - actor->jump_from[2];
		float across_speed = SDL_sqrtf(across_x * across_x + across_z * across_z) / actor->jump_air_seconds;
		float rising = actor->jump_rise_speed - JUMP_GRAVITY * t;
		// (level as it leaves the ground and as it lands: its clips' ends)
		float ends = SDL_min(SDL_min(share / 0.15f, (1.0f - share) / 0.2f), 1.0f);
		float pitch = SDL_clamp(SDL_atan2f(rising, SDL_max(across_speed, 0.5f)) * JUMP_PITCH_SHARE, -JUMP_PITCH_MOST_RADIANS,
			JUMP_PITCH_MOST_RADIANS) * ends;
		float middle = JUMP_MIDDLE_SHARE * type->height_meters;

		actor->pitch_radians += (pitch - actor->pitch_radians) * SDL_min(12.0f * seconds, 1.0f);
		// (tilted round its middle, not its feet: the model's turn is
		// round its origin, its middle goes back by so much nose up)
		actor->x = actor->jump_from[0] + across_x * share + SDL_sinf(actor->yaw_radians) * middle * SDL_sinf(actor->pitch_radians);
		actor->z = actor->jump_from[2] + across_z * share + SDL_cosf(actor->yaw_radians) * middle * SDL_sinf(actor->pitch_radians);
		actor->y = actor->jump_from[1] + actor->jump_rise_speed * t - 0.5f * JUMP_GRAVITY * t * t
			+ middle * (1.0f - SDL_cosf(actor->pitch_radians));
		actor->speed = across_speed;
		actor->ik_weight = 0.0f;
		compose_rotation(actor->yaw_radians - degrees_to_radians(type->front_degrees), actor->pitch_radians, 0.0f, actor->rotation);
		if (type->jump_actions[1] >= 0)
		{
			actor->animation_held = true;
		}
		else
		{
			choose_animation(actors, actor);
		}
		if (actor->jump_seconds >= actor->jump_air_seconds)
		{
			actor->x = actor->jump_to[0];
			actor->y = actor->jump_to[1];
			actor->z = actor->jump_to[2];
			actor->center_ground = actor->jump_to[1];
			actor->pitch_radians = 0.0f;
			actor->speed = 0.0f;
			actor->progress_x = actor->x;
			actor->progress_z = actor->z;
			actor->stuck_seconds = 0.0f;
			actor->jump_phase = JUMP_LAND;
			actor->jump_seconds = 0.0f;
			actor->jump_phase_seconds = type->jump_actions[2] >= 0 ? SDL_max(action_length(model, type->jump_actions[2]), 0.05f) : JUMP_LAND_SECONDS;
			if (type->jump_actions[2] >= 0)
			{
				start_action(actors, actor, type->jump_actions[2]);
			}
		}
		return;
	}
	// Landed: its feet onto the ground again as it settles
	place_on_ground(actors, actor, terrain, water, navigation, seconds);
	actor->ik_weight *= SDL_clamp(actor->jump_seconds / actor->jump_phase_seconds, 0.0f, 1.0f);
	if (type->jump_actions[2] >= 0)
	{
		actor->animation_held = true;
	}
	else
	{
		choose_animation(actors, actor);
	}
	if (actor->jump_seconds >= actor->jump_phase_seconds)
	{
		actor->jump_phase = JUMP_NONE;
		if (actor->jump_replan)
		{
			actor->jump_replan = false;
			if (actor->moving && !plan_path(actors, actor, type, navigation, terrain, water, "landed, searching from here"))
			{
				path_failed(actors, actor, water);
			}
		}
		else if (actor->path_index < actor->path_count)
		{
			// (there: a waypoint as any other now, reached)
			actor->path_jump[actor->path_index] = 0;
		}
	}
}

static void move_actor(actors_t *actors, actor_t *actor, navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	float seconds)
{
	const actor_type_t *type = &actors->types[actor->type];
	navigation_agent_t agent = agent_of(type);
	float map_meters = (float)terrain->size * terrain->cell_size_meters;
	float depth = 0.0f;
	float current_x = 0.0f;
	float current_z = 0.0f;
	float wanted_speed = 0.0f;
	float yaw_before = actor->yaw_radians;
	float x_before = actor->x;
	float z_before = actor->z;
	bool turning_about = false;

	// A step between standing and walking under way: its clip moves it
	actor->animation_held = false;
	actor->start_held = false;
	if (actor->gait >= 0 && seconds > 0.0f)
	{
		gait_move(actors, actor, seconds);
		actor->turn_rate = 0.0f;
		place_on_ground(actors, actor, terrain, water, navigation, seconds);
		return;
	}
	// Its jump: begun where its path's next waypoint is one it gets to by
	// a jump (it stands where it leaves the ground: the waypoint before);
	// under way, nothing else; not yet off the ground and stopped: let be
	if (seconds > 0.0f && (actor->jump_phase == JUMP_TURN || actor->jump_phase == JUMP_OFF) && !actor->moving)
	{
		actor->jump_phase = JUMP_NONE;
	}
	if (seconds > 0.0f && actor->jump_phase == JUMP_NONE && actor->moving && !actor->straight && !actor->wandering && !actor->swimming
		&& actor->path_index < actor->path_count && actor->path_jump[actor->path_index] != 0)
	{
		actor->jump_phase = JUMP_TURN;
		actor->jump_seconds = 0.0f;
		actor->jump_replan = false;
		actor->jump_to[0] = actor->path[actor->path_index][0];
		actor->jump_to[1] = actor->path_jump_height[actor->path_index];
		actor->jump_to[2] = actor->path[actor->path_index][1];
	}
	if (seconds > 0.0f && actor->jump_phase != JUMP_NONE)
	{
		jump_move(actors, actor, navigation, terrain, water, seconds);
		return;
	}
	if (water != NULL && !water_sample(water, actor->x, actor->z, &depth, &current_x, &current_z))
	{
		depth = current_x = current_z = 0.0f;
	}
	// Waiting in the water: every second it tries again; out of it, it
	// tries a last time and gives up if that fails too
	if (actor->waiting && seconds > 0.0f && (actor->wait_seconds -= seconds) <= 0.0f)
	{
		actor->wait_seconds = 1.0f;
		actor->replans = 0;
		actor->stuck_seconds = 0.0f;
		actor->progress_x = actor->x;
		actor->progress_z = actor->z;
		if (plan_path_logged(actors, actor, type, navigation, terrain, water, "waiting in the water, trying again", true))
		{
			actor->waiting = false;
		}
		else if (water_depth_at(water, actor->x, actor->z) <= 0.05f)
		{
			if (actors->log_paths)
			{
				SDL_Log("Actor %u (%s): out of the water and still no way: gives up", actor->id, type->name);
			}
			actor->waiting = false;
			push_event(actors, ACTOR_EVENT_NO_PATH, actor);
		}
	}
	// Wandering: its own frame (no path, no goal), then on the ground
	if (actor->wandering && seconds > 0.0f && !is_dying(actors, actor))
	{
		wander_move(actors, actor, navigation, terrain, seconds);
		{
			float turned = wrap_angle(actor->yaw_radians - yaw_before) / seconds;
			float ease = SDL_fabsf(turned) < SDL_fabsf(actor->turn_rate) ? 20.0f : 8.0f;

			actor->turn_rate += (turned - actor->turn_rate) * SDL_min(seconds * ease, 1.0f);
		}
		place_on_ground(actors, actor, terrain, water, navigation, seconds);
		choose_animation(actors, actor);
		return;
	}
	// Something hit it or fell across its way (the physics says so): it
	// searches again from where it is, now
	if (actor->moving && actor->path_dirty && actor->straight)
	{
		actor->path_dirty = false;
	}
	if (actor->moving && actor->path_dirty)
	{
		actor->path_dirty = false;
		if (!plan_path(actors, actor, type, navigation, terrain, water, "path marked dirty, searching again"))
		{
			path_failed(actors, actor, water);
		}
	}
	if (actor->moving && seconds > 0.0f && !actor->straight)
	{
		actor->look_seconds -= seconds;
		if (actor->look_seconds <= 0.0f)
		{
			actor->look_seconds = LOOK_SECONDS;
			if (!look_ahead(actors, actor, navigation, terrain, water))
			{
				path_failed(actors, actor, water);
			}
		}
	}
	// Off from standing by its walk's start; nearing an exact goal, its
	// stop planned and begun (people's steps)
	if (actor->moving && seconds > 0.0f && actor->straight && (gait_start(actors, actor) || gait_plan_stop(actors, actor)))
	{
		actor->turn_rate = 0.0f;
		place_on_ground(actors, actor, terrain, water, navigation, seconds);
		return;
	}
	if (actor->moving && seconds > 0.0f)
	{
		float target_x = actor->path[actor->path_index][0];
		float target_z = actor->path[actor->path_index][1];
		float dx = target_x - actor->x;
		float dz = target_z - actor->z;
		float distance = SDL_sqrtf(dx * dx + dz * dz);
		bool last = actor->path_index == actor->path_count - 1;

		// (an exact walk's last few centimeters: a shuffle onto the spot,
		// whichever way it faces, nothing in its way so near)
		if (last && actor->arrive_meters > 0.0f && distance < EXACT_SHUFFLE_METERS && distance > actor->arrive_meters)
		{
			float shuffle = SDL_min(distance, EXACT_SHUFFLE_SPEED * seconds);

			actor->x += dx / distance * shuffle;
			actor->z += dz / distance * shuffle;
			actor->speed = 0.0f;
			// (its feet shuffling, if it has that)
			if (type->gait_actions[ACTOR_GAIT_SHUFFLE] >= 0)
			{
				start_action(actors, actor, type->gait_actions[ACTOR_GAIT_SHUFFLE]);
				actor->animation_held = true;
			}
			distance -= shuffle;
			actor->progress_x = actor->x;
			actor->progress_z = actor->z;
			actor->stuck_seconds = 0.0f;
		}
		// (a straight walker's waypoints are its mind's spots, a door's
		// before it: reached close, or it cuts the corner into the jamb)
		// (a corner left short of it only where the way on is straight from
		// here: not over the edge of what it goes round)
		// (where it leaves the ground for a jump: stood on, as close)
		if (distance < (last ? (actor->arrive_meters > 0.0f ? actor->arrive_meters : ARRIVE_METERS)
			: (actor->straight || actor->path_jump[actor->path_index + 1] != 0 ? STRAIGHT_WAYPOINT_METERS : WAYPOINT_METERS))
			&& (last || actor->straight || distance < STRAIGHT_WAYPOINT_METERS || corner_cut_passable(actors, actor, navigation, terrain, water)))
		{
			if (!last)
			{
				actor->path_index++;
			}
			else if (actor->path_complete)
			{
				stop_moving(actor);
				push_event(actors, actor->path_partial ? ACTOR_EVENT_GOT_NEAR : ACTOR_EVENT_ARRIVED, actor);
				// (an exact walk's end: turned its way at once, no stop
				// between)
				if (actor->arrive_meters > 0.0f && actor->arrive_face)
				{
					actor->facing = true;
					actor->face_x = actor->x + SDL_sinf(actor->arrive_yaw) * 2.0f;
					actor->face_z = actor->z + SDL_cosf(actor->arrive_yaw) * 2.0f;
				}
			}
			else if (!plan_path(actors, actor, type, navigation, terrain, water, "end of a long path, searching on"))
			{
				path_failed(actors, actor, water);
			}
		}
	}
	if (actor->moving && seconds > 0.0f)
	{
		float target_x = actor->path[actor->path_index][0];
		float target_z = actor->path[actor->path_index][1];
		float heading;
		float turn;
		// (on a path: quicker the slower it goes, its pivot standing)
		float most = (actor->straight || actor->swimming ? degrees_to_radians(type->turn_degrees_per_second)
			: path_turn_radians_per_second(type, actor->speed)) * seconds;
		bool steered_off = false;
		bool turn_refused = false;
		bool creep_ahead = false;

		// (on a fine way: its points one by one, close; not walking
		// straight: round the corners, a point ahead)
		while (fine_active(actor))
		{
			float fine_dx = actor->fine_path[actor->fine_index][0] - actor->x;
			float fine_dz = actor->fine_path[actor->fine_index][1] - actor->z;

			if (fine_dx * fine_dx + fine_dz * fine_dz > FINE_POINT_METERS * FINE_POINT_METERS)
			{
				break;
			}
			actor->fine_index++;
			// (its end, by its waypoint: the waypoint reached, on to the
			// next; not the path's last: its arrival is its own)
			if (actor->fine_index >= actor->fine_count && actor->fine_to_waypoint && actor->path_index < actor->path_count - 1
				&& actor->path_jump[actor->path_index + 1] == 0)
			{
				actor->path_index++;
				actor->steer_seconds = 0.0f;
				target_x = actor->path[actor->path_index][0];
				target_z = actor->path[actor->path_index][1];
			}
		}
		if (fine_active(actor))
		{
			target_x = actor->fine_path[actor->fine_index][0];
			target_z = actor->fine_path[actor->fine_index][1];
		}
		else if (!actor->straight)
		{
			steer_point(actors, actor, navigation, terrain, water, seconds, &target_x, &target_z);
			// (no straight way for its body to its waypoint: a fine way
			// now, not after its steps are refused there)
			if (actor->steer_blocked && actor->fine_wait <= 0.0f)
			{
				plan_fine(actors, actor, navigation, terrain, water, &agent, "no straight way for its body");
				actor->fine_wait = FINE_AGAIN_SECONDS;
				actor->steer_blocked = false;
				if (fine_active(actor))
				{
					target_x = actor->fine_path[actor->fine_index][0];
					target_z = actor->fine_path[actor->fine_index][1];
				}
			}
		}
		heading = SDL_atan2f(target_x - actor->x, target_z - actor->z);
		// Walking straight, its nose against something and its way round
		// behind it: it backs out clear first (straight, no turning), then
		// turns; not if something (a body, another) is behind it too
		if (actor->straight && actor->arrive_meters <= 0.0f && !actor->swimming && actor->backing_meters <= 0.0f && actor->straight_free_meters <= 0.0f
			&& SDL_fabsf(wrap_angle(heading - actor->yaw_radians)) > BACK_TURN_RADIANS
			&& straight_blocked(actors, actor, SDL_sinf(actor->yaw_radians) * BACK_NOSE_METERS, SDL_cosf(actor->yaw_radians) * BACK_NOSE_METERS)
			&& !straight_blocked(actors, actor, -SDL_sinf(actor->yaw_radians) * 0.3f, -SDL_cosf(actor->yaw_radians) * 0.3f)
			&& !straight_actor_ahead(actors, actor, -SDL_sinf(actor->yaw_radians) * 0.3f, -SDL_cosf(actor->yaw_radians) * 0.3f))
		{
			actor->backing_meters = BACK_MOST_METERS;
			if (actors->log_paths)
			{
				SDL_Log("Actor %u (%s): backing out at (%.2f, %.2f), its way %.0f degrees round", actor->id, type->name, (double)actor->x, (double)actor->z,
					(double)(SDL_fabsf(wrap_angle(heading - actor->yaw_radians)) * 57.29578f));
			}
		}
		if (actor->backing_meters > 0.0f)
		{
			// (clear now, or something behind: done, it turns)
			if (!straight_blocked(actors, actor, SDL_sinf(actor->yaw_radians) * BACK_CLEAR_METERS, SDL_cosf(actor->yaw_radians) * BACK_CLEAR_METERS)
				|| straight_blocked(actors, actor, -SDL_sinf(actor->yaw_radians) * 0.2f, -SDL_cosf(actor->yaw_radians) * 0.2f)
				|| straight_actor_ahead(actors, actor, -SDL_sinf(actor->yaw_radians) * 0.2f, -SDL_cosf(actor->yaw_radians) * 0.2f))
			{
				actor->backing_meters = 0.0f;
			}
		}
		// Walking straight: round what is in its way (the nearest free
		// heading); nothing free: it waits for the others in the way a
		// while, a body all round ends the walk (there, if near its end)
		if (actor->backing_meters > 0.0f)
		{
			heading = actor->yaw_radians;
		}
		else if (actor->straight && actor->straight_free_meters <= 0.0f && !actor->swimming)
		{
			bool all_bodies = false;
			float steered = heading;

			if (!steer(actors, actor, heading, seconds, &steered, &all_bodies))
			{
				float dx = actor->goal_x - actor->x;
				float dz = actor->goal_z - actor->z;
				bool near_end = actor->path_index == actor->path_count - 1 && dx * dx + dz * dz < STRAIGHT_NEAR_END_METERS * STRAIGHT_NEAR_END_METERS;

				actor->crowd_seconds = all_bodies ? CROWD_WAIT_SECONDS + 1.0f : actor->crowd_seconds + seconds;
				if (near_end || actor->crowd_seconds > CROWD_WAIT_SECONDS)
				{
					if (actors->log_paths)
					{
						SDL_Log("Actor %u (%s): its straight walk stopped at (%.2f, %.2f): %s all round, %.1f m from its end", actor->id, type->name,
							(double)actor->x, (double)actor->z, all_bodies ? "bodies" : "others", (double)SDL_sqrtf(dx * dx + dz * dz));
					}
					actor->speed = 0.0f;
					stop_moving(actor);
					push_event(actors, near_end ? ACTOR_EVENT_ARRIVED : ACTOR_EVENT_NO_PATH, actor);
					// (bodies all round where it set out from: caught in
					// something, a hen in a trough; the third time in a row
					// its next walk goes out free a little)
					if (all_bodies && !near_end && SDL_fabsf(actor->x - actor->progress_x) + SDL_fabsf(actor->z - actor->progress_z) < 0.05f
						&& ++actor->straight_blocks >= 3)
					{
						actor->straight_blocks = 0;
						actor->straight_free_meters = 0.6f;
					}
				}
				heading = actor->yaw_radians;
				steered_off = true;
			}
			else
			{
				actor->crowd_seconds = 0.0f;
				heading = steered;
			}
		}
		turn = wrap_angle(heading - actor->yaw_radians);
		// (on a path, its way far round: turning about, which is not being
		// stuck)
		turning_about = !actor->straight && SDL_fabsf(turn) > STUCK_TURN_RADIANS;
		{
			float new_yaw = wrap_angle(actor->yaw_radians + inertial_turn(actor, turn, most / seconds, seconds));

			// (walking straight: a turn its body does not fit through is not
			// made; it goes on ahead to make room if it can (out of a door's
			// jambs), else backs out (its nose at a rack))
			if (actor->straight && !actor->swimming && actor->straight_free_meters <= 0.0f && new_yaw != actor->yaw_radians && big_body(actors, actor)
				&& !body_fits(actors, actor, actor->x, actor->z, new_yaw) && body_fits(actors, actor, actor->x, actor->z, actor->yaw_radians))
			{
				turn_refused = true;
				creep_ahead = !straight_blocked(actors, actor, SDL_sinf(actor->yaw_radians) * 0.3f, SDL_cosf(actor->yaw_radians) * 0.3f)
					&& !straight_actor_ahead(actors, actor, SDL_sinf(actor->yaw_radians) * 0.3f, SDL_cosf(actor->yaw_radians) * 0.3f);
				if (!creep_ahead && actor->backing_meters <= 0.0f
					&& !straight_blocked(actors, actor, -SDL_sinf(actor->yaw_radians) * 0.3f, -SDL_cosf(actor->yaw_radians) * 0.3f)
					&& !straight_actor_ahead(actors, actor, -SDL_sinf(actor->yaw_radians) * 0.3f, -SDL_cosf(actor->yaw_radians) * 0.3f))
				{
					actor->backing_meters = BACK_MOST_METERS * 0.5f;
				}
				new_yaw = actor->yaw_radians;
				actor->angular_speed = 0.0f;
			}
			actor->yaw_radians = new_yaw;
		}
		if (SDL_getenv("POTOP_DEBUG_ACTOR") != NULL && (uint32_t)SDL_atoi(SDL_getenv("POTOP_DEBUG_ACTOR")) == actor->id)
		{
			SDL_Log("DBG %u at (%.2f, %.2f, height %.2f) yaw %.0f heading %.0f speed %.2f backing %.2f crowd %.1f steered_off %d", actor->id, (double)actor->x, (double)actor->z, (double)actor->y,
				(double)(actor->yaw_radians * 57.3f), (double)(heading * 57.3f), (double)actor->speed, (double)actor->backing_meters, (double)actor->crowd_seconds, steered_off ? 1 : 0);
		}
		// Facing away, it turns before it goes; or, walking straight,
		// keeps some of its walk and goes round in an arc (turn_walk_fraction)
		wanted_speed = steered_off || !actor->moving ? 0.0f : SDL_max(SDL_cosf(turn), 0.0f);
		// (to be off by its walk's start: still till it faces the way)
		if (actor->start_held)
		{
			wanted_speed = 0.0f;
			actor->start_held_seconds += seconds;
		}
		if (actor->backing_meters > 0.0f)
		{
			wanted_speed = -BACK_SPEED_SHARE;
		}
		else if (turn_refused && creep_ahead)
		{
			// (room to turn ahead of it: on a little first)
			wanted_speed = SDL_max(wanted_speed, BACK_SPEED_SHARE);
		}
		if (actor->moving && !steered_off && actor->backing_meters <= 0.0f && actor->straight && !actor->swimming && type->turn_walk_fraction > 0.0f
			&& wanted_speed < type->turn_walk_fraction)
		{
			float dx = target_x - actor->x;
			float dz = target_z - actor->z;
			// (round in a circle of the walk over the turning speed: the goal
			// out of it, or it goes round it for ever)
			float turning = degrees_to_radians(type->turn_degrees_per_second);
			float arc = SDL_min(type->turn_walk_fraction, SDL_sqrtf(dx * dx + dz * dz) * turning * 0.5f / type->walk_meters_per_second);

			if (arc > wanted_speed)
			{
				float step = arc * type->walk_meters_per_second * seconds;
				float step_x = SDL_sinf(actor->yaw_radians) * step;
				float step_z = SDL_cosf(actor->yaw_radians) * step;

				// (its first step blocked: it turns on the spot, as before)
				if (actor->straight_free_meters > 0.0f
					|| (!straight_blocked(actors, actor, step_x, step_z) && !straight_actor_ahead(actors, actor, step_x, step_z)))
				{
					wanted_speed = arc;
				}
			}
		}
		if (actor->swimming)
		{
			wanted_speed *= type->swim_meters_per_second;
		}
		else
		{
			float ahead_x = actor->x + SDL_sinf(actor->yaw_radians) * 0.5f;
			float ahead_z = actor->z + SDL_cosf(actor->yaw_radians) * 0.5f;
			float rise = (navigation_surface_height(navigation, terrain, &agent, ahead_x, ahead_z, actor->y)
				- navigation_surface_height(navigation, terrain, &agent, actor->x, actor->z, actor->y)) / 0.5f;

			wanted_speed *= actor->running ? SDL_max(type->run_meters_per_second, type->walk_meters_per_second) : type->walk_meters_per_second;
			// Uphill is slow going, wading too
			wanted_speed /= 1.0f + 2.0f * SDL_max(rise, 0.0f);
			if (depth > type->wade_depth_meters)
			{
				wanted_speed *= 0.6f;
			}
			// On a path: no faster than it can turn onto the point it steers
			// for, round the circle that takes it there from the way it faces
			// (its radius the point's distance over twice the sine of how far
			// round the point is). A point inside its turning circle at its
			// pace it went round and round (a running cow, 6 m wide, for a
			// minute): it slows to where the turn fits
			if (!actor->straight && wanted_speed > 0.0f)
			{
				float to_x = target_x - actor->x;
				float to_z = target_z - actor->z;
				float off = SDL_fabsf(wrap_angle(SDL_atan2f(to_x, to_z) - actor->yaw_radians));
				float sine = SDL_sinf(SDL_min(off, MATH3D_PI * 0.5f));

				if (sine > 1e-3f)
				{
					wanted_speed = SDL_min(wanted_speed, arc_speed(type, SDL_sqrtf(to_x * to_x + to_z * to_z) / (2.0f * sine)));
				}
				// (and only where its body has the room for that circle's
				// arc: on a terrace two meters wide the arc swings it out
				// over the stairs, into the balustrade; no room: it turns
				// on the spot till the point is ahead, then goes straight)
				actor->arc_seconds -= seconds;
				if (actor->arc_seconds <= 0.0f)
				{
					actor->arc_seconds = ARC_SECONDS;
					actor->arc_clear = off <= ARC_FREE_RADIANS || turn_arc_clear(actor, navigation, terrain, water, &agent, target_x, target_z);
				}
				if (off > ARC_FREE_RADIANS && !actor->arc_clear)
				{
					wanted_speed = 0.0f;
				}
				// (to where it leaves the ground for a jump: all but still
				// there, it gathers itself)
				if (actor->path_index + 1 < actor->path_count && actor->path_jump[actor->path_index + 1] != 0
					&& target_x == actor->path[actor->path_index][0] && target_z == actor->path[actor->path_index][1])
				{
					wanted_speed = SDL_min(wanted_speed, SDL_sqrtf(2.0f * ACCELERATION * SDL_sqrtf(to_x * to_x + to_z * to_z)) + 0.15f);
				}
				// (to a corner of its way, not a point ahead past it: no
				// faster there than the corner's turn is made at, slowing
				// down for it as it comes, not past it and back)
				if (wanted_speed > 0.0f)
				{
					float next_x;
					float next_z;

					if (next_corner(actor, target_x, target_z, &next_x, &next_z))
					{
						float distance = SDL_sqrtf(to_x * to_x + to_z * to_z);
						float corner = SDL_fabsf(wrap_angle(SDL_atan2f(next_x - target_x, next_z - target_z) - SDL_atan2f(to_x, to_z)));
						float cosine = SDL_cosf(SDL_min(corner, MATH3D_PI) * 0.5f);
						// (the circle that rounds the corner no further off
						// it than the slack)
						float radius = cosine > 1e-3f ? CORNER_SLACK_METERS / (1.0f / cosine - 1.0f + 1e-6f) : 0.0f;
						float pace = arc_speed(type, radius);

						wanted_speed = SDL_min(wanted_speed, SDL_sqrtf(pace * pace + 2.0f * ACCELERATION * distance));
					}
				}
			}
		}
		// Do not overshoot the goal
		if (actor->path_index == actor->path_count - 1 && actor->path_complete)
		{
			// (the path's end, not the point it steers for)
			float dx = actor->path[actor->path_index][0] - actor->x;
			float dz = actor->path[actor->path_index][1] - actor->z;

			// (to end exactly: at its pace to the last few centimeters, then
			// a shuffle onto it; turned to it on the spot first when it is
			// not ahead, not round and round it)
			// Not an exact end: slowing down as it can (half its braking:
			// room to spare), all but still at the arrival, so its feet do
			// not step on in place after it stops
			if (actor->arrive_meters <= 0.0f)
			{
				float left = SDL_max(SDL_sqrtf(dx * dx + dz * dz) - ARRIVE_METERS, 0.0f);

				wanted_speed = SDL_min(wanted_speed, SDL_sqrtf(2.0f * ACCELERATION * left) + 0.08f);
			}
			else
			{
				float off = SDL_fabsf(wrap_angle(SDL_atan2f(dx, dz) - actor->yaw_radians));

				float circle = SDL_max(actor->speed, walk_pace(actors, actor) * 0.5f) / SDL_max(degrees_to_radians(type->turn_degrees_per_second), 0.1f);

				// (the goal too near and too far round for its turning circle
				// at this pace: it would go round and round it)
				if (off > 1.0f && SDL_sqrtf(dx * dx + dz * dz) < 2.2f * circle + 0.2f && SDL_sqrtf(dx * dx + dz * dz) >= EXACT_SHUFFLE_METERS)
				{
					wanted_speed = 0.0f;
				}
				if (off > 0.7f && SDL_sqrtf(dx * dx + dz * dz) < 0.6f && SDL_sqrtf(dx * dx + dz * dz) >= EXACT_SHUFFLE_METERS
					&& type->gait_actions[ACTOR_GAIT_STOP_LEFT] < 0)
				{
					wanted_speed = 0.0f;
				}
			}
		}
	}
	// A one-shot of its own playing (eating, getting up, lying down, a
	// flinch): it does not go meanwhile, or it slid a meter eating; it goes
	// once that is over (choose_animation lets such a clip play out)
	if (actor->action >= 0 && !walking_action(type, actor->action) && !is_dying(actors, actor)
		&& (model_of(actors, actor)->actions[actor->action].flags & ACTOR_ACTION_LOOP) == 0 && actor->backing_meters <= 0.0f)
	{
		wanted_speed = 0.0f;
	}
	// Speed up and slow down gently
	if (actor->speed < wanted_speed)
	{
		actor->speed = SDL_min(actor->speed + ACCELERATION * seconds, wanted_speed);
	}
	else
	{
		actor->speed = SDL_max(actor->speed - 2.0f * ACCELERATION * seconds, wanted_speed);
	}
	if (seconds > 0.0f)
	{
		float step_x = SDL_sinf(actor->yaw_radians) * actor->speed * seconds;
		float step_z = SDL_cosf(actor->yaw_radians) * actor->speed * seconds;
		// Running water carries it, more the deeper it is
		float carried = SDL_clamp(depth / type->swim_depth_meters, 0.0f, 1.0f);

		// Walking straight, nothing searched: a body just ahead (a fence,
		// a feeder: their shapes, not the path finding's meter grid, far
		// too coarse for a hen) stops it, the walk over: there if that is
		// near the end of it, else it found no way (the mind picks another)
		bool by_body = false;

		// (backing out: so much less left of it)
		if (actor->backing_meters > 0.0f)
		{
			actor->backing_meters -= SDL_sqrtf(step_x * step_x + step_z * step_z);
		}
		// (caught: out free for a while; see actor_t.straight_blocks)
		if (actor->straight && actor->straight_free_meters > 0.0f)
		{
			actor->straight_free_meters -= SDL_sqrtf(step_x * step_x + step_z * step_z);
		}
		else if (actor->straight && actor->moving && (step_x != 0.0f || step_z != 0.0f)
			&& ((by_body = straight_blocked(actors, actor, step_x, step_z)) || straight_actor_ahead(actors, actor, step_x, step_z)))
		{
			// (the way it faces still blocked, turning to a free one: it
			// holds still till it faces that)
			step_x = 0.0f;
			step_z = 0.0f;
			actor->speed = 0.0f;
		}
		else if (actor->straight && actor->moving && (step_x != 0.0f || step_z != 0.0f))
		{
			actor->straight_blocks = 0;
		}
		// On its path, not where the path finding says it cannot be: a turn
		// wider than the way (running round a stair's top) is not made
		// through the balustrade, it stops short (its radius off) and turns
		// on the spot; unless it is somewhere it cannot be already (pushed
		// there): out it goes
		if (!actor->straight && !actor->swimming && actor->moving && actor->speed > 0.0f && (step_x != 0.0f || step_z != 0.0f)
			&& !step_passable(navigation, terrain, water, &agent, actor, step_x, step_z, fine_active(actor)))
		{
			// (along the edge instead, a little to either side: its path
			// runs along it, its center a little over)
			static const float SLIDE_RADIANS[4] = { 0.5f, -0.5f, 1.0f, -1.0f };
			bool slid = false;

			for (int k = 0; k < 4 && !slid; k++)
			{
				float slide_cos = SDL_cosf(SLIDE_RADIANS[k]);
				float slide_sin = SDL_sinf(SLIDE_RADIANS[k]);
				float slide_x = (step_x * slide_cos + step_z * slide_sin) * slide_cos;
				float slide_z = (step_z * slide_cos - step_x * slide_sin) * slide_cos;

				if (step_passable(navigation, terrain, water, &agent, actor, slide_x, slide_z, fine_active(actor)))
				{
					step_x = slide_x;
					step_z = slide_z;
					slid = true;
				}
			}
			if (!slid)
			{
				step_x = 0.0f;
				step_z = 0.0f;
				actor->speed = 0.0f;
				// (refused a while: a way round over the quarters)
				actor->guard_seconds += seconds;
				if (actor->guard_seconds >= FINE_AFTER_SECONDS && actor->fine_wait <= 0.0f && !fine_active(actor))
				{
					plan_fine(actors, actor, navigation, terrain, water, &agent, "steps refused");
					actor->fine_wait = FINE_AGAIN_SECONDS;
				}
			}
			else
			{
				actor->guard_seconds = 0.0f;
			}
		}
		else
		{
			actor->guard_seconds = 0.0f;
		}
		actor->fine_wait -= seconds;
		actor->x += step_x + current_x * carried * seconds;
		actor->z += step_z + current_z * carried * seconds;
		actor->x = SDL_clamp(actor->x, 0.5f, map_meters - 0.5f);
		actor->z = SDL_clamp(actor->z, 0.5f, map_meters - 0.5f);
	}
	// Stuck: searches again, and gives up after a few tries
	if (actor->moving && seconds > 0.0f)
	{
		float dx = actor->x - actor->progress_x;
		float dz = actor->z - actor->progress_z;

		if (dx * dx + dz * dz > STUCK_METERS * STUCK_METERS)
		{
			actor->progress_x = actor->x;
			actor->progress_z = actor->z;
			actor->stuck_seconds = 0.0f;
			actor->replans = 0;
			actor->fine_failures = 0;
		}
		// (no way over the quarter meters where the path goes, found so
		// twice: there is none, and a search again finds the same path;
		// not pushed at for a quarter of a minute, given up now)
		else if (actor->fine_failures >= FINE_FAILURES_MOST && !actor->straight)
		{
			if (actors->log_paths)
			{
				SDL_Log("Actor %u (%s): no way for its body on from (%.2f, %.2f, height %.2f) to waypoint %d of %d (%.2f, %.2f): given up", actor->id,
					type->name, (double)actor->x, (double)actor->z, (double)actor->y, actor->path_index, actor->path_count,
					(double)actor->path[actor->path_index][0], (double)actor->path[actor->path_index][1]);
			}
			actor->fine_failures = 0;
			path_failed(actors, actor, water);
		}
		// (turning about takes its time: a quarter of it counted, so one
		// that turns for ever is still found)
		else if ((actor->stuck_seconds += seconds * (turning_about ? 0.25f : 1.0f)) > STUCK_SECONDS)
		{
			actor->stuck_seconds = 0.0f;
			if (actors->log_paths)
			{
				float ahead_x = actor->x + SDL_sinf(actor->yaw_radians) * 0.8f;
				float ahead_z = actor->z + SDL_cosf(actor->yaw_radians) * 0.8f;

				SDL_Log("Actor %u (%s): stuck at (%.1f, %.1f, height %.2f) for %.0f s, heading for waypoint %d of %d (%.1f, %.1f), try %d",
					actor->id, type->name, (double)actor->x, (double)actor->z, (double)actor->y, (double)STUCK_SECONDS, actor->path_index,
					actor->path_count, (double)actor->path[actor->path_index][0], (double)actor->path[actor->path_index][1],
					actor->replans + 1);
				log_point(actors, actor, navigation, terrain, water, "here", actor->x, actor->z, actor->y);
				log_point(actors, actor, navigation, terrain, water, "ahead", ahead_x, ahead_z, actor->y);
			}
			if (actor->straight)
			{
				// (no search: the game picks another way)
				stop_moving(actor);
				push_event(actors, ACTOR_EVENT_NO_PATH, actor);
			}
			else if (++actor->replans > MAXIMUM_REPLANS
				|| !plan_path(actors, actor, type, navigation, terrain, water, "stuck, searching again"))
			{
				path_failed(actors, actor, water);
			}
			// (the same way again, likely: and a fine way to its first
			// waypoint, past what it went round in circles at)
			else if (actor->path_index < actor->path_count)
			{
				navigation_agent_t stuck_agent = agent_of(type);

				plan_fine(actors, actor, navigation, terrain, water, &stuck_agent, "still stuck");
				actor->fine_wait = FINE_AGAIN_SECONDS;
			}
		}
	}
	// A step or two: turning on the spot to its way, sliding onto the
	// point as it turns (its turn's steps the feet's), at least a short
	// while; then it stands there, facing so
	if (!actor->moving && actor->sidestep && seconds > 0.0f && !is_dying(actors, actor))
	{
		float dx = actor->sidestep_x - actor->x;
		float dz = actor->sidestep_z - actor->z;
		float distance = SDL_sqrtf(dx * dx + dz * dz);
		float turn = wrap_angle(actor->arrive_yaw - actor->yaw_radians);
		float rate = degrees_to_radians(type->turn_degrees_per_second);
		float most = rate * seconds;
		float left = SDL_max(SDL_fabsf(turn) / rate, SIDESTEP_LEAST_SECONDS);
		float slide = SDL_min(distance, SDL_max(distance / left, 0.05f) * seconds);
		// (little to turn, and steps of its own: aside, back, forward, a
		// shuffle for a few centimeters, at their pace, turning meanwhile)
		float forward = dx * SDL_sinf(actor->yaw_radians) + dz * SDL_cosf(actor->yaw_radians);
		float lateral = dx * SDL_cosf(actor->yaw_radians) - dz * SDL_sinf(actor->yaw_radians);
		int step_action = -1;
		float step_speed = 0.0f;
		float step_rate = 1.0f;

		if (SDL_fabsf(turn) < SIDESTEP_STEPS_RADIANS && distance > 0.005f)
		{
			if (distance < SHUFFLE_METERS && type->gait_actions[ACTOR_GAIT_SHUFFLE] >= 0)
			{
				step_action = type->gait_actions[ACTOR_GAIT_SHUFFLE];
				step_speed = SHUFFLE_SPEED;
			}
			else if (SDL_fabsf(lateral) >= SDL_fabsf(forward) && type->gait_actions[lateral > 0.0f ? ACTOR_GAIT_STEP_LEFT : ACTOR_GAIT_STEP_RIGHT] >= 0)
			{
				step_action = type->gait_actions[lateral > 0.0f ? ACTOR_GAIT_STEP_LEFT : ACTOR_GAIT_STEP_RIGHT];
				step_speed = type->sidestep_meters_per_second;
			}
			else if (forward < 0.0f && type->gait_actions[ACTOR_GAIT_STEP_BACK] >= 0)
			{
				step_action = type->gait_actions[ACTOR_GAIT_STEP_BACK];
				step_speed = type->step_back_meters_per_second;
			}
			else if (forward >= 0.0f && type->walk_action >= 0 && type->gait_actions[ACTOR_GAIT_SHUFFLE] >= 0)
			{
				step_action = type->walk_action;
				step_rate = 0.6f;
				step_speed = walk_pace(actors, actor) * step_rate;
			}
		}
		if (step_action >= 0)
		{
			slide = SDL_min(distance, step_speed * seconds);
			start_action(actors, actor, step_action);
			actor->action_rate = step_rate;
			actor->animation_held = true;
		}

		actor->yaw_radians = wrap_angle(actor->yaw_radians + SDL_clamp(turn, -most, most));
		if (distance > 1e-4f)
		{
			actor->x += dx / distance * slide;
			actor->z += dz / distance * slide;
		}
		// (no turn to make: its steps all the same, the way it slides)
		if (step_action < 0 && SDL_fabsf(turn) < 0.02f && distance > 0.01f)
		{
			float side = dx * SDL_cosf(actor->yaw_radians) - dz * SDL_sinf(actor->yaw_radians);

			yaw_before = actor->yaw_radians - (side >= 0.0f ? 1.0f : -1.0f) * rate * 0.6f * seconds;
		}
		if (distance - slide < 0.005f && SDL_fabsf(turn) < 0.01f)
		{
			actor->x = actor->sidestep_x;
			actor->z = actor->sidestep_z;
			actor->yaw_radians = actor->arrive_yaw;
			actor->sidestep = false;
			push_event(actors, ACTOR_EVENT_ARRIVED, actor);
			if (step_action >= 0 && type->idle_action >= 0)
			{
				start_action(actors, actor, type->idle_action);
			}
		}
	}
	// Standing, it turns to look where the game wants it to
	if (!actor->moving && actor->facing && seconds > 0.0f && !is_dying(actors, actor))
	{
		float heading = SDL_atan2f(actor->face_x - actor->x, actor->face_z - actor->z);
		float turn = wrap_angle(heading - actor->yaw_radians);
		float new_yaw = wrap_angle(actor->yaw_radians + inertial_turn(actor, turn, degrees_to_radians(type->turn_degrees_per_second), seconds));

		// (a big straight walker's body not fitting that way round: it
		// stays as it is, near enough)
		if (!actor->straight || !big_body(actors, actor) || body_fits(actors, actor, actor->x, actor->z, new_yaw)
			|| !body_fits(actors, actor, actor->x, actor->z, actor->yaw_radians))
		{
			actor->yaw_radians = new_yaw;
		}
		else
		{
			actor->angular_speed = 0.0f;
		}
	}
	// (a test's log: a big one's body caught in something, each time it
	// gets so)
	if (actors->log_paths && actor->straight && big_body(actors, actor))
	{
		bool fits = body_fits(actors, actor, actor->x, actor->z, actor->yaw_radians);

		if (!fits && actor->body_fitted)
		{
			SDL_Log("Actor %u (%s): its body caught in something at (%.2f, %.2f)", actor->id, type->name, (double)actor->x, (double)actor->z);
		}
		actor->body_fitted = fits;
	}
	// Standing (one that walks straight, spot_turn_degrees), it turns no
	// farther than that from where it last went: a look (at a feeder), a
	// step aside, a bump from another. Going, its walk turns it round its
	// arc (turn_walk_fraction, given it by default above), on the spot
	// only where the arc's first step is blocked (not held there: held to
	// the limit and backing out instead, tested in the pens, they got stuck
	// walking in place three to thirty times as often)
	if (seconds > 0.0f && actor->straight && type->spot_turn_degrees > 0.0f && !actor->swimming && !is_dying(actors, actor))
	{
		float limit = degrees_to_radians(type->spot_turn_degrees);
		float moved_x = actor->x - x_before;
		float moved_z = actor->z - z_before;
		float going = SDL_sqrtf(moved_x * moved_x + moved_z * moved_z) / seconds;

		// (its yaw set from outside since: a new spot, there)
		if (SDL_fabsf(wrap_angle(yaw_before - actor->spot_yaw_radians)) > limit + 0.01f)
		{
			actor->spot_yaw_radians = yaw_before;
		}
		if (actor->moving || going >= SPOT_TURN_GOING_SPEED)
		{
			actor->spot_yaw_radians = actor->yaw_radians;
		}
		else
		{
			float off = wrap_angle(actor->yaw_radians - actor->spot_yaw_radians);

			if (SDL_fabsf(off) > limit)
			{
				actor->yaw_radians = wrap_angle(actor->spot_yaw_radians + (off > 0.0f ? limit : -limit));
			}
		}
	}
	// How fast it turns, eased (a frame's jitter does not start a turn's
	// animation): for choose_animation
	if (seconds > 0.0f)
	{
		float turned = wrap_angle(actor->yaw_radians - yaw_before) / seconds;

		// (quicker to settle than to start: no steps in place once it has
		// turned)
		float ease = SDL_fabsf(turned) < SDL_fabsf(actor->turn_rate) ? 20.0f : 8.0f;

		actor->turn_rate += (turned - actor->turn_rate) * SDL_min(seconds * ease, 1.0f);
	}
	place_on_ground(actors, actor, terrain, water, navigation, seconds);
	choose_animation(actors, actor);
}

// ---------------------------------------------------------------------------
// Flying

#define FLIGHT_WAYPOINT_METERS 4.0f        // past a waypoint on the way
#define FLIGHT_ACCELERATION 4.0f           // meters per second, per second, along its way
#define FLIGHT_VERTICAL_ACCELERATION 6.0f  // and up or down
#define FLIGHT_CLEARANCE_METERS 5.0f       // over what it sees ahead, at least
#define FLIGHT_LOOK_SECONDS 3.0f           // it sees as far ahead as it gets in this long
#define FLIGHT_BANK_MAXIMUM_RADIANS 0.8f   // about 45 degrees
#define FLIGHT_PITCH_MAXIMUM_RADIANS 0.5f
#define FLIGHT_GLIDE_SLOPE 0.4f            // coming down to land: meters down per meter on
#define FLIGHT_LAND_SEARCH_METERS 40.0f    // how far from its goal it looks for a spot to land
#define FLIGHT_RING_SAMPLES 24
#define FLIGHT_GRAVITY 9.81f

static float random_unit(actor_t *actor)
{
	// xorshift32
	uint32_t x = actor->random_state != 0 ? actor->random_state : 0x9E3779B9u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	actor->random_state = x;
	return (float)(x >> 8) / 16777216.0f;
}

static void pick_cruise_height(const actor_type_t *type, actor_t *actor)
{
	actor->cruise_height = lerp_float(type->fly_height_minimum_meters, type->fly_height_maximum_meters, random_unit(actor));
	actor->cruise_seconds = 4.0f + 8.0f * random_unit(actor);
}

static bool is_dying(const actors_t *actors, const actor_t *actor)
{
	return actor->action >= 0 && SDL_strncmp(model_of(actors, actor)->actions[actor->action].name, "die", 3) == 0;
}

// The highest thing on a ring around (x, z), a few meters wide
static float ring_obstacle_height(const navigation_t *navigation, const terrain_t *terrain, float x, float z, float radius)
{
	float highest = -1e30f;

	for (int k = 0; k < FLIGHT_RING_SAMPLES; k++)
	{
		float angle = 2.0f * MATH3D_PI * (float)k / (float)FLIGHT_RING_SAMPLES;

		for (int band = -1; band <= 1; band++)
		{
			float distance = radius + 2.0f * (float)band;

			highest = SDL_max(highest, navigation_obstacle_height(navigation, terrain, x + SDL_sinf(angle) * distance,
				z + SDL_cosf(angle) * distance));
		}
	}
	return highest;
}

// A spot a flyer can come down on: dry ground with room for it, nothing
// standing there or within a wing's reach (no landing in a crown)
static bool landable(const actor_type_t *type, const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	float x, float z)
{
	navigation_agent_t agent = agent_of(type);
	float ground = terrain_sample_height_meters(terrain, x, z);
	static const float offsets[5][2] = { { 0.0f, 0.0f }, { 1.5f, 0.0f }, { -1.5f, 0.0f }, { 0.0f, 1.5f }, { 0.0f, -1.5f } };

	if (water_depth_at(water, x, z) > 0.02f)
	{
		return false;
	}
	for (int k = 0; k < 5; k++)
	{
		float sample_x = x + offsets[k][0];
		float sample_z = z + offsets[k][1];

		if (navigation_obstacle_height(navigation, terrain, sample_x, sample_z) > terrain_sample_height_meters(terrain, sample_x, sample_z)
			+ 0.3f)
		{
			return false;
		}
	}
	return navigation_passable(navigation, terrain, water, &agent, x, z, ground);
}

// The nearest landable spot to (x, z), in rings two meters apart
static bool find_landing_spot(const actor_type_t *type, const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	float x, float z, float *spot_x, float *spot_z)
{
	for (float radius = 0.0f; radius <= FLIGHT_LAND_SEARCH_METERS; radius += 2.0f)
	{
		int samples = SDL_max(1, (int)(2.0f * MATH3D_PI * radius / 2.0f));

		for (int k = 0; k < samples; k++)
		{
			float angle = 2.0f * MATH3D_PI * (float)k / (float)samples;
			float candidate_x = x + SDL_sinf(angle) * radius;
			float candidate_z = z + SDL_cosf(angle) * radius;

			if (landable(type, navigation, terrain, water, candidate_x, candidate_z))
			{
				*spot_x = candidate_x;
				*spot_z = candidate_z;
				return true;
			}
		}
	}
	return false;
}

// At the goal (or put out of its place by water): circles over it if the
// ring around it is clear low enough, else comes down on the nearest spot
// it can. report: the arrival is an event (not when the water drove it up)
static void begin_arrival(actors_t *actors, actor_t *actor, const navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	bool report)
{
	const actor_type_t *type = &actors->types[actor->type];
	float goal_ground = terrain_sample_height_meters(terrain, actor->goal_x, actor->goal_z);
	float ring = ring_obstacle_height(navigation, terrain, actor->goal_x, actor->goal_z, type->circle_radius_meters);
	float lowest_circle = ring + FLIGHT_CLEARANCE_METERS;

	actor->path_count = 0;
	actor->path_index = 0;
	actor->steer_seconds = 0.0f;
	actor->circle_direction = random_unit(actor) < 0.5f ? -1.0f : 1.0f;
	if (lowest_circle <= goal_ground + type->fly_height_maximum_meters)
	{
		actor->flight = ACTOR_FLIGHT_CIRCLING;
		actor->moving = false;
		actor->circle_floor_height = lowest_circle;
	}
	else if (find_landing_spot(type, navigation, terrain, water, actor->goal_x, actor->goal_z, &actor->land_x, &actor->land_z))
	{
		actor->flight = ACTOR_FLIGHT_LANDING;
		actor->moving = true;
		actor->path[0][0] = actor->land_x;
		actor->path[0][1] = actor->land_z;
		actor->path_jump[0] = 0;
		actor->path_count = 1;
		actor->path_complete = true;
	}
	else
	{
		// Nowhere to come down: round over it as low as it can
		actor->flight = ACTOR_FLIGHT_CIRCLING;
		actor->moving = false;
		actor->circle_floor_height = lowest_circle;
	}
	if (actors->log_paths)
	{
		SDL_Log("Actor %u (%s) at its goal (%.1f, %.1f): %s", actor->id, type->name, (double)actor->goal_x, (double)actor->goal_z,
			actor->flight == ACTOR_FLIGHT_LANDING ? "no room to circle, landing" : "circling");
		if (actor->flight == ACTOR_FLIGHT_LANDING)
		{
			SDL_Log("  lands at (%.1f, %.1f)", (double)actor->land_x, (double)actor->land_z);
		}
		else
		{
			SDL_Log("  at least at %.1f m (the ground %.1f m, the highest thing around %.1f m)", (double)actor->circle_floor_height,
				(double)goal_ground, (double)ring);
		}
	}
	if (report)
	{
		push_event(actors, actor->path_partial ? ACTOR_EVENT_GOT_NEAR : ACTOR_EVENT_ARRIVED, actor);
	}
}

// Searches a flight path to its goal and takes off along it; false if
// there is none (it stays where it is)
static bool plan_flight(actors_t *actors, actor_t *actor, navigation_t *navigation, const terrain_t *terrain, const char *why)
{
	const actor_type_t *type = &actors->types[actor->type];
	float start_ground = terrain_sample_height_meters(terrain, actor->x, actor->z);
	float goal_ground = terrain_sample_height_meters(terrain, actor->goal_x, actor->goal_z);
	uint64_t start = SDL_GetPerformanceCounter();
	navigation_result_t result;

	// Never so low a ceiling that it is under the bird already
	actor->ceiling_height = SDL_max(SDL_max(start_ground, goal_ground) + type->fly_ceiling_meters, actor->y + 1.0f);
	result = navigation_find_flight_path(navigation, terrain, actor->x, actor->z, actor->goal_x, actor->goal_z, actor->ceiling_height,
		actor->path, &actor->path_count, &actor->path_complete);
	if (actors->log_paths)
	{
		SDL_Log("Actor %u (%s), %s: flying from (%.1f, %.1f, height %.1f) to (%.1f, %.1f) under %.1f m: %s, %d waypoints%s, %d blocks searched in %.2f ms",
			actor->id, type->name, why, (double)actor->x, (double)actor->z, (double)actor->y, (double)actor->goal_x, (double)actor->goal_z,
			(double)actor->ceiling_height,
			result == NAVIGATION_PATH_FOUND ? "found" : result == NAVIGATION_PATH_PARTIAL ? "PARTIAL (as near as it gets)" : "NONE",
			actor->path_count, actor->path_complete ? "" : " (more after the last)", navigation->last_expansions,
			(double)(SDL_GetPerformanceCounter() - start) * 1000.0 / (double)SDL_GetPerformanceFrequency());
		for (int k = 0; k < actor->path_count; k++)
		{
			SDL_Log("  waypoint %d: (%.1f, %.1f)", k, (double)actor->path[k][0], (double)actor->path[k][1]);
		}
	}
	actor->path_index = 0;
	actor->steer_seconds = 0.0f;
	actor->path_generation = navigation->generation;
	actor->path_partial = result == NAVIGATION_PATH_PARTIAL;
	if (result == NAVIGATION_PATH_NONE || actor->path_count == 0)
	{
		return false;
	}
	if (actor->flight == ACTOR_FLIGHT_LANDED)
	{
		// Up: off the ground at once, climbing
		actor->vertical_speed = type->climb_meters_per_second;
		actor->speed = SDL_max(actor->speed, 0.3f * type->fly_meters_per_second);
		pick_cruise_height(type, actor);
	}
	actor->flight = ACTOR_FLIGHT_FLYING;
	actor->moving = true;
	return true;
}

// Flapping, gliding down, or what it does on the ground; actions it was
// told to play (an attack) are left to finish
static void choose_flight_animation(actors_t *actors, actor_t *actor)
{
	const actor_type_t *type = &actors->types[actor->type];
	bool own = actor->action < 0 || actor->action == type->walk_action || actor->action == type->glide_action
		|| actor->action == type->idle_action;
	int wanted;

	if (!own || actor->flight == ACTOR_FLIGHT_FALLING || is_dying(actors, actor))
	{
		return;
	}
	if (actor->flight == ACTOR_FLIGHT_LANDED)
	{
		wanted = type->idle_action;
	}
	else if (type->glide_action >= 0 && actor->vertical_speed < -1.5f)
	{
		wanted = type->glide_action;
	}
	else
	{
		wanted = type->walk_action >= 0 ? type->walk_action : type->idle_action;
	}
	if (wanted >= 0)
	{
		start_action(actors, actor, wanted);
		// Flapping harder climbing
		actor->action_rate = wanted == type->walk_action ? SDL_clamp(1.0f + 0.1f * actor->vertical_speed, 0.8f, 1.5f) : 1.0f;
	}
}

static void move_flyer(actors_t *actors, actor_t *actor, navigation_t *navigation, const terrain_t *terrain, const water_t *water,
	float seconds)
{
	const actor_type_t *type = &actors->types[actor->type];
	float map_meters = (float)terrain->size * terrain->cell_size_meters;
	float turn = actor->yaw_radians - degrees_to_radians(type->front_degrees);
	float target_x;
	float target_z;
	float wanted_speed = type->fly_meters_per_second;
	float wanted_height;
	float turn_rate = 0.0f;
	float look_meters;
	float ahead;

	actor->swimming = false;
	if (is_dying(actors, actor) && actor->flight != ACTOR_FLIGHT_LANDED)
	{
		actor->flight = ACTOR_FLIGHT_FALLING;
		actor->moving = false;
	}
	if (actor->flight == ACTOR_FLIGHT_LANDED)
	{
		if (seconds > 0.0f && !is_dying(actors, actor) && water_depth_at(water, actor->x, actor->z) > 0.05f)
		{
			// The water came: up, and round over where it stood (or down
			// again on the nearest dry spot)
			if (actors->log_paths)
			{
				SDL_Log("Actor %u (%s): water under it, taking off", actor->id, type->name);
			}
			actor->goal_x = actor->x;
			actor->goal_z = actor->z;
			actor->path_partial = false;
			actor->ceiling_height = terrain_sample_height_meters(terrain, actor->x, actor->z) + type->fly_ceiling_meters;
			actor->vertical_speed = type->climb_meters_per_second;
			actor->speed = 0.3f * type->fly_meters_per_second;
			pick_cruise_height(type, actor);
			begin_arrival(actors, actor, navigation, terrain, water, false);
		}
		else
		{
			actor->speed = 0.0f;
			actor->vertical_speed = 0.0f;
			actor->pitch_radians = 0.0f;
			actor->bank_radians = 0.0f;
			place_on_ground(actors, actor, terrain, water, navigation, seconds);
			choose_flight_animation(actors, actor);
			return;
		}
	}
	if (seconds <= 0.0f)
	{
		compose_rotation(turn, actor->pitch_radians, actor->bank_radians, actor->rotation);
		return;
	}
	if (actor->flight == ACTOR_FLIGHT_FALLING)
	{
		float floor_height;
		float depth = water_depth_at(water, actor->x, actor->z);

		actor->vertical_speed -= FLIGHT_GRAVITY * seconds;
		actor->speed = SDL_max(actor->speed - 3.0f * seconds, 0.0f);
		actor->x = SDL_clamp(actor->x + SDL_sinf(actor->yaw_radians) * actor->speed * seconds, 0.5f, map_meters - 0.5f);
		actor->z = SDL_clamp(actor->z + SDL_cosf(actor->yaw_radians) * actor->speed * seconds, 0.5f, map_meters - 0.5f);
		actor->y += actor->vertical_speed * seconds;
		floor_height = navigation_surface_height(navigation, terrain, NULL, actor->x, actor->z, actor->y);
		if (depth > 0.0f)
		{
			floor_height = SDL_max(floor_height, terrain_sample_height_meters(terrain, actor->x, actor->z) + depth);
		}
		actor->pitch_radians += (-FLIGHT_PITCH_MAXIMUM_RADIANS - actor->pitch_radians) * SDL_min(2.0f * seconds, 1.0f);
		if (actor->y <= floor_height)
		{
			actor->y = floor_height;
			actor->flight = ACTOR_FLIGHT_LANDED;
			actor->vertical_speed = 0.0f;
			actor->speed = 0.0f;
			place_on_ground(actors, actor, terrain, water, navigation, seconds);
			return;
		}
		compose_rotation(turn, actor->pitch_radians, actor->bank_radians, actor->rotation);
		return;
	}
	if ((actor->cruise_seconds -= seconds) <= 0.0f)
	{
		pick_cruise_height(type, actor);
	}
	if (actor->flight == ACTOR_FLIGHT_FLYING && actor->path_dirty)
	{
		actor->path_dirty = false;
		if (!plan_flight(actors, actor, navigation, terrain, "path marked dirty, searching again"))
		{
			begin_arrival(actors, actor, navigation, terrain, water, false);
		}
	}
	// Along the path: past its waypoints, and at the goal
	if (actor->flight == ACTOR_FLIGHT_FLYING)
	{
		bool last = actor->path_index == actor->path_count - 1;
		float dx = actor->path[actor->path_index][0] - actor->x;
		float dz = actor->path[actor->path_index][1] - actor->z;
		float distance = SDL_sqrtf(dx * dx + dz * dz);

		if (last && actor->path_complete && distance < type->circle_radius_meters)
		{
			if (actor->path_partial)
			{
				// As near as it gets: that is its goal now
				actor->goal_x = actor->path[actor->path_index][0];
				actor->goal_z = actor->path[actor->path_index][1];
			}
			begin_arrival(actors, actor, navigation, terrain, water, true);
		}
		else if (!last && distance < FLIGHT_WAYPOINT_METERS)
		{
			actor->path_index++;
		}
		else if (last && !actor->path_complete && distance < FLIGHT_WAYPOINT_METERS
			&& !plan_flight(actors, actor, navigation, terrain, "end of a long path, searching on"))
		{
			begin_arrival(actors, actor, navigation, terrain, water, true);
		}
	}

	// Where it heads, how fast, and how high it would like to be
	switch (actor->flight)
	{
		case ACTOR_FLIGHT_CIRCLING:
		{
			float radial_x = actor->x - actor->goal_x;
			float radial_z = actor->z - actor->goal_z;
			// A point a little further round the circle: chasing it, it goes round
			float angle = SDL_atan2f(radial_x, radial_z) + actor->circle_direction * 0.7f;

			target_x = actor->goal_x + SDL_sinf(angle) * type->circle_radius_meters;
			target_z = actor->goal_z + SDL_cosf(angle) * type->circle_radius_meters;
			wanted_speed = 0.75f * type->fly_meters_per_second;
			// Up and down with its cruising height, never under what is around
			wanted_height = SDL_max(terrain_sample_height_meters(terrain, actor->goal_x, actor->goal_z) + actor->cruise_height,
				actor->circle_floor_height);
			break;
		}
		case ACTOR_FLIGHT_LANDING:
		{
			float land_ground = terrain_sample_height_meters(terrain, actor->land_x, actor->land_z);
			float dx = actor->land_x - actor->x;
			float dz = actor->land_z - actor->z;
			float distance = SDL_sqrtf(dx * dx + dz * dz);
			float small_radius = 0.5f * type->circle_radius_meters;

			if (distance < 0.6f && actor->y - land_ground < 0.4f)
			{
				// Down
				actor->flight = ACTOR_FLIGHT_LANDED;
				actor->moving = false;
				actor->path_count = 0;
				actor->speed = 0.0f;
				actor->vertical_speed = 0.0f;
				actor->pitch_radians = 0.0f;
				actor->bank_radians = 0.0f;
				place_on_ground(actors, actor, terrain, water, navigation, seconds);
				choose_flight_animation(actors, actor);
				if (actors->log_paths)
				{
					SDL_Log("Actor %u (%s): landed at (%.1f, %.1f)", actor->id, type->name, (double)actor->x, (double)actor->z);
				}
				return;
			}
			if (actor->y - land_ground > distance * FLIGHT_GLIDE_SLOPE + 3.0f && distance < type->circle_radius_meters)
			{
				// Too high to come straight down: round over the spot, sinking
				float angle = SDL_atan2f(actor->x - actor->land_x, actor->z - actor->land_z) + actor->circle_direction * 0.9f;

				target_x = actor->land_x + SDL_sinf(angle) * small_radius;
				target_z = actor->land_z + SDL_cosf(angle) * small_radius;
				wanted_speed = 0.5f * type->fly_meters_per_second;
				wanted_height = land_ground + small_radius * FLIGHT_GLIDE_SLOPE;
			}
			else
			{
				target_x = actor->land_x;
				target_z = actor->land_z;
				wanted_speed = type->fly_meters_per_second * SDL_clamp(distance / 25.0f, 0.1f, 1.0f);
				wanted_height = land_ground + SDL_max(distance - 0.5f, 0.0f) * FLIGHT_GLIDE_SLOPE;
			}
			break;
		}
		default:
			target_x = actor->path[actor->path_index][0];
			target_z = actor->path[actor->path_index][1];
			wanted_height = SDL_min(terrain_sample_height_meters(terrain, actor->x, actor->z) + actor->cruise_height,
				actor->ceiling_height);
			break;
	}

	// Turning, slower in tight turns
	{
		float heading = SDL_atan2f(target_x - actor->x, target_z - actor->z);
		float wanted_turn = wrap_angle(heading - actor->yaw_radians);
		float applied = inertial_turn(actor, wanted_turn, degrees_to_radians(type->turn_degrees_per_second), seconds);

		actor->yaw_radians = wrap_angle(actor->yaw_radians + applied);
		turn_rate = applied / seconds;
		wanted_speed *= 0.45f + 0.55f * SDL_max(SDL_cosf(wanted_turn), 0.0f);
	}
	if (actor->speed < wanted_speed)
	{
		actor->speed = SDL_min(actor->speed + FLIGHT_ACCELERATION * seconds, wanted_speed);
	}
	else
	{
		actor->speed = SDL_max(actor->speed - FLIGHT_ACCELERATION * seconds, wanted_speed);
	}
	actor->x = SDL_clamp(actor->x + SDL_sinf(actor->yaw_radians) * actor->speed * seconds, 0.5f, map_meters - 0.5f);
	actor->z = SDL_clamp(actor->z + SDL_cosf(actor->yaw_radians) * actor->speed * seconds, 0.5f, map_meters - 0.5f);

	// Up or down: never lower than what it sees ahead allows (landing, only
	// the stretch before the spot counts)
	look_meters = SDL_max(actor->speed * FLIGHT_LOOK_SECONDS, 8.0f);
	if (actor->flight == ACTOR_FLIGHT_CIRCLING)
	{
		// Straight ahead leaves the circle: what is around it is known
		look_meters = 4.0f;
	}
	else if (actor->flight == ACTOR_FLIGHT_LANDING)
	{
		float dx = actor->land_x - actor->x;
		float dz = actor->land_z - actor->z;

		look_meters = SDL_min(look_meters, SDL_sqrtf(dx * dx + dz * dz) - 4.0f);
	}
	ahead = -1e30f;
	for (float distance = 0.0f; distance <= look_meters; distance += 2.0f)
	{
		ahead = SDL_max(ahead, navigation_obstacle_height(navigation, terrain, actor->x + SDL_sinf(actor->yaw_radians) * distance,
			actor->z + SDL_cosf(actor->yaw_radians) * distance));
	}
	if (ahead > -1e29f)
	{
		wanted_height = SDL_max(wanted_height, ahead + (actor->flight == ACTOR_FLIGHT_LANDING ? 1.0f : FLIGHT_CLEARANCE_METERS));
	}
	{
		float wanted_vertical = SDL_clamp((wanted_height - actor->y) * 0.8f, -2.0f * type->climb_meters_per_second,
			type->climb_meters_per_second);
		float change = FLIGHT_VERTICAL_ACCELERATION * seconds;
		float lowest = actor->flight == ACTOR_FLIGHT_LANDING ? terrain_sample_height_meters(terrain, actor->x, actor->z)
			: navigation_obstacle_height(navigation, terrain, actor->x, actor->z) + 0.5f;

		actor->vertical_speed += SDL_clamp(wanted_vertical - actor->vertical_speed, -change, change);
		actor->y += actor->vertical_speed * seconds;
		if (actor->y < lowest)
		{
			actor->y = lowest;
			actor->vertical_speed = SDL_max(actor->vertical_speed, 0.0f);
		}
	}

	// Nose where it goes, leaning into the turn (the lean that turns it at
	// that speed), both eased in
	{
		float wanted_pitch = SDL_clamp(SDL_atan2f(actor->vertical_speed, SDL_max(actor->speed, 2.0f)), -FLIGHT_PITCH_MAXIMUM_RADIANS,
			FLIGHT_PITCH_MAXIMUM_RADIANS);
		float wanted_bank = SDL_clamp(-SDL_atanf(turn_rate * actor->speed / FLIGHT_GRAVITY), -FLIGHT_BANK_MAXIMUM_RADIANS,
			FLIGHT_BANK_MAXIMUM_RADIANS);
		float ease = SDL_min(4.0f * seconds, 1.0f);

		actor->pitch_radians += (wanted_pitch - actor->pitch_radians) * ease;
		actor->bank_radians += (wanted_bank - actor->bank_radians) * ease;
	}
	turn = actor->yaw_radians - degrees_to_radians(type->front_degrees);
	compose_rotation(turn, actor->pitch_radians, actor->bank_radians, actor->rotation);
	choose_flight_animation(actors, actor);
}

// The ones on the ground do not stand in one another: two nearer than
// their body radii together are pushed apart, half the overlap each (a
// heavier one is not told from a lighter yet), along the line between them
static void keep_apart(actors_t *actors, const terrain_t *terrain)
{
	float map_meters = (float)terrain->size * terrain->cell_size_meters;

	for (int i = 0; i < actors->count; i++)
	{
		actor_t *first = &actors->actors[i];
		const actor_type_t *first_type = &actors->types[first->type];
		float first_radius = body_radius(actors, first);

		if (first_type->flies || is_dying(actors, first) || first_radius <= 0.0f)
		{
			continue;
		}
		for (int j = i + 1; j < actors->count; j++)
		{
			actor_t *second = &actors->actors[j];
			const actor_type_t *second_type = &actors->types[second->type];
			float reach = first_radius + body_radius(actors, second);
			float dx = second->x - first->x;
			float dz = second->z - first->z;
			float distance_squared = dx * dx + dz * dz;
			float distance;
			float push;

			if (second_type->flies || distance_squared >= reach * reach || SDL_fabsf(second->y - first->y) > 1.0f || is_dying(actors, second))
			{
				continue;
			}
			distance = SDL_sqrtf(distance_squared);
			if (distance < 1e-4f)
			{
				// (on the very same spot: apart along some way)
				dx = 1.0f;
				dz = 0.0f;
				distance = 1.0f;
				push = 0.5f * reach;
			}
			else
			{
				push = 0.5f * (reach - distance);
			}
			// (not into a body: a hen pushed into a feeder's trough stays
			// caught in it; the one with a wall behind it stays, the other
			// takes all of the push)
			{
				bool first_free = !straight_blocked(actors, first, -dx / distance * push, -dz / distance * push);
				bool second_free = !straight_blocked(actors, second, dx / distance * push, dz / distance * push);
				float first_push = first_free ? (second_free ? push : 2.0f * push) : 0.0f;
				float second_push = second_free ? (first_free ? push : 2.0f * push) : 0.0f;

				first->x = SDL_clamp(first->x - dx / distance * first_push, 0.5f, map_meters - 0.5f);
				first->z = SDL_clamp(first->z - dz / distance * first_push, 0.5f, map_meters - 0.5f);
				second->x = SDL_clamp(second->x + dx / distance * second_push, 0.5f, map_meters - 0.5f);
				second->z = SDL_clamp(second->z + dz / distance * second_push, 0.5f, map_meters - 0.5f);
			}
		}
	}
}

void actors_move(actors_t *actors, navigation_t *navigation, const terrain_t *terrain, const water_t *water, float seconds)
{
	// Their shadows follow on their own (see actors_any_near)
	for (int i = 0; i < actors->count; i++)
	{
		actor_t *actor = &actors->actors[i];

		if (actors->types[actor->type].flies)
		{
			move_flyer(actors, actor, navigation, terrain, water, seconds);
		}
		else
		{
			move_actor(actors, actor, navigation, terrain, water, seconds);
		}
	}
	keep_apart(actors, terrain);
}

uint32_t actors_pick(const actors_t *actors, vec3_t origin, vec3_t direction)
{
	uint32_t best = 0;
	float best_ratio = 1.0f;

	for (int i = 0; i < actors->count; i++)
	{
		const actor_t *actor = &actors->actors[i];
		const actor_model_t *model = model_of(actors, actor);
		float middle[3] = { actor->x - origin.x, actor->y + 0.5f * model->bounds_maximum.y * actor->scale - origin.y,
			actor->z - origin.z };
		float along = middle[0] * direction.x + middle[1] * direction.y + middle[2] * direction.z;
		float across[3];
		float distance;
		float tolerance;

		if (along <= 0.0f)
		{
			continue;
		}
		across[0] = middle[0] - direction.x * along;
		across[1] = middle[1] - direction.y * along;
		across[2] = middle[2] - direction.z * along;
		distance = SDL_sqrtf(across[0] * across[0] + across[1] * across[1] + across[2] * across[2]);
		tolerance = SDL_max(1.5f, 0.025f * along);
		if (distance / tolerance < best_ratio)
		{
			best_ratio = distance / tolerance;
			best = actor->id;
		}
	}
	return best;
}

int actors_raycast(const actors_t *actors, vec3_t origin, vec3_t direction, float maximum_distance, bool only_interactive,
	uint32_t *ids, float *distances, int maximum_hits)
{
	int count = 0;

	for (int i = 0; i < actors->count && maximum_hits > 0; i++)
	{
		const actor_t *actor = &actors->actors[i];
		const actor_model_t *model = model_of(actors, actor);
		const float *q = actor->rotation;
		float scale = SDL_max(actor->scale, 1e-4f);
		vec3_t offset = vec3_make(origin.x - actor->x, origin.y - (actor->y - actor->root_lift), origin.z - actor->z);
		vec3_t local[2];
		float near = 0.0f;
		float far = maximum_distance;
		float low[3] = { model->bounds_minimum.x, model->bounds_minimum.y, model->bounds_minimum.z };
		float high[3] = { model->bounds_maximum.x, model->bounds_maximum.y, model->bounds_maximum.z };
		bool missed = false;
		int place;

		if (only_interactive && !actor->interactive)
		{
			continue;
		}
		// Into the model's space: turned back, scaled down
		for (int k = 0; k < 2; k++)
		{
			vec3_t v = k == 0 ? offset : direction;
			vec3_t c1 = vec3_make(2.0f * (-q[1] * v.z + q[2] * v.y), 2.0f * (-q[2] * v.x + q[0] * v.z), 2.0f * (-q[0] * v.y + q[1] * v.x));
			vec3_t c2 = vec3_make(-q[1] * c1.z + q[2] * c1.y, -q[2] * c1.x + q[0] * c1.z, -q[0] * c1.y + q[1] * c1.x);

			local[k] = vec3_scale(vec3_add(v, vec3_add(vec3_scale(c1, q[3]), c2)), 1.0f / scale);
		}
		for (int axis = 0; axis < 3 && !missed; axis++)
		{
			float o = axis == 0 ? local[0].x : axis == 1 ? local[0].y : local[0].z;
			float d = axis == 0 ? local[1].x : axis == 1 ? local[1].y : local[1].z;

			if (SDL_fabsf(d) < 1e-9f)
			{
				missed = o < low[axis] || o > high[axis];
				continue;
			}
			{
				float t1 = (low[axis] - o) / d;
				float t2 = (high[axis] - o) / d;

				near = SDL_max(near, SDL_min(t1, t2));
				far = SDL_min(far, SDL_max(t1, t2));
				missed = near > far;
			}
		}
		if (missed)
		{
			continue;
		}
		if (count == maximum_hits && near >= distances[maximum_hits - 1])
		{
			continue;
		}
		place = count < maximum_hits ? count : maximum_hits - 1;
		while (place > 0 && distances[place - 1] > near)
		{
			ids[place] = ids[place - 1];
			distances[place] = distances[place - 1];
			place--;
		}
		ids[place] = actor->id;
		distances[place] = near;
		count = SDL_min(count + 1, maximum_hits);
	}
	return count;
}

static void actor_bounds(const actors_t *actors, const actor_t *actor, vec3_t *minimum_out, vec3_t *maximum_out);

// The ray (origin, direction) through a box (low, high): where it goes in
// (>= 0) and by which face (axis and side); false if it misses
static bool ray_box(const float origin[3], const float direction[3], const float low[3], const float high[3], float farthest,
	float *enter_out, int *axis_out, float *side_out)
{
	float near = 0.0f;
	float far = farthest;
	int axis_in = -1;
	float side_in = 0.0f;

	for (int axis = 0; axis < 3; axis++)
	{
		if (SDL_fabsf(direction[axis]) < 1e-12f)
		{
			if (origin[axis] < low[axis] || origin[axis] > high[axis])
			{
				return false;
			}
			continue;
		}
		{
			float t1 = (low[axis] - origin[axis]) / direction[axis];
			float t2 = (high[axis] - origin[axis]) / direction[axis];
			float enter = SDL_min(t1, t2);

			if (enter > near)
			{
				near = enter;
				axis_in = axis;
				side_in = t1 < t2 ? -1.0f : 1.0f;
			}
			far = SDL_min(far, SDL_max(t1, t2));
			if (near > far)
			{
				return false;
			}
		}
	}
	*enter_out = near;
	*axis_out = axis_in;
	*side_out = side_in;
	return true;
}

// A world point or direction into the actor's model space (w: 1 a point, 0
// a direction), and back
static void world_to_model(const actor_t *actor, const float world[3], float w, float out[3])
{
	float conjugate[4] = { -actor->rotation[0], -actor->rotation[1], -actor->rotation[2], actor->rotation[3] };
	float offset[3] = { world[0] - w * actor->x, world[1] - w * (actor->y - actor->root_lift), world[2] - w * actor->z };
	float scale = SDL_max(actor->scale, 1e-4f);

	quaternion_rotate(conjugate, offset, out);
	for (int k = 0; k < 3; k++)
	{
		out[k] /= scale;
	}
}

bool actors_hit_test(const actors_t *actors, vec3_t origin, vec3_t direction, float maximum_distance, actor_hit_t *hit_out)
{
	float world_origin[3] = { origin.x, origin.y, origin.z };
	float world_direction[3] = { direction.x, direction.y, direction.z };
	bool found = false;
	float nearest = maximum_distance;

	for (int i = 0; i < actors->count; i++)
	{
		const actor_t *actor = &actors->actors[i];
		const actor_model_t *model;
		vec3_t low;
		vec3_t high;
		float box_low[3];
		float box_high[3];
		float model_origin[3];
		float model_direction[3];
		float enter;
		int axis;
		float side;

		if (actors->types[actor->type].model < 0)
		{
			continue;
		}
		model = model_of(actors, actor);
		if (model->hit_boxes == NULL)
		{
			continue;
		}
		// (first the box round whatever pose it takes)
		actor_bounds(actors, actor, &low, &high);
		box_low[0] = low.x;
		box_low[1] = low.y;
		box_low[2] = low.z;
		box_high[0] = high.x;
		box_high[1] = high.y;
		box_high[2] = high.z;
		if (!ray_box(world_origin, world_direction, box_low, box_high, nearest, &enter, &axis, &side))
		{
			continue;
		}
		world_to_model(actor, world_origin, 1.0f, model_origin);
		world_to_model(actor, world_direction, 0.0f, model_direction);
		for (int b = 0; b < model->bone_count && b < ACTOR_FILE_MAX_BONES; b++)
		{
			float posed[12];
			float to_box[12];
			float box_origin[3];
			float box_direction[3];

			if (model->hit_box_vertices[b] == 0)
			{
				continue;
			}
			// The box's space to the model's, as posed: the pose's skinning
			// (global times inverse bind) after the bind pose's bone space
			actor_rows_multiply(&actor->bone_rows[b * 12], &model->bone_to_model[b * 12], posed);
			if (!actor_rows_invert(posed, to_box))
			{
				continue;
			}
			for (int k = 0; k < 3; k++)
			{
				box_origin[k] = to_box[k * 4] * model_origin[0] + to_box[k * 4 + 1] * model_origin[1] + to_box[k * 4 + 2] * model_origin[2]
					+ to_box[k * 4 + 3];
				box_direction[k] = to_box[k * 4] * model_direction[0] + to_box[k * 4 + 1] * model_direction[1]
					+ to_box[k * 4 + 2] * model_direction[2];
			}
			// (the same parameter all the way: affine maps keep it, so t is
			// meters along the world's ray)
			if (ray_box(box_origin, box_direction, &model->hit_boxes[b * 6], &model->hit_boxes[b * 6 + 3], nearest, &enter, &axis, &side))
			{
				float box_normal[3] = { 0.0f, 0.0f, 0.0f };
				float model_normal[3];
				float length;

				found = true;
				nearest = enter;
				hit_out->id = actor->id;
				hit_out->distance = enter;
				hit_out->bone = b;
				hit_out->zone = model->bone_zones[b];
				for (int k = 0; k < 3; k++)
				{
					hit_out->point[k] = world_origin[k] + world_direction[k] * enter;
				}
				if (axis < 0)
				{
					// (it starts inside: against the ray)
					for (int k = 0; k < 3; k++)
					{
						hit_out->normal[k] = -world_direction[k];
					}
					continue;
				}
				box_normal[axis] = side;
				for (int k = 0; k < 3; k++)
				{
					model_normal[k] = posed[k * 4] * box_normal[0] + posed[k * 4 + 1] * box_normal[1] + posed[k * 4 + 2] * box_normal[2];
				}
				quaternion_rotate(actor->rotation, model_normal, hit_out->normal);
				length = vector_length(hit_out->normal);
				for (int k = 0; k < 3; k++)
				{
					hit_out->normal[k] /= SDL_max(length, 1e-6f);
				}
			}
		}
	}
	return found;
}

bool actors_kill(actors_t *actors, uint32_t id)
{
	int index = actors_find(actors, id);
	actor_t *actor;
	int action;

	if (index < 0)
	{
		return false;
	}
	actor = &actors->actors[index];
	if (is_dying(actors, actor))
	{
		return false;
	}
	stop_moving(actor);
	actor->speed = 0.0f;
	action = actor_model_find_action_prefix(model_of(actors, actor), "die");
	if (action >= 0)
	{
		start_action(actors, actor, action);
	}
	actors->shadow_dirty = true;
	return true;
}

void actors_set_dead(actors_t *actors, uint32_t id)
{
	int index = actors_find(actors, id);
	actor_t *actor;
	int action;

	if (index < 0)
	{
		return;
	}
	actor = &actors->actors[index];
	stop_moving(actor);
	actor->speed = 0.0f;
	action = actor_model_find_action_prefix(model_of(actors, actor), "die");
	if (action >= 0)
	{
		start_action(actors, actor, action);
		actor->previous_action = -1;
		actor->blend_left_seconds = 0.0f;
		actor->action_seconds = action_length(model_of(actors, actor), action);
	}
	if (actors->types[actor->type].flies)
	{
		actor->flight = ACTOR_FLIGHT_LANDED;
	}
	actors->shadow_dirty = true;
}

void actors_set_game_id(actors_t *actors, uint32_t id, uint32_t game_id)
{
	int index = actors_find(actors, id);

	if (index >= 0)
	{
		actors->actors[index].game_id = game_id;
	}
}

uint32_t actors_find_by_game_id(const actors_t *actors, uint32_t game_id)
{
	for (int i = 0; i < actors->count && game_id != 0; i++)
	{
		if (actors->actors[i].game_id == game_id)
		{
			return actors->actors[i].id;
		}
	}
	return 0;
}

bool actors_is_dead(const actors_t *actors, uint32_t id)
{
	int index = actors_find(actors, id);

	return index >= 0 && is_dying(actors, &actors->actors[index]);
}

bool actors_hurt(actors_t *actors, uint32_t id)
{
	int index = actors_find(actors, id);
	int action;

	if (index < 0 || is_dying(actors, &actors->actors[index]))
	{
		return false;
	}
	action = actor_model_find_action_prefix(model_of(actors, &actors->actors[index]), "pain");
	if (action < 0)
	{
		return false;
	}
	// (again from its start, if it was already flinching)
	actors->actors[index].action_seconds = 0.0f;
	start_action(actors, &actors->actors[index], action);
	return true;
}

// ---------------------------------------------------------------------------
// Drawing

// A box around the actor whatever way it faces and whatever pose it takes
// (poses stay within twice the bind pose's reach)
static void actor_bounds(const actors_t *actors, const actor_t *actor, vec3_t *minimum_out, vec3_t *maximum_out)
{
	const actor_model_t *model = &actors->models[actors->types[actor->type].model];
	float reach = 0.0f;
	float height = model->bounds_maximum.y * actor->scale;

	reach = SDL_max(reach, SDL_fabsf(model->bounds_minimum.x));
	reach = SDL_max(reach, SDL_fabsf(model->bounds_maximum.x));
	reach = SDL_max(reach, SDL_fabsf(model->bounds_minimum.z));
	reach = SDL_max(reach, SDL_fabsf(model->bounds_maximum.z));
	reach *= 2.0f * actor->scale;
	*minimum_out = vec3_make(actor->x - reach, actor->y - actor->root_lift - reach, actor->z - reach);
	*maximum_out = vec3_make(actor->x + reach, actor->y - actor->root_lift + SDL_max(height * 2.0f, reach), actor->z + reach);
}

// actor_block in data/shaders/actor_skinning.glsl
typedef struct
{
	float placement[4];
	float rotation[4];
	float highlight[4];
	float bone_rows[ACTOR_MAX_BONES_DRAWN * 12];
} actor_uniforms_t;

// The material block of object.frag and object_shadow.frag
typedef struct
{
	float highlight_parameters[4];
	int32_t alpha_tested;
	int32_t pbr;
	int32_t padding[2];
	float padding_more[4];           // (object.frag's deciduous: none)
} actor_material_uniforms_t;

// Posed nearer than this every frame, farther every second frame, past the
// second every fourth (meters from the camera)
static const float POSE_EVERY_FRAME_METERS = 30.0f;
static const float POSE_EVERY_SECOND_METERS = 60.0f;
// Not posed for longer than this (frames): its feet's places are stale, the
// ground under them looked for elsewhere; they start over (as at its birth)
static const uint32_t POSE_STALE_FRAMES = 8u;

// Its pose made for this frame, if it was not (drawn twice a frame: the near
// shadow map, the scene; far away: only every few frames, the last kept).
// What is not drawn is not posed at all (actors_update leaves it: the pig
// behind the hill does not dance for nobody)
static void ensure_pose(actors_t *actors, actor_t *actor)
{
	uint32_t age = actors->pose_clock - actor->pose_frame;
	uint32_t interval = 1u;

	if (age == 0u)
	{
		return;
	}
	if (actors->pose_eye_known)
	{
		float dx = actor->x - actors->pose_eye.x;
		float dy = actor->y - actors->pose_eye.y;
		float dz = actor->z - actors->pose_eye.z;
		float squared = dx * dx + dy * dy + dz * dz;

		interval = squared > POSE_EVERY_SECOND_METERS * POSE_EVERY_SECOND_METERS ? 4u
			: (squared > POSE_EVERY_FRAME_METERS * POSE_EVERY_FRAME_METERS ? 2u : 1u);
	}
	if (age < interval)
	{
		return;
	}
	if (age > POSE_STALE_FRAMES)
	{
		actor->feet_known = false;
	}
	update_pose(actors, actor);
	actor->pose_frame = actors->pose_clock;
}

static void draw_actor(actors_t *actors, SDL_GPURenderPass *pass, actor_t *actor, uint32_t color_sampler_slot,
	const float highlight_parameters[4])
{
	const actor_model_t *model = &actors->models[actors->types[actor->type].model];
	int bone_count = SDL_min(model->bone_count, ACTOR_MAX_BONES_DRAWN);
	actor_uniforms_t uniforms;
	actor_material_uniforms_t material;
	SDL_GPUBufferBinding vertex_binding = { model->vertex_buffer, 0 };
	SDL_GPUBufferBinding index_binding = { model->index_buffer, 0 };
	SDL_GPUTextureSamplerBinding texture_binding = { model->texture, gpu.linear_repeat };

	ensure_pose(actors, actor);

	uniforms.placement[0] = actor->x;
	uniforms.placement[1] = actor->y - actor->root_lift;
	uniforms.placement[2] = actor->z;
	uniforms.placement[3] = actor->scale;
	SDL_memcpy(uniforms.rotation, actor->rotation, sizeof(uniforms.rotation));
	uniforms.highlight[0] = actor->id == actors->highlight_id ? 1.0f : 0.0f;
	uniforms.highlight[1] = uniforms.highlight[2] = uniforms.highlight[3] = 0.0f;
	SDL_memcpy(uniforms.bone_rows, actor->bone_rows, sizeof(float) * 12 * (size_t)bone_count);
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &uniforms,
		(uint32_t)(offsetof(actor_uniforms_t, bone_rows) + sizeof(float) * 12 * (size_t)SDL_max(bone_count, 1)));
	SDL_zero(material);
	SDL_memcpy(material.highlight_parameters, highlight_parameters, sizeof(material.highlight_parameters));
	material.alpha_tested = model->alpha_tested ? 1 : 0;
	material.pbr = actors->pbr_shading && model->shaded ? 1 : 0;
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material, sizeof(material));
	SDL_BindGPUIndexBuffer(pass, &index_binding, model->index_size);
	SDL_BindGPUFragmentSamplers(pass, color_sampler_slot, &texture_binding, 1);
	if (color_sampler_slot == 3)
	{
		// The camera's pass: the tangents and the shading maps too
		SDL_GPUBufferBinding vertex_bindings[2] = { { model->vertex_buffer, 0 }, { model->tangent_buffer, 0 } };
		SDL_GPUTextureSamplerBinding maps[3] =
		{
			{ model->normal_texture != NULL ? model->normal_texture : object_default_texture(OBJECT_DEFAULT_NORMAL), gpu.linear_repeat },
			{ model->orm_texture != NULL ? model->orm_texture : object_default_texture(OBJECT_DEFAULT_ORM), gpu.linear_repeat },
			{ model->emissive_texture != NULL ? model->emissive_texture : object_default_texture(OBJECT_DEFAULT_EMISSIVE), gpu.linear_repeat },
		};

		SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, 2);
		SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
	}
	else
	{
		SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
	}
	SDL_DrawGPUIndexedPrimitives(pass, (uint32_t)model->index_count, 1, 0, 0, 0);
}

void actors_draw(actors_t *actors, SDL_GPURenderPass *pass, const camera_t *camera, const shadow_map_t *shadow,
	const shadow_map_t *near_shadow, SDL_GPUTexture *variation_texture, float time_seconds)
{
	frustum_t frustum;
	float highlight_parameters[4] = { actors->highlight_color[0], actors->highlight_color[1], actors->highlight_color[2], time_seconds };
	SDL_GPUTextureSamplerBinding variation_binding = { variation_texture, gpu.linear_repeat };
	SDL_GPUTexture *cloud_shadows = clouds_shadow_texture();
	SDL_GPUTextureSamplerBinding cloud_shadow_binding = { cloud_shadows != NULL ? cloud_shadows : variation_texture, gpu.linear_repeat };
	SDL_GPUGraphicsPipeline *bound = NULL;

	actors->drawn_count = 0;
	if (actors->count == 0)
	{
		return;
	}
	frustum = frustum_from_matrix(camera->view_projection);
	shadow_map_bind_for_shading(pass, shadow, near_shadow);
	SDL_BindGPUFragmentSamplers(pass, 2, &variation_binding, 1);
	SDL_BindGPUFragmentSamplers(pass, 4, &cloud_shadow_binding, 1);
	scene_ground_bind(pass, 8);
	actors->pose_eye = camera->eye_position;
	actors->pose_eye_known = true;
	for (int i = 0; i < actors->count; i++)
	{
		actor_t *actor = &actors->actors[i];
		vec3_t minimum;
		vec3_t maximum;
		SDL_GPUGraphicsPipeline *wanted;

		actor_bounds(actors, actor, &minimum, &maximum);
		if (!frustum_contains_box(&frustum, minimum, maximum))
		{
			continue;
		}
		// Cut out textures (fur, feathers) are seen from both sides
		wanted = actors->models[actors->types[actor->type].model].alpha_tested ? actors->two_sided_pipeline : actors->pipeline;
		if (wanted != bound)
		{
			SDL_BindGPUGraphicsPipeline(pass, wanted);
			lights_bind(pass);
			bound = wanted;
		}
		draw_actor(actors, pass, actor, 3, highlight_parameters);
		actors->drawn_count++;
	}
}

void actors_mark_shadow(const actors_t *actors, shadow_map_t *shadow)
{
	frustum_t frustum = frustum_from_matrix(shadow->light_view_projection);

	// (the same boxes actors_render_shadow culls by: whatever it draws is
	// within one; they are generous, two reaches round, twice the height)
	for (int i = 0; i < actors->count; i++)
	{
		vec3_t minimum;
		vec3_t maximum;

		actor_bounds(actors, &actors->actors[i], &minimum, &maximum);
		if (frustum_contains_box(&frustum, minimum, maximum))
		{
			shadow_map_mark_moving(shadow, minimum, maximum);
		}
	}
}

void actors_render_shadow(actors_t *actors, SDL_GPURenderPass *pass, const shadow_map_t *shadow)
{
	frustum_t frustum;
	static const float no_highlight[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	actors->shadow_dirty = false;
	if (actors->count == 0)
	{
		return;
	}
	frustum = frustum_from_matrix(shadow->light_view_projection);
	SDL_BindGPUGraphicsPipeline(pass, actors->shadow_pipeline);
	for (int i = 0; i < actors->count; i++)
	{
		actor_t *actor = &actors->actors[i];
		vec3_t minimum;
		vec3_t maximum;

		actor_bounds(actors, actor, &minimum, &maximum);
		if (frustum_contains_box(&frustum, minimum, maximum))
		{
			draw_actor(actors, pass, actor, 0, no_highlight);
		}
	}
}

void actors_set_name(actors_t *actors, uint32_t id, const char *name, bool hostile)
{
	int index = actors_find(actors, id);

	if (index >= 0)
	{
		SDL_strlcpy(actors->actors[index].name, name != NULL ? name : "", sizeof(actors->actors[index].name));
		actors->actors[index].hostile = hostile;
	}
}

void actors_look_at(actors_t *actors, uint32_t id, float x, float z)
{
	int index = actors_find(actors, id);

	if (index >= 0)
	{
		// (looking at something: it stands for it)
		actors->actors[index].wandering = false;
		actors->actors[index].facing = true;
		actors->actors[index].face_x = x;
		actors->actors[index].face_z = z;
	}
}

void actors_stop_looking(actors_t *actors, uint32_t id)
{
	int index = actors_find(actors, id);

	if (index >= 0)
	{
		actors->actors[index].facing = false;
	}
}
