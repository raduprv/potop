// Small vector and matrix helpers. Header only, everything is static inline.
//
// Conventions (same as OpenGL):
//   - right handed world, Y is up, the terrain grid lies in the X/Z plane;
//   - matrices are column major, element (row r, column c) is m[c * 4 + r];
//   - clip space depth is -1..1.

#ifndef MATH3D_H
#define MATH3D_H

#include <math.h>
#include <stdbool.h>

#define MATH3D_PI 3.14159265358979323846f

typedef struct
{
	float x;
	float y;
	float z;
} vec3_t;

typedef struct
{
	float x;
	float y;
	float z;
	float w;
} vec4_t;

typedef struct
{
	float m[16];
} mat4_t;

// A plane stored as normal (a, b, c) and offset d: a*x + b*y + c*z + d = 0.
// Points with a positive value are on the inside of the frustum.
typedef struct
{
	float a;
	float b;
	float c;
	float d;
} plane_t;

typedef struct
{
	plane_t planes[6];
} frustum_t;

static inline float clamp_float(float value, float minimum, float maximum)
{
	return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static inline int clamp_int(int value, int minimum, int maximum)
{
	return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static inline float lerp_float(float from, float to, float amount)
{
	return from + (to - from) * amount;
}

static inline float smoothstep_float(float edge_low, float edge_high, float value)
{
	float amount = clamp_float((value - edge_low) / (edge_high - edge_low), 0.0f, 1.0f);

	return amount * amount * (3.0f - 2.0f * amount);
}

static inline float degrees_to_radians(float degrees)
{
	return degrees * (MATH3D_PI / 180.0f);
}

static inline float radians_to_degrees(float radians)
{
	return radians * (180.0f / MATH3D_PI);
}

static inline vec3_t vec3_make(float x, float y, float z)
{
	vec3_t result = { x, y, z };

	return result;
}

static inline vec3_t vec3_add(vec3_t left, vec3_t right)
{
	return vec3_make(left.x + right.x, left.y + right.y, left.z + right.z);
}

static inline vec3_t vec3_subtract(vec3_t left, vec3_t right)
{
	return vec3_make(left.x - right.x, left.y - right.y, left.z - right.z);
}

static inline vec3_t vec3_scale(vec3_t vector, float factor)
{
	return vec3_make(vector.x * factor, vector.y * factor, vector.z * factor);
}

static inline float vec3_dot(vec3_t left, vec3_t right)
{
	return left.x * right.x + left.y * right.y + left.z * right.z;
}

static inline vec3_t vec3_cross(vec3_t left, vec3_t right)
{
	return vec3_make(
		left.y * right.z - left.z * right.y,
		left.z * right.x - left.x * right.z,
		left.x * right.y - left.y * right.x);
}

static inline float vec3_length(vec3_t vector)
{
	return sqrtf(vec3_dot(vector, vector));
}

static inline vec3_t vec3_normalize(vec3_t vector)
{
	float length = vec3_length(vector);

	if (length <= 1e-20f)
	{
		return vec3_make(0.0f, 1.0f, 0.0f);
	}
	return vec3_scale(vector, 1.0f / length);
}

static inline mat4_t mat4_identity(void)
{
	mat4_t result = { { 0 } };

	result.m[0] = 1.0f;
	result.m[5] = 1.0f;
	result.m[10] = 1.0f;
	result.m[15] = 1.0f;
	return result;
}

static inline mat4_t mat4_multiply(mat4_t left, mat4_t right)
{
	mat4_t result;

	for (int column = 0; column < 4; column++)
	{
		for (int row = 0; row < 4; row++)
		{
			float sum = 0.0f;

			for (int k = 0; k < 4; k++)
			{
				sum += left.m[k * 4 + row] * right.m[column * 4 + k];
			}
			result.m[column * 4 + row] = sum;
		}
	}
	return result;
}

static inline vec4_t mat4_transform(mat4_t matrix, vec4_t vector)
{
	vec4_t result;

	result.x = matrix.m[0] * vector.x + matrix.m[4] * vector.y + matrix.m[8] * vector.z + matrix.m[12] * vector.w;
	result.y = matrix.m[1] * vector.x + matrix.m[5] * vector.y + matrix.m[9] * vector.z + matrix.m[13] * vector.w;
	result.z = matrix.m[2] * vector.x + matrix.m[6] * vector.y + matrix.m[10] * vector.z + matrix.m[14] * vector.w;
	result.w = matrix.m[3] * vector.x + matrix.m[7] * vector.y + matrix.m[11] * vector.z + matrix.m[15] * vector.w;
	return result;
}

// Standard OpenGL perspective projection.
static inline mat4_t mat4_perspective(float vertical_fov_radians, float aspect_ratio, float near_distance, float far_distance)
{
	mat4_t result = { { 0 } };
	float focal_length = 1.0f / tanf(vertical_fov_radians * 0.5f);

	result.m[0] = focal_length / aspect_ratio;
	result.m[5] = focal_length;
	result.m[10] = (far_distance + near_distance) / (near_distance - far_distance);
	result.m[11] = -1.0f;
	result.m[14] = (2.0f * far_distance * near_distance) / (near_distance - far_distance);
	return result;
}

// Standard OpenGL orthographic projection.
static inline mat4_t mat4_orthographic(float left, float right, float bottom, float top, float near_distance, float far_distance)
{
	mat4_t result = mat4_identity();

	result.m[0] = 2.0f / (right - left);
	result.m[5] = 2.0f / (top - bottom);
	result.m[10] = -2.0f / (far_distance - near_distance);
	result.m[12] = -(right + left) / (right - left);
	result.m[13] = -(top + bottom) / (top - bottom);
	result.m[14] = -(far_distance + near_distance) / (far_distance - near_distance);
	return result;
}

static inline mat4_t mat4_look_at(vec3_t eye, vec3_t target, vec3_t up)
{
	vec3_t forward = vec3_normalize(vec3_subtract(target, eye));
	vec3_t side = vec3_normalize(vec3_cross(forward, up));
	vec3_t true_up = vec3_cross(side, forward);
	mat4_t result = mat4_identity();

	result.m[0] = side.x;
	result.m[4] = side.y;
	result.m[8] = side.z;
	result.m[1] = true_up.x;
	result.m[5] = true_up.y;
	result.m[9] = true_up.z;
	result.m[2] = -forward.x;
	result.m[6] = -forward.y;
	result.m[10] = -forward.z;
	result.m[12] = -vec3_dot(side, eye);
	result.m[13] = -vec3_dot(true_up, eye);
	result.m[14] = vec3_dot(forward, eye);
	return result;
}

// General 4x4 inverse (cofactor expansion). Returns false for a singular
// matrix, in which case the output is left as identity.
static inline bool mat4_inverse(mat4_t matrix, mat4_t *inverse_out)
{
	const float *m = matrix.m;
	float inv[16];
	float determinant;

	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

	determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (fabsf(determinant) < 1e-30f)
	{
		*inverse_out = mat4_identity();
		return false;
	}

	determinant = 1.0f / determinant;
	for (int i = 0; i < 16; i++)
	{
		inverse_out->m[i] = inv[i] * determinant;
	}
	return true;
}

// Extracts the six clip planes from a view-projection matrix
// (Gribb & Hartmann). Planes are normalized so that distances are in world
// units.
static inline frustum_t frustum_from_matrix(mat4_t view_projection)
{
	const float *m = view_projection.m;
	frustum_t frustum;
	// Row r of the matrix is (m[r], m[4 + r], m[8 + r], m[12 + r])
	float row_0[4] = { m[0], m[4], m[8], m[12] };
	float row_1[4] = { m[1], m[5], m[9], m[13] };
	float row_2[4] = { m[2], m[6], m[10], m[14] };
	float row_3[4] = { m[3], m[7], m[11], m[15] };
	const float *rows[3] = { row_0, row_1, row_2 };

	for (int i = 0; i < 3; i++)
	{
		for (int side = 0; side < 2; side++)
		{
			plane_t *plane = &frustum.planes[i * 2 + side];
			float sign = side == 0 ? 1.0f : -1.0f;
			float length;

			plane->a = row_3[0] + sign * rows[i][0];
			plane->b = row_3[1] + sign * rows[i][1];
			plane->c = row_3[2] + sign * rows[i][2];
			plane->d = row_3[3] + sign * rows[i][3];
			length = sqrtf(plane->a * plane->a + plane->b * plane->b + plane->c * plane->c);
			if (length > 0.0f)
			{
				plane->a /= length;
				plane->b /= length;
				plane->c /= length;
				plane->d /= length;
			}
		}
	}
	return frustum;
}

// True if an axis aligned box is at least partly inside the frustum. Uses the
// "positive vertex" test: for each plane only the box corner furthest along
// the plane normal is checked. Conservative (may keep a few invisible boxes
// near frustum corners), which is fine for culling.
static inline bool frustum_contains_box(const frustum_t *frustum, vec3_t box_minimum, vec3_t box_maximum)
{
	for (int i = 0; i < 6; i++)
	{
		const plane_t *plane = &frustum->planes[i];
		float x = plane->a >= 0.0f ? box_maximum.x : box_minimum.x;
		float y = plane->b >= 0.0f ? box_maximum.y : box_minimum.y;
		float z = plane->c >= 0.0f ? box_maximum.z : box_minimum.z;

		if (plane->a * x + plane->b * y + plane->c * z + plane->d < 0.0f)
		{
			return false;
		}
	}
	return true;
}

#endif
