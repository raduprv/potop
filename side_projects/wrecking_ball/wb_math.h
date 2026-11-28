// Small vector, quaternion and matrix helpers. Matrices are 4 x 4, column
// major (as GLSL takes them): element (row, column) is at column * 4 + row.
// The world is right handed, y up, meters.

#ifndef WB_MATH_H
#define WB_MATH_H

void wb_vector_subtract(const float a[3], const float b[3], float out[3]);
void wb_vector_cross(const float a[3], const float b[3], float out[3]);
float wb_vector_dot(const float a[3], const float b[3]);
float wb_vector_length(const float vector[3]);
void wb_vector_normalize(float vector[3]);

// A vector turned by a quaternion (x, y, z, w)
void wb_quaternion_rotate(const float rotation[4], const float vector[3], float out[3]);

void wb_matrix_multiply(const float a[16], const float b[16], float out[16]);
// A view looking from eye at target, y up
void wb_matrix_look_at(const float eye[3], const float target[3], float out[16]);
// Clip space depth 0 (near) to 1 (far), as SDL_GPU wants it
void wb_matrix_perspective(float vertical_field_of_view_radians, float aspect, float near_distance, float far_distance, float out[16]);
void wb_matrix_orthographic(float left, float right, float bottom, float top, float near_distance, float far_distance, float out[16]);

#endif
