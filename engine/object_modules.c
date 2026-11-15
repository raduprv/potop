// Modules on the map: a module's parts placed with it, moved by its states,
// removed with it. See objects.h and module_format.h.
//
// A module's body is an instance of its type (the type objects.ini names
// with module = ); each of its other parts an instance of the part's own
// type (object_types.c makes those), flagged OBJECT_STATE_PART, with the
// body's id. The body keeps the state; the parts are placed from it: where
// the body stands and how it is turned, the part's place in the module,
// and its pose in the state (between two states while it changes).

#include "objects.h"

#include <SDL3/SDL.h>

#define DEGREES_TO_RADIANS 0.017453292519943295f

// out = a * b (b's turn first)
static void quaternion_multiply(const float a[4], const float b[4], float out[4])
{
	float result[4];

	result[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	result[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	result[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	result[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	SDL_memcpy(out, result, sizeof(result));
}

static void quaternion_rotate(const float q[4], const float v[3], float out[3])
{
	// v + 2 q.xyz x (q.xyz x v + w v)
	float c[3] = { q[1] * v[2] - q[2] * v[1] + q[3] * v[0], q[2] * v[0] - q[0] * v[2] + q[3] * v[1], q[0] * v[1] - q[1] * v[0] + q[3] * v[2] };

	out[0] = v[0] + 2.0f * (q[1] * c[2] - q[2] * c[1]);
	out[1] = v[1] + 2.0f * (q[2] * c[0] - q[0] * c[2]);
	out[2] = v[2] + 2.0f * (q[0] * c[1] - q[1] * c[0]);
}

static void quaternion_axis_angle(const float axis[3], float radians, float out[4])
{
	float length = SDL_sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	float half_sine = SDL_sinf(0.5f * radians);

	if (length < 1e-6f)
	{
		out[0] = 0.0f;
		out[1] = 0.0f;
		out[2] = 0.0f;
		out[3] = 1.0f;
		return;
	}
	out[0] = axis[0] / length * half_sine;
	out[1] = axis[1] / length * half_sine;
	out[2] = axis[2] / length * half_sine;
	out[3] = SDL_cosf(0.5f * radians);
}

const module_file_t *objects_module_of(const objects_t *objects, int instance_index)
{
	const object_type_t *type;

	if (instance_index < 0 || instance_index >= objects->instance_count)
	{
		return NULL;
	}
	type = &objects->types.types[objects->instances[instance_index].type];
	if (type->module < 0 || type->module >= objects->types.module_count)
	{
		return NULL;
	}
	return &objects->types.modules[type->module];
}

// A door part's state: the module's "open" or "closed" (failing that its
// second or its first)
static int door_state(const module_file_t *module, bool open)
{
	int state = module_file_find_state(module, open ? "open" : "closed");

	if (state < 0)
	{
		state = open ? SDL_min(1, module->state_count - 1) : module->first_state;
	}
	return state;
}

// How far a change of state got, eased in and out
static float eased(float progress)
{
	float t = SDL_clamp(progress, 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

// Where a part stands as its module stands now: its place, its turn,
// whether it is there, how fast it spins (a share of its rpm)
static void part_pose(const objects_t *objects, const object_instance_t *module_instance, const module_file_t *module, int part,
	const object_instance_t *part_instance, double turns, float position_out[3], float rotation_out[4], bool *hidden_out, float *spin_out)
{
	const module_file_part_t *file_part = &module->parts[part];
	// (a door: its own state, closed or open, in the part's own fields)
	const object_instance_t *stater = (file_part->flags & MODULE_PART_DOOR) != 0 && part_instance != NULL ? part_instance : module_instance;
	int from = SDL_min((int)stater->module_state, module->state_count - 1);
	int to = SDL_min((int)stater->module_target, module->state_count - 1);
	const module_file_pose_t *a = &module->poses[from][part];
	const module_file_pose_t *b = &module->poses[to][part];
	float t = from == to ? 1.0f : eased(stater->module_progress);
	float local[3];
	float turned[3];
	float yaw[4];
	float spin[4];
	float angle;
	float scale = module_instance->scale;

	(void)objects;
	for (int k = 0; k < 3; k++)
	{
		local[k] = (file_part->position[k] + a->offset[k] + (b->offset[k] - a->offset[k]) * t) * scale;
	}
	quaternion_rotate(module_instance->rotation, local, turned);
	position_out[0] = module_instance->x + turned[0];
	position_out[1] = objects_instance_base_y(module_instance) + turned[1];
	position_out[2] = module_instance->z + turned[2];
	angle = (a->angle_degrees + (b->angle_degrees - a->angle_degrees) * t) * DEGREES_TO_RADIANS
		+ (float)((turns - SDL_floor(turns)) * 2.0 * 3.14159265358979);
	quaternion_axis_angle(file_part->axis, angle, spin);
	quaternion_axis_angle((const float[3]){ 0.0f, 1.0f, 0.0f }, file_part->yaw_degrees * DEGREES_TO_RADIANS, yaw);
	quaternion_multiply(yaw, spin, rotation_out);
	quaternion_multiply(module_instance->rotation, rotation_out, rotation_out);
	*hidden_out = (t < 0.5f ? a->flags : b->flags) & MODULE_POSE_HIDDEN;
	*spin_out = a->spin + (b->spin - a->spin) * t;
}

// A part put where its module has it now
static void place_part(objects_t *objects, int module_index, int part_index)
{
	const object_instance_t *module_instance = &objects->instances[module_index];
	const module_file_t *module = objects_module_of(objects, module_index);
	object_instance_t *part = &objects->instances[part_index];
	float position[3];
	float rotation[4];
	bool hidden;
	float spin;

	if (module == NULL || part->module_part >= module->part_count)
	{
		return;
	}
	part_pose(objects, module_instance, module, part->module_part, part, part->module_turns, position, rotation, &hidden, &spin);
	part->x = position[0];
	part->z = position[2];
	part->ground_height = module_instance->ground_height;
	part->lift_meters = position[1] - module_instance->ground_height;
	part->y = position[1];
	part->scale = module_instance->scale;
	part->yaw_radians = module_instance->yaw_radians + module->parts[part->module_part].yaw_degrees * DEGREES_TO_RADIANS;
	SDL_memcpy(part->rotation, rotation, sizeof(rotation));
	if (hidden)
	{
		part->state |= OBJECT_STATE_HIDDEN;
	}
	else
	{
		part->state &= (uint8_t)~OBJECT_STATE_HIDDEN;
	}
	objects_touch(objects, part_index);
}

static int part_type(const objects_t *objects, int module_type, int part)
{
	for (int t = 0; t < objects->types.type_count; t++)
	{
		if (objects->types.types[t].module_type == module_type && objects->types.types[t].module_part == part)
		{
			return t;
		}
	}
	return -1;
}

void object_modules_place_parts(objects_t *objects, const terrain_t *terrain, int module_index)
{
	const module_file_t *module = objects_module_of(objects, module_index);
	object_instance_t *module_instance;
	int module_type;
	uint32_t module_id;
	int group;

	if (module == NULL)
	{
		return;
	}
	module_instance = &objects->instances[module_index];
	module_instance->module_state = (uint8_t)module->first_state;
	module_instance->module_target = (uint8_t)module->first_state;
	module_instance->module_progress = 1.0f;
	module_instance->module_held_seconds = 0.0f;
	module_type = module_instance->type;
	module_id = module_instance->id;
	group = module_instance->group;
	for (int p = 1; p < module->part_count; p++)
	{
		int type = part_type(objects, module_type, p);
		float position[3];
		float rotation[4];
		bool hidden;
		float spin;
		int index;
		object_instance_t *part;

		if (type < 0)
		{
			continue;
		}
		// (the list may move as it grows: the module looked up again each time)
		module_instance = &objects->instances[module_index];
		part_pose(objects, module_instance, module, p, NULL, 0.0, position, rotation, &hidden, &spin);
		if (!objects_add_lifted(objects, terrain, type, position[0], position[2], module_instance->yaw_radians, module_instance->scale, group, 0.0f))
		{
			continue;
		}
		index = objects->instance_count - 1;
		part = &objects->instances[index];
		part->state |= OBJECT_STATE_PART;
		if ((module->parts[p].flags & MODULE_PART_MOVES) != 0)
		{
			part->state |= OBJECT_STATE_MOVING;
		}
		part->module_id = module_id;
		part->module_part = (uint8_t)p;
		part->module_turns = 0.0;
		// (a door: shut to begin with, its own state from then on)
		if ((module->parts[p].flags & MODULE_PART_DOOR) != 0)
		{
			int closed = door_state(module, false);

			part->module_state = (uint8_t)closed;
			part->module_target = (uint8_t)closed;
			part->module_progress = 1.0f;
		}
		place_part(objects, module_index, index);
	}
}

void object_modules_renumber(objects_t *objects, uint32_t old_id, uint32_t new_id)
{
	for (int i = 0; i < objects->instance_count; i++)
	{
		if ((objects->instances[i].state & OBJECT_STATE_PART) != 0 && objects->instances[i].module_id == old_id)
		{
			objects->instances[i].module_id = new_id;
		}
	}
	objects->module_list_valid = false;
}

void object_modules_widen_removal(objects_t *objects, uint8_t *remove)
{
	uint32_t *gone = NULL;
	int gone_count = 0;
	int gone_capacity = 0;

	// The modules any of whose pieces go
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		uint32_t id = 0;

		if (!remove[i])
		{
			continue;
		}
		if ((instance->state & OBJECT_STATE_PART) != 0)
		{
			id = instance->module_id;
		}
		else if (objects->types.types[instance->type].module >= 0)
		{
			id = instance->id;
		}
		if (id == 0)
		{
			continue;
		}
		if (gone_count == gone_capacity)
		{
			int capacity = SDL_max(gone_capacity * 2, 16);
			uint32_t *grown = SDL_realloc(gone, sizeof(uint32_t) * (size_t)capacity);

			if (grown == NULL)
			{
				break;
			}
			gone = grown;
			gone_capacity = capacity;
		}
		gone[gone_count++] = id;
	}
	if (gone_count == 0)
	{
		SDL_free(gone);
		return;
	}
	// (all of each: its body and every part)
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		uint32_t id = (instance->state & OBJECT_STATE_PART) != 0 ? instance->module_id
			: objects->types.types[instance->type].module >= 0 ? instance->id : 0;

		if (id == 0 || remove[i])
		{
			continue;
		}
		for (int g = 0; g < gone_count; g++)
		{
			if (gone[g] == id)
			{
				remove[i] = 1;
				break;
			}
		}
	}
	SDL_free(gone);
	objects->module_list_valid = false;
}

// The modules and their parts by index, as the list is now: each module,
// then its parts (a module has a few parts, the map a few modules: a part's
// module is looked for among them)
static void rebuild_list(objects_t *objects)
{
	int count = 0;
	int parts = 0;
	int *counts;
	int *filled;

	objects->module_count = 0;
	objects->module_list_valid = false;
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];

		count += objects->types.types[instance->type].module >= 0 ? 1 : 0;
		parts += (instance->state & OBJECT_STATE_PART) != 0 ? 1 : 0;
	}
	if (count + 1 > objects->module_capacity)
	{
		int *grown = SDL_realloc(objects->module_entries, sizeof(int) * (size_t)(count + 1));

		if (grown == NULL)
		{
			return;
		}
		objects->module_entries = grown;
		objects->module_capacity = count + 1;
	}
	if (count + parts > objects->module_index_capacity)
	{
		int *grown = SDL_realloc(objects->module_indices, sizeof(int) * (size_t)SDL_max(count + parts, 1));

		if (grown == NULL)
		{
			return;
		}
		objects->module_indices = grown;
		objects->module_index_capacity = SDL_max(count + parts, 1);
	}
	counts = SDL_calloc((size_t)SDL_max(count, 1), sizeof(int));
	filled = SDL_calloc((size_t)SDL_max(count, 1), sizeof(int));
	if (counts == NULL || filled == NULL)
	{
		SDL_free(counts);
		SDL_free(filled);
		return;
	}
	// (the modules first, in module_indices' first count places for now)
	for (int i = 0; i < objects->instance_count; i++)
	{
		if (objects->types.types[objects->instances[i].type].module >= 0)
		{
			objects->module_indices[objects->module_count++] = i;
		}
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		int module_index;

		if ((instance->state & OBJECT_STATE_PART) == 0)
		{
			continue;
		}
		module_index = objects_find_id(objects, instance->module_id);
		for (int m = 0; m < objects->module_count && module_index >= 0; m++)
		{
			if (objects->module_indices[m] == module_index)
			{
				counts[m]++;
				break;
			}
		}
	}
	// (then spread out: module m at its entry, its parts after it)
	objects->module_entries[0] = 0;
	for (int m = 0; m < objects->module_count; m++)
	{
		objects->module_entries[m + 1] = objects->module_entries[m] + 1 + counts[m];
	}
	for (int m = objects->module_count - 1; m >= 0; m--)
	{
		objects->module_indices[objects->module_entries[m]] = objects->module_indices[m];
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		int module_index;

		if ((instance->state & OBJECT_STATE_PART) == 0)
		{
			continue;
		}
		module_index = objects_find_id(objects, instance->module_id);
		for (int m = 0; m < objects->module_count && module_index >= 0; m++)
		{
			if (objects->module_indices[objects->module_entries[m]] == module_index && filled[m] < counts[m])
			{
				objects->module_indices[objects->module_entries[m] + 1 + filled[m]++] = i;
				break;
			}
		}
	}
	SDL_free(counts);
	SDL_free(filled);
	objects->module_generation = objects->generation;
	objects->module_list_valid = true;
}

void objects_update_modules(objects_t *objects, float seconds)
{
	bool moved = false;

	if (objects->types.module_count == 0)
	{
		return;
	}
	if (!objects->module_list_valid || objects->module_generation != objects->generation)
	{
		rebuild_list(objects);
	}
	for (int m = 0; m < objects->module_count; m++)
	{
		int first = objects->module_entries[m];
		int last = objects->module_entries[m + 1];
		int module_index = objects->module_indices[first];
		object_instance_t *module_instance = &objects->instances[module_index];
		const module_file_t *module = objects_module_of(objects, module_index);
		bool changing;

		if (module == NULL)
		{
			continue;
		}
		// Its state: going to the next, or held, or on by itself
		if (module_instance->module_target != module_instance->module_state)
		{
			float length = module->states[SDL_min((int)module_instance->module_target, module->state_count - 1)].seconds;

			module_instance->module_progress = length > 1e-4f ? module_instance->module_progress + seconds / length : 1.0f;
			if (module_instance->module_progress >= 1.0f)
			{
				module_instance->module_state = module_instance->module_target;
				module_instance->module_progress = 1.0f;
				module_instance->module_held_seconds = 0.0f;
			}
		}
		else
		{
			const module_file_state_t *state = &module->states[SDL_min((int)module_instance->module_state, module->state_count - 1)];

			module_instance->module_held_seconds += seconds;
			if (state->next >= 0 && state->next != module_instance->module_state && module_instance->module_held_seconds >= state->hold_seconds)
			{
				module_instance->module_target = (uint8_t)state->next;
				module_instance->module_progress = 0.0f;
			}
		}
		changing = module_instance->module_target != module_instance->module_state;
		// Its parts: those that move while it changes or spins, and any
		// whose ground is not its any more
		for (int k = first + 1; k < last; k++)
		{
			int part_index = objects->module_indices[k];
			object_instance_t *part = &objects->instances[part_index];
			bool moves = (part->state & OBJECT_STATE_MOVING) != 0;
			bool follow = SDL_fabsf(part->ground_height - module_instance->ground_height) > 1e-4f;
			float spin = 0.0f;

			if (part->module_part >= module->part_count)
			{
				continue;
			}
			// A door: swinging on its own, to its own state
			if ((module->parts[part->module_part].flags & MODULE_PART_DOOR) != 0)
			{
				if (part->module_target != part->module_state)
				{
					float length = module->states[SDL_min((int)part->module_target, module->state_count - 1)].seconds;

					part->module_progress = length > 1e-4f ? part->module_progress + seconds / length : 1.0f;
					if (part->module_progress >= 1.0f)
					{
						part->module_state = part->module_target;
						part->module_progress = 1.0f;
					}
					place_part(objects, module_index, part_index);
					moved = true;
				}
				else if (follow)
				{
					place_part(objects, module_index, part_index);
				}
				continue;
			}
			if (moves)
			{
				const module_file_part_t *file_part = &module->parts[part->module_part];
				int from = SDL_min((int)module_instance->module_state, module->state_count - 1);
				int to = SDL_min((int)module_instance->module_target, module->state_count - 1);
				float t = from == to ? 1.0f : eased(module_instance->module_progress);

				spin = module->poses[from][part->module_part].spin + (module->poses[to][part->module_part].spin - module->poses[from][part->module_part].spin) * t;
				if (spin != 0.0f && file_part->spin_rpm != 0.0f)
				{
					part->module_turns += (double)(seconds * spin * file_part->spin_rpm / 60.0f);
					part->module_turns -= SDL_floor(part->module_turns);
				}
				else
				{
					spin = 0.0f;
				}
			}
			if ((moves && (changing || spin != 0.0f)) || follow
				|| (changing && (module->poses[module_instance->module_state][part->module_part].flags != module->poses[module_instance->module_target][part->module_part].flags)))
			{
				place_part(objects, module_index, part_index);
				moved = moved || moves;
			}
		}
	}
	if (moved)
	{
		objects->moving_shadows = true;
	}
}

bool objects_set_module_state(objects_t *objects, uint32_t id, const char *state_name, int state)
{
	int index = objects_find_id(objects, id);
	const module_file_t *module = objects_module_of(objects, index);
	object_instance_t *instance;

	if (module == NULL)
	{
		return false;
	}
	if (state_name != NULL)
	{
		state = module_file_find_state(module, state_name);
	}
	if (state < 0 || state >= module->state_count)
	{
		return false;
	}
	instance = &objects->instances[index];
	if (instance->module_target == (uint8_t)state)
	{
		return true;
	}
	// (turned back on the way: from where it got, as near as one state
	// can say; gone more than half way, from the one it was going to)
	if (instance->module_target != instance->module_state && instance->module_progress >= 0.5f)
	{
		instance->module_state = instance->module_target;
	}
	instance->module_target = (uint8_t)state;
	instance->module_progress = instance->module_state == instance->module_target ? 1.0f : 0.0f;
	instance->module_held_seconds = 0.0f;
	return true;
}

int objects_module_state(objects_t *objects, uint32_t id, int *going_to)
{
	int index = objects_find_id(objects, id);

	if (objects_module_of(objects, index) == NULL)
	{
		return -1;
	}
	if (going_to != NULL)
	{
		*going_to = objects->instances[index].module_target;
	}
	return objects->instances[index].module_state;
}

int objects_nearest_module(const objects_t *objects, float x, float z, float radius)
{
	int nearest = -1;
	float best = radius * radius;

	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		float dx = instance->x - x;
		float dz = instance->z - z;

		if (objects->types.types[instance->type].module >= 0 && dx * dx + dz * dz <= best)
		{
			best = dx * dx + dz * dz;
			nearest = i;
		}
	}
	return nearest;
}

// A module's door part (MODULE_PART_DOOR) by its module's id and its part
// number; -1 if it is none
static int find_door(objects_t *objects, uint32_t module_id, int door_part)
{
	int module_index = objects_find_id(objects, module_id);
	const module_file_t *module = objects_module_of(objects, module_index);

	if (module == NULL || door_part <= 0 || door_part >= module->part_count || (module->parts[door_part].flags & MODULE_PART_DOOR) == 0)
	{
		return -1;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *part = &objects->instances[i];

		if ((part->state & OBJECT_STATE_PART) != 0 && part->module_id == module_id && part->module_part == door_part)
		{
			return i;
		}
	}
	return -1;
}

bool objects_set_door(objects_t *objects, uint32_t module_id, int door_part, bool open)
{
	int part_index = find_door(objects, module_id, door_part);
	const module_file_t *module;
	object_instance_t *part;
	int state;

	if (part_index < 0)
	{
		return false;
	}
	module = objects_module_of(objects, objects_find_id(objects, module_id));
	part = &objects->instances[part_index];
	state = door_state(module, open);
	if (part->module_target == (uint8_t)state)
	{
		return true;
	}
	// (turned back on the way: back from where it got)
	if (part->module_target != part->module_state)
	{
		part->module_state = part->module_target;
		part->module_progress = 1.0f - part->module_progress;
	}
	else
	{
		part->module_progress = 0.0f;
	}
	part->module_target = (uint8_t)state;
	return true;
}

int objects_door(objects_t *objects, uint32_t module_id, int door_part)
{
	int part_index = find_door(objects, module_id, door_part);
	const module_file_t *module;

	if (part_index < 0)
	{
		return -1;
	}
	module = objects_module_of(objects, objects_find_id(objects, module_id));
	return objects->instances[part_index].module_target == (uint8_t)door_state(module, true) ? 1 : 0;
}

int objects_door_find(objects_t *objects, uint32_t module_id, const char *name)
{
	const module_file_t *module = objects_module_of(objects, objects_find_id(objects, module_id));

	if (module == NULL || name == NULL)
	{
		return -1;
	}
	for (int p = 1; p < module->part_count; p++)
	{
		if ((module->parts[p].flags & MODULE_PART_DOOR) != 0 && SDL_strcmp(module->parts[p].name, name) == 0)
		{
			return p;
		}
	}
	return -1;
}

bool objects_door_at(objects_t *objects, vec3_t origin, vec3_t direction, float maximum_distance, uint32_t *module_id_out, int *door_part_out,
	float *distance_out)
{
	float best = maximum_distance;
	bool found = false;

	if (!objects->module_list_valid || objects->module_generation != objects->generation)
	{
		rebuild_list(objects);
	}
	for (int m = 0; m < objects->module_count; m++)
	{
		int first = objects->module_entries[m];
		int last = objects->module_entries[m + 1];
		int module_index = objects->module_indices[first];
		const module_file_t *module = objects_module_of(objects, module_index);

		if (module == NULL)
		{
			continue;
		}
		for (int k = first + 1; k < last; k++)
		{
			const object_instance_t *part = &objects->instances[objects->module_indices[k]];
			const object_type_t *type = &objects->types.types[part->type];
			const object_model_t *model;
			float inverse[4] = { -part->rotation[0], -part->rotation[1], -part->rotation[2], part->rotation[3] };
			float from[3];
			float way[3];
			float local_from[3];
			float local_way[3];
			float near = 0.0f;
			float far = best;
			bool through = true;

			if (part->module_part >= module->part_count || (module->parts[part->module_part].flags & MODULE_PART_DOOR) == 0 || type->model < 0
				|| part->scale <= 0.0f)
			{
				continue;
			}
			model = &objects->models[type->model];
			// The ray in the part's own frame (unturned, unscaled): its
			// model's bounds a box there
			from[0] = (origin.x - part->x) / part->scale;
			from[1] = (origin.y - part->y) / part->scale;
			from[2] = (origin.z - part->z) / part->scale;
			way[0] = direction.x;
			way[1] = direction.y;
			way[2] = direction.z;
			quaternion_rotate(inverse, from, local_from);
			quaternion_rotate(inverse, way, local_way);
			for (int axis = 0; axis < 3 && through; axis++)
			{
				float minimum = (&model->bounds_minimum.x)[axis] - 0.03f;
				float maximum = (&model->bounds_maximum.x)[axis] + 0.03f;

				if (SDL_fabsf(local_way[axis]) < 1e-6f)
				{
					through = local_from[axis] >= minimum && local_from[axis] <= maximum;
				}
				else
				{
					float t0 = (minimum - local_from[axis]) / local_way[axis] * part->scale;
					float t1 = (maximum - local_from[axis]) / local_way[axis] * part->scale;

					near = SDL_max(near, SDL_min(t0, t1));
					far = SDL_min(far, SDL_max(t0, t1));
					through = near <= far;
				}
			}
			if (through && near < best)
			{
				best = near;
				found = true;
				*module_id_out = objects->instances[module_index].id;
				*door_part_out = part->module_part;
				*distance_out = near;
			}
		}
	}
	return found;
}

bool objects_door_info(objects_t *objects, uint32_t module_id, int door_part, objects_door_info_t *info_out)
{
	int module_index = objects_find_id(objects, module_id);
	const module_file_t *module = objects_module_of(objects, module_index);
	int part_index = find_door(objects, module_id, door_part);
	const object_instance_t *module_instance;
	const object_instance_t *part;
	const object_type_t *type;
	const object_model_t *model;
	const module_file_part_t *file_part;
	float closed_angle;
	float open_angle;
	float local_along[3];
	float closed[4];
	float yaw[4];
	float turn[4];
	float along[3];
	float local[3];
	float hinge[3];
	float sign;

	if (part_index < 0)
	{
		return false;
	}
	module_instance = &objects->instances[module_index];
	part = &objects->instances[part_index];
	type = &objects->types.types[part->type];
	if (type->model < 0)
	{
		return false;
	}
	model = &objects->models[type->model];
	file_part = &module->parts[door_part];
	closed_angle = module->poses[door_state(module, false)][door_part].angle_degrees;
	open_angle = module->poses[door_state(module, true)][door_part].angle_degrees;
	// The leaf across its own frame: from its hinge (the origin) along
	// the longer of its bounds' X and Z (a door in a wall that runs along
	// Z lies along Z), whichever way they lie
	if (model->bounds_maximum.x - model->bounds_minimum.x >= model->bounds_maximum.z - model->bounds_minimum.z)
	{
		sign = model->bounds_maximum.x + model->bounds_minimum.x >= 0.0f ? 1.0f : -1.0f;
		local_along[0] = sign;
		local_along[2] = 0.0f;
		info_out->width = (model->bounds_maximum.x - model->bounds_minimum.x) * module_instance->scale;
	}
	else
	{
		sign = model->bounds_maximum.z + model->bounds_minimum.z >= 0.0f ? 1.0f : -1.0f;
		local_along[0] = 0.0f;
		local_along[2] = sign;
		info_out->width = (model->bounds_maximum.z - model->bounds_minimum.z) * module_instance->scale;
	}
	local_along[1] = 0.0f;
	quaternion_axis_angle(file_part->axis, closed_angle * DEGREES_TO_RADIANS, closed);
	quaternion_axis_angle((const float[3]){ 0.0f, 1.0f, 0.0f }, file_part->yaw_degrees * DEGREES_TO_RADIANS, yaw);
	quaternion_multiply(yaw, closed, turn);
	quaternion_multiply(module_instance->rotation, turn, turn);
	quaternion_rotate(turn, local_along, along);
	for (int k = 0; k < 3; k++)
	{
		local[k] = file_part->position[k] * module_instance->scale;
	}
	quaternion_rotate(module_instance->rotation, local, hinge);
	info_out->hinge[0] = module_instance->x + hinge[0];
	info_out->hinge[1] = objects_instance_base_y(module_instance) + hinge[1];
	info_out->hinge[2] = module_instance->z + hinge[2];
	info_out->along[0] = along[0];
	info_out->along[1] = 0.0f;
	info_out->along[2] = along[2];
	{
		float length = SDL_sqrtf(along[0] * along[0] + along[2] * along[2]);

		info_out->along[0] /= SDL_max(length, 1e-6f);
		info_out->along[2] /= SDL_max(length, 1e-6f);
	}
	info_out->height = (model->bounds_maximum.y - model->bounds_minimum.y) * module_instance->scale;
	// (+ round +Y turns along toward up x along = (along.z, 0, -along.x))
	sign = open_angle - closed_angle >= 0.0f ? 1.0f : -1.0f;
	info_out->swing[0] = sign * info_out->along[2];
	info_out->swing[1] = 0.0f;
	info_out->swing[2] = -sign * info_out->along[0];
	info_out->open = objects_door(objects, module_id, door_part) == 1;
	info_out->moving = part->module_state != part->module_target;
	info_out->passage = (file_part->flags & MODULE_PART_WALL_DOOR) == 0;
	return true;
}

int objects_door_list(objects_t *objects, uint32_t *module_ids_out, int *door_parts_out, int maximum)
{
	int count = 0;

	if (objects->types.module_count == 0)
	{
		return 0;
	}
	if (!objects->module_list_valid || objects->module_generation != objects->generation)
	{
		rebuild_list(objects);
	}
	for (int m = 0; m < objects->module_count && count < maximum; m++)
	{
		int first = objects->module_entries[m];
		int last = objects->module_entries[m + 1];
		int module_index = objects->module_indices[first];
		const module_file_t *module = objects_module_of(objects, module_index);

		if (module == NULL)
		{
			continue;
		}
		for (int k = first + 1; k < last && count < maximum; k++)
		{
			const object_instance_t *part = &objects->instances[objects->module_indices[k]];

			if (part->module_part < module->part_count && (module->parts[part->module_part].flags & MODULE_PART_DOOR) != 0)
			{
				module_ids_out[count] = objects->instances[module_index].id;
				door_parts_out[count] = part->module_part;
				count++;
			}
		}
	}
	return count;
}
