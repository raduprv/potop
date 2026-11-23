// Impostors' atlases: baked, cached, loaded. See impostors.h.

#include "impostors.h"
#include "file_io.h"
#include "screenshot.h"
#include "season.h"

// objects.c's PLANT_AUTUMN_SPREAD: how far apart the trees are in their autumn
#define IMPOSTOR_PLANT_AUTUMN_SPREAD 0.12f
// Raise when a plant's pictures are made differently: only theirs are baked again
#define IMPOSTOR_PLANT_BAKE_VERSION 4

#include <SDL3/SDL.h>

#include "miniz.h"

// The largest atlas's bytes (the baker's targets and download)
#define ATLAS_BYTES ((size_t)IMPOSTOR_BAKE_WIDTH * IMPOSTOR_BAKE_HEIGHT * 4)

// How a model's pictures are laid out: a plant's few, larger; the rest's
// grid round the hemisphere
typedef struct
{
	bool plant;
	bool thumbnail;                // one picture from a three quarter view (impostor_make_thumbnail)
	int month_index;               // a plant's (0 January), its foliage's pictures of that month; -1 none
	float month;                   // ... and the month itself (season.h: 1 .. 13), for its autumn
	int grid_x;                    // pictures across, down
	int grid_y;
	int cell_pixels;
	int atlas_width;
	int atlas_height;
	size_t atlas_bytes;
} picture_layout_t;

static picture_layout_t layout_for(const object_model_t *model, float month)
{
	picture_layout_t layout;

	layout.plant = model->plant_buffer != NULL;
	layout.thumbnail = false;
	layout.month_index = layout.plant ? season_month_index(month) : -1;
	layout.month = month;
	layout.grid_x = layout.plant ? IMPOSTOR_PLANT_AZIMUTHS : IMPOSTOR_GRID;
	layout.grid_y = layout.plant ? IMPOSTOR_PLANT_ELEVATIONS : IMPOSTOR_GRID;
	layout.cell_pixels = layout.plant ? IMPOSTOR_PLANT_CELL_PIXELS : IMPOSTOR_CELL_PIXELS;
	layout.atlas_width = layout.grid_x * layout.cell_pixels;
	layout.atlas_height = layout.grid_y * layout.cell_pixels;
	layout.atlas_bytes = (size_t)layout.atlas_width * (size_t)layout.atlas_height * 4;
	return layout;
}

// The cache file: this header, then the two atlases (color, then normal
// and depth), compressed together
typedef struct
{
	char magic[4];                 // "PIMP"
	uint32_t version;
	uint32_t grid;
	uint32_t cell_pixels;
	uint64_t key;
	float sphere[4];
	uint32_t compressed_bytes;
	uint32_t raw_bytes;
} impostor_file_header_t;

#define PLANT_MONTH_CELLS 32           // plant.glsl's

typedef struct
{
	int32_t frame[4];
	float sphere[4];
	float season[4];               // a plant's only, from here (impostor_bake.glsl)
	float month_offsets[PLANT_MONTH_CELLS][4];
} bake_vertex_uniforms_t;

typedef struct
{
	int32_t alpha_tested;
	int32_t lighting_normals;
	int32_t mode;
	int32_t plant;
	float foliage[4];
	float autumn_color_a[4];
	float autumn_color_b[4];
} bake_fragment_uniforms_t;

static const SDL_GPUVertexBufferDescription bake_vertex_buffers[2] =
{
	{ 0, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },  // position, normal, uv
	{ 1, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },  // tangent
};
static const SDL_GPUVertexAttribute bake_vertex_attributes[4] =
{
	{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
	{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 },
	{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 },
	{ 3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
};

// A plant's: its plant buffer too (OBJECT_PLANT_VERTEX_FLOATS a vertex)
static const SDL_GPUVertexBufferDescription plant_bake_vertex_buffers[3] =
{
	{ 0, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 1, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 2, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },  // plant data, then its snow (w: its month cell)
};
static const SDL_GPUVertexAttribute plant_bake_vertex_attributes[6] =
{
	{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
	{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 },
	{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 },
	{ 3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 4, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 5, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
};

bool impostor_baker_create(impostor_baker_t *baker)
{
	SDL_GPUShader *vertex;
	SDL_GPUShader *fragment;
	SDL_GPUTransferBufferCreateInfo download_info;

	SDL_zerop(baker);
	if (gpu.device == NULL)
	{
		return false;
	}
	vertex = gpu_load_shader("impostor_bake.vert", 0, 0, 1);
	fragment = gpu_load_shader("impostor_bake.frag", 1, 0, 1);
	if (vertex != NULL && fragment != NULL)
	{
		gpu_pipeline_info_t info;

		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = bake_vertex_buffers;
		info.vertex_buffer_count = 2;
		info.vertex_attributes = bake_vertex_attributes;
		info.vertex_attribute_count = 4;
		info.cull = SDL_GPU_CULLMODE_NONE;
		info.depth_test = true;
		info.depth_write = true;
		info.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		baker->bake_pipeline = gpu_make_pipeline(&info, "impostor baking");
		{
			SDL_GPUShader *plant_vertex = gpu_load_shader("impostor_bake_plant.vert", 0, 0, 1);

			if (plant_vertex != NULL)
			{
				info.vertex_shader = plant_vertex;
				info.vertex_buffers = plant_bake_vertex_buffers;
				info.vertex_buffer_count = 3;
				info.vertex_attributes = plant_bake_vertex_attributes;
				info.vertex_attribute_count = 6;
				baker->plant_bake_pipeline = gpu_make_pipeline(&info, "impostor baking (plants)");
				SDL_ReleaseGPUShader(gpu.device, plant_vertex);
			}
		}
	}
	if (vertex != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, vertex);
	}
	if (fragment != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, fragment);
	}
	baker->color_target = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, IMPOSTOR_BAKE_WIDTH,
		IMPOSTOR_BAKE_HEIGHT, 1, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
	baker->depth_target = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.depth_format, IMPOSTOR_BAKE_WIDTH, IMPOSTOR_BAKE_HEIGHT, 1, 1,
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
	SDL_zero(download_info);
	download_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	download_info.size = (uint32_t)(ATLAS_BYTES * 2);
	baker->download = SDL_CreateGPUTransferBuffer(gpu.device, &download_info);
	if (baker->bake_pipeline == NULL || baker->plant_bake_pipeline == NULL || baker->color_target == NULL || baker->depth_target == NULL
		|| baker->download == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Impostors: cannot bake (%s); far objects are drawn whole", SDL_GetError());
		impostor_baker_destroy(baker);
		return false;
	}
	return true;
}

void impostor_baker_destroy(impostor_baker_t *baker)
{
	if (gpu.device != NULL)
	{
		if (baker->bake_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, baker->bake_pipeline);
		}
		if (baker->plant_bake_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, baker->plant_bake_pipeline);
		}
		if (baker->color_target != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, baker->color_target);
		}
		if (baker->depth_target != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, baker->depth_target);
		}
		if (baker->download != NULL)
		{
			SDL_ReleaseGPUTransferBuffer(gpu.device, baker->download);
		}
	}
	SDL_zerop(baker);
}

// The model's bounding sphere: the center of its box, and the farthest
// vertex from it
static void bounding_sphere(const object_model_t *model, float sphere_out[4])
{
	float minimum[3] = { model->bounds_minimum.x, model->bounds_minimum.y, model->bounds_minimum.z };
	float maximum[3] = { model->bounds_maximum.x, model->bounds_maximum.y, model->bounds_maximum.z };
	float farthest = 0.0f;

	if (model->collision_positions != NULL && model->collision_vertex_count > 0)
	{
		for (int k = 0; k < 3; k++)
		{
			minimum[k] = 1e30f;
			maximum[k] = -1e30f;
		}
		for (int i = 0; i < model->collision_vertex_count; i++)
		{
			for (int k = 0; k < 3; k++)
			{
				minimum[k] = SDL_min(minimum[k], model->collision_positions[3 * i + k]);
				maximum[k] = SDL_max(maximum[k], model->collision_positions[3 * i + k]);
			}
		}
	}
	for (int k = 0; k < 3; k++)
	{
		sphere_out[k] = 0.5f * (minimum[k] + maximum[k]);
	}
	if (model->collision_positions != NULL && model->collision_vertex_count > 0)
	{
		for (int i = 0; i < model->collision_vertex_count; i++)
		{
			float dx = model->collision_positions[3 * i] - sphere_out[0];
			float dy = model->collision_positions[3 * i + 1] - sphere_out[1];
			float dz = model->collision_positions[3 * i + 2] - sphere_out[2];

			farthest = SDL_max(farthest, dx * dx + dy * dy + dz * dz);
		}
		farthest = SDL_sqrtf(farthest);
	}
	else
	{
		float dx = maximum[0] - minimum[0];
		float dy = maximum[1] - minimum[1];
		float dz = maximum[2] - minimum[2];

		farthest = 0.5f * SDL_sqrtf(dx * dx + dy * dy + dz * dz);
	}
	// (all it draws inside too: a plant's hull is only its stems, its
	// foliage reaching past them, a sprawling shrub's far past; the
	// pictures were cut at the hull's sphere. Its drawn box's corners, the
	// farthest a vertex of it can be)
	for (int corner = 0; corner < 8; corner++)
	{
		float x = (corner & 1) ? model->bounds_maximum.x : model->bounds_minimum.x;
		float y = (corner & 2) ? model->bounds_maximum.y : model->bounds_minimum.y;
		float z = (corner & 4) ? model->bounds_maximum.z : model->bounds_minimum.z;
		float dx = x - sphere_out[0];
		float dy = y - sphere_out[1];
		float dz = z - sphere_out[2];
		float distance = SDL_sqrtf(dx * dx + dy * dy + dz * dz);

		// (only when the drawn box is well past the hull: a corner of a box
		// round a round crown is farther than anything in it)
		if (distance > farthest * 1.15f)
		{
			farthest = SDL_max(farthest, distance * 0.92f);
		}
	}
	sphere_out[3] = SDL_max(farthest, 0.01f);
}

bool impostor_worth_one(const object_model_t *model)
{
	float sphere[4];
	int triangles = 0;

	for (int k = 0; k < model->material_count; k++)
	{
		triangles += model->materials[k].index_count / 3;
		// (the impostors' bake knows texture coordinates only: a building's
		// triplanar stone has them too, near enough from afar; a prop's
		// rocks have none)
		if (model->materials[k].triplanar && model->weathering_buffer == NULL)
		{
			return false;
		}
	}
	if (triangles < IMPOSTOR_MINIMUM_TRIANGLES || model->vertex_buffer == NULL || model->tangent_buffer == NULL)
	{
		return false;
	}
	bounding_sphere(model, sphere);
	return sphere[3] >= IMPOSTOR_MINIMUM_RADIUS_METERS;
}

// ---------------------------------------------------------------------------
// The cache

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
	const uint8_t *bytes = data;

	for (size_t i = 0; i < size; i++)
	{
		hash ^= bytes[i];
		hash *= 0x100000001B3ull;
	}
	return hash;
}

// What the atlas is made from: the files (and when they were written), the
// materials' flags, and how the pictures are taken
static uint64_t cache_key(const object_model_t *model, const object_texture_cache_t *textures, const picture_layout_t *layout)
{
	uint64_t hash = 0xCBF29CE484222325ull;
	int32_t how[6] = { IMPOSTOR_VERSION, layout->grid_x, layout->grid_y, layout->cell_pixels, layout->plant ? IMPOSTOR_PLANT_BAKE_VERSION : 0, layout->month_index };

	hash = hash_bytes(hash, how, sizeof(how));
	// (the pictures are framed by the model's sphere: a new one, new pictures)
	{
		float sphere[4];

		bounding_sphere(model, sphere);
		hash = hash_bytes(hash, sphere, sizeof(sphere));
	}
	hash = hash_bytes(hash, model->file_path, SDL_strlen(model->file_path));
	hash = hash_bytes(hash, &model->file_time, sizeof(model->file_time));
	for (int k = 0; k < model->material_count; k++)
	{
		const object_material_t *material = &model->materials[k];
		uint8_t flags[2] = { material->alpha_tested ? 1 : 0, material->lighting_normals ? 1 : 0 };
		int ranges[2] = { material->index_offset, material->index_count };

		hash = hash_bytes(hash, flags, sizeof(flags));
		hash = hash_bytes(hash, ranges, sizeof(ranges));
		for (int e = 0; e < textures->count; e++)
		{
			const object_texture_entry_t *entry = &textures->entries[e];

			if (entry->texture == material->texture)
			{
				hash = hash_bytes(hash, entry->path, SDL_strlen(entry->path));
				hash = hash_bytes(hash, &entry->file_time, sizeof(entry->file_time));
				break;
			}
		}
	}
	return hash;
}

// data/cache/impostors/<the model's name, slashes made underscores>.imp
static void cache_path(const object_model_t *model, char *path_out, size_t path_out_size)
{
	char relative[OBJECT_MODEL_NAME_LENGTH + 64];
	char name[OBJECT_MODEL_NAME_LENGTH];

	SDL_strlcpy(name, model->name, sizeof(name));
	for (char *c = name; *c != '\0'; c++)
	{
		if (*c == '/' || *c == '\\' || *c == ':')
		{
			*c = '_';
		}
	}
	SDL_snprintf(relative, sizeof(relative), "cache/impostors/%s.imp", name);
	file_io_data_path(relative, path_out, path_out_size);
}

static bool cache_load(const char *path, uint64_t key, const picture_layout_t *layout, uint8_t *atlases_out, float sphere_out[4])
{
	size_t size = 0;
	uint8_t *file = file_io_load_file(path, &size);
	impostor_file_header_t header;
	mz_ulong raw_bytes = (mz_ulong)(layout->atlas_bytes * 2);
	bool ok = false;

	if (file == NULL)
	{
		return false;
	}
	if (size >= sizeof(header))
	{
		SDL_memcpy(&header, file, sizeof(header));
		ok = SDL_memcmp(header.magic, "PIMP", 4) == 0 && header.version == IMPOSTOR_VERSION && header.grid == (uint32_t)(layout->grid_x + 256 * layout->grid_y)
			&& header.cell_pixels == (uint32_t)layout->cell_pixels && header.key == key && header.raw_bytes == layout->atlas_bytes * 2
			&& sizeof(header) + header.compressed_bytes <= size
			&& mz_uncompress(atlases_out, &raw_bytes, file + sizeof(header), header.compressed_bytes) == MZ_OK
			&& raw_bytes == layout->atlas_bytes * 2;
		if (ok)
		{
			SDL_memcpy(sphere_out, header.sphere, sizeof(header.sphere));
		}
	}
	SDL_free(file);
	return ok;
}

static void cache_save(const char *path, uint64_t key, const picture_layout_t *layout, const uint8_t *atlases, const float sphere[4])
{
	mz_ulong compressed_bytes = mz_compressBound((mz_ulong)(layout->atlas_bytes * 2));
	uint8_t *file = SDL_malloc(sizeof(impostor_file_header_t) + compressed_bytes);
	impostor_file_header_t header;
	char directory[512];

	if (file == NULL)
	{
		return;
	}
	if (mz_compress2(file + sizeof(header), &compressed_bytes, atlases, (mz_ulong)(layout->atlas_bytes * 2), 3) != MZ_OK)
	{
		SDL_free(file);
		return;
	}
	SDL_zero(header);
	SDL_memcpy(header.magic, "PIMP", 4);
	header.version = IMPOSTOR_VERSION;
	header.grid = (uint32_t)(layout->grid_x + 256 * layout->grid_y);
	header.cell_pixels = (uint32_t)layout->cell_pixels;
	header.key = key;
	SDL_memcpy(header.sphere, sphere, sizeof(header.sphere));
	header.compressed_bytes = (uint32_t)compressed_bytes;
	header.raw_bytes = (uint32_t)(layout->atlas_bytes * 2);
	SDL_memcpy(file, &header, sizeof(header));
	file_io_data_path("cache/impostors", directory, sizeof(directory));
	SDL_CreateDirectory(directory);
	if (!file_io_save_file(path, file, sizeof(header) + compressed_bytes))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Impostors: cannot write '%s' (baked again next time)", path);
	}
	SDL_free(file);
}

// ---------------------------------------------------------------------------
// Baking

// Both atlases of the model, into atlases_out (color, then normal and
// depth), straight from the GPU
static bool bake(impostor_baker_t *baker, const object_model_t *model, const picture_layout_t *layout, const float sphere[4], uint8_t *atlases_out)
{
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUFence *fence;
	uint8_t *mapped;

	// The model's buffers may still be on their way up
	gpu_flush_uploads();
	gpu_submit_work(NULL);
	command_buffer = SDL_AcquireGPUCommandBuffer(gpu.device);
	if (command_buffer == NULL)
	{
		return false;
	}
	for (int mode = 0; mode < 2; mode++)
	{
		SDL_GPUColorTargetInfo color;
		SDL_GPUDepthStencilTargetInfo depth;
		SDL_GPURenderPass *pass;
		SDL_GPUBufferBinding vertex_bindings[2] = { { model->vertex_buffer, 0 }, { model->tangent_buffer, 0 } };
		SDL_GPUBufferBinding index_binding = { model->index_buffer, 0 };

		SDL_zero(color);
		color.texture = baker->color_target;
		// The empty parts: nothing covered; a flat normal, the middle depth
		color.clear_color.r = mode == 0 ? 0.0f : 0.5f;
		color.clear_color.g = mode == 0 ? 0.0f : 0.5f;
		color.clear_color.b = mode == 0 ? 0.0f : 0.5f;
		color.clear_color.a = mode == 0 ? 0.0f : 0.5f;
		color.load_op = SDL_GPU_LOADOP_CLEAR;
		color.store_op = SDL_GPU_STOREOP_STORE;
		SDL_zero(depth);
		depth.texture = baker->depth_target;
		depth.clear_depth = 1.0f;
		depth.load_op = SDL_GPU_LOADOP_CLEAR;
		depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
		depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		pass = SDL_BeginGPURenderPass(command_buffer, &color, 1, &depth);
		if (pass == NULL)
		{
			SDL_CancelGPUCommandBuffer(command_buffer);
			return false;
		}
		if (layout->plant)
		{
			SDL_GPUBufferBinding plant_bindings[3] = { { model->vertex_buffer, 0 }, { model->tangent_buffer, 0 }, { model->plant_buffer, 0 } };

			SDL_BindGPUGraphicsPipeline(pass, baker->plant_bake_pipeline);
			SDL_BindGPUVertexBuffers(pass, 0, plant_bindings, 3);
		}
		else
		{
			SDL_BindGPUGraphicsPipeline(pass, baker->bake_pipeline);
			SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, 2);
		}
		SDL_BindGPUIndexBuffer(pass, &index_binding, model->index_size);
		for (int y = 0; y < layout->grid_y; y++)
		{
			for (int x = 0; x < layout->grid_x; x++)
			{
				SDL_GPUViewport viewport = { (float)(x * layout->cell_pixels), (float)(y * layout->cell_pixels), (float)layout->cell_pixels,
					(float)layout->cell_pixels, 0.0f, 1.0f };
				bake_vertex_uniforms_t vertex_uniforms;

				SDL_zero(vertex_uniforms);
				vertex_uniforms.frame[0] = x;
				vertex_uniforms.frame[1] = y;
				vertex_uniforms.frame[2] = layout->thumbnail ? 2 : (layout->plant ? 1 : 0);
				SDL_memcpy(vertex_uniforms.sphere, sphere, sizeof(vertex_uniforms.sphere));
				// A plant's month cells (objects.c's plant_month_cells,
				// the snow aside): each moved to that month's picture
				if (layout->plant)
				{
					for (int c = 0; c < model->month_cell_count && c < PLANT_MONTH_CELLS; c++)
					{
						vertex_uniforms.month_offsets[c][0] = model->month_cells[c].month_offsets[layout->month_index][0];
						vertex_uniforms.month_offsets[c][1] = model->month_cells[c].month_offsets[layout->month_index][1];
						vertex_uniforms.frame[3] = c + 1;
					}
				}
				SDL_SetGPUViewport(pass, &viewport);
				for (int k = 0; k < model->material_count; k++)
				{
					const object_material_t *material = &model->materials[k];
					SDL_GPUTextureSamplerBinding texture_binding = { material->texture, gpu.linear_repeat };
					bake_fragment_uniforms_t fragment_uniforms = { material->alpha_tested ? 1 : 0, material->lighting_normals ? 1 : 0, mode,
						layout->plant ? 1 : 0, { material->tips, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.0f } };

					// (a plant's baked snow: not in the picture, which is
					// its look out of winter)
					if (material->texture == NULL || material->index_count == 0 || material->snow_cover)
					{
						continue;
					}
					// A plant's material in the month baked: how far into
					// its autumn, how much it turns, whether it falls
					// (objects.c's season for plant.vert)
					if (layout->plant)
					{
						float first = material->autumn_first_month >= 1.0f ? material->autumn_first_month : 10.0f;
						float last = material->autumn_last_month >= 1.0f ? material->autumn_last_month : 11.0f;
						float leaves = material->leaves_month >= 1.0f ? material->leaves_month : 4.0f;

						vertex_uniforms.season[0] = season_autumn(layout->month, first, last, leaves);
						vertex_uniforms.season[1] = IMPOSTOR_PLANT_AUTUMN_SPREAD;
						vertex_uniforms.season[2] = material->alpha_tested ? material->autumn : 0.0f;
						vertex_uniforms.season[3] = material->alpha_tested ? material->falls : 0.0f;
						SDL_memcpy(fragment_uniforms.autumn_color_a, material->autumn_colors[0], sizeof(float) * 3);
						SDL_memcpy(fragment_uniforms.autumn_color_b, material->autumn_colors[1], sizeof(float) * 3);
					}
					SDL_PushGPUVertexUniformData(command_buffer, 0, &vertex_uniforms,
						layout->plant ? sizeof(vertex_uniforms) : sizeof(int32_t) * 4 + sizeof(float) * 4);
					SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
					SDL_PushGPUFragmentUniformData(command_buffer, 0, &fragment_uniforms, sizeof(fragment_uniforms));
					SDL_DrawGPUIndexedPrimitives(pass, (uint32_t)material->index_count, 1, (uint32_t)material->index_offset, 0, 0);
				}
			}
		}
		SDL_EndGPURenderPass(pass);
		{
			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command_buffer);
			SDL_GPUTextureRegion source;
			SDL_GPUTextureTransferInfo destination;

			SDL_zero(source);
			source.texture = baker->color_target;
			source.w = (uint32_t)layout->atlas_width;
			source.h = (uint32_t)layout->atlas_height;
			source.d = 1;
			SDL_zero(destination);
			destination.transfer_buffer = baker->download;
			destination.offset = (uint32_t)(layout->atlas_bytes * (size_t)mode);
			SDL_DownloadFromGPUTexture(copy, &source, &destination);
			SDL_EndGPUCopyPass(copy);
		}
	}
	fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
	if (fence == NULL)
	{
		return false;
	}
	SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
	SDL_ReleaseGPUFence(gpu.device, fence);
	mapped = SDL_MapGPUTransferBuffer(gpu.device, baker->download, false);
	if (mapped == NULL)
	{
		return false;
	}
	SDL_memcpy(atlases_out, mapped, layout->atlas_bytes * 2);
	SDL_UnmapGPUTransferBuffer(gpu.device, baker->download);
	return true;
}

// The empty texels get their covered neighbors' color and normal, so that
// the mipmaps do not grow dark or flat fringes; the depth of an empty texel
// stays in the middle
static void bleed(uint8_t *atlases, const picture_layout_t *layout)
{
	uint8_t *albedo = atlases;
	uint8_t *normal = atlases + layout->atlas_bytes;
	size_t texels = (size_t)layout->atlas_width * (size_t)layout->atlas_height;
	uint8_t *depth = SDL_malloc(texels);

	if (depth == NULL)
	{
		return;
	}
	object_texture_bleed_transparent_colors(albedo, layout->atlas_width, layout->atlas_height);
	// The normals bleed by the color's coverage: lent its alpha for it
	for (size_t i = 0; i < texels; i++)
	{
		depth[i] = normal[i * 4 + 3];
		normal[i * 4 + 3] = albedo[i * 4 + 3];
	}
	object_texture_bleed_transparent_colors(normal, layout->atlas_width, layout->atlas_height);
	for (size_t i = 0; i < texels; i++)
	{
		normal[i * 4 + 3] = albedo[i * 4 + 3] >= 128 ? depth[i] : 128;
	}
	SDL_free(depth);
}

bool impostor_make(impostor_baker_t *baker, impostor_t *impostor, const object_model_t *model, const object_texture_cache_t *textures,
	float month)
{
	uint64_t start = SDL_GetPerformanceCounter();
	uint8_t *atlases;
	char path[512];
	uint64_t key;
	bool from_cache;
	picture_layout_t layout = layout_for(model, month);

	SDL_zerop(impostor);
	if (baker->bake_pipeline == NULL || gpu.device == NULL || gpu.command_buffer != NULL)
	{
		return false;
	}
	atlases = SDL_malloc(layout.atlas_bytes * 2);
	if (atlases == NULL)
	{
		return false;
	}
	key = cache_key(model, textures, &layout);
	cache_path(model, path, sizeof(path));
	from_cache = cache_load(path, key, &layout, atlases, impostor->sphere);
	if (!from_cache)
	{
		bounding_sphere(model, impostor->sphere);
		if (!bake(baker, model, &layout, impostor->sphere, atlases))
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Impostors: cannot bake '%s': %s", model->name, SDL_GetError());
			SDL_free(atlases);
			return false;
		}
		bleed(atlases, &layout);
		cache_save(path, key, &layout, atlases, impostor->sphere);
		// Debugging: POTOP_IMPOSTOR_DUMP=<directory> saves each baked
		// atlas as PNG pictures there (color, then normal and depth)
		if (SDL_getenv("POTOP_IMPOSTOR_DUMP") != NULL)
		{
			char dump[600];
			const char *base = SDL_strrchr(path, '/');

			SDL_snprintf(dump, sizeof(dump), "%s/%s_color.png", SDL_getenv("POTOP_IMPOSTOR_DUMP"), base != NULL ? base + 1 : path);
			screenshot_save(dump, atlases, layout.atlas_width, layout.atlas_height);
			SDL_snprintf(dump, sizeof(dump), "%s/%s_normal.png", SDL_getenv("POTOP_IMPOSTOR_DUMP"), base != NULL ? base + 1 : path);
			screenshot_save(dump, atlases + layout.atlas_bytes, layout.atlas_width, layout.atlas_height);
		}
		baker->baked++;
	}
	else
	{
		baker->loaded++;
	}
	{
		int triangles = 0;
		int cut_out = 0;

		for (int k = 0; k < model->material_count; k++)
		{
			triangles += model->materials[k].index_count / 3;
			cut_out += model->materials[k].alpha_tested ? model->materials[k].index_count / 3 : 0;
		}
		impostor->look[0] = triangles > 0 ? (float)cut_out / (float)triangles : 0.0f;
	}
	impostor->look[1] = layout.plant ? 1.0f : 0.0f;
	impostor->look[2] = (float)layout.grid_x;
	impostor->look[3] = (float)layout.grid_y;
	impostor->atlas_width = layout.atlas_width;
	impostor->atlas_height = layout.atlas_height;
	impostor->month_index = layout.month_index;
	impostor->albedo = object_texture_upload(atlases, layout.atlas_width, layout.atlas_height);
	impostor->normal = object_texture_upload(atlases + layout.atlas_bytes, layout.atlas_width, layout.atlas_height);
	SDL_free(atlases);
	baker->seconds += (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();
	if (impostor->albedo == NULL || impostor->normal == NULL)
	{
		impostor_release(impostor);
		return false;
	}
	return true;
}

bool impostor_make_thumbnail(impostor_baker_t *baker, const object_model_t *model, float month, uint8_t *rgba_out)
{
	picture_layout_t layout = layout_for(model, month);
	float sphere[4];
	uint8_t *atlases;
	// The light from over the viewer's left shoulder (model axes; the view
	// impostor.glsl's IMPOSTOR_THUMBNAIL_DIRECTION)
	float light[3] = { 0.15f, 0.85f, 0.75f };
	float light_length = SDL_sqrtf(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
	size_t texels = (size_t)IMPOSTOR_THUMBNAIL_PIXELS * IMPOSTOR_THUMBNAIL_PIXELS;

	layout.thumbnail = true;
	layout.grid_x = 1;
	layout.grid_y = 1;
	layout.cell_pixels = IMPOSTOR_THUMBNAIL_PIXELS;
	layout.atlas_width = IMPOSTOR_THUMBNAIL_PIXELS;
	layout.atlas_height = IMPOSTOR_THUMBNAIL_PIXELS;
	layout.atlas_bytes = texels * 4;
	atlases = SDL_malloc(layout.atlas_bytes * 2);
	if (atlases == NULL)
	{
		return false;
	}
	bounding_sphere(model, sphere);
	if (!bake(baker, model, &layout, sphere, atlases))
	{
		SDL_free(atlases);
		return false;
	}
	// Lit as the editor's pictures want it: some light all round, the
	// rest from the one side (the leaves' wrapped, as they are drawn)
	for (size_t i = 0; i < texels; i++)
	{
		const uint8_t *albedo = atlases + 4 * i;
		const uint8_t *normal_depth = atlases + layout.atlas_bytes + 4 * i;
		float normal[3];
		float length;
		float facing = 0.0f;
		float lit;

		for (int k = 0; k < 3; k++)
		{
			normal[k] = (float)normal_depth[k] / 127.5f - 1.0f;
		}
		length = SDL_sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
		if (length > 1e-3f)
		{
			facing = (normal[0] * light[0] + normal[1] * light[1] + normal[2] * light[2]) / (length * light_length);
		}
		// (a plant's leaves: baked a quarter as long, lit wrapped round)
		lit = length < 0.5f ? 0.55f + 0.6f * (facing * 0.5f + 0.5f) : 0.4f + 0.85f * SDL_max(facing, 0.0f);
		for (int k = 0; k < 3; k++)
		{
			float linear = SDL_powf((float)albedo[k] / 255.0f, 2.2f) * lit;

			rgba_out[4 * i + k] = (uint8_t)SDL_clamp(SDL_powf(linear, 1.0f / 2.2f) * 255.0f + 0.5f, 0.0f, 255.0f);
		}
		rgba_out[4 * i + 3] = albedo[3] >= 128 ? 255 : 0;
	}
	SDL_free(atlases);
	return true;
}

void impostor_release(impostor_t *impostor)
{
	if (gpu.device != NULL)
	{
		if (impostor->albedo != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, impostor->albedo);
		}
		if (impostor->normal != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, impostor->normal);
		}
	}
	SDL_zerop(impostor);
}
