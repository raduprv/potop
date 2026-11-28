// WreckingBall's renderer: see wb_render.h

#include "wb_render.h"

#include <math.h>

#include "stb_image_write.h"
#include "wb_math.h"

#define SHADOW_MAP_SIZE 2048
#define PRISM_SIDES 12
#define SPHERE_STACKS 12
#define SPHERE_SLICES 16
#define INSTANCES_MAX 16384

typedef struct
{
	float position[3];
	float normal[3];
} vertex_t;

// What the shaders read of an instance (solid.vert's inputs 2 to 5)
typedef struct
{
	float position[4];
	float rotation[4];
	float scale[4];
	float color[4];                    // linear; alpha 0 marks the ground (drawn with a grid)
} instance_t;

typedef struct
{
	SDL_GPUBuffer *vertex_buffer;
	SDL_GPUBuffer *index_buffer;
	uint32_t index_count;
	instance_t *instances;             // this frame's
	int instance_count;
} mesh_slot_t;

typedef struct
{
	float view_projection[16];
	float light_view_projection[16];
} frame_uniforms_t;

typedef struct
{
	float sun_direction[4];
	float sun_color[4];
	float sky_color[4];
	float ground_color[4];
	float fog_color[4];                // alpha: 1 / the distance at which the fog is whole
	float eye[4];
	float shadow_texel[4];             // x: one texel of the shadow map, in its coordinates
} light_uniforms_t;

struct wb_render_s
{
	SDL_GPUDevice *device;
	SDL_Window *window;
	int width;
	int height;
	SDL_GPUTextureFormat color_format;
	SDL_GPUTextureFormat depth_format;
	SDL_GPUTextureFormat shadow_format;
	SDL_GPUTexture *color_texture;
	SDL_GPUTexture *depth_texture;
	SDL_GPUTexture *shadow_texture;
	SDL_GPUSampler *shadow_sampler;
	SDL_GPUGraphicsPipeline *scene_pipeline;
	SDL_GPUGraphicsPipeline *shadow_pipeline;
	mesh_slot_t meshes[WB_MESH_COUNT];
	SDL_GPUBuffer *instance_buffer;
	SDL_GPUTransferBuffer *instance_transfer;
	SDL_GPUTransferBuffer *download_transfer;
	uint32_t download_size;
	float shadow_center[3];
	float shadow_radius;
	instance_t *all_instances;
};

// ---------------------------------------------------------------------------
// Colors and helpers

static float srgb_to_linear(float value)
{
	return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

static void color_to_linear(uint32_t color, float out[4])
{
	out[0] = srgb_to_linear((float)((color >> 16) & 0xff) / 255.0f);
	out[1] = srgb_to_linear((float)((color >> 8) & 0xff) / 255.0f);
	out[2] = srgb_to_linear((float)(color & 0xff) / 255.0f);
	out[3] = 1.0f;
}

static SDL_GPUShader *load_shader(SDL_GPUDevice *device, const char *directory, const char *name, SDL_GPUShaderStage stage,
	uint32_t sampler_count, uint32_t uniform_buffer_count)
{
	char path[512];
	size_t size = 0;
	void *code;
	SDL_GPUShaderCreateInfo info;
	SDL_GPUShader *shader;

	SDL_snprintf(path, sizeof(path), "%s/%s.spv", directory, name);
	code = SDL_LoadFile(path, &size);
	if (code == NULL)
	{
		SDL_Log("Render: no shader %s: %s", path, SDL_GetError());
		return NULL;
	}
	SDL_zero(info);
	info.code = code;
	info.code_size = size;
	info.entrypoint = "main";
	info.format = SDL_GPU_SHADERFORMAT_SPIRV;
	info.stage = stage;
	info.num_samplers = sampler_count;
	info.num_uniform_buffers = uniform_buffer_count;
	shader = SDL_CreateGPUShader(device, &info);
	SDL_free(code);
	if (shader == NULL)
	{
		SDL_Log("Render: shader %s: %s", path, SDL_GetError());
	}
	return shader;
}

// A buffer with these bytes in it, uploaded now (waits for the upload)
static SDL_GPUBuffer *make_buffer(SDL_GPUDevice *device, SDL_GPUBufferUsageFlags usage, const void *data, uint32_t size)
{
	SDL_GPUBufferCreateInfo buffer_info;
	SDL_GPUTransferBufferCreateInfo transfer_info;
	SDL_GPUBuffer *buffer;
	SDL_GPUTransferBuffer *transfer;
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUCopyPass *copy_pass;
	SDL_GPUTransferBufferLocation source;
	SDL_GPUBufferRegion destination;
	void *mapped;

	SDL_zero(buffer_info);
	buffer_info.usage = usage;
	buffer_info.size = size;
	buffer = SDL_CreateGPUBuffer(device, &buffer_info);
	if (buffer == NULL)
	{
		return NULL;
	}
	SDL_zero(transfer_info);
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = size;
	transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
	if (transfer == NULL)
	{
		SDL_ReleaseGPUBuffer(device, buffer);
		return NULL;
	}
	mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
	SDL_memcpy(mapped, data, size);
	SDL_UnmapGPUTransferBuffer(device, transfer);
	command_buffer = SDL_AcquireGPUCommandBuffer(device);
	copy_pass = SDL_BeginGPUCopyPass(command_buffer);
	SDL_zero(source);
	source.transfer_buffer = transfer;
	SDL_zero(destination);
	destination.buffer = buffer;
	destination.size = size;
	SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);
	SDL_EndGPUCopyPass(copy_pass);
	SDL_SubmitGPUCommandBuffer(command_buffer);
	SDL_ReleaseGPUTransferBuffer(device, transfer);
	return buffer;
}

static bool set_mesh(wb_render_t *render, wb_mesh_t which, const vertex_t *vertices, int vertex_count, const uint16_t *indices, int index_count)
{
	mesh_slot_t *mesh = &render->meshes[which];

	if (mesh->vertex_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(render->device, mesh->vertex_buffer);
		mesh->vertex_buffer = NULL;
	}
	if (mesh->index_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(render->device, mesh->index_buffer);
		mesh->index_buffer = NULL;
	}
	mesh->vertex_buffer = make_buffer(render->device, SDL_GPU_BUFFERUSAGE_VERTEX, vertices, (uint32_t)(sizeof(vertex_t) * (size_t)vertex_count));
	mesh->index_buffer = make_buffer(render->device, SDL_GPU_BUFFERUSAGE_INDEX, indices, (uint32_t)(sizeof(uint16_t) * (size_t)index_count));
	mesh->index_count = (uint32_t)index_count;
	return mesh->vertex_buffer != NULL && mesh->index_buffer != NULL;
}

// ---------------------------------------------------------------------------
// The unit meshes

static void put_vertex(vertex_t *vertex, float x, float y, float z, float normal_x, float normal_y, float normal_z)
{
	vertex->position[0] = x;
	vertex->position[1] = y;
	vertex->position[2] = z;
	vertex->normal[0] = normal_x;
	vertex->normal[1] = normal_y;
	vertex->normal[2] = normal_z;
}

static bool make_box_mesh(wb_render_t *render)
{
	vertex_t vertices[24];
	uint16_t indices[36];
	int vertex_count = 0;
	int index_count = 0;

	// Each face: its normal along one axis, its corners counter clockwise
	// seen from outside (the other two axes, in the order that gives that)
	for (int axis = 0; axis < 3; axis++)
	{
		for (int side = 0; side < 2; side++)
		{
			float sign = side == 0 ? -1.0f : 1.0f;
			int first_axis = (axis + 1) % 3;
			int second_axis = (axis + 2) % 3;
			const float corners[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
			int base = vertex_count;

			for (int c = 0; c < 4; c++)
			{
				float position[3];
				float normal[3] = { 0.0f, 0.0f, 0.0f };

				position[axis] = sign;
				position[first_axis] = corners[c][0] * sign;
				position[second_axis] = corners[c][1];
				normal[axis] = sign;
				put_vertex(&vertices[vertex_count++], position[0], position[1], position[2], normal[0], normal[1], normal[2]);
			}
			indices[index_count++] = (uint16_t)base;
			indices[index_count++] = (uint16_t)(base + 1);
			indices[index_count++] = (uint16_t)(base + 2);
			indices[index_count++] = (uint16_t)base;
			indices[index_count++] = (uint16_t)(base + 2);
			indices[index_count++] = (uint16_t)(base + 3);
		}
	}
	return set_mesh(render, WB_MESH_BOX, vertices, vertex_count, indices, index_count);
}

static bool make_prism_mesh(wb_render_t *render)
{
	vertex_t vertices[(PRISM_SIDES + 1) * 2 + 2 * (PRISM_SIDES + 1)];
	uint16_t indices[PRISM_SIDES * 6 + 2 * PRISM_SIDES * 3];
	int vertex_count = 0;
	int index_count = 0;

	// The sides, smooth: a ring at each end
	for (int s = 0; s <= PRISM_SIDES; s++)
	{
		float angle = 2.0f * SDL_PI_F * (float)s / (float)PRISM_SIDES;
		float normal_y = cosf(angle);
		float normal_z = sinf(angle);

		put_vertex(&vertices[vertex_count++], -1.0f, normal_y, normal_z, 0.0f, normal_y, normal_z);
		put_vertex(&vertices[vertex_count++], 1.0f, normal_y, normal_z, 0.0f, normal_y, normal_z);
	}
	for (int s = 0; s < PRISM_SIDES; s++)
	{
		int a = s * 2;

		indices[index_count++] = (uint16_t)a;
		indices[index_count++] = (uint16_t)(a + 2);
		indices[index_count++] = (uint16_t)(a + 1);
		indices[index_count++] = (uint16_t)(a + 1);
		indices[index_count++] = (uint16_t)(a + 2);
		indices[index_count++] = (uint16_t)(a + 3);
	}
	// The ends: a fan each
	for (int end = 0; end < 2; end++)
	{
		float sign = end == 0 ? -1.0f : 1.0f;
		int middle = vertex_count;

		put_vertex(&vertices[vertex_count++], sign, 0.0f, 0.0f, sign, 0.0f, 0.0f);
		for (int s = 0; s < PRISM_SIDES; s++)
		{
			float angle = 2.0f * SDL_PI_F * (float)s / (float)PRISM_SIDES;

			put_vertex(&vertices[vertex_count++], sign, cosf(angle), sinf(angle), sign, 0.0f, 0.0f);
		}
		for (int s = 0; s < PRISM_SIDES; s++)
		{
			int a = middle + 1 + s;
			int b = middle + 1 + (s + 1) % PRISM_SIDES;

			indices[index_count++] = (uint16_t)middle;
			indices[index_count++] = (uint16_t)(end == 0 ? b : a);
			indices[index_count++] = (uint16_t)(end == 0 ? a : b);
		}
	}
	return set_mesh(render, WB_MESH_PRISM, vertices, vertex_count, indices, index_count);
}

static bool make_sphere_mesh(wb_render_t *render)
{
	vertex_t vertices[(SPHERE_STACKS + 1) * (SPHERE_SLICES + 1)];
	uint16_t indices[SPHERE_STACKS * SPHERE_SLICES * 6];
	int vertex_count = 0;
	int index_count = 0;

	for (int stack = 0; stack <= SPHERE_STACKS; stack++)
	{
		float polar = SDL_PI_F * (float)stack / (float)SPHERE_STACKS;

		for (int slice = 0; slice <= SPHERE_SLICES; slice++)
		{
			float around = 2.0f * SDL_PI_F * (float)slice / (float)SPHERE_SLICES;
			float x = sinf(polar) * cosf(around);
			float y = cosf(polar);
			float z = sinf(polar) * sinf(around);

			put_vertex(&vertices[vertex_count++], x, y, z, x, y, z);
		}
	}
	for (int stack = 0; stack < SPHERE_STACKS; stack++)
	{
		for (int slice = 0; slice < SPHERE_SLICES; slice++)
		{
			int a = stack * (SPHERE_SLICES + 1) + slice;
			int b = a + SPHERE_SLICES + 1;

			indices[index_count++] = (uint16_t)a;
			indices[index_count++] = (uint16_t)(a + 1);
			indices[index_count++] = (uint16_t)b;
			indices[index_count++] = (uint16_t)(a + 1);
			indices[index_count++] = (uint16_t)(b + 1);
			indices[index_count++] = (uint16_t)b;
		}
	}
	return set_mesh(render, WB_MESH_SPHERE, vertices, vertex_count, indices, index_count);
}

bool wb_render_set_terrain(wb_render_t *render, const float *heights, int cells, float cell_meters, float origin_x, float origin_z)
{
	int count = cells + 1;
	int vertex_count = count * count;
	int index_count = cells * cells * 6;
	vertex_t *vertices;
	uint16_t *indices;
	bool made;

	if (vertex_count > 65536)
	{
		SDL_Log("Render: a ground of %d cells a side is too big for 16 bit indices", cells);
		return false;
	}
	vertices = SDL_malloc(sizeof(vertex_t) * (size_t)vertex_count);
	indices = SDL_malloc(sizeof(uint16_t) * (size_t)index_count);
	if (vertices == NULL || indices == NULL)
	{
		SDL_free(vertices);
		SDL_free(indices);
		return false;
	}
	for (int row = 0; row < count; row++)
	{
		for (int column = 0; column < count; column++)
		{
			int left = SDL_max(column - 1, 0);
			int right = SDL_min(column + 1, cells);
			int down = SDL_max(row - 1, 0);
			int up = SDL_min(row + 1, cells);
			float slope_x = (heights[row * count + right] - heights[row * count + left]) / ((float)(right - left) * cell_meters);
			float slope_z = (heights[up * count + column] - heights[down * count + column]) / ((float)(up - down) * cell_meters);
			float normal[3] = { -slope_x, 1.0f, -slope_z };

			wb_vector_normalize(normal);
			put_vertex(&vertices[row * count + column], origin_x + (float)column * cell_meters, heights[row * count + column],
				origin_z + (float)row * cell_meters, normal[0], normal[1], normal[2]);
		}
	}
	index_count = 0;
	for (int row = 0; row < cells; row++)
	{
		for (int column = 0; column < cells; column++)
		{
			int a = row * count + column;
			int b = a + count;

			indices[index_count++] = (uint16_t)a;
			indices[index_count++] = (uint16_t)b;
			indices[index_count++] = (uint16_t)(a + 1);
			indices[index_count++] = (uint16_t)(a + 1);
			indices[index_count++] = (uint16_t)b;
			indices[index_count++] = (uint16_t)(b + 1);
		}
	}
	made = set_mesh(render, WB_MESH_TERRAIN, vertices, vertex_count, indices, index_count);
	SDL_free(vertices);
	SDL_free(indices);
	return made;
}

// ---------------------------------------------------------------------------
// Targets and pipelines

static void release_targets(wb_render_t *render)
{
	if (render->color_texture != NULL)
	{
		SDL_ReleaseGPUTexture(render->device, render->color_texture);
		render->color_texture = NULL;
	}
	if (render->depth_texture != NULL)
	{
		SDL_ReleaseGPUTexture(render->device, render->depth_texture);
		render->depth_texture = NULL;
	}
	if (render->download_transfer != NULL)
	{
		SDL_ReleaseGPUTransferBuffer(render->device, render->download_transfer);
		render->download_transfer = NULL;
	}
}

static SDL_GPUTexture *make_texture(SDL_GPUDevice *device, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, int width, int height)
{
	SDL_GPUTextureCreateInfo info;

	SDL_zero(info);
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = format;
	info.usage = usage;
	info.width = (uint32_t)width;
	info.height = (uint32_t)height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	return SDL_CreateGPUTexture(device, &info);
}

static bool make_targets(wb_render_t *render, int width, int height)
{
	SDL_GPUTransferBufferCreateInfo download_info;

	release_targets(render);
	render->width = width;
	render->height = height;
	render->color_texture = make_texture(render->device, render->color_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
		width, height);
	render->depth_texture = make_texture(render->device, render->depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, width, height);
	SDL_zero(download_info);
	download_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	download_info.size = (uint32_t)(width * height * 4);
	render->download_transfer = SDL_CreateGPUTransferBuffer(render->device, &download_info);
	render->download_size = download_info.size;
	return render->color_texture != NULL && render->depth_texture != NULL && render->download_transfer != NULL;
}

static void describe_vertex_input(SDL_GPUVertexBufferDescription buffers[2], SDL_GPUVertexAttribute attributes[6], SDL_GPUVertexInputState *state)
{
	SDL_memset(buffers, 0, sizeof(SDL_GPUVertexBufferDescription) * 2);
	SDL_memset(attributes, 0, sizeof(SDL_GPUVertexAttribute) * 6);
	buffers[0].slot = 0;
	buffers[0].pitch = sizeof(vertex_t);
	buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	buffers[1].slot = 1;
	buffers[1].pitch = sizeof(instance_t);
	buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
	attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
	attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 };
	for (int k = 0; k < 4; k++)
	{
		attributes[2 + k] = (SDL_GPUVertexAttribute){ (uint32_t)(2 + k), 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (uint32_t)(sizeof(float) * 4 * (size_t)k) };
	}
	SDL_zerop(state);
	state->vertex_buffer_descriptions = buffers;
	state->num_vertex_buffers = 2;
	state->vertex_attributes = attributes;
	state->num_vertex_attributes = 6;
}

static bool make_pipelines(wb_render_t *render, const char *shader_directory)
{
	SDL_GPUShader *solid_vertex = load_shader(render->device, shader_directory, "solid.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
	SDL_GPUShader *solid_fragment = load_shader(render->device, shader_directory, "solid.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
	SDL_GPUShader *shadow_vertex = load_shader(render->device, shader_directory, "shadow.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
	SDL_GPUShader *shadow_fragment = load_shader(render->device, shader_directory, "shadow.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
	SDL_GPUVertexBufferDescription buffers[2];
	SDL_GPUVertexAttribute attributes[6];
	SDL_GPUGraphicsPipelineCreateInfo info;
	SDL_GPUColorTargetDescription color_target;
	bool made = false;

	if (solid_vertex == NULL || solid_fragment == NULL || shadow_vertex == NULL || shadow_fragment == NULL)
	{
		goto done;
	}
	// The scene
	SDL_zero(info);
	describe_vertex_input(buffers, attributes, &info.vertex_input_state);
	info.vertex_shader = solid_vertex;
	info.fragment_shader = solid_fragment;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
	info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	info.depth_stencil_state.enable_depth_test = true;
	info.depth_stencil_state.enable_depth_write = true;
	info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
	SDL_zero(color_target);
	color_target.format = render->color_format;
	info.target_info.color_target_descriptions = &color_target;
	info.target_info.num_color_targets = 1;
	info.target_info.has_depth_stencil_target = true;
	info.target_info.depth_stencil_format = render->depth_format;
	render->scene_pipeline = SDL_CreateGPUGraphicsPipeline(render->device, &info);
	if (render->scene_pipeline == NULL)
	{
		SDL_Log("Render: the scene's pipeline: %s", SDL_GetError());
		goto done;
	}
	// The sun's shadow map: depth only, pushed back a little against acne
	SDL_zero(info);
	describe_vertex_input(buffers, attributes, &info.vertex_input_state);
	info.vertex_shader = shadow_vertex;
	info.fragment_shader = shadow_fragment;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	info.rasterizer_state.enable_depth_bias = true;
	info.rasterizer_state.depth_bias_constant_factor = 2.0f;
	info.rasterizer_state.depth_bias_slope_factor = 2.5f;
	info.depth_stencil_state.enable_depth_test = true;
	info.depth_stencil_state.enable_depth_write = true;
	info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
	info.target_info.num_color_targets = 0;
	info.target_info.has_depth_stencil_target = true;
	info.target_info.depth_stencil_format = render->shadow_format;
	render->shadow_pipeline = SDL_CreateGPUGraphicsPipeline(render->device, &info);
	if (render->shadow_pipeline == NULL)
	{
		SDL_Log("Render: the shadows' pipeline: %s", SDL_GetError());
		goto done;
	}
	made = true;
done:
	if (solid_vertex != NULL)
	{
		SDL_ReleaseGPUShader(render->device, solid_vertex);
	}
	if (solid_fragment != NULL)
	{
		SDL_ReleaseGPUShader(render->device, solid_fragment);
	}
	if (shadow_vertex != NULL)
	{
		SDL_ReleaseGPUShader(render->device, shadow_vertex);
	}
	if (shadow_fragment != NULL)
	{
		SDL_ReleaseGPUShader(render->device, shadow_fragment);
	}
	return made;
}

static SDL_GPUTextureFormat pick_depth_format(SDL_GPUDevice *device, SDL_GPUTextureUsageFlags usage)
{
	const SDL_GPUTextureFormat candidates[3] = { SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTUREFORMAT_D16_UNORM };

	for (int i = 0; i < 3; i++)
	{
		if (SDL_GPUTextureSupportsFormat(device, candidates[i], SDL_GPU_TEXTURETYPE_2D, usage))
		{
			return candidates[i];
		}
	}
	return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
}

wb_render_t *wb_render_create(SDL_Window *window, int width, int height, const char *shader_directory)
{
	wb_render_t *render = SDL_calloc(1, sizeof(*render));
	SDL_GPUSamplerCreateInfo sampler_info;
	SDL_GPUBufferCreateInfo buffer_info;
	SDL_GPUTransferBufferCreateInfo transfer_info;

	if (render == NULL)
	{
		return NULL;
	}
	render->window = window;
	render->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
	if (render->device == NULL)
	{
		SDL_Log("Render: no GPU device: %s", SDL_GetError());
		SDL_free(render);
		return NULL;
	}
	SDL_Log("Render: %s", SDL_GetGPUDeviceDriver(render->device));
	if (window != NULL)
	{
		if (!SDL_ClaimWindowForGPUDevice(render->device, window))
		{
			SDL_Log("Render: the window: %s", SDL_GetError());
			wb_render_destroy(render);
			return NULL;
		}
		SDL_GetWindowSizeInPixels(window, &width, &height);
	}
	render->color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	render->depth_format = pick_depth_format(render->device, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
	render->shadow_format = pick_depth_format(render->device, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
	render->shadow_texture = make_texture(render->device, render->shadow_format,
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
	SDL_zero(sampler_info);
	sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
	sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
	sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler_info.enable_compare = true;
	sampler_info.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	render->shadow_sampler = SDL_CreateGPUSampler(render->device, &sampler_info);
	SDL_zero(buffer_info);
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	buffer_info.size = sizeof(instance_t) * INSTANCES_MAX;
	render->instance_buffer = SDL_CreateGPUBuffer(render->device, &buffer_info);
	SDL_zero(transfer_info);
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = sizeof(instance_t) * INSTANCES_MAX;
	render->instance_transfer = SDL_CreateGPUTransferBuffer(render->device, &transfer_info);
	render->all_instances = SDL_malloc(sizeof(instance_t) * INSTANCES_MAX);
	for (int m = 0; m < WB_MESH_COUNT; m++)
	{
		render->meshes[m].instances = SDL_malloc(sizeof(instance_t) * INSTANCES_MAX);
	}
	render->shadow_radius = 30.0f;
	if (render->shadow_texture == NULL || render->shadow_sampler == NULL || render->instance_buffer == NULL || render->instance_transfer == NULL
		|| render->all_instances == NULL || !make_targets(render, width, height) || !make_pipelines(render, shader_directory)
		|| !make_box_mesh(render) || !make_prism_mesh(render) || !make_sphere_mesh(render))
	{
		SDL_Log("Render: could not be made");
		wb_render_destroy(render);
		return NULL;
	}
	for (int m = 0; m < WB_MESH_COUNT; m++)
	{
		if (render->meshes[m].instances == NULL)
		{
			wb_render_destroy(render);
			return NULL;
		}
	}
	return render;
}

void wb_render_destroy(wb_render_t *render)
{
	if (render == NULL)
	{
		return;
	}
	if (render->device != NULL)
	{
		SDL_WaitForGPUIdle(render->device);
		release_targets(render);
		for (int m = 0; m < WB_MESH_COUNT; m++)
		{
			if (render->meshes[m].vertex_buffer != NULL)
			{
				SDL_ReleaseGPUBuffer(render->device, render->meshes[m].vertex_buffer);
			}
			if (render->meshes[m].index_buffer != NULL)
			{
				SDL_ReleaseGPUBuffer(render->device, render->meshes[m].index_buffer);
			}
		}
		if (render->shadow_texture != NULL)
		{
			SDL_ReleaseGPUTexture(render->device, render->shadow_texture);
		}
		if (render->shadow_sampler != NULL)
		{
			SDL_ReleaseGPUSampler(render->device, render->shadow_sampler);
		}
		if (render->instance_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(render->device, render->instance_buffer);
		}
		if (render->instance_transfer != NULL)
		{
			SDL_ReleaseGPUTransferBuffer(render->device, render->instance_transfer);
		}
		if (render->scene_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(render->device, render->scene_pipeline);
		}
		if (render->shadow_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(render->device, render->shadow_pipeline);
		}
		if (render->window != NULL)
		{
			SDL_ReleaseWindowFromGPUDevice(render->device, render->window);
		}
		SDL_DestroyGPUDevice(render->device);
	}
	for (int m = 0; m < WB_MESH_COUNT; m++)
	{
		SDL_free(render->meshes[m].instances);
	}
	SDL_free(render->all_instances);
	SDL_free(render);
}

void wb_render_size(const wb_render_t *render, int *width, int *height)
{
	*width = render->width;
	*height = render->height;
}

void wb_render_set_shadow_bounds(wb_render_t *render, const float center[3], float radius)
{
	SDL_memcpy(render->shadow_center, center, sizeof(render->shadow_center));
	render->shadow_radius = SDL_max(radius, 1.0f);
}

// ---------------------------------------------------------------------------
// A frame

void wb_render_begin_frame(wb_render_t *render)
{
	for (int m = 0; m < WB_MESH_COUNT; m++)
	{
		render->meshes[m].instance_count = 0;
	}
}

void wb_render_add(wb_render_t *render, wb_mesh_t mesh, const float position[3], const float rotation[4], const float scale[3], uint32_t color)
{
	mesh_slot_t *slot = &render->meshes[mesh];
	instance_t *instance;

	if (slot->instance_count >= INSTANCES_MAX)
	{
		return;
	}
	instance = &slot->instances[slot->instance_count++];
	SDL_memcpy(instance->position, position, sizeof(float) * 3);
	instance->position[3] = 1.0f;
	SDL_memcpy(instance->rotation, rotation, sizeof(float) * 4);
	SDL_memcpy(instance->scale, scale, sizeof(float) * 3);
	instance->scale[3] = 1.0f;
	color_to_linear(color, instance->color);
	// (the ground is drawn with its grid)
	instance->color[3] = mesh == WB_MESH_TERRAIN ? 0.0f : 1.0f;
}

// The instances of all meshes into the instance buffer, one mesh after
// another; each mesh's first instance's number
static int upload_instances(wb_render_t *render, SDL_GPUCommandBuffer *command_buffer, uint32_t first_instances[WB_MESH_COUNT])
{
	int total = 0;
	instance_t *mapped;
	SDL_GPUCopyPass *copy_pass;
	SDL_GPUTransferBufferLocation source;
	SDL_GPUBufferRegion destination;

	for (int m = 0; m < WB_MESH_COUNT; m++)
	{
		int count = SDL_min(render->meshes[m].instance_count, INSTANCES_MAX - total);

		first_instances[m] = (uint32_t)total;
		render->meshes[m].instance_count = count;
		total += count;
	}
	if (total == 0)
	{
		return 0;
	}
	mapped = SDL_MapGPUTransferBuffer(render->device, render->instance_transfer, true);
	for (int m = 0; m < WB_MESH_COUNT; m++)
	{
		SDL_memcpy(mapped + first_instances[m], render->meshes[m].instances, sizeof(instance_t) * (size_t)render->meshes[m].instance_count);
	}
	SDL_UnmapGPUTransferBuffer(render->device, render->instance_transfer);
	copy_pass = SDL_BeginGPUCopyPass(command_buffer);
	SDL_zero(source);
	source.transfer_buffer = render->instance_transfer;
	SDL_zero(destination);
	destination.buffer = render->instance_buffer;
	destination.size = (uint32_t)(sizeof(instance_t) * (size_t)total);
	SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);
	SDL_EndGPUCopyPass(copy_pass);
	return total;
}

static void draw_meshes(wb_render_t *render, SDL_GPURenderPass *pass, const uint32_t first_instances[WB_MESH_COUNT])
{
	for (int m = 0; m < WB_MESH_COUNT; m++)
	{
		const mesh_slot_t *mesh = &render->meshes[m];
		SDL_GPUBufferBinding vertex_bindings[2];
		SDL_GPUBufferBinding index_binding;

		if (mesh->instance_count == 0 || mesh->vertex_buffer == NULL)
		{
			continue;
		}
		vertex_bindings[0] = (SDL_GPUBufferBinding){ mesh->vertex_buffer, 0 };
		vertex_bindings[1] = (SDL_GPUBufferBinding){ render->instance_buffer, 0 };
		index_binding = (SDL_GPUBufferBinding){ mesh->index_buffer, 0 };
		SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, 2);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		SDL_DrawGPUIndexedPrimitives(pass, mesh->index_count, (uint32_t)mesh->instance_count, 0, 0, first_instances[m]);
	}
}

static bool save_picture(wb_render_t *render, const char *png_path)
{
	uint8_t *pixels = SDL_MapGPUTransferBuffer(render->device, render->download_transfer, false);
	bool saved;

	if (pixels == NULL)
	{
		return false;
	}
	saved = stbi_write_png(png_path, render->width, render->height, 4, pixels, render->width * 4) != 0;
	SDL_UnmapGPUTransferBuffer(render->device, render->download_transfer);
	if (!saved)
	{
		SDL_Log("Render: %s could not be written", png_path);
	}
	return saved;
}

bool wb_render_end_frame(wb_render_t *render, const wb_camera_t *camera, const char *png_path)
{
	const float sun_direction[3] = { 0.42f, 0.80f, 0.43f };
	SDL_GPUCommandBuffer *command_buffer;
	SDL_GPUTexture *swapchain_texture = NULL;
	uint32_t swapchain_width = 0;
	uint32_t swapchain_height = 0;
	uint32_t first_instances[WB_MESH_COUNT];
	frame_uniforms_t frame;
	light_uniforms_t light;
	float view[16];
	float projection[16];
	float light_view[16];
	float light_projection[16];
	float light_eye[3];
	float sun[3];
	SDL_GPUDepthStencilTargetInfo depth_target;
	SDL_GPUColorTargetInfo color_target;
	SDL_GPURenderPass *pass;
	SDL_GPUTextureSamplerBinding shadow_binding;
	const float sky[3] = { 0.62f, 0.74f, 0.88f };

	// The window resized: the targets with it
	if (render->window != NULL)
	{
		int width;
		int height;

		SDL_GetWindowSizeInPixels(render->window, &width, &height);
		if (width > 0 && height > 0 && (width != render->width || height != render->height))
		{
			SDL_WaitForGPUIdle(render->device);
			if (!make_targets(render, width, height))
			{
				return false;
			}
		}
	}
	command_buffer = SDL_AcquireGPUCommandBuffer(render->device);
	if (command_buffer == NULL)
	{
		return false;
	}
	upload_instances(render, command_buffer, first_instances);

	// The matrices: the camera's, and the sun's box round the shadows' bounds
	SDL_memcpy(sun, sun_direction, sizeof(sun));
	wb_vector_normalize(sun);
	wb_matrix_look_at(camera->eye, camera->target, view);
	wb_matrix_perspective(camera->vertical_field_of_view_degrees * SDL_PI_F / 180.0f, (float)render->width / (float)render->height, 0.1f,
		500.0f, projection);
	wb_matrix_multiply(projection, view, frame.view_projection);
	for (int k = 0; k < 3; k++)
	{
		light_eye[k] = render->shadow_center[k] + sun[k] * render->shadow_radius * 2.0f;
	}
	wb_matrix_look_at(light_eye, render->shadow_center, light_view);
	wb_matrix_orthographic(-render->shadow_radius, render->shadow_radius, -render->shadow_radius, render->shadow_radius, 0.1f,
		render->shadow_radius * 4.0f, light_projection);
	wb_matrix_multiply(light_projection, light_view, frame.light_view_projection);

	// The shadow map
	SDL_zero(depth_target);
	depth_target.texture = render->shadow_texture;
	depth_target.clear_depth = 1.0f;
	depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
	depth_target.store_op = SDL_GPU_STOREOP_STORE;
	pass = SDL_BeginGPURenderPass(command_buffer, NULL, 0, &depth_target);
	SDL_BindGPUGraphicsPipeline(pass, render->shadow_pipeline);
	SDL_PushGPUVertexUniformData(command_buffer, 0, &frame, sizeof(frame));
	draw_meshes(render, pass, first_instances);
	SDL_EndGPURenderPass(pass);

	// The scene
	SDL_zero(light);
	SDL_memcpy(light.sun_direction, sun, sizeof(sun));
	light.sun_color[0] = 2.6f;
	light.sun_color[1] = 2.45f;
	light.sun_color[2] = 2.2f;
	for (int k = 0; k < 3; k++)
	{
		light.sky_color[k] = srgb_to_linear(sky[k]) * 0.9f;
		light.fog_color[k] = srgb_to_linear(sky[k]);
	}
	light.ground_color[0] = 0.12f;
	light.ground_color[1] = 0.11f;
	light.ground_color[2] = 0.09f;
	light.fog_color[3] = 1.0f / 260.0f;
	SDL_memcpy(light.eye, camera->eye, sizeof(float) * 3);
	light.shadow_texel[0] = 1.0f / (float)SHADOW_MAP_SIZE;
	SDL_zero(color_target);
	color_target.texture = render->color_texture;
	color_target.clear_color = (SDL_FColor){ sky[0], sky[1], sky[2], 1.0f };
	color_target.load_op = SDL_GPU_LOADOP_CLEAR;
	color_target.store_op = SDL_GPU_STOREOP_STORE;
	SDL_zero(depth_target);
	depth_target.texture = render->depth_texture;
	depth_target.clear_depth = 1.0f;
	depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
	depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
	pass = SDL_BeginGPURenderPass(command_buffer, &color_target, 1, &depth_target);
	SDL_BindGPUGraphicsPipeline(pass, render->scene_pipeline);
	SDL_PushGPUVertexUniformData(command_buffer, 0, &frame, sizeof(frame));
	SDL_PushGPUFragmentUniformData(command_buffer, 0, &light, sizeof(light));
	shadow_binding.texture = render->shadow_texture;
	shadow_binding.sampler = render->shadow_sampler;
	SDL_BindGPUFragmentSamplers(pass, 0, &shadow_binding, 1);
	draw_meshes(render, pass, first_instances);
	SDL_EndGPURenderPass(pass);

	// To the window
	if (render->window != NULL && SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, render->window, &swapchain_texture, &swapchain_width,
		&swapchain_height) && swapchain_texture != NULL)
	{
		SDL_GPUBlitInfo blit;

		SDL_zero(blit);
		blit.source.texture = render->color_texture;
		blit.source.w = (uint32_t)render->width;
		blit.source.h = (uint32_t)render->height;
		blit.destination.texture = swapchain_texture;
		blit.destination.w = swapchain_width;
		blit.destination.h = swapchain_height;
		blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter = SDL_GPU_FILTER_LINEAR;
		SDL_BlitGPUTexture(command_buffer, &blit);
	}
	// To a file
	if (png_path != NULL)
	{
		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);
		SDL_GPUTextureRegion source;
		SDL_GPUTextureTransferInfo destination;
		SDL_GPUFence *fence;

		SDL_zero(source);
		source.texture = render->color_texture;
		source.w = (uint32_t)render->width;
		source.h = (uint32_t)render->height;
		source.d = 1;
		SDL_zero(destination);
		destination.transfer_buffer = render->download_transfer;
		SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
		SDL_EndGPUCopyPass(copy_pass);
		fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
		if (fence == NULL)
		{
			return false;
		}
		SDL_WaitForGPUFences(render->device, true, &fence, 1);
		SDL_ReleaseGPUFence(render->device, fence);
		return save_picture(render, png_path);
	}
	return SDL_SubmitGPUCommandBuffer(command_buffer);
}
