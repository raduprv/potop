// Animated actor models: reading .pact files (see actor_model.h).

#include "actor_model.h"
#include "file_io.h"
#include "object_model.h"
#include "gpu.h"

#include <stddef.h>
#include <SDL3/SDL.h>

#include "miniz.h"

// Takes the next size bytes of the file, or fails if it ends before
typedef struct
{
	const uint8_t *data;
	size_t size;
	size_t offset;
	bool failed;
} file_cursor_t;

static const void *take(file_cursor_t *cursor, size_t size)
{
	const void *where;

	if (cursor->failed || size > cursor->size - cursor->offset)
	{
		cursor->failed = true;
		return NULL;
	}
	where = cursor->data + cursor->offset;
	cursor->offset += size;
	return where;
}

// The skinning matrix of one bone, global pose times inverse bind, as three
// rows of a 3x4 matrix
static void skinning_rows(const actor_file_key_t *global, const float inverse_bind[12], float rows_out[12])
{
	float x = global->rotation[0];
	float y = global->rotation[1];
	float z = global->rotation[2];
	float w = global->rotation[3];
	// The pose's rotation as a matrix, rows
	float pose[3][4] = {
		{ 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w), 2.0f * (x * z + y * w), global->translation[0] },
		{ 2.0f * (x * y + z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w), global->translation[1] },
		{ 2.0f * (x * z - y * w), 2.0f * (y * z + x * w), 1.0f - 2.0f * (x * x + y * y), global->translation[2] } };

	for (int row = 0; row < 3; row++)
	{
		for (int column = 0; column < 4; column++)
		{
			float sum = pose[row][0] * inverse_bind[column] + pose[row][1] * inverse_bind[4 + column]
				+ pose[row][2] * inverse_bind[8 + column];

			if (column == 3)
			{
				sum += pose[row][3];
			}
			rows_out[row * 4 + column] = sum;
		}
	}
}

void actor_rows_multiply(const float left[12], const float right[12], float out[12])
{
	float result[12];

	for (int row = 0; row < 3; row++)
	{
		for (int column = 0; column < 4; column++)
		{
			float sum = left[row * 4 + 0] * right[column] + left[row * 4 + 1] * right[4 + column] + left[row * 4 + 2] * right[8 + column];

			if (column == 3)
			{
				sum += left[row * 4 + 3];
			}
			result[row * 4 + column] = sum;
		}
	}
	SDL_memcpy(out, result, sizeof(result));
}

bool actor_rows_invert(const float rows[12], float out[12])
{
	float a = rows[0], b = rows[1], c = rows[2];
	float d = rows[4], e = rows[5], f = rows[6];
	float g = rows[8], h = rows[9], k = rows[10];
	float cofactor_a = e * k - f * h;
	float cofactor_b = f * g - d * k;
	float cofactor_c = d * h - e * g;
	float determinant = a * cofactor_a + b * cofactor_b + c * cofactor_c;
	float inverse[9];

	if (SDL_fabsf(determinant) < 1e-12f)
	{
		return false;
	}
	inverse[0] = cofactor_a / determinant;
	inverse[1] = (c * h - b * k) / determinant;
	inverse[2] = (b * f - c * e) / determinant;
	inverse[3] = cofactor_b / determinant;
	inverse[4] = (a * k - c * g) / determinant;
	inverse[5] = (c * d - a * f) / determinant;
	inverse[6] = cofactor_c / determinant;
	inverse[7] = (b * g - a * h) / determinant;
	inverse[8] = (a * e - b * d) / determinant;
	for (int row = 0; row < 3; row++)
	{
		out[row * 4 + 0] = inverse[row * 3 + 0];
		out[row * 4 + 1] = inverse[row * 3 + 1];
		out[row * 4 + 2] = inverse[row * 3 + 2];
		out[row * 4 + 3] = -(inverse[row * 3 + 0] * rows[3] + inverse[row * 3 + 1] * rows[7] + inverse[row * 3 + 2] * rows[11]);
	}
	return true;
}

static actor_zone_t zone_of_bone(const char *name)
{
	static const char *const head_words[] = { "head", "mouth", "jaw", "ear", "eye", "skull", "beak" };
	static const char *const limb_words[] = { "leg", "calf", "foot", "arm", "hand", "wing", "tail", "toe", "finger", "thigh", "shin", "paw" };

	for (size_t i = 0; i < SDL_arraysize(head_words); i++)
	{
		if (SDL_strcasestr(name, head_words[i]) != NULL)
		{
			return ACTOR_ZONE_HEAD;
		}
	}
	for (size_t i = 0; i < SDL_arraysize(limb_words); i++)
	{
		if (SDL_strcasestr(name, limb_words[i]) != NULL)
		{
			return ACTOR_ZONE_LIMB;
		}
	}
	return ACTOR_ZONE_BODY;
}

// Each bone's box: the vertices it moves most, in its own space (a little
// thickness at least: a flat wing)
static bool make_hit_boxes(actor_model_t *model, const actor_file_vertex_t *vertices, int vertex_count)
{
	static const float THINNEST = 0.02f;
	int bone_count = model->bone_count;

	model->bone_to_model = SDL_calloc((size_t)bone_count * 12, sizeof(float));
	model->hit_boxes = SDL_calloc((size_t)bone_count * 6, sizeof(float));
	model->hit_box_vertices = SDL_calloc((size_t)bone_count, sizeof(int));
	model->bone_zones = SDL_calloc((size_t)bone_count, sizeof(actor_zone_t));
	if (model->bone_to_model == NULL || model->hit_boxes == NULL || model->hit_box_vertices == NULL || model->bone_zones == NULL)
	{
		return false;
	}
	for (int b = 0; b < bone_count; b++)
	{
		float *box = &model->hit_boxes[b * 6];

		if (!actor_rows_invert(model->bones[b].inverse_bind, &model->bone_to_model[b * 12]))
		{
			model->hit_box_vertices[b] = -1;
		}
		model->bone_zones[b] = zone_of_bone(model->bones[b].name);
		box[0] = box[1] = box[2] = 1e30f;
		box[3] = box[4] = box[5] = -1e30f;
	}
	for (int v = 0; v < vertex_count; v++)
	{
		const actor_file_vertex_t *vertex = &vertices[v];
		int bone = vertex->bones[0];
		int most = vertex->weights[0];
		const float *to_bone;
		float *box;

		for (int k = 1; k < ACTOR_FILE_INFLUENCES; k++)
		{
			if (vertex->weights[k] > most)
			{
				most = vertex->weights[k];
				bone = vertex->bones[k];
			}
		}
		if (bone >= bone_count || model->hit_box_vertices[bone] < 0)
		{
			continue;
		}
		to_bone = model->bones[bone].inverse_bind;
		box = &model->hit_boxes[bone * 6];
		for (int axis = 0; axis < 3; axis++)
		{
			float value = to_bone[axis * 4 + 0] * vertex->position[0] + to_bone[axis * 4 + 1] * vertex->position[1]
				+ to_bone[axis * 4 + 2] * vertex->position[2] + to_bone[axis * 4 + 3];

			box[axis] = SDL_min(box[axis], value);
			box[3 + axis] = SDL_max(box[3 + axis], value);
		}
		model->hit_box_vertices[bone]++;
	}
	for (int b = 0; b < bone_count; b++)
	{
		float *box = &model->hit_boxes[b * 6];

		if (model->hit_box_vertices[b] < 3)
		{
			model->hit_box_vertices[b] = 0;
			continue;
		}
		for (int axis = 0; axis < 3; axis++)
		{
			float middle = 0.5f * (box[axis] + box[3 + axis]);
			float half = SDL_max(0.5f * (box[3 + axis] - box[axis]), 0.5f * THINNEST);

			box[axis] = middle - half;
			box[3 + axis] = middle + half;
		}
	}
	return true;
}

// Global poses from local ones (parents come first in the file)
static void compose_global(const actor_model_t *model, const actor_file_key_t *local, actor_file_key_t *global)
{
	for (int b = 0; b < model->bone_count; b++)
	{
		int parent = model->bones[b].parent;

		if (parent < 0)
		{
			global[b] = local[b];
			continue;
		}
		{
			const float *parent_rotation = global[parent].rotation;
			const float *rotation = local[b].rotation;
			const float *translation = local[b].translation;
			float px = parent_rotation[0];
			float py = parent_rotation[1];
			float pz = parent_rotation[2];
			float pw = parent_rotation[3];
			// The parent's rotation applied to the local translation
			float cross_x = 2.0f * (py * translation[2] - pz * translation[1]);
			float cross_y = 2.0f * (pz * translation[0] - px * translation[2]);
			float cross_z = 2.0f * (px * translation[1] - py * translation[0]);

			global[b].translation[0] = global[parent].translation[0] + translation[0] + pw * cross_x + (py * cross_z - pz * cross_y);
			global[b].translation[1] = global[parent].translation[1] + translation[1] + pw * cross_y + (pz * cross_x - px * cross_z);
			global[b].translation[2] = global[parent].translation[2] + translation[2] + pw * cross_z + (px * cross_y - py * cross_x);
			// parent * local
			global[b].rotation[0] = pw * rotation[0] + px * rotation[3] + py * rotation[2] - pz * rotation[1];
			global[b].rotation[1] = pw * rotation[1] - px * rotation[2] + py * rotation[3] + pz * rotation[0];
			global[b].rotation[2] = pw * rotation[2] + px * rotation[1] - py * rotation[0] + pz * rotation[3];
			global[b].rotation[3] = pw * rotation[3] - px * rotation[0] - py * rotation[1] - pz * rotation[2];
		}
	}
}

void actor_model_compose_global(const actor_model_t *model, const actor_file_key_t *local_pose, actor_file_key_t *global_out)
{
	compose_global(model, local_pose, global_out);
}

void actor_model_global_rows(const actor_model_t *model, const actor_file_key_t *global_pose, float *rows_out)
{
	for (int b = 0; b < model->bone_count; b++)
	{
		skinning_rows(&global_pose[b], model->bones[b].inverse_bind, rows_out + b * 12);
	}
}

void actor_model_pose_rows(const actor_model_t *model, const actor_file_key_t *local_pose, float *rows_out)
{
	actor_file_key_t global[ACTOR_FILE_MAX_BONES];

	compose_global(model, local_pose, global);
	actor_model_global_rows(model, global, rows_out);
}

static bool fail(actor_model_t *model, const char *relative_path, const char *reason)
{
	SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Actor '%s': %s", relative_path, reason);
	actor_model_destroy(model);
	return false;
}

// One of the shading maps (as it is: no bleeding, no cut out); NULL if it
// does not unpack
static SDL_GPUTexture *upload_map(const uint8_t *compressed, size_t compressed_size, int width, int height)
{
	mz_ulong size = (mz_ulong)width * (mz_ulong)height * 4u;
	uint8_t *pixels = SDL_malloc(size);
	SDL_GPUTexture *texture = NULL;

	if (pixels == NULL)
	{
		return NULL;
	}
	if (mz_uncompress(pixels, &size, compressed, (mz_ulong)compressed_size) == MZ_OK && size == (mz_ulong)width * (mz_ulong)height * 4u)
	{
		texture = object_texture_upload(pixels, width, height);
	}
	SDL_free(pixels);
	return texture;
}

static bool upload_texture(actor_model_t *model, const uint8_t *compressed, size_t compressed_size, int width, int height)
{
	mz_ulong size = (mz_ulong)width * (mz_ulong)height * 4u;
	uint8_t *pixels = SDL_malloc(size);

	if (pixels == NULL)
	{
		return false;
	}
	if (mz_uncompress(pixels, &size, compressed, (mz_ulong)compressed_size) != MZ_OK || size != (mz_ulong)width * (mz_ulong)height * 4u)
	{
		SDL_free(pixels);
		return false;
	}
	if (model->alpha_tested)
	{
		object_texture_bleed_transparent_colors(pixels, width, height);
	}
	model->texture = object_texture_upload(pixels, width, height);
	SDL_free(pixels);
	return model->texture != NULL;
}

bool actor_model_load(actor_model_t *model, const char *relative_path)
{
	size_t file_size = 0;
	uint8_t *file_data = file_io_load_data_file(relative_path, &file_size);
	file_cursor_t cursor = { file_data, file_size, 0, false };
	const actor_file_header_t *header;
	const actor_file_vertex_t *vertices;
	const uint32_t *indices;
	const uint8_t *texture_data;
	bool ok;

	SDL_zerop(model);
	SDL_strlcpy(model->path, relative_path, sizeof(model->path));
	if (file_data == NULL)
	{
		return fail(model, relative_path, "cannot read the file");
	}
	header = take(&cursor, sizeof(actor_file_header_t));
	if (header == NULL || SDL_memcmp(header->magic, ACTOR_FILE_MAGIC, 4) != 0)
	{
		SDL_free(file_data);
		return fail(model, relative_path, "not a .pact file");
	}
	if (header->version < ACTOR_FILE_OLDEST_VERSION || header->version > ACTOR_FILE_VERSION)
	{
		SDL_free(file_data);
		return fail(model, relative_path, "made by another version of the converter: convert it again");
	}
	if (header->bone_count == 0 || header->bone_count > ACTOR_FILE_MAX_BONES || header->vertex_count == 0
		|| header->index_count == 0 || header->index_count % 3 != 0 || header->texture_width == 0 || header->texture_height == 0
		|| header->texture_width > 8192 || header->texture_height > 8192 || header->clip_count > 1024 || header->action_count > 1024)
	{
		SDL_free(file_data);
		return fail(model, relative_path, "broken counts in the header");
	}

	model->bone_count = (int)header->bone_count;
	model->bones = SDL_malloc(sizeof(actor_file_bone_t) * (size_t)model->bone_count);
	model->bind_rows = SDL_malloc(sizeof(float) * 12 * (size_t)model->bone_count);
	if (model->bones == NULL || model->bind_rows == NULL)
	{
		SDL_free(file_data);
		return fail(model, relative_path, "out of memory");
	}
	ok = true;
	{
		const actor_file_bone_t *bones = take(&cursor, sizeof(actor_file_bone_t) * (size_t)model->bone_count);

		if (bones != NULL)
		{
			SDL_memcpy(model->bones, bones, sizeof(actor_file_bone_t) * (size_t)model->bone_count);
		}
	}
	for (int b = 0; b < model->bone_count && !cursor.failed; b++)
	{
		model->bones[b].name[ACTOR_FILE_NAME_LENGTH - 1] = '\0';
		if (model->bones[b].parent >= b || model->bones[b].parent < -1)
		{
			ok = false;
		}
	}
	vertices = take(&cursor, sizeof(actor_file_vertex_t) * header->vertex_count);
	indices = take(&cursor, sizeof(uint32_t) * header->index_count);
	if (cursor.failed || !ok)
	{
		SDL_free(file_data);
		return fail(model, relative_path, "the file ends early or has broken bones");
	}
	for (uint32_t v = 0; v < header->vertex_count; v++)
	{
		for (int k = 0; k < ACTOR_FILE_INFLUENCES; k++)
		{
			if (vertices[v].bones[k] >= header->bone_count)
			{
				SDL_free(file_data);
				return fail(model, relative_path, "a vertex names a bone that does not exist");
			}
		}
	}
	for (uint32_t i = 0; i < header->index_count; i++)
	{
		if (indices[i] >= header->vertex_count)
		{
			SDL_free(file_data);
			return fail(model, relative_path, "a triangle names a vertex that does not exist");
		}
	}

	// Clips
	model->clip_count = (int)header->clip_count;
	model->clips = SDL_calloc((size_t)SDL_max(model->clip_count, 1), sizeof(actor_clip_t));
	if (model->clips == NULL)
	{
		SDL_free(file_data);
		return fail(model, relative_path, "out of memory");
	}
	for (int c = 0; c < model->clip_count; c++)
	{
		const actor_file_clip_t *info = take(&cursor, sizeof(actor_file_clip_t));
		const actor_file_key_t *keys;
		size_t key_count;

		if (info == NULL || info->frame_count < 2 || info->frame_count > 100000 || !(info->duration_seconds > 0.0f))
		{
			SDL_free(file_data);
			return fail(model, relative_path, "a broken animation clip");
		}
		key_count = (size_t)info->frame_count * (size_t)model->bone_count;
		keys = take(&cursor, sizeof(actor_file_key_t) * key_count);
		if (keys == NULL)
		{
			SDL_free(file_data);
			return fail(model, relative_path, "the file ends inside an animation clip");
		}
		model->clips[c].info = *info;
		model->clips[c].info.name[ACTOR_FILE_NAME_LENGTH - 1] = '\0';
		model->clips[c].keys = SDL_malloc(sizeof(actor_file_key_t) * key_count);
		if (model->clips[c].keys == NULL)
		{
			SDL_free(file_data);
			return fail(model, relative_path, "out of memory");
		}
		SDL_memcpy(model->clips[c].keys, keys, sizeof(actor_file_key_t) * key_count);
	}

	// Actions
	model->action_count = (int)header->action_count;
	model->actions = SDL_calloc((size_t)SDL_max(model->action_count, 1), sizeof(actor_file_action_t));
	{
		const actor_file_action_t *actions = take(&cursor, sizeof(actor_file_action_t) * (size_t)model->action_count);

		if (model->actions == NULL || actions == NULL)
		{
			SDL_free(file_data);
			return fail(model, relative_path, "the file ends inside the actions");
		}
		SDL_memcpy(model->actions, actions, sizeof(actor_file_action_t) * (size_t)model->action_count);
	}
	for (int a = 0; a < model->action_count; a++)
	{
		model->actions[a].name[ACTOR_FILE_NAME_LENGTH - 1] = '\0';
		if (model->actions[a].clip < 0 || model->actions[a].clip >= model->clip_count)
		{
			SDL_free(file_data);
			return fail(model, relative_path, "an action names a clip that does not exist");
		}
	}

	// Texture
	model->alpha_tested = (header->texture_flags & ACTOR_TEXTURE_ALPHA_TESTED) != 0;
	texture_data = take(&cursor, header->texture_compressed_size);
	if (texture_data == NULL || !upload_texture(model, texture_data, header->texture_compressed_size, (int)header->texture_width,
			(int)header->texture_height))
	{
		SDL_free(file_data);
		return fail(model, relative_path, "cannot unpack the texture");
	}

	// The shading maps and the tangents (version 2); without tangents in
	// the file, they are made from the UVs
	{
		const uint32_t sizes[3] = { header->version >= 2 ? header->normal_compressed_size : 0u,
			header->version >= 2 ? header->orm_compressed_size : 0u, header->version >= 2 ? header->emissive_compressed_size : 0u };
		SDL_GPUTexture **targets[3] = { &model->normal_texture, &model->orm_texture, &model->emissive_texture };
		const float *file_tangents = NULL;
		float *tangents;

		for (int map = 0; map < 3; map++)
		{
			const uint8_t *data;

			if (sizes[map] == 0)
			{
				continue;
			}
			data = take(&cursor, sizes[map]);
			if (data == NULL)
			{
				SDL_free(file_data);
				return fail(model, relative_path, "the file ends inside its shading maps");
			}
			*targets[map] = upload_map(data, sizes[map], (int)header->texture_width, (int)header->texture_height);
			model->shaded = model->shaded || *targets[map] != NULL;
		}
		if (header->version >= 2 && (header->extra_flags & ACTOR_FILE_HAS_TANGENTS) != 0)
		{
			file_tangents = take(&cursor, sizeof(float) * 4 * header->vertex_count);
		}
		tangents = SDL_malloc(sizeof(float) * 4 * header->vertex_count);
		if (tangents == NULL)
		{
			SDL_free(file_data);
			return fail(model, relative_path, "out of memory");
		}
		if (file_tangents != NULL)
		{
			SDL_memcpy(tangents, file_tangents, sizeof(float) * 4 * header->vertex_count);
		}
		else
		{
			object_compute_tangents(vertices, sizeof(actor_file_vertex_t), offsetof(actor_file_vertex_t, position),
				offsetof(actor_file_vertex_t, normal), offsetof(actor_file_vertex_t, uv), header->vertex_count, indices, header->index_count,
				tangents);
		}
		model->tangent_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, (uint32_t)(sizeof(float) * 4 * header->vertex_count));
		if (model->tangent_buffer != NULL)
		{
			gpu_upload_buffer(model->tangent_buffer, 0, tangents, sizeof(float) * 4 * header->vertex_count);
		}
		SDL_free(tangents);
		if (model->tangent_buffer == NULL)
		{
			SDL_free(file_data);
			return fail(model, relative_path, "no room for its tangents");
		}
	}

	// Mesh: the file's vertex layout goes to the GPU as it is
	model->index_count = (int)header->index_count;
	{
		uint32_t vertex_bytes = (uint32_t)(sizeof(actor_file_vertex_t) * header->vertex_count);

		model->vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertex_bytes);
		gpu_upload_buffer(model->vertex_buffer, 0, vertices, vertex_bytes);
	}
	if (header->vertex_count <= 65536)
	{
		uint16_t *short_indices = SDL_malloc(sizeof(uint16_t) * header->index_count);

		if (short_indices == NULL)
		{
			SDL_free(file_data);
			return fail(model, relative_path, "out of memory");
		}
		for (uint32_t i = 0; i < header->index_count; i++)
		{
			short_indices[i] = (uint16_t)indices[i];
		}
		model->index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, (uint32_t)(sizeof(uint16_t) * header->index_count));
		gpu_upload_buffer(model->index_buffer, 0, short_indices, sizeof(uint16_t) * header->index_count);
		SDL_free(short_indices);
		model->index_size = SDL_GPU_INDEXELEMENTSIZE_16BIT;
	}
	else
	{
		model->index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, (uint32_t)(sizeof(uint32_t) * header->index_count));
		gpu_upload_buffer(model->index_buffer, 0, indices, sizeof(uint32_t) * header->index_count);
		model->index_size = SDL_GPU_INDEXELEMENTSIZE_32BIT;
	}

	model->bounds_minimum = vec3_make(header->bounds_minimum[0], header->bounds_minimum[1], header->bounds_minimum[2]);
	model->bounds_maximum = vec3_make(header->bounds_maximum[0], header->bounds_maximum[1], header->bounds_maximum[2]);
	{
		// The feet: what is within a tenth of the height from the bottom
		float feet_top = model->bounds_minimum.y + 0.1f * (model->bounds_maximum.y - model->bounds_minimum.y);

		model->feet_minimum_x = model->feet_minimum_z = 1e30f;
		model->feet_maximum_x = model->feet_maximum_z = -1e30f;
		for (uint32_t v = 0; v < header->vertex_count; v++)
		{
			const float *position = vertices[v].position;

			if (position[1] <= feet_top)
			{
				model->feet_minimum_x = SDL_min(model->feet_minimum_x, position[0]);
				model->feet_maximum_x = SDL_max(model->feet_maximum_x, position[0]);
				model->feet_minimum_z = SDL_min(model->feet_minimum_z, position[2]);
				model->feet_maximum_z = SDL_max(model->feet_maximum_z, position[2]);
			}
		}
	}
	{
		actor_file_key_t bind[ACTOR_FILE_MAX_BONES];

		for (int b = 0; b < model->bone_count; b++)
		{
			SDL_memcpy(bind[b].translation, model->bones[b].translation, sizeof(bind[b].translation));
			SDL_memcpy(bind[b].rotation, model->bones[b].rotation, sizeof(bind[b].rotation));
		}
		actor_model_pose_rows(model, bind, model->bind_rows);
	}
	if (!make_hit_boxes(model, vertices, (int)header->vertex_count))
	{
		SDL_free(file_data);
		return fail(model, relative_path, "out of memory");
	}
	SDL_free(file_data);
	if (model->vertex_buffer == NULL || model->index_buffer == NULL)
	{
		return fail(model, relative_path, "the GPU refused its buffers");
	}
	SDL_Log("Actor '%s': %d bones, %d triangles, %d clips, %d actions", relative_path, model->bone_count, model->index_count / 3,
		model->clip_count, model->action_count);
	return true;
}

void actor_model_destroy(actor_model_t *model)
{
	if (model->vertex_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, model->vertex_buffer);
	}
	if (model->index_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, model->index_buffer);
	}
	if (model->tangent_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, model->tangent_buffer);
	}
	if (model->texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, model->texture);
	}
	{
		SDL_GPUTexture *maps[3] = { model->normal_texture, model->orm_texture, model->emissive_texture };

		for (int map = 0; map < 3; map++)
		{
			if (maps[map] != NULL)
			{
				SDL_ReleaseGPUTexture(gpu.device, maps[map]);
			}
		}
	}
	for (int c = 0; c < model->clip_count && model->clips != NULL; c++)
	{
		SDL_free(model->clips[c].keys);
	}
	SDL_free(model->clips);
	SDL_free(model->actions);
	SDL_free(model->bones);
	SDL_free(model->bind_rows);
	SDL_free(model->bone_to_model);
	SDL_free(model->hit_boxes);
	SDL_free(model->hit_box_vertices);
	SDL_free(model->bone_zones);
	SDL_zerop(model);
}

void actor_model_sample_clip(const actor_model_t *model, int clip_index, float seconds, actor_file_key_t *pose_out)
{
	const actor_clip_t *clip = &model->clips[clip_index];
	int last_frame = (int)clip->info.frame_count - 1;
	float position = SDL_clamp(seconds / clip->info.duration_seconds, 0.0f, 1.0f) * (float)last_frame;
	int frame = SDL_min((int)position, last_frame - 1);
	float amount = position - (float)frame;
	const actor_file_key_t *before = clip->keys + (size_t)frame * (size_t)model->bone_count;
	const actor_file_key_t *after = before + model->bone_count;

	for (int b = 0; b < model->bone_count; b++)
	{
		const float *from = before[b].rotation;
		const float *to = after[b].rotation;
		// Frames are a thirtieth of a second apart: a normalized straight
		// line between the rotations is as good as a slerp, along the
		// shorter way
		float sign = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3] < 0.0f ? -1.0f : 1.0f;
		float length_squared = 0.0f;

		for (int k = 0; k < 3; k++)
		{
			pose_out[b].translation[k] = before[b].translation[k] + (after[b].translation[k] - before[b].translation[k]) * amount;
		}
		for (int k = 0; k < 4; k++)
		{
			pose_out[b].rotation[k] = from[k] + (sign * to[k] - from[k]) * amount;
			length_squared += pose_out[b].rotation[k] * pose_out[b].rotation[k];
		}
		{
			float scale = length_squared > 1e-12f ? 1.0f / SDL_sqrtf(length_squared) : 1.0f;

			for (int k = 0; k < 4; k++)
			{
				pose_out[b].rotation[k] *= scale;
			}
		}
	}
}

int actor_model_find_action(const actor_model_t *model, const char *name)
{
	for (int a = 0; a < model->action_count; a++)
	{
		if (SDL_strcmp(model->actions[a].name, name) == 0)
		{
			return a;
		}
	}
	return -1;
}

int actor_model_find_action_prefix(const actor_model_t *model, const char *prefix)
{
	size_t length = SDL_strlen(prefix);

	for (int a = 0; a < model->action_count; a++)
	{
		if (SDL_strncmp(model->actions[a].name, prefix, length) == 0)
		{
			return a;
		}
	}
	return -1;
}
