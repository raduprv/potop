// The plant editor's view of the plant. See plant_preview.h.

#include "plant_preview.h"

#include <SDL3/SDL.h>

#include "lights.h"
#include "math3d.h"
#include "object_model.h"
#include "scene_uniforms.h"
#include "season.h"
#include "stb_image.h"

// The patch of ground: this wide (meters), its texture one meter a tile
#define GROUND_HALF_WIDTH 15.0f
#define GROUND_TEXTURE_SIZE 64

// As the game's (objects.c): placement and size, rotation, fade
#define INSTANCE_FLOATS 9

static const SDL_GPUVertexBufferDescription vertex_buffers[3] =
{
	{ 0, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 1, sizeof(float) * INSTANCE_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
	{ 2, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
};
static const SDL_GPUVertexAttribute vertex_attributes[7] =
{
	{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
	{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 },
	{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 },
	{ 3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
	{ 4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4 },
	{ 5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, sizeof(float) * 8 },
	{ 6, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
};

// The plants' (objects.c): the same, and what each vertex is on the plant
static const SDL_GPUVertexBufferDescription plant_vertex_buffers[4] =
{
	{ 0, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 1, sizeof(float) * INSTANCE_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
	{ 2, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 3, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },  // object_model.h's OBJECT_PLANT_VERTEX_FLOATS
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
// plant.glsl's PLANT_MONTH_CELLS
#define PREVIEW_MONTH_CELLS 32

// object.frag's material block
typedef struct
{
	float highlight_parameters[4];
	int32_t alpha_tested;
	int32_t pbr;
	int32_t lighting_normals;
	int32_t padding;
	float deciduous[4];               // (none)
} material_uniforms_t;

static float srgb_to_linear(float value)
{
	return value <= 0.04045f ? value / 12.92f : SDL_powf((value + 0.055f) / 1.055f, 2.4f);
}

void plant_preview_sky(float color_out[4])
{
	color_out[0] = srgb_to_linear(158.0f / 255.0f);
	color_out[1] = srgb_to_linear(189.0f / 255.0f);
	color_out[2] = srgb_to_linear(219.0f / 255.0f);
	color_out[3] = 1.0f;
}

static SDL_GPUTexture *solid_texture(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	uint8_t pixels[4 * 4 * 4];

	for (int i = 0; i < 16; i++)
	{
		pixels[i * 4] = r;
		pixels[i * 4 + 1] = g;
		pixels[i * 4 + 2] = b;
		pixels[i * 4 + 3] = a;
	}
	return object_texture_upload(pixels, 4, 4);
}

static void release_buffer(SDL_GPUBuffer **buffer)
{
	if (*buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, *buffer);
		*buffer = NULL;
	}
}

static void release_texture(SDL_GPUTexture **texture)
{
	if (*texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, *texture);
		*texture = NULL;
	}
}

// The ground: a square, its texture earth with a lighter line every meter
// (and a darker one every five)
static bool make_ground(plant_preview_t *preview)
{
	float vertices[4][8];
	float tangents[4][4];
	static const uint32_t indices[6] = { 0, 2, 1, 0, 3, 2 };
	uint8_t *pixels = SDL_malloc(GROUND_TEXTURE_SIZE * GROUND_TEXTURE_SIZE * 4);
	static const float corners[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

	if (pixels == NULL)
	{
		return false;
	}
	for (int y = 0; y < GROUND_TEXTURE_SIZE; y++)
	{
		for (int x = 0; x < GROUND_TEXTURE_SIZE; x++)
		{
			uint8_t *texel = &pixels[(y * GROUND_TEXTURE_SIZE + x) * 4];
			bool line = x == 0 || y == 0;
			int noise = (int)((x * 73 + y * 151 + (x * y) % 7) % 9) - 4;

			texel[0] = (uint8_t)(line ? 190 : 138 + noise);
			texel[1] = (uint8_t)(line ? 178 : 124 + noise);
			texel[2] = (uint8_t)(line ? 150 : 92 + noise);
			texel[3] = 255;
		}
	}
	preview->ground_texture = object_texture_upload(pixels, GROUND_TEXTURE_SIZE, GROUND_TEXTURE_SIZE);
	SDL_free(pixels);
	for (int i = 0; i < 4; i++)
	{
		vertices[i][0] = corners[i][0] * GROUND_HALF_WIDTH;
		vertices[i][1] = 0.0f;
		vertices[i][2] = corners[i][1] * GROUND_HALF_WIDTH;
		vertices[i][3] = 0.0f;
		vertices[i][4] = 1.0f;
		vertices[i][5] = 0.0f;
		// A tile a meter, the lines on the meters
		vertices[i][6] = vertices[i][0];
		vertices[i][7] = vertices[i][2];
		tangents[i][0] = 1.0f;
		tangents[i][1] = 0.0f;
		tangents[i][2] = 0.0f;
		tangents[i][3] = 1.0f;
	}
	preview->ground_vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, sizeof(vertices));
	preview->ground_tangent_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, sizeof(tangents));
	preview->ground_index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, sizeof(indices));
	if (preview->ground_vertex_buffer == NULL || preview->ground_tangent_buffer == NULL || preview->ground_index_buffer == NULL
		|| preview->ground_texture == NULL)
	{
		return false;
	}
	gpu_upload_buffer(preview->ground_vertex_buffer, 0, vertices, sizeof(vertices));
	gpu_upload_buffer(preview->ground_tangent_buffer, 0, tangents, sizeof(tangents));
	gpu_upload_buffer(preview->ground_index_buffer, 0, indices, sizeof(indices));
	return true;
}

bool plant_preview_create(plant_preview_t *preview)
{
	SDL_GPUShader *vertex = gpu_load_shader("object.vert", 0, 0, 2);
	SDL_GPUShader *fragment = gpu_load_shader("object.frag", OBJECT_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);
	gpu_pipeline_info_t info;
	bool ok = vertex != NULL && fragment != NULL;

	SDL_zerop(preview);
	if (ok)
	{
		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = vertex_buffers;
		info.vertex_buffer_count = 3;
		info.vertex_attributes = vertex_attributes;
		info.vertex_attribute_count = 7;
		info.cull = SDL_GPU_CULLMODE_BACK;
		info.depth_test = true;
		info.depth_write = true;
		preview->pipeline = gpu_make_pipeline(&info, "plant preview");
		info.cull = SDL_GPU_CULLMODE_NONE;
		preview->alpha_pipeline = gpu_make_pipeline(&info, "plant preview (cut out)");
		ok = preview->pipeline != NULL && preview->alpha_pipeline != NULL;
	}
	if (vertex != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, vertex);
	}
	if (fragment != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, fragment);
	}
	// The plant itself: the plants' shaders, as the game draws it
	if (ok)
	{
		SDL_GPUShader *plant_vertex = gpu_load_shader("plant.vert", 4, 0, 2);
		SDL_GPUShader *plant_fragment = gpu_load_shader("plant.frag", OBJECT_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);

		if (plant_vertex != NULL && plant_fragment != NULL)
		{
			SDL_zero(info);
			info.vertex_shader = plant_vertex;
			info.fragment_shader = plant_fragment;
			info.vertex_buffers = plant_vertex_buffers;
			info.vertex_buffer_count = 4;
			info.vertex_attributes = plant_vertex_attributes;
			info.vertex_attribute_count = 9;
			info.cull = SDL_GPU_CULLMODE_BACK;
			info.depth_test = true;
			info.depth_write = true;
			preview->plant_pipeline = gpu_make_pipeline(&info, "plant preview (plant)");
			info.cull = SDL_GPU_CULLMODE_NONE;
			preview->plant_alpha_pipeline = gpu_make_pipeline(&info, "plant preview (plant, cut out)");
		}
		if (plant_vertex != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, plant_vertex);
		}
		if (plant_fragment != NULL)
		{
			SDL_ReleaseGPUShader(gpu.device, plant_fragment);
		}
		ok = preview->plant_pipeline != NULL && preview->plant_alpha_pipeline != NULL;
	}
	if (!ok)
	{
		return false;
	}
	{
		float instance[INSTANCE_FLOATS] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f };
		// No local lights: the light block's count is 0 (and one empty cluster)
		uint32_t empty[16];

		SDL_zeroa(empty);
		preview->instance_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, sizeof(instance));
		for (int i = 0; i < 2; i++)
		{
			preview->light_buffers[i] = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, sizeof(empty));
		}
		if (preview->instance_buffer == NULL || preview->light_buffers[0] == NULL || preview->light_buffers[1] == NULL)
		{
			return false;
		}
		gpu_upload_buffer(preview->instance_buffer, 0, instance, sizeof(instance));
		gpu_upload_buffer(preview->light_buffers[0], 0, empty, sizeof(empty));
		gpu_upload_buffer(preview->light_buffers[1], 0, empty, sizeof(empty));
	}
	preview->shadow_dummy = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.depth_format, 4, 4, 1, 1,
		SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
	preview->neutral = solid_texture(128, 128, 128, 255);
	preview->bark_plain = solid_texture(77, 54, 36, 255);
	return preview->shadow_dummy != NULL && preview->neutral != NULL && preview->bark_plain != NULL && make_ground(preview);
}

void plant_preview_destroy(plant_preview_t *preview)
{
	SDL_GPUBuffer **buffers[] = { &preview->vertex_buffer, &preview->tangent_buffer, &preview->index_buffer, &preview->instance_buffer,
		&preview->light_buffers[0], &preview->light_buffers[1], &preview->ground_vertex_buffer, &preview->ground_tangent_buffer,
		&preview->ground_index_buffer };
	SDL_GPUTexture **textures[] = { &preview->ground_texture, &preview->atlas_texture, &preview->bark_textures[0],
		&preview->bark_textures[1], &preview->bark_textures[2], &preview->bark_plain, &preview->shadow_dummy, &preview->neutral };

	if (gpu.device == NULL)
	{
		return;
	}
	for (size_t i = 0; i < SDL_arraysize(buffers); i++)
	{
		release_buffer(buffers[i]);
	}
	for (size_t i = 0; i < SDL_arraysize(textures); i++)
	{
		release_texture(textures[i]);
	}
	if (preview->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->pipeline);
	}
	if (preview->alpha_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->alpha_pipeline);
	}
	if (preview->plant_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->plant_pipeline);
	}
	if (preview->plant_alpha_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->plant_alpha_pipeline);
	}
	release_buffer(&preview->plant_buffer);
	SDL_zerop(preview);
}

void plant_preview_set_mesh(plant_preview_t *preview, const plant_mesh_t *mesh)
{
	size_t index_total = 0;
	float *vertices;
	float *tangents;
	uint32_t *indices;

	release_buffer(&preview->vertex_buffer);
	release_buffer(&preview->tangent_buffer);
	release_buffer(&preview->index_buffer);
	release_buffer(&preview->plant_buffer);
	preview->has_plant = false;
	if (mesh == NULL || mesh->vertex_count == 0)
	{
		return;
	}
	for (int m = 0; m < PLANT_MATERIAL_COUNT; m++)
	{
		index_total += mesh->index_count[m];
	}
	vertices = SDL_malloc(sizeof(float) * 8 * mesh->vertex_count);
	tangents = SDL_malloc(sizeof(float) * 4 * mesh->vertex_count);
	indices = SDL_malloc(sizeof(uint32_t) * SDL_max(index_total, 1));
	if (vertices == NULL || tangents == NULL || indices == NULL)
	{
		SDL_free(vertices);
		SDL_free(tangents);
		SDL_free(indices);
		return;
	}
	for (size_t i = 0; i < mesh->vertex_count; i++)
	{
		const object_file_vertex_t *source = &mesh->vertices[i];

		SDL_memcpy(&vertices[i * 8], source->position, sizeof(float) * 3);
		SDL_memcpy(&vertices[i * 8 + 3], source->normal, sizeof(float) * 3);
		SDL_memcpy(&vertices[i * 8 + 6], source->uv, sizeof(float) * 2);
		SDL_memcpy(&tangents[i * 4], source->tangent, sizeof(float) * 4);
	}
	index_total = 0;
	for (int m = 0; m < PLANT_MATERIAL_COUNT; m++)
	{
		preview->first_index[m] = (uint32_t)index_total;
		preview->index_count[m] = (uint32_t)mesh->index_count[m];
		preview->lods[m] = mesh->lods[m];
		SDL_memcpy(preview->lod_index_count[m], mesh->lod_index_count[m], sizeof(preview->lod_index_count[m]));
		if (mesh->index_count[m] > 0)
		{
			SDL_memcpy(&indices[index_total], mesh->indices[m], sizeof(uint32_t) * mesh->index_count[m]);
		}
		index_total += mesh->index_count[m];
	}
	preview->vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 8 * mesh->vertex_count));
	preview->tangent_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 4 * mesh->vertex_count));
	preview->index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, (uint32_t)(sizeof(uint32_t) * SDL_max(index_total, 1)));
	preview->plant_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 8 * mesh->vertex_count));
	SDL_memcpy(preview->plant_materials, mesh->plant_materials, sizeof(preview->plant_materials));
	if (preview->vertex_buffer != NULL && preview->tangent_buffer != NULL && preview->index_buffer != NULL && preview->plant_buffer != NULL
		&& mesh->plant_vertices != NULL)
	{
		// (what each vertex is on the plant, and where its snow takes it)
		float *plant_data = SDL_calloc(mesh->vertex_count, sizeof(float) * 8);

		if (plant_data != NULL)
		{
			for (size_t i = 0; i < mesh->vertex_count; i++)
			{
				SDL_memcpy(&plant_data[i * 8], &mesh->plant_vertices[i], sizeof(object_file_plant_vertex_t));
				if (mesh->snow_vertices != NULL)
				{
					SDL_memcpy(&plant_data[i * 8 + 4], &mesh->snow_vertices[i], sizeof(object_file_snow_vertex_t));
				}
			}
			gpu_upload_buffer(preview->plant_buffer, 0, plant_data, sizeof(float) * 8 * mesh->vertex_count);
			SDL_free(plant_data);
		}
		gpu_upload_buffer(preview->vertex_buffer, 0, vertices, sizeof(float) * 8 * mesh->vertex_count);
		gpu_upload_buffer(preview->tangent_buffer, 0, tangents, sizeof(float) * 4 * mesh->vertex_count);
		gpu_upload_buffer(preview->index_buffer, 0, indices, sizeof(uint32_t) * SDL_max(index_total, 1));
		SDL_memcpy(preview->minimum, mesh->minimum, sizeof(preview->minimum));
		SDL_memcpy(preview->maximum, mesh->maximum, sizeof(preview->maximum));
		preview->month_cell_count = SDL_min(mesh->month_cell_count, PLANT_MAX_MONTH_CELLS);
		SDL_memcpy(preview->month_cells, mesh->month_cells, sizeof(object_file_month_cell_t) * (size_t)preview->month_cell_count);
		preview->horizontal_radius = 0.0f;
		for (size_t i = 0; i < mesh->vertex_count; i++)
		{
			const float *p = mesh->vertices[i].position;

			preview->horizontal_radius = SDL_max(preview->horizontal_radius, SDL_sqrtf(p[0] * p[0] + p[2] * p[2]));
		}
		preview->has_plant = true;
	}
	SDL_free(vertices);
	SDL_free(tangents);
	SDL_free(indices);
}

void plant_preview_set_atlas(plant_preview_t *preview, const plant_atlas_t *atlas)
{
	release_texture(&preview->atlas_texture);
	if (atlas != NULL && plant_atlas_pixels(atlas) != NULL)
	{
		// As it is in memory (RGBA8: the game's is BC7)
		preview->atlas_texture = object_texture_upload(plant_atlas_pixels(atlas), plant_atlas_size(atlas), plant_atlas_size(atlas));
	}
}

void plant_preview_set_bark(plant_preview_t *preview, const char (*bark)[PLANT_PATH_LENGTH], const float plain_color[3],
	const char *data_directory)
{
	for (int k = 0; k < 3; k++)
	{
		int width = 0;
		int height = 0;
		int channels = 0;
		uint8_t *pixels;

		release_texture(&preview->bark_textures[k]);
		if (bark[k][0] == '\0')
		{
			continue;
		}
		{
			size_t size = 0;
			void *bytes = SDL_LoadFile(bark[k], &size);

			// Or as the game names it, under the data directory
			if (bytes == NULL)
			{
				char path[PLANT_PATH_LENGTH * 2];

				SDL_snprintf(path, sizeof(path), "%s/%s", data_directory, bark[k]);
				bytes = SDL_LoadFile(path, &size);
			}

			pixels = bytes != NULL ? stbi_load_from_memory(bytes, (int)size, &width, &height, &channels, 4) : NULL;
			SDL_free(bytes);
		}
		if (pixels != NULL)
		{
			preview->bark_textures[k] = object_texture_upload(pixels, width, height);
			stbi_image_free(pixels);
		}
	}
	// Without one: the plain color (as the game makes it, the factor on white)
	release_texture(&preview->bark_plain);
	preview->bark_plain = solid_texture((uint8_t)SDL_clamp((int)(plain_color[0] * 255.0f + 0.5f), 0, 255),
		(uint8_t)SDL_clamp((int)(plain_color[1] * 255.0f + 0.5f), 0, 255), (uint8_t)SDL_clamp((int)(plain_color[2] * 255.0f + 0.5f), 0, 255), 255);
}

// One material's draw (its textures, its uniforms)
static void draw_part(SDL_GPURenderPass *pass, SDL_GPUTexture *color, SDL_GPUTexture *normal, SDL_GPUTexture *orm, bool alpha_tested,
	bool pbr, bool lighting_normals, const float wind_uniforms[16], uint32_t first_index, uint32_t index_count)
{
	material_uniforms_t material;
	SDL_GPUTextureSamplerBinding color_binding = { color, gpu.linear_repeat };
	SDL_GPUTextureSamplerBinding maps[3] =
	{
		{ normal != NULL ? normal : object_default_texture(OBJECT_DEFAULT_NORMAL), gpu.linear_repeat },
		{ orm != NULL ? orm : object_default_texture(OBJECT_DEFAULT_ORM), gpu.linear_repeat },
		{ object_default_texture(OBJECT_DEFAULT_EMISSIVE), gpu.linear_repeat },
	};

	if (index_count == 0 || color == NULL)
	{
		return;
	}
	SDL_zero(material);
	material.alpha_tested = alpha_tested ? 1 : 0;
	material.pbr = pbr ? 1 : 0;
	material.lighting_normals = lighting_normals ? 1 : 0;
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material, sizeof(material));
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, wind_uniforms, sizeof(float) * 16);
	SDL_BindGPUFragmentSamplers(pass, 3, &color_binding, 1);
	SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
	SDL_DrawGPUIndexedPrimitives(pass, index_count, 1, first_index, 0, 0);
}

// A material of the plant, with the plants' shaders: its month and its
// look through it (plant.frag's block, objects.c's)
static void draw_plant_part(const plant_preview_t *preview, SDL_GPURenderPass *pass, SDL_GPUTexture *color, bool alpha_tested,
	SDL_GPUTexture *normal, bool lighting_normals, const object_file_plant_material_t *plant, const plant_view_t *view,
	const float wind_uniforms[16], uint32_t first_index, uint32_t index_count, bool snow_cover)
{
	float fragment_uniforms[20];
	float vertex_uniforms[24 + PREVIEW_MONTH_CELLS * 8];
	SDL_GPUTextureSamplerBinding color_binding = { color, gpu.linear_repeat };
	SDL_GPUTextureSamplerBinding maps[3] =
	{
		{ normal != NULL ? normal : object_default_texture(OBJECT_DEFAULT_NORMAL), gpu.linear_repeat },
		{ object_default_texture(OBJECT_DEFAULT_ORM), gpu.linear_repeat },
		{ object_default_texture(OBJECT_DEFAULT_EMISSIVE), gpu.linear_repeat },
	};
	int month_index = season_month_index(view->month);
	int cell_count = 0;
	float first = plant->autumn_first_month >= 1.0f ? plant->autumn_first_month : 10.0f;
	float last = plant->autumn_last_month >= 1.0f ? plant->autumn_last_month : 11.0f;
	float leaves = plant->leaves_month >= 1.0f ? plant->leaves_month : 4.0f;
	float autumn = season_autumn(view->month, first, last, leaves);
	float snow = season_snow(view->month, view->snow_first_month, view->snow_last_month);

	if (index_count == 0 || color == NULL)
	{
		return;
	}
	SDL_zeroa(fragment_uniforms);
	SDL_zeroa(vertex_uniforms);
	SDL_memcpy(&fragment_uniforms[4], plant->autumn_colors[0], sizeof(float) * 3);
	fragment_uniforms[7] = plant->autumn;
	SDL_memcpy(&fragment_uniforms[8], plant->autumn_colors[1], sizeof(float) * 3);
	fragment_uniforms[11] = plant->falls;
	fragment_uniforms[12] = plant->moss;
	fragment_uniforms[13] = plant->tips * season_new_growth(view->month, view->tips_first_month, view->tips_last_month);
	fragment_uniforms[14] = alpha_tested ? 1.0f : 0.0f;
	fragment_uniforms[15] = lighting_normals ? 1.0f : 0.0f;
	fragment_uniforms[16] = normal != NULL ? 1.0f : 0.0f;
	fragment_uniforms[17] = snow;
	fragment_uniforms[18] = snow_cover ? 1.0f : 0.0f;
	SDL_memcpy(vertex_uniforms, wind_uniforms, sizeof(float) * 16);
	vertex_uniforms[16] = autumn;
	// One tree: the season as it is, not earlier or later by its place
	vertex_uniforms[17] = 0.0f;
	vertex_uniforms[18] = alpha_tested ? plant->autumn : 0.0f;
	vertex_uniforms[19] = alpha_tested ? plant->falls : 0.0f;
	// The snow (plant.glsl): the branches sagging
	vertex_uniforms[20] = snow;
	vertex_uniforms[21] = plant->sag;
	vertex_uniforms[23] = preview->horizontal_radius;
	// The pictures that show others this month (plant.glsl)
	for (int c = 0; alpha_tested && c < preview->month_cell_count && cell_count < PREVIEW_MONTH_CELLS; c++)
	{
		const object_file_month_cell_t *cell = &preview->month_cells[c];

		if (cell->month_offsets[month_index][0] == 0.0f && cell->month_offsets[month_index][1] == 0.0f)
		{
			continue;
		}
		SDL_memcpy(&vertex_uniforms[24 + cell_count * 4], cell->cell, sizeof(float) * 4);
		vertex_uniforms[24 + PREVIEW_MONTH_CELLS * 4 + cell_count * 4 + 0] = cell->month_offsets[month_index][0];
		vertex_uniforms[24 + PREVIEW_MONTH_CELLS * 4 + cell_count * 4 + 1] = cell->month_offsets[month_index][1];
		cell_count++;
	}
	vertex_uniforms[22] = (float)cell_count;
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, fragment_uniforms, sizeof(fragment_uniforms));
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, vertex_uniforms, sizeof(vertex_uniforms));
	SDL_BindGPUFragmentSamplers(pass, 3, &color_binding, 1);
	SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
	SDL_DrawGPUIndexedPrimitives(pass, index_count, 1, first_index, 0, 0);
}

void plant_preview_draw(plant_preview_t *preview, SDL_GPURenderPass *pass, const plant_view_t *view)
{
	scene_uniforms_t scene;
	mat4_t projection;
	mat4_t view_matrix;
	mat4_t view_projection;
	mat4_t inverse;
	float azimuth = degrees_to_radians(view->sun_azimuth_degrees);
	float elevation = degrees_to_radians(view->sun_elevation_degrees);
	float wind[16];
	float sky[4];
	SDL_GPUTextureSamplerBinding fixed[3] =
	{
		{ preview->shadow_dummy, gpu.shadow_compare },
		{ preview->shadow_dummy, gpu.shadow_compare },
		{ preview->neutral, gpu.linear_repeat },
	};
	SDL_GPUTextureSamplerBinding cloud_shadow = { preview->neutral, gpu.linear_repeat };

	if (pass == NULL || preview->pipeline == NULL)
	{
		return;
	}
	projection = mat4_perspective(degrees_to_radians(view->vertical_fov_degrees), SDL_max(view->aspect, 0.1f), 0.05f, 2000.0f);
	view_matrix = mat4_look_at(vec3_make(view->eye[0], view->eye[1], view->eye[2]), vec3_make(view->target[0], view->target[1], view->target[2]),
		vec3_make(0.0f, 1.0f, 0.0f));
	view_projection = mat4_multiply(projection, view_matrix);
	mat4_inverse(view_projection, &inverse);

	// The scene as object.frag knows it: the sun, the sky, no shadows, no
	// clouds, no fog to speak of
	SDL_zero(scene);
	gpu_clip_matrix(view_projection.m, scene.view_projection);
	SDL_memcpy(scene.inverse_view_projection, inverse.m, sizeof(scene.inverse_view_projection));
	SDL_memcpy(scene.light_view_projection, mat4_identity().m, sizeof(scene.light_view_projection));
	SDL_memcpy(scene.near_light_view_projection, mat4_identity().m, sizeof(scene.near_light_view_projection));
	SDL_memcpy(scene.eye_position, view->eye, sizeof(scene.eye_position));
	scene.time_seconds = view->time_seconds;
	scene.sun_direction[0] = SDL_cosf(elevation) * SDL_cosf(azimuth);
	scene.sun_direction[1] = SDL_sinf(elevation);
	scene.sun_direction[2] = SDL_cosf(elevation) * SDL_sinf(azimuth);
	scene.ambient_intensity = view->ambient;
	plant_preview_sky(sky);
	for (int i = 0; i < 3; i++)
	{
		float sun_srgb[3] = { 1.0f, 242.0f / 255.0f, 217.0f / 255.0f };

		scene.sun_color[i] = srgb_to_linear(sun_srgb[i]) * SDL_clamp(SDL_sinf(elevation) * 3.0f, 0.0f, 1.0f);
		scene.sky_color[i] = sky[i];
		scene.sky_zenith_color[i] = sky[i] * 0.7f;
	}
	scene.fog_density = 0.0f;
	scene.fog_parameters[0] = 1000.0f;
	scene.shadow_strength = 0.0f;
	scene.cloud_parameters[3] = 1.0f / 900.0f;
	// The north, as the game has it (away from the noon sun): the moss
	scene.aurora[0] = -SDL_cosf(azimuth);
	scene.aurora[1] = -SDL_sinf(azimuth);
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, &scene, sizeof(scene));
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, &scene, sizeof(scene));

	// The wind as the game's (objects.c)
	SDL_zeroa(wind);
	wind[0] = 1.0f;
	wind[1] = 0.0f;
	wind[2] = SDL_clamp(view->wind, 0.0f, 1.0f);
	wind[3] = SDL_fmodf(view->time_seconds, 3600.0f);
	wind[4] = 2.0f * SDL_PI_F / 25.0f;
	wind[5] = 2.0f + 10.0f * wind[2];

	SDL_BindGPUFragmentSamplers(pass, 0, fixed, 3);
	SDL_BindGPUFragmentSamplers(pass, 4, &cloud_shadow, 1);
	// (the game's ground and water: none here, stand ins)
	scene_ground_bind(pass, 8);
	// (plant.vert's sky: none here, stand ins; the shadow map's dummy)
	scene_ground_bind_sky_vertex4(pass, 0, preview->shadow_dummy);
	SDL_BindGPUFragmentStorageBuffers(pass, 0, preview->light_buffers, 2);

	// The ground
	{
		SDL_GPUBufferBinding bindings[3] =
		{
			{ preview->ground_vertex_buffer, 0 },
			{ preview->instance_buffer, 0 },
			{ preview->ground_tangent_buffer, 0 },
		};
		SDL_GPUBufferBinding index_binding = { preview->ground_index_buffer, 0 };

		SDL_BindGPUGraphicsPipeline(pass, preview->pipeline);
		SDL_BindGPUVertexBuffers(pass, 0, bindings, 3);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		draw_part(pass, preview->ground_texture, NULL, NULL, false, false, false, wind, 0, 6);
	}
	if (!preview->has_plant)
	{
		return;
	}
	{
		SDL_GPUBufferBinding bindings[4] =
		{
			{ preview->vertex_buffer, 0 },
			{ preview->instance_buffer, 0 },
			{ preview->tangent_buffer, 0 },
			{ preview->plant_buffer, 0 },
		};
		SDL_GPUBufferBinding index_binding = { preview->index_buffer, 0 };
		SDL_GPUTexture *bark = preview->bark_textures[0] != NULL ? preview->bark_textures[0] : preview->bark_plain;

		SDL_BindGPUVertexBuffers(pass, 0, bindings, 4);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		wind[8] = view->sway;
		wind[9] = view->flutter;
		wind[14] = view->stiffness;
		wind[10] = preview->minimum[1];
		wind[11] = preview->maximum[1] - preview->minimum[1];
		SDL_BindGPUGraphicsPipeline(pass, preview->plant_pipeline);
		uint32_t counts[PLANT_MATERIAL_COUNT];

		// What the level of detail keeps of each
		for (int m = 0; m < PLANT_MATERIAL_COUNT; m++)
		{
			counts[m] = view->detail > 0 && view->detail <= 3 && preview->lods[m] ? preview->lod_index_count[m][view->detail - 1]
				: preview->index_count[m];
		}
		counts[PLANT_MATERIAL_FOLIAGE] = (uint32_t)((float)(counts[PLANT_MATERIAL_FOLIAGE] / 6) * SDL_clamp(view->foliage_kept, 0.0f, 1.0f)) * 6;
		for (int m = PLANT_MATERIAL_TRUNK; m <= PLANT_MATERIAL_BARK; m++)
		{
			draw_plant_part(preview, pass, bark, false, preview->bark_textures[1], false, &preview->plant_materials[m], view, wind,
				preview->first_index[m], counts[m], false);
		}
		// The snow baked in, lying on the boughs (the atlas: its edges)
		{
			draw_plant_part(preview, pass, preview->atlas_texture, false, NULL, false, &preview->plant_materials[PLANT_MATERIAL_SNOW], view, wind,
				preview->first_index[PLANT_MATERIAL_SNOW], counts[PLANT_MATERIAL_SNOW], true);
		}
		wind[12] = 1.0f;
		SDL_BindGPUGraphicsPipeline(pass, preview->plant_alpha_pipeline);
		draw_plant_part(preview, pass, preview->atlas_texture, true, NULL, true, &preview->plant_materials[PLANT_MATERIAL_FOLIAGE], view, wind,
			preview->first_index[PLANT_MATERIAL_FOLIAGE], counts[PLANT_MATERIAL_FOLIAGE], false);
		draw_plant_part(preview, pass, preview->atlas_texture, true, NULL, false, &preview->plant_materials[PLANT_MATERIAL_FRUIT], view, wind,
			preview->first_index[PLANT_MATERIAL_FRUIT], counts[PLANT_MATERIAL_FRUIT], false);
	}
}
