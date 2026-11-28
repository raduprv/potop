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
#define SHOCK_WAVES_MAX 4
// The instance lists: one a mesh, then the effects (drawn with the sphere)
#define LIST_FIRE WB_MESH_COUNT
#define LIST_SMOKE (WB_MESH_COUNT + 1)
#define LIST_COUNT (WB_MESH_COUNT + 2)

typedef struct
{
	float position[3];
	float normal[3];
} vertex_t;

// A corner of the water's surface (water.vert's inputs)
typedef struct
{
	float position[3];
	float normal[3];
	float depth;
	float unused;
} water_vertex_t;

// Thinner than this the water is not drawn (shallow_water.c's dry depth)
#define WATER_DRY_DEPTH 0.002f

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
	float fog_color[4];                // (not used now)
	float eye[4];
	float shadow_texel[4];             // x: one texel of the shadow map, in its coordinates
	float flash_position[4];           // w: 1 while there is a flash
	float flash_color[4];
} light_uniforms_t;

// post.frag's
typedef struct
{
	float rings[SHOCK_WAVES_MAX][4];   // pixels: middle x, y, radius, how far it bends
	float info[4];                     // count, ring width in pixels, 1 / width, 1 / height
} shock_uniforms_t;

typedef struct
{
	float position[3];
	float radius;
	float strength;
} shock_wave_t;

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
	SDL_GPUGraphicsPipeline *water_pipeline;
	SDL_GPUGraphicsPipeline *fire_pipeline;
	SDL_GPUGraphicsPipeline *smoke_pipeline;
	SDL_GPUGraphicsPipeline *post_pipeline;
	// The picture bent by shock waves goes here (the scene is read from
	// color_texture)
	SDL_GPUTexture *post_texture;
	SDL_GPUSampler *post_sampler;
	shock_wave_t shock_waves[SHOCK_WAVES_MAX];
	int shock_wave_count;
	float flash_position[3];
	float flash_color[3];
	bool flash;
	// The water's surface: a grid of corners like the ground's, written
	// anew each frame it changes
	SDL_GPUBuffer *water_vertex_buffer;
	SDL_GPUBuffer *water_index_buffer;
	SDL_GPUTransferBuffer *water_transfer;
	water_vertex_t *water_vertices;
	float *water_surface;              // a corner's height of surface (scratch)
	int water_cells;
	uint32_t water_index_count;
	bool water_dirty;                  // written since the last frame: to upload
	bool water_any;                    // anything wet to draw
	mesh_slot_t meshes[LIST_COUNT];    // the meshes', then the effects' (LIST_FIRE, LIST_SMOKE)
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
	if (render->post_texture != NULL)
	{
		SDL_ReleaseGPUTexture(render->device, render->post_texture);
		render->post_texture = NULL;
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
	render->post_texture = make_texture(render->device, render->color_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
		width, height);
	SDL_zero(download_info);
	download_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	download_info.size = (uint32_t)(width * height * 4);
	render->download_transfer = SDL_CreateGPUTransferBuffer(render->device, &download_info);
	render->download_size = download_info.size;
	return render->color_texture != NULL && render->depth_texture != NULL && render->post_texture != NULL && render->download_transfer != NULL;
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
	// The water: see through, over the solids, writing no depth
	{
		SDL_GPUShader *water_vertex = load_shader(render->device, shader_directory, "water.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *water_fragment = load_shader(render->device, shader_directory, "water.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
		SDL_GPUVertexBufferDescription water_buffer;
		SDL_GPUVertexAttribute water_attributes[3];

		if (water_vertex == NULL || water_fragment == NULL)
		{
			if (water_vertex != NULL)
			{
				SDL_ReleaseGPUShader(render->device, water_vertex);
			}
			if (water_fragment != NULL)
			{
				SDL_ReleaseGPUShader(render->device, water_fragment);
			}
			goto done;
		}
		SDL_zero(info);
		SDL_zero(water_buffer);
		water_buffer.slot = 0;
		water_buffer.pitch = sizeof(water_vertex_t);
		water_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
		water_attributes[0] = (SDL_GPUVertexAttribute){ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
		water_attributes[1] = (SDL_GPUVertexAttribute){ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 };
		water_attributes[2] = (SDL_GPUVertexAttribute){ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, sizeof(float) * 6 };
		info.vertex_input_state.vertex_buffer_descriptions = &water_buffer;
		info.vertex_input_state.num_vertex_buffers = 1;
		info.vertex_input_state.vertex_attributes = water_attributes;
		info.vertex_input_state.num_vertex_attributes = 3;
		info.vertex_shader = water_vertex;
		info.fragment_shader = water_fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.depth_stencil_state.enable_depth_test = true;
		info.depth_stencil_state.enable_depth_write = false;
		info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
		SDL_zero(color_target);
		color_target.format = render->color_format;
		color_target.blend_state.enable_blend = true;
		color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		info.target_info.color_target_descriptions = &color_target;
		info.target_info.num_color_targets = 1;
		info.target_info.has_depth_stencil_target = true;
		info.target_info.depth_stencil_format = render->depth_format;
		render->water_pipeline = SDL_CreateGPUGraphicsPipeline(render->device, &info);
		SDL_ReleaseGPUShader(render->device, water_vertex);
		SDL_ReleaseGPUShader(render->device, water_fragment);
		if (render->water_pipeline == NULL)
		{
			SDL_Log("Render: the water's pipeline: %s", SDL_GetError());
			goto done;
		}
	}
	// The effects: on solid.vert, over the solids, writing no depth; fire
	// added, smoke laid over
	for (int effect = 0; effect < WB_EFFECT_COUNT; effect++)
	{
		SDL_GPUShader *effect_fragment = load_shader(render->device, shader_directory, effect == WB_EFFECT_FIRE ? "fire.frag" : "smoke.frag",
			SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
		SDL_GPUGraphicsPipeline *pipeline;

		if (effect_fragment == NULL)
		{
			goto done;
		}
		SDL_zero(info);
		describe_vertex_input(buffers, attributes, &info.vertex_input_state);
		info.vertex_shader = solid_vertex;
		info.fragment_shader = effect_fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
		info.depth_stencil_state.enable_depth_test = true;
		info.depth_stencil_state.enable_depth_write = false;
		info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
		SDL_zero(color_target);
		color_target.format = render->color_format;
		color_target.blend_state.enable_blend = true;
		color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		if (effect == WB_EFFECT_FIRE)
		{
			color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
			color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		}
		else
		{
			color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
			color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		}
		info.target_info.color_target_descriptions = &color_target;
		info.target_info.num_color_targets = 1;
		info.target_info.has_depth_stencil_target = true;
		info.target_info.depth_stencil_format = render->depth_format;
		pipeline = SDL_CreateGPUGraphicsPipeline(render->device, &info);
		SDL_ReleaseGPUShader(render->device, effect_fragment);
		if (pipeline == NULL)
		{
			SDL_Log("Render: an effect's pipeline: %s", SDL_GetError());
			goto done;
		}
		if (effect == WB_EFFECT_FIRE)
		{
			render->fire_pipeline = pipeline;
		}
		else
		{
			render->smoke_pipeline = pipeline;
		}
	}
	// The shock waves: the picture read again, bent
	{
		SDL_GPUShader *post_vertex = load_shader(render->device, shader_directory, "post.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		SDL_GPUShader *post_fragment = load_shader(render->device, shader_directory, "post.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

		if (post_vertex == NULL || post_fragment == NULL)
		{
			if (post_vertex != NULL)
			{
				SDL_ReleaseGPUShader(render->device, post_vertex);
			}
			if (post_fragment != NULL)
			{
				SDL_ReleaseGPUShader(render->device, post_fragment);
			}
			goto done;
		}
		SDL_zero(info);
		info.vertex_shader = post_vertex;
		info.fragment_shader = post_fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		SDL_zero(color_target);
		color_target.format = render->color_format;
		info.target_info.color_target_descriptions = &color_target;
		info.target_info.num_color_targets = 1;
		render->post_pipeline = SDL_CreateGPUGraphicsPipeline(render->device, &info);
		SDL_ReleaseGPUShader(render->device, post_vertex);
		SDL_ReleaseGPUShader(render->device, post_fragment);
		if (render->post_pipeline == NULL)
		{
			SDL_Log("Render: the shock waves' pipeline: %s", SDL_GetError());
			goto done;
		}
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
	sampler_info.enable_compare = false;
	render->post_sampler = SDL_CreateGPUSampler(render->device, &sampler_info);
	SDL_zero(buffer_info);
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	buffer_info.size = sizeof(instance_t) * INSTANCES_MAX;
	render->instance_buffer = SDL_CreateGPUBuffer(render->device, &buffer_info);
	SDL_zero(transfer_info);
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = sizeof(instance_t) * INSTANCES_MAX;
	render->instance_transfer = SDL_CreateGPUTransferBuffer(render->device, &transfer_info);
	render->all_instances = SDL_malloc(sizeof(instance_t) * INSTANCES_MAX);
	for (int m = 0; m < LIST_COUNT; m++)
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
	for (int m = 0; m < LIST_COUNT; m++)
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
		if (render->water_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(render->device, render->water_pipeline);
		}
		if (render->fire_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(render->device, render->fire_pipeline);
		}
		if (render->smoke_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(render->device, render->smoke_pipeline);
		}
		if (render->post_pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(render->device, render->post_pipeline);
		}
		if (render->post_sampler != NULL)
		{
			SDL_ReleaseGPUSampler(render->device, render->post_sampler);
		}
		if (render->water_vertex_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(render->device, render->water_vertex_buffer);
		}
		if (render->water_index_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(render->device, render->water_index_buffer);
		}
		if (render->water_transfer != NULL)
		{
			SDL_ReleaseGPUTransferBuffer(render->device, render->water_transfer);
		}
		if (render->window != NULL)
		{
			SDL_ReleaseWindowFromGPUDevice(render->device, render->window);
		}
		SDL_DestroyGPUDevice(render->device);
	}
	for (int m = 0; m < LIST_COUNT; m++)
	{
		SDL_free(render->meshes[m].instances);
	}
	SDL_free(render->all_instances);
	SDL_free(render->water_vertices);
	SDL_free(render->water_surface);
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

// The water's grid: its buffers and indices, made once for its size
static bool make_water_grid(wb_render_t *render, int cells)
{
	int corners = cells + 1;
	int vertex_count = corners * corners;
	int index_count = cells * cells * 6;
	uint16_t *indices;
	SDL_GPUBufferCreateInfo buffer_info;
	SDL_GPUTransferBufferCreateInfo transfer_info;

	if (vertex_count > 65536)
	{
		return false;
	}
	render->water_vertices = SDL_calloc((size_t)vertex_count, sizeof(water_vertex_t));
	render->water_surface = SDL_calloc((size_t)vertex_count, sizeof(float));
	indices = SDL_malloc(sizeof(uint16_t) * (size_t)index_count);
	if (render->water_vertices == NULL || render->water_surface == NULL || indices == NULL)
	{
		SDL_free(indices);
		return false;
	}
	index_count = 0;
	for (int row = 0; row < cells; row++)
	{
		for (int column = 0; column < cells; column++)
		{
			int a = row * corners + column;
			int b = a + corners;

			indices[index_count++] = (uint16_t)a;
			indices[index_count++] = (uint16_t)b;
			indices[index_count++] = (uint16_t)(a + 1);
			indices[index_count++] = (uint16_t)(a + 1);
			indices[index_count++] = (uint16_t)b;
			indices[index_count++] = (uint16_t)(b + 1);
		}
	}
	render->water_index_buffer = make_buffer(render->device, SDL_GPU_BUFFERUSAGE_INDEX, indices, (uint32_t)(sizeof(uint16_t) * (size_t)index_count));
	SDL_free(indices);
	render->water_index_count = (uint32_t)index_count;
	SDL_zero(buffer_info);
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	buffer_info.size = (uint32_t)(sizeof(water_vertex_t) * (size_t)vertex_count);
	render->water_vertex_buffer = SDL_CreateGPUBuffer(render->device, &buffer_info);
	SDL_zero(transfer_info);
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = buffer_info.size;
	render->water_transfer = SDL_CreateGPUTransferBuffer(render->device, &transfer_info);
	render->water_cells = cells;
	return render->water_index_buffer != NULL && render->water_vertex_buffer != NULL && render->water_transfer != NULL;
}

void wb_render_set_water(wb_render_t *render, const float *depths, const float *floors, const float *corner_heights, int cells, float cell_meters,
	float origin_x, float origin_z)
{
	int corners = cells + 1;
	bool any = false;

	if (render->water_cells != cells && (render->water_cells != 0 || !make_water_grid(render, cells)))
	{
		return;
	}
	// Each corner: the mean surface of the wet cells round it, and the
	// mean depth of all four; a corner with no wet cell is put under the
	// ground, so the surface meets the ground where the water ends
	for (int row = 0; row < corners; row++)
	{
		for (int column = 0; column < corners; column++)
		{
			float surface_sum = 0.0f;
			float depth_sum = 0.0f;
			int wet = 0;
			water_vertex_t *vertex = &render->water_vertices[row * corners + column];

			for (int cell_row = row - 1; cell_row <= row; cell_row++)
			{
				for (int cell_column = column - 1; cell_column <= column; cell_column++)
				{
					int index = cell_row * cells + cell_column;

					if (cell_row < 0 || cell_column < 0 || cell_row >= cells || cell_column >= cells)
					{
						continue;
					}
					depth_sum += depths[index];
					if (depths[index] > WATER_DRY_DEPTH)
					{
						surface_sum += floors[index] + depths[index];
						wet++;
					}
				}
			}
			vertex->position[0] = origin_x + (float)column * cell_meters;
			vertex->position[2] = origin_z + (float)row * cell_meters;
			if (wet > 0)
			{
				vertex->position[1] = surface_sum / (float)wet;
				vertex->depth = depth_sum * 0.25f;
				any = true;
			}
			else
			{
				vertex->position[1] = corner_heights[row * corners + column] - 0.01f;
				vertex->depth = 0.0f;
			}
			render->water_surface[row * corners + column] = vertex->position[1];
		}
	}
	// The normals, from the surface round each corner
	for (int row = 0; row < corners; row++)
	{
		for (int column = 0; column < corners; column++)
		{
			int left = SDL_max(column - 1, 0);
			int right = SDL_min(column + 1, cells);
			int up = SDL_max(row - 1, 0);
			int down = SDL_min(row + 1, cells);
			float slope_x = (render->water_surface[row * corners + right] - render->water_surface[row * corners + left]) / ((float)(right - left) * cell_meters);
			float slope_z = (render->water_surface[down * corners + column] - render->water_surface[up * corners + column]) / ((float)(down - up) * cell_meters);
			float normal[3] = { -slope_x, 1.0f, -slope_z };
			water_vertex_t *vertex = &render->water_vertices[row * corners + column];

			wb_vector_normalize(normal);
			SDL_memcpy(vertex->normal, normal, sizeof(normal));
		}
	}
	render->water_any = any;
	render->water_dirty = true;
}

// ---------------------------------------------------------------------------
// A frame

void wb_render_begin_frame(wb_render_t *render)
{
	for (int m = 0; m < LIST_COUNT; m++)
	{
		render->meshes[m].instance_count = 0;
	}
	render->shock_wave_count = 0;
	render->flash = false;
}

void wb_render_add_effect(wb_render_t *render, wb_effect_t effect, const float position[3], float radius, const float color[3], float alpha)
{
	mesh_slot_t *slot = &render->meshes[effect == WB_EFFECT_FIRE ? LIST_FIRE : LIST_SMOKE];
	instance_t *instance;

	if (slot->instance_count >= INSTANCES_MAX)
	{
		return;
	}
	instance = &slot->instances[slot->instance_count++];
	SDL_memcpy(instance->position, position, sizeof(float) * 3);
	instance->position[3] = 1.0f;
	instance->rotation[0] = 0.0f;
	instance->rotation[1] = 0.0f;
	instance->rotation[2] = 0.0f;
	instance->rotation[3] = 1.0f;
	instance->scale[0] = radius;
	instance->scale[1] = radius;
	instance->scale[2] = radius;
	instance->scale[3] = 1.0f;
	SDL_memcpy(instance->color, color, sizeof(float) * 3);
	instance->color[3] = alpha;
}

void wb_render_set_flash(wb_render_t *render, const float position[3], const float color[3])
{
	SDL_memcpy(render->flash_position, position, sizeof(render->flash_position));
	SDL_memcpy(render->flash_color, color, sizeof(render->flash_color));
	render->flash = true;
}

void wb_render_add_shock_wave(wb_render_t *render, const float position[3], float radius, float strength)
{
	shock_wave_t *wave;

	if (render->shock_wave_count >= SHOCK_WAVES_MAX)
	{
		return;
	}
	wave = &render->shock_waves[render->shock_wave_count++];
	SDL_memcpy(wave->position, position, sizeof(wave->position));
	wave->radius = radius;
	wave->strength = strength;
}

// The smoke drawn from the furthest in (each laid over what is behind it)
static float smoke_sort_eye[3];

static int compare_smoke_distance(const void *first, const void *second)
{
	const instance_t *a = first;
	const instance_t *b = second;
	float distance_a = 0.0f;
	float distance_b = 0.0f;

	for (int k = 0; k < 3; k++)
	{
		distance_a += (a->position[k] - smoke_sort_eye[k]) * (a->position[k] - smoke_sort_eye[k]);
		distance_b += (b->position[k] - smoke_sort_eye[k]) * (b->position[k] - smoke_sort_eye[k]);
	}
	return distance_a < distance_b ? 1 : (distance_a > distance_b ? -1 : 0);
}

static void draw_effect(wb_render_t *render, SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *command_buffer, int list,
	SDL_GPUGraphicsPipeline *pipeline, const uint32_t first_instances[LIST_COUNT], const void *frame, uint32_t frame_size, const void *light,
	uint32_t light_size)
{
	const mesh_slot_t *sphere = &render->meshes[WB_MESH_SPHERE];
	SDL_GPUBufferBinding vertex_bindings[2];
	SDL_GPUBufferBinding index_binding;

	if (render->meshes[list].instance_count == 0 || pipeline == NULL)
	{
		return;
	}
	SDL_BindGPUGraphicsPipeline(pass, pipeline);
	SDL_PushGPUVertexUniformData(command_buffer, 0, frame, frame_size);
	SDL_PushGPUFragmentUniformData(command_buffer, 0, light, light_size);
	vertex_bindings[0] = (SDL_GPUBufferBinding){ sphere->vertex_buffer, 0 };
	vertex_bindings[1] = (SDL_GPUBufferBinding){ render->instance_buffer, 0 };
	index_binding = (SDL_GPUBufferBinding){ sphere->index_buffer, 0 };
	SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, 2);
	SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
	SDL_DrawGPUIndexedPrimitives(pass, sphere->index_count, (uint32_t)render->meshes[list].instance_count, 0, 0, first_instances[list]);
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
static int upload_instances(wb_render_t *render, SDL_GPUCommandBuffer *command_buffer, uint32_t first_instances[LIST_COUNT])
{
	int total = 0;
	instance_t *mapped;
	SDL_GPUCopyPass *copy_pass;
	SDL_GPUTransferBufferLocation source;
	SDL_GPUBufferRegion destination;

	for (int m = 0; m < LIST_COUNT; m++)
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
	for (int m = 0; m < LIST_COUNT; m++)
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

static void draw_meshes(wb_render_t *render, SDL_GPURenderPass *pass, const uint32_t first_instances[LIST_COUNT])
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
	uint32_t first_instances[LIST_COUNT];
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
	SDL_GPUTexture *final_texture;
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
	SDL_memcpy(smoke_sort_eye, camera->eye, sizeof(smoke_sort_eye));
	SDL_qsort(render->meshes[LIST_SMOKE].instances, (size_t)render->meshes[LIST_SMOKE].instance_count, sizeof(instance_t), compare_smoke_distance);
	upload_instances(render, command_buffer, first_instances);
	if (render->water_dirty && render->water_any)
	{
		uint32_t size = (uint32_t)(sizeof(water_vertex_t) * (size_t)(render->water_cells + 1) * (size_t)(render->water_cells + 1));
		void *mapped = SDL_MapGPUTransferBuffer(render->device, render->water_transfer, true);
		SDL_GPUCopyPass *copy_pass;
		SDL_GPUTransferBufferLocation source;
		SDL_GPUBufferRegion destination;

		SDL_memcpy(mapped, render->water_vertices, size);
		SDL_UnmapGPUTransferBuffer(render->device, render->water_transfer);
		copy_pass = SDL_BeginGPUCopyPass(command_buffer);
		SDL_zero(source);
		source.transfer_buffer = render->water_transfer;
		SDL_zero(destination);
		destination.buffer = render->water_vertex_buffer;
		destination.size = size;
		SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);
		SDL_EndGPUCopyPass(copy_pass);
		render->water_dirty = false;
	}

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
	SDL_memcpy(light.eye, camera->eye, sizeof(float) * 3);
	light.shadow_texel[0] = 1.0f / (float)SHADOW_MAP_SIZE;
	if (render->flash)
	{
		SDL_memcpy(light.flash_position, render->flash_position, sizeof(float) * 3);
		light.flash_position[3] = 1.0f;
		SDL_memcpy(light.flash_color, render->flash_color, sizeof(float) * 3);
	}
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
	if (render->water_any && render->water_pipeline != NULL)
	{
		SDL_GPUBufferBinding vertex_binding = { render->water_vertex_buffer, 0 };
		SDL_GPUBufferBinding index_binding = { render->water_index_buffer, 0 };

		SDL_BindGPUGraphicsPipeline(pass, render->water_pipeline);
		SDL_PushGPUVertexUniformData(command_buffer, 0, &frame, sizeof(frame));
		SDL_PushGPUFragmentUniformData(command_buffer, 0, &light, sizeof(light));
		SDL_BindGPUFragmentSamplers(pass, 0, &shadow_binding, 1);
		SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
		SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		SDL_DrawGPUIndexedPrimitives(pass, render->water_index_count, 1, 0, 0, 0);
	}
	draw_effect(render, pass, command_buffer, LIST_SMOKE, render->smoke_pipeline, first_instances, &frame, sizeof(frame), &light, sizeof(light));
	draw_effect(render, pass, command_buffer, LIST_FIRE, render->fire_pipeline, first_instances, &frame, sizeof(frame), &light, sizeof(light));
	SDL_EndGPURenderPass(pass);

	// The shock waves: the picture read again, bent round each
	final_texture = render->color_texture;
	if (render->shock_wave_count > 0 && render->post_pipeline != NULL)
	{
		shock_uniforms_t shock;
		SDL_GPUTextureSamplerBinding scene_binding;
		int count = 0;

		SDL_zero(shock);
		for (int i = 0; i < render->shock_wave_count; i++)
		{
			const shock_wave_t *wave = &render->shock_waves[i];
			float clip[4];

			for (int row = 0; row < 4; row++)
			{
				clip[row] = frame.view_projection[row] * wave->position[0] + frame.view_projection[4 + row] * wave->position[1]
					+ frame.view_projection[8 + row] * wave->position[2] + frame.view_projection[12 + row];
			}
			if (clip[3] < 0.1f)
			{
				continue;
			}
			shock.rings[count][0] = (0.5f + 0.5f * clip[0] / clip[3]) * (float)render->width;
			shock.rings[count][1] = (0.5f - 0.5f * clip[1] / clip[3]) * (float)render->height;
			// (meters to pixels at its distance: the projection's focal length)
			shock.rings[count][2] = wave->radius * projection[5] * 0.5f * (float)render->height / clip[3];
			shock.rings[count][3] = wave->strength * 22.0f * (float)render->height / 720.0f;
			count++;
		}
		shock.info[0] = (float)count;
		shock.info[1] = 10.0f * (float)render->height / 720.0f;
		shock.info[2] = 1.0f / (float)render->width;
		shock.info[3] = 1.0f / (float)render->height;
		if (count > 0)
		{
			SDL_zero(color_target);
			color_target.texture = render->post_texture;
			color_target.load_op = SDL_GPU_LOADOP_DONT_CARE;
			color_target.store_op = SDL_GPU_STOREOP_STORE;
			pass = SDL_BeginGPURenderPass(command_buffer, &color_target, 1, NULL);
			SDL_BindGPUGraphicsPipeline(pass, render->post_pipeline);
			SDL_PushGPUFragmentUniformData(command_buffer, 0, &shock, sizeof(shock));
			scene_binding.texture = render->color_texture;
			scene_binding.sampler = render->post_sampler;
			SDL_BindGPUFragmentSamplers(pass, 0, &scene_binding, 1);
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			final_texture = render->post_texture;
		}
	}

	// To the window
	if (render->window != NULL && SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, render->window, &swapchain_texture, &swapchain_width,
		&swapchain_height) && swapchain_texture != NULL)
	{
		SDL_GPUBlitInfo blit;

		SDL_zero(blit);
		blit.source.texture = final_texture;
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
		source.texture = final_texture;
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
