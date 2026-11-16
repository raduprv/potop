// The house drawn as the game draws it. See house_preview.h.

#include "house_preview.h"

#include <SDL3/SDL.h>

#include "lights.h"
#include "math3d.h"
#include "object_model.h"
#include "scene_uniforms.h"
#include "season.h"
#include "stb_image.h"

#define GROUND_HALF_WIDTH 20.0f
#define GROUND_TEXTURE_SIZE 64
// (placement, rotation, fade; then the model's look as objects_cull.comp
// writes it: age, its spread, lichen, moss: building.vert's)
#define INSTANCE_FLOATS 13

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

// The buildings' (building.vert): the same with the vertices' weathering
// as a fourth buffer (object_file_weathering_vertex_t)
static const SDL_GPUVertexBufferDescription building_vertex_buffers[4] =
{
	{ 0, sizeof(float) * 8, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 1, sizeof(float) * INSTANCE_FLOATS, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0 },
	{ 2, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
	{ 3, sizeof(float) * 4, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
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
	{ 8, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 9 },
};

// object.frag's material block
typedef struct
{
	float highlight_parameters[4];
	int32_t alpha_tested;
	int32_t pbr;
	int32_t lighting_normals;
	int32_t padding;
	float deciduous[4];               // a climber dropping its leaves (objects.c deciduous_season)
} object_uniforms_t;

// A deciduous climber through the year: as objects.c's deciduous_season
static void house_preview_deciduous(float month, float season_out[4])
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
		red = SDL_clamp(1.0f - (m - 4.5f), 0.0f, 1.0f) * 0.5f;
		gone = SDL_clamp(1.0f - (m - 4.5f), 0.0f, 1.0f);
	}
	season_out[0] = red;
	season_out[1] = gone;
	season_out[2] = 1.0f;
	season_out[3] = 0.0f;
}

// prop.frag's
typedef struct
{
	float highlight_parameters[4];
	int32_t alpha_tested;
	int32_t pbr;
	int32_t lighting_normals;
	int32_t triplanar;
	float weathering[4];
	float show[4];
	float relief[4];                              // a thatch's relief (objects.c's THATCH_RELIEF_METERS)
	float look[8];                               // the buildings\' look (prop_body.glsl): the game\'s defaults
} prop_uniforms_t;

// The buildings' look as the game's settings have it by default (settings.c)
static void default_look(float look[8])
{
	static const float defaults[8] = { 0.2f, 0.35f, 0.05f, 0.55f, 0.35f, 0.9f, 0.6f, 0.0f };

	SDL_memcpy(look, defaults, sizeof(defaults));
}

static float srgb_to_linear(float value)
{
	return value <= 0.04045f ? value / 12.92f : SDL_powf((value + 0.055f) / 1.055f, 2.4f);
}

void house_preview_sky(float color_out[4])
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

static uint8_t linear_to_byte(float value)
{
	value = value <= 0.0031308f ? value * 12.92f : 1.055f * SDL_powf(value, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)SDL_clamp((int)(value * 255.0f + 0.5f), 0, 255);
}

static bool make_ground(house_preview_t *preview)
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

			texel[0] = (uint8_t)(line ? 150 : 96 + noise);
			texel[1] = (uint8_t)(line ? 145 : 98 + noise);
			texel[2] = (uint8_t)(line ? 120 : 66 + noise);
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

static SDL_GPUGraphicsPipeline *make_pipeline(const char *vertex_name, int vertex_uniforms, const char *fragment_name, bool two_sided,
	bool building, const char *name)
{
	SDL_GPUShader *vertex = gpu_load_shader(vertex_name, 0, 0, (uint32_t)vertex_uniforms);
	SDL_GPUShader *fragment = gpu_load_shader(fragment_name, OBJECT_FRAGMENT_SAMPLERS, LIGHTS_STORAGE_BUFFERS, 2);
	SDL_GPUGraphicsPipeline *pipeline = NULL;

	if (vertex != NULL && fragment != NULL)
	{
		gpu_pipeline_info_t info;

		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = building ? building_vertex_buffers : vertex_buffers;
		info.vertex_buffer_count = building ? 4 : 3;
		info.vertex_attributes = building ? building_vertex_attributes : vertex_attributes;
		info.vertex_attribute_count = building ? 9 : 7;
		info.cull = two_sided ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
		info.depth_test = true;
		info.depth_write = true;
		pipeline = gpu_make_pipeline(&info, name);
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

bool house_preview_create(house_preview_t *preview)
{
	preview->object_pipeline = make_pipeline("object.vert", 2, "object.frag", false, false, "house view (objects)");
	preview->object_pipeline_two_sided = make_pipeline("object.vert", 2, "object.frag", true, false, "house view (two sided)");
	preview->prop_pipeline = make_pipeline("prop.vert", 1, "prop.frag", false, false, "house view (weathered)");
	preview->building_pipeline = make_pipeline("building.vert", 1, "building.frag", false, true, "house view (aged)");
	if (preview->object_pipeline == NULL || preview->object_pipeline_two_sided == NULL || preview->prop_pipeline == NULL
		|| preview->building_pipeline == NULL)
	{
		return false;
	}
	{
		float instance[INSTANCE_FLOATS] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.6f, 0.7f };
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
	return preview->shadow_dummy != NULL && preview->neutral != NULL && make_ground(preview);
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

// The house drawn so far let go (its textures stay in the cache)
static void release_house(house_preview_t *preview)
{
	release_buffer(&preview->vertex_buffer);
	release_buffer(&preview->tangent_buffer);
	release_buffer(&preview->weathering_buffer);
	release_buffer(&preview->index_buffer);
	for (int m = 0; m < preview->material_count; m++)
	{
		for (int k = 0; k < 3; k++)
		{
			release_texture(&preview->materials[m].solid[k]);
		}
		SDL_zeroa(preview->materials[m].textures);
	}
	preview->material_count = 0;
	preview->has_house = false;
}

// A texture by its name (under the data directory): read once, kept
static SDL_GPUTexture *cached_texture(house_preview_t *preview, const char *name, const char *data_directory)
{
	char path[HOUSE_PATH_LENGTH * 2];
	size_t size = 0;
	void *bytes;
	int width = 0;
	int height = 0;
	int channels = 0;
	uint8_t *pixels;
	SDL_GPUTexture *texture = NULL;

	for (int i = 0; i < preview->cache_count; i++)
	{
		if (SDL_strcmp(preview->cache[i].name, name) == 0)
		{
			return preview->cache[i].texture;
		}
	}
	SDL_snprintf(path, sizeof(path), "%s/%s", data_directory, name);
	bytes = SDL_LoadFile(path, &size);
	pixels = bytes != NULL ? stbi_load_from_memory(bytes, (int)size, &width, &height, &channels, 4) : NULL;
	SDL_free(bytes);
	if (pixels != NULL)
	{
		texture = object_texture_upload(pixels, width, height);
		stbi_image_free(pixels);
	}
	else
	{
		SDL_Log("house preview: cannot read the texture '%s'", path);
	}
	if (texture != NULL && preview->cache_count < HOUSE_PREVIEW_TEXTURE_CACHE)
	{
		SDL_strlcpy(preview->cache[preview->cache_count].name, name, sizeof(preview->cache[0].name));
		preview->cache[preview->cache_count].texture = texture;
		preview->cache_count++;
	}
	return texture;
}

void house_preview_forget_textures(house_preview_t *preview)
{
	for (int i = 0; i < preview->cache_count; i++)
	{
		release_texture(&preview->cache[i].texture);
	}
	preview->cache_count = 0;
	for (int m = 0; m < preview->material_count; m++)
	{
		SDL_zeroa(preview->materials[m].textures);
	}
}

bool house_preview_set_mesh(house_preview_t *preview, const house_mesh_t *mesh, const char *data_directory)
{
	float *vertices = SDL_malloc(sizeof(float) * 8 * mesh->vertex_count);
	float *tangents = SDL_malloc(sizeof(float) * 4 * mesh->vertex_count);

	release_house(preview);
	if (vertices == NULL || tangents == NULL || mesh->vertex_count == 0 || mesh->index_count == 0)
	{
		SDL_free(vertices);
		SDL_free(tangents);
		return false;
	}
	for (size_t i = 0; i < mesh->vertex_count; i++)
	{
		const object_file_vertex_t *source = &mesh->vertices[i];

		SDL_memcpy(&vertices[i * 8], source->position, sizeof(float) * 3);
		SDL_memcpy(&vertices[i * 8 + 3], source->normal, sizeof(float) * 3);
		SDL_memcpy(&vertices[i * 8 + 6], source->uv, sizeof(float) * 2);
		SDL_memcpy(&tangents[i * 4], source->tangent, sizeof(float) * 4);
	}
	preview->vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 8 * mesh->vertex_count));
	preview->tangent_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 4 * mesh->vertex_count));
	preview->index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, (uint32_t)(sizeof(uint32_t) * mesh->index_count));
	preview->weathering_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(object_file_weathering_vertex_t) * mesh->vertex_count));
	if (preview->vertex_buffer == NULL || preview->tangent_buffer == NULL || preview->index_buffer == NULL || preview->weathering_buffer == NULL)
	{
		SDL_free(vertices);
		SDL_free(tangents);
		return false;
	}
	gpu_upload_buffer(preview->weathering_buffer, 0, mesh->weathering, sizeof(object_file_weathering_vertex_t) * mesh->vertex_count);
	gpu_upload_buffer(preview->vertex_buffer, 0, vertices, sizeof(float) * 8 * mesh->vertex_count);
	gpu_upload_buffer(preview->tangent_buffer, 0, tangents, sizeof(float) * 4 * mesh->vertex_count);
	gpu_upload_buffer(preview->index_buffer, 0, mesh->indices, sizeof(uint32_t) * mesh->index_count);
	SDL_free(vertices);
	SDL_free(tangents);
	preview->material_count = mesh->material_count;
	for (int m = 0; m < mesh->material_count; m++)
	{
		const house_material_t *material = &mesh->materials[m];
		house_preview_material_t *target = &preview->materials[m];
		bool has = material->lod_index_count[0] > 0 || material->lod_index_count[1] > 0 || material->lod_index_count[2] > 0;

		SDL_strlcpy(target->name, material->name, sizeof(target->name));
		target->level_first[0] = (uint32_t)material->first_index;
		target->level_count[0] = (uint32_t)material->own_index_count;
		for (int k = 0; k < 3; k++)
		{
			target->level_first[k + 1] = has ? target->level_first[k] + target->level_count[k] : target->level_first[0];
			target->level_count[k + 1] = has ? (uint32_t)material->lod_index_count[k] : target->level_count[0];
		}
		target->flags = material->flags;
		target->texture_meters = material->texture_meters;
		target->solid[0] = solid_texture(linear_to_byte(material->base_color[0]), linear_to_byte(material->base_color[1]),
			linear_to_byte(material->base_color[2]), 255);
		target->solid[1] = solid_texture(255, (uint8_t)SDL_clamp((int)(material->roughness * 255.0f + 0.5f), 0, 255),
			(uint8_t)SDL_clamp((int)(material->metallic * 255.0f + 0.5f), 0, 255), 255);
		target->solid[2] = solid_texture(linear_to_byte(material->emissive[0]), linear_to_byte(material->emissive[1]), linear_to_byte(material->emissive[2]),
			255);
		for (int k = 0; k < 4; k++)
		{
			target->textures[k] = material->textures[k][0] != '\0' ? cached_texture(preview, material->textures[k], data_directory) : NULL;
		}
	}
	preview->moss = mesh->moss;
	preview->lichen = mesh->lichen;
	// (the house's lichen and moss go with its instance, as the game's)
	{
		float look[4] = { 0.0f, 0.0f, preview->lichen > 0.0f ? preview->lichen : 0.6f, preview->moss > 0.0f ? preview->moss : 0.7f };

		gpu_upload_buffer(preview->instance_buffer, sizeof(float) * 9, look, sizeof(look));
	}
	SDL_memcpy(preview->minimum, mesh->minimum, sizeof(preview->minimum));
	SDL_memcpy(preview->maximum, mesh->maximum, sizeof(preview->maximum));
	preview->has_house = true;
	return true;
}

void house_preview_draw(house_preview_t *preview, SDL_GPURenderPass *pass, const house_view_t *view)
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
	float snow = season_snow(view->month, SDL_roundf(view->snow_first_month), SDL_roundf(view->snow_last_month));

	projection = mat4_perspective(degrees_to_radians(view->vertical_fov_degrees > 1.0f ? view->vertical_fov_degrees : 38.0f), SDL_max(view->aspect, 0.1f), 0.05f, 2000.0f);
	view_matrix = mat4_look_at(vec3_make(view->eye[0], view->eye[1], view->eye[2]), vec3_make(view->target[0], view->target[1], view->target[2]),
		vec3_make(0.0f, 1.0f, 0.0f));
	view_projection = mat4_multiply(projection, view_matrix);
	mat4_inverse(view_projection, &inverse);
	SDL_zero(scene);
	scene_uniforms_set_look(&scene, projection, view->eye, view->target);
	SDL_memcpy(scene.light_view_projection, mat4_identity().m, sizeof(scene.light_view_projection));
	SDL_memcpy(scene.near_light_view_projection, mat4_identity().m, sizeof(scene.near_light_view_projection));
	SDL_memcpy(scene.eye_position, view->eye, sizeof(scene.eye_position));
	scene.time_seconds = 1.0f;
	scene.sun_direction[0] = SDL_cosf(elevation) * SDL_cosf(azimuth);
	scene.sun_direction[1] = SDL_sinf(elevation);
	scene.sun_direction[2] = SDL_cosf(elevation) * SDL_sinf(azimuth);
	scene.ambient_intensity = view->ambient > 0.0f ? view->ambient : 0.5f;
	house_preview_sky(sky);
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
	scene.aurora[0] = -SDL_cosf(azimuth);
	scene.aurora[1] = -SDL_sinf(azimuth);
	scene.weather[1] = SDL_clamp(view->wetness, 0.0f, 1.0f);
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, &scene, sizeof(scene));
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, &scene, sizeof(scene));
	SDL_zeroa(wind);
	SDL_BindGPUFragmentSamplers(pass, 0, fixed, 3);
	SDL_BindGPUFragmentSamplers(pass, 4, &cloud_shadow, 1);
	scene_ground_bind(pass, 8);
	SDL_BindGPUFragmentStorageBuffers(pass, 0, preview->light_buffers, 2);
	// The ground
	{
		SDL_GPUBufferBinding bindings[3] = { { preview->ground_vertex_buffer, 0 }, { preview->instance_buffer, 0 }, { preview->ground_tangent_buffer, 0 } };
		SDL_GPUBufferBinding index_binding = { preview->ground_index_buffer, 0 };
		SDL_GPUTextureSamplerBinding color = { preview->ground_texture, gpu.linear_repeat };
		SDL_GPUTextureSamplerBinding maps[3] =
		{
			{ object_default_texture(OBJECT_DEFAULT_NORMAL), gpu.linear_repeat },
			{ object_default_texture(OBJECT_DEFAULT_ORM), gpu.linear_repeat },
			{ object_default_texture(OBJECT_DEFAULT_EMISSIVE), gpu.linear_repeat },
		};
		object_uniforms_t material;

		SDL_zero(material);
		SDL_BindGPUGraphicsPipeline(pass, preview->object_pipeline);
		SDL_BindGPUVertexBuffers(pass, 0, bindings, 3);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material, sizeof(material));
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, wind, sizeof(wind));
		SDL_BindGPUFragmentSamplers(pass, 3, &color, 1);
		SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
		SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
	}
	// The house, material by material, each through its shader
	if (preview->has_house)
	{
		SDL_GPUBufferBinding bindings[4] =
		{
			{ preview->vertex_buffer, 0 }, { preview->instance_buffer, 0 }, { preview->tangent_buffer, 0 }, { preview->weathering_buffer, 0 },
		};
		SDL_GPUBufferBinding index_binding = { preview->index_buffer, 0 };
		int level = SDL_clamp(view->detail, 0, 3);

		SDL_BindGPUVertexBuffers(pass, 0, bindings, 4);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		for (int m = 0; m < preview->material_count; m++)
		{
			house_preview_material_t *material = &preview->materials[m];
			bool collision = (material->flags & OBJECT_MATERIAL_COLLISION_ONLY) != 0;
			bool weathered = (material->flags & OBJECT_MATERIAL_WEATHERED) != 0 && (material->flags & OBJECT_MATERIAL_ALPHA_TESTED) == 0;
			bool aged = (material->flags & OBJECT_MATERIAL_AGED) != 0 && (material->flags & OBJECT_MATERIAL_ALPHA_TESTED) == 0;
			bool two_sided = (material->flags & OBJECT_MATERIAL_DOUBLE_SIDED) != 0;
			SDL_GPUTextureSamplerBinding color = { material->textures[0] != NULL ? material->textures[0] : material->solid[0], gpu.linear_repeat };
			SDL_GPUTextureSamplerBinding maps[3] =
			{
				{ material->textures[1] != NULL ? material->textures[1] : object_default_texture(OBJECT_DEFAULT_NORMAL), gpu.linear_repeat },
				{ material->textures[2] != NULL ? material->textures[2] : material->solid[1], gpu.linear_repeat },
				{ material->textures[3] != NULL ? material->textures[3] : material->solid[2], gpu.linear_repeat },
			};
			uint32_t count = material->level_count[level];
			uint32_t first = material->level_first[level];

			if (view->hide != NULL && SDL_strcmp(view->hide, material->name) == 0)
			{
				continue;
			}
			if (collision)
			{
				if (!view->show_collision)
				{
					continue;
				}
				count = material->level_count[0];
				first = material->level_first[0];
			}
			if (count == 0)
			{
				continue;
			}
			if (aged && !collision)
			{
				// As the game draws a building (objects.c building_material_uniforms)
				prop_uniforms_t uniforms;

				SDL_zero(uniforms);
				default_look(uniforms.look);
				uniforms.pbr = 1;
				uniforms.triplanar = (material->flags & OBJECT_MATERIAL_TRIPLANAR) != 0 ? 1 : 0;
				uniforms.weathering[0] = uniforms.triplanar != 0 ? material->texture_meters : 1.0f;
				uniforms.weathering[1] = preview->lichen > 0.0f ? preview->lichen : 0.6f;
				uniforms.weathering[2] = snow;
				uniforms.weathering[3] = preview->moss > 0.0f ? preview->moss : 0.7f;
				uniforms.show[0] = (float)view->show;
				uniforms.show[1] = view->age;
				uniforms.show[2] = 0.0f;
				uniforms.show[3] = view->dirt_meters;
				uniforms.relief[0] = (material->flags & OBJECT_MATERIAL_THATCH) != 0 ? 1.0f : 0.0f;
				uniforms.relief[1] = 0.08f;
				SDL_BindGPUGraphicsPipeline(pass, preview->building_pipeline);
				SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
			}
			else if (weathered && !collision)
			{
				prop_uniforms_t uniforms;

				SDL_zero(uniforms);
				default_look(uniforms.look);
				uniforms.pbr = 1;
				uniforms.triplanar = 0;
				uniforms.weathering[0] = 1.0f;
				uniforms.weathering[1] = preview->lichen;
				uniforms.weathering[2] = snow;
				uniforms.weathering[3] = preview->moss;
				SDL_BindGPUGraphicsPipeline(pass, preview->prop_pipeline);
				SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
			}
			else
			{
				object_uniforms_t uniforms;

				SDL_zero(uniforms);
				uniforms.pbr = 1;
				uniforms.alpha_tested = (material->flags & OBJECT_MATERIAL_ALPHA_TESTED) != 0 ? 1 : 0;
				if ((material->flags & OBJECT_MATERIAL_DECIDUOUS) != 0)
				{
					house_preview_deciduous(view->month, uniforms.deciduous);
				}
				SDL_BindGPUGraphicsPipeline(pass, two_sided ? preview->object_pipeline_two_sided : preview->object_pipeline);
				SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
				SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, wind, sizeof(wind));
			}
			SDL_BindGPUFragmentSamplers(pass, 3, &color, 1);
			SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
			SDL_DrawGPUIndexedPrimitives(pass, count, 1, first, 0, 0);
		}
	}
}

void house_preview_destroy(house_preview_t *preview)
{
	release_house(preview);
	house_preview_forget_textures(preview);
	release_buffer(&preview->instance_buffer);
	release_buffer(&preview->light_buffers[0]);
	release_buffer(&preview->light_buffers[1]);
	release_buffer(&preview->ground_vertex_buffer);
	release_buffer(&preview->ground_tangent_buffer);
	release_buffer(&preview->ground_index_buffer);
	release_texture(&preview->ground_texture);
	release_texture(&preview->shadow_dummy);
	release_texture(&preview->neutral);
	if (preview->object_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->object_pipeline);
	}
	if (preview->object_pipeline_two_sided != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->object_pipeline_two_sided);
	}
	if (preview->prop_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->prop_pipeline);
	}
	if (preview->building_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->building_pipeline);
	}
	SDL_zerop(preview);
}
