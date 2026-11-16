// Local lights (see lights.h): gathered, clustered and uploaded each frame.

#include <SDL3/SDL.h>

#include "decals.h"
#include "lights.h"

// What the light buffer starts with (std430, as light_buffer in lights.glsl):
// the clusters' layout, then the lights three vec4 each
typedef struct
{
	uint32_t tiles_x;
	uint32_t tiles_y;
	uint32_t slices;
	uint32_t count;
	float near_meters;
	float slices_per_log;              // slice = log(distance / near) * this
	float far_meters;
	float darkness;                    // 0 full day .. 1 night (lights_darkness): the lit windows' glow
	float scene_pixels[2];             // the scene pass's width and height (pixels): a pixel's tile from gl_FragCoord (0: none, no clusters)
	float padding[2];
} light_header_t;

typedef struct
{
	float position_radius[4];          // where, and how far it reaches
	float color[4];                    // linear light (color times intensity, flicker in), w unused
	float side[4];                     // xyz: the way it faces; w: above 0, one sided (a window's: lighting only what is in front of the plane that far behind it)
} gpu_light_t;

typedef struct
{
	gpu_light_t light;
	float distance;                    // from the eye, for the nearest first
} gathered_light_t;

// A core as light_core.vert reads it (one instance)
typedef struct
{
	float position_size[4];
	float color[4];
} gpu_core_t;

// core_block in light_core.vert
typedef struct
{
	float camera_right[3];
	float padding_0;
	float camera_up[3];
	float padding_1;
} core_uniforms_t;

#define LIGHT_BUFFER_SIZE (sizeof(light_header_t) + sizeof(gpu_light_t) * LIGHTS_MAX)
// The cluster buffer (cluster_buffer in lights.glsl, decals.glsl): each
// cluster's four words (its lights' first index and count, its decals'),
// the indices of both, the decals as the shaders read them, then the decals'
// atlas (DECALS_ATLAS_SIZE^2 color texels and as many normal ones). The
// offsets are decals.glsl's DECAL_RECORDS and DECAL_ATLAS
#define CLUSTER_WORDS 4
#define DECAL_RECORDS_OFFSET (CLUSTER_WORDS * LIGHTS_CLUSTERS + LIGHTS_INDICES_MAX)
#define DECAL_ATLAS_OFFSET (DECAL_RECORDS_OFFSET + DECALS_MAX * DECALS_RECORD_WORDS)
#define CLUSTER_BUFFER_WORDS (DECAL_ATLAS_OFFSET + 2 * DECALS_ATLAS_SIZE * DECALS_ATLAS_SIZE)
#define CLUSTER_BUFFER_SIZE (sizeof(uint32_t) * CLUSTER_BUFFER_WORDS)
#define OBJECT_SCAN_SECONDS 0.5         // the light-giving objects are found again this often
#define CORE_BRIGHTNESS 4.0f            // a core's light, of what the light gives a meter away
#define CORE_DISTANCE_METERS 200.0f     // no cores farther than this
#define LIGHT_VISIBLE 0.01f             // a light fainter than this (of its own) is left out
#define WINDOW_LIGHT_OUT_METERS 0.5f    // a window's light, this far out in front of its glass
#define WINDOW_LIGHT_UP_METERS 0.0f     // and this much above its middle
#define WINDOW_LIGHT_RADIUS_METERS 6.0f
#define WINDOW_LIGHT_FAR_METERS 90.0f   // no window lights from houses farther than this
static const float window_light_color[3] = { 1.0f, 0.66f, 0.36f };   // a fire's, a candle's, through glass (sRGB)

static lights_t *bound_lights;          // the one the renderers bind (there is one)
// The last update's lights, for lights_light_at (there is one lights_t)
static gpu_light_t frame_lights[LIGHTS_MAX];
static int frame_light_count;

void light_source_reset(light_source_t *source)
{
	SDL_zerop(source);
	source->height_meters = -0.6f;
}

bool light_source_parse(light_source_t *source, const char *key, const char *value, bool *ok)
{
	*ok = true;
	if (SDL_strcmp(key, "light") == 0)
	{
		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;

		*ok = SDL_sscanf(value, " %f , %f , %f", &r, &g, &b) == 3 || SDL_sscanf(value, " %f %f %f", &r, &g, &b) == 3;
		source->color[0] = SDL_clamp(r, 0.0f, 1.0f);
		source->color[1] = SDL_clamp(g, 0.0f, 1.0f);
		source->color[2] = SDL_clamp(b, 0.0f, 1.0f);
		// Given a color, a light by default
		if (source->intensity <= 0.0f)
		{
			source->intensity = 1.0f;
		}
		if (source->radius_meters <= 0.0f)
		{
			source->radius_meters = 6.0f;
		}
		return true;
	}
	if (SDL_strcmp(key, "light_intensity") == 0)
	{
		*ok = SDL_sscanf(value, " %f", &source->intensity) == 1;
		return true;
	}
	if (SDL_strcmp(key, "light_radius") == 0)
	{
		*ok = SDL_sscanf(value, " %f", &source->radius_meters) == 1;
		return true;
	}
	if (SDL_strcmp(key, "light_flicker") == 0)
	{
		*ok = SDL_sscanf(value, " %f", &source->flicker) == 1;
		source->flicker = SDL_clamp(source->flicker, 0.0f, 1.0f);
		return true;
	}
	if (SDL_strcmp(key, "light_height") == 0)
	{
		*ok = SDL_sscanf(value, " %f", &source->height_meters) == 1;
		return true;
	}
	if (SDL_strcmp(key, "light_core") == 0)
	{
		*ok = SDL_sscanf(value, " %f", &source->core_meters) == 1;
		source->core_meters = SDL_max(source->core_meters, 0.0f);
		return true;
	}
	if (SDL_strcmp(key, "light_switched") == 0)
	{
		*ok = SDL_strcasecmp(value, "true") == 0 || SDL_strcasecmp(value, "false") == 0
			|| SDL_strcmp(value, "1") == 0 || SDL_strcmp(value, "0") == 0;
		source->switched = SDL_strcasecmp(value, "true") == 0 || SDL_strcmp(value, "1") == 0;
		return true;
	}
	return false;
}

bool lights_create(lights_t *lights)
{
	light_header_t header;

	SDL_zerop(lights);
	lights->light_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, (uint32_t)LIGHT_BUFFER_SIZE);
	lights->cluster_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, (uint32_t)CLUSTER_BUFFER_SIZE);
	lights->core_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(gpu_core_t) * LIGHTS_MAX));
	{
		static const SDL_GPUVertexBufferDescription core_input = { 0, sizeof(gpu_core_t), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 };
		static const SDL_GPUVertexAttribute core_attributes[2] =
		{
			{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
			{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
		};
		SDL_GPUShader *vertex = gpu_load_shader("light_core.vert", 0, 0, 2);
		SDL_GPUShader *fragment = gpu_load_shader("light_core.frag", 0, 0, 0);
		gpu_pipeline_info_t info;

		if (vertex != NULL && fragment != NULL)
		{
			SDL_zero(info);
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			info.vertex_buffers = &core_input;
			info.vertex_buffer_count = 1;
			info.vertex_attributes = core_attributes;
			info.vertex_attribute_count = 2;
			info.primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
			info.cull = SDL_GPU_CULLMODE_NONE;
			info.depth_test = true;
			info.depth_write = false;
			info.blend = GPU_BLEND_PREMULTIPLIED;
			lights->core_pipeline = gpu_make_pipeline(&info, "light cores");
		}
		if (vertex != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, vertex);
		}
		if (fragment != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, fragment);
		}
		// Without the cores the lights still light (the log says why)
	}
	if (lights->light_buffer == NULL || lights->cluster_buffer == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Lights: no buffers: %s", SDL_GetError());
		lights_destroy(lights);
		return false;
	}
	// No lights until the first update
	SDL_zero(header);
	header.tiles_x = LIGHTS_TILES_X;
	header.tiles_y = LIGHTS_TILES_Y;
	header.slices = LIGHTS_SLICES;
	gpu_upload_buffer(lights->light_buffer, 0, &header, sizeof(header));
	{
		uint32_t *zeros = SDL_calloc(CLUSTER_WORDS * LIGHTS_CLUSTERS, sizeof(uint32_t));

		if (zeros != NULL)
		{
			gpu_upload_buffer(lights->cluster_buffer, 0, zeros, sizeof(uint32_t) * CLUSTER_WORDS * LIGHTS_CLUSTERS);
			SDL_free(zeros);
		}
	}
	lights->created = true;
	bound_lights = lights;
	return true;
}

void lights_destroy(lights_t *lights)
{
	if (lights->light_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, lights->light_buffer);
	}
	if (lights->cluster_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, lights->cluster_buffer);
	}
	if (lights->core_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, lights->core_buffer);
	}
	if (lights->core_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, lights->core_pipeline);
	}
	SDL_free(lights->lit_objects);
	SDL_free(lights->window_objects);
	if (bound_lights == lights)
	{
		bound_lights = NULL;
	}
	SDL_zerop(lights);
}

void lights_bind(SDL_GPURenderPass *pass)
{
	SDL_GPUBuffer *buffers[LIGHTS_STORAGE_BUFFERS];

	if (pass == NULL || bound_lights == NULL || !bound_lights->created)
	{
		return;
	}
	buffers[0] = bound_lights->light_buffer;
	buffers[1] = bound_lights->cluster_buffer;
	SDL_BindGPUFragmentStorageBuffers(pass, 0, buffers, LIGHTS_STORAGE_BUFFERS);
}

// How bright a flickering light is now (0..1 of its own), and a little
// sway of where it is: a few sines at odd rates, shifted by its seed
static float flicker_at(float flicker, double seconds, float seed, float sway_out[3])
{
	float t = (float)SDL_fmod(seconds, 10000.0);
	float wave = 0.5f + 0.5f * (0.5f * SDL_sinf(t * 9.1f + seed) + 0.3f * SDL_sinf(t * 23.3f + seed * 1.7f)
		+ 0.2f * SDL_sinf(t * 41.9f + seed * 2.3f));

	sway_out[0] = flicker * 0.04f * SDL_sinf(t * 7.3f + seed * 3.1f);
	sway_out[1] = flicker * 0.03f * SDL_sinf(t * 5.9f + seed * 1.3f);
	sway_out[2] = flicker * 0.04f * SDL_sinf(t * 6.7f + seed * 2.9f);
	return 1.0f - flicker * 0.45f * wave;
}

static float srgb_to_linear(float value)
{
	return SDL_powf(SDL_max(value, 0.0f), 2.2f);
}

// The cores of this frame (filled as the lights are gathered)
static gpu_core_t frame_cores[LIGHTS_MAX];
static int frame_core_count;

// One light into the list (if it reaches the eye's surroundings at all),
// and its core: strength for its light, core_strength for its core (the
// sun drowns the light but a flame still shows by day)
static void gather(gathered_light_t *list, int *count, int capacity, const light_source_t *source, const float position[3],
	const float side[4], float strength, float core_strength, float seed, const float eye[3], double seconds, int *dropped)
{
	gathered_light_t *entry;
	float sway[3];
	float flicker;
	float distance;
	float linear[3];

	distance = SDL_sqrtf((position[0] - eye[0]) * (position[0] - eye[0]) + (position[1] - eye[1]) * (position[1] - eye[1])
		+ (position[2] - eye[2]) * (position[2] - eye[2]));
	flicker = flicker_at(source->flicker, seconds, seed, sway);
	for (int k = 0; k < 3; k++)
	{
		linear[k] = srgb_to_linear(source->color[k]) * source->intensity * flicker;
	}
	if (source->core_meters > 0.0f && core_strength >= LIGHT_VISIBLE && distance < CORE_DISTANCE_METERS && frame_core_count < LIGHTS_MAX)
	{
		gpu_core_t *core = &frame_cores[frame_core_count++];

		core->position_size[0] = position[0] + sway[0];
		core->position_size[1] = position[1] + sway[1];
		core->position_size[2] = position[2] + sway[2];
		core->position_size[3] = source->core_meters;
		for (int k = 0; k < 3; k++)
		{
			core->color[k] = linear[k] * core_strength * CORE_BRIGHTNESS;
		}
		core->color[3] = 0.0f;
	}
	if (strength < LIGHT_VISIBLE || distance - source->radius_meters > LIGHTS_FAR_METERS)
	{
		return;
	}
	if (*count == capacity)
	{
		(*dropped)++;
		return;
	}
	entry = &list[(*count)++];
	entry->distance = distance;
	entry->light.position_radius[0] = position[0] + sway[0];
	entry->light.position_radius[1] = position[1] + sway[1];
	entry->light.position_radius[2] = position[2] + sway[2];
	entry->light.position_radius[3] = source->radius_meters;
	for (int k = 0; k < 3; k++)
	{
		entry->light.color[k] = linear[k] * strength;
	}
	entry->light.color[3] = 0.0f;
	entry->light.side[0] = side != NULL ? side[0] : 0.0f;
	entry->light.side[1] = side != NULL ? side[1] : 0.0f;
	entry->light.side[2] = side != NULL ? side[2] : 0.0f;
	entry->light.side[3] = side != NULL ? side[3] : 0.0f;
}

float lights_darkness(float sun_elevation_degrees)
{
	float t = SDL_clamp((8.0f - sun_elevation_degrees) / 12.0f, 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

void lights_set_lamps(lights_t *lights, lights_lamps_t lamps)
{
	lights->lamps = lamps;
}

bool lights_place(lights_t *lights, const float position[3], const light_source_t *source)
{
	if (lights->placed_count == LIGHTS_PLACED_MAX || !light_source_gives_light(source))
	{
		return false;
	}
	lights->placed[lights->placed_count].source = *source;
	SDL_memcpy(lights->placed[lights->placed_count].position, position, sizeof(float) * 3);
	lights->placed_count++;
	return true;
}

bool lights_remove_near(lights_t *lights, const float position[3], float meters)
{
	int nearest = -1;
	float nearest_squared = meters * meters;

	for (int i = 0; i < lights->placed_count; i++)
	{
		const float *p = lights->placed[i].position;
		float squared = (p[0] - position[0]) * (p[0] - position[0]) + (p[1] - position[1]) * (p[1] - position[1])
			+ (p[2] - position[2]) * (p[2] - position[2]);

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
	lights->placed[nearest] = lights->placed[--lights->placed_count];
	return true;
}

void lights_clear_placed(lights_t *lights)
{
	lights->placed_count = 0;
}

// Whether a lamp burns now: automatic, each is lit at its own moment of
// the dusk (and put out at its own of the dawn), by its seed
void lights_set_carried(lights_t *lights, uint32_t key, const float position[3], const light_source_t *source)
{
	moving_light_t *free_slot = NULL;

	for (int i = 0; i < LIGHTS_MOVING_MAX; i++)
	{
		moving_light_t *moving = &lights->moving[i];

		if (moving->used && moving->key == key)
		{
			moving->source = *source;
			SDL_memcpy(moving->position, position, sizeof(moving->position));
			return;
		}
		if (!moving->used && free_slot == NULL)
		{
			free_slot = moving;
		}
	}
	if (free_slot == NULL || key == 0)
	{
		return;
	}
	SDL_zerop(free_slot);
	free_slot->used = true;
	free_slot->key = key;
	free_slot->source = *source;
	SDL_memcpy(free_slot->position, position, sizeof(free_slot->position));
}

void lights_remove_carried(lights_t *lights, uint32_t key)
{
	for (int i = 0; i < LIGHTS_MOVING_MAX; i++)
	{
		if (lights->moving[i].used && lights->moving[i].key == key && key != 0)
		{
			lights->moving[i].used = false;
		}
	}
}

bool lights_has_carried(const lights_t *lights, uint32_t key)
{
	for (int i = 0; i < LIGHTS_MOVING_MAX; i++)
	{
		if (lights->moving[i].used && lights->moving[i].key == key && key != 0)
		{
			return true;
		}
	}
	return false;
}

bool lights_flash(lights_t *lights, const float position[3], const light_source_t *source, float seconds, double now)
{
	for (int i = 0; i < LIGHTS_MOVING_MAX; i++)
	{
		moving_light_t *moving = &lights->moving[i];

		if (!moving->used)
		{
			SDL_zerop(moving);
			moving->used = true;
			moving->source = *source;
			SDL_memcpy(moving->position, position, sizeof(moving->position));
			moving->start_seconds = now;
			moving->life_seconds = SDL_max(seconds, 0.01f);
			return true;
		}
	}
	return false;
}

static bool lamp_lit(const lights_t *lights, float darkness, float seed)
{
	float moment;

	if (lights->lamps != LIGHTS_LAMPS_AUTOMATIC)
	{
		return lights->lamps == LIGHTS_LAMPS_ON;
	}
	moment = 0.3f + 0.4f * (0.5f + 0.5f * SDL_sinf(seed * 12.9898f));
	return darkness > moment;
}

static int by_distance(const void *a, const void *b)
{
	float da = ((const gathered_light_t *)a)->distance;
	float db = ((const gathered_light_t *)b)->distance;

	return da < db ? -1 : (da > db ? 1 : 0);
}

// The objects whose type gives light: their instance indices (again when
// the objects change or now and then)
static bool push_index(int **list, int *count, int *capacity, int index)
{
	if (*count == *capacity)
	{
		int grown_capacity = SDL_max(64, *capacity * 2);
		int *grown = SDL_realloc(*list, sizeof(int) * (size_t)grown_capacity);

		if (grown == NULL)
		{
			return false;
		}
		*list = grown;
		*capacity = grown_capacity;
	}
	(*list)[(*count)++] = index;
	return true;
}

// The objects near the eye that give light (their type's) or have windows
// (emissive groups named "window"): their instance indices. Within
// LIGHTS_FAR_METERS and a margin (the eye goes on a few meters before the
// next look, a light reaches out its radius), by the objects' grid: not
// every object of a big map twice a second
#define OBJECT_SCAN_MARGIN_METERS 60.0f

static bool scan_one(const objects_t *objects, int i, void *user)
{
	lights_t *lights = user;
	const object_instance_t *instance = &objects->instances[i];
	int groups;

	if (objects_instance_model(objects, instance) < 0)
	{
		return true;
	}
	groups = objects_emissive_group_count(objects, i);

	for (int g = 1; g <= groups; g++)
	{
		if (SDL_strncmp(objects_emissive_group(objects, i, g)->name, "window", 6) == 0)
		{
			push_index(&lights->window_objects, &lights->window_object_count, &lights->window_object_capacity, i);
			break;
		}
	}
	if (light_source_gives_light(&objects->types.types[instance->type].light))
	{
		push_index(&lights->lit_objects, &lights->lit_object_count, &lights->lit_object_capacity, i);
	}
	return true;
}

static void scan_objects(lights_t *lights, const objects_t *objects, const float eye[3])
{
	lights->window_object_count = 0;
	lights->lit_object_count = 0;
	objects_visit_circle(objects, eye[0], eye[2], LIGHTS_FAR_METERS + OBJECT_SCAN_MARGIN_METERS, scan_one, lights);
}

// A vector turned by a quaternion (x, y, z, w): an instance's rotation
static void quaternion_turn(const float q[4], const float v[3], float out[3])
{
	// v + 2 q.xyz x (q.xyz x v + w v)
	float c[3] = { q[1] * v[2] - q[2] * v[1] + q[3] * v[0], q[2] * v[0] - q[0] * v[2] + q[3] * v[1], q[0] * v[1] - q[1] * v[0] + q[3] * v[2] };

	out[0] = v[0] + 2.0f * (q[1] * c[2] - q[2] * c[1]);
	out[1] = v[1] + 2.0f * (q[2] * c[0] - q[0] * c[2]);
	out[2] = v[2] + 2.0f * (q[0] * c[1] - q[1] * c[0]);
}

// The way a window faces out (model space, level): as its record says
// (OBJECT_EMISSIVE_FACING), else towards the side of its model's bounds it
// is nearest (older files: wrong for a wing's, a porch's, a yard's)
static void window_outward(const object_model_t *model, const object_file_emissive_group_t *group, float out[3])
{
	const float *center = group->center;
	float to_right = model->bounds_maximum.x - center[0];
	float to_left = center[0] - model->bounds_minimum.x;
	float to_front = model->bounds_maximum.z - center[2];
	float to_back = center[2] - model->bounds_minimum.z;
	float nearest = SDL_min(SDL_min(to_right, to_left), SDL_min(to_front, to_back));

	out[0] = 0.0f;
	out[1] = 0.0f;
	out[2] = 0.0f;
	if ((group->flags & OBJECT_EMISSIVE_FACING) != 0)
	{
		uint32_t step = (group->flags & OBJECT_EMISSIVE_FACING_MASK) >> OBJECT_EMISSIVE_FACING_SHIFT;
		float yaw = (float)step * (2.0f * SDL_PI_F / 256.0f);

		out[0] = SDL_sinf(yaw);
		out[2] = SDL_cosf(yaw);
		return;
	}
	if (nearest == to_front)
	{
		out[2] = 1.0f;
	}
	else if (nearest == to_back)
	{
		out[2] = -1.0f;
	}
	else if (nearest == to_right)
	{
		out[0] = 1.0f;
	}
	else
	{
		out[0] = -1.0f;
	}
}

// The lit windows of the houses near the eye: a one sided light each, a
// little out in front of the glass and up, facing out
static void gather_windows(lights_t *lights, const objects_t *objects, gathered_light_t *list, int *count, int capacity,
	float strength, float intensity, const float eye[3], double seconds)
{
	light_source_t source;

	if (strength * intensity < LIGHT_VISIBLE)
	{
		return;
	}
	light_source_reset(&source);
	source.color[0] = window_light_color[0];
	source.color[1] = window_light_color[1];
	source.color[2] = window_light_color[2];
	source.intensity = intensity;
	source.radius_meters = WINDOW_LIGHT_RADIUS_METERS;
	for (int w = 0; w < lights->window_object_count; w++)
	{
		int index = lights->window_objects[w];
		const object_instance_t *instance;
		const object_model_t *model;
		int model_index;
		int groups;
		float dx;
		float dz;
		float reach;

		if (index >= objects->instance_count)
		{
			continue;
		}
		instance = &objects->instances[index];
		model_index = objects_instance_model(objects, instance);
		if (model_index < 0 || instance->damage == OBJECT_DAMAGE_DESTROYED)
		{
			continue;
		}
		model = &objects->models[model_index];
		dx = instance->x - eye[0];
		dz = instance->z - eye[2];
		reach = WINDOW_LIGHT_FAR_METERS + model->horizontal_radius * instance->scale;
		if (dx * dx + dz * dz > reach * reach)
		{
			continue;
		}
		groups = objects_emissive_group_count(objects, index);
		for (int g = 1; g <= groups; g++)
		{
			const object_file_emissive_group_t *group = objects_emissive_group(objects, index, g);
			float outward[3];
			float turned_center[3];
			float turned_out[3];
			float scaled[3];
			float position[3];
			float side[4];

			if (SDL_strncmp(group->name, "window", 6) != 0 || !objects_emissive_lit(objects, index, g))
			{
				continue;
			}
			window_outward(model, group, outward);
			scaled[0] = group->center[0] * instance->scale;
			scaled[1] = group->center[1] * instance->scale;
			scaled[2] = group->center[2] * instance->scale;
			quaternion_turn(instance->rotation, scaled, turned_center);
			quaternion_turn(instance->rotation, outward, turned_out);
			position[0] = instance->x + turned_center[0] + turned_out[0] * WINDOW_LIGHT_OUT_METERS;
			position[1] = objects_instance_base_y(instance) + turned_center[1] + WINDOW_LIGHT_UP_METERS;
			position[2] = instance->z + turned_center[2] + turned_out[2] * WINDOW_LIGHT_OUT_METERS;
			// (its plane: the glass's, a little behind the wall's face)
			side[0] = turned_out[0];
			side[1] = turned_out[1];
			side[2] = turned_out[2];
			side[3] = WINDOW_LIGHT_OUT_METERS;
			gather(list, count, capacity, &source, position, side, strength, 0.0f, (float)(index % 89) * 0.53f + (float)g, eye, seconds,
				&lights->dropped);
		}
	}
}

// How strong an effect's light is at its age: a fire comes up as its flames
// do, a burst flashes and dies with its particles
static float effect_strength(const particles_effect_t *effect, float age)
{
	float life = SDL_max(effect->life_maximum, 0.05f);

	if (!effect->burst)
	{
		return SDL_min(age / (0.5f * life), 1.0f);
	}
	if (age < effect->duration_seconds)
	{
		return SDL_min(age / SDL_max(0.5f * life, 0.02f), 1.0f);
	}
	return SDL_max(1.0f - (age - effect->duration_seconds) / life, 0.0f);
}

// Where a point lands on the screen, as the shaders find their cluster
// (view_projection as GLSL reads it, column after column); false behind
static bool to_screen(const float view_projection[16], const float point[3], float *x_out, float *y_out)
{
	float clip[4];

	for (int i = 0; i < 4; i++)
	{
		clip[i] = view_projection[i] * point[0] + view_projection[4 + i] * point[1] + view_projection[8 + i] * point[2] + view_projection[12 + i];
	}
	if (clip[3] < 0.05f)
	{
		return false;
	}
	*x_out = clip[0] / clip[3];
	*y_out = clip[1] / clip[3];
	return true;
}

static int slice_of(float distance, float slices_per_log)
{
	int slice;

	if (distance <= LIGHTS_NEAR_METERS)
	{
		return 0;
	}
	slice = (int)(SDL_logf(distance / LIGHTS_NEAR_METERS) * slices_per_log);
	return SDL_clamp(slice, 0, LIGHTS_SLICES - 1);
}

// Spheres (center and radius, and how far each is from the eye) into the
// clusters: the screen rectangle each one's bounding box covers (all of it
// when the box reaches behind the eye), the slices from its nearest to its
// farthest distance. word: which pair of a cluster's four words (0 the
// lights', 2 the decals'); their indices go after the first total ones.
// Returns the indices used in all (a cluster that would overflow the room
// keeps what it has room for)
static uint32_t cluster_spheres(uint32_t *cluster_data, const float (*spheres)[4], const float *distances, int count, const float view_projection[16],
	float slices_per_log, int word, uint32_t total, int *dropped)
{
	static uint32_t cluster_counts[LIGHTS_CLUSTERS];
	static int tiles[DECALS_MAX > LIGHTS_MAX ? DECALS_MAX : LIGHTS_MAX][4];
	static int slices[DECALS_MAX > LIGHTS_MAX ? DECALS_MAX : LIGHTS_MAX][2];
	uint32_t used = 0;

	SDL_zeroa(cluster_counts);
	for (int l = 0; l < count; l++)
	{
		const float *center = spheres[l];
		float radius = center[3];
		float minimum_x = 1.0f;
		float maximum_x = -1.0f;
		float minimum_y = 1.0f;
		float maximum_y = -1.0f;
		bool whole = distances[l] <= radius * 1.8f;

		for (int corner = 0; corner < 8 && !whole; corner++)
		{
			float point[3] = { center[0] + ((corner & 1) ? radius : -radius), center[1] + ((corner & 2) ? radius : -radius),
				center[2] + ((corner & 4) ? radius : -radius) };
			float x;
			float y;

			if (!to_screen(view_projection, point, &x, &y))
			{
				whole = true;
				break;
			}
			minimum_x = SDL_min(minimum_x, x);
			maximum_x = SDL_max(maximum_x, x);
			minimum_y = SDL_min(minimum_y, y);
			maximum_y = SDL_max(maximum_y, y);
		}
		if (whole)
		{
			minimum_x = -1.0f;
			maximum_x = 1.0f;
			minimum_y = -1.0f;
			maximum_y = 1.0f;
		}
		if (maximum_x < -1.0f || minimum_x > 1.0f || maximum_y < -1.0f || minimum_y > 1.0f)
		{
			tiles[l][0] = 1;                // off the screen: no tiles
			tiles[l][2] = 0;
			continue;
		}
		tiles[l][0] = SDL_clamp((int)((minimum_x * 0.5f + 0.5f) * LIGHTS_TILES_X), 0, LIGHTS_TILES_X - 1);
		tiles[l][2] = SDL_clamp((int)((maximum_x * 0.5f + 0.5f) * LIGHTS_TILES_X), 0, LIGHTS_TILES_X - 1);
		tiles[l][1] = SDL_clamp((int)((minimum_y * 0.5f + 0.5f) * LIGHTS_TILES_Y), 0, LIGHTS_TILES_Y - 1);
		tiles[l][3] = SDL_clamp((int)((maximum_y * 0.5f + 0.5f) * LIGHTS_TILES_Y), 0, LIGHTS_TILES_Y - 1);
		slices[l][0] = slice_of(distances[l] - radius, slices_per_log);
		slices[l][1] = slice_of(distances[l] + radius, slices_per_log);
		// Not enough room left for all its clusters: left out whole, not in
		// some of them (the room went to the clusters in their order: a
		// light cut off along the tiles' rows, the top of a lit wall gone
		// where a scope or a crowd of windows filled the clusters). The
		// spheres come nearest first: the far ones go
		{
			uint32_t cost = (uint32_t)(tiles[l][2] - tiles[l][0] + 1) * (uint32_t)(tiles[l][3] - tiles[l][1] + 1)
				* (uint32_t)(slices[l][1] - slices[l][0] + 1);

			if (total + used + cost > LIGHTS_INDICES_MAX)
			{
				tiles[l][0] = 1;
				tiles[l][2] = 0;
				(*dropped)++;
				continue;
			}
			used += cost;
		}
		for (int s = slices[l][0]; s <= slices[l][1]; s++)
		{
			for (int y = tiles[l][1]; y <= tiles[l][3]; y++)
			{
				for (int x = tiles[l][0]; x <= tiles[l][2]; x++)
				{
					cluster_counts[(s * LIGHTS_TILES_Y + y) * LIGHTS_TILES_X + x]++;
				}
			}
		}
	}
	for (int c = 0; c < LIGHTS_CLUSTERS; c++)
	{
		uint32_t room = total < LIGHTS_INDICES_MAX ? LIGHTS_INDICES_MAX - total : 0;
		uint32_t take = SDL_min(cluster_counts[c], room);

		cluster_data[CLUSTER_WORDS * c + word] = CLUSTER_WORDS * LIGHTS_CLUSTERS + total;
		cluster_data[CLUSTER_WORDS * c + word + 1] = 0;
		if (take < cluster_counts[c])
		{
			(*dropped)++;
		}
		cluster_counts[c] = take;
		total += take;
	}
	for (int l = 0; l < count; l++)
	{
		for (int s = slices[l][0]; tiles[l][0] <= tiles[l][2] && s <= slices[l][1]; s++)
		{
			for (int y = tiles[l][1]; y <= tiles[l][3]; y++)
			{
				for (int x = tiles[l][0]; x <= tiles[l][2]; x++)
				{
					int c = (s * LIGHTS_TILES_Y + y) * LIGHTS_TILES_X + x;
					uint32_t *first = &cluster_data[CLUSTER_WORDS * c + word];

					if (first[1] < cluster_counts[c])
					{
						cluster_data[first[0] + first[1]] = (uint32_t)l;
						first[1]++;
					}
				}
			}
		}
	}
	return total;
}

void lights_update(lights_t *lights, const particles_t *particles, const objects_t *objects, const decals_t *decals, const lights_frame_t *frame)
{
	const float *view_projection = frame->view_projection;
	const float *eye = frame->eye;
	double seconds = frame->seconds;
	// By day the sun drowns them (fires too: their flames still show)
	float intensity_scale = frame->intensity_scale * frame->darkness;
	float core_scale = frame->intensity_scale;
	static gathered_light_t gathered[LIGHTS_MAX * 4];
	static uint8_t light_buffer[LIGHT_BUFFER_SIZE];
	static uint32_t cluster_data[DECAL_ATLAS_OFFSET];
	static float spheres[DECALS_MAX > LIGHTS_MAX ? DECALS_MAX : LIGHTS_MAX][4];
	static float distances[DECALS_MAX > LIGHTS_MAX ? DECALS_MAX : LIGHTS_MAX];
	int decal_count;
	light_header_t *header = (light_header_t *)light_buffer;
	gpu_light_t *gpu_lights = (gpu_light_t *)(light_buffer + sizeof(light_header_t));
	float slices_per_log = (float)LIGHTS_SLICES / SDL_logf(LIGHTS_FAR_METERS / LIGHTS_NEAR_METERS);
	int count = 0;
	int capacity = (int)SDL_arraysize(gathered);
	uint32_t total = 0;

	if (!lights->created)
	{
		return;
	}
	lights->dropped = 0;
	frame_core_count = 0;
	if (frame->enabled && core_scale >= LIGHT_VISIBLE)
	{
		// The fires and glows of the particle effects
		for (int e = 0; particles != NULL && e < PARTICLES_EMITTERS_MAX; e++)
		{
			const particles_emitter_t *emitter = &particles->emitters[e];
			const particles_effect_t *effect;
			float position[3];

			if (!emitter->used || emitter->effect < 0 || emitter->effect >= particles->effect_count)
			{
				continue;
			}
			effect = &particles->effects[emitter->effect];
			if (!light_source_gives_light(&effect->light))
			{
				continue;
			}
			position[0] = emitter->position[0];
			position[1] = emitter->position[1] + SDL_max(effect->light.height_meters, 0.0f);
			position[2] = emitter->position[2];
			gather(gathered, &count, capacity, &effect->light, position, NULL, effect_strength(effect, emitter->age_seconds) * intensity_scale,
				effect_strength(effect, emitter->age_seconds) * core_scale,
				(float)(emitter->id % 97u) * 0.37f, eye, seconds, &lights->dropped);
		}
		// The objects that give light
		if (objects != NULL && !objects->hidden)
		{
			if (seconds >= lights->next_object_scan_seconds || objects->instance_count != lights->scanned_instance_count)
			{
				scan_objects(lights, objects, eye);
				lights->next_object_scan_seconds = seconds + OBJECT_SCAN_SECONDS;
				lights->scanned_instance_count = objects->instance_count;
			}
			for (int i = 0; i < lights->lit_object_count; i++)
			{
				int index = lights->lit_objects[i];
				const object_instance_t *instance;
				const object_type_t *type;
				float position[3];
				float height;

				if (index >= objects->instance_count)
				{
					continue;
				}
				instance = &objects->instances[index];
				type = &objects->types.types[instance->type];
				if (!light_source_gives_light(&type->light) || instance->damage == OBJECT_DAMAGE_DESTROYED)
				{
					continue;
				}
				// Where it is told: meters, or below 0 that much of the model's
				// height (a lantern's glass, about its middle; a torch's top)
				height = type->light.height_meters >= 0.0f ? type->light.height_meters
					: (type->model >= 0 ? objects->models[type->model].bounds_maximum.y * SDL_min(-type->light.height_meters, 1.0f) : 0.5f);
				position[0] = instance->x;
				position[1] = objects_instance_base_y(instance) + height * instance->scale;
				position[2] = instance->z;
				if (type->light.switched && !lamp_lit(lights, frame->darkness, (float)(index % 89) * 0.53f))
				{
					continue;
				}
				gather(gathered, &count, capacity, &type->light, position, NULL, intensity_scale, core_scale, (float)(index % 89) * 0.53f, eye,
					seconds, &lights->dropped);
			}
		}
		// The houses' lit windows
		if (objects != NULL && !objects->hidden && frame->window_lights)
		{
			gather_windows(lights, objects, gathered, &count, capacity, intensity_scale, frame->window_light_intensity, eye, seconds);
		}
		// The carried ones and the flashes (a flash fades out over its life)
		for (int i = 0; i < LIGHTS_MOVING_MAX; i++)
		{
			moving_light_t *moving = &lights->moving[i];
			float fade = 1.0f;

			if (!moving->used)
			{
				continue;
			}
			if (moving->key == 0)
			{
				float age = (float)(seconds - moving->start_seconds);

				if (age >= moving->life_seconds || age < -1.0f)
				{
					moving->used = false;
					continue;
				}
				fade = 1.0f - SDL_max(age, 0.0f) / SDL_max(moving->life_seconds, 1e-3f);
			}
			gather(gathered, &count, capacity, &moving->source, moving->position, NULL, intensity_scale * fade, core_scale * fade, (float)i * 0.61f + 0.2f,
				eye, seconds, &lights->dropped);
		}
		// The placed ones (lamps too)
		for (int i = 0; i < lights->placed_count; i++)
		{
			const placed_light_t *placed = &lights->placed[i];
			float seed = (float)i * 0.71f + 0.3f;

			if (placed->source.switched && !lamp_lit(lights, frame->darkness, seed))
			{
				continue;
			}
			gather(gathered, &count, capacity, &placed->source, placed->position, NULL, intensity_scale, core_scale, seed, eye, seconds,
				&lights->dropped);
		}
	}
	// The nearest first (the clusters' room goes to them first, see
	// cluster_spheres), as many as there is room for
	SDL_qsort(gathered, (size_t)count, sizeof(gathered_light_t), by_distance);
	if (count > LIGHTS_MAX)
	{
		lights->dropped += count - LIGHTS_MAX;
		count = LIGHTS_MAX;
	}

	// The lights into the clusters, then the decals in view (the clusters'
	// words cleared: without decals theirs stay 0)
	SDL_memset(cluster_data, 0, sizeof(uint32_t) * CLUSTER_WORDS * LIGHTS_CLUSTERS);
	for (int l = 0; l < count; l++)
	{
		SDL_memcpy(spheres[l], gathered[l].light.position_radius, sizeof(spheres[l]));
		distances[l] = gathered[l].distance;
	}
	total = cluster_spheres(cluster_data, (const float (*)[4])spheres, distances, count, view_projection, slices_per_log, 0, 0, &lights->dropped);
	decal_count = 0;
	if (decals != NULL && frame->enabled_decals)
	{
		for (int i = 0; i < DECALS_MAX && decal_count < DECALS_MAX; i++)
		{
			const decal_t *decal = &decals->list[i];
			float to_eye[3] = { decal->center[0] - eye[0], decal->center[1] - eye[1], decal->center[2] - eye[2] };
			float distance = SDL_sqrtf(to_eye[0] * to_eye[0] + to_eye[1] * to_eye[1] + to_eye[2] * to_eye[2]);
			float fade;

			if (!decal->used || distance > DECALS_FAR_METERS)
			{
				continue;
			}
			// (fading out over the last third of the way, and with its age)
			fade = SDL_clamp((DECALS_FAR_METERS - distance) / (DECALS_FAR_METERS / 3.0f), 0.0f, 1.0f) * decals_age_fade(decals, decal, seconds);
			if (fade <= 0.0f)
			{
				continue;
			}
			spheres[decal_count][0] = decal->center[0];
			spheres[decal_count][1] = decal->center[1];
			spheres[decal_count][2] = decal->center[2];
			spheres[decal_count][3] = SDL_sqrtf(decal->half_sizes[0] * decal->half_sizes[0] + decal->half_sizes[1] * decal->half_sizes[1]
				+ decal->half_sizes[2] * decal->half_sizes[2]);
			distances[decal_count] = distance;
			decals_pack(decals, decal, fade, &cluster_data[DECAL_RECORDS_OFFSET + decal_count * DECALS_RECORD_WORDS]);
			decal_count++;
		}
		total = cluster_spheres(cluster_data, (const float (*)[4])spheres, distances, decal_count, view_projection, slices_per_log, 2, total, &lights->dropped);
		// The atlas, once (or when it is another one)
		if (lights->sent_atlas != decals->atlas && decals->atlas != NULL)
		{
			gpu_upload_buffer(lights->cluster_buffer, sizeof(uint32_t) * DECAL_ATLAS_OFFSET, decals->atlas,
				sizeof(uint32_t) * 2 * DECALS_ATLAS_SIZE * DECALS_ATLAS_SIZE);
			lights->sent_atlas = decals->atlas;
		}
	}
	lights->decal_count = decal_count;

	SDL_zerop(header);
	header->tiles_x = LIGHTS_TILES_X;
	header->tiles_y = LIGHTS_TILES_Y;
	header->slices = LIGHTS_SLICES;
	header->count = (uint32_t)count;
	header->near_meters = LIGHTS_NEAR_METERS;
	header->slices_per_log = slices_per_log;
	header->far_meters = LIGHTS_FAR_METERS;
	header->darkness = frame->darkness;
	header->scene_pixels[0] = frame->scene_pixels[0];
	header->scene_pixels[1] = frame->scene_pixels[1];
	for (int l = 0; l < count; l++)
	{
		gpu_lights[l] = gathered[l].light;
		frame_lights[l] = gathered[l].light;
	}
	frame_light_count = count;
	lights->core_count = frame_core_count;
	lights->count = count;
	if (lights->core_count > 0 && lights->core_buffer != NULL)
	{
		gpu_upload_buffer(lights->core_buffer, 0, frame_cores, sizeof(gpu_core_t) * (size_t)lights->core_count);
	}
	gpu_upload_buffer(lights->light_buffer, 0, light_buffer, sizeof(light_header_t) + sizeof(gpu_light_t) * (size_t)SDL_max(count, 1));
	gpu_upload_buffer(lights->cluster_buffer, 0, cluster_data, sizeof(uint32_t) * (CLUSTER_WORDS * LIGHTS_CLUSTERS + SDL_max(total, 1u)));
	if (decal_count > 0)
	{
		gpu_upload_buffer(lights->cluster_buffer, sizeof(uint32_t) * DECAL_RECORDS_OFFSET, &cluster_data[DECAL_RECORDS_OFFSET],
			sizeof(uint32_t) * DECALS_RECORD_WORDS * (size_t)decal_count);
	}
}

float lights_light_at(const lights_t *lights, const float point[3])
{
	float total = 0.0f;

	(void)lights;
	for (int l = 0; l < frame_light_count; l++)
	{
		const float *position_radius = frame_lights[l].position_radius;
		const float *color = frame_lights[l].color;
		float dx = position_radius[0] - point[0];
		float dy = position_radius[1] - point[1];
		float dz = position_radius[2] - point[2];
		float distance_squared = dx * dx + dy * dy + dz * dz;
		float radius_squared = position_radius[3] * position_radius[3];
		float reach;
		float window;

		if (distance_squared >= radius_squared)
		{
			continue;
		}
		// (a window's: nothing behind its wall)
		if (frame_lights[l].side[3] > 0.0f
			&& frame_lights[l].side[3] + dx * frame_lights[l].side[0] + dy * frame_lights[l].side[1] + dz * frame_lights[l].side[2] <= 0.0f)
		{
			continue;
		}
		// (as lights.glsl lights a surface facing it: the square law,
		// softened near it, brought to nothing at its radius)
		reach = distance_squared / radius_squared;
		window = 1.0f - reach * reach;
		total += (0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2]) * window * window / (distance_squared + 0.25f);
	}
	return total;
}

void lights_draw_cores(lights_t *lights, SDL_GPURenderPass *pass, const camera_t *camera)
{
	const float *view = camera->view.m;
	core_uniforms_t uniforms;
	SDL_GPUBufferBinding binding = { lights->core_buffer, 0 };

	if (pass == NULL || !lights->created || lights->core_pipeline == NULL || lights->core_count == 0)
	{
		return;
	}
	SDL_zero(uniforms);
	uniforms.camera_right[0] = view[0];
	uniforms.camera_right[1] = view[4];
	uniforms.camera_right[2] = view[8];
	uniforms.camera_up[0] = view[1];
	uniforms.camera_up[1] = view[5];
	uniforms.camera_up[2] = view[9];
	SDL_BindGPUGraphicsPipeline(pass, lights->core_pipeline);
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
	SDL_DrawGPUPrimitives(pass, 4, (uint32_t)lights->core_count, 0, 0);
}
