// The value noise's lattice: one random byte per grid point, repeating
// every NOISE_LATTICE_SIZE points both ways. The shaders read it from the
// variation texture's green channel (materials.c puts it there; blue: the
// same moved by NOISE_LATTICE_SLICE_X, _Y, the next slice up for the 3D
// noise), the CPU from here (material_map.c: the patches where the
// shaders put them).
//
// A value noise from it is one bilinear read: the point's cell plus the
// smoothstep of where in the cell it is, plus half a texel (the filter
// then blends the four corners by exactly the smoothstep's weights). The
// 3D one: slice z of the lattice is the 2D one moved by z times
// (SLICE_X, SLICE_Y); green and blue give slices z and z + 1 in the same
// read, blended by the third smoothstep.

#ifndef NOISE_LATTICE_H
#define NOISE_LATTICE_H

#include <stdint.h>

enum
{
	NOISE_LATTICE_SIZE = 512,      // materials.c's VARIATION_TEXTURE_SIZE
	NOISE_LATTICE_SLICE_X = 37,
	NOISE_LATTICE_SLICE_Y = 17
};

// The lattice (NOISE_LATTICE_SIZE squared bytes, row by row), made on
// first use, the same every run; NULL if there is no memory
const uint8_t *noise_lattice(void);

// The 2D value noise at (x, y) in lattice cells (0..1), as the shaders'
// value_noise
float noise_lattice_value(float x, float y);

#endif
