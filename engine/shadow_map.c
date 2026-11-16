// Sun shadows. See shadow_map.h.

#include "shadow_map.h"

#include <SDL3/SDL.h>

// The chunk mesh's vertex: its cell offset in the chunk (terrain_renderer.c)
static const SDL_GPUVertexBufferDescription chunk_buffer = { 0, sizeof(float) * 2, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 };
static const SDL_GPUVertexAttribute chunk_attribute = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 };

bool shadow_map_create(shadow_map_t *shadow, int size)
{
	gpu_pipeline_info_t pipeline;

	SDL_zerop(shadow);
	shadow->needs_render = true;
	shadow->light_view_projection = mat4_identity();
	if (size <= 0)
	{
		SDL_Log("Shadows disabled (render.shadow_map_size = 0)");
		// The lit shaders still bind a map; this one is never read
		shadow->depth_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.shadow_depth_format, 1, 1, 1, 1,
			SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		return shadow->depth_texture != NULL;
	}

	shadow->depth_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.shadow_depth_format, (uint32_t)size, (uint32_t)size, 1, 1,
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
	SDL_zero(pipeline);
	pipeline.vertex_shader = gpu_load_shader("shadow.vert", 1, 0, 2);
	pipeline.fragment_shader = gpu_load_shader("shadow.frag", 0, 0, 0);
	pipeline.vertex_buffers = &chunk_buffer;
	pipeline.vertex_buffer_count = 1;
	pipeline.vertex_attributes = &chunk_attribute;
	pipeline.vertex_attribute_count = 1;
	// The terrain is a single sheet, not a closed solid, and leaves are
	// single sided cards: both sides cast shadows. The slope scaled depth
	// bias keeps lit slopes from shadowing themselves ("shadow acne")
	pipeline.cull = SDL_GPU_CULLMODE_NONE;
	pipeline.depth_test = true;
	pipeline.depth_write = true;
	pipeline.depth_bias_constant = 4.0f;
	pipeline.depth_bias_slope = 2.0f;
	pipeline.depth_only = true;
	shadow->terrain_pipeline = gpu_make_pipeline(&pipeline, "terrain shadow");
	SDL_ReleaseGPUShader(gpu.device, pipeline.vertex_shader);
	SDL_ReleaseGPUShader(gpu.device, pipeline.fragment_shader);
	if (shadow->depth_texture == NULL || shadow->terrain_pipeline == NULL)
	{
		shadow_map_destroy(shadow);
		return false;
	}
	shadow->size = size;
	SDL_Log("Shadow map: %dx%d", size, size);
	return true;
}

void shadow_map_destroy(shadow_map_t *shadow)
{
	if (shadow->depth_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, shadow->depth_texture);
	}
	if (shadow->still_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, shadow->still_texture);
	}
	if (shadow->terrain_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, shadow->terrain_pipeline);
	}
	SDL_zerop(shadow);
}

void shadow_map_update_projection(shadow_map_t *shadow, const terrain_t *terrain, vec3_t sun_direction)
{
	float extent = (float)terrain->size * terrain->cell_size_meters;
	float minimum_height = 1e30f;
	float maximum_height = -1e30f;
	vec3_t center;
	vec3_t eye;
	vec3_t up;
	mat4_t view;
	float view_minimum[3] = { 1e30f, 1e30f, 1e30f };
	float view_maximum[3] = { -1e30f, -1e30f, -1e30f };
	int chunk_count = terrain->chunks_per_side * terrain->chunks_per_side;

	for (int i = 0; i < chunk_count; i++)
	{
		minimum_height = SDL_min(minimum_height, terrain->chunks[i].minimum_height_meters);
		maximum_height = SDL_max(maximum_height, terrain->chunks[i].maximum_height_meters);
	}
	// Headroom above the terrain for water and, later, objects
	maximum_height += 20.0f;

	sun_direction = vec3_normalize(sun_direction);
	center = vec3_make(extent * 0.5f, 0.5f * (minimum_height + maximum_height), extent * 0.5f);
	eye = vec3_add(center, vec3_scale(sun_direction, extent));
	up = SDL_fabsf(sun_direction.y) > 0.99f ? vec3_make(0.0f, 0.0f, 1.0f) : vec3_make(0.0f, 1.0f, 0.0f);
	view = mat4_look_at(eye, center, up);

	// Fit the projection tightly around the eight corners of the map's box
	for (int corner = 0; corner < 8; corner++)
	{
		vec4_t world_corner =
		{
			(corner & 1) ? extent : 0.0f,
			(corner & 2) ? maximum_height : minimum_height,
			(corner & 4) ? extent : 0.0f,
			1.0f
		};
		vec4_t view_corner = mat4_transform(view, world_corner);
		float coordinates[3] = { view_corner.x, view_corner.y, view_corner.z };

		for (int axis = 0; axis < 3; axis++)
		{
			view_minimum[axis] = SDL_min(view_minimum[axis], coordinates[axis]);
			view_maximum[axis] = SDL_max(view_maximum[axis], coordinates[axis]);
		}
	}

	// The view looks down -Z, so the near plane is at -maximum z
	shadow->light_view = view;
	shadow->depth_near = -view_maximum[2] - 1.0f;
	shadow->depth_far = -view_minimum[2] + 1.0f;
	shadow->light_view_projection = mat4_multiply(
		mat4_orthographic(view_minimum[0], view_maximum[0], view_minimum[1], view_maximum[1], shadow->depth_near, shadow->depth_far),
		view);
	shadow->needs_render = true;
}

void shadow_map_update_near_projection(shadow_map_t *near_shadow, const shadow_map_t *far_shadow, vec3_t center, float extent_meters)
{
	vec4_t world_center = { center.x, center.y, center.z, 1.0f };
	vec4_t view_center = mat4_transform(far_shadow->light_view, world_center);
	float half_extent = extent_meters * 0.5f;
	float texel_size = extent_meters / (float)SDL_max(near_shadow->size, 1);
	// Snapped to whole texels: when the square moves, the texels of the
	// map land on the same spots of the ground as before, so shadow edges
	// do not crawl
	float snapped_x = SDL_floorf(view_center.x / texel_size + 0.5f) * texel_size;
	float snapped_y = SDL_floorf(view_center.y / texel_size + 0.5f) * texel_size;

	// Same view and depth range as the far map: everything between the sun
	// and the square casts into it
	near_shadow->light_view = far_shadow->light_view;
	near_shadow->depth_near = far_shadow->depth_near;
	near_shadow->depth_far = far_shadow->depth_far;
	near_shadow->light_view_projection = mat4_multiply(
		mat4_orthographic(snapped_x - half_extent, snapped_x + half_extent, snapped_y - half_extent, snapped_y + half_extent,
			far_shadow->depth_near, far_shadow->depth_far),
		far_shadow->light_view);
	near_shadow->center = center;
	near_shadow->extent_meters = extent_meters;
	near_shadow->needs_render = true;
}

// A pass into target (the map's depth or its still layer), cleared or
// loaded as it is, the light's matrix pushed
static SDL_GPURenderPass *begin_into(shadow_map_t *shadow, SDL_GPUTexture *target, bool clear)
{
	SDL_GPUDepthStencilTargetInfo depth;
	SDL_GPURenderPass *pass;
	float light_view_projection[16];

	SDL_zero(depth);
	depth.texture = target;
	depth.clear_depth = 1.0f;
	depth.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	depth.store_op = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	pass = SDL_BeginGPURenderPass(gpu.command_buffer, NULL, 0, &depth);
	gpu_clip_matrix(shadow->light_view_projection.m, light_view_projection);
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, light_view_projection, sizeof(light_view_projection));
	return pass;
}

SDL_GPURenderPass *shadow_map_begin(shadow_map_t *shadow)
{
	if (shadow->size <= 0 || gpu.command_buffer == NULL)
	{
		shadow->needs_render = false;
		return NULL;
	}
	gpu_flush_uploads();
	return begin_into(shadow, shadow->depth_texture, true);
}

SDL_GPURenderPass *shadow_map_begin_still(shadow_map_t *shadow)
{
	if (shadow->size <= 0 || gpu.command_buffer == NULL)
	{
		shadow->needs_render = false;
		return NULL;
	}
	if (shadow->still_texture == NULL)
	{
		shadow->still_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, gpu.shadow_depth_format, (uint32_t)shadow->size, (uint32_t)shadow->size,
			1, 1, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (shadow->still_texture == NULL)
		{
			return NULL;
		}
	}
	gpu_flush_uploads();
	return begin_into(shadow, shadow->still_texture, true);
}

void shadow_map_mark_moving(shadow_map_t *shadow, vec3_t minimum, vec3_t maximum)
{
	float lowest[2] = { 1e30f, 1e30f };
	float highest[2] = { -1e30f, -1e30f };
	float size = (float)shadow->size;
	shadow_rectangle_t *rectangle;

	if (shadow->size <= 0)
	{
		return;
	}
	// Its corners on the map (orthographic: the box's outline holds it)
	for (int corner = 0; corner < 8; corner++)
	{
		vec3_t point = vec3_make((corner & 1) ? maximum.x : minimum.x, (corner & 2) ? maximum.y : minimum.y, (corner & 4) ? maximum.z : minimum.z);
		const float *m = shadow->light_view_projection.m;
		float clip_x = m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12];
		float clip_y = m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13];
		// (texels from the top left, as the map is drawn: y down)
		float texel_x = (clip_x * 0.5f + 0.5f) * size;
		float texel_y = (0.5f - clip_y * 0.5f) * size;

		lowest[0] = SDL_min(lowest[0], texel_x);
		lowest[1] = SDL_min(lowest[1], texel_y);
		highest[0] = SDL_max(highest[0], texel_x);
		highest[1] = SDL_max(highest[1], texel_y);
	}
	// (a texel more each way: rounding; off the map: nothing)
	lowest[0] = SDL_max(SDL_floorf(lowest[0]) - 1.0f, 0.0f);
	lowest[1] = SDL_max(SDL_floorf(lowest[1]) - 1.0f, 0.0f);
	highest[0] = SDL_min(SDL_ceilf(highest[0]) + 1.0f, size);
	highest[1] = SDL_min(SDL_ceilf(highest[1]) + 1.0f, size);
	if (highest[0] <= lowest[0] || highest[1] <= lowest[1])
	{
		return;
	}
	if (shadow->marked_count >= SHADOW_MAP_MARKS_MAX)
	{
		shadow->marked_overflow = true;
		return;
	}
	rectangle = &shadow->marked[shadow->marked_count++];
	rectangle->x = (uint32_t)lowest[0];
	rectangle->y = (uint32_t)lowest[1];
	rectangle->width = (uint32_t)(highest[0] - lowest[0]);
	rectangle->height = (uint32_t)(highest[1] - lowest[1]);
}

// The still layer copied into the map over a rectangle
static void copy_still(SDL_GPUCopyPass *copy, const shadow_map_t *shadow, const shadow_rectangle_t *rectangle)
{
	SDL_GPUTextureLocation source;
	SDL_GPUTextureLocation destination;

	SDL_zero(source);
	SDL_zero(destination);
	source.texture = shadow->still_texture;
	source.x = rectangle->x;
	source.y = rectangle->y;
	destination.texture = shadow->depth_texture;
	destination.x = rectangle->x;
	destination.y = rectangle->y;
	SDL_CopyGPUTextureToTexture(copy, &source, &destination, rectangle->width, rectangle->height, 1, false);
}

SDL_GPURenderPass *shadow_map_begin_moving(shadow_map_t *shadow, bool whole)
{
	SDL_GPURenderPass *pass = NULL;

	if (shadow->size <= 0 || gpu.command_buffer == NULL)
	{
		shadow->marked_count = 0;
		shadow->marked_overflow = false;
		shadow->drawn_known = false;
		return NULL;
	}
	gpu_flush_uploads();
	if (shadow->still_texture == NULL)
	{
		pass = begin_into(shadow, shadow->depth_texture, true);
		whole = true;
	}
	else
	{
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(gpu.command_buffer);
		// (Direct3D 12 copies a depth texture only whole)
		bool partial = !whole && !shadow->marked_overflow && shadow->drawn_known
			&& SDL_strcmp(SDL_GetGPUDeviceDriver(gpu.device), "direct3d12") != 0;

		if (partial)
		{
			// Where it was drawn the last time, where it is drawn now
			for (int i = 0; i < shadow->drawn_count; i++)
			{
				copy_still(copy, shadow, &shadow->drawn[i]);
			}
			for (int i = 0; i < shadow->marked_count; i++)
			{
				copy_still(copy, shadow, &shadow->marked[i]);
			}
		}
		else
		{
			shadow_rectangle_t all = { 0, 0, (uint32_t)shadow->size, (uint32_t)shadow->size };

			// (a copy on the GPU: a few hundred microseconds for a big map)
			copy_still(copy, shadow, &all);
		}
		SDL_EndGPUCopyPass(copy);
		pass = begin_into(shadow, shadow->depth_texture, false);
	}
	// This frame's marks are the last time's for the next
	SDL_memcpy(shadow->drawn, shadow->marked, sizeof(shadow_rectangle_t) * (size_t)shadow->marked_count);
	shadow->drawn_count = shadow->marked_count;
	shadow->drawn_known = !shadow->marked_overflow;
	shadow->marked_count = 0;
	shadow->marked_overflow = false;
	return pass;
}

void shadow_map_render_terrain(shadow_map_t *shadow, SDL_GPURenderPass *pass, const terrain_t *terrain, SDL_GPUBuffer *chunk_vertex_buffer,
	SDL_GPUBuffer *chunk_index_buffer, int chunk_index_count, SDL_GPUTexture *height_texture, bool cull)
{
	frustum_t frustum = frustum_from_matrix(shadow->light_view_projection);
	float chunk_extent = (float)TERRAIN_CHUNK_SIZE * terrain->cell_size_meters;
	SDL_GPUBufferBinding vertex_binding = { chunk_vertex_buffer, 0 };
	SDL_GPUBufferBinding index_binding = { chunk_index_buffer, 0 };
	SDL_GPUTextureSamplerBinding height_binding = { height_texture, gpu.nearest_clamp };
	struct
	{
		int32_t chunk_origin[2];
		int32_t map_size;
		float cell_size;
	} chunk;

	if (pass == NULL || chunk_vertex_buffer == NULL || height_texture == NULL)
	{
		return;
	}
	SDL_BindGPUGraphicsPipeline(pass, shadow->terrain_pipeline);
	SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
	SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
	SDL_BindGPUVertexSamplers(pass, 0, &height_binding, 1);
	chunk.map_size = terrain->size;
	chunk.cell_size = terrain->cell_size_meters;
	for (int chunk_y = 0; chunk_y < terrain->chunks_per_side; chunk_y++)
	{
		for (int chunk_x = 0; chunk_x < terrain->chunks_per_side; chunk_x++)
		{
			if (cull)
			{
				const terrain_chunk_t *terrain_chunk = &terrain->chunks[chunk_y * terrain->chunks_per_side + chunk_x];
				vec3_t box_minimum = vec3_make((float)chunk_x * chunk_extent, terrain_chunk->minimum_height_meters, (float)chunk_y * chunk_extent);
				vec3_t box_maximum = vec3_make(box_minimum.x + chunk_extent, terrain_chunk->maximum_height_meters, box_minimum.z + chunk_extent);

				if (!frustum_contains_box(&frustum, box_minimum, box_maximum))
				{
					continue;
				}
			}
			chunk.chunk_origin[0] = chunk_x * TERRAIN_CHUNK_SIZE;
			chunk.chunk_origin[1] = chunk_y * TERRAIN_CHUNK_SIZE;
			SDL_PushGPUVertexUniformData(gpu.command_buffer, 1, &chunk, sizeof(chunk));
			SDL_DrawGPUIndexedPrimitives(pass, (uint32_t)chunk_index_count, 1, 0, 0, 0);
		}
	}
}

void shadow_map_end(shadow_map_t *shadow, SDL_GPURenderPass *pass)
{
	if (pass != NULL)
	{
		SDL_EndGPURenderPass(pass);
	}
	shadow->needs_render = false;
}

void shadow_map_bind_for_shading(SDL_GPURenderPass *pass, const shadow_map_t *far_shadow, const shadow_map_t *near_shadow)
{
	bool near_used = near_shadow != NULL && near_shadow->size > 0 && far_shadow->size > 0;
	SDL_GPUTextureSamplerBinding maps[2] =
	{
		{ far_shadow->depth_texture, gpu.shadow_compare },
		{ near_used ? near_shadow->depth_texture : far_shadow->depth_texture, gpu.shadow_compare },
	};

	SDL_BindGPUFragmentSamplers(pass, 0, maps, 2);
}
