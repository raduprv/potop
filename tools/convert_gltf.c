// glTF 2.0 into .pact actors and .e3d objects. See convert_gltf.h.
//
// How a skinned model becomes an actor. glTF skins a vertex v (in its
// mesh's space) as  sum of weight * world(joint) * inverse_bind(joint) * v,
// where a joint's world matrix may hold a scale (Blender and Mixamo
// exports often put one on the armature). The game's bones are rigid (a
// rotation and a translation), so each joint's world matrix is split into
// its rigid part and the rest, X = rigid^-1 * world (taken at rest: a scale
// that does not change with the animation), and the rest goes into the
// game's inverse bind, which is a full 3x4 matrix. The vertices are moved
// into the game's model space by M, the skinning matrix of the first root
// joint at rest (for a model whose rest pose is its bind pose, as most are,
// that is where the mesh stands), so the game's inverse bind of a joint is
// X * inverse_bind * M^-1, and skinning in the game gives what glTF would.

#include "convert_gltf.h"
#include "actor_format.h"
#include "object_format.h"
#include "miniz.h"

#include "cgltf.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES_PER_SECOND 30.0f
#define MAX_ACTIONS 128
#define MAX_SURFACES 64
#define E3D_MAX_MATERIALS 16           // OBJECT_MODEL_MAX_MATERIALS
#define PATH_LENGTH 1024
#define SOLID_TEXTURE_SIZE 8           // a material without a texture: a small square of its color
#define ATLAS_PADDING 4                // texels around each packed texture (its edges repeated)

// ---------------------------------------------------------------------------
// Small math. Matrices are 4x4, column major as in glTF: element (row r,
// column c) is m[c * 4 + r]. Quaternions (x, y, z, w) turn v into q v q*.

static void matrix_identity(float out[16])
{
	memset(out, 0, sizeof(float) * 16);
	out[0] = out[5] = out[10] = out[15] = 1.0f;
}

static void matrix_multiply(float out[16], const float a[16], const float b[16])
{
	float result[16];

	for (int c = 0; c < 4; c++)
	{
		for (int r = 0; r < 4; r++)
		{
			result[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] + a[2 * 4 + r] * b[c * 4 + 2]
				+ a[3 * 4 + r] * b[c * 4 + 3];
		}
	}
	memcpy(out, result, sizeof(result));
}

static void matrix_from_trs(float out[16], const float t[3], const float q[4], const float s[3])
{
	float x = q[0];
	float y = q[1];
	float z = q[2];
	float w = q[3];

	out[0] = (1.0f - 2.0f * (y * y + z * z)) * s[0];
	out[1] = (2.0f * (x * y + z * w)) * s[0];
	out[2] = (2.0f * (x * z - y * w)) * s[0];
	out[3] = 0.0f;
	out[4] = (2.0f * (x * y - z * w)) * s[1];
	out[5] = (1.0f - 2.0f * (x * x + z * z)) * s[1];
	out[6] = (2.0f * (y * z + x * w)) * s[1];
	out[7] = 0.0f;
	out[8] = (2.0f * (x * z + y * w)) * s[2];
	out[9] = (2.0f * (y * z - x * w)) * s[2];
	out[10] = (1.0f - 2.0f * (x * x + y * y)) * s[2];
	out[11] = 0.0f;
	out[12] = t[0];
	out[13] = t[1];
	out[14] = t[2];
	out[15] = 1.0f;
}

// The inverse of an affine matrix (false if it has none)
static bool matrix_inverse(float out[16], const float m[16])
{
	float a = m[0], b = m[4], c = m[8];
	float d = m[1], e = m[5], f = m[9];
	float g = m[2], h = m[6], i = m[10];
	float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	float inverse[16];

	if (fabsf(determinant) < 1e-20f)
	{
		matrix_identity(out);
		return false;
	}
	determinant = 1.0f / determinant;
	inverse[0] = (e * i - f * h) * determinant;
	inverse[4] = (c * h - b * i) * determinant;
	inverse[8] = (b * f - c * e) * determinant;
	inverse[1] = (f * g - d * i) * determinant;
	inverse[5] = (a * i - c * g) * determinant;
	inverse[9] = (c * d - a * f) * determinant;
	inverse[2] = (d * h - e * g) * determinant;
	inverse[6] = (b * g - a * h) * determinant;
	inverse[10] = (a * e - b * d) * determinant;
	inverse[3] = inverse[7] = inverse[11] = 0.0f;
	inverse[15] = 1.0f;
	for (int r = 0; r < 3; r++)
	{
		inverse[12 + r] = -(inverse[0 * 4 + r] * m[12] + inverse[1 * 4 + r] * m[13] + inverse[2 * 4 + r] * m[14]);
	}
	memcpy(out, inverse, sizeof(inverse));
	return true;
}

static float matrix_determinant3(const float m[16])
{
	return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

static void matrix_point(const float m[16], const float p[3], float out[3])
{
	float result[3];

	for (int r = 0; r < 3; r++)
	{
		result[r] = m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2] + m[12 + r];
	}
	memcpy(out, result, sizeof(result));
}

static void normalize3(float v[3])
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 1e-20f)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

// How a normal turns with a matrix: by its inverse transpose
static void matrix_normal(const float m[16], const float n[3], float out[3])
{
	float inverse[16];
	float result[3];

	matrix_inverse(inverse, m);
	for (int r = 0; r < 3; r++)
	{
		// Row r of the transpose is column r of the inverse
		result[r] = inverse[r * 4 + 0] * n[0] + inverse[r * 4 + 1] * n[1] + inverse[r * 4 + 2] * n[2];
	}
	normalize3(result);
	memcpy(out, result, sizeof(result));
}

static void quaternion_normalize(float q[4])
{
	float length = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);

	if (length < 1e-20f)
	{
		q[0] = q[1] = q[2] = 0.0f;
		q[3] = 1.0f;
		return;
	}
	for (int k = 0; k < 4; k++)
	{
		q[k] /= length;
	}
}

static void quaternion_slerp(const float a[4], const float b[4], float amount, float out[4])
{
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	float sign = dot < 0.0f ? -1.0f : 1.0f;
	float weight_a;
	float weight_b;

	dot *= sign;
	if (dot > 0.9995f)
	{
		weight_a = 1.0f - amount;
		weight_b = amount * sign;
	}
	else
	{
		float angle = acosf(dot);
		float inverse_sine = 1.0f / sinf(angle);

		weight_a = sinf((1.0f - amount) * angle) * inverse_sine;
		weight_b = sinf(amount * angle) * inverse_sine * sign;
	}
	for (int k = 0; k < 4; k++)
	{
		out[k] = a[k] * weight_a + b[k] * weight_b;
	}
	quaternion_normalize(out);
}

// The rotation of a matrix whose columns may be scaled (they are
// straightened out first), and its translation
static void matrix_rigid_part(const float m[16], float translation_out[3], float rotation_out[4])
{
	float x[3] = { m[0], m[1], m[2] };
	float y[3] = { m[4], m[5], m[6] };
	float z[3];
	float dot;
	float trace;
	float r[3][3];

	normalize3(x);
	dot = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
	for (int k = 0; k < 3; k++)
	{
		y[k] -= x[k] * dot;
	}
	normalize3(y);
	z[0] = x[1] * y[2] - x[2] * y[1];
	z[1] = x[2] * y[0] - x[0] * y[2];
	z[2] = x[0] * y[1] - x[1] * y[0];
	// r[row][column]
	for (int k = 0; k < 3; k++)
	{
		r[k][0] = x[k];
		r[k][1] = y[k];
		r[k][2] = z[k];
	}
	trace = r[0][0] + r[1][1] + r[2][2];
	if (trace > 0.0f)
	{
		float s = sqrtf(trace + 1.0f) * 2.0f;

		rotation_out[3] = 0.25f * s;
		rotation_out[0] = (r[2][1] - r[1][2]) / s;
		rotation_out[1] = (r[0][2] - r[2][0]) / s;
		rotation_out[2] = (r[1][0] - r[0][1]) / s;
	}
	else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
	{
		float s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;

		rotation_out[3] = (r[2][1] - r[1][2]) / s;
		rotation_out[0] = 0.25f * s;
		rotation_out[1] = (r[0][1] + r[1][0]) / s;
		rotation_out[2] = (r[0][2] + r[2][0]) / s;
	}
	else if (r[1][1] > r[2][2])
	{
		float s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;

		rotation_out[3] = (r[0][2] - r[2][0]) / s;
		rotation_out[0] = (r[0][1] + r[1][0]) / s;
		rotation_out[1] = 0.25f * s;
		rotation_out[2] = (r[1][2] + r[2][1]) / s;
	}
	else
	{
		float s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;

		rotation_out[3] = (r[1][0] - r[0][1]) / s;
		rotation_out[0] = (r[0][2] + r[2][0]) / s;
		rotation_out[1] = (r[1][2] + r[2][1]) / s;
		rotation_out[2] = 0.25f * s;
	}
	quaternion_normalize(rotation_out);
	translation_out[0] = m[12];
	translation_out[1] = m[13];
	translation_out[2] = m[14];
}

static void rigid_matrix(const float m[16], float out[16])
{
	float translation[3];
	float rotation[4];
	const float one[3] = { 1.0f, 1.0f, 1.0f };

	matrix_rigid_part(m, translation, rotation);
	matrix_from_trs(out, translation, rotation, one);
}

// ---------------------------------------------------------------------------
// The node tree and its poses

typedef struct
{
	float translation[3];
	float rotation[4];
	float scale[3];
} node_pose_t;

typedef struct
{
	cgltf_data *data;
	char directory[PATH_LENGTH];       // of the .gltf: where its files are
	size_t node_count;
	int *order;                        // the nodes, parents first
	node_pose_t *rest;
	float root[16];                    // over everything: the up axis, the turn, the scale, the ground
} model_t;

static int node_index(const model_t *model, const cgltf_node *node)
{
	return (int)(node - model->data->nodes);
}

static void rest_pose(const cgltf_node *node, node_pose_t *pose)
{
	float matrix[16];

	if (node->has_matrix)
	{
		float column_x = sqrtf(node->matrix[0] * node->matrix[0] + node->matrix[1] * node->matrix[1] + node->matrix[2] * node->matrix[2]);
		float column_y = sqrtf(node->matrix[4] * node->matrix[4] + node->matrix[5] * node->matrix[5] + node->matrix[6] * node->matrix[6]);
		float column_z = sqrtf(node->matrix[8] * node->matrix[8] + node->matrix[9] * node->matrix[9] + node->matrix[10] * node->matrix[10]);

		memcpy(matrix, node->matrix, sizeof(matrix));
		matrix_rigid_part(matrix, pose->translation, pose->rotation);
		pose->scale[0] = column_x;
		pose->scale[1] = column_y;
		pose->scale[2] = matrix_determinant3(matrix) < 0.0f ? -column_z : column_z;
		return;
	}
	for (int k = 0; k < 3; k++)
	{
		pose->translation[k] = node->has_translation ? node->translation[k] : 0.0f;
		pose->scale[k] = node->has_scale ? node->scale[k] : 1.0f;
	}
	for (int k = 0; k < 4; k++)
	{
		pose->rotation[k] = node->has_rotation ? node->rotation[k] : (k == 3 ? 1.0f : 0.0f);
	}
	quaternion_normalize(pose->rotation);
}

static bool model_open(model_t *model, const char *path)
{
	cgltf_options options;
	cgltf_result result;
	size_t placed = 0;

	memset(model, 0, sizeof(*model));
	memset(&options, 0, sizeof(options));
	result = cgltf_parse_file(&options, path, &model->data);
	if (result != cgltf_result_success)
	{
		static const char *const reasons[] = { "", "the file is too short", "not glTF", "broken JSON", "not valid glTF",
			"bad options", "no such file", "cannot read the file", "out of memory", "glTF 1.0, too old" };

		fprintf(stderr, "Cannot read '%s' as glTF: %s\n", path,
			(int)result >= 0 && (size_t)result < sizeof(reasons) / sizeof(reasons[0]) ? reasons[result] : "unknown error");
		return false;
	}
	result = cgltf_load_buffers(&options, model->data, path);
	if (result != cgltf_result_success)
	{
		fprintf(stderr, "Cannot load the buffers of '%s' (error %d: a .bin file missing?)\n", path, (int)result);
		return false;
	}
	if (cgltf_validate(model->data) != cgltf_result_success)
	{
		fprintf(stderr, "'%s' does not hold together (cgltf_validate)\n", path);
		return false;
	}
	snprintf(model->directory, sizeof(model->directory), "%s", path);
	{
		char *slash = strrchr(model->directory, '/');

		if (slash != NULL)
		{
			slash[1] = '\0';
		}
		else
		{
			model->directory[0] = '\0';
		}
	}
	model->node_count = model->data->nodes_count;
	model->order = calloc(model->node_count + 1, sizeof(int));
	model->rest = calloc(model->node_count + 1, sizeof(node_pose_t));
	for (size_t n = 0; n < model->node_count; n++)
	{
		rest_pose(&model->data->nodes[n], &model->rest[n]);
		if (model->data->nodes[n].parent == NULL)
		{
			model->order[placed++] = (int)n;
		}
	}
	// Breadth first from the roots: parents always before their children
	for (size_t at = 0; at < placed; at++)
	{
		const cgltf_node *node = &model->data->nodes[model->order[at]];

		for (size_t c = 0; c < node->children_count && placed < model->node_count; c++)
		{
			model->order[placed++] = node_index(model, node->children[c]);
		}
	}
	model->node_count = placed;
	matrix_identity(model->root);
	return true;
}

static void model_close(model_t *model)
{
	if (model->data != NULL)
	{
		cgltf_free(model->data);
	}
	free(model->order);
	free(model->rest);
	memset(model, 0, sizeof(*model));
}

// Every node's world matrix (the root's transform included) in a pose
static void compute_worlds(const model_t *model, const node_pose_t *poses, float (*worlds)[16])
{
	for (size_t at = 0; at < model->node_count; at++)
	{
		int n = model->order[at];
		const cgltf_node *node = &model->data->nodes[n];
		float local[16];

		matrix_from_trs(local, poses[n].translation, poses[n].rotation, poses[n].scale);
		if (node->parent != NULL)
		{
			matrix_multiply(worlds[n], worlds[node_index(model, node->parent)], local);
		}
		else
		{
			matrix_multiply(worlds[n], model->root, local);
		}
	}
}

// ---------------------------------------------------------------------------
// Animation sampling

static float accessor_float(const cgltf_accessor *accessor, cgltf_size index)
{
	float value = 0.0f;

	cgltf_accessor_read_float(accessor, index, &value, 1);
	return value;
}

static float sampler_end(const cgltf_animation_sampler *sampler)
{
	return sampler->input->count > 0 ? accessor_float(sampler->input, sampler->input->count - 1) : 0.0f;
}

// A channel's value at a time (components: 3, or 4 for rotations)
static void sample_channel(const cgltf_animation_channel *channel, float time_seconds, float *out, int components)
{
	const cgltf_animation_sampler *sampler = channel->sampler;
	cgltf_size count = sampler->input->count;
	bool cubic = sampler->interpolation == cgltf_interpolation_type_cubic_spline;
	cgltf_size stride = cubic ? 3 : 1;
	cgltf_size low = 0;
	float before;
	float after;
	float a[4];
	float b[4];

	if (count == 0)
	{
		return;
	}
	// The value of key k is element k * stride (+1 for the spline's middle)
	if (count == 1 || time_seconds <= accessor_float(sampler->input, 0))
	{
		cgltf_accessor_read_float(sampler->output, cubic ? 1 : 0, out, (cgltf_size)components);
		if (components == 4)
		{
			quaternion_normalize(out);
		}
		return;
	}
	if (time_seconds >= accessor_float(sampler->input, count - 1))
	{
		cgltf_accessor_read_float(sampler->output, (count - 1) * stride + (cubic ? 1 : 0), out, (cgltf_size)components);
		if (components == 4)
		{
			quaternion_normalize(out);
		}
		return;
	}
	{
		cgltf_size high = count - 1;

		while (high - low > 1)
		{
			cgltf_size middle = (low + high) / 2;

			if (accessor_float(sampler->input, middle) <= time_seconds)
			{
				low = middle;
			}
			else
			{
				high = middle;
			}
		}
	}
	before = accessor_float(sampler->input, low);
	after = accessor_float(sampler->input, low + 1);
	{
		float span = after - before;
		float amount = span > 1e-9f ? (time_seconds - before) / span : 0.0f;

		if (sampler->interpolation == cgltf_interpolation_type_step)
		{
			cgltf_accessor_read_float(sampler->output, low, out, (cgltf_size)components);
		}
		else if (cubic)
		{
			// Hermite, from the value and out tangent of the key before and
			// the value and in tangent of the one after (tangents per second)
			float value_a[4];
			float out_tangent[4];
			float value_b[4];
			float in_tangent[4];
			float t = amount;
			float t2 = t * t;
			float t3 = t2 * t;

			cgltf_accessor_read_float(sampler->output, low * 3 + 1, value_a, (cgltf_size)components);
			cgltf_accessor_read_float(sampler->output, low * 3 + 2, out_tangent, (cgltf_size)components);
			cgltf_accessor_read_float(sampler->output, (low + 1) * 3 + 0, in_tangent, (cgltf_size)components);
			cgltf_accessor_read_float(sampler->output, (low + 1) * 3 + 1, value_b, (cgltf_size)components);
			for (int k = 0; k < components; k++)
			{
				out[k] = (2.0f * t3 - 3.0f * t2 + 1.0f) * value_a[k] + (t3 - 2.0f * t2 + t) * span * out_tangent[k]
					+ (-2.0f * t3 + 3.0f * t2) * value_b[k] + (t3 - t2) * span * in_tangent[k];
			}
		}
		else
		{
			cgltf_accessor_read_float(sampler->output, low, a, (cgltf_size)components);
			cgltf_accessor_read_float(sampler->output, low + 1, b, (cgltf_size)components);
			if (components == 4)
			{
				quaternion_normalize(a);
				quaternion_normalize(b);
				quaternion_slerp(a, b, amount, out);
			}
			else
			{
				for (int k = 0; k < components; k++)
				{
					out[k] = a[k] + (b[k] - a[k]) * amount;
				}
			}
		}
	}
	if (components == 4)
	{
		quaternion_normalize(out);
	}
}

// The rest pose with an animation applied at a time
static void animated_poses(const model_t *model, const cgltf_animation *animation, float time_seconds, node_pose_t *poses)
{
	memcpy(poses, model->rest, sizeof(node_pose_t) * model->data->nodes_count);
	for (cgltf_size c = 0; c < animation->channels_count; c++)
	{
		const cgltf_animation_channel *channel = &animation->channels[c];
		node_pose_t *pose;

		if (channel->target_node == NULL || channel->sampler == NULL)
		{
			continue;
		}
		pose = &poses[node_index(model, channel->target_node)];
		switch (channel->target_path)
		{
			case cgltf_animation_path_type_translation:
				sample_channel(channel, time_seconds, pose->translation, 3);
				break;
			case cgltf_animation_path_type_rotation:
				sample_channel(channel, time_seconds, pose->rotation, 4);
				break;
			case cgltf_animation_path_type_scale:
				sample_channel(channel, time_seconds, pose->scale, 3);
				break;
			default:
				break;
		}
	}
}

static float animation_duration(const cgltf_animation *animation)
{
	float duration = 0.0f;

	for (cgltf_size s = 0; s < animation->samplers_count; s++)
	{
		duration = fmaxf(duration, sampler_end(&animation->samplers[s]));
	}
	return duration;
}

// ---------------------------------------------------------------------------
// Textures: one surface per distinct base color (texture times factor)

typedef struct
{
	const cgltf_material *material;    // NULL: glTF's default material
	cgltf_alpha_mode alpha_mode;
	float alpha_cutoff;
	bool double_sided;
	// The base color (RGBA8, top row first, malloc), and what physically
	// based shading takes, each the same size (see actor_format.h)
	uint8_t *pixels;
	uint8_t *normal_pixels;            // NULL: flat
	uint8_t *orm_pixels;
	uint8_t *emissive_pixels;          // NULL: none
	int width;
	int height;
	bool cut_out;                      // has transparent texels the game cuts out
	// Where it went in the actor's atlas
	int atlas_x;
	int atlas_y;
} surface_t;

typedef struct
{
	surface_t surfaces[MAX_SURFACES];
	int count;
	int max_size;
	bool image_failed;
} surfaces_t;

static uint8_t *read_whole_file(const char *path, size_t *size_out)
{
	FILE *file = fopen(path, "rb");
	uint8_t *data;
	long size;

	if (file == NULL)
	{
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc(size > 0 ? (size_t)size : 1u);
	if (data == NULL || (size > 0 && fread(data, 1, (size_t)size, file) != (size_t)size))
	{
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*size_out = (size_t)size;
	return data;
}

// An image's pixels (RGBA8, malloc), from its buffer view, a data URI or a
// file next to the model; NULL (after saying why) if it cannot be had
static uint8_t *decode_image(const model_t *model, const cgltf_image *image, int *width_out, int *height_out)
{
	const uint8_t *bytes = NULL;
	size_t size = 0;
	uint8_t *owned = NULL;
	stbi_uc *decoded;
	uint8_t *pixels;
	int channels = 0;

	if (image->buffer_view != NULL)
	{
		bytes = cgltf_buffer_view_data(image->buffer_view);
		size = image->buffer_view->size;
	}
	else if (image->uri != NULL && strncmp(image->uri, "data:", 5) == 0)
	{
		const char *comma = strchr(image->uri, ',');
		cgltf_options options;
		size_t length;
		void *data = NULL;

		if (comma == NULL || strstr(image->uri, ";base64") == NULL)
		{
			fprintf(stderr, "An image's data URI is not base64\n");
			return NULL;
		}
		length = strlen(comma + 1);
		size = length / 4 * 3 - (length >= 1 && comma[length] == '=' ? 1 : 0) - (length >= 2 && comma[length - 1] == '=' ? 1 : 0);
		memset(&options, 0, sizeof(options));
		if (cgltf_load_buffer_base64(&options, size, comma + 1, &data) != cgltf_result_success)
		{
			fprintf(stderr, "Cannot decode an image's data URI\n");
			return NULL;
		}
		owned = data;
		bytes = owned;
	}
	else if (image->uri != NULL)
	{
		char name[PATH_LENGTH];
		char path[PATH_LENGTH * 2];

		snprintf(name, sizeof(name), "%s", image->uri);
		cgltf_decode_uri(name);
		snprintf(path, sizeof(path), "%s%s", model->directory, name);
		owned = read_whole_file(path, &size);
		if (owned == NULL)
		{
			fprintf(stderr, "Cannot read the image '%s'\n", path);
			return NULL;
		}
		bytes = owned;
	}
	if (bytes == NULL)
	{
		fprintf(stderr, "An image has neither data nor a file\n");
		return NULL;
	}
	decoded = stbi_load_from_memory(bytes, (int)size, width_out, height_out, &channels, 4);
	free(owned);
	if (decoded == NULL)
	{
		fprintf(stderr, "Cannot decode the image '%s' (%s; PNG and JPEG are read)\n", image->uri != NULL ? image->uri : image->name,
			stbi_failure_reason());
		return NULL;
	}
	pixels = malloc((size_t)*width_out * (size_t)*height_out * 4u);
	memcpy(pixels, decoded, (size_t)*width_out * (size_t)*height_out * 4u);
	stbi_image_free(decoded);
	return pixels;
}

// Halves an image (box filter) until both sides fit
static void shrink_to(uint8_t **pixels, int *width, int *height, int max_size)
{
	while (*width > max_size || *height > max_size)
	{
		int new_width = *width > 1 ? *width / 2 : 1;
		int new_height = *height > 1 ? *height / 2 : 1;
		uint8_t *smaller = malloc((size_t)new_width * (size_t)new_height * 4u);

		for (int y = 0; y < new_height; y++)
		{
			for (int x = 0; x < new_width; x++)
			{
				for (int k = 0; k < 4; k++)
				{
					int sum = 0;

					for (int dy = 0; dy < 2; dy++)
					{
						for (int dx = 0; dx < 2; dx++)
						{
							int source_x = x * 2 + dx < *width ? x * 2 + dx : *width - 1;
							int source_y = y * 2 + dy < *height ? y * 2 + dy : *height - 1;

							sum += (*pixels)[((size_t)source_y * (size_t)*width + (size_t)source_x) * 4u + (size_t)k];
						}
					}
					smaller[((size_t)y * (size_t)new_width + (size_t)x) * 4u + (size_t)k] = (uint8_t)((sum + 2) / 4);
				}
			}
		}
		free(*pixels);
		*pixels = smaller;
		*width = new_width;
		*height = new_height;
	}
}

// An image resized to a size (bilinear), malloc
static uint8_t *resized(const uint8_t *pixels, int width, int height, int new_width, int new_height)
{
	uint8_t *result = malloc((size_t)new_width * (size_t)new_height * 4u);

	for (int y = 0; y < new_height; y++)
	{
		float source_y = ((float)y + 0.5f) * (float)height / (float)new_height - 0.5f;
		int y0 = (int)floorf(source_y);
		float fy = source_y - (float)y0;
		int row0 = y0 < 0 ? 0 : (y0 >= height ? height - 1 : y0);
		int row1 = y0 + 1 < 0 ? 0 : (y0 + 1 >= height ? height - 1 : y0 + 1);

		for (int x = 0; x < new_width; x++)
		{
			float source_x = ((float)x + 0.5f) * (float)width / (float)new_width - 0.5f;
			int x0 = (int)floorf(source_x);
			float fx = source_x - (float)x0;
			int column0 = x0 < 0 ? 0 : (x0 >= width ? width - 1 : x0);
			int column1 = x0 + 1 < 0 ? 0 : (x0 + 1 >= width ? width - 1 : x0 + 1);

			for (int k = 0; k < 4; k++)
			{
				float a = pixels[((size_t)row0 * (size_t)width + (size_t)column0) * 4u + (size_t)k];
				float b = pixels[((size_t)row0 * (size_t)width + (size_t)column1) * 4u + (size_t)k];
				float c = pixels[((size_t)row1 * (size_t)width + (size_t)column0) * 4u + (size_t)k];
				float d = pixels[((size_t)row1 * (size_t)width + (size_t)column1) * 4u + (size_t)k];
				float value = (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;

				result[((size_t)y * (size_t)new_width + (size_t)x) * 4u + (size_t)k] = (uint8_t)(value + 0.5f);
			}
		}
	}
	return result;
}

// A texture of a material at a size (resized if it is another), malloc;
// NULL if there is none or it cannot be read
static uint8_t *texture_at(surfaces_t *surfaces, const model_t *model, const cgltf_texture *texture, int width, int height,
	int *width_out, int *height_out)
{
	uint8_t *pixels;
	int texture_width = 0;
	int texture_height = 0;

	if (texture == NULL || texture->image == NULL)
	{
		return NULL;
	}
	pixels = decode_image(model, texture->image, &texture_width, &texture_height);
	if (pixels == NULL)
	{
		surfaces->image_failed = true;
		return NULL;
	}
	if (width <= 0)
	{
		shrink_to(&pixels, &texture_width, &texture_height, surfaces->max_size);
		*width_out = texture_width;
		*height_out = texture_height;
		return pixels;
	}
	if (texture_width != width || texture_height != height)
	{
		uint8_t *sized = resized(pixels, texture_width, texture_height, width, height);

		free(pixels);
		pixels = sized;
	}
	return pixels;
}

static uint8_t clamp_byte(float value)
{
	return (uint8_t)(value < 0.0f ? 0.0f : (value > 255.0f ? 255.0f : value + 0.5f));
}

// The surface of a material (made on first use); -1 if there are too many.
// Every map comes out the base color's size, the factors already in
static int surface_of(surfaces_t *surfaces, const model_t *model, const cgltf_material *material)
{
	float factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	float metallic = 1.0f;
	float roughness = 1.0f;
	float emissive[3] = { 0.0f, 0.0f, 0.0f };
	float normal_scale = 1.0f;
	float occlusion_strength = 1.0f;
	const cgltf_texture *base_texture = NULL;
	const cgltf_texture *metal_rough_texture = NULL;
	const cgltf_texture *normal_texture = NULL;
	const cgltf_texture *occlusion_texture = NULL;
	const cgltf_texture *emissive_texture = NULL;
	uint8_t *normal_map;
	uint8_t *metal_rough;
	uint8_t *occlusion;
	uint8_t *emissive_map;
	surface_t *surface;
	size_t texel_count;

	for (int s = 0; s < surfaces->count; s++)
	{
		if (surfaces->surfaces[s].material == material)
		{
			return s;
		}
	}
	if (surfaces->count == MAX_SURFACES)
	{
		return -1;
	}
	surface = &surfaces->surfaces[surfaces->count];
	memset(surface, 0, sizeof(*surface));
	surface->material = material;
	surface->alpha_mode = cgltf_alpha_mode_opaque;
	surface->alpha_cutoff = 0.5f;
	if (material != NULL)
	{
		surface->alpha_mode = material->alpha_mode;
		surface->alpha_cutoff = material->alpha_cutoff;
		surface->double_sided = material->double_sided;
		if (material->has_pbr_metallic_roughness)
		{
			memcpy(factor, material->pbr_metallic_roughness.base_color_factor, sizeof(factor));
			base_texture = material->pbr_metallic_roughness.base_color_texture.texture;
			metal_rough_texture = material->pbr_metallic_roughness.metallic_roughness_texture.texture;
			metallic = material->pbr_metallic_roughness.metallic_factor;
			roughness = material->pbr_metallic_roughness.roughness_factor;
		}
		else if (material->has_pbr_specular_glossiness)
		{
			memcpy(factor, material->pbr_specular_glossiness.diffuse_factor, sizeof(factor));
			base_texture = material->pbr_specular_glossiness.diffuse_texture.texture;
			metallic = 0.0f;
			roughness = 1.0f - material->pbr_specular_glossiness.glossiness_factor;
		}
		normal_texture = material->normal_texture.texture;
		normal_scale = material->normal_texture.scale;
		occlusion_texture = material->occlusion_texture.texture;
		occlusion_strength = material->occlusion_texture.scale;
		emissive_texture = material->emissive_texture.texture;
		memcpy(emissive, material->emissive_factor, sizeof(emissive));
		if (material->has_emissive_strength)
		{
			for (int k = 0; k < 3; k++)
			{
				emissive[k] *= material->emissive_strength.emissive_strength;
			}
		}
	}
	surface->pixels = texture_at(surfaces, model, base_texture, 0, 0, &surface->width, &surface->height);
	if (surface->pixels == NULL)
	{
		surface->width = SOLID_TEXTURE_SIZE;
		surface->height = SOLID_TEXTURE_SIZE;
		surface->pixels = malloc(SOLID_TEXTURE_SIZE * SOLID_TEXTURE_SIZE * 4);
		memset(surface->pixels, 255, SOLID_TEXTURE_SIZE * SOLID_TEXTURE_SIZE * 4);
	}
	texel_count = (size_t)surface->width * (size_t)surface->height;
	// The base color's factor baked in (the base color is in sRGB; the
	// factor is near enough taken there too)
	for (size_t t = 0; t < texel_count; t++)
	{
		uint8_t *texel = &surface->pixels[t * 4];

		for (int k = 0; k < 4; k++)
		{
			texel[k] = clamp_byte((float)texel[k] * factor[k]);
		}
		if (surface->alpha_mode == cgltf_alpha_mode_opaque)
		{
			texel[3] = 255;
		}
		else
		{
			// Cut out at the material's own cutoff (blended ones at a half:
			// the game draws nothing half transparent)
			float threshold = surface->alpha_mode == cgltf_alpha_mode_mask ? surface->alpha_cutoff * 255.0f : 127.0f;

			texel[3] = (float)texel[3] >= threshold ? 255 : 0;
			surface->cut_out |= texel[3] == 0;
		}
	}

	// The normal map, its scale in (X and Y stretched, the length mended)
	normal_map = texture_at(surfaces, model, normal_texture, surface->width, surface->height, NULL, NULL);
	if (normal_map != NULL)
	{
		for (size_t t = 0; t < texel_count; t++)
		{
			uint8_t *texel = &normal_map[t * 4];
			float n[3] = { ((float)texel[0] / 255.0f * 2.0f - 1.0f) * normal_scale, ((float)texel[1] / 255.0f * 2.0f - 1.0f) * normal_scale,
				(float)texel[2] / 255.0f * 2.0f - 1.0f };

			normalize3(n);
			for (int k = 0; k < 3; k++)
			{
				texel[k] = clamp_byte((n[k] * 0.5f + 0.5f) * 255.0f);
			}
			texel[3] = 255;
		}
		surface->normal_pixels = normal_map;
	}

	// Occlusion, roughness, metal: glTF's channels (occlusion in R of its own
	// texture, often the same image; roughness G and metal B), the factors
	// and the occlusion strength in
	metal_rough = texture_at(surfaces, model, metal_rough_texture, surface->width, surface->height, NULL, NULL);
	occlusion = texture_at(surfaces, model, occlusion_texture, surface->width, surface->height, NULL, NULL);
	surface->orm_pixels = malloc(texel_count * 4);
	for (size_t t = 0; t < texel_count; t++)
	{
		uint8_t *texel = &surface->orm_pixels[t * 4];
		float occluded = occlusion != NULL ? (float)occlusion[t * 4] / 255.0f : 1.0f;
		float rough = metal_rough != NULL ? (float)metal_rough[t * 4 + 1] / 255.0f : 1.0f;
		float metal = metal_rough != NULL ? (float)metal_rough[t * 4 + 2] / 255.0f : 1.0f;

		texel[0] = clamp_byte((1.0f + (occluded - 1.0f) * occlusion_strength) * 255.0f);
		texel[1] = clamp_byte(rough * roughness * 255.0f);
		texel[2] = clamp_byte(metal * metallic * 255.0f);
		texel[3] = 255;
	}
	free(metal_rough);
	free(occlusion);

	// Emissive (only where it shines at all)
	if (emissive[0] > 0.0f || emissive[1] > 0.0f || emissive[2] > 0.0f)
	{
		emissive_map = texture_at(surfaces, model, emissive_texture, surface->width, surface->height, NULL, NULL);
		if (emissive_map == NULL)
		{
			emissive_map = malloc(texel_count * 4);
			memset(emissive_map, 255, texel_count * 4);
		}
		for (size_t t = 0; t < texel_count; t++)
		{
			for (int k = 0; k < 3; k++)
			{
				emissive_map[t * 4 + (size_t)k] = clamp_byte((float)emissive_map[t * 4 + (size_t)k] * emissive[k]);
			}
			emissive_map[t * 4 + 3] = 255;
		}
		surface->emissive_pixels = emissive_map;
	}
	return surfaces->count++;
}

// ---------------------------------------------------------------------------
// Gathering the meshes

typedef struct
{
	float position[3];                 // for the actor: in the space its inverse binds start from
	float normal[3];
	float rest_position[3];            // where it stands at rest, game space (the object, the bounds)
	float rest_normal[3];
	float tangent[4];                  // xyz along U, w the bitangent's sign (as the normal: actor's space)
	float rest_tangent[4];
	float uv[2];
	int bones[ACTOR_FILE_INFLUENCES];  // the game's bone indices
	float weights[ACTOR_FILE_INFLUENCES];
	int surface;
} gathered_vertex_t;

typedef struct
{
	gathered_vertex_t *vertices;
	int vertex_count;
	int vertex_capacity;
	uint32_t *indices;
	int index_count;
	int index_capacity;
	int dropped_influences;
	int skipped_primitives;
} gathered_t;

typedef struct
{
	const cgltf_skin *skin;            // NULL: one still bone
	int bone_count;
	int joint_count;                   // the skin's (past the game's most, its unweighted ends left out: bone_of_joint 0)
	int joint_of_bone[ACTOR_FILE_MAX_BONES];   // the skin's joint index of each game bone
	int bone_of_joint[256];
	int parent[ACTOR_FILE_MAX_BONES];
	int node_of_bone[ACTOR_FILE_MAX_BONES];
	float inverse_bind[ACTOR_FILE_MAX_BONES][16];  // the skin's own
	float game_inverse_bind[ACTOR_FILE_MAX_BONES][16];
	float vertex_matrix[16];           // M (see the top of the file)
} skeleton_t;

static int bone_of_node(const model_t *model, const skeleton_t *skeleton, const cgltf_node *node)
{
	for (; node != NULL; node = node->parent)
	{
		for (int b = 0; b < skeleton->bone_count; b++)
		{
			if (skeleton->node_of_bone[b] == node_index(model, node))
			{
				return b;
			}
		}
	}
	return 0;
}

// Which of a skin's joints move some vertex (weighted in a primitive
// skinned by it), or have one that does under them: the rest are ends a
// rig carries for its own sake (Blender's "_end" leaves)
static int needed_joints(const model_t *model, const cgltf_skin *skin, bool needed[256])
{
	int joint_count = (int)skin->joints_count;
	int count = 0;

	memset(needed, 0, sizeof(bool) * 256);
	for (cgltf_size n = 0; n < model->data->nodes_count; n++)
	{
		const cgltf_node *node = &model->data->nodes[n];

		if (node->skin != skin || node->mesh == NULL)
		{
			continue;
		}
		for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
		{
			const cgltf_primitive *primitive = &node->mesh->primitives[p];
			const cgltf_accessor *joints[2] = { NULL, NULL };
			const cgltf_accessor *weights[2] = { NULL, NULL };

			for (cgltf_size a = 0; a < primitive->attributes_count; a++)
			{
				const cgltf_attribute *attribute = &primitive->attributes[a];

				if (attribute->index < 0 || attribute->index > 1)
				{
					continue;
				}
				if (attribute->type == cgltf_attribute_type_joints)
				{
					joints[attribute->index] = attribute->data;
				}
				else if (attribute->type == cgltf_attribute_type_weights)
				{
					weights[attribute->index] = attribute->data;
				}
			}
			for (int set = 0; set < 2; set++)
			{
				if (joints[set] == NULL || weights[set] == NULL)
				{
					continue;
				}
				for (cgltf_size v = 0; v < joints[set]->count; v++)
				{
					cgltf_uint joint_values[4] = { 0, 0, 0, 0 };
					float weight_values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

					cgltf_accessor_read_uint(joints[set], v, joint_values, 4);
					cgltf_accessor_read_float(weights[set], v, weight_values, 4);
					for (int k = 0; k < 4; k++)
					{
						if (weight_values[k] > 0.0f && joint_values[k] < (cgltf_uint)joint_count)
						{
							needed[joint_values[k]] = true;
						}
					}
				}
			}
		}
	}
	// (up from each needed one: its ancestors are needed too)
	for (int j = 0; j < joint_count; j++)
	{
		if (!needed[j])
		{
			continue;
		}
		for (const cgltf_node *parent = skin->joints[j]->parent; parent != NULL; parent = parent->parent)
		{
			for (int k = 0; k < joint_count; k++)
			{
				if (skin->joints[k] == parent)
				{
					needed[k] = true;
				}
			}
		}
	}
	for (int j = 0; j < joint_count; j++)
	{
		count += needed[j] ? 1 : 0;
	}
	return count;
}

// The skin's joints as the game's bones, parents first
static bool make_skeleton(const model_t *model, const cgltf_skin *skin, skeleton_t *skeleton)
{
	int joint_count;
	int placed = 0;
	bool is_placed[256] = { false };
	int joint_parent[256];
	bool needed[256];
	int needed_count;

	memset(skeleton, 0, sizeof(*skeleton));
	skeleton->skin = skin;
	if (skin == NULL)
	{
		skeleton->bone_count = 1;
		skeleton->parent[0] = -1;
		skeleton->node_of_bone[0] = -1;
		matrix_identity(skeleton->inverse_bind[0]);
		matrix_identity(skeleton->game_inverse_bind[0]);
		matrix_identity(skeleton->vertex_matrix);
		return true;
	}
	joint_count = (int)skin->joints_count;
	if (joint_count > 256)
	{
		fprintf(stderr, "The skin has %d joints; the converter takes 256 at most\n", joint_count);
		return false;
	}
	skeleton->joint_count = joint_count;
	// Past the game's most: the joints that move nothing (and have nothing
	// under them that does) left out
	for (int j = 0; j < joint_count; j++)
	{
		needed[j] = true;
	}
	needed_count = joint_count;
	if (joint_count > ACTOR_FILE_MAX_BONES)
	{
		needed_count = needed_joints(model, skin, needed);
		if (needed_count > ACTOR_FILE_MAX_BONES)
		{
			fprintf(stderr, "The skin has %d joints, %d of them moving something; the game takes %d at most\n", joint_count, needed_count,
				ACTOR_FILE_MAX_BONES);
			return false;
		}
		printf("  the skin's %d joints: %d left out (they move nothing), %d bones\n", joint_count, joint_count - needed_count, needed_count);
	}
	for (int j = 0; j < joint_count; j++)
	{
		const cgltf_node *parent = skin->joints[j]->parent;

		joint_parent[j] = -1;
		skeleton->bone_of_joint[j] = 0;
		if (!needed[j])
		{
			is_placed[j] = true;
			continue;
		}
		for (; parent != NULL && joint_parent[j] < 0; parent = parent->parent)
		{
			for (int k = 0; k < joint_count; k++)
			{
				if (needed[k] && skin->joints[k] == parent)
				{
					joint_parent[j] = k;
					break;
				}
			}
		}
	}
	while (placed < needed_count)
	{
		int placed_before = placed;

		for (int j = 0; j < joint_count; j++)
		{
			if (!is_placed[j] && (joint_parent[j] < 0 || is_placed[joint_parent[j]]))
			{
				is_placed[j] = true;
				skeleton->bone_of_joint[j] = placed;
				skeleton->joint_of_bone[placed++] = j;
			}
		}
		if (placed == placed_before)
		{
			fprintf(stderr, "The skin's joints form a loop\n");
			return false;
		}
	}
	skeleton->bone_count = needed_count;
	for (int b = 0; b < needed_count; b++)
	{
		int j = skeleton->joint_of_bone[b];

		skeleton->parent[b] = joint_parent[j] < 0 ? -1 : skeleton->bone_of_joint[joint_parent[j]];
		skeleton->node_of_bone[b] = node_index(model, skin->joints[j]);
		if (skin->inverse_bind_matrices != NULL)
		{
			cgltf_accessor_read_float(skin->inverse_bind_matrices, (cgltf_size)j, skeleton->inverse_bind[b], 16);
		}
		else
		{
			matrix_identity(skeleton->inverse_bind[b]);
		}
	}
	return true;
}

// The game's inverse binds and M, from the rest pose (see the top of the
// file); worlds: the rest pose's
static void bind_skeleton(skeleton_t *skeleton, float (*worlds)[16])
{
	float inverse_vertex[16];

	if (skeleton->skin == NULL)
	{
		return;
	}
	matrix_multiply(skeleton->vertex_matrix, worlds[skeleton->node_of_bone[0]], skeleton->inverse_bind[0]);
	matrix_inverse(inverse_vertex, skeleton->vertex_matrix);
	for (int b = 0; b < skeleton->bone_count; b++)
	{
		const float *world = worlds[skeleton->node_of_bone[b]];
		float rigid[16];
		float inverse_rigid[16];
		float rest_of_it[16];

		rigid_matrix(world, rigid);
		matrix_inverse(inverse_rigid, rigid);
		matrix_multiply(rest_of_it, inverse_rigid, world);
		matrix_multiply(skeleton->game_inverse_bind[b], rest_of_it, skeleton->inverse_bind[b]);
		matrix_multiply(skeleton->game_inverse_bind[b], skeleton->game_inverse_bind[b], inverse_vertex);
	}
}

// A tangent through a matrix: its direction as the surface's, its sign
// turned where the matrix mirrors
static void transform_tangent(const float m[16], const float tangent[4], float out[4])
{
	float direction[3];

	for (int r = 0; r < 3; r++)
	{
		direction[r] = m[0 * 4 + r] * tangent[0] + m[1 * 4 + r] * tangent[1] + m[2 * 4 + r] * tangent[2];
	}
	normalize3(direction);
	memcpy(out, direction, sizeof(direction));
	out[3] = (tangent[3] < 0.0f ? -1.0f : 1.0f) * (matrix_determinant3(m) < 0.0f ? -1.0f : 1.0f);
}

// Tangents the file did not have: from the triangles' texture coordinates
// (summed per vertex, then made square to its normal), at rest and in the
// actor's space
static void make_tangents(gathered_t *gathered, int base, int vertex_count, int first_index)
{
	float (*sums)[6] = calloc((size_t)vertex_count + 1, sizeof(float[6]));   // tangent, bitangent

	for (int space = 0; space < 2; space++)
	{
		memset(sums, 0, sizeof(float[6]) * (size_t)vertex_count);
		for (int i = first_index; i + 2 < gathered->index_count; i += 3)
		{
			gathered_vertex_t *corners[3];
			float *p[3];
			float e1[3];
			float e2[3];
			float du1;
			float dv1;
			float du2;
			float dv2;
			float determinant;
			float tangent[3];
			float bitangent[3];

			for (int k = 0; k < 3; k++)
			{
				corners[k] = &gathered->vertices[gathered->indices[i + k]];
				p[k] = space == 0 ? corners[k]->rest_position : corners[k]->position;
			}
			for (int k = 0; k < 3; k++)
			{
				e1[k] = p[1][k] - p[0][k];
				e2[k] = p[2][k] - p[0][k];
			}
			du1 = corners[1]->uv[0] - corners[0]->uv[0];
			dv1 = corners[1]->uv[1] - corners[0]->uv[1];
			du2 = corners[2]->uv[0] - corners[0]->uv[0];
			dv2 = corners[2]->uv[1] - corners[0]->uv[1];
			determinant = du1 * dv2 - du2 * dv1;
			if (fabsf(determinant) < 1e-12f)
			{
				continue;
			}
			determinant = 1.0f / determinant;
			for (int k = 0; k < 3; k++)
			{
				tangent[k] = (e1[k] * dv2 - e2[k] * dv1) * determinant;
				bitangent[k] = (e2[k] * du1 - e1[k] * du2) * determinant;
			}
			for (int c = 0; c < 3; c++)
			{
				int v = (int)gathered->indices[i + c] - base;

				if (v < 0 || v >= vertex_count)
				{
					continue;
				}
				for (int k = 0; k < 3; k++)
				{
					sums[v][k] += tangent[k];
					sums[v][3 + k] += bitangent[k];
				}
			}
		}
		for (int v = 0; v < vertex_count; v++)
		{
			gathered_vertex_t *vertex = &gathered->vertices[base + v];
			const float *n = space == 0 ? vertex->rest_normal : vertex->normal;
			float *out = space == 0 ? vertex->rest_tangent : vertex->tangent;
			float t[3] = { sums[v][0], sums[v][1], sums[v][2] };
			float dot = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
			float cross[3];

			for (int k = 0; k < 3; k++)
			{
				t[k] -= n[k] * dot;
			}
			if (t[0] * t[0] + t[1] * t[1] + t[2] * t[2] < 1e-20f)
			{
				// No texture direction here: any square to the normal
				float axis[3] = { fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
				float along = axis[0] * n[0] + axis[1] * n[1] + axis[2] * n[2];

				for (int k = 0; k < 3; k++)
				{
					t[k] = axis[k] - n[k] * along;
				}
			}
			normalize3(t);
			cross[0] = n[1] * t[2] - n[2] * t[1];
			cross[1] = n[2] * t[0] - n[0] * t[2];
			cross[2] = n[0] * t[1] - n[1] * t[0];
			memcpy(out, t, sizeof(t));
			out[3] = cross[0] * sums[v][3] + cross[1] * sums[v][4] + cross[2] * sums[v][5] < 0.0f ? -1.0f : 1.0f;
		}
	}
	free(sums);
}

static void gathered_reserve(gathered_t *gathered, int vertices, int indices)
{
	if (gathered->vertex_count + vertices > gathered->vertex_capacity)
	{
		gathered->vertex_capacity = (gathered->vertex_count + vertices) * 2;
		gathered->vertices = realloc(gathered->vertices, sizeof(gathered_vertex_t) * (size_t)gathered->vertex_capacity);
	}
	if (gathered->index_count + indices > gathered->index_capacity)
	{
		gathered->index_capacity = (gathered->index_count + indices) * 2;
		gathered->indices = realloc(gathered->indices, sizeof(uint32_t) * (size_t)gathered->index_capacity);
	}
}

static const cgltf_accessor *attribute(const cgltf_primitive *primitive, cgltf_attribute_type type, int set)
{
	for (cgltf_size a = 0; a < primitive->attributes_count; a++)
	{
		if (primitive->attributes[a].type == type && primitive->attributes[a].index == set)
		{
			return primitive->attributes[a].data;
		}
	}
	return NULL;
}

// Keeps the four heaviest of up to eight influences
static void keep_heaviest(const int *bones, const float *weights, int count, gathered_vertex_t *vertex, gathered_t *gathered)
{
	int used[8] = { 0 };

	for (int k = 0; k < ACTOR_FILE_INFLUENCES; k++)
	{
		int best = -1;

		for (int i = 0; i < count; i++)
		{
			if (!used[i] && weights[i] > 0.0f && (best < 0 || weights[i] > weights[best]))
			{
				best = i;
			}
		}
		vertex->bones[k] = best >= 0 ? bones[best] : 0;
		vertex->weights[k] = best >= 0 ? weights[best] : 0.0f;
		if (best >= 0)
		{
			used[best] = 1;
		}
	}
	for (int i = 0; i < count; i++)
	{
		if (!used[i] && weights[i] > 1e-4f)
		{
			gathered->dropped_influences++;
		}
	}
}

// Every triangle of every mesh in the scene, posed at rest
static bool gather(const model_t *model, const skeleton_t *skeleton, float (*worlds)[16], surfaces_t *surfaces, gathered_t *gathered)
{
	for (size_t at = 0; at < model->node_count; at++)
	{
		const cgltf_node *node = &model->data->nodes[model->order[at]];
		const float *world = worlds[model->order[at]];
		bool skinned = node->skin != NULL && node->skin == skeleton->skin;
		bool mirrored = !skinned && matrix_determinant3(world) < 0.0f;
		int attached_bone = skinned ? 0 : bone_of_node(model, skeleton, node);

		if (node->mesh == NULL)
		{
			continue;
		}
		if (node->skin != NULL && !skinned)
		{
			printf("  warning: mesh '%s' has another skin than the first; it is taken at rest, on the root\n",
				node->mesh->name != NULL ? node->mesh->name : "?");
		}
		for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
		{
			const cgltf_primitive *primitive = &node->mesh->primitives[p];
			const cgltf_accessor *positions = attribute(primitive, cgltf_attribute_type_position, 0);
			const cgltf_accessor *normals = attribute(primitive, cgltf_attribute_type_normal, 0);
			const cgltf_accessor *tangents = attribute(primitive, cgltf_attribute_type_tangent, 0);
			int uv_set = 0;
			const cgltf_accessor *uvs;
			const cgltf_accessor *joints[2] = { attribute(primitive, cgltf_attribute_type_joints, 0),
				attribute(primitive, cgltf_attribute_type_joints, 1) };
			const cgltf_accessor *weights[2] = { attribute(primitive, cgltf_attribute_type_weights, 0),
				attribute(primitive, cgltf_attribute_type_weights, 1) };
			int surface;
			int base = gathered->vertex_count;
			int vertex_count;
			int index_count;

			if (primitive->type != cgltf_primitive_type_triangles || positions == NULL)
			{
				gathered->skipped_primitives++;
				continue;
			}
			if (primitive->material != NULL && primitive->material->has_pbr_metallic_roughness
				&& primitive->material->pbr_metallic_roughness.base_color_texture.texture != NULL)
			{
				uv_set = primitive->material->pbr_metallic_roughness.base_color_texture.texcoord;
			}
			uvs = attribute(primitive, cgltf_attribute_type_texcoord, uv_set);
			surface = surface_of(surfaces, model, primitive->material);
			if (surface < 0)
			{
				fprintf(stderr, "More than %d different materials\n", MAX_SURFACES);
				return false;
			}
			vertex_count = (int)positions->count;
			index_count = primitive->indices != NULL ? (int)primitive->indices->count : vertex_count;
			gathered_reserve(gathered, vertex_count, index_count);
			for (int v = 0; v < vertex_count; v++)
			{
				gathered_vertex_t *vertex = &gathered->vertices[base + v];
				float position[3];
				float normal[3] = { 0.0f, 0.0f, 0.0f };
				float tangent[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

				memset(vertex, 0, sizeof(*vertex));
				cgltf_accessor_read_float(positions, (cgltf_size)v, position, 3);
				if (tangents != NULL)
				{
					cgltf_accessor_read_float(tangents, (cgltf_size)v, tangent, 4);
				}
				if (normals != NULL)
				{
					cgltf_accessor_read_float(normals, (cgltf_size)v, normal, 3);
				}
				if (uvs != NULL)
				{
					cgltf_accessor_read_float(uvs, (cgltf_size)v, vertex->uv, 2);
				}
				vertex->surface = surface;
				if (skinned)
				{
					int bones[8] = { 0 };
					float influence[8] = { 0.0f };
					int count = 0;
					float rest_matrix[16] = { 0 };
					float total = 0.0f;

					for (int set = 0; set < 2; set++)
					{
						cgltf_uint joint_values[4] = { 0, 0, 0, 0 };
						float weight_values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

						if (joints[set] == NULL || weights[set] == NULL)
						{
							continue;
						}
						cgltf_accessor_read_uint(joints[set], (cgltf_size)v, joint_values, 4);
						cgltf_accessor_read_float(weights[set], (cgltf_size)v, weight_values, 4);
						for (int k = 0; k < 4; k++)
						{
							int joint = joint_values[k] < (cgltf_uint)skeleton->joint_count ? (int)joint_values[k] : 0;

							bones[count] = skeleton->bone_of_joint[joint];
							influence[count] = weight_values[k];
							count++;
						}
					}
					keep_heaviest(bones, influence, count, vertex, gathered);
					for (int k = 0; k < ACTOR_FILE_INFLUENCES; k++)
					{
						total += vertex->weights[k];
					}
					if (total <= 1e-9f)
					{
						vertex->bones[0] = 0;
						vertex->weights[0] = 1.0f;
						total = 1.0f;
					}
					// Where it stands at rest: skinned as glTF does it
					for (int k = 0; k < ACTOR_FILE_INFLUENCES; k++)
					{
						float skin_matrix[16];
						int b = vertex->bones[k];

						if (vertex->weights[k] <= 0.0f)
						{
							continue;
						}
						matrix_multiply(skin_matrix, worlds[skeleton->node_of_bone[b]], skeleton->inverse_bind[b]);
						for (int e = 0; e < 16; e++)
						{
							rest_matrix[e] += skin_matrix[e] * vertex->weights[k] / total;
						}
					}
					matrix_point(rest_matrix, position, vertex->rest_position);
					matrix_normal(rest_matrix, normal, vertex->rest_normal);
					matrix_point(skeleton->vertex_matrix, position, vertex->position);
					matrix_normal(skeleton->vertex_matrix, normal, vertex->normal);
					transform_tangent(rest_matrix, tangent, vertex->rest_tangent);
					transform_tangent(skeleton->vertex_matrix, tangent, vertex->tangent);
				}
				else
				{
					matrix_point(world, position, vertex->rest_position);
					matrix_normal(world, normal, vertex->rest_normal);
					memcpy(vertex->position, vertex->rest_position, sizeof(vertex->position));
					memcpy(vertex->normal, vertex->rest_normal, sizeof(vertex->normal));
					transform_tangent(world, tangent, vertex->rest_tangent);
					memcpy(vertex->tangent, vertex->rest_tangent, sizeof(vertex->tangent));
					vertex->bones[0] = attached_bone;
					vertex->weights[0] = 1.0f;
				}
			}
			for (int i = 0; i + 2 < index_count; i += 3)
			{
				uint32_t triangle[3];

				for (int k = 0; k < 3; k++)
				{
					uint32_t index = primitive->indices != NULL ? (uint32_t)cgltf_accessor_read_index(primitive->indices, (cgltf_size)(i + k))
						: (uint32_t)(i + k);

					triangle[k] = (uint32_t)base + (index < (uint32_t)vertex_count ? index : 0u);
				}
				gathered->indices[gathered->index_count++] = triangle[0];
				gathered->indices[gathered->index_count++] = mirrored ? triangle[2] : triangle[1];
				gathered->indices[gathered->index_count++] = mirrored ? triangle[1] : triangle[2];
			}
			// Normals the file did not have: smooth, from the triangles (at
			// rest, and in the actor's own space)
			if (normals == NULL)
			{
				int first_index = gathered->index_count - (index_count / 3) * 3;

				for (int v = base; v < base + vertex_count; v++)
				{
					memset(gathered->vertices[v].rest_normal, 0, sizeof(float) * 3);
					memset(gathered->vertices[v].normal, 0, sizeof(float) * 3);
				}
				for (int i = first_index; i + 2 < gathered->index_count; i += 3)
				{
					for (int space = 0; space < 2; space++)
					{
						gathered_vertex_t *corners[3] = { &gathered->vertices[gathered->indices[i]], &gathered->vertices[gathered->indices[i + 1]],
							&gathered->vertices[gathered->indices[i + 2]] };
						float *a = space == 0 ? corners[0]->rest_position : corners[0]->position;
						float *b = space == 0 ? corners[1]->rest_position : corners[1]->position;
						float *c = space == 0 ? corners[2]->rest_position : corners[2]->position;
						float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
						float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
						float face[3] = { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0] };

						for (int k = 0; k < 3; k++)
						{
							float *normal = space == 0 ? corners[k]->rest_normal : corners[k]->normal;

							for (int e = 0; e < 3; e++)
							{
								normal[e] += face[e];
							}
						}
					}
				}
				for (int v = base; v < base + vertex_count; v++)
				{
					normalize3(gathered->vertices[v].rest_normal);
					normalize3(gathered->vertices[v].normal);
				}
			}
			if (tangents == NULL)
			{
				make_tangents(gathered, base, vertex_count, gathered->index_count - (index_count / 3) * 3);
			}
			gathered->vertex_count += vertex_count;
		}
	}
	if (gathered->index_count == 0)
	{
		fprintf(stderr, "The model has no triangles\n");
		return false;
	}
	return true;
}

static void rest_bounds(const gathered_t *gathered, float minimum[3], float maximum[3])
{
	for (int k = 0; k < 3; k++)
	{
		minimum[k] = 1e30f;
		maximum[k] = -1e30f;
	}
	for (int v = 0; v < gathered->vertex_count; v++)
	{
		for (int k = 0; k < 3; k++)
		{
			minimum[k] = fminf(minimum[k], gathered->vertices[v].rest_position[k]);
			maximum[k] = fmaxf(maximum[k], gathered->vertices[v].rest_position[k]);
		}
	}
}

static void free_gathered(gathered_t *gathered)
{
	free(gathered->vertices);
	free(gathered->indices);
	memset(gathered, 0, sizeof(*gathered));
}

static void free_surfaces(surfaces_t *surfaces)
{
	for (int s = 0; s < surfaces->count; s++)
	{
		free(surfaces->surfaces[s].pixels);
		free(surfaces->surfaces[s].normal_pixels);
		free(surfaces->surfaces[s].orm_pixels);
		free(surfaces->surfaces[s].emissive_pixels);
	}
	surfaces->count = 0;
}

// ---------------------------------------------------------------------------
// Options

typedef struct
{
	char name[ACTOR_FILE_NAME_LENGTH];
	char clip[128];                    // a clip's name or number
	int loop;                          // -1: guess, 0: once, 1: loop
	float duration_seconds;
} action_option_t;

typedef struct
{
	const char *input;
	const char *output;
	float scale;
	float height;                      // 0: as it is
	bool z_up;
	float turn_degrees;
	bool ground;
	bool in_place;
	bool list;
	int max_texture;
	const char *no_collide;            // material names, comma separated: drawn, not in the way (.pobj)
	action_option_t actions[MAX_ACTIONS];
	int action_count;
} options_t;

static void usage(void)
{
	fprintf(stderr,
		"Usage: potop_convert [options] model.gltf|model.glb output.pact|output.pobj|output.e3d\n"
		"  .pact: an animated actor; .pobj: an object; .e3d: an Eternal Lands object (its textures as PNG files next to it)\n"
		"  --scale S            sizes times S\n"
		"  --height M           scaled to stand M meters tall at rest (after --scale)\n"
		"  --up z               the model has Z up (glTF says Y)\n"
		"  --turn DEGREES       turned around the vertical (the game's actors face +Z, as glTF's do)\n"
		"  --ground             moved so its bottom is at 0, its middle over the origin\n"
		"  --in-place           the root bones do not travel (walks that move the model forward)\n"
		"  --action NAME=CLIP[:loop|:once][@SECONDS]   an action from a clip (its name or number);\n"
		"                       given once or more, only these; otherwise every clip is one\n"
		"  --max-texture N      textures halved until no side is above N (2048)\n"
		"  --no-collide A,B     .pobj: these materials (names as --list shows them) are not in the way;\n"
		"                       cut out ones (leaves) never are\n"
		"  --list               what the file holds, nothing written\n");
}

static bool parse_action(const char *text, action_option_t *action)
{
	const char *equals = strchr(text, '=');
	char rest[160];
	char *at;
	char *colon;

	memset(action, 0, sizeof(*action));
	action->loop = -1;
	if (equals == NULL || equals == text)
	{
		return false;
	}
	snprintf(action->name, sizeof(action->name), "%.*s", (int)(equals - text) < ACTOR_FILE_NAME_LENGTH - 1 ? (int)(equals - text)
		: ACTOR_FILE_NAME_LENGTH - 1, text);
	snprintf(rest, sizeof(rest), "%s", equals + 1);
	at = strchr(rest, '@');
	if (at != NULL)
	{
		action->duration_seconds = strtof(at + 1, NULL);
		*at = '\0';
	}
	colon = strrchr(rest, ':');
	if (colon != NULL && (strcmp(colon, ":loop") == 0 || strcmp(colon, ":once") == 0))
	{
		action->loop = strcmp(colon, ":loop") == 0 ? 1 : 0;
		*colon = '\0';
	}
	snprintf(action->clip, sizeof(action->clip), "%s", rest);
	return action->clip[0] != '\0';
}

static bool parse_options(int argument_count, char **arguments, options_t *options)
{
	int positional = 0;

	memset(options, 0, sizeof(*options));
	options->scale = 1.0f;
	options->max_texture = 2048;
	for (int i = 0; i < argument_count; i++)
	{
		const char *argument = arguments[i];
		const char *value = i + 1 < argument_count ? arguments[i + 1] : NULL;

		if (strcmp(argument, "--list") == 0)
		{
			options->list = true;
		}
		else if (strcmp(argument, "--ground") == 0)
		{
			options->ground = true;
		}
		else if (strcmp(argument, "--in-place") == 0)
		{
			options->in_place = true;
		}
		else if (argument[0] == '-' && argument[1] == '-' && value == NULL)
		{
			fprintf(stderr, "%s needs a value\n", argument);
			return false;
		}
		else if (strcmp(argument, "--scale") == 0)
		{
			options->scale = strtof(value, NULL);
			i++;
		}
		else if (strcmp(argument, "--height") == 0)
		{
			options->height = strtof(value, NULL);
			i++;
		}
		else if (strcmp(argument, "--up") == 0)
		{
			options->z_up = value[0] == 'z' || value[0] == 'Z';
			i++;
		}
		else if (strcmp(argument, "--turn") == 0)
		{
			options->turn_degrees = strtof(value, NULL);
			i++;
		}
		else if (strcmp(argument, "--no-collide") == 0)
		{
			options->no_collide = value;
			i++;
		}
		else if (strcmp(argument, "--max-texture") == 0)
		{
			options->max_texture = atoi(value);
			i++;
		}
		else if (strcmp(argument, "--action") == 0)
		{
			if (options->action_count == MAX_ACTIONS || !parse_action(value, &options->actions[options->action_count]))
			{
				fprintf(stderr, "Cannot read the action '%s' (NAME=CLIP[:loop|:once][@SECONDS])\n", value);
				return false;
			}
			options->action_count++;
			i++;
		}
		else if (argument[0] == '-' && argument[1] == '-')
		{
			fprintf(stderr, "Unknown option %s\n", argument);
			return false;
		}
		else if (positional == 0)
		{
			options->input = argument;
			positional++;
		}
		else if (positional == 1)
		{
			options->output = argument;
			positional++;
		}
		else
		{
			fprintf(stderr, "Too many files given\n");
			return false;
		}
	}
	if (options->max_texture < 16)
	{
		options->max_texture = 16;
	}
	return options->input != NULL && (options->list || options->output != NULL);
}

bool is_gltf_path(const char *path)
{
	size_t length = strlen(path);

	return (length > 5 && strcmp(path + length - 5, ".gltf") == 0) || (length > 4 && strcmp(path + length - 4, ".glb") == 0);
}

static bool ends_with(const char *text, const char *end)
{
	size_t length = strlen(text);
	size_t end_length = strlen(end);

	return length >= end_length && strcmp(text + length - end_length, end) == 0;
}

// ---------------------------------------------------------------------------
// --list

static void list_model(const model_t *model)
{
	const cgltf_data *data = model->data;

	printf("%s: %zu nodes, %zu meshes, %zu skins, %zu animations, %zu materials, %zu images\n", "glTF", data->nodes_count,
		data->meshes_count, data->skins_count, data->animations_count, data->materials_count, data->images_count);
	for (cgltf_size m = 0; m < data->meshes_count; m++)
	{
		size_t triangles = 0;

		for (cgltf_size p = 0; p < data->meshes[m].primitives_count; p++)
		{
			const cgltf_primitive *primitive = &data->meshes[m].primitives[p];
			const cgltf_accessor *positions = attribute(primitive, cgltf_attribute_type_position, 0);

			triangles += (primitive->indices != NULL ? primitive->indices->count : (positions != NULL ? positions->count : 0)) / 3;
		}
		printf("  mesh %zu '%s': %zu primitives, %zu triangles\n", m, data->meshes[m].name != NULL ? data->meshes[m].name : "",
			data->meshes[m].primitives_count, triangles);
	}
	for (cgltf_size s = 0; s < data->skins_count; s++)
	{
		printf("  skin %zu '%s': %zu joints\n", s, data->skins[s].name != NULL ? data->skins[s].name : "", data->skins[s].joints_count);
	}
	for (cgltf_size a = 0; a < data->animations_count; a++)
	{
		printf("  clip %zu '%s': %.3f s, %zu channels\n", a, data->animations[a].name != NULL ? data->animations[a].name : "",
			(double)animation_duration(&data->animations[a]), data->animations[a].channels_count);
	}
	for (cgltf_size i = 0; i < data->images_count; i++)
	{
		printf("  image %zu '%s'%s\n", i, data->images[i].uri != NULL && strncmp(data->images[i].uri, "data:", 5) != 0
			? data->images[i].uri : (data->images[i].name != NULL ? data->images[i].name : ""),
			data->images[i].buffer_view != NULL ? " (in the file)" : "");
	}
	for (cgltf_size i = 0; i < data->materials_count; i++)
	{
		const cgltf_material *material = &data->materials[i];
		const cgltf_texture *base = material->has_pbr_metallic_roughness ? material->pbr_metallic_roughness.base_color_texture.texture : NULL;
		static const char *const modes[] = { "opaque", "mask", "blend" };

		printf("  material %zu '%s': %s", i, material->name != NULL ? material->name : "",
			(size_t)material->alpha_mode < 3 ? modes[material->alpha_mode] : "?");
		if (material->alpha_mode == cgltf_alpha_mode_mask)
		{
			printf(" at %.2f", (double)material->alpha_cutoff);
		}
		if (base != NULL && base->image != NULL)
		{
			printf(", base color image %zu", (size_t)(base->image - data->images));
		}
		printf("%s\n", material->double_sided ? ", two sided" : "");
	}
}

// ---------------------------------------------------------------------------
// Writing

static void write_or_fail(FILE *file, const void *data, size_t size, bool *ok)
{
	if (*ok && size > 0 && fwrite(data, 1, size, file) != size)
	{
		*ok = false;
	}
}

// An action's name from a clip's: what follows the last '|' (Blender
// writes "Armature|Walk"), lower case, spaces as underscores
static void action_name_of(const char *clip, char *out, size_t out_size)
{
	const char *start = strrchr(clip, '|');
	size_t length = 0;

	start = start != NULL ? start + 1 : clip;
	for (; *start != '\0' && length + 1 < out_size; start++)
	{
		out[length++] = *start == ' ' ? '_' : (char)tolower((unsigned char)*start);
	}
	out[length] = '\0';
}

static bool guess_loop(const char *name)
{
	// (what plays once first: a walk's start or stop, an idle's glance
	// round, a carry's pick up are once, whatever else their names say)
	static const char *const once[] = { "stop", "start", "pick", "put", "look", "stretch", "wipe", "quench", "drink", "inspect", "coal", "take",
		"greet", "arms_in", "arms_out", "die", "death", "hit", "attack", "jump" };
	static const char *const looping[] = { "walk", "run", "idle", "loop", "swim", "fly", "glide", "survey", "stand", "breath", "turn", "bellows", "forge",
		"hammer", "carry", "sit", "step", "shuffle", "hips", "talk", "listen" };
	char lower[128];

	action_name_of(name, lower, sizeof(lower));
	for (size_t i = 0; i < sizeof(once) / sizeof(once[0]); i++)
	{
		if (strstr(lower, once[i]) != NULL)
		{
			return false;
		}
	}
	for (size_t i = 0; i < sizeof(looping) / sizeof(looping[0]); i++)
	{
		if (strstr(lower, looping[i]) != NULL)
		{
			return true;
		}
	}
	return false;
}

static void rows_of(const float m[16], float rows[12])
{
	for (int r = 0; r < 3; r++)
	{
		for (int c = 0; c < 4; c++)
		{
			rows[r * 4 + c] = m[c * 4 + r];
		}
	}
}

// The layers of the texture the actor has: the surfaces' base color, normal
// map, occlusion-roughness-metal and emissive
enum
{
	LAYER_BASE = 0,
	LAYER_NORMAL,
	LAYER_ORM,
	LAYER_EMISSIVE,
	LAYER_COUNT
};

static const uint8_t *surface_layer(const surface_t *surface, int layer)
{
	switch (layer)
	{
		case LAYER_NORMAL:
			return surface->normal_pixels;
		case LAYER_ORM:
			return surface->orm_pixels;
		case LAYER_EMISSIVE:
			return surface->emissive_pixels;
		default:
			return surface->pixels;
	}
}

// The texture of an actor, each layer alike: its surfaces packed in rows
// (each with its edges repeated around it, so filtering does not reach its
// neighbors) and the UVs moved into their places; one surface stays as it
// is. A layer no surface has is left out (NULL); where some have it and
// others not, the others get its neutral value (a flat normal, no light)
static void pack_atlas(surfaces_t *surfaces, gathered_t *gathered, uint8_t *layers_out[LAYER_COUNT], int *width_out, int *height_out)
{
	static const uint8_t neutral[LAYER_COUNT][4] = { { 255, 255, 255, 255 }, { 128, 128, 255, 255 }, { 255, 255, 0, 255 }, { 0, 0, 0, 255 } };
	int area = 0;
	int widest = 0;
	int width = 1;
	int x = 0;
	int y = 0;
	int row_height = 0;
	int height;
	bool clamped = false;
	bool single = surfaces->count == 1;

	if (single)
	{
		width = surfaces->surfaces[0].width;
		height = surfaces->surfaces[0].height;
		surfaces->surfaces[0].atlas_x = 0;
		surfaces->surfaces[0].atlas_y = 0;
	}
	else
	{
		for (int s = 0; s < surfaces->count; s++)
		{
			int w = surfaces->surfaces[s].width + 2 * ATLAS_PADDING;
			int h = surfaces->surfaces[s].height + 2 * ATLAS_PADDING;

			area += w * h;
			widest = w > widest ? w : widest;
		}
		while (width < widest || width * width < area)
		{
			width *= 2;
		}
		for (int s = 0; s < surfaces->count; s++)
		{
			surface_t *surface = &surfaces->surfaces[s];
			int w = surface->width + 2 * ATLAS_PADDING;
			int h = surface->height + 2 * ATLAS_PADDING;

			if (x + w > width)
			{
				x = 0;
				y += row_height;
				row_height = 0;
			}
			surface->atlas_x = x + ATLAS_PADDING;
			surface->atlas_y = y + ATLAS_PADDING;
			x += w;
			row_height = h > row_height ? h : row_height;
		}
		height = y + row_height;
	}
	for (int layer = 0; layer < LAYER_COUNT; layer++)
	{
		bool any = false;
		uint8_t *atlas;

		for (int s = 0; s < surfaces->count; s++)
		{
			any |= surface_layer(&surfaces->surfaces[s], layer) != NULL;
		}
		layers_out[layer] = NULL;
		if (!any)
		{
			continue;
		}
		atlas = calloc((size_t)width * (size_t)height, 4);
		for (int s = 0; s < surfaces->count; s++)
		{
			const surface_t *surface = &surfaces->surfaces[s];
			const uint8_t *pixels = surface_layer(surface, layer);
			int padding = single ? 0 : ATLAS_PADDING;

			for (int ty = -padding; ty < surface->height + padding; ty++)
			{
				int source_y = ty < 0 ? 0 : (ty >= surface->height ? surface->height - 1 : ty);

				for (int tx = -padding; tx < surface->width + padding; tx++)
				{
					int source_x = tx < 0 ? 0 : (tx >= surface->width ? surface->width - 1 : tx);

					memcpy(&atlas[((size_t)(surface->atlas_y + ty) * (size_t)width + (size_t)(surface->atlas_x + tx)) * 4u],
						pixels != NULL ? &pixels[((size_t)source_y * (size_t)surface->width + (size_t)source_x) * 4u] : neutral[layer], 4);
				}
			}
		}
		layers_out[layer] = atlas;
	}
	if (!single)
	{
		for (int v = 0; v < gathered->vertex_count; v++)
		{
			gathered_vertex_t *vertex = &gathered->vertices[v];
			const surface_t *surface = &surfaces->surfaces[vertex->surface];

			for (int k = 0; k < 2; k++)
			{
				if (vertex->uv[k] < -0.001f || vertex->uv[k] > 1.001f)
				{
					clamped = true;
				}
				vertex->uv[k] = vertex->uv[k] < 0.0f ? 0.0f : (vertex->uv[k] > 1.0f ? 1.0f : vertex->uv[k]);
			}
			vertex->uv[0] = ((float)surface->atlas_x + vertex->uv[0] * (float)surface->width) / (float)width;
			vertex->uv[1] = ((float)surface->atlas_y + vertex->uv[1] * (float)surface->height) / (float)height;
		}
	}
	if (clamped)
	{
		printf("  warning: some texture coordinates repeat the texture (outside 0..1); packed with others, they are clamped\n");
	}
	*width_out = width;
	*height_out = height;
}

// RGBA8 pixels compressed with zlib (malloc); NULL if that failed
static unsigned char *compress_pixels(const uint8_t *pixels, int width, int height, uint32_t *size_out)
{
	mz_ulong raw_size = (mz_ulong)width * (mz_ulong)height * 4u;
	mz_ulong size = mz_compressBound(raw_size);
	unsigned char *compressed = malloc(size);

	if (compressed == NULL || mz_compress2(compressed, &size, pixels, raw_size, MZ_BEST_COMPRESSION) != MZ_OK)
	{
		free(compressed);
		return NULL;
	}
	*size_out = (uint32_t)size;
	return compressed;
}

static int find_clip(const cgltf_data *data, const char *name)
{
	char *end = NULL;
	long number = strtol(name, &end, 10);

	if (end != name && *end == '\0' && number >= 0 && (cgltf_size)number < data->animations_count)
	{
		return (int)number;
	}
	for (cgltf_size a = 0; a < data->animations_count; a++)
	{
		char lower[128];
		char wanted[128];

		if (data->animations[a].name == NULL)
		{
			continue;
		}
		action_name_of(data->animations[a].name, lower, sizeof(lower));
		action_name_of(name, wanted, sizeof(wanted));
		if (strcmp(data->animations[a].name, name) == 0 || strcmp(lower, wanted) == 0)
		{
			return (int)a;
		}
	}
	return -1;
}

static int write_actor(const options_t *options, const model_t *model, const skeleton_t *skeleton, surfaces_t *surfaces,
	gathered_t *gathered)
{
	const cgltf_data *data = model->data;
	size_t node_count = data->nodes_count;
	node_pose_t *poses = calloc(node_count + 1, sizeof(node_pose_t));
	float (*worlds)[16] = calloc(node_count + 1, sizeof(float[16]));
	float rest_local[ACTOR_FILE_MAX_BONES][7];
	float previous_rotation[ACTOR_FILE_MAX_BONES][4];
	actor_file_header_t header;
	actor_file_bone_t bones[ACTOR_FILE_MAX_BONES];
	actor_file_action_t actions[MAX_ACTIONS];
	int action_count = 0;
	int clip_count = skeleton->skin != NULL ? (int)data->animations_count : 0;
	uint8_t *layers[LAYER_COUNT];
	unsigned char *compressed[LAYER_COUNT] = { NULL };
	uint32_t compressed_size[LAYER_COUNT] = { 0 };
	int atlas_width;
	int atlas_height;
	bool cut_out = false;
	FILE *output;
	bool ok = true;

	// The bones' rest pose, each relative to its parent
	memcpy(poses, model->rest, sizeof(node_pose_t) * node_count);
	compute_worlds(model, poses, worlds);
	memset(bones, 0, sizeof(bones));
	for (int b = 0; b < skeleton->bone_count; b++)
	{
		actor_file_bone_t *bone = &bones[b];
		float translation[3] = { 0.0f, 0.0f, 0.0f };
		float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

		if (skeleton->skin != NULL)
		{
			const cgltf_node *node = &data->nodes[skeleton->node_of_bone[b]];
			float rigid[16];
			float local[16];

			rigid_matrix(worlds[skeleton->node_of_bone[b]], rigid);
			if (skeleton->parent[b] >= 0)
			{
				float parent_rigid[16];
				float inverse_parent[16];

				rigid_matrix(worlds[skeleton->node_of_bone[skeleton->parent[b]]], parent_rigid);
				matrix_inverse(inverse_parent, parent_rigid);
				matrix_multiply(local, inverse_parent, rigid);
			}
			else
			{
				memcpy(local, rigid, sizeof(local));
			}
			matrix_rigid_part(local, translation, rotation);
			snprintf(bone->name, sizeof(bone->name), "%s", node->name != NULL ? node->name : "bone");
		}
		else
		{
			snprintf(bone->name, sizeof(bone->name), "root");
		}
		bone->parent = skeleton->parent[b];
		memcpy(bone->translation, translation, sizeof(translation));
		memcpy(bone->rotation, rotation, sizeof(rotation));
		rows_of(skeleton->game_inverse_bind[b], bone->inverse_bind);
		memcpy(rest_local[b], translation, sizeof(translation));
		memcpy(rest_local[b] + 3, rotation, sizeof(rotation));
	}

	// Actions: as asked, or one for each clip, and idle and walk where the
	// clips do not have them (the game asks for those two)
	if (options->action_count > 0)
	{
		for (int a = 0; a < options->action_count; a++)
		{
			const action_option_t *wanted = &options->actions[a];
			int clip = find_clip(data, wanted->clip);

			if (clip < 0 || clip >= clip_count)
			{
				fprintf(stderr, "No clip '%s' for the action '%s' (--list shows them)\n", wanted->clip, wanted->name);
				return 1;
			}
			memset(&actions[action_count], 0, sizeof(actions[0]));
			snprintf(actions[action_count].name, sizeof(actions[0].name), "%s", wanted->name);
			actions[action_count].clip = clip;
			{
				const char *clip_name = data->animations[clip].name != NULL ? data->animations[clip].name : "";
				bool loop = wanted->loop < 0 ? clip_name[0] == '\0' || guess_loop(clip_name) || guess_loop(wanted->name) : wanted->loop == 1;

				actions[action_count].flags = loop ? ACTOR_ACTION_LOOP : 0u;
			}
			actions[action_count].duration_seconds = wanted->duration_seconds;
			action_count++;
		}
	}
	else
	{
		bool has_idle = false;
		bool has_walk = false;
		int walk_like = -1;

		for (int c = 0; c < clip_count && action_count < MAX_ACTIONS - 2; c++)
		{
			const char *clip_name = data->animations[c].name != NULL ? data->animations[c].name : "";
			actor_file_action_t *action = &actions[action_count++];

			memset(action, 0, sizeof(*action));
			action_name_of(clip_name, action->name, sizeof(action->name));
			if (action->name[0] == '\0')
			{
				snprintf(action->name, sizeof(action->name), "clip_%d", c);
			}
			action->clip = c;
			// (a clip without a name is most often the model's one loop)
			action->flags = clip_name[0] == '\0' || guess_loop(clip_name) ? ACTOR_ACTION_LOOP : 0u;
			has_idle |= strcmp(action->name, "idle") == 0;
			has_walk |= strcmp(action->name, "walk") == 0;
			if (walk_like < 0 && strstr(action->name, "walk") != NULL)
			{
				walk_like = c;
			}
		}
		if (clip_count > 0 && !has_walk && walk_like >= 0)
		{
			memset(&actions[action_count], 0, sizeof(actions[0]));
			snprintf(actions[action_count].name, sizeof(actions[0].name), "walk");
			actions[action_count].clip = walk_like;
			actions[action_count].flags = ACTOR_ACTION_LOOP;
			action_count++;
		}
		if (clip_count > 0 && !has_idle)
		{
			memset(&actions[action_count], 0, sizeof(actions[0]));
			snprintf(actions[action_count].name, sizeof(actions[0].name), "idle");
			actions[action_count].clip = 0;
			actions[action_count].flags = ACTOR_ACTION_LOOP;
			action_count++;
		}
	}

	// The texture, and its shading maps
	pack_atlas(surfaces, gathered, layers, &atlas_width, &atlas_height);
	for (int s = 0; s < surfaces->count; s++)
	{
		cut_out |= surfaces->surfaces[s].cut_out;
	}
	for (int layer = 0; layer < LAYER_COUNT; layer++)
	{
		if (layers[layer] == NULL)
		{
			continue;
		}
		compressed[layer] = compress_pixels(layers[layer], atlas_width, atlas_height, &compressed_size[layer]);
		if (compressed[layer] == NULL)
		{
			fprintf(stderr, "Cannot compress the texture\n");
			return 1;
		}
	}

	memset(&header, 0, sizeof(header));
	memcpy(header.magic, ACTOR_FILE_MAGIC, 4);
	header.version = ACTOR_FILE_VERSION;
	header.bone_count = (uint32_t)skeleton->bone_count;
	header.vertex_count = (uint32_t)gathered->vertex_count;
	header.index_count = (uint32_t)gathered->index_count;
	header.clip_count = (uint32_t)clip_count;
	header.action_count = (uint32_t)action_count;
	header.texture_width = (uint32_t)atlas_width;
	header.texture_height = (uint32_t)atlas_height;
	header.texture_flags = cut_out ? ACTOR_TEXTURE_ALPHA_TESTED : 0u;
	header.texture_compressed_size = compressed_size[LAYER_BASE];
	header.normal_compressed_size = compressed_size[LAYER_NORMAL];
	header.orm_compressed_size = compressed_size[LAYER_ORM];
	header.emissive_compressed_size = compressed_size[LAYER_EMISSIVE];
	header.extra_flags = ACTOR_FILE_HAS_TANGENTS;
	rest_bounds(gathered, header.bounds_minimum, header.bounds_maximum);

	output = fopen(options->output, "wb");
	if (output == NULL)
	{
		fprintf(stderr, "Cannot write '%s'\n", options->output);
		return 1;
	}
	write_or_fail(output, &header, sizeof(header), &ok);
	write_or_fail(output, bones, sizeof(actor_file_bone_t) * (size_t)skeleton->bone_count, &ok);
	for (int v = 0; v < gathered->vertex_count && ok; v++)
	{
		const gathered_vertex_t *source = &gathered->vertices[v];
		actor_file_vertex_t vertex;
		float total = 0.0f;
		int byte_sum = 0;
		int heaviest = 0;

		memset(&vertex, 0, sizeof(vertex));
		memcpy(vertex.position, source->position, sizeof(vertex.position));
		memcpy(vertex.normal, source->normal, sizeof(vertex.normal));
		memcpy(vertex.uv, source->uv, sizeof(vertex.uv));
		for (int k = 0; k < ACTOR_FILE_INFLUENCES; k++)
		{
			total += source->weights[k];
		}
		for (int k = 0; k < ACTOR_FILE_INFLUENCES; k++)
		{
			vertex.bones[k] = (uint8_t)source->bones[k];
			vertex.weights[k] = total > 0.0f ? (uint8_t)lroundf(source->weights[k] / total * 255.0f) : (k == 0 ? 255 : 0);
			byte_sum += vertex.weights[k];
			if (vertex.weights[k] > vertex.weights[heaviest])
			{
				heaviest = k;
			}
		}
		vertex.weights[heaviest] = (uint8_t)(vertex.weights[heaviest] + (255 - byte_sum));
		write_or_fail(output, &vertex, sizeof(vertex), &ok);
	}
	write_or_fail(output, gathered->indices, sizeof(uint32_t) * (size_t)gathered->index_count, &ok);
	for (int c = 0; c < clip_count && ok; c++)
	{
		const cgltf_animation *animation = &data->animations[c];
		actor_file_clip_t clip;
		float duration = animation_duration(animation);

		memset(&clip, 0, sizeof(clip));
		snprintf(clip.name, sizeof(clip.name), "%s", animation->name != NULL ? animation->name : "clip");
		clip.duration_seconds = duration;
		clip.frame_count = (uint32_t)ceilf(duration * SAMPLES_PER_SECOND) + 1u;
		if (clip.frame_count < 2)
		{
			clip.frame_count = 2;
		}
		write_or_fail(output, &clip, sizeof(clip), &ok);
		for (int b = 0; b < skeleton->bone_count; b++)
		{
			memcpy(previous_rotation[b], rest_local[b] + 3, sizeof(previous_rotation[b]));
		}
		for (uint32_t frame = 0; frame < clip.frame_count && ok; frame++)
		{
			float time_seconds = duration * (float)frame / (float)(clip.frame_count - 1);

			animated_poses(model, animation, time_seconds, poses);
			compute_worlds(model, poses, worlds);
			for (int b = 0; b < skeleton->bone_count; b++)
			{
				actor_file_key_t key;
				float rigid[16];
				float local[16];

				rigid_matrix(worlds[skeleton->node_of_bone[b]], rigid);
				if (skeleton->parent[b] >= 0)
				{
					float parent_rigid[16];
					float inverse_parent[16];

					rigid_matrix(worlds[skeleton->node_of_bone[skeleton->parent[b]]], parent_rigid);
					matrix_inverse(inverse_parent, parent_rigid);
					matrix_multiply(local, inverse_parent, rigid);
				}
				else
				{
					memcpy(local, rigid, sizeof(local));
				}
				matrix_rigid_part(local, key.translation, key.rotation);
				if (options->in_place && skeleton->parent[b] < 0)
				{
					// Up and down it may go; forward and sideways it stays
					key.translation[0] = rest_local[b][0];
					key.translation[2] = rest_local[b][2];
				}
				// The same hemisphere as the frame before, so the game's
				// blending takes the short way
				if (key.rotation[0] * previous_rotation[b][0] + key.rotation[1] * previous_rotation[b][1]
					+ key.rotation[2] * previous_rotation[b][2] + key.rotation[3] * previous_rotation[b][3] < 0.0f)
				{
					for (int k = 0; k < 4; k++)
					{
						key.rotation[k] = -key.rotation[k];
					}
				}
				memcpy(previous_rotation[b], key.rotation, sizeof(previous_rotation[b]));
				write_or_fail(output, &key, sizeof(key), &ok);
			}
		}
	}
	write_or_fail(output, actions, sizeof(actor_file_action_t) * (size_t)action_count, &ok);
	for (int layer = 0; layer < LAYER_COUNT; layer++)
	{
		write_or_fail(output, compressed[layer], (size_t)compressed_size[layer], &ok);
	}
	for (int v = 0; v < gathered->vertex_count && ok; v++)
	{
		write_or_fail(output, gathered->vertices[v].tangent, sizeof(float) * 4, &ok);
	}
	if (fclose(output) != 0)
	{
		ok = false;
	}
	for (int layer = 0; layer < LAYER_COUNT; layer++)
	{
		free(compressed[layer]);
		free(layers[layer]);
	}
	free(poses);
	free(worlds);
	if (!ok)
	{
		fprintf(stderr, "Writing '%s' failed\n", options->output);
		return 1;
	}
	printf("%s: %d bones, %d vertices, %d triangles, %d clips, %d actions, texture %dx%d%s; maps:%s orm%s\n", options->output,
		skeleton->bone_count, gathered->vertex_count, gathered->index_count / 3, clip_count, action_count, atlas_width, atlas_height,
		cut_out ? " (cut out)" : "", compressed_size[LAYER_NORMAL] > 0 ? " normal" : "",
		compressed_size[LAYER_EMISSIVE] > 0 ? " emissive" : "");
	for (int a = 0; a < action_count; a++)
	{
		printf("  action %-16s clip %d '%s' %s\n", actions[a].name, actions[a].clip,
			data->animations[actions[a].clip].name != NULL ? data->animations[actions[a].clip].name : "",
			(actions[a].flags & ACTOR_ACTION_LOOP) != 0 ? "loop" : "once");
	}
	if (skeleton->skin == NULL && data->animations_count > 0)
	{
		printf("  warning: the model has animations but no skin: they move whole nodes, which the actor does not (left out)\n");
	}
	return 0;
}

// --- .e3d -----------------------------------------------------------------

typedef struct
{
	uint8_t *data;
	size_t size;
	size_t capacity;
} memory_t;

static void memory_write(void *context, void *data, int size)
{
	memory_t *memory = context;

	if (memory->size + (size_t)size > memory->capacity)
	{
		memory->capacity = (memory->size + (size_t)size) * 2;
		memory->data = realloc(memory->data, memory->capacity);
	}
	memcpy(memory->data + memory->size, data, (size_t)size);
	memory->size += (size_t)size;
}

static void put_u32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
	at[2] = (uint8_t)(value >> 16);
	at[3] = (uint8_t)(value >> 24);
}

static void put_f32(uint8_t *at, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, 4);
	put_u32(at, bits);
}

// Game axes (Y up) to the .e3d's (Z up): (x, y, z) -> (x, -z, y), the
// inverse of what object_model.c does on the way in
static void game_to_e3d(const float game[3], float out[3])
{
	out[0] = game[0];
	out[1] = -game[2];
	out[2] = game[1];
}

static int write_object(const options_t *options, surfaces_t *surfaces, const gathered_t *gathered)
{
	// The layout object_model.c reads: version 1.1, full floats, normals,
	// 32 bit indices
	enum { HEADER = 28, E3D_HEADER = 40, VERTEX = 32, MATERIAL = 172 };
	int material_count = 0;
	int surface_material[MAX_SURFACES];
	uint32_t *sorted;
	int sorted_count = 0;
	int range_start[E3D_MAX_MATERIALS];
	int range_count[E3D_MAX_MATERIALS];
	float range_minimum[E3D_MAX_MATERIALS][3];
	float range_maximum[E3D_MAX_MATERIALS][3];
	uint32_t range_low[E3D_MAX_MATERIALS];
	uint32_t range_high[E3D_MAX_MATERIALS];
	char stem[PATH_LENGTH];
	char directory[PATH_LENGTH];
	size_t size;
	uint8_t *file;
	FILE *output;
	bool ok = true;

	// Materials: the surfaces with triangles, in order
	for (int s = 0; s < surfaces->count; s++)
	{
		surface_material[s] = -1;
	}
	for (int i = 0; i < gathered->index_count; i += 3)
	{
		int s = gathered->vertices[gathered->indices[i]].surface;

		if (surface_material[s] < 0)
		{
			if (material_count == E3D_MAX_MATERIALS)
			{
				fprintf(stderr, "More than %d materials: an object takes %d at most\n", E3D_MAX_MATERIALS, E3D_MAX_MATERIALS);
				return 1;
			}
			surface_material[s] = material_count++;
		}
	}
	sorted = malloc(sizeof(uint32_t) * ((size_t)(gathered->index_count > 0 ? gathered->index_count : 0) + 1));
	for (int m = 0; m < material_count; m++)
	{
		range_start[m] = sorted_count;
		range_low[m] = UINT32_MAX;
		range_high[m] = 0;
		for (int k = 0; k < 3; k++)
		{
			range_minimum[m][k] = 1e30f;
			range_maximum[m][k] = -1e30f;
		}
		for (int i = 0; i < gathered->index_count; i += 3)
		{
			if (surface_material[gathered->vertices[gathered->indices[i]].surface] != m)
			{
				continue;
			}
			for (int k = 0; k < 3; k++)
			{
				uint32_t index = gathered->indices[i + k];
				float corner[3];

				sorted[sorted_count++] = index;
				range_low[m] = index < range_low[m] ? index : range_low[m];
				range_high[m] = index > range_high[m] ? index : range_high[m];
				game_to_e3d(gathered->vertices[index].rest_position, corner);
				for (int e = 0; e < 3; e++)
				{
					range_minimum[m][e] = fminf(range_minimum[m][e], corner[e]);
					range_maximum[m][e] = fmaxf(range_maximum[m][e], corner[e]);
				}
			}
		}
		range_count[m] = sorted_count - range_start[m];
	}

	// The textures, as PNG files next to the object
	snprintf(directory, sizeof(directory), "%s", options->output);
	{
		char *slash = strrchr(directory, '/');
		const char *name = slash != NULL ? slash + 1 : options->output;
		char *dot;

		snprintf(stem, sizeof(stem), "%s", name);
		dot = strrchr(stem, '.');
		if (dot != NULL)
		{
			*dot = '\0';
		}
		if (slash != NULL)
		{
			slash[1] = '\0';
		}
		else
		{
			directory[0] = '\0';
		}
	}

	size = HEADER + E3D_HEADER + (size_t)gathered->vertex_count * VERTEX + (size_t)sorted_count * 4 + (size_t)material_count * MATERIAL;
	file = calloc(size, 1);
	memcpy(file, "e3dx", 4);
	file[4] = 1;
	file[5] = 1;
	put_u32(file + 24, HEADER);
	{
		uint8_t *header = file + HEADER;
		uint32_t vertex_offset = HEADER + E3D_HEADER;
		uint32_t index_offset = vertex_offset + (uint32_t)gathered->vertex_count * VERTEX;
		uint32_t material_offset = index_offset + (uint32_t)sorted_count * 4u;

		put_u32(header + 0, (uint32_t)gathered->vertex_count);
		put_u32(header + 4, VERTEX);
		put_u32(header + 8, vertex_offset);
		put_u32(header + 12, (uint32_t)sorted_count);
		put_u32(header + 16, 4);
		put_u32(header + 20, index_offset);
		put_u32(header + 24, (uint32_t)material_count);
		put_u32(header + 28, MATERIAL);
		put_u32(header + 32, material_offset);
		header[36] = 0x01;                 // normals
		header[37] = 0x00;                 // all full floats, 32 bit indices
		for (int v = 0; v < gathered->vertex_count; v++)
		{
			uint8_t *vertex = file + vertex_offset + (size_t)v * VERTEX;
			const gathered_vertex_t *source = &gathered->vertices[v];
			float normal[3];
			float position[3];

			game_to_e3d(source->rest_normal, normal);
			game_to_e3d(source->rest_position, position);
			put_f32(vertex + 0, source->uv[0]);
			put_f32(vertex + 4, source->uv[1]);
			for (int k = 0; k < 3; k++)
			{
				put_f32(vertex + 8 + k * 4, normal[k]);
				put_f32(vertex + 20 + k * 4, position[k]);
			}
		}
		for (int i = 0; i < sorted_count; i++)
		{
			put_u32(file + index_offset + (size_t)i * 4, sorted[i]);
		}
		for (int s = 0; s < surfaces->count; s++)
		{
			int m = surface_material[s];
			uint8_t *material;
			char texture_name[160];
			char texture_path[PATH_LENGTH * 2];
			memory_t png = { NULL, 0, 0 };
			FILE *texture_file;

			if (m < 0)
			{
				continue;
			}
			material = file + material_offset + (size_t)m * MATERIAL;
			if (material_count == 1)
			{
				snprintf(texture_name, sizeof(texture_name), "%s.png", stem);
			}
			else
			{
				snprintf(texture_name, sizeof(texture_name), "%s_%d.png", stem, m);
			}
			if (strlen(texture_name) > 127)
			{
				fprintf(stderr, "The texture name '%s' is too long for an .e3d (127)\n", texture_name);
				return 1;
			}
			put_u32(material + 0, surfaces->surfaces[s].cut_out ? 1u : 0u);
			memcpy(material + 4, texture_name, strlen(texture_name));
			for (int k = 0; k < 3; k++)
			{
				put_f32(material + 132 + k * 4, range_minimum[m][k]);
				put_f32(material + 144 + k * 4, range_maximum[m][k]);
			}
			put_u32(material + 156, range_low[m]);
			put_u32(material + 160, range_high[m]);
			put_u32(material + 164, (uint32_t)range_start[m]);
			put_u32(material + 168, (uint32_t)range_count[m]);
			stbi_write_png_to_func(memory_write, &png, surfaces->surfaces[s].width, surfaces->surfaces[s].height, 4,
				surfaces->surfaces[s].pixels, surfaces->surfaces[s].width * 4);
			snprintf(texture_path, sizeof(texture_path), "%s%s", directory, texture_name);
			texture_file = fopen(texture_path, "wb");
			if (texture_file == NULL || fwrite(png.data, 1, png.size, texture_file) != png.size)
			{
				fprintf(stderr, "Cannot write '%s'\n", texture_path);
				ok = false;
			}
			if (texture_file != NULL)
			{
				fclose(texture_file);
			}
			printf("  texture %s: %dx%d%s\n", texture_path, surfaces->surfaces[s].width, surfaces->surfaces[s].height,
				surfaces->surfaces[s].cut_out ? " (cut out)" : "");
			free(png.data);
		}
	}
	output = fopen(options->output, "wb");
	if (output == NULL)
	{
		fprintf(stderr, "Cannot write '%s'\n", options->output);
		return 1;
	}
	write_or_fail(output, file, size, &ok);
	if (fclose(output) != 0)
	{
		ok = false;
	}
	free(file);
	free(sorted);
	if (!ok)
	{
		return 1;
	}
	printf("%s: %d vertices, %d triangles, %d materials\n", options->output, gathered->vertex_count, sorted_count / 3, material_count);
	return 0;
}

// --- .pobj ------------------------------------------------------------------

// Whether name (or its short form, as written) is one of the comma
// separated list
static bool listed_name(const char *list, const char *name, const char *short_name)
{
	const char *at = list;

	while (at != NULL && *at != '\0')
	{
		const char *end = strchr(at, ',');
		size_t length = end != NULL ? (size_t)(end - at) : strlen(at);

		if (length > 0 && ((name != NULL && strlen(name) == length && strncmp(name, at, length) == 0)
			|| (short_name != NULL && strlen(short_name) == length && strncmp(short_name, at, length) == 0)))
		{
			return true;
		}
		at = end != NULL ? end + 1 : NULL;
	}
	return false;
}

static int write_own_object(const options_t *options, surfaces_t *surfaces, const gathered_t *gathered)
{
	object_file_header_t header;
	object_file_material_t materials[OBJECT_FILE_MAX_MATERIALS];
	int surface_material[MAX_SURFACES];
	int material_surface[OBJECT_FILE_MAX_MATERIALS];
	int material_count = 0;
	uint32_t *sorted = malloc(sizeof(uint32_t) * ((size_t)(gathered->index_count > 0 ? gathered->index_count : 0) + 1));
	int sorted_count = 0;
	// The textures: their pixels, kinds and sizes, in the order written
	const uint8_t *texture_pixels[OBJECT_FILE_MAX_MATERIALS * 4];
	object_file_texture_t textures[OBJECT_FILE_MAX_MATERIALS * 4];
	unsigned char *compressed[OBJECT_FILE_MAX_MATERIALS * 4];
	int texture_count = 0;
	bool any_normal = false;
	bool any_emissive = false;
	FILE *output;
	bool ok = true;

	// Materials: the surfaces with triangles, in order; the triangles
	// sorted into their ranges
	for (int s = 0; s < surfaces->count; s++)
	{
		surface_material[s] = -1;
	}
	for (int i = 0; i + 2 < gathered->index_count; i += 3)
	{
		int s = gathered->vertices[gathered->indices[i]].surface;

		if (surface_material[s] < 0)
		{
			if (material_count == OBJECT_FILE_MAX_MATERIALS)
			{
				fprintf(stderr, "More than %d materials: an object takes %d at most\n", OBJECT_FILE_MAX_MATERIALS, OBJECT_FILE_MAX_MATERIALS);
				free(sorted);
				return 1;
			}
			material_surface[material_count] = s;
			surface_material[s] = material_count++;
		}
	}
	memset(materials, 0, sizeof(materials));
	for (int m = 0; m < material_count; m++)
	{
		const surface_t *surface = &surfaces->surfaces[material_surface[m]];
		object_file_material_t *material = &materials[m];

		material->index_offset = (uint32_t)sorted_count;
		for (int i = 0; i + 2 < gathered->index_count; i += 3)
		{
			if (gathered->vertices[gathered->indices[i]].surface == material_surface[m])
			{
				for (int k = 0; k < 3; k++)
				{
					sorted[sorted_count++] = gathered->indices[i + k];
				}
			}
		}
		material->index_count = (uint32_t)sorted_count - material->index_offset;
		snprintf(material->name, sizeof(material->name), "%s",
			surface->material != NULL && surface->material->name != NULL ? surface->material->name : "material");
		material->flags = (surface->alpha_mode != cgltf_alpha_mode_opaque ? OBJECT_MATERIAL_ALPHA_TESTED : 0u)
			| (surface->double_sided ? OBJECT_MATERIAL_DOUBLE_SIDED : 0u);
		if (listed_name(options->no_collide, surface->material != NULL ? surface->material->name : NULL, material->name))
		{
			material->flags |= OBJECT_MATERIAL_PASSABLE;
		}
		// The factors are in the textures already
		for (int k = 0; k < 4; k++)
		{
			material->base_color[k] = 1.0f;
		}
		material->metallic = 1.0f;
		material->roughness = 1.0f;
		material->normal_scale = 1.0f;
		material->occlusion_strength = 1.0f;
		material->alpha_cutoff = 0.5f;
		material->normal_texture = -1;
		material->emissive_texture = -1;
		material->base_color_texture = texture_count;
		texture_pixels[texture_count] = surface->pixels;
		textures[texture_count].kind = OBJECT_TEXTURE_COLOR;
		texture_count++;
		material->orm_texture = texture_count;
		texture_pixels[texture_count] = surface->orm_pixels;
		textures[texture_count].kind = OBJECT_TEXTURE_DATA;
		texture_count++;
		if (surface->normal_pixels != NULL)
		{
			material->normal_texture = texture_count;
			texture_pixels[texture_count] = surface->normal_pixels;
			textures[texture_count].kind = OBJECT_TEXTURE_DATA;
			texture_count++;
			any_normal = true;
		}
		if (surface->emissive_pixels != NULL)
		{
			for (int k = 0; k < 3; k++)
			{
				material->emissive[k] = 1.0f;
			}
			material->emissive_texture = texture_count;
			texture_pixels[texture_count] = surface->emissive_pixels;
			textures[texture_count].kind = OBJECT_TEXTURE_COLOR;
			texture_count++;
			any_emissive = true;
		}
	}
	for (int t = 0; t < texture_count; t++)
	{
		int m = 0;

		// Every texture of a material is its base color's size
		for (int k = 0; k < material_count; k++)
		{
			if (materials[k].base_color_texture <= t)
			{
				m = k;
			}
		}
		textures[t].width = (uint32_t)surfaces->surfaces[material_surface[m]].width;
		textures[t].height = (uint32_t)surfaces->surfaces[material_surface[m]].height;
		compressed[t] = compress_pixels(texture_pixels[t], (int)textures[t].width, (int)textures[t].height, &textures[t].compressed_size);
		if (compressed[t] == NULL)
		{
			fprintf(stderr, "Cannot compress a texture\n");
			return 1;
		}
	}

	memset(&header, 0, sizeof(header));
	memcpy(header.magic, OBJECT_FILE_MAGIC, 4);
	header.version = OBJECT_FILE_VERSION;
	header.vertex_count = (uint32_t)gathered->vertex_count;
	header.index_count = (uint32_t)sorted_count;
	header.material_count = (uint32_t)material_count;
	header.texture_count = (uint32_t)texture_count;
	rest_bounds(gathered, header.bounds_minimum, header.bounds_maximum);

	output = fopen(options->output, "wb");
	if (output == NULL)
	{
		fprintf(stderr, "Cannot write '%s'\n", options->output);
		return 1;
	}
	write_or_fail(output, &header, sizeof(header), &ok);
	for (int v = 0; v < gathered->vertex_count && ok; v++)
	{
		const gathered_vertex_t *source = &gathered->vertices[v];
		object_file_vertex_t vertex;

		memcpy(vertex.position, source->rest_position, sizeof(vertex.position));
		memcpy(vertex.normal, source->rest_normal, sizeof(vertex.normal));
		memcpy(vertex.tangent, source->rest_tangent, sizeof(vertex.tangent));
		memcpy(vertex.uv, source->uv, sizeof(vertex.uv));
		write_or_fail(output, &vertex, sizeof(vertex), &ok);
	}
	write_or_fail(output, sorted, sizeof(uint32_t) * (size_t)sorted_count, &ok);
	write_or_fail(output, materials, sizeof(object_file_material_t) * (size_t)material_count, &ok);
	for (int t = 0; t < texture_count; t++)
	{
		write_or_fail(output, &textures[t], sizeof(textures[t]), &ok);
		write_or_fail(output, compressed[t], (size_t)textures[t].compressed_size, &ok);
		free(compressed[t]);
	}
	if (fclose(output) != 0)
	{
		ok = false;
	}
	free(sorted);
	if (!ok)
	{
		fprintf(stderr, "Writing '%s' failed\n", options->output);
		return 1;
	}
	printf("%s: %d vertices, %d triangles, %d materials, %d textures; maps: base, orm%s%s\n", options->output, gathered->vertex_count,
		sorted_count / 3, material_count, texture_count, any_normal ? ", normal" : "", any_emissive ? ", emissive" : "");
	for (int m = 0; m < material_count; m++)
	{
		const surface_t *surface = &surfaces->surfaces[material_surface[m]];

		printf("  material %-24s %dx%d%s%s%s\n", materials[m].name, surface->width, surface->height,
			(materials[m].flags & OBJECT_MATERIAL_ALPHA_TESTED) != 0 ? " cut out" : "",
			(materials[m].flags & OBJECT_MATERIAL_DOUBLE_SIDED) != 0 ? " double sided" : "",
			(materials[m].flags & (OBJECT_MATERIAL_ALPHA_TESTED | OBJECT_MATERIAL_PASSABLE)) != 0 ? " passable" : "");
	}
	return 0;
}

// ---------------------------------------------------------------------------

int convert_gltf(int argument_count, char **arguments)
{
	options_t *options = calloc(1, sizeof(options_t));
	model_t model;
	skeleton_t *skeleton = calloc(1, sizeof(skeleton_t));
	surfaces_t *surfaces = calloc(1, sizeof(surfaces_t));
	gathered_t gathered;
	float (*worlds)[16];
	float minimum[3];
	float maximum[3];
	const cgltf_skin *skin = NULL;
	bool as_object;
	int result;

	if (!parse_options(argument_count, arguments, options))
	{
		usage();
		return 2;
	}
	if (!model_open(&model, options->input))
	{
		model_close(&model);
		return 1;
	}
	if (options->list)
	{
		list_model(&model);
		model_close(&model);
		return 0;
	}
	as_object = ends_with(options->output, ".e3d") || ends_with(options->output, ".pobj");
	if (!as_object && !ends_with(options->output, ".pact"))
	{
		fprintf(stderr, "The output must be a .pact (an actor), a .pobj (an object) or an .e3d (an Eternal Lands object)\n");
		model_close(&model);
		return 2;
	}
	// The skin: the first one a mesh uses
	for (cgltf_size n = 0; n < model.data->nodes_count && skin == NULL; n++)
	{
		if (model.data->nodes[n].mesh != NULL && model.data->nodes[n].skin != NULL)
		{
			skin = model.data->nodes[n].skin;
		}
	}
	if (!make_skeleton(&model, skin, skeleton))
	{
		model_close(&model);
		return 1;
	}
	surfaces->max_size = options->max_texture;
	worlds = calloc(model.data->nodes_count + 1, sizeof(float[16]));

	// Over everything: the up axis, the turn and the scale; then once more
	// with the height and the ground, which need the size it came out at
	for (int pass = 0; pass < 2; pass++)
	{
		float up[16];
		float turn[16];
		float scale[16];
		float place[16];
		const float zero[3] = { 0.0f, 0.0f, 0.0f };
		float factor = options->scale > 0.0f ? options->scale : 1.0f;
		float offset[3] = { 0.0f, 0.0f, 0.0f };

		matrix_identity(up);
		if (options->z_up)
		{
			// Z up to Y up: (x, y, z) -> (x, z, -y)
			const float half_turn_x[4] = { -0.70710678f, 0.0f, 0.0f, 0.70710678f };
			const float one[3] = { 1.0f, 1.0f, 1.0f };

			matrix_from_trs(up, zero, half_turn_x, one);
		}
		{
			float angle = options->turn_degrees * 3.14159265f / 180.0f;
			const float around_y[4] = { 0.0f, sinf(angle * 0.5f), 0.0f, cosf(angle * 0.5f) };
			const float one[3] = { 1.0f, 1.0f, 1.0f };

			matrix_from_trs(turn, zero, around_y, one);
		}
		if (pass == 1)
		{
			float tall = maximum[1] - minimum[1];

			if (options->height > 0.0f && tall > 1e-9f)
			{
				factor *= options->height / tall;
			}
			if (options->ground)
			{
				float relative = options->height > 0.0f && tall > 1e-9f ? options->height / tall : 1.0f;

				offset[0] = -(minimum[0] + maximum[0]) * 0.5f * relative;
				offset[1] = -minimum[1] * relative;
				offset[2] = -(minimum[2] + maximum[2]) * 0.5f * relative;
			}
		}
		{
			const float no_turn[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			const float scales[3] = { factor, factor, factor };
			const float one[3] = { 1.0f, 1.0f, 1.0f };

			matrix_from_trs(scale, zero, no_turn, scales);
			matrix_from_trs(place, offset, no_turn, one);
		}
		matrix_multiply(model.root, turn, up);
		matrix_multiply(model.root, scale, model.root);
		matrix_multiply(model.root, place, model.root);

		compute_worlds(&model, model.rest, worlds);
		bind_skeleton(skeleton, worlds);
		memset(&gathered, 0, sizeof(gathered));
		free_surfaces(surfaces);
		if (!gather(&model, skeleton, worlds, surfaces, &gathered))
		{
			model_close(&model);
			return 1;
		}
		rest_bounds(&gathered, minimum, maximum);
		if (pass == 0 && options->height <= 0.0f && !options->ground)
		{
			break;
		}
		if (pass == 0)
		{
			free_gathered(&gathered);
		}
	}
	if (surfaces->image_failed)
	{
		printf("  warning: a texture could not be read; its material is its color alone\n");
	}
	if (gathered.skipped_primitives > 0)
	{
		printf("  warning: %d primitives are not triangles (points, lines) or have no positions: left out\n", gathered.skipped_primitives);
	}
	if (gathered.dropped_influences > 0)
	{
		printf("  %d influences beyond four per vertex dropped\n", gathered.dropped_influences);
	}
	if (ends_with(options->output, ".pobj"))
	{
		result = write_own_object(options, surfaces, &gathered);
	}
	else
	{
		result = as_object ? write_object(options, surfaces, &gathered) : write_actor(options, &model, skeleton, surfaces, &gathered);
	}
	if (result == 0)
	{
		printf("  size %.2f x %.2f x %.2f m (x, y up, z), from (%.2f %.2f %.2f)\n", (double)(maximum[0] - minimum[0]),
			(double)(maximum[1] - minimum[1]), (double)(maximum[2] - minimum[2]), (double)minimum[0], (double)minimum[1],
			(double)minimum[2]);
	}
	free_gathered(&gathered);
	free_surfaces(surfaces);
	free(worlds);
	free(surfaces);
	free(skeleton);
	free(options);
	model_close(&model);
	return result;
}
