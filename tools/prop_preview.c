// The prop editor's view of the prop. See prop_preview.h.

#include "prop_preview.h"

#include <SDL3/SDL.h>

#include "lights.h"
#include "math3d.h"
#include "object_model.h"
#include "scene_uniforms.h"
#include "season.h"
#include "stb_image.h"

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

// object.frag's material block (the ground)
typedef struct
{
	float highlight_parameters[4];
	int32_t alpha_tested;
	int32_t pbr;
	int32_t lighting_normals;
	int32_t padding;
	float deciduous[4];               // (none)
} ground_uniforms_t;

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
	float relief[4];                              // (a building's thatch only: none)
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

void prop_preview_sky(float color_out[4])
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

// The ground: a square of earth with a lighter line every meter
static bool make_ground(prop_preview_t *preview)
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

			texel[0] = (uint8_t)(line ? 170 : 108 + noise);
			texel[1] = (uint8_t)(line ? 160 : 100 + noise);
			texel[2] = (uint8_t)(line ? 130 : 72 + noise);
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

static SDL_GPUGraphicsPipeline *make_pipeline(const char *vertex_name, int vertex_uniforms, const char *fragment_name, const char *name)
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
		info.vertex_buffers = vertex_buffers;
		info.vertex_buffer_count = 3;
		info.vertex_attributes = vertex_attributes;
		info.vertex_attribute_count = 7;
		info.cull = SDL_GPU_CULLMODE_BACK;
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

bool prop_preview_create(prop_preview_t *preview)
{
	SDL_zerop(preview);
	preview->ground_pipeline = make_pipeline("object.vert", 2, "object.frag", "prop preview (ground)");
	preview->prop_pipeline = make_pipeline("prop.vert", 1, "prop.frag", "prop preview");
	if (preview->ground_pipeline == NULL || preview->prop_pipeline == NULL)
	{
		return false;
	}
	{
		float instance[INSTANCE_FLOATS] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f };
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

void prop_preview_destroy(prop_preview_t *preview)
{
	SDL_GPUBuffer **buffers[] = { &preview->vertex_buffer, &preview->tangent_buffer, &preview->index_buffer, &preview->instance_buffer,
		&preview->light_buffers[0], &preview->light_buffers[1], &preview->ground_vertex_buffer, &preview->ground_tangent_buffer,
		&preview->ground_index_buffer };
	SDL_GPUTexture **textures[] = { &preview->ground_texture, &preview->shadow_dummy, &preview->neutral };

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
	for (int m = 0; m < PROP_MAX_MATERIALS; m++)
	{
		for (int k = 0; k < 3; k++)
		{
			release_texture(&preview->materials[m].textures[k]);
		}
		release_texture(&preview->materials[m].solid[0]);
		release_texture(&preview->materials[m].solid[1]);
	}
	if (preview->ground_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->ground_pipeline);
	}
	if (preview->prop_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, preview->prop_pipeline);
	}
	SDL_zerop(preview);
}

void prop_preview_set_mesh(prop_preview_t *preview, const prop_mesh_t *mesh)
{
	float *vertices;
	float *tangents;

	release_buffer(&preview->vertex_buffer);
	release_buffer(&preview->tangent_buffer);
	release_buffer(&preview->index_buffer);
	preview->has_prop = false;
	if (mesh == NULL || mesh->vertex_count == 0 || mesh->index_count == 0)
	{
		return;
	}
	vertices = SDL_malloc(sizeof(float) * 8 * mesh->vertex_count);
	tangents = SDL_malloc(sizeof(float) * 4 * mesh->vertex_count);
	if (vertices == NULL || tangents == NULL)
	{
		SDL_free(vertices);
		SDL_free(tangents);
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
	preview->vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 8 * mesh->vertex_count));
	preview->tangent_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 4 * mesh->vertex_count));
	preview->index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, (uint32_t)(sizeof(uint32_t) * mesh->index_count));
	if (preview->vertex_buffer != NULL && preview->tangent_buffer != NULL && preview->index_buffer != NULL)
	{
		gpu_upload_buffer(preview->vertex_buffer, 0, vertices, sizeof(float) * 8 * mesh->vertex_count);
		gpu_upload_buffer(preview->tangent_buffer, 0, tangents, sizeof(float) * 4 * mesh->vertex_count);
		gpu_upload_buffer(preview->index_buffer, 0, mesh->indices, sizeof(uint32_t) * mesh->index_count);
		// Each material's own, then its levels of detail (as the game draws
		// them); its factors as solid textures, for where it has none
		preview->material_count = mesh->material_count;
		for (int m = 0; m < mesh->material_count; m++)
		{
			const prop_material_t *material = &mesh->materials[m];
			bool has = material->lod_index_count[0] > 0;
			uint8_t color[4];
			uint8_t arm[4] = { 255, 0, 0, 255 };

			preview->materials[m].level_first[0] = (uint32_t)material->first_index;
			preview->materials[m].level_count[0] = (uint32_t)material->own_index_count;
			for (int k = 0; k < 3; k++)
			{
				preview->materials[m].level_first[k + 1] = has ? preview->materials[m].level_first[k] + preview->materials[m].level_count[k]
					: preview->materials[m].level_first[0];
				preview->materials[m].level_count[k + 1] = has ? (uint32_t)material->lod_index_count[k] : preview->materials[m].level_count[0];
			}
			preview->materials[m].triplanar = material->triplanar;
			preview->materials[m].weathered = material->weathered;
			preview->materials[m].texture_meters = material->texture_meters;
			for (int k = 0; k < 3; k++)
			{
				float value = material->base_color[k];

				value = value <= 0.0031308f ? value * 12.92f : 1.055f * SDL_powf(value, 1.0f / 2.4f) - 0.055f;
				color[k] = (uint8_t)SDL_clamp((int)(value * 255.0f + 0.5f), 0, 255);
			}
			color[3] = 255;
			arm[1] = (uint8_t)SDL_clamp((int)(material->roughness * 255.0f + 0.5f), 0, 255);
			arm[2] = (uint8_t)SDL_clamp((int)(material->metallic * 255.0f + 0.5f), 0, 255);
			release_texture(&preview->materials[m].solid[0]);
			release_texture(&preview->materials[m].solid[1]);
			preview->materials[m].solid[0] = solid_texture(color[0], color[1], color[2], 255);
			preview->materials[m].solid[1] = solid_texture(arm[0], arm[1], arm[2], 255);
		}
		preview->moss = mesh->moss;
		preview->lichen = mesh->lichen;
		SDL_memcpy(preview->minimum, mesh->minimum, sizeof(preview->minimum));
		SDL_memcpy(preview->maximum, mesh->maximum, sizeof(preview->maximum));
		preview->has_prop = true;
	}
	SDL_free(vertices);
	SDL_free(tangents);
}

void prop_preview_set_textures(prop_preview_t *preview, const prop_mesh_t *mesh, const char *data_directory, bool reload)
{
	for (int m = 0; m < mesh->material_count && m < PROP_MAX_MATERIALS; m++)
	{
		for (int k = 0; k < 3; k++)
		{
			const char *name = mesh->materials[m].textures[k];
			char path[PROP_PATH_LENGTH * 2];
			size_t size = 0;
			void *bytes;
			int width = 0;
			int height = 0;
			int channels = 0;
			uint8_t *pixels;

			if (!reload && SDL_strcmp(preview->materials[m].texture_names[k], name) == 0)
			{
				continue;
			}
			release_texture(&preview->materials[m].textures[k]);
			SDL_strlcpy(preview->materials[m].texture_names[k], name, sizeof(preview->materials[m].texture_names[k]));
			if (name[0] == '\0')
			{
				continue;
			}
			SDL_snprintf(path, sizeof(path), "%s/%s", data_directory, name);
			bytes = SDL_LoadFile(path, &size);
			pixels = bytes != NULL ? stbi_load_from_memory(bytes, (int)size, &width, &height, &channels, 4) : NULL;
			SDL_free(bytes);
			if (pixels != NULL)
			{
				preview->materials[m].textures[k] = object_texture_upload(pixels, width, height);
				stbi_image_free(pixels);
			}
			else
			{
				SDL_Log("Prop preview: cannot read the texture '%s'", path);
			}
		}
	}
}

void prop_preview_draw(prop_preview_t *preview, SDL_GPURenderPass *pass, const prop_view_t *view)
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

	if (pass == NULL || preview->prop_pipeline == NULL)
	{
		return;
	}
	projection = mat4_perspective(degrees_to_radians(view->vertical_fov_degrees), SDL_max(view->aspect, 0.1f), 0.05f, 2000.0f);
	view_matrix = mat4_look_at(vec3_make(view->eye[0], view->eye[1], view->eye[2]), vec3_make(view->target[0], view->target[1], view->target[2]),
		vec3_make(0.0f, 1.0f, 0.0f));
	view_projection = mat4_multiply(projection, view_matrix);
	mat4_inverse(view_projection, &inverse);

	// The scene as the shaders know it: the sun, the sky, no shadows
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
	prop_preview_sky(sky);
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
	scene.weather[1] = SDL_clamp(view->wetness, 0.0f, 1.0f);
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, &scene, sizeof(scene));
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, &scene, sizeof(scene));
	SDL_zeroa(wind);

	SDL_BindGPUFragmentSamplers(pass, 0, fixed, 3);
	SDL_BindGPUFragmentSamplers(pass, 4, &cloud_shadow, 1);
	// (the game's ground and water: none here, stand ins)
	scene_ground_bind(pass, 8);
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
		SDL_GPUTextureSamplerBinding color = { preview->ground_texture, gpu.linear_repeat };
		SDL_GPUTextureSamplerBinding maps[3] =
		{
			{ object_default_texture(OBJECT_DEFAULT_NORMAL), gpu.linear_repeat },
			{ object_default_texture(OBJECT_DEFAULT_ORM), gpu.linear_repeat },
			{ object_default_texture(OBJECT_DEFAULT_EMISSIVE), gpu.linear_repeat },
		};
		ground_uniforms_t material;

		SDL_zero(material);
		SDL_BindGPUGraphicsPipeline(pass, preview->ground_pipeline);
		SDL_BindGPUVertexBuffers(pass, 0, bindings, 3);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material, sizeof(material));
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, wind, sizeof(wind));
		SDL_BindGPUFragmentSamplers(pass, 3, &color, 1);
		SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
		SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
	}
	if (!preview->has_prop)
	{
		return;
	}
	{
		SDL_GPUBufferBinding bindings[3] =
		{
			{ preview->vertex_buffer, 0 },
			{ preview->instance_buffer, 0 },
			{ preview->tangent_buffer, 0 },
		};
		SDL_GPUBufferBinding index_binding = { preview->index_buffer, 0 };
		int level = view->detail < 0 ? 0 : (view->detail > 3 ? 3 : view->detail);

		SDL_BindGPUGraphicsPipeline(pass, preview->prop_pipeline);
		SDL_BindGPUVertexBuffers(pass, 0, bindings, 3);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		for (int m = 0; m < preview->material_count; m++)
		{
			SDL_GPUTextureSamplerBinding color = { preview->materials[m].textures[0] != NULL ? preview->materials[m].textures[0]
				: preview->materials[m].solid[0], gpu.linear_repeat };
			SDL_GPUTextureSamplerBinding maps[3] =
			{
				{ preview->materials[m].textures[1] != NULL ? preview->materials[m].textures[1] : object_default_texture(OBJECT_DEFAULT_NORMAL),
					gpu.linear_repeat },
				{ preview->materials[m].textures[2] != NULL ? preview->materials[m].textures[2] : preview->materials[m].solid[1], gpu.linear_repeat },
				{ object_default_texture(OBJECT_DEFAULT_EMISSIVE), gpu.linear_repeat },
			};
			prop_uniforms_t material;

			if (color.texture == NULL || maps[1].texture == NULL)
			{
				continue;
			}
			SDL_zero(material);
			default_look(material.look);
			material.pbr = view->pbr ? 1 : 0;
			material.triplanar = preview->materials[m].triplanar ? 1 : 0;
			material.weathering[0] = preview->materials[m].texture_meters;
			material.weathering[1] = preview->materials[m].weathered ? preview->lichen : -1.0f;
			material.weathering[2] = season_snow(view->month, view->snow_first_month, view->snow_last_month);
			material.weathering[3] = preview->moss;
			material.show[0] = (float)view->show;
			SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &material, sizeof(material));
			SDL_BindGPUFragmentSamplers(pass, 3, &color, 1);
			SDL_BindGPUFragmentSamplers(pass, 5, maps, 3);
			SDL_DrawGPUIndexedPrimitives(pass, preview->materials[m].level_count[level], 1, preview->materials[m].level_first[level], 0, 0);
		}
	}
}
