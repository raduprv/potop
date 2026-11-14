// A plant atlas' file (tools/potop_plant writes it, object_model.c reads
// it): data/textures/<name>.dds, the leaves, twigs and the rest of the
// plants in it, each picture's months in cells of their own (the plants
// say which cell shows in which month: object_file_month_cell_t).
//
// A DDS with the DX10 header: BC7 (DXGI_FORMAT_BC7_UNORM, the shaders take
// the colors as sRGB themselves), 2D, its full chain of levels down to 1 x
// 1 (largest first; a level smaller than 4 texels a side is still a whole
// block). The header's first reserved words say what it is to the game:
// PLANT_ATLAS_TAG, and a hash of the pixels it was made from (the tool
// leaves an unchanged one as it is).

#ifndef PLANT_ATLAS_FILE_H
#define PLANT_ATLAS_FILE_H

#include <stdint.h>

#define PLANT_ATLAS_TAG 0x4c544150u              // "PATL"
#define PLANT_ATLAS_DXGI_BC7_UNORM 98u
#define PLANT_ATLAS_DIMENSION_TEXTURE_2D 3u

typedef struct
{
	char magic[4];                                // "DDS "
	uint32_t header_size;                         // 124
	uint32_t flags;                               // caps, height, width, pixel format, mip count, linear size
	uint32_t height;
	uint32_t width;
	uint32_t linear_size;                         // level 0's bytes (one layer)
	uint32_t depth;
	uint32_t level_count;
	uint32_t tag;                                 // reserved words: PLANT_ATLAS_TAG
	uint32_t hash[2];                             // of the pixels it was made from
	uint32_t reserved[8];
	uint32_t format_size;                         // 32
	uint32_t format_flags;                        // four cc
	uint32_t four_cc;                             // "DX10"
	uint32_t bit_count;
	uint32_t masks[4];
	uint32_t caps;                                // texture, mipmap, complex
	uint32_t caps2;
	uint32_t caps3;
	uint32_t caps4;
	uint32_t reserved2;
	uint32_t dxgi_format;                         // PLANT_ATLAS_DXGI_BC7_UNORM
	uint32_t resource_dimension;                  // PLANT_ATLAS_DIMENSION_TEXTURE_2D
	uint32_t misc_flag;
	uint32_t layer_count;                         // the array's size: 1
	uint32_t misc_flags2;
} plant_atlas_file_header_t;

#endif
