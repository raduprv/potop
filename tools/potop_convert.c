// potop_convert: turns animated actors from other formats into the game's
// own .pact files (see engine/actor_format.h). A desktop tool: it is not part
// of the game and never ships with it.
//
//   potop_convert [--root DIRECTORY] actor.xml output.pact
//       converts an Eternal Lands actor definition: the Cal3D skeleton
//       (.csf), mesh (.cmf) and animations (.caf) it names, and its DDS
//       skin. Paths in the definition are relative to the Eternal Lands
//       data directory (by default the parent of the definition's
//       directory); a file not found there is looked for by its name next
//       to the definition.
//   potop_convert [options] model.gltf|model.glb output.pact|output.pobj|output.e3d
//       converts a glTF 2.0 model: into an animated actor (.pact), an
//       object (.pobj) or an Eternal Lands object (.e3d, its textures as
//       PNG files next to it). See
//       convert_gltf.h, and its usage for the options (--list shows what
//       a file holds).
//   potop_convert --dump file.pact
//       prints what a .pact file holds.
//
// Only the binary Cal3D files of format version 700 are read for now (what
// Eternal Lands uses); others are refused with a message rather than read
// wrong.
//
// Cal3D, like Eternal Lands, has Z up; the game has Y up with the map's
// rows along +Z, so a point (x, y, z) becomes (x, z, -y), as for the .e3d
// objects (see engine/object_model.h). Cal3D also writes its quaternions the
// other way around from the usual convention (it turns a vector by
// q* v q), so they are conjugated on the way in.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL.h>

#include "actor_format.h"
#include "convert_gltf.h"
#include "dds.h"
#include "miniz.h"

#define CAL3D_SUPPORTED_VERSION_MINIMUM 700
#define CAL3D_SUPPORTED_VERSION_MAXIMUM 999
#define SAMPLES_PER_SECOND 30.0f
#define MAX_ACTIONS 128
#define MAX_CLIPS 64
#define PATH_LENGTH 1024

// ---------------------------------------------------------------------------
// Small math: quaternions in the usual convention, (x, y, z, w)

typedef struct
{
	float x;
	float y;
	float z;
	float w;
} quaternion_t;

typedef struct
{
	float x;
	float y;
	float z;
} vector_t;

static quaternion_t quaternion_multiply(quaternion_t left, quaternion_t right)
{
	quaternion_t result;

	result.x = left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y;
	result.y = left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x;
	result.z = left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w;
	result.w = left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z;
	return result;
}

static quaternion_t quaternion_conjugate(quaternion_t rotation)
{
	quaternion_t result = { -rotation.x, -rotation.y, -rotation.z, rotation.w };

	return result;
}

static quaternion_t quaternion_normalize(quaternion_t rotation)
{
	float length = sqrtf(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w);
	quaternion_t identity = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (length < 1e-12f)
	{
		return identity;
	}
	rotation.x /= length;
	rotation.y /= length;
	rotation.z /= length;
	rotation.w /= length;
	return rotation;
}

static vector_t quaternion_rotate(quaternion_t rotation, vector_t vector)
{
	// v + 2w (r x v) + 2 r x (r x v), with r the vector part
	vector_t twice_cross = {
		2.0f * (rotation.y * vector.z - rotation.z * vector.y),
		2.0f * (rotation.z * vector.x - rotation.x * vector.z),
		2.0f * (rotation.x * vector.y - rotation.y * vector.x) };
	vector_t result;

	result.x = vector.x + rotation.w * twice_cross.x + (rotation.y * twice_cross.z - rotation.z * twice_cross.y);
	result.y = vector.y + rotation.w * twice_cross.y + (rotation.z * twice_cross.x - rotation.x * twice_cross.z);
	result.z = vector.z + rotation.w * twice_cross.z + (rotation.x * twice_cross.y - rotation.y * twice_cross.x);
	return result;
}

// Spherical interpolation along the shorter way
static quaternion_t quaternion_slerp(quaternion_t from, quaternion_t to, float amount)
{
	float cosine = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
	float from_weight;
	float to_weight;
	quaternion_t result;

	if (cosine < 0.0f)
	{
		cosine = -cosine;
		to.x = -to.x;
		to.y = -to.y;
		to.z = -to.z;
		to.w = -to.w;
	}
	if (cosine > 0.9995f)
	{
		from_weight = 1.0f - amount;
		to_weight = amount;
	}
	else
	{
		float angle = acosf(cosine);
		float sine = sinf(angle);

		from_weight = sinf((1.0f - amount) * angle) / sine;
		to_weight = sinf(amount * angle) / sine;
	}
	result.x = from.x * from_weight + to.x * to_weight;
	result.y = from.y * from_weight + to.y * to_weight;
	result.z = from.z * from_weight + to.z * to_weight;
	result.w = from.w * from_weight + to.w * to_weight;
	return quaternion_normalize(result);
}

// Cal3D (Z up) to the game (Y up): (x, y, z) -> (x, z, -y)
static vector_t source_to_game_vector(vector_t vector)
{
	vector_t result = { vector.x, vector.z, -vector.y };

	return result;
}

// A Cal3D quaternion to the usual convention (conjugate) in game axes (the
// vector part turns like a vector)
static quaternion_t source_to_game_rotation(quaternion_t rotation)
{
	quaternion_t usual = quaternion_conjugate(rotation);
	quaternion_t result = { usual.x, usual.z, -usual.y, usual.w };

	return quaternion_normalize(result);
}

// ---------------------------------------------------------------------------
// Reading binary files

typedef struct
{
	const char *path;
	uint8_t *data;
	size_t size;
	size_t offset;
	bool failed;
} reader_t;

static bool reader_open(reader_t *reader, const char *path)
{
	FILE *file = fopen(path, "rb");
	long size;

	memset(reader, 0, sizeof(*reader));
	reader->path = path;
	if (file == NULL)
	{
		fprintf(stderr, "Cannot open '%s'\n", path);
		return false;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	reader->data = malloc(size > 0 ? (size_t)size : 1);
	if (reader->data == NULL || size < 0 || fread(reader->data, 1, (size_t)size, file) != (size_t)size)
	{
		fprintf(stderr, "Cannot read '%s'\n", path);
		fclose(file);
		free(reader->data);
		reader->data = NULL;
		return false;
	}
	fclose(file);
	reader->size = (size_t)size;
	return true;
}

static void reader_close(reader_t *reader)
{
	free(reader->data);
	reader->data = NULL;
}

static bool reader_take(reader_t *reader, void *out, size_t size)
{
	if (reader->failed || reader->offset + size > reader->size)
	{
		if (!reader->failed)
		{
			fprintf(stderr, "'%s' ends too early (at byte %zu)\n", reader->path, reader->offset);
		}
		reader->failed = true;
		memset(out, 0, size);
		return false;
	}
	memcpy(out, reader->data + reader->offset, size);
	reader->offset += size;
	return true;
}

static int32_t read_int(reader_t *reader)
{
	uint8_t bytes[4];

	reader_take(reader, bytes, 4);
	return (int32_t)((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24));
}

static float read_float(reader_t *reader)
{
	int32_t bits = read_int(reader);
	float value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

static vector_t read_vector(reader_t *reader)
{
	vector_t vector;

	vector.x = read_float(reader);
	vector.y = read_float(reader);
	vector.z = read_float(reader);
	return vector;
}

static quaternion_t read_quaternion(reader_t *reader)
{
	quaternion_t rotation;

	rotation.x = read_float(reader);
	rotation.y = read_float(reader);
	rotation.z = read_float(reader);
	rotation.w = read_float(reader);
	return rotation;
}

// The four byte magic ("CSF\0" and so on) and the version
static bool read_cal3d_header(reader_t *reader, const char *magic)
{
	char found[4];
	int32_t version;

	reader_take(reader, found, 4);
	if (reader->failed || memcmp(found, magic, 3) != 0 || found[3] != '\0')
	{
		fprintf(stderr, "'%s' is not a binary Cal3D %s file\n", reader->path, magic);
		return false;
	}
	version = read_int(reader);
	if (version < CAL3D_SUPPORTED_VERSION_MINIMUM || version > CAL3D_SUPPORTED_VERSION_MAXIMUM)
	{
		fprintf(stderr, "'%s' has Cal3D version %d; only %d to %d can be read for now\n", reader->path, (int)version,
			CAL3D_SUPPORTED_VERSION_MINIMUM, CAL3D_SUPPORTED_VERSION_MAXIMUM);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Cal3D skeleton (.csf)

typedef struct
{
	char name[ACTOR_FILE_NAME_LENGTH];
	int parent;                        // in the source's order
	vector_t translation;              // game axes, usual convention
	quaternion_t rotation;
	vector_t bone_space_translation;   // model space to bone space, as the file says
	quaternion_t bone_space_rotation;
} source_bone_t;

typedef struct
{
	source_bone_t *bones;
	int bone_count;
} source_skeleton_t;

static bool load_skeleton(const char *path, source_skeleton_t *skeleton)
{
	reader_t reader;
	int32_t bone_count;

	memset(skeleton, 0, sizeof(*skeleton));
	if (!reader_open(&reader, path))
	{
		return false;
	}
	if (!read_cal3d_header(&reader, "CSF"))
	{
		reader_close(&reader);
		return false;
	}
	bone_count = read_int(&reader);
	if (bone_count <= 0 || bone_count > ACTOR_FILE_MAX_BONES)
	{
		fprintf(stderr, "'%s' has %d bones; 1 to %d are supported\n", path, (int)bone_count, ACTOR_FILE_MAX_BONES);
		reader_close(&reader);
		return false;
	}
	skeleton->bones = calloc((size_t)bone_count, sizeof(source_bone_t));
	skeleton->bone_count = bone_count;
	for (int b = 0; b < bone_count && !reader.failed; b++)
	{
		source_bone_t *bone = &skeleton->bones[b];
		int32_t name_length = read_int(&reader);
		int32_t child_count;

		if (name_length < 0 || reader.offset + (size_t)name_length > reader.size)
		{
			fprintf(stderr, "'%s': bone %d has a broken name\n", path, b);
			reader.failed = true;
			break;
		}
		{
			int copied = name_length < ACTOR_FILE_NAME_LENGTH - 1 ? name_length : ACTOR_FILE_NAME_LENGTH - 1;

			memcpy(bone->name, reader.data + reader.offset, (size_t)copied);
			bone->name[copied] = '\0';
			reader.offset += (size_t)name_length;
		}
		bone->translation = source_to_game_vector(read_vector(&reader));
		bone->rotation = source_to_game_rotation(read_quaternion(&reader));
		bone->bone_space_translation = source_to_game_vector(read_vector(&reader));
		bone->bone_space_rotation = source_to_game_rotation(read_quaternion(&reader));
		bone->parent = read_int(&reader);
		child_count = read_int(&reader);
		if (bone->parent < -1 || bone->parent >= bone_count || child_count < 0 || child_count > bone_count)
		{
			fprintf(stderr, "'%s': bone %d has a broken parent or child list\n", path, b);
			reader.failed = true;
			break;
		}
		// The child list is implied by the parents
		for (int c = 0; c < child_count; c++)
		{
			read_int(&reader);
		}
	}
	reader_close(&reader);
	if (reader.failed)
	{
		free(skeleton->bones);
		skeleton->bones = NULL;
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Cal3D mesh (.cmf)

typedef struct
{
	vector_t position;
	vector_t normal;
	float uv[2];
	int bones[ACTOR_FILE_INFLUENCES];  // source bone indices
	float weights[ACTOR_FILE_INFLUENCES];
} source_vertex_t;

typedef struct
{
	source_vertex_t *vertices;
	int vertex_count;
	uint32_t *indices;
	int index_count;
	int dropped_influences;            // beyond four on a vertex
	float dropped_weight_maximum;      // the heaviest of those
} source_mesh_t;

static bool load_mesh(const char *path, int bone_count, source_mesh_t *mesh)
{
	reader_t reader;
	int32_t submesh_count;

	memset(mesh, 0, sizeof(*mesh));
	if (!reader_open(&reader, path))
	{
		return false;
	}
	if (!read_cal3d_header(&reader, "CMF"))
	{
		reader_close(&reader);
		return false;
	}
	submesh_count = read_int(&reader);
	if (submesh_count < 0 || submesh_count > 256)
	{
		fprintf(stderr, "'%s' has a broken submesh count\n", path);
		reader_close(&reader);
		return false;
	}
	// All submeshes go into one mesh: there is one texture per actor
	for (int s = 0; s < submesh_count && !reader.failed; s++)
	{
		int32_t material = read_int(&reader);
		int32_t vertex_count = read_int(&reader);
		int32_t face_count = read_int(&reader);
		int32_t lod_count = read_int(&reader);
		int32_t spring_count = read_int(&reader);
		int32_t map_count = read_int(&reader);
		int first_vertex = mesh->vertex_count;

		(void)material;
		(void)lod_count;
		if (vertex_count < 0 || face_count < 0 || spring_count < 0 || map_count < 0 || map_count > 8
			|| vertex_count > 1000000 || face_count > 1000000)
		{
			fprintf(stderr, "'%s': submesh %d has broken counts\n", path, s);
			reader.failed = true;
			break;
		}
		mesh->vertices = realloc(mesh->vertices, sizeof(source_vertex_t) * (size_t)(mesh->vertex_count + vertex_count));
		mesh->indices = realloc(mesh->indices, sizeof(uint32_t) * (size_t)(mesh->index_count + face_count * 3));
		for (int v = 0; v < vertex_count && !reader.failed; v++)
		{
			source_vertex_t *vertex = &mesh->vertices[mesh->vertex_count++];
			int32_t influence_count;
			int kept = 0;

			memset(vertex, 0, sizeof(*vertex));
			vertex->position = source_to_game_vector(read_vector(&reader));
			vertex->normal = source_to_game_vector(read_vector(&reader));
			read_int(&reader);         // collapse id (level of detail)
			read_int(&reader);         // face collapse count
			for (int m = 0; m < map_count; m++)
			{
				float u = read_float(&reader);
				float uv_v = read_float(&reader);

				// Eternal Lands' Cal3D meshes count v from the other end
				// than its .e3d objects: against the texture's rows (top
				// first, see engine/dds.h) it is flipped, which the UV
				// layout over the bear's skin shows plainly
				if (m == 0)
				{
					vertex->uv[0] = u;
					vertex->uv[1] = 1.0f - uv_v;
				}
			}
			influence_count = read_int(&reader);
			if (influence_count < 0 || influence_count > 64)
			{
				fprintf(stderr, "'%s': vertex %d has %d influences\n", path, v, (int)influence_count);
				reader.failed = true;
				break;
			}
			for (int i = 0; i < influence_count; i++)
			{
				int32_t bone = read_int(&reader);
				float weight = read_float(&reader);

				if (bone < 0 || bone >= bone_count)
				{
					fprintf(stderr, "'%s': vertex %d names bone %d, the skeleton has %d\n", path, v, (int)bone, bone_count);
					reader.failed = true;
					break;
				}
				// Keep the four heaviest
				if (kept < ACTOR_FILE_INFLUENCES)
				{
					vertex->bones[kept] = bone;
					vertex->weights[kept] = weight;
					kept++;
				}
				else
				{
					int lightest = 0;

					for (int k = 1; k < ACTOR_FILE_INFLUENCES; k++)
					{
						if (vertex->weights[k] < vertex->weights[lightest])
						{
							lightest = k;
						}
					}
					mesh->dropped_influences++;
					if (weight > vertex->weights[lightest])
					{
						float dropped = vertex->weights[lightest];

						vertex->bones[lightest] = bone;
						vertex->weights[lightest] = weight;
						weight = dropped;
					}
					if (weight > mesh->dropped_weight_maximum)
					{
						mesh->dropped_weight_maximum = weight;
					}
				}
			}
			if (spring_count > 0)
			{
				read_float(&reader);   // physical property weight (cloth), not used
			}
		}
		for (int k = 0; k < spring_count; k++)
		{
			read_int(&reader);
			read_int(&reader);
			read_float(&reader);
			read_float(&reader);
		}
		for (int f = 0; f < face_count && !reader.failed; f++)
		{
			for (int k = 0; k < 3; k++)
			{
				int32_t index = read_int(&reader);

				if (index < 0 || index >= vertex_count)
				{
					fprintf(stderr, "'%s': face %d names vertex %d of %d\n", path, f, (int)index, (int)vertex_count);
					reader.failed = true;
					break;
				}
				mesh->indices[mesh->index_count++] = (uint32_t)(first_vertex + index);
			}
		}
	}
	reader_close(&reader);
	if (reader.failed || mesh->vertex_count == 0 || mesh->index_count == 0)
	{
		if (!reader.failed)
		{
			fprintf(stderr, "'%s' has no triangles\n", path);
		}
		free(mesh->vertices);
		free(mesh->indices);
		memset(mesh, 0, sizeof(*mesh));
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Cal3D animation (.caf)

typedef struct
{
	float time_seconds;
	vector_t translation;              // game axes, usual convention
	quaternion_t rotation;
} source_key_t;

typedef struct
{
	int bone;                          // source bone index
	source_key_t *keys;
	int key_count;
} source_track_t;

typedef struct
{
	char name[ACTOR_FILE_NAME_LENGTH];
	char path[PATH_LENGTH];
	float duration_seconds;
	source_track_t *tracks;
	int track_count;
} source_animation_t;

static bool load_animation(const char *path, int bone_count, source_animation_t *animation)
{
	reader_t reader;
	int32_t track_count;

	if (!reader_open(&reader, path))
	{
		return false;
	}
	if (!read_cal3d_header(&reader, "CAF"))
	{
		reader_close(&reader);
		return false;
	}
	animation->duration_seconds = read_float(&reader);
	track_count = read_int(&reader);
	if (!(animation->duration_seconds > 0.0f) || track_count < 0 || track_count > 1024)
	{
		fprintf(stderr, "'%s' has a broken duration or track count\n", path);
		reader_close(&reader);
		return false;
	}
	animation->tracks = calloc((size_t)(track_count > 0 ? track_count : 1), sizeof(source_track_t));
	animation->track_count = track_count;
	for (int t = 0; t < track_count && !reader.failed; t++)
	{
		source_track_t *track = &animation->tracks[t];
		int32_t key_count;

		track->bone = read_int(&reader);
		key_count = read_int(&reader);
		if (track->bone < 0 || track->bone >= bone_count || key_count <= 0 || key_count > 100000)
		{
			fprintf(stderr, "'%s': track %d has bone %d and %d keys\n", path, t, track->bone, (int)key_count);
			reader.failed = true;
			break;
		}
		track->keys = calloc((size_t)key_count, sizeof(source_key_t));
		track->key_count = key_count;
		for (int k = 0; k < key_count; k++)
		{
			source_key_t *key = &track->keys[k];

			key->time_seconds = read_float(&reader);
			key->translation = source_to_game_vector(read_vector(&reader));
			key->rotation = source_to_game_rotation(read_quaternion(&reader));
			if (k > 0 && key->time_seconds < track->keys[k - 1].time_seconds)
			{
				fprintf(stderr, "'%s': track %d has keys out of order\n", path, t);
				reader.failed = true;
				break;
			}
		}
	}
	reader_close(&reader);
	return !reader.failed;
}

// The track's pose at a time: held before the first key and after the
// last, interpolated in between
static void sample_track(const source_track_t *track, float time_seconds, vector_t *translation_out, quaternion_t *rotation_out)
{
	const source_key_t *keys = track->keys;
	int next = 0;

	while (next < track->key_count && keys[next].time_seconds < time_seconds)
	{
		next++;
	}
	if (next == 0)
	{
		*translation_out = keys[0].translation;
		*rotation_out = keys[0].rotation;
		return;
	}
	if (next == track->key_count)
	{
		*translation_out = keys[next - 1].translation;
		*rotation_out = keys[next - 1].rotation;
		return;
	}
	{
		const source_key_t *before = &keys[next - 1];
		const source_key_t *after = &keys[next];
		float span = after->time_seconds - before->time_seconds;
		float amount = span > 1e-9f ? (time_seconds - before->time_seconds) / span : 1.0f;

		translation_out->x = before->translation.x + (after->translation.x - before->translation.x) * amount;
		translation_out->y = before->translation.y + (after->translation.y - before->translation.y) * amount;
		translation_out->z = before->translation.z + (after->translation.z - before->translation.z) * amount;
		*rotation_out = quaternion_slerp(before->rotation, after->rotation, amount);
	}
}

// ---------------------------------------------------------------------------
// The Eternal Lands actor definition (XML), read just far enough

typedef struct
{
	char name[ACTOR_FILE_NAME_LENGTH];
	char path[PATH_LENGTH];
	bool loop;
	float duration_seconds;            // 0: the animation's own
} source_action_t;

typedef struct
{
	char skin[PATH_LENGTH];
	char mesh[PATH_LENGTH];
	char skeleton[PATH_LENGTH];
	source_action_t actions[MAX_ACTIONS];
	int action_count;
} actor_definition_t;

static void trim_copy(char *destination, size_t destination_size, const char *start, const char *end)
{
	size_t length;

	while (start < end && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n'))
	{
		start++;
	}
	while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
	{
		end--;
	}
	length = (size_t)(end - start);
	if (length >= destination_size)
	{
		length = destination_size - 1;
	}
	memcpy(destination, start, length);
	destination[length] = '\0';
}

// The text between <tag> and </tag>, searched from text on
static bool find_element_text(const char *text, const char *tag, char *out, size_t out_size)
{
	char open[64];
	char close[64];
	const char *start;
	const char *end;

	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	start = strstr(text, open);
	if (start == NULL)
	{
		return false;
	}
	start += strlen(open);
	end = strstr(start, close);
	if (end == NULL)
	{
		return false;
	}
	trim_copy(out, out_size, start, end);
	return true;
}

static bool load_definition(const char *path, actor_definition_t *definition)
{
	reader_t reader;
	char *text;
	const char *frames;
	const char *frames_end;
	const char *cursor;

	memset(definition, 0, sizeof(*definition));
	if (!reader_open(&reader, path))
	{
		return false;
	}
	text = malloc(reader.size + 1);
	memcpy(text, reader.data, reader.size);
	text[reader.size] = '\0';
	reader_close(&reader);

	if (!find_element_text(text, "mesh", definition->mesh, sizeof(definition->mesh))
		|| !find_element_text(text, "skeleton", definition->skeleton, sizeof(definition->skeleton))
		|| !find_element_text(text, "skin", definition->skin, sizeof(definition->skin)))
	{
		fprintf(stderr, "'%s' lacks a <mesh>, <skeleton> or <skin>\n", path);
		free(text);
		return false;
	}

	// <frames><CAL_walk duration="500">./animations/bear_walk.caf 0</CAL_walk>...
	// The number after the file: 0 loops, 1 plays once
	frames = strstr(text, "<frames>");
	frames_end = frames != NULL ? strstr(frames, "</frames>") : NULL;
	if (frames == NULL || frames_end == NULL)
	{
		fprintf(stderr, "'%s' has no <frames>\n", path);
		free(text);
		return false;
	}
	cursor = frames + strlen("<frames>");
	while (cursor < frames_end && definition->action_count < MAX_ACTIONS)
	{
		const char *tag_start = strchr(cursor, '<');
		const char *tag_end;
		const char *name_end;
		const char *content_end;
		char tag_name[96];
		char attributes[256];
		char content[PATH_LENGTH + 16];
		source_action_t *action;
		const char *duration;
		char *space;

		if (tag_start == NULL || tag_start >= frames_end)
		{
			break;
		}
		if (tag_start[1] == '/' || tag_start[1] == '!')
		{
			cursor = tag_start + 1;
			continue;
		}
		tag_end = strchr(tag_start, '>');
		if (tag_end == NULL || tag_end > frames_end)
		{
			break;
		}
		name_end = tag_start + 1;
		while (name_end < tag_end && *name_end != ' ' && *name_end != '\t' && *name_end != '/')
		{
			name_end++;
		}
		trim_copy(tag_name, sizeof(tag_name), tag_start + 1, name_end);
		trim_copy(attributes, sizeof(attributes), name_end, tag_end);
		content_end = strstr(tag_end, "</");
		if (content_end == NULL || content_end > frames_end)
		{
			break;
		}
		trim_copy(content, sizeof(content), tag_end + 1, content_end);
		cursor = content_end + 2;

		action = &definition->actions[definition->action_count++];
		// CAL_walk -> walk
		snprintf(action->name, sizeof(action->name), "%s", strncmp(tag_name, "CAL_", 4) == 0 ? tag_name + 4 : tag_name);
		space = strrchr(content, ' ');
		action->loop = true;
		if (space != NULL)
		{
			action->loop = atoi(space + 1) == 0;
			*space = '\0';
		}
		snprintf(action->path, sizeof(action->path), "%s", content);
		duration = strstr(attributes, "duration=\"");
		action->duration_seconds = duration != NULL ? (float)atof(duration + strlen("duration=\"")) / 1000.0f : 0.0f;
	}
	free(text);
	if (definition->action_count == 0)
	{
		fprintf(stderr, "'%s' has no animations under <frames>\n", path);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Paths

static bool file_exists(const char *path)
{
	FILE *file = fopen(path, "rb");

	if (file == NULL)
	{
		return false;
	}
	fclose(file);
	return true;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *backslash = strrchr(path, '\\');

	if (backslash != NULL && (slash == NULL || backslash > slash))
	{
		slash = backslash;
	}
	return slash != NULL ? slash + 1 : path;
}

// "./meshes/bear1.cmf" under the data root, or "bear1.cmf" next to the
// definition
static bool resolve_path(const char *root, const char *definition_directory, const char *relative, char *out, size_t out_size)
{
	const char *trimmed = relative;

	while (strncmp(trimmed, "./", 2) == 0)
	{
		trimmed += 2;
	}
	snprintf(out, out_size, "%s/%s", root, trimmed);
	if (file_exists(out))
	{
		return true;
	}
	snprintf(out, out_size, "%s/%s", definition_directory, base_name(trimmed));
	if (file_exists(out))
	{
		return true;
	}
	fprintf(stderr, "Cannot find '%s' (looked under '%s' and next to the definition)\n", relative, root);
	return false;
}

// The file's name without directory and extension
static void stem_of(const char *path, char *out, size_t out_size)
{
	const char *name = base_name(path);
	const char *dot = strrchr(name, '.');
	size_t length = dot != NULL ? (size_t)(dot - name) : strlen(name);

	if (length >= out_size)
	{
		length = out_size - 1;
	}
	memcpy(out, name, length);
	out[length] = '\0';
}

// ---------------------------------------------------------------------------
// Building the actor

typedef struct
{
	vector_t translation;
	quaternion_t rotation;
} pose_t;

// Global poses from local ones; bones are in an order where parents come
// first
static void compose_global_poses(const pose_t *local, const int *parents, int bone_count, pose_t *global)
{
	for (int b = 0; b < bone_count; b++)
	{
		if (parents[b] < 0)
		{
			global[b] = local[b];
		}
		else
		{
			const pose_t *parent = &global[parents[b]];
			vector_t moved = quaternion_rotate(parent->rotation, local[b].translation);

			global[b].translation.x = parent->translation.x + moved.x;
			global[b].translation.y = parent->translation.y + moved.y;
			global[b].translation.z = parent->translation.z + moved.z;
			global[b].rotation = quaternion_normalize(quaternion_multiply(parent->rotation, local[b].rotation));
		}
	}
}

// The inverse of a pose, as 3 rows of a 3x4 matrix
static void inverse_pose_rows(const pose_t *pose, float rows_out[12])
{
	quaternion_t inverse = quaternion_conjugate(pose->rotation);
	vector_t axis_x = quaternion_rotate(inverse, (vector_t){ 1.0f, 0.0f, 0.0f });
	vector_t axis_y = quaternion_rotate(inverse, (vector_t){ 0.0f, 1.0f, 0.0f });
	vector_t axis_z = quaternion_rotate(inverse, (vector_t){ 0.0f, 0.0f, 1.0f });
	vector_t moved = quaternion_rotate(inverse, pose->translation);
	// Column c of the rotation is the image of axis c
	float columns[3][3] = {
		{ axis_x.x, axis_x.y, axis_x.z },
		{ axis_y.x, axis_y.y, axis_y.z },
		{ axis_z.x, axis_z.y, axis_z.z } };

	for (int row = 0; row < 3; row++)
	{
		rows_out[row * 4 + 0] = columns[0][row];
		rows_out[row * 4 + 1] = columns[1][row];
		rows_out[row * 4 + 2] = columns[2][row];
	}
	rows_out[3] = -moved.x;
	rows_out[7] = -moved.y;
	rows_out[11] = -moved.z;
}

static void write_or_fail(FILE *file, const void *data, size_t size, bool *ok)
{
	if (*ok && size > 0 && fwrite(data, 1, size, file) != size)
	{
		*ok = false;
	}
}

static int convert_actor(const char *definition_path, const char *root_argument, const char *output_path)
{
	actor_definition_t definition;
	char definition_directory[PATH_LENGTH];
	char root[PATH_LENGTH];
	char path[PATH_LENGTH];
	source_skeleton_t skeleton;
	source_mesh_t mesh;
	source_animation_t *animations;
	int animation_count = 0;
	int order[ACTOR_FILE_MAX_BONES];       // new index -> source index
	int new_index[ACTOR_FILE_MAX_BONES];   // source index -> new index
	int parents[ACTOR_FILE_MAX_BONES];     // in the new order
	pose_t bind_local[ACTOR_FILE_MAX_BONES];
	pose_t bind_global[ACTOR_FILE_MAX_BONES];
	actor_file_header_t header;
	actor_file_bone_t bones[ACTOR_FILE_MAX_BONES];
	actor_file_action_t actions[MAX_ACTIONS];
	uint8_t *pixels = NULL;
	int texture_width = 0;
	int texture_height = 0;
	bool has_alpha = false;
	bool alpha_tested = false;
	unsigned char *compressed = NULL;
	mz_ulong compressed_size = 0;
	float bind_mismatch = 0.0f;
	FILE *output;
	bool ok = true;

	if (!load_definition(definition_path, &definition))
	{
		return 1;
	}
	snprintf(definition_directory, sizeof(definition_directory), "%s", definition_path);
	{
		char *slash = strrchr(definition_directory, '/');

		if (slash != NULL)
		{
			*slash = '\0';
		}
		else
		{
			snprintf(definition_directory, sizeof(definition_directory), ".");
		}
	}
	if (root_argument != NULL)
	{
		snprintf(root, sizeof(root), "%s", root_argument);
	}
	else
	{
		snprintf(root, sizeof(root), "%s/..", definition_directory);
	}

	// Skeleton, with the bones put in an order where every parent comes
	// before its children
	if (!resolve_path(root, definition_directory, definition.skeleton, path, sizeof(path)) || !load_skeleton(path, &skeleton))
	{
		return 1;
	}
	{
		int placed = 0;
		bool is_placed[ACTOR_FILE_MAX_BONES] = { false };

		while (placed < skeleton.bone_count)
		{
			int placed_before = placed;

			for (int b = 0; b < skeleton.bone_count; b++)
			{
				int parent = skeleton.bones[b].parent;

				if (!is_placed[b] && (parent < 0 || is_placed[parent]))
				{
					is_placed[b] = true;
					new_index[b] = placed;
					order[placed++] = b;
				}
			}
			if (placed == placed_before)
			{
				fprintf(stderr, "The skeleton's parents form a loop\n");
				return 1;
			}
		}
	}
	for (int b = 0; b < skeleton.bone_count; b++)
	{
		const source_bone_t *source = &skeleton.bones[order[b]];

		parents[b] = source->parent < 0 ? -1 : new_index[source->parent];
		bind_local[b].translation = source->translation;
		bind_local[b].rotation = source->rotation;
	}
	compose_global_poses(bind_local, parents, skeleton.bone_count, bind_global);
	memset(bones, 0, sizeof(bones));
	for (int b = 0; b < skeleton.bone_count; b++)
	{
		const source_bone_t *source = &skeleton.bones[order[b]];
		actor_file_bone_t *bone = &bones[b];
		vector_t probe = { 0.3f, -0.2f, 0.5f };

		snprintf(bone->name, sizeof(bone->name), "%s", source->name);
		bone->parent = parents[b];
		bone->translation[0] = bind_local[b].translation.x;
		bone->translation[1] = bind_local[b].translation.y;
		bone->translation[2] = bind_local[b].translation.z;
		bone->rotation[0] = bind_local[b].rotation.x;
		bone->rotation[1] = bind_local[b].rotation.y;
		bone->rotation[2] = bind_local[b].rotation.z;
		bone->rotation[3] = bind_local[b].rotation.w;
		// The inverse bind comes from the bind pose itself; the file's own
		// bone space transform should say the same, which checks that the
		// skeleton was read right
		inverse_pose_rows(&bind_global[b], bone->inverse_bind);
		{
			vector_t from_file = quaternion_rotate(source->bone_space_rotation, probe);
			float computed[3];

			from_file.x += source->bone_space_translation.x;
			from_file.y += source->bone_space_translation.y;
			from_file.z += source->bone_space_translation.z;
			for (int row = 0; row < 3; row++)
			{
				computed[row] = bone->inverse_bind[row * 4 + 0] * probe.x + bone->inverse_bind[row * 4 + 1] * probe.y
					+ bone->inverse_bind[row * 4 + 2] * probe.z + bone->inverse_bind[row * 4 + 3];
			}
			bind_mismatch = fmaxf(bind_mismatch, fabsf(computed[0] - from_file.x));
			bind_mismatch = fmaxf(bind_mismatch, fabsf(computed[1] - from_file.y));
			bind_mismatch = fmaxf(bind_mismatch, fabsf(computed[2] - from_file.z));
		}
	}

	// Mesh
	if (!resolve_path(root, definition_directory, definition.mesh, path, sizeof(path)) || !load_mesh(path, skeleton.bone_count, &mesh))
	{
		return 1;
	}

	// Animations: one clip per file, however many actions use it
	animations = calloc(MAX_CLIPS, sizeof(source_animation_t));
	for (int a = 0; a < definition.action_count; a++)
	{
		source_action_t *source = &definition.actions[a];
		int clip = -1;

		for (int c = 0; c < animation_count; c++)
		{
			if (strcmp(animations[c].path, source->path) == 0)
			{
				clip = c;
				break;
			}
		}
		if (clip < 0)
		{
			source_animation_t *animation;

			if (animation_count == MAX_CLIPS)
			{
				fprintf(stderr, "More than %d animation files\n", MAX_CLIPS);
				return 1;
			}
			animation = &animations[animation_count];
			snprintf(animation->path, sizeof(animation->path), "%s", source->path);
			stem_of(source->path, animation->name, sizeof(animation->name));
			if (!resolve_path(root, definition_directory, source->path, path, sizeof(path))
				|| !load_animation(path, skeleton.bone_count, animation))
			{
				return 1;
			}
			clip = animation_count++;
		}
		memset(&actions[a], 0, sizeof(actions[a]));
		snprintf(actions[a].name, sizeof(actions[a].name), "%s", source->name);
		actions[a].clip = clip;
		actions[a].flags = source->loop ? ACTOR_ACTION_LOOP : 0u;
		actions[a].duration_seconds = source->duration_seconds;
	}

	// Texture: decoded here, so the game needs no DDS (phones lack S3TC)
	if (!resolve_path(root, definition_directory, definition.skin, path, sizeof(path)))
	{
		return 1;
	}
	{
		reader_t reader;

		if (!reader_open(&reader, path))
		{
			return 1;
		}
		if (!dds_decode(reader.data, reader.size, &pixels, &texture_width, &texture_height, &has_alpha))
		{
			fprintf(stderr, "Cannot decode the texture '%s'\n", path);
			reader_close(&reader);
			return 1;
		}
		reader_close(&reader);
	}
	// DXT1 may hide one bit alpha in its blocks; what counts is whether any
	// texel is cut out
	for (int i = 0; i < texture_width * texture_height; i++)
	{
		if (pixels[i * 4 + 3] < 128)
		{
			alpha_tested = true;
			break;
		}
	}
	{
		mz_ulong raw_size = (mz_ulong)texture_width * (mz_ulong)texture_height * 4u;

		compressed_size = mz_compressBound(raw_size);
		compressed = malloc(compressed_size);
		if (mz_compress2(compressed, &compressed_size, pixels, raw_size, MZ_BEST_COMPRESSION) != MZ_OK)
		{
			fprintf(stderr, "Cannot compress the texture\n");
			return 1;
		}
	}

	// Header
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, ACTOR_FILE_MAGIC, 4);
	header.version = ACTOR_FILE_VERSION;
	header.bone_count = (uint32_t)skeleton.bone_count;
	header.vertex_count = (uint32_t)mesh.vertex_count;
	header.index_count = (uint32_t)mesh.index_count;
	header.clip_count = (uint32_t)animation_count;
	header.action_count = (uint32_t)definition.action_count;
	header.texture_width = (uint32_t)texture_width;
	header.texture_height = (uint32_t)texture_height;
	header.texture_flags = alpha_tested ? ACTOR_TEXTURE_ALPHA_TESTED : 0u;
	header.texture_compressed_size = (uint32_t)compressed_size;
	for (int k = 0; k < 3; k++)
	{
		header.bounds_minimum[k] = 1e30f;
		header.bounds_maximum[k] = -1e30f;
	}
	for (int v = 0; v < mesh.vertex_count; v++)
	{
		float position[3] = { mesh.vertices[v].position.x, mesh.vertices[v].position.y, mesh.vertices[v].position.z };

		for (int k = 0; k < 3; k++)
		{
			header.bounds_minimum[k] = fminf(header.bounds_minimum[k], position[k]);
			header.bounds_maximum[k] = fmaxf(header.bounds_maximum[k], position[k]);
		}
	}

	output = fopen(output_path, "wb");
	if (output == NULL)
	{
		fprintf(stderr, "Cannot write '%s'\n", output_path);
		return 1;
	}
	write_or_fail(output, &header, sizeof(header), &ok);
	write_or_fail(output, bones, sizeof(actor_file_bone_t) * (size_t)skeleton.bone_count, &ok);
	for (int v = 0; v < mesh.vertex_count && ok; v++)
	{
		const source_vertex_t *source = &mesh.vertices[v];
		actor_file_vertex_t vertex;
		float weight_sum = 0.0f;
		int byte_sum = 0;
		int heaviest = 0;
		vector_t normal = source->normal;
		float normal_length = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);

		memset(&vertex, 0, sizeof(vertex));
		vertex.position[0] = source->position.x;
		vertex.position[1] = source->position.y;
		vertex.position[2] = source->position.z;
		if (normal_length > 1e-12f)
		{
			normal.x /= normal_length;
			normal.y /= normal_length;
			normal.z /= normal_length;
		}
		vertex.normal[0] = normal.x;
		vertex.normal[1] = normal.y;
		vertex.normal[2] = normal.z;
		vertex.uv[0] = source->uv[0];
		vertex.uv[1] = source->uv[1];
		for (int k = 0; k < ACTOR_FILE_INFLUENCES; k++)
		{
			weight_sum += source->weights[k];
		}
		if (weight_sum <= 1e-9f)
		{
			// No influence at all: the root carries it
			vertex.weights[0] = 255;
		}
		else
		{
			for (int k = 0; k < ACTOR_FILE_INFLUENCES; k++)
			{
				vertex.bones[k] = (uint8_t)new_index[source->bones[k]];
				vertex.weights[k] = (uint8_t)lroundf(source->weights[k] / weight_sum * 255.0f);
				byte_sum += vertex.weights[k];
				if (vertex.weights[k] > vertex.weights[heaviest])
				{
					heaviest = k;
				}
			}
			// The bytes must sum to exactly 255
			vertex.weights[heaviest] = (uint8_t)(vertex.weights[heaviest] + (255 - byte_sum));
		}
		write_or_fail(output, &vertex, sizeof(vertex), &ok);
	}
	write_or_fail(output, mesh.indices, sizeof(uint32_t) * (size_t)mesh.index_count, &ok);
	for (int c = 0; c < animation_count && ok; c++)
	{
		const source_animation_t *animation = &animations[c];
		actor_file_clip_t clip;
		const source_track_t *track_of_bone[ACTOR_FILE_MAX_BONES] = { NULL };

		memset(&clip, 0, sizeof(clip));
		snprintf(clip.name, sizeof(clip.name), "%s", animation->name);
		clip.duration_seconds = animation->duration_seconds;
		clip.frame_count = (uint32_t)ceilf(animation->duration_seconds * SAMPLES_PER_SECOND) + 1u;
		if (clip.frame_count < 2)
		{
			clip.frame_count = 2;
		}
		for (int t = 0; t < animation->track_count; t++)
		{
			track_of_bone[new_index[animation->tracks[t].bone]] = &animation->tracks[t];
		}
		write_or_fail(output, &clip, sizeof(clip), &ok);
		for (uint32_t frame = 0; frame < clip.frame_count && ok; frame++)
		{
			float time_seconds = animation->duration_seconds * (float)frame / (float)(clip.frame_count - 1);

			for (int b = 0; b < skeleton.bone_count; b++)
			{
				actor_file_key_t key;
				vector_t translation = bind_local[b].translation;
				quaternion_t rotation = bind_local[b].rotation;

				if (track_of_bone[b] != NULL)
				{
					sample_track(track_of_bone[b], time_seconds, &translation, &rotation);
				}
				key.translation[0] = translation.x;
				key.translation[1] = translation.y;
				key.translation[2] = translation.z;
				key.rotation[0] = rotation.x;
				key.rotation[1] = rotation.y;
				key.rotation[2] = rotation.z;
				key.rotation[3] = rotation.w;
				write_or_fail(output, &key, sizeof(key), &ok);
			}
		}
	}
	write_or_fail(output, actions, sizeof(actor_file_action_t) * (size_t)definition.action_count, &ok);
	write_or_fail(output, compressed, (size_t)compressed_size, &ok);
	if (fclose(output) != 0)
	{
		ok = false;
	}
	if (!ok)
	{
		fprintf(stderr, "Writing '%s' failed\n", output_path);
		return 1;
	}

	printf("%s: %d bones, %d vertices, %d triangles, %d clips, %d actions, texture %dx%d%s\n", output_path, skeleton.bone_count,
		mesh.vertex_count, mesh.index_count / 3, animation_count, definition.action_count, texture_width, texture_height,
		alpha_tested ? " (cut out)" : "");
	printf("  size %.2f x %.2f x %.2f m (x, y up, z)\n", header.bounds_maximum[0] - header.bounds_minimum[0],
		header.bounds_maximum[1] - header.bounds_minimum[1], header.bounds_maximum[2] - header.bounds_minimum[2]);
	printf("  bind pose check: the computed inverse bind differs from the file's by at most %.6f m\n", bind_mismatch);
	if (mesh.dropped_influences > 0)
	{
		printf("  %d influences beyond four per vertex dropped (the heaviest weighed %.3f)\n", mesh.dropped_influences,
			mesh.dropped_weight_maximum);
	}
	SDL_free(pixels);
	free(compressed);
	return 0;
}

// ---------------------------------------------------------------------------
// --dump

static int dump_actor(const char *path)
{
	reader_t reader;
	actor_file_header_t header;
	size_t keys_bytes = 0;

	if (!reader_open(&reader, path))
	{
		return 1;
	}
	reader_take(&reader, &header, sizeof(header));
	if (reader.failed || memcmp(header.magic, ACTOR_FILE_MAGIC, 4) != 0 || header.version < ACTOR_FILE_OLDEST_VERSION
		|| header.version > ACTOR_FILE_VERSION)
	{
		fprintf(stderr, "'%s' is not a version %u to %u .pact file\n", path, ACTOR_FILE_OLDEST_VERSION, ACTOR_FILE_VERSION);
		reader_close(&reader);
		return 1;
	}
	if (header.version < 2)
	{
		header.normal_compressed_size = 0;
		header.orm_compressed_size = 0;
		header.emissive_compressed_size = 0;
		header.extra_flags = 0;
	}
	printf("%s: %zu bytes, version %u\n", path, reader.size, header.version);
	printf("  %u bones, %u vertices, %u triangles, %u clips, %u actions\n", header.bone_count, header.vertex_count,
		header.index_count / 3, header.clip_count, header.action_count);
	printf("  bounds (%.3f %.3f %.3f) to (%.3f %.3f %.3f)\n", header.bounds_minimum[0], header.bounds_minimum[1],
		header.bounds_minimum[2], header.bounds_maximum[0], header.bounds_maximum[1], header.bounds_maximum[2]);
	printf("  texture %ux%u%s, %u bytes compressed\n", header.texture_width, header.texture_height,
		(header.texture_flags & ACTOR_TEXTURE_ALPHA_TESTED) != 0 ? " cut out" : "", header.texture_compressed_size);
	printf("  bones:");
	for (uint32_t b = 0; b < header.bone_count; b++)
	{
		actor_file_bone_t bone;

		reader_take(&reader, &bone, sizeof(bone));
		printf("%s %s<%d", b % 6 == 0 ? "\n   " : ",", bone.name, bone.parent);
	}
	printf("\n");
	reader.offset += sizeof(actor_file_vertex_t) * header.vertex_count + sizeof(uint32_t) * header.index_count;
	printf("  clips:\n");
	for (uint32_t c = 0; c < header.clip_count && !reader.failed; c++)
	{
		actor_file_clip_t clip;

		reader_take(&reader, &clip, sizeof(clip));
		printf("    %2u %-24s %.3f s, %u frames\n", c, clip.name, clip.duration_seconds, clip.frame_count);
		keys_bytes = sizeof(actor_file_key_t) * (size_t)clip.frame_count * header.bone_count;
		reader.offset += keys_bytes;
	}
	printf("  actions:\n");
	for (uint32_t a = 0; a < header.action_count && !reader.failed; a++)
	{
		actor_file_action_t action;

		reader_take(&reader, &action, sizeof(action));
		printf("    %-20s clip %2d %s", action.name, action.clip, (action.flags & ACTOR_ACTION_LOOP) != 0 ? "loop" : "once");
		if (action.duration_seconds > 0.0f)
		{
			printf(", %.3f s", action.duration_seconds);
		}
		printf("\n");
	}
	printf("  shading maps:%s%s%s%s\n", header.normal_compressed_size > 0 ? " normal" : "", header.orm_compressed_size > 0
		? " occlusion-roughness-metal" : "", header.emissive_compressed_size > 0 ? " emissive" : "",
		(header.extra_flags & ACTOR_FILE_HAS_TANGENTS) != 0 ? ", tangents" : (header.normal_compressed_size + header.orm_compressed_size
		+ header.emissive_compressed_size == 0 ? " none" : ""));
	{
		size_t expected = reader.offset + header.texture_compressed_size + header.normal_compressed_size + header.orm_compressed_size
			+ header.emissive_compressed_size + ((header.extra_flags & ACTOR_FILE_HAS_TANGENTS) != 0 ? sizeof(float) * 4 * header.vertex_count : 0);

		if (!reader.failed && expected != reader.size)
		{
			printf("  warning: the file is %zu bytes, its parts add up to %zu\n", reader.size, expected);
		}
	}
	reader_close(&reader);
	return reader.failed ? 1 : 0;
}

int main(int argument_count, char **arguments)
{
	const char *root = NULL;
	int first = 1;

	if (argument_count == 3 && strcmp(arguments[1], "--dump") == 0)
	{
		return dump_actor(arguments[2]);
	}
	// glTF: its own options (see convert_gltf.h)
	for (int i = 1; i < argument_count; i++)
	{
		if (is_gltf_path(arguments[i]))
		{
			return convert_gltf(argument_count - 1, arguments + 1);
		}
	}
	if (argument_count >= 3 && strcmp(arguments[1], "--root") == 0)
	{
		root = arguments[2];
		first = 3;
	}
	if (argument_count - first != 2)
	{
		fprintf(stderr, "Usage: potop_convert [--root DIRECTORY] actor.xml output.pact\n"
			"       potop_convert [options] model.gltf|model.glb output.pact|output.pobj|output.e3d   (a model alone: its options)\n"
			"       potop_convert --dump file.pact\n");
		return 2;
	}
	return convert_actor(arguments[first], root, arguments[first + 1]);
}
