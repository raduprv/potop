// Placed objects. See objects.h.

#include "lights.h"
#include "objects.h"
#include "profile.h"
#include "clouds.h"
#include "gpu.h"
#include "season.h"
#include "scene_uniforms.h"
#include "terrain_renderer.h"

#include <SDL3/SDL.h>

// Floats per instance in the instance buffer: x, y, z, scale, rotation
// (quaternion x, y, z, w), fade
#define INSTANCE_FLOATS 15
// Objects fade out over this last fraction of their draw distance
#define FADE_FRACTION 0.2f
// Floating objects can rise this far above the ground in a cell's bounds
#define FLOATING_HEADROOM_METERS 20.0f
// Floating objects sit this far above the water surface, so the water does
// not cover them
#define FLOATING_CLEARANCE_METERS 0.03f
// A building's moss and lichen at age 1, unless its file says (the
// header's moss, lichen)
#define BUILDING_MOSS 0.7f
#define BUILDING_LICHEN 0.6f
// The occlusion culling's debug view: the boxes the camera's culling hid
// (objects_cull.comp's occlusion_buffer, occlusion_debug.frag): a count
// (and three words unused), then three vec4s each, at most this many
#define OCCLUSION_DEBUG_RECORDS 4096
// How deep a thatch's relief goes (m)
#define THATCH_RELIEF_METERS 0.08f
#define OCCLUSION_DEBUG_BYTES (16 + OCCLUSION_DEBUG_RECORDS * 48)

// ---------------------------------------------------------------------------
// GPU slots (see objects_t)

static void touch_slot(objects_t *objects, int slot)
{
	if (slot < 0 || slot >= objects->slot_count || objects->slot_dirty[slot])
	{
		return;
	}
	objects->slot_dirty[slot] = 1;
	objects->dirty_slots[objects->dirty_count++] = slot;
}

static bool grow_slot_arrays(objects_t *objects, int needed)
{
	int capacity;
	int *grown_instance;
	int *grown_free;
	uint8_t *grown_dirty;
	int *grown_dirty_list;

	if (needed <= objects->slot_array_capacity)
	{
		return true;
	}
	capacity = SDL_max(needed, SDL_max(1024, objects->slot_array_capacity * 2));
	grown_instance = SDL_realloc(objects->slot_instance, sizeof(int) * (size_t)capacity);
	if (grown_instance == NULL)
	{
		return false;
	}
	objects->slot_instance = grown_instance;
	grown_free = SDL_realloc(objects->free_slots, sizeof(int) * (size_t)capacity);
	if (grown_free == NULL)
	{
		return false;
	}
	objects->free_slots = grown_free;
	grown_dirty = SDL_realloc(objects->slot_dirty, (size_t)capacity);
	if (grown_dirty == NULL)
	{
		return false;
	}
	SDL_memset(grown_dirty + objects->slot_array_capacity, 0, (size_t)(capacity - objects->slot_array_capacity));
	objects->slot_dirty = grown_dirty;
	grown_dirty_list = SDL_realloc(objects->dirty_slots, sizeof(int) * (size_t)capacity);
	if (grown_dirty_list == NULL)
	{
		return false;
	}
	objects->dirty_slots = grown_dirty_list;
	objects->slot_array_capacity = capacity;
	return true;
}

// A slot for a new object (-1 if out of memory)
static int take_slot(objects_t *objects)
{
	int slot;

	if (objects->free_slot_count > 0)
	{
		return objects->free_slots[--objects->free_slot_count];
	}
	if (!grow_slot_arrays(objects, objects->slot_count + 1))
	{
		return -1;
	}
	slot = objects->slot_count++;
	objects->slot_instance[slot] = -1;
	return slot;
}

// The object of a slot is gone: the slot is emptied on the GPU and reused
static void release_slot(objects_t *objects, int slot)
{
	if (slot < 0 || slot >= objects->slot_count)
	{
		return;
	}
	objects->slot_instance[slot] = -1;
	objects->free_slots[objects->free_slot_count++] = slot;
	touch_slot(objects, slot);
}

// Every object is gone (a new map): no slot is looked at any more
static void release_all_slots(objects_t *objects)
{
	for (int k = 0; k < objects->dirty_count; k++)
	{
		objects->slot_dirty[objects->dirty_slots[k]] = 0;
	}
	objects->dirty_count = 0;
	objects->slot_count = 0;
	objects->free_slot_count = 0;
}

void objects_touch(objects_t *objects, int instance_index)
{
	if (instance_index >= 0 && instance_index < objects->instance_count)
	{
		touch_slot(objects, objects->instances[instance_index].gpu_slot);
	}
}

// The model loaded from path (relative to data/), loading it if it is new;
// -1 if it cannot be loaded
static int model_for_path(objects_t *objects, const char *path, int *failed)
{
	for (int m = 0; m < objects->model_count; m++)
	{
		if (SDL_strcmp(objects->models[m].name, path) == 0)
		{
			return m;
		}
	}
	uint64_t start = SDL_GetPerformanceCounter();

	if (object_model_load(&objects->models[objects->model_count], path, &objects->textures))
	{
		double milliseconds = 1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();

		// (the slow ones, for the start's log: a big texture, an atlas)
		if (milliseconds >= 20.0)
		{
			SDL_Log("Objects: '%s' took %.0f ms (with its textures)", path, milliseconds);
		}
		return objects->model_count++;
	}
	(*failed)++;
	return -1;
}

// The types' ages (objects.ini: age, age_random, dirt_meters) on their
// models (and their damaged and destroyed looks'), for the buildings' shader
static void apply_type_ages(objects_t *objects)
{
	const object_types_t *types = &objects->types;

	for (int t = 0; t < types->type_count; t++)
	{
		const object_type_t *type = &types->types[t];
		int models[3] = { type->model, type->damaged_model, type->destroyed_model };

		for (int k = 0; k < 3; k++)
		{
			if (models[k] >= 0 && models[k] < objects->model_count)
			{
				objects->models[models[k]].age = type->age;
				objects->models[models[k]].age_random = type->age_random;
				objects->models[models[k]].dirt_meters = type->dirt_meters;
			}
		}
	}
}

// Loads the models of all types (and their damaged and destroyed looks),
// sharing one model between types that use the same file
static void load_models(objects_t *objects)
{
	object_types_t *types = &objects->types;
	size_t capacity = (size_t)SDL_max(3 * types->type_count, 1);
	int failed = 0;

	objects->models = SDL_calloc(capacity, sizeof(object_model_t));
	if (objects->models == NULL)
	{
		return;
	}
	for (int t = 0; t < types->type_count; t++)
	{
		object_type_t *type = &types->types[t];

		type->model = model_for_path(objects, type->model_path, &failed);
		type->damaged_model = type->damaged_model_path[0] != '\0' ? model_for_path(objects, type->damaged_model_path, &failed) : -1;
		type->destroyed_model = type->destroyed_model_path[0] != '\0' ? model_for_path(objects, type->destroyed_model_path, &failed) : -1;
	}
	apply_type_ages(objects);
	SDL_Log("Objects: %d types in %d categories, %d models, %d textures%s", types->type_count, types->category_count,
		objects->model_count, objects->textures.count, failed > 0 ? " (some models failed to load, see above)" : "");
}

// The vertex layout every object pipeline shares: the model's vertices,
// then the instances (placement, rotation, fade)
// (the camera's pipelines also take the tangents, as a third buffer: the
// shadows have no use for them)
static const SDL_GPUVertexBufferDescription object_vertex_buffers[3] =
{
	{ 0, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 }, // position, normal, uv (object_model.c)
	{ 1, sizeof(float) * INSTANCE_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
	{ 2, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },  // tangent
};
static const SDL_GPUVertexAttribute object_vertex_attributes[8] =
{
	{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
	{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 },
	{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 },
	{ 3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
	{ 5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, sizeof(float) * 8 },
	{ 6, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 7, 1, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, sizeof(float) * 9 },   // the emissive groups put out (the camera's only)
};

// The plants' (object_format.h, OBJECT_FILE_PLANT): the same, and what each
// vertex is on the plant as a fourth buffer (the shadows' too, which leave
// the tangents out)
static const SDL_GPUVertexBufferDescription plant_vertex_buffers[4] =
{
	{ 0, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 1, sizeof(float) * INSTANCE_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
	{ 2, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 3, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },  // OBJECT_PLANT_VERTEX_FLOATS: object_file_plant_vertex_t, its snow
};
static const SDL_GPUVertexAttribute plant_vertex_attributes[9] =
{
	{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
	{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 },
	{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 },
	{ 3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
	{ 5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, sizeof(float) * 8 },
	{ 6, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 7, 3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 8, 3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
};
static const SDL_GPUVertexAttribute plant_shadow_vertex_attributes[6] =
{
	{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
	{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 },
	{ 3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
	{ 7, 3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 8, 3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
};

// The buildings' (object_format.h, OBJECT_MATERIAL_AGED): the camera's
// object layout, and the vertices' weathering as a fourth buffer
static const SDL_GPUVertexBufferDescription building_vertex_buffers[4] =
{
	{ 0, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 1, sizeof(float) * INSTANCE_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
	{ 2, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 3, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },  // object_file_weathering_vertex_t
};
static const SDL_GPUVertexAttribute building_vertex_attributes[9] =
{
	{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
	{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 },
	{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 },
	{ 3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
	{ 5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, sizeof(float) * 8 },
	{ 6, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 7, 3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 8, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 11 }, // its model's look: age, its spread, lichen, moss (objects_cull.comp)
};

// The buildings' pipeline (building.vert, building.frag: the props' shaders
// with BUILDING); NULL if it cannot be made (their aged materials are drawn
// as other objects then)
static SDL_GPUGraphicsPipeline *create_building_pipeline(void)
{
	SDL_GPUShader *vertex = gpu_load_shader("building.vert", 0, 0, 1);
	SDL_GPUShader *fragment = gpu_load_shader("building.frag", OBJECT_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);
	SDL_GPUGraphicsPipeline *pipeline = NULL;

	if (vertex != NULL && fragment != NULL)
	{
		gpu_pipeline_info_t info;

		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = building_vertex_buffers;
		info.vertex_buffer_count = 4;
		info.vertex_attributes = building_vertex_attributes;
		info.vertex_attribute_count = 9;
		info.cull = SDL_GPU_CULLMODE_BACK;
		info.depth_test = true;
		info.depth_write = true;
		info.multisampled = true;
		info.alpha_to_coverage = true;
		pipeline = gpu_make_pipeline(&info, "buildings");
	}
	if (vertex != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, vertex);
	}
	if (fragment != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, fragment);
	}
	if (pipeline == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Objects: no building shaders (%s): the buildings are drawn unweathered", SDL_GetError());
	}
	return pipeline;
}

// The occlusion culling's debug view (objects_draw_occlusion_debug): one
// triangle over the scene (post.vert), blended over it, no depth test; its
// fragment shader reads the hidden boxes and the pyramid (two storage
// buffers)
static SDL_GPUGraphicsPipeline *create_occlusion_debug_pipeline(void)
{
	SDL_GPUShader *vertex = gpu_load_shader("post.vert", 0, 0, 0);
	SDL_GPUShader *fragment = gpu_load_shader("occlusion_debug.frag", 0, 2, 1);
	SDL_GPUGraphicsPipeline *pipeline = NULL;

	if (vertex != NULL && fragment != NULL)
	{
		gpu_pipeline_info_t info;

		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.cull = SDL_GPU_CULLMODE_NONE;
		info.blend = GPU_BLEND_ALPHA;
		pipeline = gpu_make_pipeline(&info, "occlusion debug");
	}
	if (vertex != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, vertex);
	}
	if (fragment != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, fragment);
	}
	return pipeline;
}

// The props' pipeline (prop.vert, prop.frag): the objects' vertices; NULL
// if it cannot be made (the props are drawn as other objects then). Or
// (shell) the moss's shells' (prop_shell.vert, prop_shell.frag: a block of
// their own for the vertices)
static SDL_GPUGraphicsPipeline *create_prop_pipeline(bool shell)
{
	SDL_GPUShader *vertex = gpu_load_shader(shell ? "prop_shell.vert" : "prop.vert", 0, 0, shell ? 2 : 1);
	SDL_GPUShader *fragment = gpu_load_shader(shell ? "prop_shell.frag" : "prop.frag", OBJECT_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);
	SDL_GPUGraphicsPipeline *pipeline = NULL;

	if (vertex != NULL && fragment != NULL)
	{
		gpu_pipeline_info_t info;

		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = object_vertex_buffers;
		info.vertex_buffer_count = 3;
		info.vertex_attributes = object_vertex_attributes;
		info.vertex_attribute_count = 7;
		info.cull = SDL_GPU_CULLMODE_BACK;
		info.depth_test = true;
		info.depth_write = true;
		info.multisampled = true;
		info.alpha_to_coverage = true;
		pipeline = gpu_make_pipeline(&info, shell ? "moss shells" : "props");
	}
	if (vertex != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, vertex);
	}
	if (fragment != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, fragment);
	}
	if (pipeline == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, shell ? "Objects: no moss shell shaders (%s): the moss under water painted on alone"
			: "Objects: no prop shaders (%s): the props are drawn plain", SDL_GetError());
	}
	return pipeline;
}

// The plants' pipelines (plant.vert, plant.frag, plant_shadow.vert); false
// if they cannot be made (the plants are not drawn then)
static bool create_plant_pipelines(objects_t *objects)
{
	SDL_GPUShader *vertex = gpu_load_shader("plant.vert", 4, 0, 2);
	SDL_GPUShader *fragment = gpu_load_shader("plant.frag", OBJECT_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);
	SDL_GPUShader *shadow_vertex = gpu_load_shader("plant_shadow.vert", 0, 0, 2);
	SDL_GPUShader *shadow_fragment = gpu_load_shader("object_shadow.frag", 1, 0, 2);
	gpu_pipeline_info_t info;
	bool ok = vertex != NULL && fragment != NULL && shadow_vertex != NULL && shadow_fragment != NULL;

	if (ok)
	{
		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = plant_vertex_buffers;
		info.vertex_buffer_count = 4;
		info.vertex_attributes = plant_vertex_attributes;
		info.vertex_attribute_count = 9;
		info.cull = SDL_GPU_CULLMODE_BACK;
		info.depth_test = true;
		info.depth_write = true;
		info.multisampled = true;
		info.alpha_to_coverage = true;
		objects->plant_pipeline = gpu_make_pipeline(&info, "plants");
		info.cull = SDL_GPU_CULLMODE_NONE;
		objects->plant_alpha_pipeline = gpu_make_pipeline(&info, "plants (alpha tested)");
		info.vertex_shader = shadow_vertex;
		info.fragment_shader = shadow_fragment;
		info.vertex_attributes = plant_shadow_vertex_attributes;
		info.vertex_attribute_count = 6;
		info.depth_only = true;
		info.depth_bias_constant = 4.0f;
		info.depth_bias_slope = 2.0f;
		objects->plant_shadow_pipeline = gpu_make_pipeline(&info, "plant shadows");
		ok = objects->plant_pipeline != NULL && objects->plant_alpha_pipeline != NULL && objects->plant_shadow_pipeline != NULL;
	}

	SDL_GPUShader *shaders[4] = { vertex, fragment, shadow_vertex, shadow_fragment };

	for (int i = 0; i < 4; i++)
	{
		if (shaders[i] != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, shaders[i]);
		}
	}
	if (!ok)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Objects: no plant shaders (%s): the plants are not drawn", SDL_GetError());
	}
	return ok;
}

static bool create_pipelines(objects_t *objects)
{
	SDL_GPUShader *vertex = gpu_load_shader("object.vert", 0, 0, 2);
	SDL_GPUShader *fragment = gpu_load_shader("object.frag", OBJECT_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);
	SDL_GPUShader *shadow_vertex = gpu_load_shader("object_shadow.vert", 0, 0, 1);
	SDL_GPUShader *shadow_fragment = gpu_load_shader("object_shadow.frag", 1, 0, 2);
	gpu_pipeline_info_t info;
	bool ok = vertex != NULL && fragment != NULL && shadow_vertex != NULL && shadow_fragment != NULL;

	if (ok)
	{
		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = object_vertex_buffers;
		info.vertex_buffer_count = 3;
		info.vertex_attributes = object_vertex_attributes;
		info.vertex_attribute_count = 8;
		info.cull = SDL_GPU_CULLMODE_BACK;
		info.depth_test = true;
		info.depth_write = true;
		info.multisampled = true;
		info.alpha_to_coverage = true;
		objects->pipeline = gpu_make_pipeline(&info, "objects");
		objects->alpha_culled_pipeline = gpu_make_pipeline(&info, "objects (alpha tested, one sided)");
		info.cull = SDL_GPU_CULLMODE_NONE;
		objects->alpha_pipeline = gpu_make_pipeline(&info, "objects (alpha tested)");

		info.vertex_shader = shadow_vertex;
		info.fragment_shader = shadow_fragment;
		info.vertex_buffer_count = 2;
		info.vertex_attribute_count = 6;
		info.depth_only = true;
		info.depth_bias_constant = 4.0f;
		info.depth_bias_slope = 2.0f;
		objects->shadow_pipeline = gpu_make_pipeline(&info, "object shadows");
		objects->shadow_alpha_pipeline = objects->shadow_pipeline;
		ok = objects->pipeline != NULL && objects->alpha_pipeline != NULL && objects->shadow_pipeline != NULL;
	}
	SDL_ReleaseGPUShader(gpu.device, vertex);
	SDL_ReleaseGPUShader(gpu.device, fragment);
	SDL_ReleaseGPUShader(gpu.device, shadow_vertex);
	SDL_ReleaseGPUShader(gpu.device, shadow_fragment);
	// The impostors (impostors.h): a square per instance, from the
	// instance buffer alone (its corners from the vertex index)
	if (ok)
	{
		static const SDL_GPUVertexBufferDescription impostor_buffers[1] =
		{
			{ 0, sizeof(float) * INSTANCE_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
		};
		static const SDL_GPUVertexAttribute impostor_attributes[3] =
		{
			{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
			{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
			{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, sizeof(float) * 8 },
		};
		static const uint16_t square[6] = { 0, 1, 2, 2, 1, 3 };
		SDL_GPUShader *impostor_vertex = gpu_load_shader("impostor.vert", 4, 0, 2);
		SDL_GPUShader *impostor_fragment = gpu_load_shader("impostor.frag", IMPOSTOR_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);

		if (impostor_vertex != NULL && impostor_fragment != NULL)
		{
			SDL_zero(info);
			info.vertex_shader = impostor_vertex;
			info.fragment_shader = impostor_fragment;
			info.vertex_buffers = impostor_buffers;
			info.vertex_buffer_count = 1;
			info.vertex_attributes = impostor_attributes;
			info.vertex_attribute_count = 3;
			info.cull = SDL_GPU_CULLMODE_NONE;
			info.depth_test = true;
			info.depth_write = true;
			info.multisampled = true;
			info.alpha_to_coverage = true;
			objects->impostor_pipeline = gpu_make_pipeline(&info, "impostors");
		}
		if (impostor_vertex != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, impostor_vertex);
		}
		if (impostor_fragment != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, impostor_fragment);
		}
		objects->impostor_index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, sizeof(square));
		if (objects->impostor_index_buffer != NULL)
		{
			gpu_upload_buffer(objects->impostor_index_buffer, 0, square, sizeof(square));
		}
		// Without them, the objects are drawn whole at every distance
		if (objects->impostor_pipeline == NULL || objects->impostor_index_buffer == NULL)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Objects: no impostors (%s)", SDL_GetError());
		}
	}
	if (ok)
	{
		gpu_compute_layout_t cull_layout = { 0, 8, 5, 1, 256, 0, 0 };
		gpu_compute_layout_t update_layout = { 0, 1, 2, 1, 256, 0, 0 };

		create_plant_pipelines(objects);
		objects->prop_pipeline = create_prop_pipeline(false);
		objects->prop_shell_pipeline = objects->prop_pipeline != NULL ? create_prop_pipeline(true) : NULL;
		objects->building_pipeline = create_building_pipeline();
		objects->cull_pipeline = gpu_load_compute("objects_cull.comp", &cull_layout);
		objects->update_pipeline = gpu_load_compute("objects_update.comp", &update_layout);
		// (the debug view's: the culling always has a buffer to bind; the
		// view itself may be missing)
		objects->occlusion_debug_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE
			| SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, OCCLUSION_DEBUG_BYTES);
		objects->occlusion_debug_pipeline = create_occlusion_debug_pipeline();
		ok = objects->cull_pipeline != NULL && objects->update_pipeline != NULL && objects->occlusion_debug_buffer != NULL;
	}
	return ok;
}

bool objects_create(objects_t *objects, const terrain_t *terrain, const settings_t *settings)
{
	SDL_zerop(objects);
	objects->windows_by_hour = true;
	objects->windows_minute = -1;
	objects->draw_distance_scale = SDL_max(settings->objects.draw_distance_scale, 0.01f);
	if (!create_pipelines(objects))
	{
		return false;
	}
	trace_mark("pipelines (their shaders)");

	if (!objects_resize_map(objects, terrain))
	{
		return false;
	}
	trace_mark("the map's grid");

	object_texture_cache_initialize(&objects->textures, settings->render.anisotropy);
	object_types_load(&objects->types);
	trace_mark("objects.ini");
	load_models(objects);
	trace_mark("models and textures");
	objects->impostors_enabled = settings->objects.impostors;
	objects->lod_enabled = settings->objects.lod;
	objects->lod_distance_scale = settings->objects.lod_distance_scale;
	objects->lod_foliage_kept = settings->objects.lod_foliage_kept;
	objects->prop_lod_pixels = settings->objects.prop_lod_pixels;
	if (objects->impostor_pipeline != NULL)
	{
		impostor_baker_create(&objects->impostor_baker);
	}
	trace_mark("impostor baker");
	objects->impostors_pending = true;
	// Types ranked by draw distance, farthest first
	objects->type_rank = SDL_calloc((size_t)SDL_max(objects->types.type_count, 1), sizeof(int));
	if (objects->type_rank == NULL)
	{
		return false;
	}
	for (int t = 0; t < objects->types.type_count; t++)
	{
		int rank = 0;

		for (int other = 0; other < objects->types.type_count; other++)
		{
			float mine = objects->types.types[t].draw_distance_meters;
			float theirs = objects->types.types[other].draw_distance_meters;

			if (theirs > mine || (theirs == mine && other < t))
			{
				rank++;
			}
		}
		objects->type_rank[t] = rank;
	}
	return true;
}

bool objects_resize_map(objects_t *objects, const terrain_t *terrain)
{
	float map_meters = (float)terrain->size * terrain->cell_size_meters;
	int cell_count;

	SDL_free(objects->grid_first);
	SDL_free(objects->ray_cell_stamps);
	objects->ray_cell_stamps = NULL;
	SDL_free(objects->grid_minimum_y);
	SDL_free(objects->grid_maximum_y);
	SDL_free(objects->grid_draw_distance);
	objects->grid_size = SDL_max(1, (int)SDL_ceilf(map_meters / OBJECTS_GRID_CELL_METERS));
	cell_count = objects->grid_size * objects->grid_size;
	// One more for the free objects after the cells
	objects->grid_first = SDL_calloc((size_t)cell_count + 2, sizeof(int));
	objects->grid_minimum_y = SDL_calloc((size_t)cell_count, sizeof(float));
	objects->grid_maximum_y = SDL_calloc((size_t)cell_count, sizeof(float));
	objects->grid_draw_distance = SDL_calloc((size_t)cell_count, sizeof(float));
	objects->instance_count = 0;
	objects->floating_count = 0;
	release_all_slots(objects);
	objects->generation++;
	objects->grid_dirty = true;
	objects->far_shadow_dirty = true;
	objects->near_shadow_dirty = true;
	return objects->grid_first != NULL && objects->grid_minimum_y != NULL && objects->grid_maximum_y != NULL
		&& objects->grid_draw_distance != NULL;
}

// The tables built from the models (upload_tables): let go, to be built
// again before the next draw
static void release_tables(objects_t *objects)
{
	SDL_GPUBuffer **buffers[] = { &objects->type_buffer, &objects->model_buffer, &objects->draw_buffer, &objects->pool_vertex_buffer,
		&objects->pool_index_buffer, &objects->pool_tangent_buffer, &objects->pool_weathering_buffer };

	for (size_t i = 0; i < SDL_arraysize(buffers); i++)
	{
		if (*buffers[i] != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, *buffers[i]);
			*buffers[i] = NULL;
		}
	}
	for (int p = 0; p < 3; p++)
	{
		SDL_GPUBuffer **pass_buffers[] = { &objects->count_buffers[p], &objects->argument_storage[p], &objects->argument_buffers[p] };

		for (size_t k = 0; k < SDL_arraysize(pass_buffers); k++)
		{
			if (*pass_buffers[k] != NULL)
			{
				SDL_ReleaseGPUBuffer(gpu.device, *pass_buffers[k]);
				*pass_buffers[k] = NULL;
			}
		}
	}
	SDL_free(objects->draw_models);
	SDL_free(objects->draw_materials);
	SDL_free(objects->draw_levels);
	SDL_free(objects->draw_drop_levels);
	objects->draw_models = NULL;
	objects->draw_materials = NULL;
	objects->draw_levels = NULL;
	objects->draw_drop_levels = NULL;
	objects->draw_count = 0;
	objects->shadow_batch_count = 0;
	objects->building_batch_count = 0;
	objects->gpu_tables_ready = false;
}

static void swap_material_texture(object_material_t *material, SDL_GPUTexture *old_texture, SDL_GPUTexture *new_texture)
{
	SDL_GPUTexture **slots[4] = { &material->texture, &material->normal_texture, &material->orm_texture, &material->emissive_texture };

	for (int i = 0; i < 4; i++)
	{
		if (*slots[i] == old_texture)
		{
			*slots[i] = new_texture;
		}
	}
}

// Whether a model gets an impostor: the model of a type that wants one
// (not a damaged or destroyed look), and worth one
static bool wants_impostor(const objects_t *objects, int model)
{
	// Not the plants: their levels of detail do better far away
	if (model >= 0 && model < objects->model_count && objects->models[model].plant_buffer != NULL)
	{
		return false;
	}
	for (int t = 0; t < objects->types.type_count; t++)
	{
		const object_type_t *type = &objects->types.types[t];

		if (type->model == model && type->impostor)
		{
			return impostor_worth_one(&objects->models[model]);
		}
	}
	return false;
}

void objects_update_impostors(objects_t *objects)
{
	bool changed = false;
	int made = 0;
	int baked_before = objects->impostor_baker.baked;
	int loaded_before = objects->impostor_baker.loaded;
	double seconds_before = objects->impostor_baker.seconds;

	if (!objects->impostors_pending || gpu.command_buffer != NULL || gpu.device == NULL)
	{
		return;
	}
	objects->impostors_pending = false;
	if (objects->impostor_baker.bake_pipeline == NULL)
	{
		return;
	}
	if (objects->impostor_capacity < objects->model_count)
	{
		impostor_t *grown = SDL_realloc(objects->impostors, sizeof(impostor_t) * (size_t)objects->model_count);

		if (grown == NULL)
		{
			return;
		}
		SDL_memset(grown + objects->impostor_capacity, 0, sizeof(impostor_t) * (size_t)(objects->model_count - objects->impostor_capacity));
		objects->impostors = grown;
		objects->impostor_capacity = objects->model_count;
	}
	for (int m = 0; m < objects->model_count; m++)
	{
		impostor_t *impostor = &objects->impostors[m];
		bool wanted = wants_impostor(objects, m);

		if (wanted && impostor->albedo == NULL)
		{
			if (impostor_make(&objects->impostor_baker, impostor, &objects->models[m], &objects->textures))
			{
				made++;
			}
			changed = true;
		}
		else if (!wanted && impostor->albedo != NULL)
		{
			impostor_release(impostor);
			changed = true;
		}
	}
	if (changed)
	{
		int total = 0;

		for (int m = 0; m < objects->model_count; m++)
		{
			total += objects->impostors[m].albedo != NULL ? 1 : 0;
		}
		SDL_Log("Impostors: %d made (%d baked, %d from the cache) in %.2f s; %d models have one (%.0f MB)", made,
			objects->impostor_baker.baked - baked_before, objects->impostor_baker.loaded - loaded_before,
			objects->impostor_baker.seconds - seconds_before, total,
			(double)total * 2.0 * IMPOSTOR_ATLAS_PIXELS * IMPOSTOR_ATLAS_PIXELS * 4.0 * 4.0 / 3.0 / (1024.0 * 1024.0));
		// The culling's tables know which models have one
		release_tables(objects);
	}
}

bool objects_reload_changed(objects_t *objects)
{
	SDL_GPUTexture *old_textures[16];
	SDL_GPUTexture *new_textures[16];
	int swapped = object_texture_cache_reload_changed(&objects->textures, old_textures, new_textures, (int)SDL_arraysize(old_textures));
	bool models_changed = false;

	// Textures of their own files (.e3d models share them): every model
	// that uses one is pointed at the new one
	for (int i = 0; i < swapped; i++)
	{
		for (int m = 0; m < objects->model_count; m++)
		{
			for (int k = 0; k < objects->models[m].material_count; k++)
			{
				if (objects->models[m].materials[k].texture == old_textures[i] && m < objects->impostor_capacity
					&& objects->impostors[m].albedo != NULL)
				{
					// Its pictures, taken again with the new texture
					impostor_release(&objects->impostors[m]);
					objects->impostors_pending = true;
					models_changed = true;
				}
				swap_material_texture(&objects->models[m].materials[k], old_textures[i], new_textures[i]);
			}
		}
		SDL_ReleaseGPUTexture(gpu.device, old_textures[i]);
	}
	for (int m = 0; m < objects->model_count; m++)
	{
		object_model_t *model = &objects->models[m];
		object_model_t fresh;
		char name[OBJECT_MODEL_NAME_LENGTH];

		if (!object_model_file_changed(model))
		{
			continue;
		}
		SDL_strlcpy(name, model->name, sizeof(name));
		SDL_zero(fresh);
		if (!object_model_load(&fresh, name, &objects->textures))
		{
			// Half written, or broken: tried again when written again
			model->file_time = 0;
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Objects: '%s' changed but does not load; the old one stays", name);
			continue;
		}
		// A .pobj's textures are its own: gone with the old model
		if (model->own_textures)
		{
			for (int k = 0; k < model->material_count; k++)
			{
				object_material_t *material = &model->materials[k];
				SDL_GPUTexture *textures[4] = { material->texture, material->normal_texture, material->orm_texture, material->emissive_texture };

				for (int t = 0; t < 4; t++)
				{
					if (textures[t] != NULL)
					{
						object_texture_cache_drop(&objects->textures, textures[t]);
					}
				}
			}
		}
		// The collision shape stays: physics and navigation were built from it
		SDL_free(fresh.collision_positions);
		SDL_free(fresh.collision_indices);
		fresh.collision_positions = model->collision_positions;
		fresh.collision_vertex_count = model->collision_vertex_count;
		fresh.collision_indices = model->collision_indices;
		fresh.collision_index_count = model->collision_index_count;
		fresh.pick_index_count = model->pick_index_count;
		model->collision_positions = NULL;
		model->collision_indices = NULL;
		object_model_destroy(model);
		*model = fresh;
		models_changed = true;
		// Its pictures, taken again
		if (m < objects->impostor_capacity)
		{
			impostor_release(&objects->impostors[m]);
		}
		objects->impostors_pending = true;
		SDL_Log("Reloaded the model '%s' (%s)", name, model->file_path);
	}
	if (models_changed)
	{
		apply_type_ages(objects);
		release_tables(objects);
		objects->grid_dirty = true;
	}
	if (models_changed || swapped > 0)
	{
		objects->far_shadow_dirty = true;
		objects->near_shadow_dirty = true;
	}
	return models_changed || swapped > 0;
}

void objects_destroy(objects_t *objects)
{
	SDL_free(objects->strays);
	objects->strays = NULL;
	SDL_free(objects->stray_slots);
	objects->stray_slots = NULL;
	SDL_free(objects->stray_marks);
	objects->stray_marks = NULL;
	objects->stray_count = 0;
	objects->stray_capacity = 0;
	SDL_free(objects->order_staging);
	objects->order_staging = NULL;
	SDL_free(objects->range_staging);
	objects->range_staging = NULL;
	SDL_free(objects->changes);
	objects->changes = NULL;
	objects->change_count = 0;
	objects->change_capacity = 0;
	SDL_free(objects->regrow_ids);
	objects->regrow_ids = NULL;
	objects->regrow_count = 0;
	objects->regrow_capacity = 0;
	objects->regrow_known = false;
	SDL_free(objects->spin_instances);
	objects->spin_instances = NULL;
	objects->spin_count = 0;
	objects->spin_capacity = 0;
	objects->spin_known = false;
	for (int i = 0; i < objects->model_count; i++)
	{
		object_model_destroy(&objects->models[i]);
	}
	object_texture_cache_destroy(&objects->textures);
	SDL_free(objects->type_table);
	SDL_free(objects->type_shares);
	SDL_free(objects->models);
	SDL_free(objects->module_entries);
	SDL_free(objects->module_indices);
	object_types_free(&objects->types);
	SDL_free(objects->instances);
	SDL_free(objects->sort_scratch);
	SDL_free(objects->id_slots);
	SDL_free(objects->grid_first);
	SDL_free(objects->run_first);
	SDL_free(objects->run_start);
	SDL_free(objects->run_type);
	SDL_free(objects->ray_cell_stamps);
	objects->ray_cell_stamps = NULL;
	SDL_free(objects->grid_minimum_y);
	SDL_free(objects->grid_maximum_y);
	SDL_free(objects->grid_draw_distance);
	SDL_free(objects->type_rank);
	SDL_free(objects->floating);
	SDL_free(objects->draw_models);
	SDL_free(objects->draw_materials);
	SDL_free(objects->slot_instance);
	SDL_free(objects->free_slots);
	SDL_free(objects->slot_dirty);
	SDL_free(objects->dirty_slots);
	SDL_free(objects->update_staging);
	SDL_free(objects->cell_visible_staging);
	SDL_free(objects->model_wind);
	SDL_free(objects->model_used);
	for (int p = 0; p < 3; p++)
	{
		SDL_free(objects->model_in_view[p]);
	}
	if (gpu.device != NULL)
	{
		SDL_GPUBuffer *buffers[] = { objects->record_buffer, objects->slot_cell_buffer, objects->type_buffer, objects->model_buffer,
			objects->draw_buffer, objects->update_buffer, objects->pool_vertex_buffer, objects->pool_index_buffer, objects->pool_tangent_buffer,
			objects->pool_weathering_buffer, objects->order_buffer };

		for (size_t i = 0; i < SDL_arraysize(buffers); i++)
		{
			if (buffers[i] != NULL)
			{
				SDL_ReleaseGPUBuffer(gpu.device, buffers[i]);
			}
		}
		for (int i = 0; i < 3; i++)
		{
			SDL_GPUBuffer *pass_buffers[] = { objects->cell_visible_buffers[i], objects->count_buffers[i], objects->instance_buffers[i],
				objects->argument_storage[i], objects->argument_buffers[i], objects->decision_buffers[i] };

			for (size_t k = 0; k < SDL_arraysize(pass_buffers); k++)
			{
				if (pass_buffers[k] != NULL)
				{
					SDL_ReleaseGPUBuffer(gpu.device, pass_buffers[k]);
				}
			}
		}
		if (objects->cull_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, objects->cull_pipeline);
		}
		if (objects->update_pipeline != NULL)
		{
			SDL_ReleaseGPUComputePipeline(gpu.device, objects->update_pipeline);
		}
		if (objects->occlusion_debug_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, objects->occlusion_debug_buffer);
		}
		if (objects->occlusion_debug_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->occlusion_debug_pipeline);
		}
	}
	if (objects->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->pipeline);
	}
	if (objects->alpha_culled_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->alpha_culled_pipeline);
		objects->alpha_culled_pipeline = NULL;
	}
	if (objects->alpha_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->alpha_pipeline);
	}
	if (objects->building_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->building_pipeline);
	}
	if (objects->prop_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->prop_pipeline);
	}
	if (objects->prop_shell_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->prop_shell_pipeline);
	}
	if (objects->shadow_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->shadow_pipeline);
	}
	{
		SDL_GPUGraphicsPipeline *plant_pipelines[3] = { objects->plant_pipeline, objects->plant_alpha_pipeline, objects->plant_shadow_pipeline };

		for (int i = 0; i < 3; i++)
		{
			if (plant_pipelines[i] != NULL)
			{
				SDL_ReleaseGPUGraphicsPipeline(gpu.device, plant_pipelines[i]);
			}
		}
	}
	if (objects->impostors != NULL)
	{
		for (int m = 0; m < objects->impostor_capacity; m++)
		{
			impostor_release(&objects->impostors[m]);
		}
		SDL_free(objects->impostors);
	}
	impostor_baker_destroy(&objects->impostor_baker);
	if (gpu.device != NULL)
	{
		if (objects->impostor_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, objects->impostor_pipeline);
		}
		if (objects->impostor_index_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, objects->impostor_index_buffer);
		}
	}
	SDL_zerop(objects);
}

static const object_model_t *model_of(const objects_t *objects, const object_instance_t *instance)
{
	return &objects->models[objects->types.types[instance->type].model];
}

// Where the object's origin sits: on the lowest ground under its trunk, so
// it never floats on a slope (sinking a little into the uphill side looks
// natural; hovering over the downhill side does not)
static float ground_height_under(const terrain_t *terrain, float x, float z, float footprint)
{
	float height = terrain_sample_height_meters(terrain, x, z);

	height = SDL_min(height, terrain_sample_height_meters(terrain, x + footprint, z));
	height = SDL_min(height, terrain_sample_height_meters(terrain, x - footprint, z));
	height = SDL_min(height, terrain_sample_height_meters(terrain, x, z + footprint));
	height = SDL_min(height, terrain_sample_height_meters(terrain, x, z - footprint));
	return height;
}

// How deep an object is set into the ground: loose bodies (a barrel, a
// stone) not at all, they would jump out of it once awake
static float sink_of(const objects_t *objects, const object_instance_t *instance)
{
	const object_type_t *type = &objects->types.types[instance->type];

	return type->physics == OBJECT_PHYSICS_BODY && type->anchor_newtons <= 0.0f ? 0.0f : OBJECTS_SINK_METERS;
}

static float footprint_of(const objects_t *objects, const object_instance_t *instance)
{
	return SDL_min(0.3f * instance->scale, model_of(objects, instance)->horizontal_radius * instance->scale);
}

static void mark_shadows_dirty(objects_t *objects, int type)
{
	object_shadow_t shadow = objects->types.types[type].shadow;

	if (shadow == OBJECT_SHADOW_ALL)
	{
		objects->far_shadow_dirty = true;
	}
	else if (shadow == OBJECT_SHADOW_NEAR)
	{
		objects->near_shadow_dirty = true;
	}
}

static bool ensure_capacity(objects_t *objects, int needed)
{
	int capacity;
	object_instance_t *grown;
	object_instance_t *grown_scratch;

	if (needed <= objects->instance_capacity)
	{
		return true;
	}
	capacity = SDL_max(needed, SDL_max(256, objects->instance_capacity * 2));
	grown = SDL_realloc(objects->instances, sizeof(object_instance_t) * (size_t)capacity);
	if (grown == NULL)
	{
		return false;
	}
	objects->instances = grown;
	grown_scratch = SDL_realloc(objects->sort_scratch, sizeof(object_instance_t) * (size_t)capacity);
	if (grown_scratch == NULL)
	{
		return false;
	}
	objects->sort_scratch = grown_scratch;
	{
		uint8_t *grown_marks = SDL_realloc(objects->stray_marks, (size_t)capacity);

		if (grown_marks == NULL)
		{
			return false;
		}
		SDL_memset(grown_marks + objects->instance_capacity, 0, (size_t)(capacity - objects->instance_capacity));
		objects->stray_marks = grown_marks;
	}
	objects->instance_capacity = capacity;
	return true;
}

void objects_random_placement(const objects_t *objects, int type, float random_a, float random_b, float *yaw_out, float *scale_out)
{
	const object_type_t *definition = &objects->types.types[type];

	// (a plant facing the light: only a little either way of its front)
	if (definition->random_yaw && definition->yaw_spread_degrees > 0.0f && definition->yaw_spread_degrees < 180.0f)
	{
		*yaw_out = (random_a * 2.0f - 1.0f) * definition->yaw_spread_degrees * MATH3D_PI / 180.0f;
	}
	else
	{
		*yaw_out = definition->random_yaw ? random_a * 2.0f * MATH3D_PI : 0.0f;
	}
	*scale_out = definition->scale_minimum + (definition->scale_maximum - definition->scale_minimum) * random_b;
}

// ---------------------------------------------------------------------------
// Ids

static uint32_t id_hash(uint32_t id)
{
	id ^= id >> 16;
	id *= 0x7feb352du;
	id ^= id >> 15;
	return id;
}

// The table made anew for the instances there are, with room to grow
static void rebuild_id_slots(objects_t *objects)
{
	int wanted = 1024;

	while (wanted < objects->instance_count * 4 + 64)
	{
		wanted *= 2;
	}
	if (wanted != objects->id_slot_capacity)
	{
		int *grown = SDL_realloc(objects->id_slots, sizeof(int) * (size_t)wanted);

		if (grown == NULL)
		{
			return;
		}
		objects->id_slots = grown;
		objects->id_slot_capacity = wanted;
	}
	SDL_memset(objects->id_slots, 0, sizeof(int) * (size_t)objects->id_slot_capacity);
	for (int i = 0; i < objects->instance_count; i++)
	{
		uint32_t mask = (uint32_t)objects->id_slot_capacity - 1;
		uint32_t slot = id_hash(objects->instances[i].id) & mask;

		while (objects->id_slots[slot] != 0)
		{
			slot = (slot + 1) & mask;
		}
		objects->id_slots[slot] = i + 1;
	}
	objects->id_slot_used = objects->instance_count;
	objects->id_slots_stale = false;
}

int objects_find_id(objects_t *objects, uint32_t id)
{
	uint32_t mask;
	uint32_t slot;

	if (id == 0)
	{
		return -1;
	}
	if (objects->id_slots_stale || objects->id_slots == NULL || objects->id_slot_used * 2 + 64 > objects->id_slot_capacity)
	{
		rebuild_id_slots(objects);
		if (objects->id_slots == NULL)
		{
			return -1;
		}
	}
	mask = (uint32_t)objects->id_slot_capacity - 1;
	slot = id_hash(id) & mask;
	while (objects->id_slots[slot] != 0)
	{
		int index = objects->id_slots[slot] - 1;

		if (index < objects->instance_count && objects->instances[index].id == id)
		{
			// (gone: as good as none)
			return (objects->instances[index].state & OBJECT_STATE_DEAD) != 0 ? -1 : index;
		}
		slot = (slot + 1) & mask;
	}
	return -1;
}

// Puts a new instance (its id set) into the table, if the table is good
static void add_id_slot(objects_t *objects, int index)
{
	uint32_t mask;
	uint32_t slot;

	if (objects->id_slots_stale || objects->id_slots == NULL || (objects->id_slot_used + 1) * 2 + 64 > objects->id_slot_capacity)
	{
		objects->id_slots_stale = true;
		return;
	}
	objects->id_slot_used++;
	mask = (uint32_t)objects->id_slot_capacity - 1;
	slot = id_hash(objects->instances[index].id) & mask;
	while (objects->id_slots[slot] != 0)
	{
		slot = (slot + 1) & mask;
	}
	objects->id_slots[slot] = index + 1;
}

// A random id no instance has (xorshift: never 0)
static uint32_t new_id(objects_t *objects)
{
	if (objects->id_random == 0)
	{
		objects->id_random = (uint32_t)SDL_GetPerformanceCounter() | 1u;
	}
	for (;;)
	{
		uint32_t value = objects->id_random;

		value ^= value << 13;
		value ^= value >> 17;
		value ^= value << 5;
		objects->id_random = value;
		if (objects_find_id(objects, value) < 0)
		{
			return value;
		}
	}
}

bool objects_set_owner(objects_t *objects, uint32_t id, uint32_t owner)
{
	int index = objects_find_id(objects, id);

	if (index < 0)
	{
		return false;
	}
	objects->instances[index].owner = owner;
	return true;
}

static void spin_rotate(const float q[4], const float v[3], float out[3]);

int objects_spot_count(const objects_t *objects, int instance_index)
{
	int model;

	if (instance_index < 0 || instance_index >= objects->instance_count)
	{
		return 0;
	}
	model = objects_instance_model(objects, &objects->instances[instance_index]);
	return model >= 0 ? objects->models[model].spot_count : 0;
}

const object_file_spot_t *objects_spot_world(const objects_t *objects, int instance_index, int spot, float position_out[3], float *yaw_radians_out,
	float extents_out[2])
{
	const object_instance_t *instance;
	const object_file_spot_t *found;
	float scaled[3];
	float turned[3];
	int model;

	if (spot < 0 || spot >= objects_spot_count(objects, instance_index))
	{
		return NULL;
	}
	instance = &objects->instances[instance_index];
	model = objects_instance_model(objects, instance);
	found = &objects->models[model].spots[spot];
	scaled[0] = found->position[0] * instance->scale;
	scaled[1] = found->position[1] * instance->scale;
	scaled[2] = found->position[2] * instance->scale;
	spin_rotate(instance->rotation, scaled, turned);
	position_out[0] = instance->x + turned[0];
	position_out[1] = objects_instance_base_y(instance) + turned[1];
	position_out[2] = instance->z + turned[2];
	// (its facing turned with the object: yaw round Y, 0 down +Z)
	*yaw_radians_out = found->yaw_degrees * (SDL_PI_F / 180.0f) + instance->yaw_radians;
	extents_out[0] = found->extents[0] * instance->scale;
	extents_out[1] = found->extents[1] * instance->scale;
	return found;
}

int objects_emissive_group_count(const objects_t *objects, int instance_index)
{
	int model;

	if (instance_index < 0 || instance_index >= objects->instance_count)
	{
		return 0;
	}
	model = objects_instance_model(objects, &objects->instances[instance_index]);
	return model >= 0 ? objects->models[model].emissive_group_count : 0;
}

const object_file_emissive_group_t *objects_emissive_group(const objects_t *objects, int instance_index, int group)
{
	int model;

	if (group < 1 || group > objects_emissive_group_count(objects, instance_index))
	{
		return NULL;
	}
	model = objects_instance_model(objects, &objects->instances[instance_index]);
	return &objects->models[model].emissive_groups[group - 1];
}

bool objects_set_emissive(objects_t *objects, int instance_index, int group, bool lit)
{
	int count = objects_emissive_group_count(objects, instance_index);
	object_instance_t *instance;
	uint32_t before[2];

	if (count == 0 || group < 0 || group > count)
	{
		return false;
	}
	instance = &objects->instances[instance_index];
	before[0] = instance->emissive_off[0];
	before[1] = instance->emissive_off[1];
	for (int g = group == 0 ? 1 : group; g <= (group == 0 ? count : group); g++)
	{
		uint32_t bit = 1u << ((g - 1) & 31);

		if (lit)
		{
			instance->emissive_off[(g - 1) >> 5] &= ~bit;
		}
		else
		{
			instance->emissive_off[(g - 1) >> 5] |= bit;
		}
	}
	if (before[0] != instance->emissive_off[0] || before[1] != instance->emissive_off[1])
	{
		touch_slot(objects, instance->gpu_slot);
	}
	return true;
}

bool objects_emissive_lit(const objects_t *objects, int instance_index, int group)
{
	if (group < 1 || group > objects_emissive_group_count(objects, instance_index))
	{
		return false;
	}
	return (objects->instances[instance_index].emissive_off[(group - 1) >> 5] & (1u << ((group - 1) & 31))) == 0;
}

// A number 0..1 from two (the same for the same two)
static float window_hash(uint32_t a, uint32_t b)
{
	uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u) * 2246822519u;

	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	h *= 3266489917u;
	h ^= h >> 16;
	return (float)(h & 0xFFFFFFu) / 16777216.0f;
}

void objects_light_windows(objects_t *objects, double hour, float night_amount)
{
	int minute = (int)SDL_floor(hour * 60.0);
	int end;

	if (!objects->windows_by_hour)
	{
		return;
	}
	// (a new minute: a pass over the objects begun, a slice a frame: not a
	// big map's millions in one frame; a minute that comes meanwhile
	// begins it again)
	if (minute != objects->windows_minute)
	{
		objects->windows_minute = minute;
		objects->windows_cursor = 0;
		objects->windows_pass = true;
	}
	if (!objects->windows_pass)
	{
		return;
	}
	end = SDL_min(objects->windows_cursor + OBJECTS_WINDOWS_PER_FRAME, objects->instance_count);
	for (int i = objects->windows_cursor; i < end; i++)
	{
		int count = objects_emissive_group_count(objects, i);
		uint32_t id = objects->instances[i].id;
		// (a house nobody is in: dark all night)
		bool empty = window_hash(id, 0u) < 0.12f;

		for (int g = 1; g <= count; g++)
		{
			const object_file_emissive_group_t *group = objects_emissive_group(objects, i, g);
			// (dark enough for this window's people to want a light: each
			// its own, so they go out one by one as the day comes, and come
			// on one by one at dusk)
			float wants_light = 0.05f + 0.35f * window_hash(id, (uint32_t)g * 3u + 4u);
			bool lit = false;

			if (SDL_strncmp(group->name, "window", 6) != 0)
			{
				continue;
			}
			// At night: each window from its own hour in the evening (19 to
			// 21) to its own late hour (21:30 to 2), and again from its own
			// hour before dawn (4:30 to 6) on; dark between, and all day
			// (out as the sun comes up)
			if (night_amount > wants_light)
			{
				// (the hours from noon: the evening and the night one stretch)
				double from_noon = SDL_fmod(hour + 12.0, 24.0);
				float on = 7.0f + 2.0f * window_hash(id, (uint32_t)g * 3u + 1u);
				float off = 9.5f + 4.5f * window_hash(id, (uint32_t)g * 3u + 2u);
				float morning = 16.5f + 1.5f * window_hash(id, (uint32_t)g * 3u + 3u);

				lit = !empty && ((from_noon >= (double)on && from_noon < (double)off) || from_noon >= (double)morning);
			}
			objects_set_emissive(objects, i, g, lit);
		}
	}
	objects->windows_cursor = end;
	objects->windows_pass = end < objects->instance_count;
}

bool objects_set_interactive(objects_t *objects, uint32_t id, bool interactive)
{
	int index = objects_find_id(objects, id);

	if (index < 0)
	{
		return false;
	}
	if (interactive)
	{
		objects->instances[index].state |= OBJECT_STATE_INTERACTIVE;
	}
	else
	{
		objects->instances[index].state &= (uint8_t)~OBJECT_STATE_INTERACTIVE;
	}
	return true;
}

void objects_clear_interactive(objects_t *objects)
{
	for (int i = 0; i < objects->instance_count; i++)
	{
		objects->instances[i].state &= (uint8_t)~OBJECT_STATE_INTERACTIVE;
	}
}

bool objects_set_id(objects_t *objects, int index, uint32_t id)
{
	int other;

	if (index < 0 || index >= objects->instance_count || id == 0)
	{
		return false;
	}
	other = objects_find_id(objects, id);
	if (other >= 0 && other != index)
	{
		return false;
	}
	// (its old slot stays behind: it no longer matches, a look up passes it)
	if (objects->types.types[objects->instances[index].type].module >= 0)
	{
		object_modules_renumber(objects, objects->instances[index].id, id);
	}
	objects->instances[index].id = id;
	add_id_slot(objects, index);
	return true;
}

// A change the physics and the navigation hear of (objects_changes_since)
static void note_change(objects_t *objects, int index, object_change_kind_t kind)
{
	object_change_t *change;

	if (objects->change_count >= objects->change_capacity)
	{
		int capacity = SDL_max(objects->change_capacity * 2, 256);
		object_change_t *grown = SDL_realloc(objects->changes, sizeof(object_change_t) * (size_t)capacity);

		if (grown == NULL)
		{
			// (no room to say what: everything again)
			objects->grid_dirty = true;
			return;
		}
		objects->changes = grown;
		objects->change_capacity = capacity;
	}
	change = &objects->changes[objects->change_count++];
	change->index = index;
	change->kind = kind;
	change->x = objects->instances[index].x;
	change->z = objects->instances[index].z;
}

// One no longer where the sort put it (come loose, another type now): the
// cells' readers go through it too
static void add_stray(objects_t *objects, int index)
{
	if (index >= objects->sorted_count || objects->stray_marks == NULL || objects->stray_marks[index])
	{
		return;
	}
	if (objects->stray_count >= objects->stray_capacity)
	{
		int capacity = SDL_max(objects->stray_capacity * 2, 256);
		int *grown = SDL_realloc(objects->strays, sizeof(int) * (size_t)capacity);
		int *grown_slots;

		if (grown == NULL)
		{
			objects->grid_dirty = true;
			return;
		}
		objects->strays = grown;
		grown_slots = SDL_realloc(objects->stray_slots, sizeof(int) * (size_t)capacity);
		if (grown_slots == NULL)
		{
			objects->grid_dirty = true;
			return;
		}
		objects->stray_slots = grown_slots;
		objects->stray_capacity = capacity;
	}
	objects->stray_marks[index] = 1;
	objects->strays[objects->stray_count] = index;
	objects->stray_slots[objects->stray_count] = objects->instances[index].gpu_slot;
	objects->stray_count++;
	// (its slot's cell word says so: the culling looks at it in the order's
	// tail, not where the sort left it)
	touch_slot(objects, objects->instances[index].gpu_slot);
}

// No strays any more (sorted, or the list compacted): their slots go up
// again without the stray flag; the marks cleared (by the strays' indices
// while they are still valid, or all of them)
static void clear_strays(objects_t *objects, bool indices_valid)
{
	for (int n = 0; n < objects->stray_count; n++)
	{
		touch_slot(objects, objects->stray_slots[n]);
		if (indices_valid && objects->stray_marks != NULL && objects->strays[n] < objects->instance_capacity)
		{
			objects->stray_marks[objects->strays[n]] = 0;
		}
	}
	if (!indices_valid && objects->stray_marks != NULL)
	{
		SDL_memset(objects->stray_marks, 0, (size_t)objects->instance_capacity);
	}
	objects->stray_count = 0;
	objects->sort_serial++;
}

// The list compacted or cleared (indices moved): no strays to go by, the
// changes forgotten (a new generation: everyone goes through it all)
static void forget_strays(objects_t *objects)
{
	clear_strays(objects, false);
	objects->sorted_count = 0;
	objects->change_base += (uint32_t)objects->change_count;
	objects->change_count = 0;
}

const object_change_t *objects_changes_since(const objects_t *objects, uint32_t serial, int *count_out, uint32_t *next_out)
{
	uint32_t end = objects->change_base + (uint32_t)objects->change_count;

	*next_out = end;
	if (serial < objects->change_base || serial >= end)
	{
		*count_out = 0;
		return NULL;
	}
	*count_out = (int)(end - serial);
	return &objects->changes[serial - objects->change_base];
}

bool objects_add(objects_t *objects, const terrain_t *terrain, int type, float x, float z, float yaw_radians, float scale, int group)
{
	return objects_add_lifted(objects, terrain, type, x, z, yaw_radians, scale, group, 0.0f);
}

bool objects_add_lifted(objects_t *objects, const terrain_t *terrain, int type, float x, float z, float yaw_radians, float scale,
	int group, float lift_meters)
{
	object_instance_t *instance;
	float map_meters = (float)terrain->size * terrain->cell_size_meters;

	if (type < 0 || type >= objects->types.type_count || objects->types.types[type].model < 0)
	{
		return false;
	}
	if (x < 0.0f || z < 0.0f || x >= map_meters || z >= map_meters)
	{
		return false;
	}
	if (!ensure_capacity(objects, objects->instance_count + 1))
	{
		return false;
	}
	{
		uint32_t id = new_id(objects);

		instance = &objects->instances[objects->instance_count++];
		instance->id = id;
		add_id_slot(objects, objects->instance_count - 1);
	}
	instance->type = type;
	instance->group = group;
	instance->x = x;
	instance->z = z;
	instance->yaw_radians = yaw_radians;
	// (interactive only once the game says so: objects_set_interactive)
	instance->state = 0;
	instance->damage = OBJECT_DAMAGE_INTACT;
	instance->harvested = 0;
	instance->regrow_hours = 0.0f;
	instance->harvest_original_type = -1;
	instance->owner = 0;
	instance->emissive_off[0] = 0u;
	instance->emissive_off[1] = 0u;
	instance->module_state = 0;
	instance->module_target = 0;
	instance->module_progress = 1.0f;
	instance->module_held_seconds = 0.0f;
	instance->module_turns = 0.0;
	instance->module_id = 0;
	instance->module_part = 0;
	{
		const object_type_t *type_of = &objects->types.types[type];
		const object_model_t *model = &objects->models[type_of->model];

		for (int g = 0; g < model->emissive_group_count; g++)
		{
			if ((model->emissive_groups[g].flags & OBJECT_EMISSIVE_OFF_AT_FIRST) != 0)
			{
				instance->emissive_off[g >> 5] |= 1u << (g & 31);
			}
		}
	}
	objects->windows_minute = -1;
	instance->flatten = 0.0f;
	instance->flatten_yaw = 0.0f;
	instance->body = -1;
	objects_update_rotation(instance);
	instance->scale = scale;
	instance->lift_meters = lift_meters;
	instance->ground_height = ground_height_under(terrain, x, z, footprint_of(objects, instance)) - sink_of(objects, instance);
	instance->y = instance->ground_height + lift_meters;
	instance->gpu_slot = take_slot(objects);
	// (put at the end, outside the cells, found by its own position until
	// the next sort: objects_sort sorts again only when many have come; a
	// module's parts come with it and it changes places with the last:
	// sorted again)
	if (objects->types.types[type].module >= 0)
	{
		objects->grid_dirty = true;
		objects->generation++;
	}
	else
	{
		note_change(objects, objects->instance_count - 1, OBJECT_CHANGE_ADDED);
	}
	if (instance->gpu_slot >= 0)
	{
		// Its index is valid until the next sort: this frame's flush finds it
		objects->slot_instance[instance->gpu_slot] = objects->instance_count - 1;
		touch_slot(objects, instance->gpu_slot);
	}
	mark_shadows_dirty(objects, type);
	// A module: its parts after it, then it last again (what added it finds
	// it at the end of the list, as any other)
	if (objects->types.types[type].module >= 0)
	{
		int body = objects->instance_count - 1;
		int last;

		object_modules_place_parts(objects, terrain, body);
		last = objects->instance_count - 1;
		if (last != body)
		{
			object_instance_t swap = objects->instances[body];

			objects->instances[body] = objects->instances[last];
			objects->instances[last] = swap;
			if (objects->instances[body].gpu_slot >= 0)
			{
				objects->slot_instance[objects->instances[body].gpu_slot] = body;
			}
			if (objects->instances[last].gpu_slot >= 0)
			{
				objects->slot_instance[objects->instances[last].gpu_slot] = last;
			}
			objects->id_slots_stale = true;
		}
		objects->module_list_valid = false;
	}
	return true;
}

// Removes the instances flagged in 'remove' (one byte per instance),
// keeping the order of the rest
static int compact(objects_t *objects, const uint8_t *remove_given)
{
	int kept = 0;
	uint8_t *widened = NULL;
	const uint8_t *remove = remove_given;

	// (a module goes whole: its parts with it, it with any of its parts)
	if (objects->types.module_count > 0 && objects->instance_count > 0)
	{
		widened = SDL_malloc((size_t)objects->instance_count);
		if (widened != NULL)
		{
			SDL_memcpy(widened, remove_given, (size_t)objects->instance_count);
			object_modules_widen_removal(objects, widened);
			remove = widened;
		}
	}

	for (int i = 0; i < objects->instance_count; i++)
	{
		if (remove[i])
		{
			mark_shadows_dirty(objects, objects->instances[i].type);
			release_slot(objects, objects->instances[i].gpu_slot);
			continue;
		}
		objects->instances[kept++] = objects->instances[i];
	}
	SDL_free(widened);
	{
		int removed = objects->instance_count - kept;

		objects->instance_count = kept;
		if (removed > 0)
		{
			objects->id_slots_stale = true;
			objects->grid_dirty = true;
			objects->generation++;
			forget_strays(objects);
		}
		return removed;
	}
}

int objects_remove_flagged(objects_t *objects, const uint8_t *remove)
{
	return compact(objects, remove);
}

void objects_kill(objects_t *objects, int index)
{
	object_instance_t *instance;

	if (index < 0 || index >= objects->instance_count)
	{
		return;
	}
	instance = &objects->instances[index];
	if ((instance->state & OBJECT_STATE_DEAD) != 0)
	{
		return;
	}
	// (a module whole: its parts with it; rare in the game, the list gone
	// through then)
	if (objects->types.module_count > 0 && (objects->types.types[instance->type].module >= 0 || (instance->state & OBJECT_STATE_PART) != 0))
	{
		uint8_t *remove = SDL_calloc((size_t)objects->instance_count, 1);

		if (remove != NULL)
		{
			remove[index] = 1;
			object_modules_widen_removal(objects, remove);
			for (int i = 0; i < objects->instance_count; i++)
			{
				if (remove[i] && (objects->instances[i].state & OBJECT_STATE_DEAD) == 0)
				{
					objects->instances[i].state = (uint8_t)((objects->instances[i].state | OBJECT_STATE_DEAD) & ~OBJECT_STATE_INTERACTIVE);
					mark_shadows_dirty(objects, objects->instances[i].type);
					touch_slot(objects, objects->instances[i].gpu_slot);
					note_change(objects, i, OBJECT_CHANGE_KILLED);
				}
			}
			SDL_free(remove);
			return;
		}
	}
	instance->state = (uint8_t)((instance->state | OBJECT_STATE_DEAD) & ~OBJECT_STATE_INTERACTIVE);
	mark_shadows_dirty(objects, instance->type);
	// (its slot drawn as nothing: objects_instance_model says none)
	touch_slot(objects, instance->gpu_slot);
	note_change(objects, index, OBJECT_CHANGE_KILLED);
}

int objects_kill_flagged(objects_t *objects, const uint8_t *remove)
{
	int killed = 0;

	for (int i = 0; i < objects->instance_count; i++)
	{
		if (remove[i] && (objects->instances[i].state & OBJECT_STATE_DEAD) == 0)
		{
			objects_kill(objects, i);
			killed++;
		}
	}
	return killed;
}

int objects_kill_group(objects_t *objects, int group)
{
	int killed = 0;

	if (group <= 0)
	{
		return 0;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		if (objects->instances[i].group == group && (objects->instances[i].state & OBJECT_STATE_DEAD) == 0)
		{
			objects_kill(objects, i);
			killed++;
		}
	}
	return killed;
}

int objects_compact_dead(objects_t *objects)
{
	uint8_t *remove;
	bool any = false;
	int removed;

	for (int i = 0; i < objects->instance_count && !any; i++)
	{
		any = (objects->instances[i].state & OBJECT_STATE_DEAD) != 0;
	}
	if (!any)
	{
		return 0;
	}
	remove = SDL_calloc((size_t)objects->instance_count, 1);
	if (remove == NULL)
	{
		return 0;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		remove[i] = (objects->instances[i].state & OBJECT_STATE_DEAD) != 0 ? 1 : 0;
	}
	removed = compact(objects, remove);
	SDL_free(remove);
	return removed;
}

bool objects_retype(objects_t *objects, int index, int type)
{
	object_instance_t *instance;

	if (index < 0 || index >= objects->instance_count || type < 0 || type >= objects->types.type_count || objects->types.types[type].model < 0)
	{
		return false;
	}
	instance = &objects->instances[index];
	if (instance->type == type)
	{
		return true;
	}
	mark_shadows_dirty(objects, instance->type);
	instance->type = type;
	mark_shadows_dirty(objects, type);
	touch_slot(objects, instance->gpu_slot);
	// (out of its cell's order by draw distance now: a stray till the sort)
	add_stray(objects, index);
	note_change(objects, index, OBJECT_CHANGE_RETYPED);
	return true;
}

int objects_remove_in_radius(objects_t *objects, float x, float z, float radius, int category)
{
	uint8_t *remove;
	int removed;
	bool any = false;

	if (objects->instance_count == 0)
	{
		return 0;
	}
	remove = SDL_calloc((size_t)objects->instance_count, 1);
	if (remove == NULL)
	{
		return 0;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		float offset_x = instance->x - x;
		float offset_z = instance->z - z;

		if (offset_x * offset_x + offset_z * offset_z <= radius * radius
			&& (category < 0 || objects->types.types[instance->type].category == category))
		{
			remove[i] = 1;
			any = true;
		}
	}
	removed = any ? compact(objects, remove) : 0;
	SDL_free(remove);
	return removed;
}

bool objects_remove_nearest(objects_t *objects, float x, float z, float radius)
{
	int nearest = -1;
	float nearest_distance_squared = radius * radius;

	for (int i = 0; i < objects->instance_count; i++)
	{
		float offset_x = objects->instances[i].x - x;
		float offset_z = objects->instances[i].z - z;
		float distance_squared = offset_x * offset_x + offset_z * offset_z;

		if (distance_squared <= nearest_distance_squared && (objects->instances[i].state & OBJECT_STATE_DEAD) == 0)
		{
			nearest = i;
			nearest_distance_squared = distance_squared;
		}
	}
	if (nearest < 0)
	{
		return false;
	}
	// (a module, or a part of one: all of it)
	if (objects->types.types[objects->instances[nearest].type].module >= 0 || (objects->instances[nearest].state & OBJECT_STATE_PART) != 0)
	{
		uint8_t *remove = SDL_calloc((size_t)objects->instance_count, 1);

		if (remove == NULL)
		{
			return false;
		}
		remove[nearest] = 1;
		compact(objects, remove);
		SDL_free(remove);
		return true;
	}
	mark_shadows_dirty(objects, objects->instances[nearest].type);
	release_slot(objects, objects->instances[nearest].gpu_slot);
	SDL_memmove(&objects->instances[nearest], &objects->instances[nearest + 1],
		sizeof(object_instance_t) * (size_t)(objects->instance_count - nearest - 1));
	objects->instance_count--;
	objects->id_slots_stale = true;
	objects->grid_dirty = true;
	objects->generation++;
	forget_strays(objects);
	return true;
}

void objects_clear(objects_t *objects, bool only_spawned)
{
	uint8_t *remove;

	objects->regrow_known = false;
	if (!only_spawned)
	{
		objects->instance_count = 0;
		objects->id_slots_stale = true;
		release_all_slots(objects);
		forget_strays(objects);
		objects->generation++;
		objects->grid_dirty = true;
		objects->far_shadow_dirty = true;
		objects->near_shadow_dirty = true;
		return;
	}
	if (objects->instance_count == 0)
	{
		return;
	}
	remove = SDL_calloc((size_t)objects->instance_count, 1);
	if (remove == NULL)
	{
		return;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		remove[i] = objects->types.types[objects->instances[i].type].spawn_per_hectare > 0.0f && objects->instances[i].group == 0 ? 1 : 0;
	}
	compact(objects, remove);
	SDL_free(remove);
}

int objects_new_group(objects_t *objects)
{
	objects->next_group = SDL_max(objects->next_group, 0) + 1;
	return objects->next_group;
}

int objects_remove_group(objects_t *objects, int group)
{
	uint8_t *remove;
	int removed;

	if (group == 0 || objects->instance_count == 0)
	{
		return 0;
	}
	remove = SDL_calloc((size_t)objects->instance_count, 1);
	if (remove == NULL)
	{
		return 0;
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		remove[i] = objects->instances[i].group == group ? 1 : 0;
	}
	removed = compact(objects, remove);
	SDL_free(remove);
	return removed;
}

int objects_nearest_group(const objects_t *objects, float x, float z, float radius)
{
	int group = 0;
	float nearest = radius * radius;

	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		float offset_x = instance->x - x;
		float offset_z = instance->z - z;
		float distance_squared = offset_x * offset_x + offset_z * offset_z;

		if (instance->group != 0 && distance_squared <= nearest && (instance->state & OBJECT_STATE_DEAD) == 0)
		{
			nearest = distance_squared;
			group = instance->group;
		}
	}
	return group;
}

static int grid_cell_of(const objects_t *objects, float x, float z);

int objects_clear_vegetation(objects_t *objects, const objects_clear_circle_t *circles, int circle_count)
{
	uint8_t *remove;
	int removed = 0;
	bool any = false;

	if (objects->instance_count == 0 || circle_count == 0)
	{
		return 0;
	}
	objects_sort(objects);
	remove = SDL_calloc((size_t)objects->instance_count, 1);
	if (remove == NULL)
	{
		return 0;
	}
	for (int c = 0; c < circle_count; c++)
	{
		const objects_clear_circle_t *circle = &circles[c];
		// The widest plant reaches this far past its center
		float reach = circle->radius + 6.0f;
		int last = objects->grid_size - 1;
		int first_x = SDL_clamp((int)((circle->x - reach) / OBJECTS_GRID_CELL_METERS), 0, last);
		int first_z = SDL_clamp((int)((circle->z - reach) / OBJECTS_GRID_CELL_METERS), 0, last);
		int last_x = SDL_clamp((int)((circle->x + reach) / OBJECTS_GRID_CELL_METERS), 0, last);
		int last_z = SDL_clamp((int)((circle->z + reach) / OBJECTS_GRID_CELL_METERS), 0, last);

		for (int j = first_z; j <= last_z; j++)
		{
			for (int i = first_x; i <= last_x; i++)
			{
				int cell = j * objects->grid_size + i;

				for (int k = objects->grid_first[cell]; k < objects->grid_first[cell + 1]; k++)
				{
					const object_instance_t *instance = &objects->instances[k];
					const object_type_t *type = &objects->types.types[instance->type];
					float limit;
					float offset_x;
					float offset_z;

					if (remove[k] || instance->group != 0 || type->spawn_per_hectare <= 0.0f)
					{
						continue;
					}
					limit = circle->radius + 0.5f * objects->models[type->model].horizontal_radius * instance->scale;
					offset_x = instance->x - circle->x;
					offset_z = instance->z - circle->z;
					if (offset_x * offset_x + offset_z * offset_z < limit * limit)
					{
						remove[k] = 1;
						any = true;
					}
				}
			}
		}
	}
	if (any)
	{
		removed = compact(objects, remove);
	}
	SDL_free(remove);
	return removed;
}

static int grid_cell_of(const objects_t *objects, float x, float z)
{
	int i = SDL_clamp((int)(x / OBJECTS_GRID_CELL_METERS), 0, objects->grid_size - 1);
	int j = SDL_clamp((int)(z / OBJECTS_GRID_CELL_METERS), 0, objects->grid_size - 1);

	return j * objects->grid_size + i;
}

// Vertical extent and farthest draw distance of one grid cell's objects
static void update_cell_bounds(objects_t *objects, int cell)
{
	float minimum = 1e30f;
	float maximum = -1e30f;
	float distance = 0.0f;

	for (int k = objects->grid_first[cell]; k < objects->grid_first[cell + 1]; k++)
	{
		const object_instance_t *instance = &objects->instances[k];
		const object_type_t *type = &objects->types.types[instance->type];
		const object_model_t *model = &objects->models[type->model];
		float top = instance->ground_height + model->bounds_maximum.y * instance->scale;

		if (type->floats)
		{
			top += FLOATING_HEADROOM_METERS;
		}
		minimum = SDL_min(minimum, instance->ground_height + model->bounds_minimum.y * instance->scale);
		maximum = SDL_max(maximum, top);
		distance = SDL_max(distance, type->draw_distance_meters);
	}
	objects->grid_minimum_y[cell] = minimum;
	objects->grid_maximum_y[cell] = maximum;
	objects->grid_draw_distance[cell] = distance;
}

// The grid cell an object is sorted into; free objects go after the cells
static int sort_cell_of(const objects_t *objects, const object_instance_t *instance)
{
	if (instance->state & OBJECT_STATE_FREE)
	{
		return objects->grid_size * objects->grid_size;
	}
	return grid_cell_of(objects, instance->x, instance->z);
}

// Follows the water surface as a drawn object (lily pads); bodies float by
// the physics instead
static bool follows_water(const objects_t *objects, const object_instance_t *instance)
{
	const object_type_t *type = &objects->types.types[instance->type];

	return type->floats && type->physics != OBJECT_PHYSICS_BODY && !(instance->state & OBJECT_STATE_FREE);
}

// Each cell's runs of one type (see objects_t.run_first); left unmade
// (run_cells 0: the models in view then marked object by object) if memory
// runs out
static void make_type_runs(objects_t *objects, int cell_count)
{
	int *first = SDL_realloc(objects->run_first, sizeof(int) * ((size_t)cell_count + 1));

	objects->run_cells = 0;
	if (first == NULL)
	{
		return;
	}
	objects->run_first = first;
	objects->run_count = 0;
	for (int c = 0; c < cell_count; c++)
	{
		objects->run_first[c] = objects->run_count;
		for (int k = objects->grid_first[c]; k < objects->grid_first[c + 1]; k++)
		{
			int type = objects->instances[k].type;

			if (k > objects->grid_first[c] && objects->instances[k - 1].type == type)
			{
				continue;
			}
			if (objects->run_count == objects->run_capacity)
			{
				int capacity = SDL_max(objects->run_capacity * 2, 1024);
				int *starts = SDL_realloc(objects->run_start, sizeof(int) * (size_t)capacity);
				int *types;

				if (starts == NULL)
				{
					return;
				}
				objects->run_start = starts;
				types = SDL_realloc(objects->run_type, sizeof(int) * (size_t)capacity);
				if (types == NULL)
				{
					return;
				}
				objects->run_type = types;
				objects->run_capacity = capacity;
			}
			objects->run_start[objects->run_count] = k;
			objects->run_type[objects->run_count] = type;
			objects->run_count++;
		}
	}
	objects->run_first[cell_count] = objects->run_count;
	objects->run_cells = cell_count;
}

void objects_sort(objects_t *objects)
{
	int cell_count = objects->grid_size * objects->grid_size;
	int type_count = SDL_max(objects->types.type_count, 1);
	// The cells, then the free objects
	size_t bucket_count = (size_t)(cell_count + 1) * (size_t)type_count;
	int *bucket_start;
	object_instance_t *swap;

	// (sorted again when something was taken out, or when many were added
	// (a map loaded, populated, a village laid out); a few added in the
	// game, come loose or turned into another type stay strays meanwhile)
	if (!objects->grid_dirty && objects->instance_count - objects->sorted_count <= OBJECTS_ADDED_BEFORE_SORT
		&& objects->stray_count <= OBJECTS_STRAYS_BEFORE_SORT)
	{
		return;
	}
	// Counting sort by (cell, draw distance rank); stable, so objects of a
	// type keep their order
	bucket_start = SDL_calloc(bucket_count + 1, sizeof(int));
	if (bucket_start == NULL)
	{
		return;
	}
	// (all where they belong after it: no strays; by their indices before
	// they move)
	clear_strays(objects, true);
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		size_t bucket = (size_t)sort_cell_of(objects, instance) * (size_t)type_count + (size_t)objects->type_rank[instance->type];

		bucket_start[bucket + 1]++;
	}
	for (size_t b = 0; b < bucket_count; b++)
	{
		bucket_start[b + 1] += bucket_start[b];
	}
	for (int c = 0; c <= cell_count + 1; c++)
	{
		objects->grid_first[c] = bucket_start[(size_t)c * (size_t)type_count];
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		size_t bucket = (size_t)sort_cell_of(objects, instance) * (size_t)type_count + (size_t)objects->type_rank[instance->type];

		objects->sort_scratch[bucket_start[bucket]++] = *instance;
	}
	SDL_free(bucket_start);
	swap = objects->instances;
	objects->instances = objects->sort_scratch;
	objects->sort_scratch = swap;
	// The order changed: indices kept elsewhere (the physics, the ids'
	// table) are stale
	objects->generation++;
	objects->id_slots_stale = true;
	for (int c = 0; c < cell_count; c++)
	{
		update_cell_bounds(objects, c);
	}
	make_type_runs(objects, cell_count);

	// The floating ones, which follow the water every frame
	objects->floating_count = 0;
	SDL_free(objects->floating);
	objects->floating = NULL;
	for (int i = 0; i < objects->instance_count; i++)
	{
		objects->floating_count += follows_water(objects, &objects->instances[i]) ? 1 : 0;
	}
	if (objects->floating_count > 0)
	{
		objects->floating = SDL_malloc(sizeof(int) * (size_t)objects->floating_count);
		objects->floating_count = 0;
		for (int i = 0; objects->floating != NULL && i < objects->instance_count; i++)
		{
			if (follows_water(objects, &objects->instances[i]))
			{
				objects->floating[objects->floating_count++] = i;
			}
		}
	}
	objects->grid_dirty = false;
	// (all where they belong now: no strays, nothing changed since)
	objects->sorted_count = objects->instance_count;
	objects->change_base += (uint32_t)objects->change_count;
	objects->change_count = 0;
}

// True if any chunk overlapping the rectangle had its shape changed since
// the last upload
// Whether a chunk in the rectangle is in the current pass
static bool chunks_in_pass(const terrain_t *terrain, float minimum_x, float minimum_z, float maximum_x, float maximum_z)
{
	float chunk_meters = terrain->cell_size_meters * (float)TERRAIN_CHUNK_SIZE;
	int first_x = SDL_clamp((int)(minimum_x / chunk_meters), 0, terrain->chunks_per_side - 1);
	int first_y = SDL_clamp((int)(minimum_z / chunk_meters), 0, terrain->chunks_per_side - 1);
	int last_x = SDL_clamp((int)(maximum_x / chunk_meters), 0, terrain->chunks_per_side - 1);
	int last_y = SDL_clamp((int)(maximum_z / chunk_meters), 0, terrain->chunks_per_side - 1);

	for (int y = first_y; y <= last_y; y++)
	{
		for (int x = first_x; x <= last_x; x++)
		{
			if (terrain->chunks[y * terrain->chunks_per_side + x].objects_pass)
			{
				return true;
			}
		}
	}
	return false;
}

void objects_follow_terrain(objects_t *objects, terrain_t *terrain)
{
	// Footprints reach a little over the cell's edge
	const float margin = 2.0f;
	int chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;
	int grid_cells = objects->grid_size * objects->grid_size;
	int budget;
	int done = 0;

	if (!objects->follow_pass_active)
	{
		int in_pass = 0;

		if (!terrain->any_chunk_objects_dirty)
		{
			return;
		}
		// A pass goes through the chunks changed so far; what changes
		// meanwhile waits for the next one
		for (int chunk = 0; chunk < chunk_count; chunk++)
		{
			terrain->chunks[chunk].objects_pass = terrain->chunks[chunk].objects_dirty;
			terrain->chunks[chunk].objects_dirty = false;
			in_pass += terrain->chunks[chunk].objects_pass ? 1 : 0;
		}
		terrain->any_chunk_objects_dirty = false;
		objects->follow_pass_active = true;
		objects->follow_pass_everything = in_pass == chunk_count;
		objects->follow_cursor = 0;
	}
	if (objects->instance_count > 0)
	{
		objects_sort(objects);
	}
	budget = objects->follow_pass_everything ? INT32_MAX : OBJECTS_FOLLOW_BUDGET;
	while (objects->follow_cursor < grid_cells && done < budget && objects->instance_count > 0)
	{
		int cell = objects->follow_cursor++;
		int i = cell % objects->grid_size;
		int j = cell / objects->grid_size;
		float minimum_x = (float)i * OBJECTS_GRID_CELL_METERS;
		float minimum_z = (float)j * OBJECTS_GRID_CELL_METERS;

		if (objects->grid_first[cell] == objects->grid_first[cell + 1]
			|| !chunks_in_pass(terrain, minimum_x - margin, minimum_z - margin, minimum_x + OBJECTS_GRID_CELL_METERS + margin,
				minimum_z + OBJECTS_GRID_CELL_METERS + margin))
		{
			continue;
		}
		{
			int moved = 0;

			for (int k = objects->grid_first[cell]; k < objects->grid_first[cell + 1]; k++)
			{
				object_instance_t *instance = &objects->instances[k];
				float ground;

				// (a module's parts stand on its ground: objects_update_modules;
				// a loose one is the physics', a dead one nowhere)
				if ((instance->state & (OBJECT_STATE_PART | OBJECT_STATE_FREE | OBJECT_STATE_DEAD)) != 0)
				{
					continue;
				}
				ground = ground_height_under(terrain, instance->x, instance->z, footprint_of(objects, instance)) - sink_of(objects, instance);

				// Only those whose ground really moved go up to the GPU again
				// (most of a changed chunk is as it was: a few millimeters
				// of erosion somewhere in it)
				if (SDL_fabsf(ground - instance->ground_height) <= OBJECTS_FOLLOW_METERS)
				{
					continue;
				}
				instance->ground_height = ground;
				objects_touch(objects, k);
				moved++;
			}
			done += objects->grid_first[cell + 1] - objects->grid_first[cell];
			frame_profile_count(FRAME_OBJECTS_CHECKED, objects->grid_first[cell + 1] - objects->grid_first[cell]);
			frame_profile_count(FRAME_OBJECTS_MOVED, moved);
			if (moved > 0)
			{
				update_cell_bounds(objects, cell);
			}
		}
	}
	if (objects->follow_cursor >= grid_cells || objects->instance_count == 0)
	{
		for (int chunk = 0; chunk < chunk_count; chunk++)
		{
			terrain->chunks[chunk].objects_pass = false;
		}
		objects->follow_pass_active = false;
	}
}

void objects_follow_water(objects_t *objects, const water_t *water)
{
	objects_sort(objects);
	objects->dry_water = water;
	if (water == NULL || water->depth == NULL)
	{
		return;
	}
	for (int k = 0; k < objects->floating_count; k++)
	{
		object_instance_t *instance = &objects->instances[objects->floating[k]];
		int32_t cell_x = clamp_int((int32_t)(instance->x / water->cell_size_meters + 0.5f), 0, water->size - 1);
		int32_t cell_y = clamp_int((int32_t)(instance->z / water->cell_size_meters + 0.5f), 0, water->size - 1);

		float depth = (float)water->depth[cell_y * water->size + cell_x] * TERRAIN_METERS_PER_HEIGHT_UNIT;
		float lift = depth > 0.01f ? depth + FLOATING_CLEARANCE_METERS : 0.0f;

		if (lift != instance->lift_meters)
		{
			instance->lift_meters = lift;
			objects_touch(objects, objects->floating[k]);
		}
	}
}

typedef enum
{
	PASS_CAMERA = 0,               // what the camera sees, with fading
	PASS_FAR_SHADOW,               // every object casting shadows into the far map
	PASS_NEAR_SHADOW               // the near map: culled, and only what the camera draws
} pass_t;

// objects_cull.comp's modes, its slot record and the uniform block
#define CULL_CLEAR 1
#define CULL_COUNT 2
#define CULL_OFFSETS 3
#define CULL_SCATTER 4
#define RECORD_WORDS 10
// A record's word 7: flags (objects_cull.comp's); 8 and 9: the emissive groups put out
#define UPDATE_WORDS 12
#define DRAW_ARGUMENT_WORDS 5
#define CELL_NONE 0x00FFFFFFu
#define FLAG_TUMBLES 0x01000000u
#define FLAG_SPINS 0x02000000u     // turning all the time: kept out of the far shadow map and the sky's occlusion, drawn once and kept
#define FLAG_STRAY 0x04000000u     // a stray (objects_t.strays): looked at in the order's tail, not where the sort left it
#define ORDER_NONE 0xFFFFFFFFu     // the order's: no slot there
#define MODEL_NONE 0xFFFFu
// Updates sent up at once (more go in several batches)
#define UPDATE_BATCH 65536
// The largest dispatch along x the GPU must allow; more work loops
#define MAXIMUM_GROUPS 65535

typedef struct
{
	float planes[24];
	float eye[4];
	int32_t mode;
	int32_t pass_kind;
	int32_t slot_count;
	int32_t model_count;
	int32_t draw_count;
	int32_t capacity;
	int32_t highlight_slot;
	int32_t has_frustum;
	float fade_fraction;
	float impostor_factor;
	float least_height;
	int32_t free_filter;               // the near shadow map's layers: objects_shadow_layer_t
	float view_projection[16];
	float hiz_size[4];
	float lod[4];
	float lod_pixels[4];
	float debug[4];                    // x: 1 the camera's culling keeps the boxes it hid (objects_t.occlusion_shown)
	int32_t work[4];                   // objects to look at (the ranges' total), the ranges, where the strays start in the order, where the ranges start in the cells' buffer (words)
	int32_t water[4];                  // where the cells' dry bits start in the cells' buffer (words), 1 if they are there
	int32_t hiz_levels[HIZ_MAX_LEVELS][4]; // the pyramid's levels (hiz_level_table)
} cull_uniforms_t;

SDL_COMPILE_TIME_ASSERT(cull_uniforms_size, sizeof(cull_uniforms_t) == 320 + 16 * HIZ_MAX_LEVELS);

// Levels of detail (objects_cull.comp): each model's objects counted per
// level, its impostors after; a model's four vec4s in the model table
#define LOD_LEVELS 4
#define MODEL_FLOATS 24
// A model's height on screen (pixels) below which it is drawn at levels 1,
// 2 and 3 (objects.lod_distance_scale moves them), and the share of the way
// to the next level over which what that one leaves out fades
static const float lod_base_pixels[3] = { 300.0f, 120.0f, 50.0f };
#define LOD_FADE_FRACTION 0.15f
// The draw table's last word: the level, and whether the material is
// shuffled (its foliage thinned out at levels 2 and 3, objects.lod_foliage_kept)
#define DRAW_SHUFFLED 0x100u
// ...and whether its arguments are for the shared buffers in the shadows'
// passes (its first index and vertex offset the model's place in them)
#define DRAW_POOLED 0x200u
// ...and in the camera's pass (a building's opaque material)
#define DRAW_POOLED_CAMERA 0x400u
// How far apart the trees are in the autumn (plant.glsl)
#define PLANT_AUTUMN_SPREAD 0.12f

// Shortest distance from a point to a box
static float distance_to_box(vec3_t point, vec3_t box_minimum, vec3_t box_maximum)
{
	float dx = SDL_max(SDL_max(box_minimum.x - point.x, 0.0f), point.x - box_maximum.x);
	float dy = SDL_max(SDL_max(box_minimum.y - point.y, 0.0f), point.y - box_maximum.y);
	float dz = SDL_max(SDL_max(box_minimum.z - point.z, 0.0f), point.z - box_maximum.z);

	return SDL_sqrtf(dx * dx + dy * dy + dz * dz);
}

static SDL_GPUBuffer *grow_buffer(SDL_GPUBuffer *buffer, SDL_GPUBufferUsageFlags usage, uint32_t *capacity_bytes, uint32_t needed_bytes)
{
	uint32_t capacity;

	if (buffer != NULL && *capacity_bytes >= needed_bytes)
	{
		return buffer;
	}
	if (buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, buffer);
	}
	capacity = SDL_max(needed_bytes + needed_bytes / 2, 4096u);
	buffer = gpu_create_buffer(usage, capacity);
	*capacity_bytes = buffer != NULL ? capacity : 0;
	return buffer;
}

// Whether a model has an impostor to draw
static bool has_impostor(const objects_t *objects, int model)
{
	return model < objects->impostor_capacity && objects->impostors[model].albedo != NULL && objects->impostor_pipeline != NULL;
}

// Whether a model goes in the shadows' shared buffers (see objects.h): one
// with buffers of its own, not a plant (each plant's shadow is drawn with
// its own wind and season)
static bool model_pooled(const object_model_t *model)
{
	return model->vertex_buffer != NULL && model->index_buffer != NULL && model->index_size == SDL_GPU_INDEXELEMENTSIZE_32BIT
		&& model->plant_buffer == NULL && model->vertex_total > 0 && model->index_total > 0;
}

// Whether a pooled model is a building's (its tangents and weathering in
// the shared buffers too, for the camera)
static bool model_pooled_building(const object_model_t *model)
{
	return model_pooled(model) && model->tangent_buffer != NULL && model->weathering_buffer != NULL;
}

// Whether a draw of the table can go in the shadows' one call: an opaque
// material of a pooled model (nothing of it per draw in the shadow's shaders)
static bool draw_batched_in_shadows(const object_model_t *model, const object_material_t *material)
{
	return model_pooled(model) && !material->alpha_tested && !material->deciduous && material->texture != NULL;
}

// The shadows' shared buffers made (again): every pooled model's vertices
// and indices copied in on the GPU, each model's first vertex and index
// there into bases (two per model; 0 for the others). False if the GPU
// refused (the shadows are then drawn a draw at a time)
static bool build_pool(objects_t *objects, uint32_t *bases)
{
	uint64_t vertex_total = 0;
	uint64_t index_total = 0;
	uint64_t building_vertex_total = 0;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUCopyPass *copy;

	// (the buildings' models first: their tangents and weathering then make
	// buffers of their own as long as their part, the same vertex numbers)
	for (int round = 0; round < 2; round++)
	{
		for (int m = 0; m < objects->model_count; m++)
		{
			const object_model_t *model = &objects->models[m];

			if (round == 0)
			{
				bases[2 * m] = 0;
				bases[2 * m + 1] = 0;
			}
			if (!model_pooled(model) || model_pooled_building(model) != (round == 0))
			{
				continue;
			}
			bases[2 * m] = (uint32_t)index_total;
			bases[2 * m + 1] = (uint32_t)vertex_total;
			index_total += model->index_total;
			vertex_total += model->vertex_total;
		}
		if (round == 0)
		{
			building_vertex_total = vertex_total;
		}
	}
	if (vertex_total == 0 || (size_t)OBJECT_MODEL_VERTEX_BYTES * vertex_total > 0x7FFFFFFFu || sizeof(uint32_t) * index_total > 0x7FFFFFFFu)
	{
		return false;
	}
	objects->pool_vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)((size_t)OBJECT_MODEL_VERTEX_BYTES * vertex_total));
	objects->pool_index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, (uint32_t)(sizeof(uint32_t) * index_total));
	if (objects->pool_vertex_buffer == NULL || objects->pool_index_buffer == NULL)
	{
		return false;
	}
	if (building_vertex_total > 0)
	{
		objects->pool_tangent_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 4 * building_vertex_total));
		objects->pool_weathering_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 4 * building_vertex_total));
	}
	// (the models' own uploads first: they may still be waiting)
	gpu_flush_uploads();
	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return false;
	}
	copy = SDL_BeginGPUCopyPass(command_buffer);
	if (copy == NULL)
	{
		return false;
	}
	for (int m = 0; m < objects->model_count; m++)
	{
		const object_model_t *model = &objects->models[m];
		// (what each buffer of it takes: the vertices, the indices; a
		// building's tangents and weathering)
		SDL_GPUBuffer *sources[4] = { model->vertex_buffer, model->index_buffer, model->tangent_buffer, model->weathering_buffer };
		SDL_GPUBuffer *targets[4] = { objects->pool_vertex_buffer, objects->pool_index_buffer, objects->pool_tangent_buffer, objects->pool_weathering_buffer };
		uint32_t strides[4] = { (uint32_t)OBJECT_MODEL_VERTEX_BYTES, (uint32_t)sizeof(uint32_t), (uint32_t)sizeof(float) * 4, (uint32_t)sizeof(float) * 4 };
		uint32_t firsts[4] = { bases[2 * m + 1], bases[2 * m], bases[2 * m + 1], bases[2 * m + 1] };
		uint32_t counts[4] = { model->vertex_total, model->index_total, model->vertex_total, model->vertex_total };

		if (!model_pooled(model))
		{
			continue;
		}
		for (int k = 0; k < (model_pooled_building(model) && objects->pool_weathering_buffer != NULL ? 4 : 2); k++)
		{
			SDL_GPUBufferLocation source = { sources[k], 0 };
			SDL_GPUBufferLocation destination = { targets[k], strides[k] * firsts[k] };

			SDL_CopyGPUBufferToBuffer(copy, &source, &destination, strides[k] * counts[k], false);
		}
	}
	SDL_EndGPUCopyPass(copy);
	SDL_Log("Objects: the shared buffers: %.1f MB of vertices (%.1f MB of the buildings' with their tangents and weathering), %.1f MB of indices",
		(double)((size_t)OBJECT_MODEL_VERTEX_BYTES * vertex_total) / 1048576.0, (double)(64 * building_vertex_total) / 1048576.0,
		(double)(sizeof(uint32_t) * index_total) / 1048576.0);
	return true;
}

static bool building_material(const objects_t *objects, const object_model_t *model, const object_material_t *material);

// A draw of the table, while it is sorted (upload_tables)
typedef struct
{
	uint32_t words[4];
	int model;
	int material;
	int level;
	float drop_level;
	int group;                         // 0 the camera's buildings, 1 the rest of the shadows' one call, 2 the others
	int order;                         // where it was
} table_draw_t;

// Two materials alike for the camera's buildings' one call: the same
// textures, laid the same way, the same look (building_material_uniforms)
static int compare_building_materials(const object_material_t *a, const object_material_t *b)
{
	const void *a_keys[4] = { a->texture, a->normal_texture, a->orm_texture, a->emissive_texture };
	const void *b_keys[4] = { b->texture, b->normal_texture, b->orm_texture, b->emissive_texture };

	for (int k = 0; k < 4; k++)
	{
		if (a_keys[k] != b_keys[k])
		{
			return (uintptr_t)a_keys[k] < (uintptr_t)b_keys[k] ? -1 : 1;
		}
	}
	if (a->triplanar != b->triplanar)
	{
		return a->triplanar ? 1 : -1;
	}
	if (a->triplanar_meters != b->triplanar_meters)
	{
		return a->triplanar_meters < b->triplanar_meters ? -1 : 1;
	}
	if (a->thatch != b->thatch)
	{
		return a->thatch ? 1 : -1;
	}
	if (a->shaded != b->shaded)
	{
		return a->shaded ? 1 : -1;
	}
	return 0;
}

// The table's order: by group; the camera's buildings by material, then as
// they came; the rest as they came (by model)
static int compare_table_draws(void *context, const void *left, const void *right)
{
	const objects_t *objects = context;
	const table_draw_t *a = left;
	const table_draw_t *b = right;

	if (a->group != b->group)
	{
		return a->group < b->group ? -1 : 1;
	}
	if (a->group == 0)
	{
		int by_material = compare_building_materials(&objects->models[a->model].materials[a->material],
			&objects->models[b->model].materials[b->material]);

		if (by_material != 0)
		{
			return by_material;
		}
	}
	return a->order < b->order ? -1 : (a->order > b->order ? 1 : 0);
}

// The tables that do not change while the game runs: per type, per model,
// and the draws (every material of every model)
static bool upload_tables(objects_t *objects)
{
	int type_count = SDL_max(objects->types.type_count, 1);
	int model_count = SDL_max(objects->model_count, 1);
	float *types = SDL_calloc((size_t)type_count * 4, sizeof(float));
	// Per model: its box (see objects_cull.comp), then its impostor's
	// bounding sphere (radius 0: none)
	float *models = SDL_calloc((size_t)model_count * MODEL_FLOATS, sizeof(float));
	uint32_t *draws;
	int draw_count = 0;
	bool ok = types != NULL && models != NULL;

	for (int m = 0; m < objects->model_count; m++)
	{
		draw_count += objects->models[m].material_count * (objects->models[m].lods ? LOD_LEVELS : 1) + (has_impostor(objects, m) ? 1 : 0);
	}
	draws = SDL_calloc((size_t)SDL_max(draw_count, 1) * 4, sizeof(uint32_t));
	objects->draw_models = SDL_calloc((size_t)SDL_max(draw_count, 1), sizeof(int));
	objects->draw_materials = SDL_calloc((size_t)SDL_max(draw_count, 1), sizeof(int));
	objects->draw_levels = SDL_calloc((size_t)SDL_max(draw_count, 1), sizeof(int));
	objects->draw_drop_levels = SDL_calloc((size_t)SDL_max(draw_count, 1), sizeof(float));
	ok = ok && draws != NULL && objects->draw_models != NULL && objects->draw_materials != NULL && objects->draw_levels != NULL
		&& objects->draw_drop_levels != NULL;
	// (the shadows' shared buffers: each model's first index and vertex there)
	uint32_t *pool_bases = SDL_calloc((size_t)model_count * 2, sizeof(uint32_t));
	bool pooled = ok && pool_bases != NULL && build_pool(objects, pool_bases);

	if (!pooled && ok)
	{
		SDL_Log("Objects: no shared buffers for the shadows; drawn a material at a time");
	}
	objects->shadow_batch_count = 0;
	if (ok)
	{
		for (int t = 0; t < objects->types.type_count; t++)
		{
			const object_type_t *type = &objects->types.types[t];

			types[4 * t] = type->draw_distance_meters;
			types[4 * t + 1] = type->shadow == OBJECT_SHADOW_ALL ? 2.0f : type->shadow == OBJECT_SHADOW_NEAR ? 1.0f : 0.0f;
			// The box comes from the type's own model, whatever it looks like now
			types[4 * t + 2] = (float)SDL_max(type->model, 0);
			// (all of them there, until objects_update_seasons says)
			types[4 * t + 3] = 1.0f;
		}
		for (int m = 0; m < objects->model_count; m++)
		{
			const object_model_t *model = &objects->models[m];

			models[MODEL_FLOATS * m] = model->horizontal_radius;
			models[MODEL_FLOATS * m + 1] = model->bounds_minimum.y;
			models[MODEL_FLOATS * m + 2] = model->bounds_maximum.y;
			models[MODEL_FLOATS * m + 3] = SDL_max(SDL_fabsf(model->bounds_maximum.y), SDL_fabsf(model->bounds_minimum.y));
			if (has_impostor(objects, m))
			{
				SDL_memcpy(&models[MODEL_FLOATS * m + 4], objects->impostors[m].sphere, sizeof(float) * 4);
			}
			models[MODEL_FLOATS * m + 8] = model->lods ? 1.0f : 0.0f;
			// Its levels (objects_cull.comp): a tree's by its height on
			// screen (they leave out its finest branches); a prop's (meshes
			// of their own, tools/potop_prop) by how far off each is: 2 and
			// the three errors
			// (its size on screen the larger of its height and its width:
			// a low wide one, a yard of sheds, piles and tools 4 m high and
			// 14 across, went coarse a few meters off, judged by its
			// height alone)
			models[MODEL_FLOATS * m + 9] = SDL_max(model->bounds_maximum.y - model->bounds_minimum.y, 1.6f * model->horizontal_radius);
			for (int k = 0; k < model->material_count; k++)
			{
				if (model->materials[k].lod_meshes)
				{
					models[MODEL_FLOATS * m + 8] = 2.0f;
					SDL_memcpy(&models[MODEL_FLOATS * m + 9], model->lod_errors, sizeof(float) * 3);
					break;
				}
			}
			// (its type's lod_distance_scale: the houses' finer farther off,
			// the forests' as they were; the largest of the types using it)
			{
				float scale = 0.0f;

				for (int t = 0; t < objects->types.type_count; t++)
				{
					const object_type_t *type = &objects->types.types[t];

					if (type->model == m || type->damaged_model == m || type->destroyed_model == m)
					{
						scale = SDL_max(scale, type->lod_distance_scale);
					}
				}
				// (by height: the height on screen it is judged by; a
				// generated house's or a prop's own levels: each one's error
				// in meters, the same: farther before it is small enough)
				for (int k = 0; k < (models[MODEL_FLOATS * m + 8] > 1.5f ? 3 : 1); k++)
				{
					models[MODEL_FLOATS * m + 9 + k] *= scale > 0.0f ? SDL_clamp(scale, 0.1f, 20.0f) : 1.0f;
				}
			}
			// Its box across (x and z, least and most): the occlusion test's
			// box turned as the object stands (a long house's outline, not a
			// square round its farthest corner)
			models[MODEL_FLOATS * m + 12] = model->bounds_minimum.x;
			models[MODEL_FLOATS * m + 13] = model->bounds_minimum.z;
			models[MODEL_FLOATS * m + 14] = model->bounds_maximum.x;
			models[MODEL_FLOATS * m + 15] = model->bounds_maximum.z;
			// Its place in the shared buffers (bits, objects_cull.comp)
			if (pooled)
			{
				SDL_memcpy(&models[MODEL_FLOATS * m + 16], &pool_bases[2 * m], sizeof(uint32_t) * 2);
			}
			// A building's look (its type's age, its spread, its lichen and
			// moss): into each of its instances, for prop_body.glsl, so that
			// the buildings of many models share one draw
			models[MODEL_FLOATS * m + 20] = model->age;
			models[MODEL_FLOATS * m + 21] = model->age_random;
			models[MODEL_FLOATS * m + 22] = model->lichen > 0.0f ? model->lichen : BUILDING_LICHEN;
			models[MODEL_FLOATS * m + 23] = model->moss > 0.0f ? model->moss : BUILDING_MOSS;
			// Each material at each of the model's levels of detail (one if it
			// has none), of that level's count (level * model_count + m)
			for (int level = 0; level < (model->lods ? LOD_LEVELS : 1); level++)
			{
				for (int k = 0; k < model->material_count; k++)
				{
					const object_material_t *material = &model->materials[k];
					int index_count = level > 0 && material->lods ? material->lod_index_count[level - 1] : material->index_count;

					if (index_count <= 0)
					{
						continue;
					}
					draws[4 * objects->draw_count] = (uint32_t)(level * objects->model_count + m);
					draws[4 * objects->draw_count + 1] = (uint32_t)index_count;
					// (its own mesh at that level, or the first part of its own)
					draws[4 * objects->draw_count + 2] = (uint32_t)(level > 0 && material->lod_meshes ? material->lod_index_offset[level - 1]
						: material->index_offset);
					draws[4 * objects->draw_count + 3] = (uint32_t)level | (material->shuffled ? DRAW_SHUFFLED : 0u)
						| (pooled && draw_batched_in_shadows(model, material) ? DRAW_POOLED : 0u)
						| (pooled && draw_batched_in_shadows(model, material) && model_pooled_building(model) && objects->pool_weathering_buffer != NULL
							&& building_material(objects, model, material) ? DRAW_POOLED_CAMERA : 0u);
					objects->draw_models[objects->draw_count] = m;
					objects->draw_materials[objects->draw_count] = k;
					objects->draw_levels[objects->draw_count] = level;
					// What the next level leaves out fades as it nears: the
					// branches deeper than the ones it keeps (0: nothing)
					objects->draw_drop_levels[objects->draw_count] = level < LOD_LEVELS - 1 && material->lods && !material->lod_meshes
						&& !material->alpha_tested && material->lod_index_count[level] < index_count ? (float)material->lod_keep_level[level] : 0.0f;
					objects->draw_count++;
				}
			}
		}
		// The camera's buildings' draws first (by material), the rest of the
		// shadows' one call next, the others after them (as they came)
		{
			int material_draws = objects->draw_count;
			table_draw_t *sorted = SDL_malloc(sizeof(table_draw_t) * (size_t)SDL_max(material_draws, 1));

			if (sorted != NULL)
			{
				for (int d = 0; d < material_draws; d++)
				{
					SDL_memcpy(sorted[d].words, &draws[4 * d], sizeof(uint32_t) * 4);
					sorted[d].model = objects->draw_models[d];
					sorted[d].material = objects->draw_materials[d];
					sorted[d].level = objects->draw_levels[d];
					sorted[d].drop_level = objects->draw_drop_levels[d];
					sorted[d].group = (draws[4 * d + 3] & DRAW_POOLED_CAMERA) != 0u ? 0 : ((draws[4 * d + 3] & DRAW_POOLED) != 0u ? 1 : 2);
					sorted[d].order = d;
				}
				SDL_qsort_r(sorted, (size_t)material_draws, sizeof(table_draw_t), compare_table_draws, objects);
				objects->building_batch_count = 0;
				objects->shadow_batch_count = 0;
				for (int d = 0; d < material_draws; d++)
				{
					SDL_memcpy(&draws[4 * d], sorted[d].words, sizeof(uint32_t) * 4);
					objects->draw_models[d] = sorted[d].model;
					objects->draw_materials[d] = sorted[d].material;
					objects->draw_levels[d] = sorted[d].level;
					objects->draw_drop_levels[d] = sorted[d].drop_level;
					objects->building_batch_count += sorted[d].group == 0 ? 1 : 0;
					objects->shadow_batch_count += sorted[d].group <= 1 ? 1 : 0;
				}
				{
					int runs = 0;

					for (int d = 0; d < objects->building_batch_count; d++)
					{
						runs += d == 0 || compare_building_materials(&objects->models[objects->draw_models[d - 1]].materials[objects->draw_materials[d - 1]],
							&objects->models[objects->draw_models[d]].materials[objects->draw_materials[d]]) != 0 ? 1 : 0;
					}
					SDL_Log("Objects: %d of %d draws go into the shadows in one call; the buildings' %d in %d calls (one a material) for the camera",
						objects->shadow_batch_count, material_draws, objects->building_batch_count, runs);
				}
			}
			else
			{
				// (no room to sort them: none batched, each drawn as it was)
				for (int d = 0; d < material_draws; d++)
				{
					draws[4 * d + 3] &= ~(DRAW_POOLED | DRAW_POOLED_CAMERA);
				}
			}
			SDL_free(sorted);
		}
		// The impostors' draws, after every material's: one per model that
		// has one, of the model's second set of counts (model_count + m)
		for (int m = 0; m < objects->model_count; m++)
		{
			if (has_impostor(objects, m))
			{
				draws[4 * objects->draw_count] = (uint32_t)(LOD_LEVELS * objects->model_count + m);
				draws[4 * objects->draw_count + 1] = 6;
				draws[4 * objects->draw_count + 2] = 0;
				objects->draw_models[objects->draw_count] = m;
				objects->draw_materials[objects->draw_count] = -1;
				objects->draw_levels[objects->draw_count] = 0;
				objects->draw_drop_levels[objects->draw_count] = 0.0f;
				objects->draw_count++;
			}
		}
		// Each model's sway and flutter in the wind: from the type it is the
		// model of (the first, if several)
		SDL_free(objects->model_wind);
		objects->model_wind = SDL_calloc((size_t)model_count * 3, sizeof(float));
		objects->model_wind_count = objects->model_wind != NULL ? objects->model_count : 0;
		for (int t = objects->types.type_count - 1; t >= 0 && objects->model_wind != NULL; t--)
		{
			const object_type_t *type = &objects->types.types[t];

			// (a still one, a mushroom, none whatever its type says)
			if (type->model >= 0 && type->model < objects->model_count && !objects->models[type->model].still)
			{
				const object_model_t *model = &objects->models[type->model];

				// (a plant's own, from its pobj, where its type does not say)
				objects->model_wind[3 * type->model] = model->wind_given && !type->sway_own ? model->sway : type->sway_meters;
				objects->model_wind[3 * type->model + 1] = model->wind_given && !type->flutter_own ? model->flutter : type->flutter_meters;
				objects->model_wind[3 * type->model + 2] = model->wind_given && !type->stiffness_own ? model->stiffness : type->stiffness;
			}
		}
		objects->type_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, (uint32_t)(sizeof(float) * 4 * (size_t)type_count));
		objects->model_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
			(uint32_t)(sizeof(float) * MODEL_FLOATS * (size_t)model_count));
		objects->draw_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
			(uint32_t)(sizeof(uint32_t) * 4 * (size_t)SDL_max(draw_count, 1)));
		ok = objects->type_buffer != NULL && objects->model_buffer != NULL && objects->draw_buffer != NULL;
	}
	for (int p = 0; p < 3 && ok; p++)
	{
		// Per model at each level of detail, then per model's impostor:
		// count, next place, first place
		objects->count_buffers[p] = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
			(uint32_t)(sizeof(uint32_t) * 3 * (LOD_LEVELS + 1) * (size_t)model_count));
		objects->argument_storage[p] = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
			(uint32_t)(sizeof(uint32_t) * DRAW_ARGUMENT_WORDS * (size_t)SDL_max(draw_count, 1)));
		// Only the indirect draws read this one (see particles.h on why the
		// arguments are written elsewhere and copied)
		objects->argument_buffers[p] = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDIRECT,
			(uint32_t)(sizeof(uint32_t) * DRAW_ARGUMENT_WORDS * (size_t)SDL_max(draw_count, 1)));
		ok = objects->count_buffers[p] != NULL && objects->argument_storage[p] != NULL && objects->argument_buffers[p] != NULL;
	}
	if (ok)
	{
		gpu_upload_buffer(objects->type_buffer, 0, types, sizeof(float) * 4 * (size_t)type_count);
		gpu_upload_buffer(objects->model_buffer, 0, models, sizeof(float) * MODEL_FLOATS * (size_t)model_count);
		gpu_upload_buffer(objects->draw_buffer, 0, draws, sizeof(uint32_t) * 4 * (size_t)SDL_max(draw_count, 1));
	}
	SDL_free(objects->type_table);
	objects->type_table = types;
	SDL_free(models);
	SDL_free(draws);
	SDL_free(pool_bases);
	objects->gpu_tables_ready = ok;
	if (!ok)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Objects: the GPU refused the culling tables; no objects drawn");
	}
	return ok;
}

// A slot's record and cell word, as objects_cull.comp reads them
// A quaternion (x, y, z, w) turning a vector
static void spin_rotate(const float q[4], const float v[3], float out[3])
{
	// v + 2 q.xyz x (q.xyz x v + w v)
	float c[3] = { q[1] * v[2] - q[2] * v[1] + q[3] * v[0], q[2] * v[0] - q[0] * v[2] + q[3] * v[1], q[0] * v[1] - q[1] * v[0] + q[3] * v[2] };

	out[0] = v[0] + 2.0f * (q[1] * c[2] - q[2] * c[1]);
	out[1] = v[1] + 2.0f * (q[2] * c[0] - q[0] * c[2]);
	out[2] = v[2] + 2.0f * (q[0] * c[1] - q[1] * c[0]);
}

// A turning one where the clock has it: its model's origin is what it turns
// round (a windmill's sails' hub), standing spin_pivot from the spot it was
// placed on, turned as it stands; its own turn round the type's axis first.
// (Its placement stays put as it turns: its distance, its level of detail,
// its fade never jump.) Each from its own start (by where it stands), not
// all in step
static void spin_placement(const objects_t *objects, const object_instance_t *instance, const object_type_t *type, float placement[4],
	float rotation[4])
{
	uint32_t spot = (uint32_t)SDL_lroundf(instance->x * 7.0f) * 73856093u ^ (uint32_t)SDL_lroundf(instance->z * 7.0f) * 19349663u;
	double turns = objects->spin_seconds * (double)type->spin_rpm / 60.0 + (double)(spot % 1000u) / 1000.0;
	float angle = (float)((turns - SDL_floor(turns)) * 2.0 * 3.14159265358979);
	float half_sine = SDL_sinf(0.5f * angle);
	float spin[4] = { type->spin_axis[0] * half_sine, type->spin_axis[1] * half_sine, type->spin_axis[2] * half_sine, SDL_cosf(0.5f * angle) };
	const float *q = instance->rotation;
	float total[4];
	float pivot[3];

	// total = q * spin (the spin first)
	total[0] = q[3] * spin[0] + q[0] * spin[3] + q[1] * spin[2] - q[2] * spin[1];
	total[1] = q[3] * spin[1] - q[0] * spin[2] + q[1] * spin[3] + q[2] * spin[0];
	total[2] = q[3] * spin[2] + q[0] * spin[1] - q[1] * spin[0] + q[2] * spin[3];
	total[3] = q[3] * spin[3] - q[0] * spin[0] - q[1] * spin[1] - q[2] * spin[2];
	spin_rotate(q, type->spin_pivot, pivot);
	for (int k = 0; k < 3; k++)
	{
		placement[k] += pivot[k] * placement[3];
	}
	SDL_memcpy(rotation, total, sizeof(total));
}

static void pack_slot(const objects_t *objects, int slot, uint32_t *out)
{
	int k = objects->slot_instance[slot];

	out[0] = (uint32_t)slot;
	SDL_memset(&out[1], 0, sizeof(uint32_t) * (RECORD_WORDS + 1));
	out[1 + 6] = MODEL_NONE;
	out[1 + RECORD_WORDS] = CELL_NONE;
	if (k < 0 || k >= objects->instance_count)
	{
		return;
	}
	{
		const object_instance_t *instance = &objects->instances[k];
		const object_type_t *type = &objects->types.types[instance->type];
		int model = objects_instance_model(objects, instance);
		float placement[4] = { instance->x, objects_instance_base_y(instance), instance->z, instance->scale };
		float turned[4] = { instance->rotation[0], instance->rotation[1], instance->rotation[2], instance->rotation[3] };
		int16_t rotation[4];
		// (a turning one moves every frame: no grid cell, as a loose one,
		// drawn in the near map's moving layer and culled round its sphere)
		bool moving = type->spins || (instance->state & OBJECT_STATE_MOVING) != 0;
		bool free = (instance->state & OBJECT_STATE_FREE) != 0 || moving;

		if (type->spins)
		{
			spin_placement(objects, instance, type, placement, turned);
		}
		SDL_memcpy(&out[1], placement, sizeof(placement));
		for (int i = 0; i < 4; i++)
		{
			rotation[i] = (int16_t)SDL_lroundf(SDL_clamp(turned[i], -1.0f, 1.0f) * 32767.0f);
		}
		out[1 + 4] = (uint32_t)(uint16_t)rotation[0] | ((uint32_t)(uint16_t)rotation[1] << 16);
		out[1 + 5] = (uint32_t)(uint16_t)rotation[2] | ((uint32_t)(uint16_t)rotation[3] << 16);
		out[1 + 6] = (model < 0 ? MODEL_NONE : (uint32_t)model) | ((uint32_t)instance->type << 16);
		// (flags: none now)
		out[1 + 7] = 0u;
		out[1 + 8] = instance->emissive_off[0];
		out[1 + 9] = instance->emissive_off[1];
		out[1 + RECORD_WORDS] = (free ? CELL_NONE : (uint32_t)grid_cell_of(objects, instance->x, instance->z))
			| (free || instance->flatten > 0.0f ? FLAG_TUMBLES : 0u) | (moving ? FLAG_SPINS : 0u)
			| (k < objects->sorted_count && objects->stray_marks != NULL && objects->stray_marks[k] ? FLAG_STRAY : 0u);
	}
}

static void dispatch_cull(objects_t *objects, SDL_GPUCommandBuffer *command_buffer, pass_t pass, cull_uniforms_t *uniforms, int mode,
	uint32_t groups)
{
	SDL_GPUStorageBufferReadWriteBinding read_write[5];
	// (SDL_GPU allows 8 read-only storage buffers a stage: the pass's ranges
	// are in its visible cells' buffer, after the bits)
	SDL_GPUBuffer *read_only[8] = { objects->record_buffer, objects->slot_cell_buffer, objects->type_buffer, objects->model_buffer,
		objects->draw_buffer, objects->cell_visible_buffers[pass], hiz_buffer(objects->occlusion), objects->order_buffer };
	SDL_GPUComputePass *compute;

	SDL_zeroa(read_write);
	read_write[0].buffer = objects->count_buffers[pass];
	read_write[1].buffer = objects->instance_buffers[pass];
	read_write[2].buffer = objects->argument_storage[pass];
	read_write[3].buffer = objects->decision_buffers[pass];
	read_write[4].buffer = objects->occlusion_debug_buffer;
	compute = SDL_BeginGPUComputePass(command_buffer, NULL, 0, read_write, 5);
	if (compute == NULL)
	{
		return;
	}
	SDL_BindGPUComputePipeline(compute, objects->cull_pipeline);
	SDL_BindGPUComputeStorageBuffers(compute, 0, read_only, 8);
	uniforms->mode = mode;
	SDL_PushGPUComputeUniformData(command_buffer, 0, uniforms, sizeof(*uniforms));
	SDL_DispatchGPUCompute(compute, SDL_clamp(groups, 1u, (uint32_t)MAXIMUM_GROUPS), 1, 1);
	SDL_EndGPUComputePass(compute);
}

// The slots of the objects that changed go up (in batches), and the
// record buffers grow with the slots
static bool flush_slots(objects_t *objects)
{
	if (objects->slot_count == 0)
	{
		return true;
	}
	// Free objects move with the physics every frame; their slots follow
	// (those after the cells, and the strays come loose since the sort)
	{
		int cell_count = objects->grid_size * objects->grid_size;

		for (int k = objects->grid_first[cell_count]; k < objects->grid_first[cell_count + 1]; k++)
		{
			touch_slot(objects, objects->instances[k].gpu_slot);
		}
		for (int n = 0; n < objects->stray_count; n++)
		{
			int k = objects->strays[n];

			if (k < objects->instance_count && (objects->instances[k].state & OBJECT_STATE_FREE) != 0)
			{
				touch_slot(objects, objects->instances[k].gpu_slot);
			}
		}
		for (int k = objects->sorted_count; k < objects->instance_count; k++)
		{
			if ((objects->instances[k].state & OBJECT_STATE_FREE) != 0)
			{
				touch_slot(objects, objects->instances[k].gpu_slot);
			}
		}
	}
	if ((uint32_t)objects->slot_count > objects->slot_capacity)
	{
		uint32_t capacity = (uint32_t)objects->slot_count + (uint32_t)objects->slot_count / 2 + 1024;
		uint32_t record_bytes = 0;
		uint32_t cell_bytes = 0;

		// New buffers: everything goes up again
		if (objects->record_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, objects->record_buffer);
			objects->record_buffer = NULL;
		}
		if (objects->slot_cell_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, objects->slot_cell_buffer);
			objects->slot_cell_buffer = NULL;
		}
		objects->record_buffer = grow_buffer(NULL, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
			&record_bytes, capacity * sizeof(uint32_t) * RECORD_WORDS);
		objects->slot_cell_buffer = grow_buffer(NULL, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
			&cell_bytes, capacity * sizeof(uint32_t));
		if (objects->record_buffer == NULL || objects->slot_cell_buffer == NULL)
		{
			objects->slot_capacity = 0;
			return false;
		}
		objects->slot_capacity = SDL_min(record_bytes / (uint32_t)(sizeof(uint32_t) * RECORD_WORDS), cell_bytes / (uint32_t)sizeof(uint32_t));
		for (int slot = 0; slot < objects->slot_count; slot++)
		{
			touch_slot(objects, slot);
		}
	}
	if (objects->dirty_count == 0)
	{
		return true;
	}
	// Which instance is in which slot: the sorting moved them
	if (objects->slot_map_generation != objects->generation)
	{
		for (int slot = 0; slot < objects->slot_count; slot++)
		{
			objects->slot_instance[slot] = -1;
		}
		for (int k = 0; k < objects->instance_count; k++)
		{
			int slot = objects->instances[k].gpu_slot;

			if (slot >= 0 && slot < objects->slot_count)
			{
				objects->slot_instance[slot] = k;
			}
		}
		objects->slot_map_generation = objects->generation;
	}
	if (objects->update_staging == NULL)
	{
		objects->update_staging = SDL_malloc(sizeof(uint32_t) * UPDATE_WORDS * UPDATE_BATCH);
		if (objects->update_staging == NULL)
		{
			return false;
		}
	}
	objects->update_buffer = grow_buffer(objects->update_buffer, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, &objects->update_capacity,
		sizeof(uint32_t) * UPDATE_WORDS * UPDATE_BATCH);
	if (objects->update_buffer == NULL)
	{
		return false;
	}
	for (int first = 0; first < objects->dirty_count; first += UPDATE_BATCH)
	{
		int count = SDL_min(UPDATE_BATCH, objects->dirty_count - first);
		SDL_GPUCommandBuffer *command_buffer;
		SDL_GPUStorageBufferReadWriteBinding read_write[2];
		SDL_GPUComputePass *compute;
		int32_t uniforms[4] = { count, 0, 0, 0 };

		for (int i = 0; i < count; i++)
		{
			int slot = objects->dirty_slots[first + i];

			pack_slot(objects, slot, &objects->update_staging[(size_t)i * UPDATE_WORDS]);
			objects->slot_dirty[slot] = 0;
		}
		gpu_upload_buffer(objects->update_buffer, 0, objects->update_staging, sizeof(uint32_t) * UPDATE_WORDS * (size_t)count);
		gpu_flush_uploads();
		command_buffer = gpu_commands();
		if (command_buffer == NULL)
		{
			return false;
		}
		SDL_zeroa(read_write);
		read_write[0].buffer = objects->record_buffer;
		read_write[1].buffer = objects->slot_cell_buffer;
		compute = SDL_BeginGPUComputePass(command_buffer, NULL, 0, read_write, 2);
		if (compute == NULL)
		{
			return false;
		}
		SDL_BindGPUComputePipeline(compute, objects->update_pipeline);
		SDL_BindGPUComputeStorageBuffers(compute, 0, &objects->update_buffer, 1);
		SDL_PushGPUComputeUniformData(command_buffer, 0, uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(compute, (uint32_t)(count + 255) / 256, 1, 1);
		SDL_EndGPUComputePass(compute);
	}
	objects->dirty_count = 0;
	return true;
}

// The grid cells the pass may see (a bit each, for the GPU), and how many
// objects it can draw at most: for each cell, those whose type is drawn
// at least as far as the cell is (within a cell they are sorted by draw
// distance, the farthest first), and every free object
// A type's models (its own, damaged, destroyed) marked in a per-model table
static void mark_type_models(const objects_t *objects, uint8_t *table, int type_index)
{
	const object_type_t *type = &objects->types.types[type_index];

	if (type->model >= 0 && type->model < objects->model_count)
	{
		table[type->model] = 1;
	}
	if (type->damaged_model >= 0 && type->damaged_model < objects->model_count)
	{
		table[type->damaged_model] = 1;
	}
	if (type->destroyed_model >= 0 && type->destroyed_model < objects->model_count)
	{
		table[type->destroyed_model] = 1;
	}
}

// The pass's table of models in view made ready (empty); NULL: none kept
// (the draws then go by model_used)
static uint8_t *begin_models_in_view(objects_t *objects, pass_t pass)
{
	objects->model_in_view_valid[pass] = false;
	if (pass == PASS_FAR_SHADOW || objects->model_count <= 0)
	{
		return NULL;
	}
	if (objects->model_in_view_capacity[pass] < objects->model_count)
	{
		uint8_t *grown = SDL_realloc(objects->model_in_view[pass], (size_t)objects->model_count);

		if (grown == NULL)
		{
			return NULL;
		}
		objects->model_in_view[pass] = grown;
		objects->model_in_view_capacity[pass] = objects->model_count;
	}
	SDL_memset(objects->model_in_view[pass], 0, (size_t)objects->model_count);
	return objects->model_in_view[pass];
}

// The culling's order on the GPU brought up to date (see objects_t.order_buffer):
// whole after a sort, its tail when those added since or the strays changed
static bool update_order(objects_t *objects)
{
	uint32_t entries = (uint32_t)objects->instance_count + (uint32_t)objects->stray_count;
	uint32_t first = 0;
	bool whole = !objects->order_valid || objects->order_sort_serial != objects->sort_serial
		|| objects->order_sorted_count != objects->sorted_count;

	if (!whole && objects->order_instance_count == objects->instance_count && objects->order_stray_count == objects->stray_count)
	{
		return objects->order_buffer != NULL;
	}
	if (objects->order_staging == NULL || objects->order_staging_capacity < SDL_max(entries, 1u))
	{
		uint32_t capacity = SDL_max(entries + entries / 2, 4096u);
		uint32_t *grown = SDL_realloc(objects->order_staging, sizeof(uint32_t) * capacity);

		if (grown == NULL)
		{
			objects->order_valid = false;
			return false;
		}
		objects->order_staging = grown;
		objects->order_staging_capacity = capacity;
	}
	if (objects->order_buffer == NULL || objects->order_capacity_bytes < sizeof(uint32_t) * SDL_max(entries, 1u))
	{
		objects->order_buffer = grow_buffer(objects->order_buffer, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, &objects->order_capacity_bytes,
			(uint32_t)sizeof(uint32_t) * SDL_max(entries, 1u));
		if (objects->order_buffer == NULL)
		{
			objects->order_valid = false;
			return false;
		}
		// (a new buffer: all of it goes up)
		whole = true;
	}
	if (!whole)
	{
		first = (uint32_t)objects->sorted_count;
	}
	// The instances' slots (the strays' none where the sort left them),
	// then the strays'
	for (uint32_t k = first; k < (uint32_t)objects->instance_count; k++)
	{
		int slot = objects->instances[k].gpu_slot;
		bool stray = (int)k < objects->sorted_count && objects->stray_marks != NULL && objects->stray_marks[k];

		objects->order_staging[k] = slot < 0 || stray ? ORDER_NONE : (uint32_t)slot;
	}
	for (int n = 0; n < objects->stray_count; n++)
	{
		int k = objects->strays[n];
		int slot = k < objects->instance_count ? objects->instances[k].gpu_slot : -1;

		objects->order_staging[objects->instance_count + n] = slot < 0 ? ORDER_NONE : (uint32_t)slot;
	}
	// (a stray come since the whole order went up is still there where the
	// sort left it: its slot's cell word has FLAG_STRAY, and the GPU skips
	// it there; only the tail goes up again)
	if (entries > first)
	{
		gpu_upload_buffer(objects->order_buffer, first * (uint32_t)sizeof(uint32_t), &objects->order_staging[first],
			sizeof(uint32_t) * (size_t)(entries - first));
	}
	objects->order_valid = true;
	objects->order_sort_serial = objects->sort_serial;
	objects->order_sorted_count = objects->sorted_count;
	objects->order_instance_count = objects->instance_count;
	objects->order_stray_count = objects->stray_count;
	return true;
}

// A range of the order for the pass to look at (joined to the one before
// when it goes on from it)
static void add_order_range(objects_t *objects, uint32_t order_first, uint32_t order_last, uint32_t *range_count, uint32_t *work_total)
{
	uint32_t count = *range_count;

	if (order_last <= order_first)
	{
		return;
	}
	if (count > 0)
	{
		uint32_t previous_first = objects->range_staging[2u * (count - 1u)];
		uint32_t previous_work = objects->range_staging[2u * (count - 1u) + 1u];

		if (previous_first + (*work_total - previous_work) == order_first)
		{
			*work_total += order_last - order_first;
			return;
		}
	}
	if (objects->range_staging == NULL || objects->range_staging_capacity <= count)
	{
		uint32_t capacity = SDL_max(objects->range_staging_capacity * 2u, 1024u);
		uint32_t *grown = SDL_realloc(objects->range_staging, sizeof(uint32_t) * 2u * capacity);

		if (grown == NULL)
		{
			return;
		}
		objects->range_staging = grown;
		objects->range_staging_capacity = capacity;
	}
	objects->range_staging[2u * count] = order_first;
	objects->range_staging[2u * count + 1u] = *work_total;
	*work_total += order_last - order_first;
	*range_count = count + 1u;
}

// Whether no water is anywhere near grid cell (i, j)'s objects (they reach
// over its edge by their radius, the water's surface is taken from the four
// cells round a point: a cell more): all the water's tiles there dry
static bool cell_dry(const objects_t *objects, int i, int j)
{
	const water_t *water = objects->dry_water;
	float tile_meters;
	float reach = 12.0f;
	int32_t first_x;
	int32_t first_y;
	int32_t last_x;
	int32_t last_y;

	if (water == NULL || water->tile_state == NULL || water->tile_total_depth == NULL || water->tiles_per_side <= 0)
	{
		return false;
	}
	tile_meters = (float)WATER_TILE_SIZE * water->cell_size_meters;
	reach += 2.0f * water->cell_size_meters;
	first_x = clamp_int((int32_t)SDL_floorf(((float)i * OBJECTS_GRID_CELL_METERS - reach) / tile_meters), 0, water->tiles_per_side - 1);
	first_y = clamp_int((int32_t)SDL_floorf(((float)j * OBJECTS_GRID_CELL_METERS - reach) / tile_meters), 0, water->tiles_per_side - 1);
	last_x = clamp_int((int32_t)SDL_floorf(((float)(i + 1) * OBJECTS_GRID_CELL_METERS + reach) / tile_meters), 0, water->tiles_per_side - 1);
	last_y = clamp_int((int32_t)SDL_floorf(((float)(j + 1) * OBJECTS_GRID_CELL_METERS + reach) / tile_meters), 0, water->tiles_per_side - 1);
	for (int32_t y = first_y; y <= last_y; y++)
	{
		for (int32_t x = first_x; x <= last_x; x++)
		{
			int32_t tile = y * water->tiles_per_side + x;

			if (water->tile_state[tile] != WATER_TILE_DRY || water->tile_total_depth[tile] > 0)
			{
				return false;
			}
		}
	}
	return true;
}

// The pass's visible cells (a bit each, for the GPU) and the ranges of the
// order it looks at (work_out: their total, range_count_out: how many);
// returns how many objects it may draw at most
static uint32_t find_cells(objects_t *objects, pass_t pass, const frustum_t *frustum, vec3_t eye, uint32_t *work_out, uint32_t *range_count_out)
{
	uint32_t work_total = 0;
	uint32_t range_count = 0;
	uint8_t *in_view = begin_models_in_view(objects, pass);
	int cell_count = objects->grid_size * objects->grid_size;
	uint32_t words = (uint32_t)(cell_count + 31) / 32;
	uint32_t bound = 0;

	if (objects->cell_visible_staging == NULL || objects->cell_visible_words < words)
	{
		// (the visible bits, then the dry ones)
		uint32_t *grown = SDL_realloc(objects->cell_visible_staging, sizeof(uint32_t) * 2u * words);

		if (grown == NULL)
		{
			return 0;
		}
		objects->cell_visible_staging = grown;
		objects->cell_visible_words = words;
		for (int p = 0; p < 3; p++)
		{
			if (objects->cell_visible_buffers[p] != NULL)
			{
				SDL_ReleaseGPUBuffer(gpu.device, objects->cell_visible_buffers[p]);
			}
			objects->cell_visible_buffers[p] = NULL;
			objects->cell_visible_capacity_bytes[p] = 0;
		}
	}
	objects->cell_words_used = words;
	SDL_memset(objects->cell_visible_staging, 0, sizeof(uint32_t) * 2u * words);
	for (int j = 0; j < objects->grid_size; j++)
	{
		for (int i = 0; i < objects->grid_size; i++)
		{
			int cell = j * objects->grid_size + i;
			int first = objects->grid_first[cell];
			int last = objects->grid_first[cell + 1];
			vec3_t cell_minimum;
			vec3_t cell_maximum;
			float cell_distance;

			if (first == last)
			{
				continue;
			}
			// Objects reach over the cell's edge by their radius
			cell_minimum = vec3_make((float)i * OBJECTS_GRID_CELL_METERS - 12.0f, objects->grid_minimum_y[cell],
				(float)j * OBJECTS_GRID_CELL_METERS - 12.0f);
			cell_maximum = vec3_make((float)(i + 1) * OBJECTS_GRID_CELL_METERS + 12.0f, objects->grid_maximum_y[cell],
				(float)(j + 1) * OBJECTS_GRID_CELL_METERS + 12.0f);
			if (frustum != NULL && !frustum_contains_box(frustum, cell_minimum, cell_maximum))
			{
				continue;
			}
			if (pass != PASS_FAR_SHADOW)
			{
				int low = first;
				int high = last;

				cell_distance = distance_to_box(eye, cell_minimum, cell_maximum);
				if (cell_distance > objects->grid_draw_distance[cell] * objects->draw_distance_scale)
				{
					continue;
				}
				// The first object drawn less far than the cell is
				while (low < high)
				{
					int middle = (low + high) / 2;

					if (objects->types.types[objects->instances[middle].type].draw_distance_meters * objects->draw_distance_scale >= cell_distance)
					{
						low = middle + 1;
					}
					else
					{
						high = middle;
					}
				}
				last = low;
			}
			bound += (uint32_t)(last - first);
			objects->cell_visible_staging[cell >> 5] |= 1u << (cell & 31);
			if (pass == PASS_CAMERA && cell_dry(objects, i, j))
			{
				objects->cell_visible_staging[words + (uint32_t)(cell >> 5)] |= 1u << (cell & 31);
			}
			// (the cells hold what was sorted: those added since are in the tail)
			add_order_range(objects, (uint32_t)first, (uint32_t)SDL_min(last, objects->sorted_count), &range_count, &work_total);
			// (the cell's objects the GPU may draw: what is past its draw
			// distance is not among them)
			// By its runs of one type (a type turned into another since
			// the sort is a stray: marked below), not object by object
			if (in_view != NULL && objects->run_cells == cell_count)
			{
				for (int r = objects->run_first[cell]; r < objects->run_first[cell + 1] && objects->run_start[r] < last; r++)
				{
					mark_type_models(objects, in_view, objects->run_type[r]);
				}
			}
			else if (in_view != NULL)
			{
				for (int k = first; k < last; k++)
				{
					mark_type_models(objects, in_view, objects->instances[k].type);
				}
			}
		}
	}
	bound += (uint32_t)(objects->grid_first[cell_count + 1] - objects->grid_first[cell_count]);
	// (and the strays and those added since the sort: wherever they are)
	bound += (uint32_t)objects->stray_count + (uint32_t)SDL_max(objects->instance_count - objects->sorted_count, 0);
	// The free objects (culled one by one on the GPU) all counted in view
	if (in_view != NULL)
	{
		for (int k = objects->grid_first[cell_count]; k < objects->grid_first[cell_count + 1]; k++)
		{
			mark_type_models(objects, in_view, objects->instances[k].type);
		}
		for (int n = 0; n < objects->stray_count; n++)
		{
			if (objects->strays[n] < objects->instance_count)
			{
				mark_type_models(objects, in_view, objects->instances[objects->strays[n]].type);
			}
		}
		for (int k = objects->sorted_count; k < objects->instance_count; k++)
		{
			mark_type_models(objects, in_view, objects->instances[k].type);
		}
		objects->model_in_view_valid[pass] = true;
	}
	if (pass == PASS_FAR_SHADOW)
	{
		// Everything is in view there, but only what casts far shadows is
		// drawn (not the grass): count those (this pass is rare)
		bound = 0;
		for (int k = 0; k < objects->instance_count; k++)
		{
			bound += objects->types.types[objects->instances[k].type].shadow == OBJECT_SHADOW_ALL ? 1u : 0u;
		}
	}
	// The loose ones (after the cells), then the order's tail: those added
	// since the sort and the strays
	add_order_range(objects, (uint32_t)objects->grid_first[cell_count],
		(uint32_t)SDL_min(objects->grid_first[cell_count + 1], objects->sorted_count), &range_count, &work_total);
	add_order_range(objects, (uint32_t)objects->sorted_count, (uint32_t)objects->instance_count + (uint32_t)objects->stray_count,
		&range_count, &work_total);
	// The visible bits and, after them, the ranges, in one buffer (grown
	// first: what goes up goes into the buffer as it is then)
	{
		uint32_t needed = (uint32_t)sizeof(uint32_t) * (2u * words + 2u * SDL_max(range_count, 1u));

		objects->cell_visible_buffers[pass] = grow_buffer(objects->cell_visible_buffers[pass], SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
			&objects->cell_visible_capacity_bytes[pass], needed);
		if (objects->cell_visible_buffers[pass] == NULL)
		{
			range_count = 0;
			work_total = 0;
		}
		else
		{
			gpu_upload_buffer(objects->cell_visible_buffers[pass], 0, objects->cell_visible_staging, sizeof(uint32_t) * 2u * words);
			if (range_count > 0)
			{
				gpu_upload_buffer(objects->cell_visible_buffers[pass], 2u * words * (uint32_t)sizeof(uint32_t), objects->range_staging,
					sizeof(uint32_t) * 2u * range_count);
			}
		}
	}
	*work_out = work_total;
	*range_count_out = range_count;
	return bound;
}

// Fills the pass's instance buffer and draw arguments on the GPU
static void cull_pass(objects_t *objects, pass_t pass, const frustum_t *frustum, vec3_t eye)
{
	cull_uniforms_t uniforms;
	uint32_t bound;
	uint32_t work_count = 0;
	uint32_t range_count = 0;
	uint32_t work_groups;
	SDL_GPUCommandBuffer *command_buffer;

	objects->prepared_pass = -1;
	if (objects->cull_pipeline == NULL || gpu.device == NULL || objects->occlusion == NULL || hiz_buffer(objects->occlusion) == NULL)
	{
		return;
	}
	if (!objects->gpu_tables_ready && !upload_tables(objects))
	{
		return;
	}
	objects_sort(objects);
	if (!flush_slots(objects) || objects->slot_count == 0 || objects->record_buffer == NULL)
	{
		return;
	}
	if (!update_order(objects))
	{
		return;
	}
	bound = find_cells(objects, pass, frustum, eye, &work_count, &range_count);
	if (objects->cell_visible_buffers[pass] == NULL)
	{
		return;
	}
	{
		uint32_t bytes = 0;
		uint32_t needed = SDL_max(bound, 1024u);

		if (objects->instance_buffers[pass] == NULL || objects->instance_capacity_per_pass[pass] < needed)
		{
			bytes = objects->instance_capacity_per_pass[pass] * (uint32_t)sizeof(float) * INSTANCE_FLOATS;
			objects->instance_buffers[pass] = grow_buffer(objects->instance_buffers[pass],
				SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, &bytes, needed * (uint32_t)sizeof(float) * INSTANCE_FLOATS);
			objects->instance_capacity_per_pass[pass] = bytes / ((uint32_t)sizeof(float) * INSTANCE_FLOATS);
			if (objects->instance_buffers[pass] == NULL)
			{
				return;
			}
		}
		if (objects->decision_buffers[pass] == NULL || objects->decision_capacity[pass] < (uint32_t)objects->slot_count)
		{
			// Two each: the fade, the level of detail
			bytes = objects->decision_capacity[pass] * 2u * (uint32_t)sizeof(float);
			objects->decision_buffers[pass] = grow_buffer(objects->decision_buffers[pass],
				SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, &bytes,
				(uint32_t)objects->slot_count * 2u * (uint32_t)sizeof(float));
			objects->decision_capacity[pass] = bytes / (2u * (uint32_t)sizeof(float));
			if (objects->decision_buffers[pass] == NULL)
			{
				return;
			}
		}
	}

	SDL_zero(uniforms);
	uniforms.least_height = pass == PASS_FAR_SHADOW ? SDL_max(objects->far_shadow_least_height, objects->far_shadow_small_meters) : 0.0f;
	uniforms.free_filter = pass == PASS_NEAR_SHADOW ? objects->shadow_free_filter : OBJECTS_SHADOW_ALL;
	if (frustum != NULL)
	{
		for (int i = 0; i < 6; i++)
		{
			uniforms.planes[4 * i] = frustum->planes[i].a;
			uniforms.planes[4 * i + 1] = frustum->planes[i].b;
			uniforms.planes[4 * i + 2] = frustum->planes[i].c;
			uniforms.planes[4 * i + 3] = frustum->planes[i].d;
		}
		uniforms.has_frustum = 1;
	}
	uniforms.eye[0] = eye.x;
	uniforms.eye[1] = eye.y;
	uniforms.eye[2] = eye.z;
	uniforms.eye[3] = objects->draw_distance_scale;
	uniforms.pass_kind = (int32_t)pass;
	uniforms.slot_count = objects->slot_count;
	uniforms.work[0] = (int32_t)work_count;
	uniforms.work[1] = (int32_t)range_count;
	uniforms.work[2] = objects->instance_count;
	uniforms.work[3] = (int32_t)(2u * objects->cell_words_used);
	uniforms.water[0] = (int32_t)objects->cell_words_used;
	uniforms.water[1] = pass == PASS_CAMERA && objects->dry_water != NULL && objects->dry_water->tile_state != NULL ? 1 : 0;
	uniforms.model_count = objects->model_count;
	uniforms.draw_count = objects->draw_count;
	uniforms.capacity = (int32_t)objects->instance_capacity_per_pass[pass];
	uniforms.highlight_slot = objects->highlight_valid && objects->highlight_generation == objects->generation
		&& objects->highlight_instance >= 0 && objects->highlight_instance < objects->instance_count && pass == PASS_CAMERA
		? objects->instances[objects->highlight_instance].gpu_slot : -1;
	uniforms.fade_fraction = FADE_FRACTION;
	uniforms.impostor_factor = pass == PASS_CAMERA && objects->impostors_enabled ? objects->impostor_factor : 0.0f;
	// Levels of detail: the camera's by size on screen; the near shadow map
	// at least level 1 (the finest branches' shadows are lost in the
	// leaves'), the far one at level 2
	uniforms.lod[0] = objects->lod_enabled ? objects->lod_factor : 0.0f;
	uniforms.lod[1] = LOD_FADE_FRACTION;
	uniforms.lod[2] = !objects->lod_enabled ? 0.0f : (pass == PASS_FAR_SHADOW ? 2.0f : (pass == PASS_NEAR_SHADOW ? 1.0f : 0.0f));
	uniforms.lod[3] = SDL_clamp(objects->lod_foliage_kept, 0.05f, 1.0f);
	for (int k = 0; k < 3; k++)
	{
		uniforms.lod_pixels[k] = lod_base_pixels[k] / SDL_max(objects->lod_distance_scale, 0.05f);
	}
	uniforms.lod_pixels[3] = SDL_clamp(objects->prop_lod_pixels, 0.05f, 50.0f);
	if (pass == PASS_CAMERA && objects->occlusion->ready && !objects->no_terrain_occlusion)
	{
		// The matrix the pyramid was made with (last frame's camera), as
		// the shaders draw with it (depth 0..1)
		gpu_clip_matrix(objects->occlusion->view_projection, uniforms.view_projection);
		uniforms.hiz_size[0] = (float)objects->occlusion->width;
		uniforms.hiz_size[1] = (float)objects->occlusion->height;
		uniforms.hiz_size[2] = (float)objects->occlusion->level_count;
		hiz_level_table(objects->occlusion, uniforms.hiz_levels);
		uniforms.hiz_size[3] = 1.0f;
		uniforms.debug[0] = !objects->occlusion_shown ? 0.0f : (objects->occlusion_all_shown ? 2.0f : 1.0f);
	}

	gpu_flush_uploads();
	command_buffer = gpu_commands();
	if (command_buffer == NULL)
	{
		return;
	}
	// One thread per object in the pass's ranges (not per slot: most are
	// in cells nobody sees); more than the dispatch allows loop
	work_groups = (work_count + 255u) / 256u;
	dispatch_cull(objects, command_buffer, pass, &uniforms, CULL_CLEAR, ((uint32_t)objects->model_count + 255) / 256);
	dispatch_cull(objects, command_buffer, pass, &uniforms, CULL_COUNT, work_groups);
	dispatch_cull(objects, command_buffer, pass, &uniforms, CULL_OFFSETS, 1);
	dispatch_cull(objects, command_buffer, pass, &uniforms, CULL_SCATTER, work_groups);
	{
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command_buffer);
		SDL_GPUBufferLocation source = { objects->argument_storage[pass], 0 };
		SDL_GPUBufferLocation destination = { objects->argument_buffers[pass], 0 };

		if (copy != NULL)
		{
			SDL_CopyGPUBufferToBuffer(copy, &source, &destination,
				(uint32_t)(sizeof(uint32_t) * DRAW_ARGUMENT_WORDS * (size_t)SDL_max(objects->draw_count, 1)), false);
			SDL_EndGPUCopyPass(copy);
		}
	}
	objects->prepared_pass = (int)pass;
	if (pass == PASS_CAMERA)
	{
		// What the GPU may draw at most (it culls the rest one by one)
		objects->drawn_instance_count = (int)bound;
	}
}

// Draws every material of every model whose alpha testing matches, from
// the instances and arguments the GPU wrote for the pass: the plants'
// (plants: with the plants' pipeline bound) or the rest
#define PLANT_MONTH_CELLS 32                 // plant.glsl's

// A plant material in the month (season.h): how far into its autumn (0
// green .. 1 bare), the snow on its bark (and its branches' sag)
static void plant_month(const objects_t *objects, const object_material_t *material, float *autumn_out, float *snow_out)
{
	float first = material->autumn_first_month >= 1.0f ? material->autumn_first_month : 10.0f;
	float last = material->autumn_last_month >= 1.0f ? material->autumn_last_month : 11.0f;
	float leaves = material->leaves_month >= 1.0f ? material->leaves_month : 4.0f;

	*autumn_out = season_autumn(objects->month, first, last, leaves);
	*snow_out = objects->snow_amount;
}

// A float as a half (IEEE 754 binary16; plant.glsl unpacks two of them
// from a float's bits: the atlas' offsets, sixteenths and such, are exact)
static uint16_t half_of(float value)
{
	uint32_t bits;
	uint32_t sign;
	int32_t exponent;
	uint32_t mantissa;

	SDL_memcpy(&bits, &value, sizeof(bits));
	sign = (bits >> 16) & 0x8000u;
	exponent = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
	mantissa = bits & 0x7FFFFFu;
	if (exponent <= 0)
	{
		return (uint16_t)sign;
	}
	if (exponent >= 31)
	{
		return (uint16_t)(sign | 0x7C00u);
	}
	return (uint16_t)(sign | ((uint32_t)exponent << 10) | ((mantissa + 0x1000u) >> 13));
}

// The plant's pictures that show others this month (plant.glsl): their
// cells and how far, after the uniforms' first floats; how many. Through
// the snow's months and the month either side of them (with the snow from
// December to February: November to March), by the snow on things
// instead, for an evergreen (by_snow: a conifer's foliage never falls; a
// cherry's March pictures are its leaves, they go by the month): four
// pictures, the least snow first (the plant's winter
// pictures as tools/potop_plant has them: the month after the snow's last,
// the month before its first, its first, the middle of it: March,
// November, December, January), the offsets packed as halves (x: the
// first two, y: the last two); how many, below 0
static int plant_month_cells(const objects_t *objects, const object_model_t *model, int month_index, bool by_snow,
	float cells_out[PLANT_MONTH_CELLS * 4], float offsets_out[PLANT_MONTH_CELLS * 4])
{
	int count = 0;
	int first = (int)objects->snow_first_month - 1;     // (0 January)
	int last = (int)objects->snow_last_month - 1;
	int span = (last - first + 12) % 12 + 1;

	if (by_snow && objects->snow_first_month >= 1.0f && objects->snow_last_month >= 1.0f
		&& (month_index - (first + 11) % 12 + 12) % 12 < span + 2)
	{
		int levels[4] = { (last + 1) % 12, (first + 11) % 12, first, (first + span / 2) % 12 };

		for (int c = 0; c < model->month_cell_count && count < PLANT_MONTH_CELLS; c++)
		{
			const object_file_month_cell_t *cell = &model->month_cells[c];
			bool any = false;

			for (int k = 0; k < 4; k++)
			{
				any |= cell->month_offsets[levels[k]][0] != 0.0f || cell->month_offsets[levels[k]][1] != 0.0f;
			}
			if (!any)
			{
				continue;
			}
			SDL_memcpy(&cells_out[count * 4], cell->cell, sizeof(float) * 4);
			{
				uint32_t words[4];

				for (int k = 0; k < 2; k++)
				{
					const float *a = cell->month_offsets[levels[2 * k]];
					const float *b = cell->month_offsets[levels[2 * k + 1]];

					words[2 * k] = (uint32_t)half_of(a[0]) | ((uint32_t)half_of(a[1]) << 16);
					words[2 * k + 1] = (uint32_t)half_of(b[0]) | ((uint32_t)half_of(b[1]) << 16);
				}
				SDL_memcpy(&offsets_out[count * 4], words, sizeof(words));
			}
			count++;
		}
		return -count;
	}

	for (int c = 0; c < model->month_cell_count && count < PLANT_MONTH_CELLS; c++)
	{
		const object_file_month_cell_t *cell = &model->month_cells[c];

		if (cell->month_offsets[month_index][0] == 0.0f && cell->month_offsets[month_index][1] == 0.0f)
		{
			continue;
		}
		SDL_memcpy(&cells_out[count * 4], cell->cell, sizeof(float) * 4);
		offsets_out[count * 4 + 0] = cell->month_offsets[month_index][0];
		offsets_out[count * 4 + 1] = cell->month_offsets[month_index][1];
		offsets_out[count * 4 + 2] = 0.0f;
		offsets_out[count * 4 + 3] = 0.0f;
		count++;
	}
	return count;
}

// Which of them a draw takes
typedef enum
{
	DRAW_OTHERS,                      // all but the plants (the camera's: and the props)
	DRAW_PLANTS,
	DRAW_PROPS,                       // the camera's only (the shadows draw them with the others)
	DRAW_BUILDINGS,                   // the same
	DRAW_CUT_OUT_ONE_SIDED,           // the camera's alpha tested: a building's, not double sided (its details)
} draw_kind_t;

// A material the buildings' pipeline draws (for the camera)
static bool building_material(const objects_t *objects, const object_model_t *model, const object_material_t *material)
{
	return objects->building_pipeline != NULL && material->aged && model->weathering_buffer != NULL && !material->alpha_tested;
}

// A material the props' pipeline draws (for the camera)
static bool prop_material(const objects_t *objects, const object_model_t *model, const object_material_t *material)
{
	return objects->prop_pipeline != NULL && (material->triplanar || material->weathered) && !material->alpha_tested
		&& !building_material(objects, model, material);
}

// A building's part drawn by the objects' own pipeline (its cut out details,
// the dark behind its slits and boards): shifted and marked with the
// building for the temporal anti-aliasing (a building alone has a
// weathering buffer)
static bool building_detail(draw_kind_t kind, const object_model_t *model)
{
	return kind == DRAW_CUT_OUT_ONE_SIDED || (kind == DRAW_OTHERS && model->weathering_buffer != NULL);
}

// Which kind of the camera's draws a material is in (not the plants')
static draw_kind_t camera_draw_kind(const objects_t *objects, const object_model_t *model, const object_material_t *material)
{
	if (building_material(objects, model, material))
	{
		return DRAW_BUILDINGS;
	}
	if (material->alpha_tested && !material->double_sided && model->weathering_buffer != NULL && objects->alpha_culled_pipeline != NULL)
	{
		return DRAW_CUT_OUT_ONE_SIDED;
	}
	return prop_material(objects, model, material) ? DRAW_PROPS : DRAW_OTHERS;
}

// object.frag's and prop.frag's material block
typedef struct
{
	float highlight_parameters[4];
	int32_t alpha_tested;
	int32_t pbr;
	int32_t lighting_normals;
	int32_t triplanar;
	float weathering[4];
	float show[4];                    // (the prop editor's)
	float relief[4];                  // a building's thatch (prop_body.glsl): x 1 its relief marched into, y how deep (m)
	float look[8];                    // the buildings' look (prop_body.glsl, objects_t.look)
} material_uniforms_t;

// prop.frag's block for a prop's material
static void prop_material_uniforms(const objects_t *objects, const object_model_t *model, const object_material_t *material,
	material_uniforms_t *uniforms)
{
	uniforms->pbr = objects->pbr_shading && material->shaded ? 1 : 0;
	uniforms->lighting_normals = 0;
	uniforms->triplanar = material->triplanar ? 1 : 0;
	uniforms->weathering[0] = material->triplanar_meters;
	uniforms->weathering[1] = material->weathered ? model->lichen : -1.0f;
	uniforms->weathering[2] = objects->snow_amount;
	uniforms->weathering[3] = model->moss;
}

// building.frag's block for a building's aged material: the weathering as
// much as its age says (moss and lichen at age 1), the age (show: y its
// type's, z its spread, w the dirt's height at its foot)
static void building_material_uniforms(const objects_t *objects, const object_model_t *model, const object_material_t *material,
	material_uniforms_t *uniforms)
{
	uniforms->pbr = objects->pbr_shading && material->shaded ? 1 : 0;
	uniforms->lighting_normals = 0;
	// (a building's hard stone may be laid on from the sides: no seams on
	// its boulders)
	uniforms->triplanar = material->triplanar ? 1 : 0;
	// (the model's own look comes with each instance: building_look in
	// prop_body.glsl; here only whether it is weathered at all, and the age
	// the debug settings force on every building, -1: none)
	(void)model;
	uniforms->weathering[0] = material->triplanar_meters;
	uniforms->weathering[1] = 0.0f;
	uniforms->weathering[2] = objects->snow_amount;
	uniforms->weathering[3] = 0.0f;
	uniforms->show[0] = 0.0f;
	uniforms->show[1] = objects->age_overridden ? objects->age_override : -1.0f;
	uniforms->show[2] = 0.0f;
	uniforms->show[3] = 0.0f;
	uniforms->relief[0] = material->thatch && !objects->relief_off ? 1.0f : 0.0f;
	uniforms->relief[1] = THATCH_RELIEF_METERS;
}

// Which models any object on the map may show: made again when the
// instance list changed (adds, removals: a harvest or a type change is one
// of each); a type's damaged and destroyed models counted with it, so a
// blow needs no update
static void refresh_models_used(objects_t *objects)
{
	if (objects->model_used_valid && objects->model_used_generation == objects->generation
		&& objects->model_used_capacity >= objects->model_count)
	{
		return;
	}
	if (objects->model_used_capacity < objects->model_count)
	{
		uint8_t *grown = SDL_realloc(objects->model_used, (size_t)objects->model_count);

		if (grown == NULL)
		{
			objects->model_used_valid = false;
			return;
		}
		objects->model_used = grown;
		objects->model_used_capacity = objects->model_count;
	}
	if (objects->model_count > 0)
	{
		SDL_memset(objects->model_used, 0, (size_t)objects->model_count);
	}
	for (int i = 0; i < objects->instance_count; i++)
	{
		mark_type_models(objects, objects->model_used, objects->instances[i].type);
	}
	objects->model_used_generation = objects->generation;
	objects->model_used_valid = true;
}

// Whether a model's draws are worth recording in the pass prepared: an
// object of it in a grid cell in view, or (the far shadow map) anywhere on
// the map (true while unknown)
static bool model_on_map(const objects_t *objects, int model)
{
	int pass = objects->prepared_pass;

	if (pass >= 0 && pass < 3 && objects->model_in_view_valid[pass])
	{
		return model >= objects->model_in_view_capacity[pass] || objects->model_in_view[pass][model] != 0;
	}
	return !objects->model_used_valid || model >= objects->model_used_capacity || objects->model_used[model] != 0;
}

// A deciduous climber through the year (object.frag's season, the
// material block's weathering): x how red its leaves (0 green .. 1 crimson),
// y how many of them are gone (0 .. 1: bare vines), z 1 (it is one).
// Reddening from mid September to mid October, falling from late October
// to late November, bare till mid April, its leaves back by mid May
static void deciduous_season(float month, float season_out[4])
{
	float m = SDL_fmodf(month - 1.0f + 12.0f, 12.0f) + 1.0f;
	float red = 0.0f;
	float gone = 0.0f;

	// (season.h: 1 the start of January, 9.5 the middle of September)
	if (m >= 9.5f && m < 11.8f)
	{
		red = SDL_clamp(m - 9.5f, 0.0f, 1.0f);
		gone = SDL_clamp(m - 10.8f, 0.0f, 1.0f);
	}
	else if (m >= 11.8f || m < 4.5f)
	{
		red = 1.0f;
		gone = 1.0f;
	}
	else if (m < 5.5f)
	{
		// (the new leaves come in reddish, then green)
		red = SDL_clamp(1.0f - (m - 4.5f), 0.0f, 1.0f) * 0.5f;
		gone = SDL_clamp(1.0f - (m - 4.5f), 0.0f, 1.0f);
	}
	season_out[0] = red;
	season_out[1] = gone;
	season_out[2] = 1.0f;
	season_out[3] = 0.0f;
}

static int draw_models(const objects_t *objects, SDL_GPURenderPass *pass, bool alpha_pass, draw_kind_t kind)
{
	int draw_calls = 0;
	int bound_model = -1;
	bool plants = kind == DRAW_PLANTS;
	bool props = kind == DRAW_PROPS;
	bool buildings = kind == DRAW_BUILDINGS;
	material_uniforms_t material_uniforms;

	SDL_zero(material_uniforms);
	SDL_memcpy(material_uniforms.highlight_parameters, objects->highlight_uniform, sizeof(material_uniforms.highlight_parameters));
	SDL_memcpy(material_uniforms.look, objects->look, sizeof(material_uniforms.look));
	material_uniforms.alpha_tested = alpha_pass ? 1 : 0;
	// The physically based shading for the materials with maps of their
	// own; pushed again only where that changes from one draw to the next
	material_uniforms.pbr = -1;
	material_uniforms.lighting_normals = -1;
	// The shadows: every opaque material of every pooled model in one call,
	// from the shared buffers (objects.h; the ones whose objects are out of
	// the map's view have no instances: nothing drawn of them)
	int first_draw = 0;

	if ((objects->prepared_pass == PASS_FAR_SHADOW || objects->prepared_pass == PASS_NEAR_SHADOW) && !plants && !alpha_pass
		&& objects->shadow_batch_count > 0 && objects->pool_vertex_buffer != NULL && objects->pool_index_buffer != NULL)
	{
		SDL_GPUBufferBinding vertex_bindings[2] =
		{
			{ objects->pool_vertex_buffer, 0 },
			{ objects->instance_buffers[objects->prepared_pass], 0 },
		};
		SDL_GPUBufferBinding index_binding = { objects->pool_index_buffer, 0 };
		const object_model_t *first_model = &objects->models[objects->draw_models[0]];
		SDL_GPUTextureSamplerBinding texture_binding = { first_model->materials[objects->draw_materials[0]].texture, gpu.linear_repeat };

		SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, 2);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		// (opaque: the texture is not looked at, but a sampler is bound)
		SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
		material_uniforms.pbr = 0;
		material_uniforms.lighting_normals = 0;
		SDL_zeroa(material_uniforms.weathering);
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material_uniforms, sizeof(material_uniforms));
		SDL_DrawGPUIndexedPrimitivesIndirect(pass, objects->argument_buffers[objects->prepared_pass], 0, (uint32_t)objects->shadow_batch_count);
		draw_calls++;
		first_draw = objects->shadow_batch_count;
	}
	// The camera's buildings: each material (its textures, its look) of
	// every pooled building in one call, from the shared buffers (objects.h;
	// a model's own look comes with its instances)
	if (objects->prepared_pass == PASS_CAMERA && buildings && !alpha_pass && objects->building_batch_count > 0 && objects->pool_vertex_buffer != NULL
		&& objects->pool_index_buffer != NULL && objects->pool_tangent_buffer != NULL && objects->pool_weathering_buffer != NULL)
	{
		SDL_GPUBufferBinding vertex_bindings[4] =
		{
			{ objects->pool_vertex_buffer, 0 },
			{ objects->instance_buffers[PASS_CAMERA], 0 },
			{ objects->pool_tangent_buffer, 0 },
			{ objects->pool_weathering_buffer, 0 },
		};
		SDL_GPUBufferBinding index_binding = { objects->pool_index_buffer, 0 };
		SDL_GPUSampler *sampler = gpu_biased_sampler(objects->texture_bias);
		int count = objects->building_batch_count;

		SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, 4);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		for (int d = 0; d < count;)
		{
			const object_model_t *model = &objects->models[objects->draw_models[d]];
			const object_material_t *material = &model->materials[objects->draw_materials[d]];
			int end = d + 1;

			while (end < count && compare_building_materials(material, &objects->models[objects->draw_models[end]].materials[objects->draw_materials[end]]) == 0)
			{
				end++;
			}
			{
				SDL_GPUTextureSamplerBinding texture_binding = { material->texture, sampler };
				SDL_GPUTextureSamplerBinding maps[3] =
				{
					{ material->normal_texture != NULL ? material->normal_texture : object_default_texture(OBJECT_DEFAULT_NORMAL), sampler },
					{ material->orm_texture != NULL ? material->orm_texture : object_default_texture(OBJECT_DEFAULT_ORM), sampler },
					{ material->emissive_texture != NULL ? material->emissive_texture : object_default_texture(OBJECT_DEFAULT_EMISSIVE), sampler },
				};

				building_material_uniforms(objects, model, material, &material_uniforms);
				SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material_uniforms, sizeof(material_uniforms));
				SDL_BindGPUFragmentSamplers(pass, 3, &texture_binding, 1);
				SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
			}
			SDL_DrawGPUIndexedPrimitivesIndirect(pass, objects->argument_buffers[PASS_CAMERA], (uint32_t)(sizeof(uint32_t) * DRAW_ARGUMENT_WORDS * (size_t)d),
				(uint32_t)(end - d));
			draw_calls++;
			d = end;
		}
		first_draw = count;
	}
	for (int d = first_draw; d < objects->draw_count; d++)
	{
		const object_model_t *model = &objects->models[objects->draw_models[d]];
		const object_material_t *material;
		SDL_GPUTextureSamplerBinding texture_binding;

		// The impostors' draws: see draw_impostors; models no object shows
		if (objects->draw_materials[d] < 0 || !model_on_map(objects, objects->draw_models[d]))
		{
			continue;
		}
		float plant_autumn = 0.0f;
		float plant_snow = 0.0f;

		material = &model->materials[objects->draw_materials[d]];
		if (material->alpha_tested != alpha_pass || material->texture == NULL || (model->plant_buffer != NULL) != plants)
		{
			continue;
		}
		if (objects->prepared_pass == PASS_CAMERA && !plants && camera_draw_kind(objects, model, material) != kind)
		{
			continue;
		}
		if (plants)
		{
			plant_month(objects, material, &plant_autumn, &plant_snow);
		}
		if (bound_model != objects->draw_models[d])
		{
			SDL_GPUBufferBinding vertex_bindings[4] =
			{
				{ model->vertex_buffer, 0 },
				{ objects->instance_buffers[objects->prepared_pass], 0 },
				{ model->tangent_buffer, 0 },
				{ buildings ? model->weathering_buffer : model->plant_buffer, 0 },
			};
			SDL_GPUBufferBinding index_binding = { model->index_buffer, 0 };

			SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, plants || buildings ? 4 : (objects->prepared_pass == PASS_CAMERA ? 3 : 2));
			SDL_BindGPUIndexBuffer(pass, &index_binding, model->index_size);
			bound_model = objects->draw_models[d];
		}
		if (plants && objects->prepared_pass == PASS_CAMERA)
		{
			// plant.frag's block: the material through the seasons
			float plant_uniforms[20];

			SDL_memcpy(plant_uniforms, objects->highlight_uniform, sizeof(float) * 4);
			SDL_memcpy(&plant_uniforms[4], material->autumn_colors[0], sizeof(float) * 3);
			plant_uniforms[7] = material->autumn;
			SDL_memcpy(&plant_uniforms[8], material->autumn_colors[1], sizeof(float) * 3);
			plant_uniforms[11] = material->falls;
			plant_uniforms[12] = material->moss;
			plant_uniforms[13] = material->tips * season_new_growth(objects->month, objects->tips_first_month, objects->tips_last_month);
			plant_uniforms[14] = material->alpha_tested ? 1.0f : 0.0f;
			plant_uniforms[15] = material->lighting_normals ? 1.0f : 0.0f;
			plant_uniforms[16] = material->normal_texture != NULL ? 1.0f : 0.0f;
			plant_uniforms[17] = plant_snow;
			plant_uniforms[18] = material->snow_cover ? 1.0f : 0.0f;
			plant_uniforms[19] = 0.0f;
			SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, plant_uniforms, sizeof(plant_uniforms));
			// (the next object's draw pushes its own again)
			material_uniforms.pbr = -1;
		}
		else if (props)
		{
			// prop.frag's block: every draw its own
			prop_material_uniforms(objects, model, material, &material_uniforms);
			SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material_uniforms, sizeof(material_uniforms));
		}
		else if (buildings)
		{
			// building.frag's block: every draw its own
			building_material_uniforms(objects, model, material, &material_uniforms);
			SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material_uniforms, sizeof(material_uniforms));
		}
		else
		{
			int32_t pbr = objects->pbr_shading && material->shaded && objects->prepared_pass == PASS_CAMERA ? 1 : 0;
			int32_t lighting_normals = material->lighting_normals ? 1 : 0;
			float season[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

			if (material->deciduous)
			{
				deciduous_season(objects->month, season);
			}
			// (a building's cut out details: their pixels marked for the
			// temporal anti-aliasing, as the building's own are)
			season[3] = building_detail(kind, model) && objects->prepared_pass == PASS_CAMERA ? 1.0f : 0.0f;
			if (pbr != material_uniforms.pbr || lighting_normals != material_uniforms.lighting_normals
				|| SDL_memcmp(season, material_uniforms.weathering, sizeof(season)) != 0)
			{
				material_uniforms.pbr = pbr;
				material_uniforms.lighting_normals = lighting_normals;
				SDL_memcpy(material_uniforms.weathering, season, sizeof(season));
				SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material_uniforms, sizeof(material_uniforms));
			}
		}
		// The season (plant.glsl): the plants' vertex shaders, the shadows' too
		float season[4] =
		{
			plant_autumn, PLANT_AUTUMN_SPREAD, material->alpha_tested ? material->autumn : 0.0f,
			material->alpha_tested ? material->falls : 0.0f,
		};

		// The snow (plant.glsl): the branches sagging; z: how many of the
		// pictures show others this month
		float snow[4] =
		{
			plant_snow, material->sag, 0.0f, model->horizontal_radius,
		};
		float month_cells[PLANT_MONTH_CELLS * 4];
		float month_offsets[PLANT_MONTH_CELLS * 4];

		if (plants && material->alpha_tested)
		{
			snow[2] = (float)plant_month_cells(objects, model, season_month_index(objects->month), material->falls <= 0.0f, month_cells,
				month_offsets);
		}
		if (plants && objects->prepared_pass != PASS_CAMERA)
		{
			float shadow_uniforms[12 + PLANT_MONTH_CELLS * 8];

			SDL_zeroa(shadow_uniforms);
			SDL_memcpy(shadow_uniforms, season, sizeof(season));
			SDL_memcpy(&shadow_uniforms[4], snow, sizeof(snow));
			shadow_uniforms[8] = model->bounds_maximum.y - model->bounds_minimum.y;
			shadow_uniforms[9] = material->alpha_tested ? 1.0f : 0.0f;
			if (snow[2] != 0.0f)
			{
				SDL_memcpy(&shadow_uniforms[12], month_cells, sizeof(float) * 4 * (size_t)SDL_fabsf(snow[2]));
				SDL_memcpy(&shadow_uniforms[12 + PLANT_MONTH_CELLS * 4], month_offsets, sizeof(float) * 4 * (size_t)SDL_fabsf(snow[2]));
			}
			SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, shadow_uniforms, sizeof(shadow_uniforms));
		}
		if (objects->prepared_pass == PASS_CAMERA && !props)
		{
			// The wind (object.vert): this model's plant, for this material
			float wind_uniforms[16];
			int m = objects->draw_models[d];

			SDL_memcpy(wind_uniforms, objects->wind_uniform, sizeof(float) * 8);
			wind_uniforms[8] = m < objects->model_wind_count ? objects->model_wind[3 * m] : 0.0f;
			wind_uniforms[9] = m < objects->model_wind_count ? objects->model_wind[3 * m + 1] : 0.0f;
			wind_uniforms[10] = model->bounds_minimum.y;
			wind_uniforms[11] = model->bounds_maximum.y - model->bounds_minimum.y;
			wind_uniforms[12] = material->alpha_tested ? 1.0f : 0.0f;
			wind_uniforms[13] = objects->lod_enabled ? objects->draw_drop_levels[d] : 0.0f;
			wind_uniforms[14] = m < objects->model_wind_count ? objects->model_wind[3 * m + 2] : 0.0f;
			// (a building's cut out details shift with its walls for the
			// temporal anti-aliasing: else the walls move under them and they
			// flicker where they lie close)
			wind_uniforms[15] = building_detail(kind, model) ? 1.0f : 0.0f;
			// A conifer laden with snow (its foliage never falls: its
			// snowy pictures, plant_month_cells) leans in the wind about half
			// as far and hardly shakes: the snow is heavy and holds the
			// needles together. The same for its bark and its foliage, or
			// they would part
			if (plants && objects->snow_amount > 0.0f)
			{
				bool evergreen = false;

				for (int k = 0; k < model->material_count; k++)
				{
					evergreen |= model->materials[k].alpha_tested && model->materials[k].falls <= 0.0f;
				}
				if (evergreen)
				{
					wind_uniforms[8] *= 1.0f - 0.5f * objects->snow_amount;
					wind_uniforms[9] *= 1.0f - 0.85f * objects->snow_amount;
				}
			}
			if (plants)
			{
				float plant_wind[24 + PLANT_MONTH_CELLS * 8];

				SDL_zeroa(plant_wind);
				SDL_memcpy(plant_wind, wind_uniforms, sizeof(wind_uniforms));
				SDL_memcpy(&plant_wind[16], season, sizeof(season));
				SDL_memcpy(&plant_wind[20], snow, sizeof(snow));
				if (snow[2] != 0.0f)
				{
					SDL_memcpy(&plant_wind[24], month_cells, sizeof(float) * 4 * (size_t)SDL_fabsf(snow[2]));
					SDL_memcpy(&plant_wind[24 + PLANT_MONTH_CELLS * 4], month_offsets, sizeof(float) * 4 * (size_t)SDL_fabsf(snow[2]));
				}
				SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, plant_wind, sizeof(plant_wind));
			}
			else
			{
				SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, wind_uniforms, sizeof(wind_uniforms));
			}
		}
		// (a building's, under the TAA: sharper, its averaging softens them)
		SDL_GPUSampler *sampler = objects->prepared_pass == PASS_CAMERA && (buildings || building_detail(kind, model))
			? gpu_biased_sampler(objects->texture_bias) : gpu.linear_repeat;

		texture_binding.texture = material->texture;
		texture_binding.sampler = sampler;
		SDL_BindGPUFragmentSamplers(pass, objects->prepared_pass == PASS_CAMERA ? 3 : 0, &texture_binding, 1);
		if (objects->prepared_pass == PASS_CAMERA)
		{
			SDL_GPUTextureSamplerBinding maps[3] =
			{
				{ material->normal_texture != NULL ? material->normal_texture : object_default_texture(OBJECT_DEFAULT_NORMAL), sampler },
				{ material->orm_texture != NULL ? material->orm_texture : object_default_texture(OBJECT_DEFAULT_ORM), sampler },
				{ material->emissive_texture != NULL ? material->emissive_texture : object_default_texture(OBJECT_DEFAULT_EMISSIVE),
					sampler },
			};

			SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
		}
		SDL_DrawGPUIndexedPrimitivesIndirect(pass,
			objects->argument_buffers[objects->prepared_pass],
			(uint32_t)(sizeof(uint32_t) * DRAW_ARGUMENT_WORDS * (size_t)d), 1);
		draw_calls++;
	}
	return draw_calls;
}

// The impostors the camera's culling picked: a square each (see impostors.h)
static int draw_impostors(const objects_t *objects, SDL_GPURenderPass *pass)
{
	int draw_calls = 0;
	SDL_GPUBufferBinding instance_binding = { objects->instance_buffers[PASS_CAMERA], 0 };
	SDL_GPUBufferBinding index_binding = { objects->impostor_index_buffer, 0 };

	if (objects->impostor_pipeline == NULL || objects->impostor_index_buffer == NULL)
	{
		return 0;
	}
	SDL_BindGPUGraphicsPipeline(pass, objects->impostor_pipeline);
	lights_bind(pass);
	{
		// A tint to tell them from the models (rgb, how much)
		float tint[4] = { 1.0f, 0.2f, 0.9f, objects->impostors_tinted ? 0.6f : 0.0f };

		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, tint, sizeof(tint));
	}
	SDL_BindGPUVertexBuffers(pass, 0, &instance_binding, 1);
	SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
	for (int d = 0; d < objects->draw_count; d++)
	{
		const impostor_t *impostor;

		if (objects->draw_materials[d] >= 0 || !model_on_map(objects, objects->draw_models[d]))
		{
			continue;
		}
		impostor = &objects->impostors[objects->draw_models[d]];
		{
			SDL_GPUTextureSamplerBinding albedo = { impostor->albedo, gpu.linear_clamp };
			SDL_GPUTextureSamplerBinding normal = { impostor->normal, gpu.linear_clamp };

			SDL_BindGPUFragmentSamplers(pass, 3, &albedo, 1);
			SDL_BindGPUFragmentSamplers(pass, 5, &normal, 1);
		}
		{
			float uniforms[8];

			SDL_memcpy(uniforms, impostor->sphere, sizeof(impostor->sphere));
			SDL_memcpy(uniforms + 4, impostor->look, sizeof(impostor->look));
			SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, uniforms, sizeof(uniforms));
		}
		SDL_DrawGPUIndexedPrimitivesIndirect(pass, objects->argument_buffers[PASS_CAMERA],
			(uint32_t)(sizeof(uint32_t) * DRAW_ARGUMENT_WORDS * (size_t)d), 1);
		draw_calls++;
	}
	return draw_calls;
}

void objects_prepare(objects_t *objects, const camera_t *camera)
{
	frustum_t frustum = frustum_from_matrix(camera->view_projection);

	objects->drawn_instance_count = 0;
	if (objects->instance_count == 0 || objects->hidden)
	{
		objects->prepared_pass = -1;
		return;
	}
	cull_pass(objects, PASS_CAMERA, &frustum, camera->eye_position);
}

// The moss under water's shells (prop_shell.vert): over every weathered
// prop drawn at its finest level of detail (the near ones; the shader puts
// those past the shells' distance out of view), a draw a layer
static int draw_moss_shells(const objects_t *objects, SDL_GPURenderPass *pass, const render_settings_t *settings, float time_seconds)
{
	int draw_calls = 0;
	int layers = SDL_min(settings->moss_shells, 32);
	material_uniforms_t material_uniforms;

	SDL_zero(material_uniforms);
	SDL_memcpy(material_uniforms.highlight_parameters, objects->highlight_uniform, sizeof(material_uniforms.highlight_parameters));
	for (int d = 0; d < objects->draw_count; d++)
	{
		const object_model_t *model;
		const object_material_t *material;

		if (objects->draw_materials[d] < 0 || objects->draw_levels[d] != 0 || !model_on_map(objects, objects->draw_models[d]))
		{
			continue;
		}
		model = &objects->models[objects->draw_models[d]];
		material = &model->materials[objects->draw_materials[d]];
		if (material->alpha_tested || material->texture == NULL || model->plant_buffer != NULL || !material->weathered
			|| !prop_material(objects, model, material))
		{
			continue;
		}
		{
			SDL_GPUBufferBinding vertex_bindings[3] =
			{
				{ model->vertex_buffer, 0 },
				{ objects->instance_buffers[PASS_CAMERA], 0 },
				{ model->tangent_buffer, 0 },
			};
			SDL_GPUBufferBinding index_binding = { model->index_buffer, 0 };
			SDL_GPUTextureSamplerBinding texture_binding = { material->texture, gpu.linear_repeat };
			SDL_GPUTextureSamplerBinding maps[3] =
			{
				{ material->normal_texture != NULL ? material->normal_texture : object_default_texture(OBJECT_DEFAULT_NORMAL), gpu.linear_repeat },
				{ material->orm_texture != NULL ? material->orm_texture : object_default_texture(OBJECT_DEFAULT_ORM), gpu.linear_repeat },
				{ material->emissive_texture != NULL ? material->emissive_texture : object_default_texture(OBJECT_DEFAULT_EMISSIVE),
					gpu.linear_repeat },
			};

			SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, 3);
			SDL_BindGPUIndexBuffer(pass, &index_binding, model->index_size);
			SDL_BindGPUFragmentSamplers(pass, 3, &texture_binding, 1);
			SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
		}
		prop_material_uniforms(objects, model, material, &material_uniforms);
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material_uniforms, sizeof(material_uniforms));
		for (int layer = 1; layer <= layers; layer++)
		{
			float shell[4] =
			{
				(float)layer / (float)layers, SDL_max(settings->moss_shell_meters, 0.001f), settings->moss_shell_distance_meters,
				SDL_fmodf(time_seconds, 3600.0f),
			};

			SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, shell, sizeof(shell));
			SDL_DrawGPUIndexedPrimitivesIndirect(pass, objects->argument_buffers[PASS_CAMERA],
				(uint32_t)(sizeof(uint32_t) * DRAW_ARGUMENT_WORDS * (size_t)d), 1);
			draw_calls++;
		}
	}
	return draw_calls;
}

void objects_draw(objects_t *objects, SDL_GPURenderPass *pass, const render_settings_t *settings, const shadow_map_t *shadow,
	const shadow_map_t *near_shadow, SDL_GPUTexture *variation_texture, float time_seconds)
{
	SDL_GPUTextureSamplerBinding variation_binding = { variation_texture, gpu.linear_repeat };
	SDL_GPUTexture *cloud_shadows = clouds_shadow_texture();

	// The buildings' look, as the settings have it now (live)
	objects->look[0] = settings->roof_lightness;
	objects->look[1] = settings->roof_fade;
	objects->look[2] = settings->roof_warmth;
	objects->look[3] = settings->roof_patches;
	objects->look[4] = settings->roof_runs;
	objects->look[5] = settings->roof_mended;
	objects->look[6] = settings->wood_dulled;
	objects->look[7] = 0.0f;
	SDL_GPUTextureSamplerBinding cloud_shadow_binding = { cloud_shadows != NULL ? cloud_shadows : variation_texture, gpu.linear_repeat };

	objects->texture_bias = settings->taa ? SDL_clamp(settings->taa_mip_bias, -3.0f, 0.0f) : 0.0f;
	objects->pbr_shading = settings->pbr_shading;
	objects->draw_call_count = 0;
	refresh_models_used(objects);
	if (objects->prepared_pass != PASS_CAMERA || objects->drawn_instance_count == 0)
	{
		return;
	}
	objects->highlight_uniform[0] = objects->highlight_color[0];
	objects->highlight_uniform[1] = objects->highlight_color[1];
	objects->highlight_uniform[2] = objects->highlight_color[2];
	objects->highlight_uniform[3] = time_seconds;
	{
		float strength = SDL_clamp(settings->plant_wind_strength, 0.0f, 1.0f);
		float direction = settings->plant_wind_direction_degrees * SDL_PI_F / 180.0f;
		float spacing = SDL_max(settings->plant_wind_gust_meters, 1.0f);

		objects->wind_uniform[0] = SDL_cosf(direction);
		objects->wind_uniform[1] = SDL_sinf(direction);
		objects->wind_uniform[2] = strength;
		// The clock kept small (the shaders' floats)
		objects->wind_uniform[3] = SDL_fmodf(time_seconds, 3600.0f);
		objects->wind_uniform[4] = 2.0f * SDL_PI_F / spacing;
		// The gusts roll faster in a stronger wind
		objects->wind_uniform[5] = 2.0f + 10.0f * strength;
		objects->wind_uniform[6] = 0.0f;
		objects->wind_uniform[7] = 0.0f;
	}
	shadow_map_bind_for_shading(pass, shadow, near_shadow);
	SDL_BindGPUFragmentSamplers(pass, 2, &variation_binding, 1);
	SDL_BindGPUFragmentSamplers(pass, 4, &cloud_shadow_binding, 1);
	scene_ground_bind(pass, 8);
	// (the plants' and impostors' vertex shaders: sky_vertex.glsl)
	scene_ground_bind_sky_vertex4(pass, 0, shadow != NULL ? shadow->depth_texture : NULL);
	SDL_BindGPUGraphicsPipeline(pass, objects->pipeline);
	lights_bind(pass);
	objects->draw_call_count += draw_models(objects, pass, false, DRAW_OTHERS);
	if (objects->prop_pipeline != NULL)
	{
		SDL_BindGPUGraphicsPipeline(pass, objects->prop_pipeline);
		lights_bind(pass);
		objects->draw_call_count += draw_models(objects, pass, false, DRAW_PROPS);
	}
	if (objects->building_pipeline != NULL)
	{
		SDL_BindGPUGraphicsPipeline(pass, objects->building_pipeline);
		lights_bind(pass);
		objects->draw_call_count += draw_models(objects, pass, false, DRAW_BUILDINGS);
	}
	if (objects->prop_shell_pipeline != NULL && settings->moss_shells > 0 && settings->moss_shell_distance_meters > 0.0f)
	{
		SDL_BindGPUGraphicsPipeline(pass, objects->prop_shell_pipeline);
		lights_bind(pass);
		objects->draw_call_count += draw_moss_shells(objects, pass, settings, time_seconds);
	}
	if (objects->plant_pipeline != NULL)
	{
		SDL_BindGPUGraphicsPipeline(pass, objects->plant_pipeline);
		lights_bind(pass);
		objects->draw_call_count += draw_models(objects, pass, false, DRAW_PLANTS);
	}
	SDL_BindGPUGraphicsPipeline(pass, objects->alpha_pipeline);
	lights_bind(pass);
	objects->draw_call_count += draw_models(objects, pass, true, DRAW_OTHERS);
	if (objects->alpha_culled_pipeline != NULL)
	{
		SDL_BindGPUGraphicsPipeline(pass, objects->alpha_culled_pipeline);
		lights_bind(pass);
		objects->draw_call_count += draw_models(objects, pass, true, DRAW_CUT_OUT_ONE_SIDED);
	}
	if (objects->plant_alpha_pipeline != NULL)
	{
		SDL_BindGPUGraphicsPipeline(pass, objects->plant_alpha_pipeline);
		lights_bind(pass);
		objects->draw_call_count += draw_models(objects, pass, true, DRAW_PLANTS);
	}
	objects->draw_call_count += draw_impostors(objects, pass);
}

void objects_prepare_shadow(objects_t *objects, const shadow_map_t *shadow, const camera_t *camera)
{
	frustum_t frustum = frustum_from_matrix(shadow->light_view_projection);
	int drawn = objects->drawn_instance_count;

	if (objects->instance_count == 0 || objects->hidden)
	{
		objects->prepared_pass = -1;
		return;
	}
	if (camera != NULL)
	{
		cull_pass(objects, PASS_NEAR_SHADOW, &frustum, camera->eye_position);
	}
	else
	{
		cull_pass(objects, PASS_FAR_SHADOW, NULL, vec3_make(0.0f, 0.0f, 0.0f));
	}
	// The statistics are about what the camera sees
	objects->drawn_instance_count = drawn;
}

void objects_prepare_shadow_layer(objects_t *objects, const shadow_map_t *shadow, const camera_t *camera, objects_shadow_layer_t layer)
{
	objects->shadow_free_filter = layer;
	objects_prepare_shadow(objects, shadow, camera);
	objects->shadow_free_filter = OBJECTS_SHADOW_ALL;
}

void objects_prepare_sky_occlusion(objects_t *objects, const shadow_map_t *shadow, float least_height)
{
	objects->far_shadow_least_height = SDL_max(least_height, 0.0f);
	objects_prepare_shadow(objects, shadow, NULL);
	objects->far_shadow_least_height = 0.0f;
}

void objects_render_shadow(objects_t *objects, SDL_GPURenderPass *pass)
{
	if (objects->prepared_pass != PASS_FAR_SHADOW && objects->prepared_pass != PASS_NEAR_SHADOW)
	{
		return;
	}
	int draw_calls = 0;

	refresh_models_used(objects);
	SDL_BindGPUGraphicsPipeline(pass, objects->shadow_pipeline);
	draw_calls += draw_models(objects, pass, false, DRAW_OTHERS);
	draw_calls += draw_models(objects, pass, true, DRAW_OTHERS);
	if (objects->plant_shadow_pipeline != NULL)
	{
		SDL_BindGPUGraphicsPipeline(pass, objects->plant_shadow_pipeline);
		draw_calls += draw_models(objects, pass, false, DRAW_PLANTS);
		draw_calls += draw_models(objects, pass, true, DRAW_PLANTS);
	}
	frame_profile_count(FRAME_SHADOW_DRAW_CALLS, draw_calls);
	objects->prepared_pass = -1;
}

int objects_instance_model(const objects_t *objects, const object_instance_t *instance)
{
	const object_type_t *type = &objects->types.types[instance->type];

	if ((instance->state & (OBJECT_STATE_HIDDEN | OBJECT_STATE_DEAD)) != 0)
	{
		return -1;
	}
	if (instance->damage == OBJECT_DAMAGE_DESTROYED)
	{
		return type->destroyed_model;
	}
	if (instance->damage == OBJECT_DAMAGE_DAMAGED && type->damaged_model >= 0)
	{
		return type->damaged_model;
	}
	return type->model;
}

void objects_update_rotation(object_instance_t *instance)
{
	// Turned by its yaw around Y; laid down by tipping it around the
	// horizontal axis across flatten_yaw, so its top goes that way
	float half_yaw = 0.5f * instance->yaw_radians;
	float yaw[4] = { 0.0f, SDL_sinf(half_yaw), 0.0f, SDL_cosf(half_yaw) };

	if (instance->flatten <= 0.0f)
	{
		SDL_memcpy(instance->rotation, yaw, sizeof(yaw));
		return;
	}
	{
		// Tipping towards direction (sin d, 0, cos d): around the axis
		// (cos d, 0, -sin d) by the tip angle (up to about 80 degrees)
		float angle = SDL_min(instance->flatten, 1.0f) * 1.4f;
		float axis_x = SDL_cosf(instance->flatten_yaw);
		float axis_z = -SDL_sinf(instance->flatten_yaw);
		float half_sine = SDL_sinf(0.5f * angle);
		float tip[4] = { axis_x * half_sine, 0.0f, axis_z * half_sine, SDL_cosf(0.5f * angle) };

		// rotation = tip * yaw (the yaw first, then the tip)
		instance->rotation[0] = tip[3] * yaw[0] + tip[0] * yaw[3] + tip[1] * yaw[2] - tip[2] * yaw[1];
		instance->rotation[1] = tip[3] * yaw[1] - tip[0] * yaw[2] + tip[1] * yaw[3] + tip[2] * yaw[0];
		instance->rotation[2] = tip[3] * yaw[2] + tip[0] * yaw[1] - tip[1] * yaw[0] + tip[2] * yaw[3];
		instance->rotation[3] = tip[3] * yaw[3] - tip[0] * yaw[0] - tip[1] * yaw[1] - tip[2] * yaw[2];
	}
}

void objects_set_free(objects_t *objects, int instance_index, bool free)
{
	object_instance_t *instance = &objects->instances[instance_index];
	bool is_free = (instance->state & OBJECT_STATE_FREE) != 0;

	if (is_free == free)
	{
		return;
	}
	if (free)
	{
		instance->y = objects_instance_base_y(instance);
		instance->state |= OBJECT_STATE_FREE;
		// (left where it is in the list, a stray: not the whole map sorted
		// again for a barrel the water took)
		add_stray(objects, instance_index);
		note_change(objects, instance_index, OBJECT_CHANGE_FREED);
	}
	else
	{
		instance->state &= (uint8_t)~OBJECT_STATE_FREE;
		objects->grid_dirty = true;
	}
	objects_touch(objects, instance_index);
	mark_shadows_dirty(objects, instance->type);
}

void objects_refresh_ground(objects_t *objects, const terrain_t *terrain, int instance_index)
{
	object_instance_t *instance = &objects->instances[instance_index];

	instance->ground_height = ground_height_under(terrain, instance->x, instance->z, footprint_of(objects, instance)) - sink_of(objects, instance);
	objects_touch(objects, instance_index);
}

void objects_set_highlight(objects_t *objects, int instance, const float color[3])
{
	objects->highlight_valid = instance >= 0 && instance < objects->instance_count;
	objects->highlight_instance = instance;
	objects->highlight_generation = objects->generation;
	if (color != NULL)
	{
		SDL_memcpy(objects->highlight_color, color, sizeof(objects->highlight_color));
	}
}

// v turned by the quaternion q (x, y, z, w), or back with inverse
static vec3_t turn_vector(const float q[4], vec3_t v, bool inverse)
{
	float x = inverse ? -q[0] : q[0];
	float y = inverse ? -q[1] : q[1];
	float z = inverse ? -q[2] : q[2];
	vec3_t cross_1 = vec3_make(2.0f * (y * v.z - z * v.y), 2.0f * (z * v.x - x * v.z), 2.0f * (x * v.y - y * v.x));
	vec3_t cross_2 = vec3_make(y * cross_1.z - z * cross_1.y, z * cross_1.x - x * cross_1.z, x * cross_1.y - y * cross_1.x);

	return vec3_add(v, vec3_add(vec3_scale(cross_1, q[3]), cross_2));
}

// Where a ray enters a box (slabs), -1 if it misses it before far
static float ray_box(vec3_t origin, vec3_t direction, vec3_t minimum, vec3_t maximum, float far)
{
	float near = 0.0f;
	const float o[3] = { origin.x, origin.y, origin.z };
	const float d[3] = { direction.x, direction.y, direction.z };
	const float low[3] = { minimum.x, minimum.y, minimum.z };
	const float high[3] = { maximum.x, maximum.y, maximum.z };

	for (int axis = 0; axis < 3; axis++)
	{
		if (SDL_fabsf(d[axis]) < 1e-9f)
		{
			if (o[axis] < low[axis] || o[axis] > high[axis])
			{
				return -1.0f;
			}
			continue;
		}
		{
			float t1 = (low[axis] - o[axis]) / d[axis];
			float t2 = (high[axis] - o[axis]) / d[axis];

			near = SDL_max(near, SDL_min(t1, t2));
			far = SDL_min(far, SDL_max(t1, t2));
		}
		if (near > far)
		{
			return -1.0f;
		}
	}
	return near;
}

// The nearest triangle of the model the ray (in model space) goes
// through, before far; -1 if none (both sides count)
static float ray_triangles(const object_model_t *model, vec3_t origin, vec3_t direction, float far)
{
	float nearest = -1.0f;

	for (int i = 0; i + 2 < model->pick_index_count; i += 3)
	{
		const float *a = &model->collision_positions[(size_t)model->collision_indices[i] * 3];
		const float *b = &model->collision_positions[(size_t)model->collision_indices[i + 1] * 3];
		const float *c = &model->collision_positions[(size_t)model->collision_indices[i + 2] * 3];
		vec3_t edge_1 = vec3_make(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
		vec3_t edge_2 = vec3_make(c[0] - a[0], c[1] - a[1], c[2] - a[2]);
		vec3_t p = vec3_cross(direction, edge_2);
		float determinant = vec3_dot(edge_1, p);
		vec3_t to_origin;
		vec3_t q;
		float inverse;
		float u;
		float v;
		float t;

		if (SDL_fabsf(determinant) < 1e-12f)
		{
			continue;
		}
		inverse = 1.0f / determinant;
		to_origin = vec3_make(origin.x - a[0], origin.y - a[1], origin.z - a[2]);
		u = vec3_dot(to_origin, p) * inverse;
		if (u < 0.0f || u > 1.0f)
		{
			continue;
		}
		q = vec3_cross(to_origin, edge_1);
		v = vec3_dot(direction, q) * inverse;
		if (v < 0.0f || u + v > 1.0f)
		{
			continue;
		}
		t = vec3_dot(edge_2, q) * inverse;
		if (t > 0.0f && t < far && (nearest < 0.0f || t < nearest))
		{
			nearest = t;
		}
	}
	return nearest;
}

// One object against the ray; false if it misses. distance in, the far
// limit; out, where it hits
static bool ray_object(const objects_t *objects, const object_instance_t *instance, vec3_t origin, vec3_t direction, float *distance)
{
	const object_type_t *type = &objects->types.types[instance->type];
	const object_model_t *model;
	vec3_t placement;
	vec3_t local_origin;
	vec3_t local_direction;
	vec3_t middle;
	vec3_t to_middle;
	float radius;
	float scale;
	float along;
	float closest;
	float hit;

	if (type->model < 0 || (instance->state & (OBJECT_STATE_HIDDEN | OBJECT_STATE_DEAD)) != 0)
	{
		return false;
	}
	model = &objects->models[type->model];
	scale = SDL_max(instance->scale, 1e-4f);
	placement = vec3_make(instance->x, objects_instance_base_y(instance), instance->z);
	// A ball around it first: most objects are nowhere near the ray
	middle = vec3_add(placement, turn_vector(instance->rotation,
		vec3_scale(vec3_add(model->bounds_minimum, model->bounds_maximum), 0.5f * scale), false));
	radius = 0.5f * scale * vec3_length(vec3_subtract(model->bounds_maximum, model->bounds_minimum));
	to_middle = vec3_subtract(middle, origin);
	along = vec3_dot(to_middle, direction);
	closest = vec3_dot(to_middle, to_middle) - along * along;
	if (along < -radius || along - radius > *distance || closest > radius * radius)
	{
		return false;
	}
	// In the model's own space the ray keeps its length unit (meters of
	// the world, divided by the scale)
	local_origin = vec3_scale(turn_vector(instance->rotation, vec3_subtract(origin, placement), true), 1.0f / scale);
	local_direction = vec3_scale(turn_vector(instance->rotation, direction, true), 1.0f / scale);
	hit = ray_box(local_origin, local_direction, model->bounds_minimum, model->bounds_maximum, *distance);
	if (hit < 0.0f)
	{
		return false;
	}
	if (model->pick_index_count >= 3)
	{
		hit = ray_triangles(model, local_origin, local_direction, *distance);
		if (hit < 0.0f)
		{
			return false;
		}
	}
	*distance = hit;
	return true;
}

// Into the list, nearest first; returns the new count
static int insert_hit(object_ray_hit_t *hits, int count, int maximum_hits, int instance, float distance, vec3_t origin, vec3_t direction)
{
	int place = count < maximum_hits ? count : maximum_hits - 1;

	if (count == maximum_hits && distance >= hits[maximum_hits - 1].distance)
	{
		return count;
	}
	while (place > 0 && hits[place - 1].distance > distance)
	{
		hits[place] = hits[place - 1];
		place--;
	}
	hits[place].instance = instance;
	hits[place].distance = distance;
	hits[place].point = vec3_add(origin, vec3_scale(direction, distance));
	return SDL_min(count + 1, maximum_hits);
}

static int ray_instances(objects_t *objects, int first, int last, vec3_t origin, vec3_t direction, float maximum_distance,
	objects_ray_mode_t mode, object_ray_hit_t *hits, int count, int maximum_hits)
{
	for (int i = first; i < last; i++)
	{
		const object_instance_t *instance = &objects->instances[i];
		float distance = maximum_distance;

		if (mode == OBJECTS_RAY_INTERACTIVE && !(instance->state & OBJECT_STATE_INTERACTIVE))
		{
			continue;
		}
		// (a cell's own: one come loose since the sort is elsewhere, looked
		// at with the strays)
		if (!objects->grid_dirty && objects->grid_first != NULL && (instance->state & OBJECT_STATE_FREE) != 0 && i < objects->sorted_count
			&& i < objects->grid_first[objects->grid_size * objects->grid_size])
		{
			continue;
		}
		if (ray_object(objects, instance, origin, direction, &distance))
		{
			count = insert_hit(hits, count, maximum_hits, i, distance, origin, direction);
		}
	}
	return count;
}

int objects_raycast(objects_t *objects, vec3_t origin, vec3_t direction, float maximum_distance, objects_ray_mode_t mode,
	object_ray_hit_t *hits, int maximum_hits)
{
	int count = 0;
	int cells = objects->grid_size * objects->grid_size;
	float horizontal;
	int steps;

	if (maximum_hits <= 0 || objects->instance_count == 0)
	{
		return 0;
	}
	if (objects->grid_dirty || objects->grid_first == NULL || objects->grid_size <= 0)
	{
		// Not sorted into the grid (just changed): every object
		return ray_instances(objects, 0, objects->instance_count, origin, direction, maximum_distance, mode, hits, 0, maximum_hits);
	}
	if (objects->ray_cell_stamps == NULL)
	{
		objects->ray_cell_stamps = SDL_calloc((size_t)cells, sizeof(uint32_t));
		if (objects->ray_cell_stamps == NULL)
		{
			return ray_instances(objects, 0, objects->instance_count, origin, direction, maximum_distance, mode, hits, 0, maximum_hits);
		}
	}
	objects->ray_stamp++;
	if (objects->ray_stamp == 0)
	{
		SDL_memset(objects->ray_cell_stamps, 0, sizeof(uint32_t) * (size_t)cells);
		objects->ray_stamp = 1;
	}
	// Along the ray, seen from above, every half cell: the cell there and
	// the ones around it (an object belongs to the cell of its foot, but a
	// house or a tree reaches into the next one)
	horizontal = maximum_distance * SDL_sqrtf(direction.x * direction.x + direction.z * direction.z);
	steps = (int)SDL_ceilf(horizontal / (0.5f * OBJECTS_GRID_CELL_METERS)) + 1;
	for (int step = 0; step <= steps; step++)
	{
		float along = maximum_distance * (float)step / (float)steps;
		int cell_x = (int)SDL_floorf((origin.x + direction.x * along) / OBJECTS_GRID_CELL_METERS);
		int cell_z = (int)SDL_floorf((origin.z + direction.z * along) / OBJECTS_GRID_CELL_METERS);

		for (int j = cell_z - 1; j <= cell_z + 1; j++)
		{
			for (int i = cell_x - 1; i <= cell_x + 1; i++)
			{
				int cell;

				if (i < 0 || j < 0 || i >= objects->grid_size || j >= objects->grid_size)
				{
					continue;
				}
				cell = j * objects->grid_size + i;
				if (objects->ray_cell_stamps[cell] == objects->ray_stamp)
				{
					continue;
				}
				objects->ray_cell_stamps[cell] = objects->ray_stamp;
				count = ray_instances(objects, objects->grid_first[cell], objects->grid_first[cell + 1], origin, direction, maximum_distance,
					mode, hits, count, maximum_hits);
			}
		}
	}
	// The loose ones, kept after the last cell, and those added since the sort
	count = ray_instances(objects, objects->grid_first[cells], objects->instance_count, origin, direction, maximum_distance, mode, hits,
		count, maximum_hits);
	// The strays come loose since the sort, wherever they are now
	for (int n = 0; n < objects->stray_count; n++)
	{
		int k = objects->strays[n];
		float distance = maximum_distance;

		if (k >= objects->instance_count || (objects->instances[k].state & OBJECT_STATE_FREE) == 0)
		{
			continue;
		}
		if (mode == OBJECTS_RAY_INTERACTIVE && !(objects->instances[k].state & OBJECT_STATE_INTERACTIVE))
		{
			continue;
		}
		if (ray_object(objects, &objects->instances[k], origin, direction, &distance))
		{
			count = insert_hit(hits, count, maximum_hits, k, distance, origin, direction);
		}
	}
	return count;
}

// A plant's season (object_type_t.months_first): the share of them there,
// coming over the first half month, going over the last half
static float season_share(float month, float first, float last)
{
	float length = (float)(((int)last - (int)first + 12) % 12 + 1);
	float progress = season_span_progress(month, first, last);
	float into;

	if (progress < 0.0f)
	{
		return 0.0f;
	}
	into = progress * length;
	return SDL_min(SDL_min(into / 0.5f, (length - into) / 0.5f), 1.0f);
}

void objects_update_spin(objects_t *objects, double seconds)
{
	bool any = false;

	objects->spin_seconds = seconds;
	for (int t = 0; t < objects->types.type_count && !any; t++)
	{
		any = objects->types.types[t].spins;
	}
	objects->any_spins = any;
	if (!any)
	{
		return;
	}
	if (!objects->spin_known || objects->spin_generation != objects->generation)
	{
		objects->spin_count = 0;
		for (int k = 0; k < objects->instance_count; k++)
		{
			if (!objects->types.types[objects->instances[k].type].spins)
			{
				continue;
			}
			if (objects->spin_count >= objects->spin_capacity)
			{
				int capacity = SDL_max(objects->spin_capacity * 2, 64);
				int *grown = SDL_realloc(objects->spin_instances, sizeof(int) * (size_t)capacity);

				if (grown == NULL)
				{
					break;
				}
				objects->spin_instances = grown;
				objects->spin_capacity = capacity;
			}
			objects->spin_instances[objects->spin_count++] = k;
		}
		objects->spin_generation = objects->generation;
		objects->spin_known = true;
	}
	for (int n = 0; n < objects->spin_count; n++)
	{
		int k = objects->spin_instances[n];

		if (k < objects->instance_count)
		{
			touch_slot(objects, objects->instances[k].gpu_slot);
		}
	}
	objects->moving_shadows = true;
}

void objects_update_seasons(objects_t *objects)
{
	int type_count = objects->types.type_count;
	bool changed = false;

	if (objects->type_table == NULL || type_count <= 0)
	{
		return;
	}
	if (objects->type_shares == NULL)
	{
		objects->type_shares = SDL_malloc(sizeof(float) * (size_t)type_count);
		if (objects->type_shares == NULL)
		{
			return;
		}
	}
	for (int t = 0; t < type_count; t++)
	{
		const object_type_t *type = &objects->types.types[t];
		float share = 1.0f;

		if (type->months_first >= 1.0f && type->months_last >= 1.0f)
		{
			share = season_share(objects->month, type->months_first, type->months_last);
		}
		// (in fiftieths: the GPU's table and the shadows follow a step at a
		// time, not every frame the snow grows)
		share = SDL_roundf(share * 50.0f) / 50.0f;
		objects->type_shares[t] = share;
		if (SDL_fabsf(objects->type_table[4 * t + 3] - share) > 0.001f)
		{
			objects->type_table[4 * t + 3] = share;
			changed = true;
		}
	}
	if (changed && objects->type_buffer != NULL)
	{
		gpu_upload_buffer(objects->type_buffer, 0, objects->type_table, sizeof(float) * 4 * (size_t)type_count);
		objects->far_shadow_dirty = true;
		objects->near_shadow_dirty = true;
	}
}

void objects_draw_occlusion_debug(objects_t *objects, SDL_GPURenderPass *pass, float near_distance, float far_distance)
{
	SDL_GPUBuffer *buffers[2];
	float uniforms[8];

	if (!objects->occlusion_shown || pass == NULL || objects->occlusion_debug_pipeline == NULL || objects->occlusion == NULL
		|| hiz_buffer(objects->occlusion) == NULL || gpu.command_buffer == NULL)
	{
		return;
	}
	buffers[0] = objects->occlusion_debug_buffer;
	buffers[1] = hiz_buffer(objects->occlusion);
	// The pyramid's level 0 size, whether it is drawn, the records' limit;
	// the camera's near and far (its depth made linear)
	uniforms[0] = (float)objects->occlusion->width;
	uniforms[1] = (float)objects->occlusion->height;
	uniforms[2] = objects->occlusion_depth_shown && objects->occlusion->ready ? 1.0f : 0.0f;
	uniforms[3] = (float)OCCLUSION_DEBUG_RECORDS;
	uniforms[4] = near_distance;
	uniforms[5] = far_distance;
	uniforms[6] = 0.0f;
	uniforms[7] = 0.0f;
	SDL_BindGPUGraphicsPipeline(pass, objects->occlusion_debug_pipeline);
	SDL_BindGPUFragmentStorageBuffers(pass, 0, buffers, 2);
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, uniforms, sizeof(uniforms));
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

// The instances within radius of (x, z), each to visit (index, user):
// by the objects' grid (only its cells round the circle, and the loose
// objects after them) when it is sorted, else all of them. A big map holds
// millions: a pass over every one was tens of milliseconds, a frame
void objects_visit_circle(const objects_t *objects, float x, float z, float radius, objects_visit_t visit, void *user)
{
	int cells = objects->grid_size * objects->grid_size;
	float radius_squared = radius * radius;

	if (objects->grid_dirty || objects->grid_first == NULL || cells <= 0)
	{
		for (int i = 0; i < objects->instance_count; i++)
		{
			float dx = objects->instances[i].x - x;
			float dz = objects->instances[i].z - z;

			if ((objects->instances[i].state & OBJECT_STATE_DEAD) != 0)
			{
				continue;
			}
			if (dx * dx + dz * dz <= radius_squared && !visit(objects, i, user))
			{
				return;
			}
		}
		return;
	}
	{
		int last = objects->grid_size - 1;
		int first_x = SDL_clamp((int)((x - radius) / OBJECTS_GRID_CELL_METERS), 0, last);
		int first_z = SDL_clamp((int)((z - radius) / OBJECTS_GRID_CELL_METERS), 0, last);
		int last_x = SDL_clamp((int)((x + radius) / OBJECTS_GRID_CELL_METERS), 0, last);
		int last_z = SDL_clamp((int)((z + radius) / OBJECTS_GRID_CELL_METERS), 0, last);

		for (int j = first_z; j <= last_z; j++)
		{
			for (int i = first_x; i <= last_x; i++)
			{
				int cell = j * objects->grid_size + i;

				for (int k = objects->grid_first[cell]; k < objects->grid_first[cell + 1]; k++)
				{
					float dx = objects->instances[k].x - x;
					float dz = objects->instances[k].z - z;

					// (gone, or come loose and elsewhere now: not here)
					if ((objects->instances[k].state & (OBJECT_STATE_FREE | OBJECT_STATE_DEAD)) != 0)
					{
						continue;
					}
					if (dx * dx + dz * dz <= radius_squared && !visit(objects, k, user))
					{
						return;
					}
				}
			}
		}
		// (the loose ones, after the cells, and those added since the sort)
		for (int k = objects->grid_first[cells]; k < objects->instance_count; k++)
		{
			float dx = objects->instances[k].x - x;
			float dz = objects->instances[k].z - z;

			if ((objects->instances[k].state & OBJECT_STATE_DEAD) != 0)
			{
				continue;
			}
			if (dx * dx + dz * dz <= radius_squared && !visit(objects, k, user))
			{
				return;
			}
		}
		// (the strays come loose since: wherever they are now)
		for (int n = 0; n < objects->stray_count; n++)
		{
			int k = objects->strays[n];
			float dx;
			float dz;

			if (k >= objects->instance_count || (objects->instances[k].state & (OBJECT_STATE_FREE | OBJECT_STATE_DEAD)) != OBJECT_STATE_FREE)
			{
				continue;
			}
			dx = objects->instances[k].x - x;
			dz = objects->instances[k].z - z;
			if (dx * dx + dz * dz <= radius_squared && !visit(objects, k, user))
			{
				return;
			}
		}
	}
}
