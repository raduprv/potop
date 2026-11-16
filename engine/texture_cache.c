// Compressed textures kept in the cache. See texture_cache.h.

#include "texture_cache.h"
#include "file_io.h"
#include "parallel.h"

#include <SDL3/SDL.h>

#include "miniz.h"
#include "bc7enc/bc7enc.h"
#include "bcdec/bcdec.h"

typedef struct
{
	char magic[4];                 // "PTXC"
	uint32_t version;              // TEXTURE_CACHE_VERSION
	uint32_t key;
	uint32_t width;
	uint32_t height;
	uint32_t layer_count;
	uint32_t level_count;
	uint32_t reserved;
} texture_cache_header_t;

uint32_t texture_cache_key_bytes(uint32_t key, const void *data, size_t size)
{
	return (uint32_t)mz_crc32(key, (const unsigned char *)data, size);
}

uint32_t texture_cache_key_float(uint32_t key, float value)
{
	return texture_cache_key_bytes(key, &value, sizeof(value));
}

uint32_t texture_cache_key_int(uint32_t key, int32_t value)
{
	return texture_cache_key_bytes(key, &value, sizeof(value));
}

uint32_t texture_cache_key_string(uint32_t key, const char *text)
{
	// (with its end: "ab" "c" differs from "a" "bc")
	return texture_cache_key_bytes(key, text, SDL_strlen(text) + 1);
}

uint32_t texture_cache_key_data_file(uint32_t key, const char *relative_path)
{
	size_t size = 0;
	void *data;

	key = texture_cache_key_string(key, relative_path);
	if (!file_io_data_file_exists(relative_path))
	{
		return texture_cache_key_int(key, -1);
	}
	data = file_io_load_data_file(relative_path, &size);
	if (data == NULL)
	{
		return texture_cache_key_int(key, -2);
	}
	key = texture_cache_key_bytes(key, data, size);
	SDL_free(data);
	return key;
}

uint32_t texture_cache_level_count(uint32_t width, uint32_t height)
{
	uint32_t count = 1;

	while (width > 1 || height > 1)
	{
		width = SDL_max(width / 2, 1u);
		height = SDL_max(height / 2, 1u);
		count++;
	}
	return count;
}

size_t texture_cache_level_bytes(uint32_t width, uint32_t height)
{
	return (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * 16;
}

size_t texture_cache_layer_bytes(uint32_t width, uint32_t height, uint32_t level_count)
{
	size_t total = 0;

	for (uint32_t k = 0; k < level_count; k++)
	{
		total += texture_cache_level_bytes(SDL_max(width >> k, 1u), SDL_max(height >> k, 1u));
	}
	return total;
}

static bc7enc_compress_block_params encode_parameters;

static void encode_initialize(void)
{
	static bool initialized = false;

	if (initialized)
	{
		return;
	}
	bc7enc_compress_block_init();
	SDL_zero(encode_parameters);
	// (as the plant atlases: good, not the slowest)
	encode_parameters.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;
	encode_parameters.m_try_least_squares = BC7ENC_TRUE;
	encode_parameters.m_mode_partition_estimation_filterbank = BC7ENC_TRUE;
	encode_parameters.m_use_mode5_for_alpha = BC7ENC_TRUE;
	encode_parameters.m_use_mode7_for_alpha = BC7ENC_TRUE;
	encode_parameters.m_perceptual = BC7ENC_FALSE;
	for (int k = 0; k < 4; k++)
	{
		encode_parameters.m_weights[k] = 1;
	}
	initialized = true;
}

// One level's blocks, its rows of blocks on every core
static void encode_level(const uint8_t *pixels, uint32_t width, uint32_t height, uint8_t *blocks)
{
	int blocks_across = (int)((width + 3) / 4);
	int rows = (int)((height + 3) / 4);

#pragma omp parallel for schedule(dynamic, 4) num_threads(parallel_threads_for_items(rows, 4))
	for (int by = 0; by < rows; by++)
	{
		for (int bx = 0; bx < blocks_across; bx++)
		{
			uint8_t texels[16 * 4];

			// (past the sides: the last row or column again)
			for (int y = 0; y < 4; y++)
			{
				uint32_t source_y = SDL_min((uint32_t)(by * 4 + y), height - 1);

				for (int x = 0; x < 4; x++)
				{
					uint32_t source_x = SDL_min((uint32_t)(bx * 4 + x), width - 1);

					SDL_memcpy(&texels[(y * 4 + x) * 4], &pixels[((size_t)source_y * width + source_x) * 4], 4);
				}
			}
			bc7enc_compress_block(&blocks[((size_t)by * (size_t)blocks_across + (size_t)bx) * 16], texels, &encode_parameters);
		}
	}
}

// sRGB and linear light (a table one way, the formula back)
static float srgb_to_linear_table[256];

static void srgb_table_initialize(void)
{
	static bool initialized = false;

	if (initialized)
	{
		return;
	}
	for (int i = 0; i < 256; i++)
	{
		float value = (float)i / 255.0f;

		srgb_to_linear_table[i] = value <= 0.04045f ? value / 12.92f : SDL_powf((value + 0.055f) / 1.055f, 2.4f);
	}
	initialized = true;
}

static uint8_t linear_to_srgb_byte(float value)
{
	float encoded;

	value = SDL_clamp(value, 0.0f, 1.0f);
	encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * SDL_powf(value, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)SDL_clamp((int)(encoded * 255.0f + 0.5f), 0, 255);
}

static uint8_t unit_to_byte(float value)
{
	return (uint8_t)SDL_clamp((int)(SDL_clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f), 0, 255);
}

// The halving filter: a Kaiser windowed sinc over 6 texels of the level
// above (3 each side of the new texel's middle, which falls between two of
// them), normalized; sharper than a 2 x 2 average and without its aliasing
#define HALVING_TAPS 6

static void halving_weights(float weights[HALVING_TAPS])
{
	const float alpha = 4.0f;
	const float half_width = 3.0f;
	float sum = 0.0f;

	for (int k = 0; k < HALVING_TAPS; k++)
	{
		// (the tap's distance from the new texel's middle, in texels of the
		// level above: -2.5 .. 2.5; the sinc's cutoff at the new level's
		// Nyquist, half the old)
		float distance = (float)k - 2.5f;
		float x = distance * 0.5f;
		float sinc = SDL_fabsf(x) < 1e-6f ? 1.0f : SDL_sinf(SDL_PI_F * x) / (SDL_PI_F * x);
		float ratio = distance / half_width;
		float window_argument = alpha * SDL_sqrtf(SDL_max(1.0f - ratio * ratio, 0.0f));
		// (the Bessel function I0, its series: plenty of terms for alpha 4)
		float bessel = 1.0f;
		float bessel_alpha = 1.0f;
		float term = 1.0f;
		float term_alpha = 1.0f;

		for (int n = 1; n < 16; n++)
		{
			term *= (window_argument * 0.5f / (float)n) * (window_argument * 0.5f / (float)n);
			term_alpha *= (alpha * 0.5f / (float)n) * (alpha * 0.5f / (float)n);
			bessel += term;
			bessel_alpha += term_alpha;
		}
		weights[k] = sinc * bessel / bessel_alpha;
		sum += weights[k];
	}
	for (int k = 0; k < HALVING_TAPS; k++)
	{
		weights[k] /= sum;
	}
}

// One level halved (four floats a texel, the texture repeating): across,
// then down; a dimension already 1 stays
static void halve_level(const float *level, uint32_t width, uint32_t height, float *scratch, float *next, uint32_t next_width,
	uint32_t next_height)
{
	float weights[HALVING_TAPS];

	halving_weights(weights);
	// Across: width to next_width, every row
#pragma omp parallel for schedule(static) num_threads(parallel_threads_for_items((int32_t)height, 16))
	for (int y = 0; y < (int)height; y++)
	{
		for (uint32_t x = 0; x < next_width; x++)
		{
			float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

			for (int k = 0; k < HALVING_TAPS; k++)
			{
				uint32_t source_x = width == next_width ? x : (uint32_t)(((int64_t)x * 2 + k - 2 + (int64_t)width * 4) % width);
				const float *texel = &level[((size_t)y * width + source_x) * 4];
				float weight = width == next_width ? (k == 2 ? 1.0f : 0.0f) : weights[k];

				for (int c = 0; c < 4; c++)
				{
					sum[c] += texel[c] * weight;
				}
			}
			SDL_memcpy(&scratch[((size_t)y * next_width + x) * 4], sum, sizeof(sum));
		}
	}
	// Down: height to next_height
#pragma omp parallel for schedule(static) num_threads(parallel_threads_for_items((int32_t)next_height, 16))
	for (int y = 0; y < (int)next_height; y++)
	{
		for (uint32_t x = 0; x < next_width; x++)
		{
			float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

			for (int k = 0; k < HALVING_TAPS; k++)
			{
				uint32_t source_y = height == next_height ? (uint32_t)y : (uint32_t)(((int64_t)y * 2 + k - 2 + (int64_t)height * 4) % height);
				const float *texel = &scratch[((size_t)source_y * next_width + x) * 4];
				float weight = height == next_height ? (k == 2 ? 1.0f : 0.0f) : weights[k];

				for (int c = 0; c < 4; c++)
				{
					sum[c] += texel[c] * weight;
				}
			}
			SDL_memcpy(&next[((size_t)y * next_width + x) * 4], sum, sizeof(sum));
		}
	}
}

// How much of a level is covered: its alpha times scale at a half or more
static float coverage_at(const float *level, size_t texels, float scale)
{
	size_t covered = 0;

	for (size_t i = 0; i < texels; i++)
	{
		covered += level[i * 4 + 3] * scale >= 0.5f ? 1u : 0u;
	}
	return (float)covered / (float)SDL_max(texels, (size_t)1);
}

// A level in floats (linear light, a normal's -1..1) into RGBA8 as its kind keeps it
static void level_to_bytes(const float *level, size_t texels, texture_cache_kind_t kind, uint8_t *bytes)
{
	for (size_t i = 0; i < texels; i++)
	{
		const float *texel = &level[i * 4];

		if (kind == TEXTURE_CACHE_NORMAL)
		{
			float length = SDL_sqrtf(texel[0] * texel[0] + texel[1] * texel[1] + texel[2] * texel[2]);
			float scale = length > 1e-6f ? 1.0f / length : 0.0f;

			bytes[i * 4 + 0] = unit_to_byte(texel[0] * scale * 0.5f + 0.5f);
			bytes[i * 4 + 1] = unit_to_byte(texel[1] * scale * 0.5f + 0.5f);
			bytes[i * 4 + 2] = unit_to_byte(length > 1e-6f ? texel[2] * scale * 0.5f + 0.5f : 1.0f);
		}
		else if (kind == TEXTURE_CACHE_DATA)
		{
			for (int c = 0; c < 3; c++)
			{
				bytes[i * 4 + c] = unit_to_byte(texel[c]);
			}
		}
		else
		{
			for (int c = 0; c < 3; c++)
			{
				bytes[i * 4 + c] = linear_to_srgb_byte(texel[c]);
			}
		}
		bytes[i * 4 + 3] = unit_to_byte(texel[3]);
	}
}

void texture_cache_encode_layer(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t level_count, texture_cache_kind_t kind,
	uint8_t *blocks_out)
{
	size_t texels = (size_t)width * height;
	float *level = SDL_malloc(texels * 4 * sizeof(float));
	float *next = SDL_malloc(texels * 4 * sizeof(float));
	float *scratch = SDL_malloc(texels * 4 * sizeof(float));
	uint8_t *bytes = SDL_malloc(texels * 4);
	uint32_t level_width = width;
	uint32_t level_height = height;
	size_t offset = 0;
	float coverage = 0.0f;

	if (level == NULL || next == NULL || scratch == NULL || bytes == NULL)
	{
		SDL_free(level);
		SDL_free(next);
		SDL_free(scratch);
		SDL_free(bytes);
		return;
	}
	encode_initialize();
	srgb_table_initialize();
	// The top level in floats: colors in linear light, normals as vectors
	for (size_t i = 0; i < texels; i++)
	{
		for (int c = 0; c < 3; c++)
		{
			uint8_t value = pixels[i * 4 + c];

			level[i * 4 + c] = kind == TEXTURE_CACHE_NORMAL ? (float)value / 255.0f * 2.0f - 1.0f
				: (kind == TEXTURE_CACHE_DATA ? (float)value / 255.0f : srgb_to_linear_table[value]);
		}
		level[i * 4 + 3] = (float)pixels[i * 4 + 3] / 255.0f;
	}
	if (kind == TEXTURE_CACHE_COLOR_CUT_OUT)
	{
		coverage = coverage_at(level, texels, 1.0f);
	}
	for (uint32_t k = 0; k < level_count; k++)
	{
		size_t level_texels = (size_t)level_width * level_height;

		// (the top level as it came: exact)
		if (k == 0)
		{
			SDL_memcpy(bytes, pixels, texels * 4);
		}
		else
		{
			level_to_bytes(level, level_texels, kind, bytes);
		}
		encode_level(bytes, level_width, level_height, blocks_out + offset);
		offset += texture_cache_level_bytes(level_width, level_height);
		if (k + 1 < level_count)
		{
			uint32_t next_width = SDL_max(level_width / 2, 1u);
			uint32_t next_height = SDL_max(level_height / 2, 1u);
			size_t next_texels = (size_t)next_width * next_height;
			float *swap;

			halve_level(level, level_width, level_height, scratch, next, next_width, next_height);
			// A normal's length back to one (the filter shortens them
			// where they spread: else the relief flattens and the light
			// on it goes wrong far off)
			if (kind == TEXTURE_CACHE_NORMAL)
			{
				for (size_t i = 0; i < next_texels; i++)
				{
					float *texel = &next[i * 4];
					float length = SDL_sqrtf(texel[0] * texel[0] + texel[1] * texel[1] + texel[2] * texel[2]);

					if (length > 1e-6f)
					{
						texel[0] /= length;
						texel[1] /= length;
						texel[2] /= length;
					}
				}
			}
			// A cut out's alpha scaled so as much of it stays covered as at
			// the top (else leaves and ivy thin out with distance)
			if (kind == TEXTURE_CACHE_COLOR_CUT_OUT && coverage > 0.0f && coverage < 1.0f)
			{
				float low = 0.0f;
				float high = 8.0f;

				for (int step = 0; step < 14; step++)
				{
					float middle = 0.5f * (low + high);

					if (coverage_at(next, next_texels, middle) < coverage)
					{
						low = middle;
					}
					else
					{
						high = middle;
					}
				}
				for (size_t i = 0; i < next_texels; i++)
				{
					next[i * 4 + 3] = SDL_min(next[i * 4 + 3] * high, 1.0f);
				}
			}
			swap = level;
			level = next;
			next = swap;
			level_width = next_width;
			level_height = next_height;
		}
	}
	SDL_free(level);
	SDL_free(next);
	SDL_free(scratch);
	SDL_free(bytes);
}

void texture_cache_name(const char *relative_path, char *name_out, size_t name_out_size)
{
	size_t length = 0;

	for (const char *c = relative_path; *c != '\0' && length + 1 < name_out_size; c++)
	{
		name_out[length++] = (*c == '/' || *c == '\\' || *c == ':' || *c == '.') ? '_' : *c;
	}
	name_out[length] = '\0';
}

static void cache_path(const char *name, char *path_out, size_t path_out_size)
{
	char relative[256];

	SDL_snprintf(relative, sizeof(relative), "cache/textures/%s.ptx", name);
	file_io_data_path(relative, path_out, path_out_size);
}

uint8_t *texture_cache_load(const char *name, uint32_t key, uint32_t width, uint32_t height, uint32_t layer_count, uint32_t level_count)
{
	char path[1024];
	size_t size = 0;
	uint8_t *file;
	const texture_cache_header_t *header;
	size_t blocks_bytes = texture_cache_layer_bytes(width, height, level_count) * layer_count;
	uint8_t *blocks;

	cache_path(name, path, sizeof(path));
	file = file_io_load_file(path, &size);
	if (file == NULL)
	{
		return NULL;
	}
	header = (const texture_cache_header_t *)(const void *)file;
	if (size != sizeof(*header) + blocks_bytes || SDL_memcmp(header->magic, "PTXC", 4) != 0 || header->version != TEXTURE_CACHE_VERSION
		|| header->key != key || header->width != width || header->height != height || header->layer_count != layer_count
		|| header->level_count != level_count)
	{
		SDL_free(file);
		return NULL;
	}
	blocks = SDL_malloc(blocks_bytes);
	if (blocks != NULL)
	{
		SDL_memcpy(blocks, file + sizeof(*header), blocks_bytes);
	}
	SDL_free(file);
	return blocks;
}

uint8_t *texture_cache_load_image(const char *name, uint32_t key, uint32_t *width_out, uint32_t *height_out, uint32_t *level_count_out)
{
	char path[1024];
	size_t size = 0;
	uint8_t *file;
	const texture_cache_header_t *header;
	size_t blocks_bytes;
	uint8_t *blocks;

	cache_path(name, path, sizeof(path));
	file = file_io_load_file(path, &size);
	if (file == NULL)
	{
		return NULL;
	}
	header = (const texture_cache_header_t *)(const void *)file;
	if (size < sizeof(*header) || SDL_memcmp(header->magic, "PTXC", 4) != 0 || header->version != TEXTURE_CACHE_VERSION || header->key != key
		|| header->layer_count != 1 || header->width == 0 || header->height == 0 || header->level_count == 0 || header->level_count > 16)
	{
		SDL_free(file);
		return NULL;
	}
	blocks_bytes = texture_cache_layer_bytes(header->width, header->height, header->level_count);
	if (size != sizeof(*header) + blocks_bytes)
	{
		SDL_free(file);
		return NULL;
	}
	blocks = SDL_malloc(blocks_bytes);
	if (blocks != NULL)
	{
		SDL_memcpy(blocks, file + sizeof(*header), blocks_bytes);
		*width_out = header->width;
		*height_out = header->height;
		*level_count_out = header->level_count;
	}
	SDL_free(file);
	return blocks;
}

void texture_cache_save(const char *name, uint32_t key, uint32_t width, uint32_t height, uint32_t layer_count, uint32_t level_count,
	const uint8_t *blocks)
{
	char path[1024];
	char directory[1024];
	size_t blocks_bytes = texture_cache_layer_bytes(width, height, level_count) * layer_count;
	uint8_t *file = SDL_malloc(sizeof(texture_cache_header_t) + blocks_bytes);
	texture_cache_header_t header;

	if (file == NULL)
	{
		return;
	}
	SDL_zero(header);
	SDL_memcpy(header.magic, "PTXC", 4);
	header.version = TEXTURE_CACHE_VERSION;
	header.key = key;
	header.width = width;
	header.height = height;
	header.layer_count = layer_count;
	header.level_count = level_count;
	SDL_memcpy(file, &header, sizeof(header));
	SDL_memcpy(file + sizeof(header), blocks, blocks_bytes);
	file_io_data_path("cache/textures", directory, sizeof(directory));
	SDL_CreateDirectory(directory);
	cache_path(name, path, sizeof(path));
	if (!file_io_save_file(path, file, sizeof(header) + blocks_bytes))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Texture cache: cannot write '%s' (made again next time)", path);
	}
	SDL_free(file);
}

static bool bc7_native(void)
{
	return SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D_ARRAY,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
}

SDL_GPUTexture *texture_cache_create(uint32_t width, uint32_t height, uint32_t layer_count, uint32_t level_count)
{
	return gpu_create_texture(layer_count > 1 ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D,
		bc7_native() ? SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, width, height, layer_count, level_count,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
}

void texture_cache_layer_smallest(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t level_count, uint8_t rgba_out[4])
{
	size_t offset = 0;
	uint8_t block[16 * 4];

	for (uint32_t k = 0; k + 1 < level_count; k++)
	{
		offset += texture_cache_level_bytes(SDL_max(width >> k, 1u), SDL_max(height >> k, 1u));
	}
	bcdec_bc7(blocks + offset, block, 16);
	SDL_memcpy(rgba_out, block, 4);
}

void texture_cache_upload_layer(SDL_GPUTexture *texture, uint32_t layer, uint32_t width, uint32_t height, uint32_t level_count,
	const uint8_t *blocks)
{
	bool native = bc7_native();
	uint8_t *decoded = native ? NULL : SDL_malloc((size_t)width * height * 4);
	size_t offset = 0;

	if (texture == NULL || (!native && decoded == NULL))
	{
		SDL_free(decoded);
		return;
	}
	for (uint32_t k = 0; k < level_count; k++)
	{
		uint32_t level_width = SDL_max(width >> k, 1u);
		uint32_t level_height = SDL_max(height >> k, 1u);
		size_t bytes = texture_cache_level_bytes(level_width, level_height);

		if (native)
		{
			gpu_upload_texture(texture, k, layer, 0, 0, level_width, level_height, blocks + offset, bytes);
		}
		else
		{
			uint32_t blocks_across = (level_width + 3) / 4;

			for (uint32_t by = 0; by < (level_height + 3) / 4; by++)
			{
				for (uint32_t bx = 0; bx < blocks_across; bx++)
				{
					uint8_t block[16 * 4];

					bcdec_bc7(blocks + offset + ((size_t)by * blocks_across + bx) * 16, block, 16);
					for (uint32_t y = 0; y < 4 && by * 4 + y < level_height; y++)
					{
						for (uint32_t x = 0; x < 4 && bx * 4 + x < level_width; x++)
						{
							SDL_memcpy(&decoded[((size_t)(by * 4 + y) * level_width + bx * 4 + x) * 4], &block[(y * 4 + x) * 4], 4);
						}
					}
				}
			}
			gpu_upload_texture(texture, k, layer, 0, 0, level_width, level_height, decoded, (size_t)level_width * level_height * 4);
		}
		offset += bytes;
	}
	SDL_free(decoded);
}
