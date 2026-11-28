// Small vector, quaternion and matrix helpers: see wb_math.h

#include "wb_math.h"

#include <math.h>
#include <string.h>

void wb_vector_subtract(const float a[3], const float b[3], float out[3])
{
	out[0] = a[0] - b[0];
	out[1] = a[1] - b[1];
	out[2] = a[2] - b[2];
}

void wb_vector_cross(const float a[3], const float b[3], float out[3])
{
	float result[3];

	result[0] = a[1] * b[2] - a[2] * b[1];
	result[1] = a[2] * b[0] - a[0] * b[2];
	result[2] = a[0] * b[1] - a[1] * b[0];
	memcpy(out, result, sizeof(result));
}

float wb_vector_dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float wb_vector_length(const float vector[3])
{
	return sqrtf(wb_vector_dot(vector, vector));
}

void wb_vector_normalize(float vector[3])
{
	float length = wb_vector_length(vector);

	if (length > 0.0f)
	{
		vector[0] /= length;
		vector[1] /= length;
		vector[2] /= length;
	}
}

void wb_quaternion_rotate(const float rotation[4], const float vector[3], float out[3])
{
	float twice_cross[3];
	float second[3];
	float result[3];

	wb_vector_cross(rotation, vector, twice_cross);
	for (int k = 0; k < 3; k++)
	{
		twice_cross[k] *= 2.0f;
	}
	wb_vector_cross(rotation, twice_cross, second);
	for (int k = 0; k < 3; k++)
	{
		result[k] = vector[k] + rotation[3] * twice_cross[k] + second[k];
	}
	memcpy(out, result, sizeof(result));
}

void wb_matrix_multiply(const float a[16], const float b[16], float out[16])
{
	float result[16];

	for (int column = 0; column < 4; column++)
	{
		for (int row = 0; row < 4; row++)
		{
			float sum = 0.0f;

			for (int k = 0; k < 4; k++)
			{
				sum += a[k * 4 + row] * b[column * 4 + k];
			}
			result[column * 4 + row] = sum;
		}
	}
	memcpy(out, result, sizeof(result));
}

void wb_matrix_look_at(const float eye[3], const float target[3], float out[16])
{
	float forward[3];
	float right[3];
	float up[3];
	const float world_up[3] = { 0.0f, 1.0f, 0.0f };

	wb_vector_subtract(target, eye, forward);
	wb_vector_normalize(forward);
	wb_vector_cross(forward, world_up, right);
	if (wb_vector_length(right) < 1e-5f)
	{
		const float other_up[3] = { 0.0f, 0.0f, 1.0f };

		wb_vector_cross(forward, other_up, right);
	}
	wb_vector_normalize(right);
	wb_vector_cross(right, forward, up);
	memset(out, 0, sizeof(float) * 16);
	out[0] = right[0];
	out[4] = right[1];
	out[8] = right[2];
	out[1] = up[0];
	out[5] = up[1];
	out[9] = up[2];
	out[2] = -forward[0];
	out[6] = -forward[1];
	out[10] = -forward[2];
	out[12] = -wb_vector_dot(right, eye);
	out[13] = -wb_vector_dot(up, eye);
	out[14] = wb_vector_dot(forward, eye);
	out[15] = 1.0f;
}

void wb_matrix_perspective(float vertical_field_of_view_radians, float aspect, float near_distance, float far_distance, float out[16])
{
	float focal = 1.0f / tanf(0.5f * vertical_field_of_view_radians);

	memset(out, 0, sizeof(float) * 16);
	out[0] = focal / aspect;
	out[5] = focal;
	out[10] = far_distance / (near_distance - far_distance);
	out[11] = -1.0f;
	out[14] = near_distance * far_distance / (near_distance - far_distance);
}

void wb_matrix_orthographic(float left, float right, float bottom, float top, float near_distance, float far_distance, float out[16])
{
	memset(out, 0, sizeof(float) * 16);
	out[0] = 2.0f / (right - left);
	out[5] = 2.0f / (top - bottom);
	out[10] = -1.0f / (far_distance - near_distance);
	out[12] = -(right + left) / (right - left);
	out[13] = -(top + bottom) / (top - bottom);
	out[14] = -near_distance / (far_distance - near_distance);
	out[15] = 1.0f;
}
