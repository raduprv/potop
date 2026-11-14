// A small reader for OpenVDB files (.vdb), for the desktop tools only: one
// float grid (a cloud's density, as EmberGen and the like write it) into a
// dense box of floats. No OpenVDB, no Boost, no C++.
//
// What it reads: file format 222 and newer, float grids with the standard
// 5-4-3 tree, stored as floats or halves, uncompressed (EmberGen's
// "compression: none"), zip compressed, or Blosc compressed (Houdini's
// default) with BloscLZ, LZ4 or zlib inside and byte shuffling. Blosc's
// bit shuffle and its other codecs are refused with a clear message.

#ifndef VDB_READ_H
#define VDB_READ_H

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
	int nx;              // the box, in voxels (x, y up, z)
	int ny;
	int nz;
	int origin[3];       // the box's lowest corner, in the file's voxel coordinates
	double voxel_size;   // the file's voxel size (its own units; EmberGen: 1)
	float *values;       // nx * ny * nz, x fastest, then y, then z; 0 where the file has nothing
	float largest;       // the largest value in it
} vdb_volume_t;

// The grid named grid_name (NULL: the first float grid) of the file, cropped
// to where it has values. False, with why in error, if it cannot be read.
bool vdb_read(const char *path, const char *grid_name, vdb_volume_t *volume_out, char *error, size_t error_size);

void vdb_volume_free(vdb_volume_t *volume);

#endif
