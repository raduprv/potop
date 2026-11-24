// Textures made once and kept compressed (BC7) in data/cache/textures:
// the terrain's layers and the models' textures go up as they are, their
// levels made already, instead of decoding PNGs, building normals and
// mipmaps at every start. Each cached file carries a key (a CRC32 of what
// it was made from: the source files' bytes, the settings that change the
// result, the version of the making): another key and it is made again.
//
// On a GPU without BC7 the blocks are decoded to RGBA8 on the way up.

#ifndef TEXTURE_CACHE_H
#define TEXTURE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu.h"

// Bump when what goes into a cached texture changes (the making, the
// encoder's settings): every cached texture is made again
#define TEXTURE_CACHE_VERSION 2u

// The key, built a piece at a time (start from 0)
uint32_t texture_cache_key_bytes(uint32_t key, const void *data, size_t size);
uint32_t texture_cache_key_float(uint32_t key, float value);
uint32_t texture_cache_key_int(uint32_t key, int32_t value);
uint32_t texture_cache_key_string(uint32_t key, const char *text);
// A data file's bytes (relative to data/), or a mark that it is missing
uint32_t texture_cache_key_data_file(uint32_t key, const char *relative_path);

// The levels of a texture of width x height (down to 1 x 1)
uint32_t texture_cache_level_count(uint32_t width, uint32_t height);
// Bytes of one level's BC7 blocks, and of a whole layer (every level)
size_t texture_cache_level_bytes(uint32_t width, uint32_t height);
size_t texture_cache_layer_bytes(uint32_t width, uint32_t height, uint32_t level_count);

// What a texture's texels are, for making its levels
typedef enum
{
	TEXTURE_CACHE_COLOR = 0,               // RGB a color (sRGB: averaged in linear light), A a value
	TEXTURE_CACHE_COLOR_CUT_OUT,           // the same, A the coverage cut at a half: kept as much covered at every level
	TEXTURE_CACHE_NORMAL,                  // RGB a normal (renormalized at every level), A a value
	TEXTURE_CACHE_DATA                     // values (occlusion, roughness, metal): averaged as they are
} texture_cache_kind_t;

// One layer of RGBA8 pixels into BC7 blocks, every level (each made from
// the one above with a Kaiser windowed filter, the texture repeating, as
// its kind wants), blocks_out holding texture_cache_layer_bytes. On every
// core
void texture_cache_encode_layer(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t level_count, texture_cache_kind_t kind,
	uint8_t *blocks_out);

// One level's RGBA8 pixels into BC7 blocks, as they are
// (texture_cache_level_bytes of them): for a texture whose levels are made
// by who has it (an impostor's atlas: a picture's cell never mixed with
// the next one's). On every core
void texture_cache_encode_pixels(const uint8_t *pixels, uint32_t width, uint32_t height, uint8_t *blocks_out);

// The cached file name (under data/cache/textures/) of a key and a name;
// load: its blocks (SDL_free them) if its key and its sizes are those
// asked for, else NULL. save: the blocks of layer_count layers
uint8_t *texture_cache_load(const char *name, uint32_t key, uint32_t width, uint32_t height, uint32_t layer_count, uint32_t level_count);
// The same for one picture of whatever size it was saved at (its size and
// levels out)
uint8_t *texture_cache_load_image(const char *name, uint32_t key, uint32_t *width_out, uint32_t *height_out, uint32_t *level_count_out);
void texture_cache_save(const char *name, uint32_t key, uint32_t width, uint32_t height, uint32_t layer_count, uint32_t level_count,
	const uint8_t *blocks);

// A cached file's name from a data file's path (its directories in it)
void texture_cache_name(const char *relative_path, char *name_out, size_t name_out_size);

// A texture (2D, or a 2D array with layer_count > 1) to hold them: BC7
// where the GPU reads it, else RGBA8
SDL_GPUTexture *texture_cache_create(uint32_t width, uint32_t height, uint32_t layer_count, uint32_t level_count);
// The same, a 2D array whatever its layers (one too: a shader's sampler
// of arrays wants an array)
SDL_GPUTexture *texture_cache_create_array(uint32_t width, uint32_t height, uint32_t layer_count, uint32_t level_count);
// A layer's smallest level's first texel (its blocks, every level: as
// texture_cache_upload_layer takes them), decoded: what a shader's
// textureLod at the last level reads, its average (RGBA8)
void texture_cache_layer_smallest(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t level_count, uint8_t rgba_out[4]);

// One layer's blocks (every level) up into it (decoded where it is RGBA8)
void texture_cache_upload_layer(SDL_GPUTexture *texture, uint32_t layer, uint32_t width, uint32_t height, uint32_t level_count,
	const uint8_t *blocks);

#endif
