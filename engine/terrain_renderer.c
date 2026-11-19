// Terrain rendering. See terrain_renderer.h.

#include "lights.h"
#include "terrain_renderer.h"
#include "clouds.h"
#include "erosion.h"
#include "water_gpu.h"
#include "scene_uniforms.h"
#include "parallel.h"
#include "material_map.h"
#include "profile.h"

#include <stdlib.h>
#include <SDL3/SDL.h>

// Chunks prepared per thread when only their wetness and soils are the CPU's
#define TERRAIN_CHUNKS_PER_THREAD 16

// One chunk's data, prepared for upload (see terrain_renderer_upload)
#define NORMAL_REGION_SIZE (TERRAIN_CHUNK_SIZE + 2 * TERRAIN_NORMAL_REACH_LIMIT)

struct terrain_upload_slot_s
{
	int chunk_x;
	int chunk_y;
	bool shape;                    // heights or soil changed, not just wetness
	bool soils_changed;            // the soil counts differ from before
	bool bare_settling;            // the bare bedrock shown is not where the ground says yet: again soon
	float since_seconds;           // since the chunk last went up
	int normal_x;                  // region of the normal texture to update
	int normal_y;
	int normal_width;
	int normal_height;
	float heights[TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE];
	uint8_t surface[TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE * 4];
	uint8_t wetness[TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE];   // for the GPU's surface (water_gpu_ground_wetness)
	uint8_t normals[NORMAL_REGION_SIZE * NORMAL_REGION_SIZE * 4];
};

// Texture units used by the terrain program
// The shaders' constants (constants.glsl) must still match
_Static_assert(MATERIAL_GRASS == 0 && MATERIAL_DIRT == 1 && MATERIAL_ROCK == 2 && MATERIAL_SAND == 3 && MATERIAL_CLAY == 4
	&& MATERIAL_SNOW == 5 && MATERIAL_SILT == 6 && MATERIAL_NEEDLES == 7 && MATERIAL_LEAVES == 8 && MATERIAL_TRAMPLED == 9
	&& MATERIAL_PETALS == 10 && MATERIAL_TRAMPLED_DIRT == 11 && MATERIAL_COUNT == 12, "update data/shaders/constants.glsl");
_Static_assert(SOIL_SAND == 0 && SOIL_EARTH == 1 && SOIL_CLAY == 2 && SOIL_ROCK == 3, "update data/shaders/constants.glsl");

// The per frame block of terrain.frag (terrain_block), std140
typedef struct
{
	int32_t material_layers[MATERIAL_COUNT][4];
	float material_scales[MATERIAL_COUNT][4];
	float material_patches[MATERIAL_COUNT][4];
	float brush_line[4];
	float slope_ranges[4];
	float variation_parameters[3];
	float brush_radius;
	float detail_fade[2];
	float navigation_cell_meters[2];
	int32_t map_size;
	float cell_size;
	float max_height;
	float snow_height_fraction;
	float snow_aspect_shift;
	float soil_blend_cells;
	int32_t debug_view;
	int32_t tile_overlay;
	int32_t navigation_overlay;
	int32_t material_map;
	int32_t padding[2];
	float macro_averages[MATERIAL_COUNT][4];   // each material's macro texture's average (rgb as the texture holds it; a: its luminance)
	float grass_green[4];                      // rock_weathering's green: the grass's own texture's average, linear, times 0.8; w: 1 puddles (puddles.glsl)
} terrain_fragment_uniforms_t;

// The per chunk block of terrain.vert (chunk_block), std140
typedef struct
{
	int32_t chunk_origin[2];
	int32_t map_size;
	float cell_size;
	float lod_eye[3];
	int32_t lod_step;
	float lod_morph_range[2];
	float padding[2];
} terrain_chunk_uniforms_t;

// The chunk grid's vertices, and each drawn chunk's origin (per instance)
static const SDL_GPUVertexBufferDescription chunk_buffer_descriptions[2] =
{
	{ 0, sizeof(float) * 2, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 1, sizeof(int32_t) * 2, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
};
static const SDL_GPUVertexAttribute chunk_attributes[2] =
{
	{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
	{ 1, 1, SDL_GPU_VERTEXELEMENTFORMAT_INT2, 0 },
};

// Builds the shared chunk grid: (TERRAIN_CHUNK_SIZE + 1)^2 vertices holding
// only their local grid coordinates, and two triangles per quad, once for
// every level of detail (level k uses every 2^k-th vertex), one after the
// other in the same index buffer. 65 * 65 vertices fit in 16 bit indices.
static void create_chunk_mesh(terrain_renderer_t *renderer)
{
	const int vertices_per_side = TERRAIN_CHUNK_SIZE + 1;
	float *vertices = SDL_malloc(sizeof(float) * 2 * (size_t)(vertices_per_side * vertices_per_side));
	int total_index_count = 0;
	uint16_t *indices;
	int index = 0;

	for (int level = 0; level < TERRAIN_LOD_LEVEL_COUNT; level++)
	{
		int quads_per_side = TERRAIN_CHUNK_SIZE >> level;

		total_index_count += 6 * quads_per_side * quads_per_side;
	}
	indices = SDL_malloc(sizeof(uint16_t) * (size_t)total_index_count);

	for (int y = 0; y < vertices_per_side; y++)
	{
		for (int x = 0; x < vertices_per_side; x++)
		{
			vertices[(y * vertices_per_side + x) * 2 + 0] = (float)x;
			vertices[(y * vertices_per_side + x) * 2 + 1] = (float)y;
		}
	}

	for (int level = 0; level < TERRAIN_LOD_LEVEL_COUNT; level++)
	{
		int step = 1 << level;

		renderer->lod_index_offset[level] = index;
		for (int y = 0; y < TERRAIN_CHUNK_SIZE; y += step)
		{
			for (int x = 0; x < TERRAIN_CHUNK_SIZE; x += step)
			{
				uint16_t corner_00 = (uint16_t)(y * vertices_per_side + x);
				uint16_t corner_10 = (uint16_t)(corner_00 + step);
				uint16_t corner_01 = (uint16_t)(corner_00 + step * vertices_per_side);
				uint16_t corner_11 = (uint16_t)(corner_01 + step);

				// Counter clockwise when seen from above (+Y), with grid x
				// along world X and grid y along world Z. The diagonal runs
				// from corner 10 to corner 01 at every level; terrain.vert
				// relies on that when it morphs towards the coarser level.
				indices[index++] = corner_00;
				indices[index++] = corner_01;
				indices[index++] = corner_10;
				indices[index++] = corner_10;
				indices[index++] = corner_01;
				indices[index++] = corner_11;
			}
		}
		renderer->lod_index_count[level] = index - renderer->lod_index_offset[level];
	}
	// Shadow map and water draw the full detail grid, the first level
	renderer->index_count = renderer->lod_index_count[0];

	{
		uint32_t vertex_bytes = (uint32_t)(sizeof(float) * 2 * (size_t)(vertices_per_side * vertices_per_side));
		uint32_t index_bytes = (uint32_t)(sizeof(uint16_t) * (size_t)index);

		renderer->vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertex_bytes);
		renderer->index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, index_bytes);
		gpu_upload_buffer(renderer->vertex_buffer, 0, vertices, vertex_bytes);
		gpu_upload_buffer(renderer->index_buffer, 0, indices, index_bytes);
	}
	SDL_free(vertices);
	SDL_free(indices);
}

bool terrain_renderer_create(terrain_renderer_t *renderer, const render_settings_t *settings)
{
	gpu_pipeline_info_t pipeline;
	uint8_t zero = 0;

	SDL_zerop(renderer);
	renderer->soil_blend_meters = settings->soil_blend_meters;
	renderer->material_normal_meters = settings->material_normal_meters;
	renderer->material_sun_azimuth_degrees = settings->sun_azimuth_degrees;

	SDL_zero(pipeline);
	pipeline.vertex_shader = gpu_load_shader("terrain.vert", 1, 0, 2);
	pipeline.fragment_shader = gpu_load_shader("terrain.frag", 16, LIGHTS_STORAGE_BUFFERS, 2);
	pipeline.vertex_buffers = chunk_buffer_descriptions;
	pipeline.vertex_buffer_count = 2;
	pipeline.vertex_attributes = chunk_attributes;
	pipeline.vertex_attribute_count = 2;
	pipeline.cull = SDL_GPU_CULLMODE_BACK;
	pipeline.depth_test = true;
	pipeline.depth_write = true;
	pipeline.multisampled = true;
	renderer->pipeline = gpu_make_pipeline(&pipeline, "terrain");
	pipeline.fill = SDL_GPU_FILLMODE_LINE;
	renderer->wireframe_pipeline = gpu_make_pipeline(&pipeline, "terrain wireframe");
	SDL_ReleaseGPUShader(gpu.device, pipeline.fragment_shader);
	pipeline.fragment_shader = gpu_load_shader("terrain_map.frag", 16, LIGHTS_STORAGE_BUFFERS, 2);
	pipeline.fill = SDL_GPU_FILLMODE_FILL;
	renderer->map_pipeline = gpu_make_pipeline(&pipeline, "terrain from the material map");
	pipeline.fill = SDL_GPU_FILLMODE_LINE;
	renderer->map_wireframe_pipeline = gpu_make_pipeline(&pipeline, "terrain from the material map, wireframe");
	{
		// (without it a baked map's terrain is made on the CPU, chunk by chunk)
		gpu_compute_layout_t layout = { 1, 0, 0, 1, 256, 0, 1 };

		renderer->normals_pipeline = gpu_load_compute("terrain_normals.comp", &layout);
	}
	SDL_ReleaseGPUShader(gpu.device, pipeline.vertex_shader);
	SDL_ReleaseGPUShader(gpu.device, pipeline.fragment_shader);
	if (renderer->pipeline == NULL)
	{
		return false;
	}
	renderer->empty_integer_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 1, 1, 1, 1,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
	gpu_upload_texture(renderer->empty_integer_texture, 0, 0, 0, 0, 1, 1, &zero, 1);
	create_chunk_mesh(renderer);

	renderer->upload_slots = SDL_malloc(sizeof(struct terrain_upload_slot_s) * TERRAIN_UPLOAD_BATCH);
	if (renderer->upload_slots == NULL)
	{
		return false;
	}

	if (!materials_load(&renderer->materials, settings))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Loading terrain materials failed");
		return false;
	}
	return true;
}

static void release_holes(terrain_renderer_t *renderer)
{
	for (int i = 0; i < renderer->hole_chunk_slots; i++)
	{
		SDL_free(renderer->hole_indices[i]);
	}
	SDL_free(renderer->hole_indices);
	SDL_free(renderer->hole_index_counts);
	SDL_free(renderer->hole_index_offsets);
	SDL_free(renderer->holed_instances);
	SDL_free(renderer->holed_chunks);
	if (renderer->hole_index_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, renderer->hole_index_buffer);
	}
	renderer->hole_indices = NULL;
	renderer->hole_index_counts = NULL;
	renderer->hole_index_offsets = NULL;
	renderer->holed_instances = NULL;
	renderer->holed_chunks = NULL;
	renderer->hole_index_buffer = NULL;
	renderer->hole_chunk_slots = 0;
	renderer->holed_count = 0;
}

// A chunk's quads at full detail but those hidden whole (their four cells
// MATERIAL_GROUND_HIDDEN: under a building's floor); none hidden: none kept
static void chunk_holes(terrain_renderer_t *renderer, const terrain_t *terrain, int chunk_index)
{
	const int vertices_per_side = TERRAIN_CHUNK_SIZE + 1;
	int chunk_x = chunk_index % terrain->chunks_per_side;
	int chunk_y = chunk_index / terrain->chunks_per_side;
	int hidden = 0;
	uint16_t *indices;
	int count = 0;

	if (chunk_index >= renderer->hole_chunk_slots)
	{
		return;
	}
	for (int y = 0; y < TERRAIN_CHUNK_SIZE; y++)
	{
		for (int x = 0; x < TERRAIN_CHUNK_SIZE; x++)
		{
			bool all = true;

			for (int k = 0; k < 4 && all; k++)
			{
				int32_t cell_x = SDL_min(chunk_x * TERRAIN_CHUNK_SIZE + x + (k & 1), terrain->size - 1);
				int32_t cell_y = SDL_min(chunk_y * TERRAIN_CHUNK_SIZE + y + (k >> 1), terrain->size - 1);

				all = (terrain->materials[(size_t)terrain_index(terrain, cell_x, cell_y) * TERRAIN_MATERIAL_BYTES + MATERIAL_MAP_GROUND] & MATERIAL_GROUND_HIDDEN) != 0;
			}
			hidden += all ? 1 : 0;
		}
	}
	if (hidden == 0)
	{
		if (renderer->hole_indices[chunk_index] != NULL)
		{
			SDL_free(renderer->hole_indices[chunk_index]);
			renderer->hole_indices[chunk_index] = NULL;
			renderer->hole_index_counts[chunk_index] = 0;
			renderer->holes_changed = true;
		}
		return;
	}
	indices = SDL_malloc(sizeof(uint16_t) * 6 * (size_t)(TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE - hidden + 1));
	if (indices == NULL)
	{
		return;
	}
	for (int y = 0; y < TERRAIN_CHUNK_SIZE; y++)
	{
		for (int x = 0; x < TERRAIN_CHUNK_SIZE; x++)
		{
			bool all = true;
			uint16_t corner_00 = (uint16_t)(y * vertices_per_side + x);
			uint16_t corner_10 = (uint16_t)(corner_00 + 1);
			uint16_t corner_01 = (uint16_t)(corner_00 + vertices_per_side);
			uint16_t corner_11 = (uint16_t)(corner_01 + 1);

			for (int k = 0; k < 4 && all; k++)
			{
				int32_t cell_x = SDL_min(chunk_x * TERRAIN_CHUNK_SIZE + x + (k & 1), terrain->size - 1);
				int32_t cell_y = SDL_min(chunk_y * TERRAIN_CHUNK_SIZE + y + (k >> 1), terrain->size - 1);

				all = (terrain->materials[(size_t)terrain_index(terrain, cell_x, cell_y) * TERRAIN_MATERIAL_BYTES + MATERIAL_MAP_GROUND] & MATERIAL_GROUND_HIDDEN) != 0;
			}
			if (all)
			{
				continue;
			}
			// (as create_chunk_mesh has them)
			indices[count++] = corner_00;
			indices[count++] = corner_01;
			indices[count++] = corner_10;
			indices[count++] = corner_10;
			indices[count++] = corner_01;
			indices[count++] = corner_11;
		}
	}
	SDL_free(renderer->hole_indices[chunk_index]);
	renderer->hole_indices[chunk_index] = indices;
	renderer->hole_index_counts[chunk_index] = count;
	renderer->holes_changed = true;
}

// All the chunks' indices with holes into one buffer
static void upload_holes(terrain_renderer_t *renderer)
{
	int total = 0;
	uint16_t *joined;

	renderer->holes_changed = false;
	for (int i = 0; i < renderer->hole_chunk_slots; i++)
	{
		renderer->hole_index_offsets[i] = total;
		total += renderer->hole_index_counts[i];
	}
	if (renderer->hole_index_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, renderer->hole_index_buffer);
		renderer->hole_index_buffer = NULL;
	}
	if (total == 0)
	{
		return;
	}
	joined = SDL_malloc(sizeof(uint16_t) * (size_t)total);
	if (joined == NULL)
	{
		return;
	}
	for (int i = 0; i < renderer->hole_chunk_slots; i++)
	{
		if (renderer->hole_index_counts[i] > 0)
		{
			SDL_memcpy(joined + renderer->hole_index_offsets[i], renderer->hole_indices[i], sizeof(uint16_t) * (size_t)renderer->hole_index_counts[i]);
		}
	}
	renderer->hole_index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, (uint32_t)(sizeof(uint16_t) * (size_t)total));
	if (renderer->hole_index_buffer != NULL)
	{
		gpu_upload_buffer(renderer->hole_index_buffer, 0, joined, (uint32_t)(sizeof(uint16_t) * (size_t)total));
	}
	SDL_free(joined);
}

void terrain_renderer_destroy(terrain_renderer_t *renderer)
{
	release_holes(renderer);
	SDL_GPUTexture *textures[] = { renderer->height_texture, renderer->surface_texture, renderer->tile_texture, renderer->navigation_texture,
		renderer->soil_weight_texture, renderer->normal_texture, renderer->empty_integer_texture, renderer->material_texture };

	materials_destroy(&renderer->materials);
	for (size_t i = 0; i < SDL_arraysize(textures); i++)
	{
		if (textures[i] != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, textures[i]);
		}
	}
	SDL_free(renderer->tile_staging);
	SDL_free(renderer->soil_box_counts);
	SDL_free(renderer->soil_weight_staging);
	SDL_free(renderer->material_staging);
	SDL_free(renderer->bare_shown);
	SDL_free(renderer->chunk_bare_ready);
	if (renderer->normals_pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(gpu.device, renderer->normals_pipeline);
	}
	if (renderer->vertex_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, renderer->vertex_buffer);
	}
	if (renderer->index_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, renderer->index_buffer);
	}
	if (renderer->instance_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, renderer->instance_buffer);
	}
	SDL_free(renderer->instance_staging);
	if (renderer->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, renderer->pipeline);
	}
	if (renderer->wireframe_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, renderer->wireframe_pipeline);
	}
	if (renderer->map_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, renderer->map_pipeline);
	}
	if (renderer->map_wireframe_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, renderer->map_wireframe_pipeline);
	}
	SDL_free(renderer->upload_slots);
	SDL_free(renderer->chunk_needs_soil_blur);
	SDL_free(renderer->chunk_pending_upload);
	SDL_free(renderer->chunk_pending_shape);
	SDL_free(renderer->chunk_uploaded_seconds);
	SDL_free(renderer->chunk_far);
	SDL_free(renderer->visible_chunks);
	SDL_free(renderer->chunk_bounds);
	SDL_free(renderer->block_bounds);
	renderer->chunk_bounds = NULL;
	renderer->block_bounds = NULL;
	renderer->bounds_chunks_per_side = 0;
	SDL_zerop(renderer);
}

// A texture over the whole map (or one per tile): filtering is the
// sampler's business (heights and integer textures are read exactly)
static SDL_GPUTexture *create_map_texture(SDL_GPUTextureFormat format, int size)
{
	return gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, format, (uint32_t)size, (uint32_t)size, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
}

static void release_texture(SDL_GPUTexture **texture)
{
	if (*texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, *texture);
		*texture = NULL;
	}
}

// Cells per texel of the soil weight texture: about a quarter of the soil
// blend width (the blur spans two texels each way, the filtering one more),
// as a power of two so a chunk covers whole texels
static int soil_cells_per_texel(float soil_blend_meters, float cell_size_meters)
{
	float blend_cells = soil_blend_meters / SDL_max(cell_size_meters, 1e-3f);
	int cells = 1;

	while (cells < 16 && (float)(cells * 2) <= blend_cells * 0.25f)
	{
		cells *= 2;
	}
	return cells;
}

static void ensure_map_textures(terrain_renderer_t *renderer, int size, float cell_size_meters)
{
	int chunks_per_side = size / TERRAIN_CHUNK_SIZE;
	int cells_per_texel = soil_cells_per_texel(renderer->soil_blend_meters, cell_size_meters);

	if (renderer->texture_map_size == size && renderer->soil_cells_per_texel == cells_per_texel)
	{
		return;
	}
	release_texture(&renderer->height_texture);
	release_texture(&renderer->surface_texture);
	// (written by the GPU too: water_ground.comp)
	renderer->height_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, (uint32_t)size, (uint32_t)size, 1, 1,
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
	renderer->surface_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (uint32_t)size, (uint32_t)size,
		1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
	renderer->texture_map_size = size;
	SDL_free(renderer->bare_shown);
	SDL_free(renderer->chunk_bare_ready);
	renderer->bare_shown = SDL_calloc((size_t)size * (size_t)size * 2, 1);
	renderer->chunk_bare_ready = SDL_calloc((size_t)chunks_per_side * (size_t)chunks_per_side, sizeof(bool));

	release_texture(&renderer->normal_texture);
	renderer->normal_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (uint32_t)size, (uint32_t)size,
		1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
	renderer->material_normal_cells = SDL_clamp((int)SDL_lroundf(renderer->material_normal_meters / SDL_max(cell_size_meters, 1e-3f)),
		1, TERRAIN_NORMAL_REACH_LIMIT);

	release_texture(&renderer->material_texture);
	renderer->material_texture = create_map_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, size);
	if (renderer->material_staging == NULL)
	{
		renderer->material_staging = SDL_malloc((size_t)TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE * TERRAIN_MATERIAL_BYTES);
	}

	release_texture(&renderer->soil_weight_texture);
	{
		int texels_per_side = size / cells_per_texel;

		renderer->soil_cells_per_texel = cells_per_texel;
		renderer->soil_weight_texture = create_map_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, texels_per_side);
		SDL_free(renderer->soil_box_counts);
		SDL_free(renderer->soil_weight_staging);
		renderer->soil_box_counts = SDL_calloc((size_t)texels_per_side * (size_t)texels_per_side * 4, sizeof(uint16_t));
		// Large enough for one chunk plus the blur's reach on every side
		renderer->soil_weight_staging = SDL_malloc((size_t)4 * (TERRAIN_CHUNK_SIZE + 4) * (TERRAIN_CHUNK_SIZE + 4));
	}

	SDL_free(renderer->visible_chunks);
	SDL_free(renderer->chunk_bounds);
	SDL_free(renderer->block_bounds);
	renderer->chunk_bounds = NULL;
	renderer->block_bounds = NULL;
	renderer->bounds_chunks_per_side = 0;
	SDL_free(renderer->chunk_needs_soil_blur);
	renderer->chunk_needs_soil_blur = SDL_calloc((size_t)(chunks_per_side * chunks_per_side), sizeof(bool));
	// A new map uploads everything anyway
	SDL_free(renderer->chunk_pending_upload);
	SDL_free(renderer->chunk_pending_shape);
	SDL_free(renderer->chunk_uploaded_seconds);
	SDL_free(renderer->chunk_far);
	renderer->chunk_far = SDL_calloc((size_t)(chunks_per_side * chunks_per_side), sizeof(bool));
	renderer->chunk_pending_upload = SDL_calloc((size_t)(chunks_per_side * chunks_per_side), sizeof(bool));
	renderer->chunk_pending_shape = SDL_calloc((size_t)(chunks_per_side * chunks_per_side), sizeof(bool));
	renderer->chunk_uploaded_seconds = SDL_calloc((size_t)(chunks_per_side * chunks_per_side), sizeof(double));
	renderer->any_chunk_pending = false;
	renderer->total_chunk_count = chunks_per_side * chunks_per_side;
	renderer->visible_chunk_capacity = renderer->total_chunk_count;
	renderer->visible_chunks = SDL_malloc(sizeof(renderer->visible_chunks[0]) * (size_t)renderer->visible_chunk_capacity);
}

// How long the bare bedrock shown takes to follow the ground all the way
// (rock to soil or back)
static const float BARE_FADE_SECONDS = 1.5f;

void terrain_surface_bytes(const terrain_t *terrain, const water_t *water, const soil_settings_t *soil_settings, int32_t index,
	uint8_t bytes_out[TERRAIN_BAKED_SURFACE_BYTES])
{
	float silt_visible_units = soil_settings->silt_visible_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER;
	float silt_full_units = SDL_max(soil_settings->silt_full_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER,
		silt_visible_units + 1.0f);
	// Erodible soil worn down to less than this shows the bedrock
	const float bare_units = 0.05f * (float)TERRAIN_HEIGHT_UNITS_PER_METER;
	float silt;
	float wetness;
	float bare = 0.0f;
	int32_t capacity;

	if (terrain->baked)
	{
		int32_t x = index % terrain->size;
		int32_t y = index / terrain->size;
		const uint8_t *surface = terrain->baked_surface[(y / TERRAIN_CHUNK_SIZE) * terrain->chunks_per_side + x / TERRAIN_CHUNK_SIZE];

		if (surface == NULL)
		{
			SDL_memset(bytes_out, 0, TERRAIN_BAKED_SURFACE_BYTES);
			return;
		}
		SDL_memcpy(bytes_out, surface + ((y % TERRAIN_CHUNK_SIZE) * TERRAIN_CHUNK_SIZE + x % TERRAIN_CHUNK_SIZE) * TERRAIN_BAKED_SURFACE_BYTES,
			TERRAIN_BAKED_SURFACE_BYTES);
		return;
	}
	silt = smoothstep_float(silt_visible_units, silt_full_units, (float)terrain->silt_thickness[index]);
	capacity = erosion_moisture_capacity_units(terrain, soil_settings, index);
	wetness = capacity > 0 ? SDL_min((float)water->moisture[index] / (float)capacity, 1.0f) : 0.0f;
	// The rock shows through the last few centimeters of soil, and silt
	// too thin to show does not hide it (a film of silt left and washed
	// off again every step flipped it from rock to soil and back, many
	// times a second, under running water)
	if (terrain->soil_types[index] != SOIL_ROCK)
	{
		bare = (1.0f - smoothstep_float(0.0f, bare_units, (float)(terrain->heights[index] - terrain->bedrock_heights[index]))) * (1.0f - silt);
	}
	bytes_out[0] = (uint8_t)(silt * 255.0f + 0.5f);
	bytes_out[1] = (uint8_t)(wetness * 255.0f + 0.5f);
	bytes_out[2] = (uint8_t)(bare * 255.0f + 0.5f);
}

// Surface state of one chunk, as bytes: silt cover, wetness, bare bedrock,
// water on it (a few centimeters or more: the full value; it goes up with
// the wetness's own updates, which follow the water)
// (bare_shown: the bare bedrock as shown, followed a step at a time; NULL:
// at once. ready: the chunk's is there already. slot->bare_settling: some
// of it is still on its way)
static void fill_surface(struct terrain_upload_slot_s *slot, const terrain_t *terrain, const water_t *water,
	const soil_settings_t *soil_settings, uint8_t *bare_shown, bool ready)
{
	int start_x = slot->chunk_x * TERRAIN_CHUNK_SIZE;
	int start_y = slot->chunk_y * TERRAIN_CHUNK_SIZE;
	int step = SDL_clamp((int)(255.0f * slot->since_seconds / BARE_FADE_SECONDS), 1, 255);

	slot->bare_settling = false;
	for (int y = 0; y < TERRAIN_CHUNK_SIZE; y++)
	{
		for (int x = 0; x < TERRAIN_CHUNK_SIZE; x++)
		{
			int32_t index = terrain_index(terrain, start_x + x, start_y + y);
			uint8_t *texel = &slot->surface[(y * TERRAIN_CHUNK_SIZE + x) * 4];
			uint8_t bytes[TERRAIN_BAKED_SURFACE_BYTES];
			int bare_byte;
			int silt_byte;

			terrain_surface_bytes(terrain, water, soil_settings, index, bytes);
			silt_byte = bytes[0];
			bare_byte = bytes[2];
			if (bare_shown != NULL && !terrain->baked)
			{
				// (all the way over in BARE_FADE_SECONDS, however often
				// the chunk goes up; the silt too: a film of it laid and
				// washed off again and again under running water crawled
				// over the rock like a river under the river)
				int shown = bare_shown[(size_t)index * 2];
				int silt_shown = bare_shown[(size_t)index * 2 + 1];

				if (ready && SDL_abs(bare_byte - shown) > step)
				{
					bare_byte = shown + SDL_clamp(bare_byte - shown, -step, step);
					slot->bare_settling = true;
				}
				if (ready && SDL_abs(silt_byte - silt_shown) > step)
				{
					silt_byte = silt_shown + SDL_clamp(silt_byte - silt_shown, -step, step);
					slot->bare_settling = true;
				}
				bare_shown[(size_t)index * 2] = (uint8_t)bare_byte;
				bare_shown[(size_t)index * 2 + 1] = (uint8_t)silt_byte;
			}
			texel[0] = (uint8_t)silt_byte;
			texel[1] = bytes[1];
			slot->wetness[y * TERRAIN_CHUNK_SIZE + x] = texel[1];
			texel[2] = (uint8_t)bare_byte;
			texel[3] = (uint8_t)(smoothstep_float(0.005f * (float)TERRAIN_HEIGHT_UNITS_PER_METER, 0.03f * (float)TERRAIN_HEIGHT_UNITS_PER_METER,
				(float)water->depth[index]) * 255.0f + 0.5f);
		}
	}
}

// The wetness of one chunk alone (the GPU makes the rest of the surface)
static void fill_wetness(struct terrain_upload_slot_s *slot, const terrain_t *terrain, const water_t *water,
	const soil_settings_t *soil_settings)
{
	int start_x = slot->chunk_x * TERRAIN_CHUNK_SIZE;
	int start_y = slot->chunk_y * TERRAIN_CHUNK_SIZE;

	for (int y = 0; y < TERRAIN_CHUNK_SIZE; y++)
	{
		for (int x = 0; x < TERRAIN_CHUNK_SIZE; x++)
		{
			int32_t index = terrain_index(terrain, start_x + x, start_y + y);
			int32_t capacity = erosion_moisture_capacity_units(terrain, soil_settings, index);
			float wetness = capacity > 0 ? SDL_min((float)water->moisture[index] / (float)capacity, 1.0f) : 0.0f;

			slot->wetness[y * TERRAIN_CHUNK_SIZE + x] = (uint8_t)(wetness * 255.0f + 0.5f);
		}
	}
}

// Heights of one chunk: integer units to float meters
static void fill_heights(struct terrain_upload_slot_s *slot, const terrain_t *terrain)
{
	int start_x = slot->chunk_x * TERRAIN_CHUNK_SIZE;
	int start_y = slot->chunk_y * TERRAIN_CHUNK_SIZE;

	for (int y = 0; y < TERRAIN_CHUNK_SIZE; y++)
	{
		const int32_t *source = &terrain->heights[terrain_index(terrain, start_x, start_y + y)];
		float *destination = &slot->heights[y * TERRAIN_CHUNK_SIZE];

		for (int x = 0; x < TERRAIN_CHUNK_SIZE; x++)
		{
			destination[x] = (float)source[x] * TERRAIN_METERS_PER_HEIGHT_UNIT;
		}
	}
}

// Encodes a normal's x or z (-1..1) as a byte
static uint8_t encode_normal_component(float value)
{
	return (uint8_t)SDL_clamp(value * 127.5f + 127.5f + 0.5f, 0.0f, 255.0f);
}

// Normals of a chunk and of the cells around it whose material normal
// reaches into it (their heights' neighbors changed too). Neighbor rows and
// columns are clamped once per row and column, not per lookup.
static void fill_normals(struct terrain_upload_slot_s *slot, const terrain_t *terrain, int reach)
{
	int start_x = slot->chunk_x * TERRAIN_CHUNK_SIZE;
	int start_y = slot->chunk_y * TERRAIN_CHUNK_SIZE;
	int first_x = SDL_max(start_x - reach, 0);
	int first_y = SDL_max(start_y - reach, 0);
	int last_x = SDL_min(start_x + TERRAIN_CHUNK_SIZE + reach, terrain->size);
	int last_y = SDL_min(start_y + TERRAIN_CHUNK_SIZE + reach, terrain->size);
	int width = last_x - first_x;
	int last_cell = terrain->size - 1;
	float cell_size = terrain->cell_size_meters;
	float sharp_y = 2.0f * cell_size;
	float wide_y = 2.0f * (float)reach * cell_size;
	const int32_t *heights = terrain->heights;

	slot->normal_x = first_x;
	slot->normal_y = first_y;
	slot->normal_width = width;
	slot->normal_height = last_y - first_y;
	for (int y = first_y; y < last_y; y++)
	{
		const int32_t *row = &heights[(size_t)y * (size_t)terrain->size];
		const int32_t *row_back = &heights[(size_t)SDL_max(y - 1, 0) * (size_t)terrain->size];
		const int32_t *row_front = &heights[(size_t)SDL_min(y + 1, last_cell) * (size_t)terrain->size];
		const int32_t *row_far_back = &heights[(size_t)SDL_max(y - reach, 0) * (size_t)terrain->size];
		const int32_t *row_far_front = &heights[(size_t)SDL_min(y + reach, last_cell) * (size_t)terrain->size];
		uint8_t *texel = &slot->normals[(size_t)(y - first_y) * (size_t)width * 4];

		for (int x = first_x; x < last_x; x++, texel += 4)
		{
			int left = SDL_max(x - 1, 0);
			int right = SDL_min(x + 1, last_cell);
			int far_left = SDL_max(x - reach, 0);
			int far_right = SDL_min(x + reach, last_cell);
			// Central differences: the normal of a height field h(x, z) is
			// (-dh/dx, 1, -dh/dz), here scaled by 2 * reach * cell_size
			float sharp_x = (float)(row[left] - row[right]) * TERRAIN_METERS_PER_HEIGHT_UNIT;
			float sharp_z = (float)(row_back[x] - row_front[x]) * TERRAIN_METERS_PER_HEIGHT_UNIT;
			float wide_x = (float)(row[far_left] - row[far_right]) * TERRAIN_METERS_PER_HEIGHT_UNIT;
			float wide_z = (float)(row_far_back[x] - row_far_front[x]) * TERRAIN_METERS_PER_HEIGHT_UNIT;
			float sharp_scale = 1.0f / SDL_sqrtf(sharp_x * sharp_x + sharp_y * sharp_y + sharp_z * sharp_z);
			float wide_scale = 1.0f / SDL_sqrtf(wide_x * wide_x + wide_y * wide_y + wide_z * wide_z);

			texel[0] = encode_normal_component(sharp_x * sharp_scale);
			texel[1] = encode_normal_component(sharp_z * sharp_scale);
			texel[2] = encode_normal_component(wide_x * wide_scale);
			texel[3] = encode_normal_component(wide_z * wide_scale);
		}
	}
}

// Soil weights, step one: how many cells of each soil every texel of the
// chunk covers. Anything that is not sand, earth or clay counts as rock,
// as in the shader before. Returns true if any count changed.
static bool count_chunk_soils(terrain_renderer_t *renderer, const terrain_t *terrain, int start_x, int start_y)
{
	int cells_per_texel = renderer->soil_cells_per_texel;
	int texels_per_side = terrain->size / cells_per_texel;
	int texels_per_chunk = TERRAIN_CHUNK_SIZE / cells_per_texel;
	bool changed = false;

	for (int texel_y = 0; texel_y < texels_per_chunk; texel_y++)
	{
		for (int texel_x = 0; texel_x < texels_per_chunk; texel_x++)
		{
			int map_texel_x = start_x / cells_per_texel + texel_x;
			int map_texel_y = start_y / cells_per_texel + texel_y;
			uint16_t *stored = &renderer->soil_box_counts[((size_t)map_texel_y * (size_t)texels_per_side + (size_t)map_texel_x) * 4];
			uint16_t counts[4] = { 0, 0, 0, 0 };

			for (int y = 0; y < cells_per_texel; y++)
			{
				const uint8_t *soils = &terrain->soil_types[terrain_index(terrain, map_texel_x * cells_per_texel,
					map_texel_y * cells_per_texel + y)];

				for (int x = 0; x < cells_per_texel; x++)
				{
					switch (soils[x])
					{
					case SOIL_SAND: counts[0]++; break;
					case SOIL_EARTH: counts[1]++; break;
					case SOIL_CLAY: counts[2]++; break;
					default: counts[3]++; break;
					}
				}
			}
			for (int k = 0; k < 4; k++)
			{
				if (stored[k] != counts[k])
				{
					stored[k] = counts[k];
					changed = true;
				}
			}
		}
	}
	return changed;
}

// Soil weights, step two: a 5x5 binomial blur of the counts over the chunk's
// texels and two texels around it (their blur reads this chunk's counts),
// uploaded as bytes. Must run after the counts of every changed chunk are
// up to date.
static void blur_chunk_soils(terrain_renderer_t *renderer, const terrain_t *terrain, int start_x, int start_y)
{
	static const int binomial[5] = { 1, 4, 6, 4, 1 };
	int cells_per_texel = renderer->soil_cells_per_texel;
	int texels_per_side = terrain->size / cells_per_texel;
	int texels_per_chunk = TERRAIN_CHUNK_SIZE / cells_per_texel;
	int first_x = SDL_max(start_x / cells_per_texel - 2, 0);
	int first_y = SDL_max(start_y / cells_per_texel - 2, 0);
	int last_x = SDL_min(start_x / cells_per_texel + texels_per_chunk + 2, texels_per_side);
	int last_y = SDL_min(start_y / cells_per_texel + texels_per_chunk + 2, texels_per_side);
	int width = last_x - first_x;
	int height = last_y - first_y;
	// Kernel sum 256, times the cells per texel
	float scale = 255.0f / (256.0f * (float)(cells_per_texel * cells_per_texel));

	for (int texel_y = first_y; texel_y < last_y; texel_y++)
	{
		for (int texel_x = first_x; texel_x < last_x; texel_x++)
		{
			int sums[4] = { 0, 0, 0, 0 };
			uint8_t *destination = &renderer->soil_weight_staging[((texel_y - first_y) * width + (texel_x - first_x)) * 4];

			for (int offset_y = -2; offset_y <= 2; offset_y++)
			{
				int sample_y = SDL_clamp(texel_y + offset_y, 0, texels_per_side - 1);

				for (int offset_x = -2; offset_x <= 2; offset_x++)
				{
					int sample_x = SDL_clamp(texel_x + offset_x, 0, texels_per_side - 1);
					int weight = binomial[offset_x + 2] * binomial[offset_y + 2];
					const uint16_t *counts = &renderer->soil_box_counts[((size_t)sample_y * (size_t)texels_per_side + (size_t)sample_x) * 4];

					sums[0] += weight * counts[0];
					sums[1] += weight * counts[1];
					sums[2] += weight * counts[2];
					sums[3] += weight * counts[3];
				}
			}
			for (int k = 0; k < 4; k++)
			{
				destination[k] = (uint8_t)SDL_min((float)sums[k] * scale + 0.5f, 255.0f);
			}
		}
	}
	gpu_upload_texture(renderer->soil_weight_texture, 0, 0, (uint32_t)first_x, (uint32_t)first_y, (uint32_t)width, (uint32_t)height,
		renderer->soil_weight_staging, (size_t)width * (size_t)height * 4);
}

// The same over the whole map at once (a baked map's load): rows side by
// side, one upload. False if there was no memory for it
static bool blur_all_soils(terrain_renderer_t *renderer, const terrain_t *terrain)
{
	static const int binomial[5] = { 1, 4, 6, 4, 1 };
	int cells_per_texel = renderer->soil_cells_per_texel;
	int texels_per_side = terrain->size / cells_per_texel;
	float scale = 255.0f / (256.0f * (float)(cells_per_texel * cells_per_texel));
	uint8_t *weights = SDL_malloc((size_t)texels_per_side * (size_t)texels_per_side * 4);

	if (weights == NULL)
	{
		return false;
	}
#pragma omp parallel for schedule(static)
	for (int texel_y = 0; texel_y < texels_per_side; texel_y++)
	{
		for (int texel_x = 0; texel_x < texels_per_side; texel_x++)
		{
			int sums[4] = { 0, 0, 0, 0 };
			uint8_t *destination = &weights[((size_t)texel_y * (size_t)texels_per_side + (size_t)texel_x) * 4];

			for (int offset_y = -2; offset_y <= 2; offset_y++)
			{
				int sample_y = SDL_clamp(texel_y + offset_y, 0, texels_per_side - 1);

				for (int offset_x = -2; offset_x <= 2; offset_x++)
				{
					int sample_x = SDL_clamp(texel_x + offset_x, 0, texels_per_side - 1);
					int weight = binomial[offset_x + 2] * binomial[offset_y + 2];
					const uint16_t *counts = &renderer->soil_box_counts[((size_t)sample_y * (size_t)texels_per_side + (size_t)sample_x) * 4];

					sums[0] += weight * counts[0];
					sums[1] += weight * counts[1];
					sums[2] += weight * counts[2];
					sums[3] += weight * counts[3];
				}
			}
			for (int k = 0; k < 4; k++)
			{
				destination[k] = (uint8_t)SDL_min((float)sums[k] * scale + 0.5f, 255.0f);
			}
		}
	}
	gpu_upload_texture(renderer->soil_weight_texture, 0, 0, 0, 0, (uint32_t)texels_per_side, (uint32_t)texels_per_side, weights,
		(size_t)texels_per_side * (size_t)texels_per_side * 4);
	SDL_free(weights);
	return true;
}

void terrain_renderer_upload_tiles(terrain_renderer_t *renderer, const water_t *water)
{
	int32_t tile_count = water->tiles_per_side * water->tiles_per_side;

	if (renderer->tile_texture_size != water->tiles_per_side)
	{
		release_texture(&renderer->tile_texture);
		SDL_free(renderer->tile_staging);
		renderer->tile_staging = SDL_malloc((size_t)tile_count);
		renderer->tile_texture = create_map_texture(SDL_GPU_TEXTUREFORMAT_R8_UNORM, water->tiles_per_side);
		renderer->tile_texture_size = water->tiles_per_side;
	}
	if (!renderer->tile_overlay || renderer->tile_staging == NULL)
	{
		return;
	}
	for (int32_t tile = 0; tile < tile_count; tile++)
	{
		bool mud = water->tile_sediment[tile] > 0;
		uint8_t value = TERRAIN_TILE_OVERLAY_DRY;

		if (water->tile_state[tile] == WATER_TILE_ACTIVE)
		{
			value = mud ? TERRAIN_TILE_OVERLAY_ACTIVE_WITH_MUD : TERRAIN_TILE_OVERLAY_ACTIVE;
		}
		else if (water->tile_state[tile] == WATER_TILE_SLEEPING || mud)
		{
			value = mud ? TERRAIN_TILE_OVERLAY_SLEEPING_WITH_MUD : TERRAIN_TILE_OVERLAY_SLEEPING;
		}
		renderer->tile_staging[tile] = value;
	}
	gpu_upload_texture(renderer->tile_texture, 0, 0, 0, 0, (uint32_t)water->tiles_per_side, (uint32_t)water->tiles_per_side,
		renderer->tile_staging, (size_t)tile_count);
}

void terrain_renderer_upload_navigation(terrain_renderer_t *renderer, int size, float cell_meters, const uint8_t *values)
{
	if (renderer->navigation_texture == NULL || renderer->navigation_texture_size != size)
	{
		release_texture(&renderer->navigation_texture);
		renderer->navigation_texture = create_map_texture(SDL_GPU_TEXTUREFORMAT_R8_UNORM, size);
		renderer->navigation_texture_size = size;
	}
	renderer->navigation_cell_meters = cell_meters;
	gpu_upload_texture(renderer->navigation_texture, 0, 0, 0, 0, (uint32_t)size, (uint32_t)size, values, (size_t)size * (size_t)size);
}

static float lod_base_distance(const terrain_t *terrain, const render_settings_t *settings);

void terrain_renderer_bake_materials(const terrain_renderer_t *renderer, terrain_t *terrain, const render_settings_t *settings)
{
	material_rules_t rules;

	material_rules_make(&rules, settings, &renderer->materials, terrain->cell_size_meters);
	{
		float azimuth = degrees_to_radians(renderer->material_sun_azimuth_degrees);

		rules.sun_x = SDL_cosf(azimuth);
		rules.sun_z = SDL_sinf(azimuth);
	}
	material_map_update(terrain, &rules);
}

// The material map: baked where it must be, its changed chunks up (all of
// them on a new texture). Small (a chunk is 16 KB): no batching.
static void upload_materials(terrain_renderer_t *renderer, terrain_t *terrain, const render_settings_t *settings, bool recreated)
{
	int chunks_per_side = terrain->chunks_per_side;
	int chunk_total = chunks_per_side * chunks_per_side;

	terrain_renderer_bake_materials(renderer, terrain, settings);
	if (renderer->hole_chunk_slots != chunk_total)
	{
		release_holes(renderer);
		renderer->hole_indices = SDL_calloc((size_t)SDL_max(chunk_total, 1), sizeof(uint16_t *));
		renderer->hole_index_counts = SDL_calloc((size_t)SDL_max(chunk_total, 1), sizeof(int));
		renderer->hole_index_offsets = SDL_calloc((size_t)SDL_max(chunk_total, 1), sizeof(int));
		renderer->holed_instances = SDL_calloc((size_t)SDL_max(chunk_total, 1), sizeof(int));
		renderer->holed_chunks = SDL_calloc((size_t)SDL_max(chunk_total, 1), sizeof(int));
		renderer->hole_chunk_slots = renderer->hole_indices != NULL && renderer->hole_index_counts != NULL && renderer->hole_index_offsets != NULL
			&& renderer->holed_instances != NULL && renderer->holed_chunks != NULL ? chunk_total : 0;
	}
	if (!terrain->any_chunk_materials_changed && !recreated)
	{
		return;
	}
	for (int chunk_y = 0; chunk_y < chunks_per_side; chunk_y++)
	{
		for (int chunk_x = 0; chunk_x < chunks_per_side; chunk_x++)
		{
			terrain_chunk_t *chunk = &terrain->chunks[chunk_y * chunks_per_side + chunk_x];
			size_t row_bytes = (size_t)TERRAIN_CHUNK_SIZE * TERRAIN_MATERIAL_BYTES;

			if (!chunk->materials_changed && !recreated)
			{
				continue;
			}
			chunk->materials_changed = false;
			for (int y = 0; y < TERRAIN_CHUNK_SIZE; y++)
			{
				size_t index = (size_t)terrain_index(terrain, chunk_x * TERRAIN_CHUNK_SIZE, chunk_y * TERRAIN_CHUNK_SIZE + y);

				SDL_memcpy(renderer->material_staging + (size_t)y * row_bytes, terrain->materials + index * TERRAIN_MATERIAL_BYTES, row_bytes);
			}
			gpu_upload_texture(renderer->material_texture, 0, 0, (uint32_t)(chunk_x * TERRAIN_CHUNK_SIZE), (uint32_t)(chunk_y * TERRAIN_CHUNK_SIZE),
				TERRAIN_CHUNK_SIZE, TERRAIN_CHUNK_SIZE, renderer->material_staging, row_bytes * TERRAIN_CHUNK_SIZE);
			chunk_holes(renderer, terrain, chunk_y * chunks_per_side + chunk_x);
		}
	}
	terrain->any_chunk_materials_changed = false;
	if (renderer->holes_changed)
	{
		upload_holes(renderer);
	}
}

// Is this chunk far enough that its changes can wait: beyond full detail
// (the first level of detail starts there), or out of view?
static bool update_chunk_bounds(terrain_renderer_t *renderer, const terrain_t *terrain);

// (lowest, highest: the chunk's height range, as its chunk or the
// renderer's copy of them has it)
static bool chunk_can_wait(const terrain_t *terrain, const camera_t *camera, const frustum_t *frustum, float lod_base, int chunk_index,
	float lowest, float highest)
{
	float chunk_extent = (float)TERRAIN_CHUNK_SIZE * terrain->cell_size_meters;
	int chunk_x = chunk_index % terrain->chunks_per_side;
	int chunk_y = chunk_index / terrain->chunks_per_side;
	vec3_t box_minimum = vec3_make((float)chunk_x * chunk_extent, lowest, (float)chunk_y * chunk_extent);
	// Water stands above the ground; leave room for it, or a lake seen
	// from below would count as out of view
	vec3_t box_maximum = vec3_make(box_minimum.x + chunk_extent, highest + 20.0f, box_minimum.z + chunk_extent);
	float outside_x;
	float outside_z;

	// (too far first: most of a big map's chunks, told with a few
	// subtractions; the frustum's six planes only for the near ones)
	if (lod_base > 0.0f)
	{
		outside_x = SDL_max(SDL_max(box_minimum.x - camera->eye_position.x, camera->eye_position.x - box_maximum.x), 0.0f);
		outside_z = SDL_max(SDL_max(box_minimum.z - camera->eye_position.z, camera->eye_position.z - box_maximum.z), 0.0f);
		if (outside_x * outside_x + outside_z * outside_z >= lod_base * lod_base)
		{
			return true;
		}
	}
	return !frustum_contains_box(frustum, box_minimum, box_maximum);
}

// A baked map's whole terrain at once (just loaded): the heights and the
// surface go up a row of chunks at a time, the normals are made from the
// heights on the GPU, the soils counted on the CPU side by side. Many
// times faster than chunk by chunk (whose normals were most of it). True
// if it did it (else the chunks go up the usual way).
static bool upload_baked_whole(terrain_renderer_t *renderer, terrain_t *terrain, const water_t *water, const soil_settings_t *soil_settings,
	double now_seconds)
{
	int32_t size = terrain->size;
	int chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;
	float *heights;
	uint8_t *surface;
	uint64_t start = SDL_GetPerformanceCounter();
	double rows_milliseconds;
	double soils_milliseconds;

	if (!terrain->baked || renderer->normals_pipeline == NULL || renderer->height_texture == NULL || renderer->normal_texture == NULL
		|| renderer->surface_texture == NULL)
	{
		return false;
	}
	for (int chunk = 0; chunk < chunk_count; chunk++)
	{
		if (!terrain->chunks[chunk].needs_shape_upload && !renderer->chunk_pending_shape[chunk])
		{
			return false;
		}
	}
	heights = SDL_malloc((size_t)size * TERRAIN_CHUNK_SIZE * sizeof(float));
	surface = SDL_malloc((size_t)size * TERRAIN_CHUNK_SIZE * 4);
	if (heights == NULL || surface == NULL)
	{
		SDL_free(heights);
		SDL_free(surface);
		return false;
	}
	for (int32_t chunk_y = 0; chunk_y < terrain->chunks_per_side; chunk_y++)
	{
#pragma omp parallel for schedule(static)
		for (int32_t y = 0; y < TERRAIN_CHUNK_SIZE; y++)
		{
			int32_t cell_y = chunk_y * TERRAIN_CHUNK_SIZE + y;

			for (int32_t x = 0; x < size; x++)
			{
				int32_t index = cell_y * size + x;
				size_t texel = (size_t)y * (size_t)size + (size_t)x;
				uint8_t bytes[TERRAIN_BAKED_SURFACE_BYTES];

				heights[texel] = (float)terrain->heights[index] * TERRAIN_METERS_PER_HEIGHT_UNIT;
				terrain_surface_bytes(terrain, water, soil_settings, index, bytes);
				surface[texel * 4] = bytes[0];
				surface[texel * 4 + 1] = bytes[1];
				surface[texel * 4 + 2] = bytes[2];
				surface[texel * 4 + 3] = (uint8_t)(smoothstep_float(0.005f * (float)TERRAIN_HEIGHT_UNITS_PER_METER,
					0.03f * (float)TERRAIN_HEIGHT_UNITS_PER_METER, (float)water->depth[index]) * 255.0f + 0.5f);
			}
		}
		gpu_upload_texture(renderer->height_texture, 0, 0, 0, (uint32_t)(chunk_y * TERRAIN_CHUNK_SIZE), (uint32_t)size, TERRAIN_CHUNK_SIZE,
			heights, (size_t)size * TERRAIN_CHUNK_SIZE * sizeof(float));
		gpu_upload_texture(renderer->surface_texture, 0, 0, 0, (uint32_t)(chunk_y * TERRAIN_CHUNK_SIZE), (uint32_t)size, TERRAIN_CHUNK_SIZE,
			surface, (size_t)size * TERRAIN_CHUNK_SIZE * 4);
	}
	SDL_free(heights);
	SDL_free(surface);
	rows_milliseconds = 1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();

	// The normals, once the heights are there
	gpu_flush_uploads();
	{
		SDL_GPUCommandBuffer *command_buffer = gpu_commands();
		SDL_GPUStorageTextureReadWriteBinding target;
		SDL_GPUTextureSamplerBinding sampler;
		SDL_GPUComputePass *pass;
		struct
		{
			int32_t map_size;
			int32_t reach;
			float cell_size;
			float padding;
		} uniforms = { size, SDL_max(renderer->material_normal_cells, 1), terrain->cell_size_meters, 0.0f };

		if (command_buffer == NULL)
		{
			return false;
		}
		SDL_zero(target);
		target.texture = renderer->normal_texture;
		pass = SDL_BeginGPUComputePass(command_buffer, &target, 1, NULL, 0);
		if (pass == NULL)
		{
			return false;
		}
		sampler.texture = renderer->height_texture;
		sampler.sampler = gpu.nearest_clamp;
		SDL_BindGPUComputePipeline(pass, renderer->normals_pipeline);
		SDL_BindGPUComputeSamplers(pass, 0, &sampler, 1);
		SDL_PushGPUComputeUniformData(command_buffer, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (uint32_t)((size + 255) / 256), (uint32_t)size, 1);
		SDL_EndGPUComputePass(pass);
	}

	// The soils: every chunk counted (each its own texels), then blurred
	start = SDL_GetPerformanceCounter();
#pragma omp parallel for schedule(dynamic, 16)
	for (int chunk = 0; chunk < chunk_count; chunk++)
	{
		count_chunk_soils(renderer, terrain, (chunk % terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE,
			(chunk / terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE);
	}
	{
		bool whole = blur_all_soils(renderer, terrain);

		for (int chunk = 0; chunk < chunk_count && !whole; chunk++)
		{
			blur_chunk_soils(renderer, terrain, (chunk % terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE,
				(chunk / terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE);
		}
	}
	for (int chunk = 0; chunk < chunk_count; chunk++)
	{
		renderer->chunk_needs_soil_blur[chunk] = false;
		terrain->chunks[chunk].needs_upload = false;
		terrain->chunks[chunk].needs_shape_upload = false;
		renderer->chunk_pending_upload[chunk] = false;
		renderer->chunk_pending_shape[chunk] = false;
		renderer->chunk_uploaded_seconds[chunk] = now_seconds;
		if (renderer->chunk_bare_ready != NULL)
		{
			renderer->chunk_bare_ready[chunk] = true;
		}
	}
	soils_milliseconds = 1000.0 * (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();
	renderer->uploaded_chunk_count += chunk_count;
	terrain->any_chunk_needs_upload = false;
	renderer->any_chunk_pending = false;
	SDL_Log("Terrain: a baked map's whole terrain up: heights and surface %.0f ms, the normals on the GPU, the soils %.0f ms", rows_milliseconds,
		soils_milliseconds);
	return true;
}

bool terrain_renderer_upload(terrain_renderer_t *renderer, terrain_t *terrain, water_t *water, const soil_settings_t *soil_settings,
	const camera_t *camera, const render_settings_t *settings, double now_seconds)
{
	int uploaded_map_size = renderer->texture_map_size;
	int uploaded_cells_per_texel = renderer->soil_cells_per_texel;
	int chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;
	uint64_t start_counter = SDL_GetPerformanceCounter();
	bool recreated;
	int next_chunk = 0;
	bool still_pending = false;
	bool from_gpu;
	frustum_t frustum = frustum_from_matrix(camera->view_projection);
	float lod_base = lod_base_distance(terrain, settings);
	double wait_seconds = (double)SDL_max(0.0f, settings->far_terrain_upload_seconds);

	int64_t chunks_at_start = renderer->uploaded_chunk_count;

	ensure_map_textures(renderer, terrain->size, terrain->cell_size_meters);
	recreated = renderer->texture_map_size != uploaded_map_size || renderer->soil_cells_per_texel != uploaded_cells_per_texel;
	// The heights, normals and surface made on the GPU from the ground the
	// simulation holds (water_ground.comp) once it can: here only the
	// wetness (the moisture is the CPU's) and the soils (edits)
	{
		water_ground_targets_t targets;

		SDL_zero(targets);
		targets.height_texture = renderer->height_texture;
		targets.normal_texture = renderer->normal_texture;
		targets.surface_texture = renderer->surface_texture;
		targets.normal_reach = renderer->material_normal_cells;
		targets.bare_units = 0.05f * (float)TERRAIN_HEIGHT_UNITS_PER_METER;
		targets.silt_visible_units = soil_settings->silt_visible_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER;
		targets.silt_full_units = SDL_max(soil_settings->silt_full_millimeters * 0.001f * (float)TERRAIN_HEIGHT_UNITS_PER_METER,
			targets.silt_visible_units + 1.0f);
		// (a baked map has no simulation: the CPU makes them, once)
		water_gpu_set_ground_targets(water, settings->terrain_from_gpu && !terrain->baked ? &targets : NULL);
	}
	from_gpu = water_gpu_ground_live(water);
	renderer->upload_frame_count++;
	{
		bool bounds_kept = update_chunk_bounds(renderer, terrain);

		for (int chunk = 0; chunk < chunk_count; chunk++)
		{
			renderer->chunk_far[chunk] = chunk_can_wait(terrain, camera, &frustum, SDL_max(lod_base, settings->far_upload_meters), chunk,
				bounds_kept ? renderer->chunk_bounds[2 * chunk] : terrain->chunks[chunk].minimum_height_meters,
				bounds_kept ? renderer->chunk_bounds[2 * chunk + 1] : terrain->chunks[chunk].maximum_height_meters);
		}
	}
	frame_profile_end(FRAME_TERRAIN_SETUP, start_counter);
	{
		uint64_t materials_start = SDL_GetPerformanceCounter();

		upload_materials(renderer, terrain, settings, recreated);
		frame_profile_end(FRAME_TERRAIN_MATERIALS, materials_start);
	}
	if (!terrain->any_chunk_needs_upload && !renderer->any_chunk_pending && !recreated)
	{
		return false;
	}
	if (recreated)
	{
		// Every count is new: the comparison in count_chunk_soils must
		// not find old values that happen to match
		SDL_memset(renderer->soil_box_counts, 0xff, (size_t)(terrain->size / renderer->soil_cells_per_texel)
			* (size_t)(terrain->size / renderer->soil_cells_per_texel) * 4 * sizeof(uint16_t));
	}
	if ((recreated || terrain->any_chunk_needs_upload) && upload_baked_whole(renderer, terrain, water, soil_settings, now_seconds))
	{
		renderer->upload_milliseconds_sum += 1000.0 * (double)(SDL_GetPerformanceCounter() - start_counter) / (double)SDL_GetPerformanceFrequency();
		return true;
	}

	// In batches: collect changed chunks, prepare them in parallel (the
	// CPU work: heights to floats, normals, surface state, soil counts),
	// then hand them to the GPU one after the other
	while (next_chunk < chunk_count)
	{
		struct terrain_upload_slot_s *slots = renderer->upload_slots;
		int slot_count = 0;

		for (; next_chunk < chunk_count && slot_count < TERRAIN_UPLOAD_BATCH; next_chunk++)
		{
			terrain_chunk_t *chunk = &terrain->chunks[next_chunk];

			// The terrain's flags are taken over here and cleared (the
			// objects have already followed them this frame)
			if (chunk->needs_upload)
			{
				renderer->chunk_pending_upload[next_chunk] = true;
				renderer->chunk_pending_shape[next_chunk] |= chunk->needs_shape_upload;
				chunk->needs_upload = false;
				chunk->needs_shape_upload = false;
			}
			if (!renderer->chunk_pending_upload[next_chunk] && !recreated)
			{
				continue;
			}
			// Far away it can wait: erosion under far water changes a chunk
			// every step, nobody sees it, and each upload costs
			if (!recreated && wait_seconds > 0.0 && now_seconds - renderer->chunk_uploaded_seconds[next_chunk] < wait_seconds
				&& renderer->chunk_far[next_chunk])
			{
				still_pending = true;
				renderer->deferred_chunk_count++;
				continue;
			}
			slots[slot_count].chunk_x = next_chunk % terrain->chunks_per_side;
			slots[slot_count].chunk_y = next_chunk / terrain->chunks_per_side;
			// Wetness changes alone leave heights, normals and soils as they were
			slots[slot_count].shape = renderer->chunk_pending_shape[next_chunk] || recreated;
			slot_count++;
			renderer->chunk_pending_upload[next_chunk] = false;
			renderer->chunk_pending_shape[next_chunk] = false;
			slots[slot_count - 1].since_seconds = (float)(now_seconds - renderer->chunk_uploaded_seconds[next_chunk]);
			renderer->chunk_uploaded_seconds[next_chunk] = now_seconds;
		}

		uint64_t part_start = SDL_GetPerformanceCounter();

		// (a few chunks on this thread alone: a chunk's wetness and soils
		// take a fraction of a millisecond, waking the other threads
		// often took four, the scheduler's tick)
#pragma omp parallel for schedule(dynamic, 1) num_threads(parallel_threads_for_items(slot_count, from_gpu ? TERRAIN_CHUNKS_PER_THREAD : 1))
		for (int i = 0; i < slot_count; i++)
		{
			struct terrain_upload_slot_s *slot = &slots[i];

			slot->soils_changed = false;
			slot->bare_settling = false;
			if (from_gpu)
			{
				fill_wetness(slot, terrain, water, soil_settings);
				if (slot->shape)
				{
					slot->soils_changed = count_chunk_soils(renderer, terrain, slot->chunk_x * TERRAIN_CHUNK_SIZE,
						slot->chunk_y * TERRAIN_CHUNK_SIZE);
				}
				continue;
			}
			{
				int chunk = slot->chunk_y * terrain->chunks_per_side + slot->chunk_x;
				bool ready = renderer->chunk_bare_ready != NULL && renderer->chunk_bare_ready[chunk];

				fill_surface(slot, terrain, water, soil_settings, renderer->bare_shown, ready);
				if (renderer->chunk_bare_ready != NULL)
				{
					renderer->chunk_bare_ready[chunk] = true;
				}
			}
			if (slot->shape)
			{
				fill_heights(slot, terrain);
				fill_normals(slot, terrain, renderer->material_normal_cells);
				slot->soils_changed = count_chunk_soils(renderer, terrain, slot->chunk_x * TERRAIN_CHUNK_SIZE,
					slot->chunk_y * TERRAIN_CHUNK_SIZE);
			}
		}

		frame_profile_end(FRAME_CHUNK_PREPARE, part_start);
		part_start = SDL_GetPerformanceCounter();
		for (int i = 0; i < slot_count; i++)
		{
			const struct terrain_upload_slot_s *slot = &slots[i];
			int start_x = slot->chunk_x * TERRAIN_CHUNK_SIZE;

			// (its bare rock still fading: up again, the surface alone)
			if (slot->bare_settling)
			{
				renderer->chunk_pending_upload[slot->chunk_y * terrain->chunks_per_side + slot->chunk_x] = true;
				still_pending = true;
			}
			int start_y = slot->chunk_y * TERRAIN_CHUNK_SIZE;

			// The wetness to the GPU's surface (it draws the tile again)
			water_gpu_ground_wetness(water, slot->chunk_y * terrain->chunks_per_side + slot->chunk_x, slot->wetness);
			if (!from_gpu)
			{
				gpu_upload_texture(renderer->surface_texture, 0, 0, (uint32_t)start_x, (uint32_t)start_y, TERRAIN_CHUNK_SIZE,
					TERRAIN_CHUNK_SIZE, slot->surface, sizeof(slot->surface));
			}
			if (slot->shape && !from_gpu)
			{
				gpu_upload_texture(renderer->height_texture, 0, 0, (uint32_t)start_x, (uint32_t)start_y, TERRAIN_CHUNK_SIZE,
					TERRAIN_CHUNK_SIZE, slot->heights, sizeof(slot->heights));
				gpu_upload_texture(renderer->normal_texture, 0, 0, (uint32_t)slot->normal_x, (uint32_t)slot->normal_y,
					(uint32_t)slot->normal_width, (uint32_t)slot->normal_height, slot->normals,
					(size_t)slot->normal_width * (size_t)slot->normal_height * 4);
			}
			if (slot->soils_changed)
			{
				// The blur reads the counts of neighboring chunks too, so it
				// waits until all of them are counted
				renderer->chunk_needs_soil_blur[slot->chunk_y * terrain->chunks_per_side + slot->chunk_x] = true;
			}
		}
		renderer->uploaded_chunk_count += slot_count;
		frame_profile_end(FRAME_CHUNK_SEND, part_start);
	}
	// Soil weights of the chunks whose soils changed
	{
		uint64_t blur_start = SDL_GetPerformanceCounter();

		for (int chunk = 0; chunk < chunk_count; chunk++)
		{
			if (renderer->chunk_needs_soil_blur[chunk])
			{
				blur_chunk_soils(renderer, terrain, (chunk % terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE,
					(chunk / terrain->chunks_per_side) * TERRAIN_CHUNK_SIZE);
				renderer->chunk_needs_soil_blur[chunk] = false;
			}
		}
		frame_profile_end(FRAME_SOIL_BLUR, blur_start);
	}
	terrain->any_chunk_needs_upload = false;
	renderer->any_chunk_pending = still_pending;
	{
		double milliseconds = 1000.0 * (double)(SDL_GetPerformanceCounter() - start_counter) / (double)SDL_GetPerformanceFrequency();
		int64_t made = renderer->uploaded_chunk_count - chunks_at_start;

		renderer->upload_milliseconds_sum += milliseconds;
		// (a whole map at once: a new one, a load)
		if (made >= 256)
		{
			SDL_Log("Terrain: %lld chunks made (%s) and sent up in %.0f ms%s", (long long)made, from_gpu ? "the surface's wetness on the CPU, the rest on the GPU"
				: "heights, normals, surface and soils on the CPU", milliseconds, still_pending ? ", more next frame" : "");
		}
	}
	return true;
}

vec3_t terrain_renderer_sun_direction(const render_settings_t *settings)
{
	float sun_azimuth = degrees_to_radians(settings->sun_azimuth_degrees);
	float sun_elevation = degrees_to_radians(settings->sun_elevation_degrees);

	return vec3_make(
		SDL_cosf(sun_elevation) * SDL_cosf(sun_azimuth),
		SDL_sinf(sun_elevation),
		SDL_cosf(sun_elevation) * SDL_sinf(sun_azimuth));
}

// By level of detail (each level one draw), then near to far within it
static int compare_visible_chunks(const void *left, const void *right)
{
	const struct visible_chunk_s *a = left;
	const struct visible_chunk_s *b = right;

	if (a->lod_level != b->lod_level)
	{
		return a->lod_level - b->lod_level;
	}
	return (a->distance_squared > b->distance_squared) - (a->distance_squared < b->distance_squared);
}

// Distance from which level k of detail starts: lod_base * 2^k. The base is
// raised to at least 2.5 chunk diagonals; below that, a chunk could border
// one two levels coarser, or one still morphing, and cracks would open.
static float lod_base_distance(const terrain_t *terrain, const render_settings_t *settings)
{
	float chunk_diagonal = (float)TERRAIN_CHUNK_SIZE * terrain->cell_size_meters * 1.41421356f;

	if (settings->lod_distance_meters <= 0.0f)
	{
		return 0.0f;
	}
	return SDL_max(settings->lod_distance_meters, 2.5f * chunk_diagonal);
}

// The height used for every level of detail distance this frame: the
// camera's height above the ground under it. A per frame constant (instead
// of each vertex's own height) keeps the distance of a vertex shared by two
// chunks the same in both, which is what keeps them crack free.
static float lod_eye_height(const terrain_t *terrain, const camera_t *camera)
{
	return SDL_max(camera->eye_position.y - terrain_sample_height_meters(terrain, camera->eye_position.x, camera->eye_position.z), 0.0f);
}

#define VISIBLE_BLOCK_CHUNKS 8

// The renderer's copy of the chunks' height ranges, and the blocks' (see
// terrain_renderer_t.chunk_bounds), made again if the terrain's moved;
// false: no memory, the chunks themselves are read
static bool update_chunk_bounds(terrain_renderer_t *renderer, const terrain_t *terrain)
{
	int chunks_per_side = terrain->chunks_per_side;
	int blocks_per_side = (chunks_per_side + VISIBLE_BLOCK_CHUNKS - 1) / VISIBLE_BLOCK_CHUNKS;

	if (renderer->bounds_chunks_per_side != chunks_per_side || renderer->chunk_bounds == NULL || renderer->block_bounds == NULL)
	{
		float *chunks = SDL_realloc(renderer->chunk_bounds, sizeof(float) * 2 * (size_t)SDL_max(chunks_per_side * chunks_per_side, 1));
		float *blocks;

		renderer->bounds_chunks_per_side = 0;
		if (chunks == NULL)
		{
			return false;
		}
		renderer->chunk_bounds = chunks;
		blocks = SDL_realloc(renderer->block_bounds, sizeof(float) * 2 * (size_t)SDL_max(blocks_per_side * blocks_per_side, 1));
		if (blocks == NULL)
		{
			return false;
		}
		renderer->block_bounds = blocks;
		renderer->bounds_chunks_per_side = chunks_per_side;
		renderer->bounds_terrain = NULL;
	}
	if (renderer->bounds_terrain == terrain->chunks && renderer->bounds_serial == terrain->chunk_bounds_serial)
	{
		return true;
	}
	for (int block = 0; block < blocks_per_side * blocks_per_side; block++)
	{
		renderer->block_bounds[2 * block] = 1e30f;
		renderer->block_bounds[2 * block + 1] = -1e30f;
	}
	for (int chunk_y = 0; chunk_y < chunks_per_side; chunk_y++)
	{
		for (int chunk_x = 0; chunk_x < chunks_per_side; chunk_x++)
		{
			const terrain_chunk_t *chunk = &terrain->chunks[chunk_y * chunks_per_side + chunk_x];
			float *block = &renderer->block_bounds[2 * ((chunk_y / VISIBLE_BLOCK_CHUNKS) * blocks_per_side + chunk_x / VISIBLE_BLOCK_CHUNKS)];

			renderer->chunk_bounds[2 * (chunk_y * chunks_per_side + chunk_x)] = chunk->minimum_height_meters;
			renderer->chunk_bounds[2 * (chunk_y * chunks_per_side + chunk_x) + 1] = chunk->maximum_height_meters;
			block[0] = SDL_min(block[0], chunk->minimum_height_meters);
			block[1] = SDL_max(block[1], chunk->maximum_height_meters);
		}
	}
	renderer->bounds_terrain = terrain->chunks;
	renderer->bounds_serial = terrain->chunk_bounds_serial;
	return true;
}
#define VISIBLE_ORDER_STEPS 256        // the visible chunks are put in order by distance this coarsely: a chunk's side a step

// The visible chunks by their level of detail, each level's near to far,
// by counting them per step of distance (a chunk's side: as near to front
// to back as the depth test needs) instead of sorting a thousand of them
// every frame; a plain sort if there is no memory for it
static void order_visible_chunks(terrain_renderer_t *renderer, float chunk_extent)
{
	static struct visible_chunk_s *ordered = NULL;
	static int ordered_capacity = 0;
	static uint32_t counts[TERRAIN_LOD_LEVEL_COUNT * VISIBLE_ORDER_STEPS + 1];
	int count = renderer->visible_chunk_count;

	if (ordered_capacity < count)
	{
		struct visible_chunk_s *grown = SDL_realloc(ordered, sizeof(renderer->visible_chunks[0]) * (size_t)count);

		if (grown == NULL)
		{
			SDL_qsort(renderer->visible_chunks, (size_t)count, sizeof(renderer->visible_chunks[0]), compare_visible_chunks);
			return;
		}
		ordered = grown;
		ordered_capacity = count;
	}
	SDL_memset(counts, 0, sizeof(counts));
	for (int pass = 0; pass < 2; pass++)
	{
		for (int k = 0; k < count; k++)
		{
			const struct visible_chunk_s *entry = &renderer->visible_chunks[k];
			int step = SDL_clamp((int)(SDL_sqrtf(entry->distance_squared) / chunk_extent), 0, VISIBLE_ORDER_STEPS - 1);
			int key = SDL_clamp(entry->lod_level, 0, TERRAIN_LOD_LEVEL_COUNT - 1) * VISIBLE_ORDER_STEPS + step;

			if (pass == 0)
			{
				counts[key + 1]++;
			}
			else
			{
				ordered[counts[key]++] = *entry;
			}
		}
		for (int key = 0; pass == 0 && key < TERRAIN_LOD_LEVEL_COUNT * VISIBLE_ORDER_STEPS; key++)
		{
			counts[key + 1] += counts[key];
		}
	}
	SDL_memcpy(renderer->visible_chunks, ordered, sizeof(renderer->visible_chunks[0]) * (size_t)count);
}

static void collect_visible_chunks(terrain_renderer_t *renderer, const terrain_t *terrain, const camera_t *camera,
	const render_settings_t *settings)
{
	frustum_t frustum = frustum_from_matrix(camera->view_projection);
	float chunk_extent = (float)TERRAIN_CHUNK_SIZE * terrain->cell_size_meters;
	float lod_base = lod_base_distance(terrain, settings);
	float eye_height = lod_eye_height(terrain, camera);

	renderer->visible_chunk_count = 0;
	// By blocks of VISIBLE_BLOCK_CHUNKS chunks a side first: one out of
	// view is thrown out whole, its chunks not tried one by one (on a big
	// map most of them)
	bool bounds_kept = update_chunk_bounds(renderer, terrain);
	int blocks_per_side = (terrain->chunks_per_side + VISIBLE_BLOCK_CHUNKS - 1) / VISIBLE_BLOCK_CHUNKS;

	for (int block_y = 0; block_y < terrain->chunks_per_side; block_y += VISIBLE_BLOCK_CHUNKS)
	{
	for (int block_x = 0; block_x < terrain->chunks_per_side; block_x += VISIBLE_BLOCK_CHUNKS)
	{
	int block_last_x = SDL_min(block_x + VISIBLE_BLOCK_CHUNKS, terrain->chunks_per_side) - 1;
	int block_last_y = SDL_min(block_y + VISIBLE_BLOCK_CHUNKS, terrain->chunks_per_side) - 1;

	if (bounds_kept)
	{
		const float *block = &renderer->block_bounds[2 * ((block_y / VISIBLE_BLOCK_CHUNKS) * blocks_per_side + block_x / VISIBLE_BLOCK_CHUNKS)];

		if (!frustum_contains_box(&frustum, vec3_make((float)block_x * chunk_extent, block[0], (float)block_y * chunk_extent),
			vec3_make((float)(block_last_x + 1) * chunk_extent, block[1], (float)(block_last_y + 1) * chunk_extent)))
		{
			continue;
		}
	}
	for (int chunk_y = block_y; chunk_y <= block_last_y; chunk_y++)
	{
		for (int chunk_x = block_x; chunk_x <= block_last_x; chunk_x++)
		{
			int chunk_index = chunk_y * terrain->chunks_per_side + chunk_x;
			float lowest = bounds_kept ? renderer->chunk_bounds[2 * chunk_index] : terrain->chunks[chunk_index].minimum_height_meters;
			float highest = bounds_kept ? renderer->chunk_bounds[2 * chunk_index + 1] : terrain->chunks[chunk_index].maximum_height_meters;
			vec3_t box_minimum = vec3_make((float)chunk_x * chunk_extent, lowest, (float)chunk_y * chunk_extent);
			vec3_t box_maximum = vec3_make(box_minimum.x + chunk_extent, highest, box_minimum.z + chunk_extent);
			vec3_t center;
			vec3_t offset;
			struct visible_chunk_s *entry;
			int level = 0;

			if (!frustum_contains_box(&frustum, box_minimum, box_maximum))
			{
				continue;
			}
			center = vec3_scale(vec3_add(box_minimum, box_maximum), 0.5f);
			offset = vec3_subtract(center, camera->eye_position);
			if (lod_base > 0.0f)
			{
				// Nearest point of the chunk, measured the way terrain.vert does
				float outside_x = SDL_max(SDL_max(box_minimum.x - camera->eye_position.x, camera->eye_position.x - box_maximum.x), 0.0f);
				float outside_z = SDL_max(SDL_max(box_minimum.z - camera->eye_position.z, camera->eye_position.z - box_maximum.z), 0.0f);
				float nearest = SDL_sqrtf(outside_x * outside_x + outside_z * outside_z + eye_height * eye_height);
				float threshold = lod_base;

				while (level < TERRAIN_LOD_LEVEL_COUNT - 1 && nearest >= threshold)
				{
					level++;
					threshold *= 2.0f;
				}
			}
			entry = &renderer->visible_chunks[renderer->visible_chunk_count++];
			entry->chunk_x = chunk_x;
			entry->chunk_y = chunk_y;
			entry->distance_squared = vec3_dot(offset, offset);
			entry->lod_level = level;
		}
	}
	}
	}
	// Front to back: near chunks fill the depth buffer first, so pixels of
	// far chunks hidden behind hills are rejected before shading (the near
	// levels first, each near to far)
	order_visible_chunks(renderer, chunk_extent);
	frame_profile_count(FRAME_TERRAIN_CHUNKS_DRAWN, renderer->visible_chunk_count);
}

void terrain_renderer_cloud_uniforms(const render_settings_t *settings, float time_seconds, float parameters_out[4], float motion_out[3])
{
	float tiling = SDL_max(settings->cloud_tiling_meters, 1.0f);
	float wind_radians = degrees_to_radians(settings->wind_direction_degrees);
	// The offset wraps to stay small and precise however long the game
	// runs. The cloud field (three turned layers) and the volumetric noise
	// never quite repeat, so a wrap is a jump: it comes only every few
	// hundred kilometers of wind (a multiple of the volumetric clouds' map,
	// CLOUD_WEATHER_METERS, which wraps there unseen)
	float travelled = (float)SDL_fmod((double)time_seconds * (double)settings->wind_speed_meters_per_second, 200000.0);

	parameters_out[0] = clamp_float(settings->cloud_coverage, 0.0f, 1.0f);
	parameters_out[1] = SDL_max(settings->cloud_softness, 0.001f);
	parameters_out[2] = clamp_float(settings->cloud_shadow_strength, 0.0f, 1.0f);
	parameters_out[3] = 1.0f / tiling;
	motion_out[0] = SDL_cosf(wind_radians) * travelled;
	motion_out[1] = SDL_sinf(wind_radians) * travelled;
	motion_out[2] = SDL_max(settings->cloud_height_meters, CLOUD_MINIMUM_HEIGHT_METERS);
}

void terrain_renderer_prepare_draw(terrain_renderer_t *renderer, const terrain_t *terrain, const camera_t *camera,
	const render_settings_t *settings)
{
	renderer->instances_ready = false;
	SDL_zeroa(renderer->level_first);
	SDL_zeroa(renderer->level_count);
	if (renderer->visible_chunks == NULL)
	{
		return;
	}
	collect_visible_chunks(renderer, terrain, camera, settings);
	if (renderer->instance_capacity < renderer->visible_chunk_capacity)
	{
		uint32_t bytes = (uint32_t)(sizeof(int32_t) * 2 * (size_t)renderer->visible_chunk_capacity);
		int32_t *staging = SDL_realloc(renderer->instance_staging, bytes);

		if (staging == NULL)
		{
			return;
		}
		renderer->instance_staging = staging;
		if (renderer->instance_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, renderer->instance_buffer);
		}
		renderer->instance_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, bytes);
		if (renderer->instance_buffer == NULL)
		{
			renderer->instance_capacity = 0;
			return;
		}
		renderer->instance_capacity = renderer->visible_chunk_capacity;
	}
	// (the full detail chunks with ground hidden under buildings after the
	// rest: each drawn on its own, its own indices)
	renderer->holed_count = 0;
	{
		int placed = 0;

		for (int pass = 0; pass < 2; pass++)
		{
			for (int i = 0; i < renderer->visible_chunk_count; i++)
			{
				const struct visible_chunk_s *entry = &renderer->visible_chunks[i];
				int chunk_index = entry->chunk_y * terrain->chunks_per_side + entry->chunk_x;
				bool holed = entry->lod_level == 0 && renderer->hole_index_buffer != NULL && chunk_index < renderer->hole_chunk_slots
					&& renderer->hole_index_counts[chunk_index] > 0;

				if (holed != (pass == 1))
				{
					continue;
				}
				renderer->instance_staging[2 * placed] = entry->chunk_x * TERRAIN_CHUNK_SIZE;
				renderer->instance_staging[2 * placed + 1] = entry->chunk_y * TERRAIN_CHUNK_SIZE;
				if (holed)
				{
					renderer->holed_instances[renderer->holed_count] = placed;
					renderer->holed_chunks[renderer->holed_count] = chunk_index;
					renderer->holed_count++;
				}
				else
				{
					if (renderer->level_count[entry->lod_level] == 0)
					{
						renderer->level_first[entry->lod_level] = placed;
					}
					renderer->level_count[entry->lod_level]++;
				}
				placed++;
			}
		}
	}
	if (renderer->visible_chunk_count > 0)
	{
		gpu_upload_buffer(renderer->instance_buffer, 0, renderer->instance_staging, sizeof(int32_t) * 2 * (size_t)renderer->visible_chunk_count);
	}
	renderer->instances_ready = true;
}

void terrain_renderer_draw(terrain_renderer_t *renderer, SDL_GPURenderPass *pass, const terrain_t *terrain, const camera_t *camera,
	const render_settings_t *settings, const brush_outline_t *brush, const shadow_map_t *shadow, const shadow_map_t *near_shadow)
{
	const material_set_t *materials = &renderer->materials;
	float lod_base = lod_base_distance(terrain, settings);
	terrain_fragment_uniforms_t fragment;
	terrain_chunk_uniforms_t chunk;
	SDL_GPUBufferBinding vertex_binding = { renderer->vertex_buffer, 0 };
	SDL_GPUBufferBinding index_binding = { renderer->index_buffer, 0 };
	SDL_GPUTextureSamplerBinding height_binding = { renderer->height_texture, gpu.nearest_clamp };
	SDL_GPUTexture *cloud_shadows = clouds_shadow_texture();
	SDL_GPUTextureSamplerBinding fragment_textures[14] =
	{
		{ materials->variation_texture, gpu.linear_repeat },
		{ renderer->soil_weight_texture, gpu.linear_clamp },
		{ renderer->normal_texture, gpu.linear_clamp },
		{ materials->color_height_array, gpu.linear_repeat },
		{ materials->normal_array, gpu.linear_repeat },
		{ renderer->surface_texture, gpu.linear_clamp },
		{ renderer->tile_texture != NULL ? renderer->tile_texture : renderer->empty_integer_texture, gpu.nearest_clamp },
		{ renderer->navigation_texture != NULL ? renderer->navigation_texture : renderer->empty_integer_texture, gpu.nearest_clamp },
		// (without the volumetric clouds' shadows, something: not read then)
		{ cloud_shadows != NULL ? cloud_shadows : materials->variation_texture, gpu.linear_repeat },
		{ renderer->material_texture, gpu.nearest_clamp },
		// (none yet: nothing on the ground)
		{ renderer->overlay_texture != NULL ? renderer->overlay_texture : renderer->empty_integer_texture, gpu.linear_clamp },
		// (none: not read then)
		{ renderer->sky_occlusion_texture != NULL ? renderer->sky_occlusion_texture : renderer->empty_integer_texture, gpu.linear_clamp },
		{ renderer->sky_layers_texture != NULL ? renderer->sky_layers_texture : renderer->empty_integer_texture, gpu.linear_clamp },
		{ renderer->sky_bounce_texture != NULL ? renderer->sky_bounce_texture : renderer->empty_integer_texture, gpu.linear_clamp },
	};

	if (pass == NULL || renderer->height_texture == NULL || !renderer->instances_ready)
	{
		return;
	}

	SDL_zero(fragment);
	for (int i = 0; i < MATERIAL_COUNT; i++)
	{
		fragment.material_layers[i][0] = materials->first_layer[i];
		fragment.material_layers[i][1] = materials->variant_count[i];
		fragment.material_layers[i][2] = materials->macro_layer[i];
		fragment.material_layers[i][3] = materials->relief_layer[i];
		fragment.material_scales[i][0] = materials->repeats_per_meter[i];
		fragment.material_scales[i][1] = materials->macro_repeats_per_meter[i];
		fragment.material_scales[i][2] = (float)materials->wet_layer[i];
		fragment.material_patches[i][0] = materials->patch_repeats_per_meter[i];
		fragment.material_patches[i][1] = materials->patch_strength[i];
		fragment.material_patches[i][2] = materials->patch_edge_meters[i];
		fragment.material_patches[i][3] = 1.0f / SDL_max(materials->relief_meters[i], 0.5f);
		// (the same every pixel: worked out here once, not read from the
		// smallest level per pixel)
		if (materials->macro_layer[i] >= 0 && materials->macro_layer[i] < MATERIAL_MAX_LAYERS)
		{
			const uint8_t *average = materials->layer_average[materials->macro_layer[i]];

			for (int c = 0; c < 3; c++)
			{
				fragment.macro_averages[i][c] = (float)average[c] / 255.0f;
			}
			fragment.macro_averages[i][3] = fragment.macro_averages[i][0] * 0.3f + fragment.macro_averages[i][1] * 0.59f
				+ fragment.macro_averages[i][2] * 0.11f;
		}
	}
	if (materials->first_layer[MATERIAL_GRASS] >= 0 && materials->first_layer[MATERIAL_GRASS] < MATERIAL_MAX_LAYERS)
	{
		const uint8_t *average = materials->layer_average[materials->first_layer[MATERIAL_GRASS]];

		for (int c = 0; c < 3; c++)
		{
			fragment.grass_green[c] = SDL_powf((float)average[c] / 255.0f, 2.2f) * 0.8f;
		}
	}
	fragment.grass_green[3] = settings->puddles ? 1.0f : 0.0f;
	fragment.brush_line[0] = brush->start_x;
	fragment.brush_line[1] = brush->start_z;
	fragment.brush_line[2] = brush->end_x;
	fragment.brush_line[3] = brush->end_z;
	fragment.brush_radius = brush->radius;
	fragment.slope_ranges[0] = settings->grass_slope_start;
	fragment.slope_ranges[1] = SDL_max(settings->grass_slope_end, settings->grass_slope_start + 0.01f);
	fragment.slope_ranges[2] = settings->cliff_slope_start;
	fragment.slope_ranges[3] = SDL_max(settings->cliff_slope_end, settings->cliff_slope_start + 0.01f);
	fragment.variation_parameters[0] = 1.0f / SDL_max(settings->variation_tiling_meters, 1.0f);
	fragment.variation_parameters[1] = settings->variation_strength;
	fragment.variation_parameters[2] = settings->variation_hue_strength;
	fragment.detail_fade[0] = settings->detail_fade_start_meters;
	fragment.detail_fade[1] = SDL_max(settings->detail_fade_end_meters, settings->detail_fade_start_meters + 1.0f);
	fragment.navigation_cell_meters[0] = renderer->navigation_cell_meters;
	fragment.navigation_cell_meters[1] = (float)renderer->navigation_texture_size;
	fragment.map_size = terrain->size;
	fragment.cell_size = terrain->cell_size_meters;
	fragment.max_height = terrain->max_height_meters;
	fragment.snow_height_fraction = settings->snow_height_fraction;
	fragment.snow_aspect_shift = settings->snow_aspect_shift;
	fragment.soil_blend_cells = settings->soil_blend_meters / terrain->cell_size_meters;
	fragment.debug_view = (int32_t)renderer->debug_view;
	fragment.tile_overlay = renderer->tile_overlay ? 1 : 0;
	fragment.navigation_overlay = renderer->navigation_overlay && renderer->navigation_texture != NULL ? 1 : 0;
	fragment.material_map = settings->material_map ? 1 : 0;

	{
		bool map = settings->material_map && renderer->map_pipeline != NULL;
		SDL_GPUGraphicsPipeline *solid = map ? renderer->map_pipeline : renderer->pipeline;
		SDL_GPUGraphicsPipeline *lines = map ? renderer->map_wireframe_pipeline : renderer->wireframe_pipeline;

		SDL_BindGPUGraphicsPipeline(pass, renderer->wireframe && lines != NULL ? lines : solid);
	}
	lights_bind(pass);
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &fragment, sizeof(fragment));
	SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
	SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
	SDL_BindGPUVertexSamplers(pass, 0, &height_binding, 1);
	shadow_map_bind_for_shading(pass, shadow, near_shadow);
	SDL_BindGPUFragmentSamplers(pass, 2, fragment_textures, 14);

	SDL_zero(chunk);
	chunk.map_size = terrain->size;
	chunk.cell_size = terrain->cell_size_meters;
	chunk.lod_eye[0] = camera->eye_position.x;
	chunk.lod_eye[1] = lod_eye_height(terrain, camera);
	chunk.lod_eye[2] = camera->eye_position.z;
	SDL_zeroa(renderer->chunks_per_lod_level);
	renderer->triangles_drawn = 0;
	chunk.chunk_origin[0] = 0;
	chunk.chunk_origin[1] = 0;
	// One draw per level of detail: its chunks' origins a run of the
	// instance buffer (terrain_renderer_prepare_draw)
	for (int level = 0; level < TERRAIN_LOD_LEVEL_COUNT; level++)
	{
		int count = renderer->level_count[level];
		// Vertices dropped by the next level slide onto it over the last 30%
		// before the next level's distance; the coarsest level never morphs
		float morph_end = lod_base * (float)(1 << level);
		float morph_start = 0.7f * morph_end;
		bool morphs = lod_base > 0.0f && level < TERRAIN_LOD_LEVEL_COUNT - 1;
		SDL_GPUBufferBinding instance_binding = { renderer->instance_buffer, (uint32_t)(sizeof(int32_t) * 2 * (size_t)renderer->level_first[level]) };

		if (count == 0)
		{
			continue;
		}
		chunk.lod_step = 1 << level;
		chunk.lod_morph_range[0] = morphs ? morph_start : 1e30f;
		chunk.lod_morph_range[1] = morphs ? 1.0f / (morph_end - morph_start) : 0.0f;
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &chunk, sizeof(chunk));
		SDL_BindGPUVertexBuffers(pass, 1, &instance_binding, 1);
		SDL_DrawGPUIndexedPrimitives(pass, (uint32_t)renderer->lod_index_count[level], (uint32_t)count, (uint32_t)renderer->lod_index_offset[level], 0, 0);
		renderer->chunks_per_lod_level[level] += count;
		renderer->triangles_drawn += count * renderer->lod_index_count[level] / 3;
	}
	// The full detail chunks with ground hidden under buildings: one draw
	// each, its quads but the hidden ones
	if (renderer->holed_count > 0 && renderer->hole_index_buffer != NULL)
	{
		SDL_GPUBufferBinding hole_binding = { renderer->hole_index_buffer, 0 };
		float morph_end = lod_base;
		bool morphs = lod_base > 0.0f && TERRAIN_LOD_LEVEL_COUNT > 1;

		chunk.lod_step = 1;
		chunk.lod_morph_range[0] = morphs ? 0.7f * morph_end : 1e30f;
		chunk.lod_morph_range[1] = morphs ? 1.0f / (morph_end - 0.7f * morph_end) : 0.0f;
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &chunk, sizeof(chunk));
		SDL_BindGPUIndexBuffer(pass, &hole_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		for (int i = 0; i < renderer->holed_count; i++)
		{
			int chunk_index = renderer->holed_chunks[i];
			SDL_GPUBufferBinding instance_binding = { renderer->instance_buffer, (uint32_t)(sizeof(int32_t) * 2 * (size_t)renderer->holed_instances[i]) };

			SDL_BindGPUVertexBuffers(pass, 1, &instance_binding, 1);
			SDL_DrawGPUIndexedPrimitives(pass, (uint32_t)renderer->hole_index_counts[chunk_index], 1, (uint32_t)renderer->hole_index_offsets[chunk_index], 0, 0);
			renderer->chunks_per_lod_level[0]++;
			renderer->triangles_drawn += renderer->hole_index_counts[chunk_index] / 3;
		}
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
	}
	renderer->instances_ready = false;
}
