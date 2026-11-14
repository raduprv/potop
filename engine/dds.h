// DDS texture decoding.
//
// Eternal Lands textures are DDS files, mostly DXT1/DXT3/DXT5 compressed.
// Desktop GL could take those as they are, but most phone GPUs cannot (S3TC
// is a desktop extension), so they are decoded to plain RGBA here and the
// mipmaps are rebuilt by GL. Uncompressed 24 and 32 bit DDS files work too.
//
// Rows come out in file order, top row first. The models were made for
// textures uploaded that way (Eternal Lands hands DDS data to GL as is), so
// the texture coordinates match without flipping.

#ifndef DDS_H
#define DDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decodes the first (largest) mip level. On success *pixels_out holds
// width * height RGBA bytes, to be released with SDL_free. has_alpha_out
// is set when the format carries an alpha channel.
bool dds_decode(const void *data, size_t size, uint8_t **pixels_out, int *width_out, int *height_out, bool *has_alpha_out);

#endif
