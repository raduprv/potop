// Model loading: the game's own .pobj (see object_format.h) and Eternal
// Lands' .e3d. See object_model.h.
//
// File layout (little endian):
//   file header   magic "e3dx", version (4 bytes), MD5 of the rest (16),
//                 offset of the e3d header (int32)
//   e3d header    vertex count, size, offset; index count, size, offset;
//                 material count, size, offset (int32 each); vertex options
//                 and vertex format (one byte each, two reserved)
//   vertices      per vertex, in this order: uv, extra uv (optional),
//                 normal (optional), tangent (optional), position, color
//                 (optional); the format bits say which parts are half
//                 floats and whether normals are packed in 16 bits
//   indices       16 or 32 bit
//   materials     options (non zero: alpha tested), texture name (128
//                 bytes), bounding box, index range

#include "object_model.h"
#include "object_format.h"
#include "dds.h"
#include "file_io.h"
#include "gpu.h"
#include "texture_cache.h"

#include <SDL3/SDL.h>
#include <stddef.h>

#include "stb_image.h"
#include "miniz.h"
#include "plant_atlas_file.h"
// The BC7 decoder, for a GPU that does not read BC7 (third party, public domain)
#define BCDEC_IMPLEMENTATION
#include "bcdec/bcdec.h"

#define ELC_HEADER_SIZE 28
#define E3D_HEADER_SIZE 40
#define E3D_MATERIAL_SIZE 172
#define E3D_EXTRA_TEXTURE_SIZE 128

// Vertex options
#define E3D_HAS_NORMAL 0x01
#define E3D_HAS_TANGENT 0x02
#define E3D_HAS_EXTRA_UV 0x04
#define E3D_HAS_COLOR 0x08
// Vertex format
#define E3D_HALF_POSITION 0x01
#define E3D_HALF_UV 0x02
#define E3D_HALF_EXTRA_UV 0x04
#define E3D_COMPRESSED_NORMAL 0x08
#define E3D_SHORT_INDEX 0x10

// Our vertex: position, normal, uv
typedef struct
{
	float position[3];
	float normal[3];
	float uv[2];
} object_vertex_t;

// (objects.c lays them out in its shared buffers by object_model.h's size)
SDL_COMPILE_TIME_ASSERT(object_vertex_size, sizeof(object_vertex_t) == OBJECT_MODEL_VERTEX_BYTES);

static uint32_t read_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint16_t read_u16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static float read_f32(const uint8_t *bytes)
{
	uint32_t bits = read_u32(bytes);
	float value;

	SDL_memcpy(&value, &bits, sizeof(value));
	return value;
}

// IEEE half precision to float
static float half_to_float(uint16_t half)
{
	uint32_t sign = (uint32_t)(half & 0x8000) << 16;
	uint32_t exponent = (half >> 10) & 0x1f;
	uint32_t mantissa = half & 0x3ff;
	uint32_t bits;
	float value;

	if (exponent == 0)
	{
		// Zero or subnormal: mantissa * 2^-24
		value = (float)mantissa * (1.0f / 16777216.0f);
		return sign ? -value : value;
	}
	if (exponent == 31)
	{
		bits = sign | 0x7f800000u | (mantissa << 13);
	}
	else
	{
		bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
	}
	SDL_memcpy(&value, &bits, sizeof(value));
	return value;
}

// Eternal Lands' 16 bit normal packing: three sign bits, then a point on the
// triangle x + y + z = 126 (7 + 6 bits), normalized back onto the sphere
static void uncompress_normal(uint16_t value, float normal_out[3])
{
	uint32_t x = (value & 0x1f80) >> 7;
	uint32_t y = value & 0x007f;
	float length;

	if (x + y >= 127)
	{
		x = 127 - x;
		y = 127 - y;
	}
	normal_out[0] = (float)x;
	normal_out[1] = (float)y;
	normal_out[2] = (float)(126 - (int)x - (int)y);
	if (value & 0x8000)
	{
		normal_out[0] = -normal_out[0];
	}
	if (value & 0x4000)
	{
		normal_out[1] = -normal_out[1];
	}
	if (value & 0x2000)
	{
		normal_out[2] = -normal_out[2];
	}
	length = SDL_sqrtf(normal_out[0] * normal_out[0] + normal_out[1] * normal_out[1] + normal_out[2] * normal_out[2]);
	if (length > 0.0f)
	{
		normal_out[0] /= length;
		normal_out[1] /= length;
		normal_out[2] /= length;
	}
}

static int expected_vertex_size(uint32_t options, uint32_t format)
{
	int size = 0;

	size += (format & E3D_HALF_UV) ? 4 : 8;
	if (options & E3D_HAS_EXTRA_UV)
	{
		size += (format & E3D_HALF_EXTRA_UV) ? 4 : 8;
	}
	if (options & E3D_HAS_NORMAL)
	{
		size += (format & E3D_COMPRESSED_NORMAL) ? 2 : 12;
	}
	if (options & E3D_HAS_TANGENT)
	{
		size += (format & E3D_COMPRESSED_NORMAL) ? 2 : 12;
	}
	size += (format & E3D_HALF_POSITION) ? 6 : 12;
	if (options & E3D_HAS_COLOR)
	{
		size += 4;
	}
	return size;
}

// Model axes (Z up) to game axes (Y up): (x, y, z) -> (x, z, -y)
static void model_to_game(const float model[3], float game_out[3])
{
	game_out[0] = model[0];
	game_out[1] = model[2];
	game_out[2] = -model[1];
}

// Decodes one vertex starting at 'source'
static void read_vertex(const uint8_t *source, uint32_t options, uint32_t format, object_vertex_t *vertex)
{
	float normal[3] = { 0.0f, 0.0f, 1.0f };
	float position[3];

	if (format & E3D_HALF_UV)
	{
		vertex->uv[0] = half_to_float(read_u16(source));
		vertex->uv[1] = half_to_float(read_u16(source + 2));
		source += 4;
	}
	else
	{
		vertex->uv[0] = read_f32(source);
		vertex->uv[1] = read_f32(source + 4);
		source += 8;
	}
	if (options & E3D_HAS_EXTRA_UV)
	{
		source += (format & E3D_HALF_EXTRA_UV) ? 4 : 8;
	}
	if (options & E3D_HAS_NORMAL)
	{
		if (format & E3D_COMPRESSED_NORMAL)
		{
			uncompress_normal(read_u16(source), normal);
			source += 2;
		}
		else
		{
			normal[0] = read_f32(source);
			normal[1] = read_f32(source + 4);
			normal[2] = read_f32(source + 8);
			source += 12;
		}
	}
	if (options & E3D_HAS_TANGENT)
	{
		source += (format & E3D_COMPRESSED_NORMAL) ? 2 : 12;
	}
	if (format & E3D_HALF_POSITION)
	{
		position[0] = half_to_float(read_u16(source));
		position[1] = half_to_float(read_u16(source + 2));
		position[2] = half_to_float(read_u16(source + 4));
	}
	else
	{
		position[0] = read_f32(source);
		position[1] = read_f32(source + 4);
		position[2] = read_f32(source + 8);
	}
	model_to_game(position, vertex->position);
	model_to_game(normal, vertex->normal);
}

SDL_GPUTexture *object_texture_upload(const uint8_t *pixels, int width, int height)
{
	SDL_GPUTexture *texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (uint32_t)width,
		(uint32_t)height, 1, 0, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);

	if (texture != NULL)
	{
		gpu_upload_texture(texture, 0, 0, 0, 0, (uint32_t)width, (uint32_t)height, pixels, (size_t)width * (size_t)height * 4);
		gpu_generate_mipmaps(texture);
	}
	return texture;
}

// Gives the transparent pixels of a cut out texture the color of their
// opaque neighbors. They are never seen at full size, but mipmapping
// averages them in: with their usual black, leaves and reeds grow dark
// fringes at a distance. Grows the opaque color outwards a pixel per pass;
// what is left after the passes gets the average opaque color.
void object_texture_bleed_transparent_colors(uint8_t *pixels, int width, int height)
{
	const int pass_count = 16;
	uint8_t *filled = SDL_malloc((size_t)width * (size_t)height);
	uint8_t *next_filled = SDL_malloc((size_t)width * (size_t)height);
	uint64_t sum[3] = { 0, 0, 0 };
	uint64_t opaque_count = 0;

	if (filled == NULL || next_filled == NULL)
	{
		SDL_free(filled);
		SDL_free(next_filled);
		return;
	}
	for (int i = 0; i < width * height; i++)
	{
		filled[i] = pixels[i * 4 + 3] >= 128 ? 1 : 0;
		if (filled[i])
		{
			sum[0] += pixels[i * 4 + 0];
			sum[1] += pixels[i * 4 + 1];
			sum[2] += pixels[i * 4 + 2];
			opaque_count++;
		}
	}
	if (opaque_count == 0 || opaque_count == (uint64_t)width * (uint64_t)height)
	{
		SDL_free(filled);
		SDL_free(next_filled);
		return;
	}
	for (int pass = 0; pass < pass_count; pass++)
	{
		bool changed = false;

		SDL_memcpy(next_filled, filled, (size_t)width * (size_t)height);
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				static const int offsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
				int index = y * width + x;
				int color[3] = { 0, 0, 0 };
				int count = 0;

				if (filled[index])
				{
					continue;
				}
				for (int k = 0; k < 4; k++)
				{
					// Wrapping, as the texture repeats
					int neighbor_x = (x + offsets[k][0] + width) % width;
					int neighbor_y = (y + offsets[k][1] + height) % height;
					int neighbor = neighbor_y * width + neighbor_x;

					if (filled[neighbor])
					{
						color[0] += pixels[neighbor * 4 + 0];
						color[1] += pixels[neighbor * 4 + 1];
						color[2] += pixels[neighbor * 4 + 2];
						count++;
					}
				}
				if (count > 0)
				{
					pixels[index * 4 + 0] = (uint8_t)(color[0] / count);
					pixels[index * 4 + 1] = (uint8_t)(color[1] / count);
					pixels[index * 4 + 2] = (uint8_t)(color[2] / count);
					next_filled[index] = 1;
					changed = true;
				}
			}
		}
		SDL_memcpy(filled, next_filled, (size_t)width * (size_t)height);
		if (!changed)
		{
			break;
		}
	}
	for (int i = 0; i < width * height; i++)
	{
		if (!filled[i])
		{
			pixels[i * 4 + 0] = (uint8_t)(sum[0] / opaque_count);
			pixels[i * 4 + 1] = (uint8_t)(sum[1] / opaque_count);
			pixels[i * 4 + 2] = (uint8_t)(sum[2] / opaque_count);
		}
	}
	SDL_free(filled);
	SDL_free(next_filled);
}

static size_t bc7_level_bytes(uint32_t width, uint32_t height)
{
	return (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * 16;
}

// A plant atlas (engine/plant_atlas_file.h) into a texture, its levels as
// they are: BC7 where the GPU reads it, else decoded to RGBA8. NULL if it
// is not one (or is broken)
static SDL_GPUTexture *load_plant_atlas(const uint8_t *data, size_t size, const char *relative_path)
{
	const plant_atlas_file_header_t *header = (const plant_atlas_file_header_t *)data;
	size_t total = 0;
	size_t offset = sizeof(plant_atlas_file_header_t);
	bool native;
	SDL_GPUTexture *texture;
	uint8_t *decoded = NULL;

	if (header->dxgi_format != PLANT_ATLAS_DXGI_BC7_UNORM || header->width == 0 || header->height == 0 || header->level_count == 0
		|| header->level_count > 16 || header->layer_count != 1)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a plant atlas this game reads (make it again)", relative_path);
		return NULL;
	}
	for (uint32_t k = 0; k < header->level_count; k++)
	{
		total += bc7_level_bytes(SDL_max(header->width >> k, 1u), SDL_max(header->height >> k, 1u));
	}
	if (size < offset + total)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The plant atlas '%s' is truncated", relative_path);
		return NULL;
	}
	native = SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D,
		SDL_GPU_TEXTUREUSAGE_SAMPLER);
	texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, native ? SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		header->width, header->height, 1, header->level_count, SDL_GPU_TEXTUREUSAGE_SAMPLER);
	if (texture == NULL)
	{
		return NULL;
	}
	if (!native)
	{
		decoded = SDL_malloc((size_t)header->width * (size_t)header->height * 4);
		if (decoded == NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, texture);
			return NULL;
		}
	}
	{
		for (uint32_t k = 0; k < header->level_count; k++)
		{
			uint32_t width = SDL_max(header->width >> k, 1u);
			uint32_t height = SDL_max(header->height >> k, 1u);
			size_t bytes = bc7_level_bytes(width, height);

			if (native)
			{
				gpu_upload_texture(texture, k, 0, 0, 0, width, height, data + offset, bytes);
			}
			else
			{
				// Block by block into the level's texels (those past its sides
				// left out)
				for (uint32_t by = 0; by < (height + 3) / 4; by++)
				{
					for (uint32_t bx = 0; bx < (width + 3) / 4; bx++)
					{
						uint8_t block[16 * 4];
						const uint8_t *source = data + offset + ((size_t)by * ((width + 3) / 4) + bx) * 16;

						bcdec_bc7(source, block, 16);
						for (uint32_t y = 0; y < 4 && by * 4 + y < height; y++)
						{
							for (uint32_t x = 0; x < 4 && bx * 4 + x < width; x++)
							{
								SDL_memcpy(&decoded[((size_t)(by * 4 + y) * width + bx * 4 + x) * 4], &block[(y * 4 + x) * 4], 4);
							}
						}
					}
				}
				gpu_upload_texture(texture, k, 0, 0, 0, width, height, decoded, (size_t)width * height * 4);
			}
			offset += bytes;
		}
	}
	SDL_free(decoded);
	SDL_Log("Plant atlas '%s': %ux%u, %s", relative_path, header->width, header->height, native ? "BC7" : "decoded (no BC7 on this GPU)");
	return texture;
}

// Loads a texture file (DDS, or anything stb_image reads) into a mipmapped
// RGBA8 texture; a plant atlas (engine/plant_atlas_file.h) as it is. Returns
// NULL on failure.
// data: values, not colors (a normal map, an orm map): its alpha is a value
// too (a height for the relief), not coverage, and nothing is bled into
// where it is low (the normals there were overwritten by their neighbors':
// stripes and wedges on a stone roof)
static SDL_GPUTexture *load_texture(const char *relative_path, bool data)
{
	size_t file_size = 0;
	void *file_data = file_io_load_data_file(relative_path, &file_size);
	uint8_t *pixels = NULL;
	int width = 0;
	int height = 0;
	bool has_alpha = false;
	bool from_stb = false;
	SDL_GPUTexture *texture;
	uint32_t cache_key = 0;
	char cache_file_name[256];

	if (file_data == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot read texture '%s'", relative_path);
		return NULL;
	}
	if (file_size >= sizeof(plant_atlas_file_header_t) && SDL_memcmp(file_data, "DDS ", 4) == 0
		&& ((const plant_atlas_file_header_t *)file_data)->tag == PLANT_ATLAS_TAG)
	{
		texture = load_plant_atlas(file_data, file_size, relative_path);
		SDL_free(file_data);
		return texture;
	}
	// Made before from the same bytes: from the cache, compressed as it is
	{
		char cache_name[256];
		uint32_t key = texture_cache_key_int(data ? 1 : 0, (int32_t)TEXTURE_CACHE_VERSION);
		uint32_t cached_width = 0;
		uint32_t cached_height = 0;
		uint32_t cached_levels = 0;
		uint8_t *blocks;

		key = texture_cache_key_bytes(key, file_data, file_size);
		texture_cache_name(relative_path, cache_name, sizeof(cache_name));
		blocks = texture_cache_load_image(cache_name, key, &cached_width, &cached_height, &cached_levels);
		if (blocks != NULL)
		{
			texture = texture_cache_create(cached_width, cached_height, 1, cached_levels);
			texture_cache_upload_layer(texture, 0, cached_width, cached_height, cached_levels, blocks);
			SDL_free(blocks);
			SDL_free(file_data);
			return texture;
		}
		cache_key = key;
		SDL_strlcpy(cache_file_name, cache_name, sizeof(cache_file_name));
	}
	if (file_size >= 4 && SDL_memcmp(file_data, "DDS ", 4) == 0)
	{
		if (!dds_decode(file_data, file_size, &pixels, &width, &height, &has_alpha))
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot decode texture '%s'", relative_path);
		}
	}
	else
	{
		int channels = 0;

		pixels = stbi_load_from_memory(file_data, (int)file_size, &width, &height, &channels, 4);
		from_stb = true;
		has_alpha = channels == 4 || channels == 2;
		if (pixels == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot decode texture '%s': %s", relative_path, stbi_failure_reason());
		}
	}
	SDL_free(file_data);
	if (pixels == NULL)
	{
		return NULL;
	}

	if (has_alpha && !data)
	{
		object_texture_bleed_transparent_colors(pixels, width, height);
	}
	// Compressed with its levels, kept for next time (a size in whole
	// blocks only; the rest goes up as it is)
	if (width % 4 == 0 && height % 4 == 0)
	{
		uint32_t levels = texture_cache_level_count((uint32_t)width, (uint32_t)height);
		uint8_t *blocks = SDL_malloc(texture_cache_layer_bytes((uint32_t)width, (uint32_t)height, levels));

		if (blocks != NULL)
		{
			// (its kind: a normal map by its name, other values as they
			// are; a color with holes cut in it kept as covered far off)
			texture_cache_kind_t kind = TEXTURE_CACHE_COLOR;

			if (data)
			{
				kind = SDL_strcasestr(relative_path, "normal") != NULL ? TEXTURE_CACHE_NORMAL : TEXTURE_CACHE_DATA;
			}
			else if (has_alpha)
			{
				size_t holes = 0;
				size_t texels = (size_t)width * (size_t)height;

				for (size_t t = 0; t < texels; t++)
				{
					holes += pixels[t * 4 + 3] < 128 ? 1u : 0u;
				}
				kind = holes * 100 > texels ? TEXTURE_CACHE_COLOR_CUT_OUT : TEXTURE_CACHE_COLOR;
			}
			texture_cache_encode_layer(pixels, (uint32_t)width, (uint32_t)height, levels, kind, blocks);
			texture_cache_save(cache_file_name, cache_key, (uint32_t)width, (uint32_t)height, 1, levels, blocks);
			texture = texture_cache_create((uint32_t)width, (uint32_t)height, 1, levels);
			texture_cache_upload_layer(texture, 0, (uint32_t)width, (uint32_t)height, levels, blocks);
			SDL_free(blocks);
			if (from_stb)
			{
				stbi_image_free(pixels);
			}
			else
			{
				SDL_free(pixels);
			}
			return texture;
		}
	}
	texture = object_texture_upload(pixels, width, height);
	if (from_stb)
	{
		stbi_image_free(pixels);
	}
	else
	{
		SDL_free(pixels);
	}
	return texture;
}

static SDL_GPUTexture *default_textures[OBJECT_DEFAULT_COUNT];

SDL_GPUTexture *object_texture_solid(const uint8_t color[4])
{
	uint8_t pixels[4 * 4 * 4];

	for (int t = 0; t < 16; t++)
	{
		SDL_memcpy(&pixels[t * 4], color, 4);
	}
	return object_texture_upload(pixels, 4, 4);
}

SDL_GPUTexture *object_default_texture(object_default_texture_t which)
{
	// A flat normal; no occlusion, roughness 0.9, no metal, and no
	// reflection at all (alpha 0: see object.frag); black
	static const uint8_t colors[OBJECT_DEFAULT_COUNT][4] = { { 128, 128, 255, 255 }, { 255, 230, 0, 0 }, { 0, 0, 0, 255 } };

	if (default_textures[which] == NULL)
	{
		default_textures[which] = object_texture_solid(colors[which]);
	}
	return default_textures[which];
}

void object_default_textures_release(void)
{
	for (int i = 0; i < OBJECT_DEFAULT_COUNT; i++)
	{
		if (default_textures[i] != NULL && gpu.device != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, default_textures[i]);
		}
		default_textures[i] = NULL;
	}
}

void object_compute_tangents(const void *vertices, size_t stride, size_t position_offset, size_t normal_offset, size_t uv_offset,
	uint32_t vertex_count, const uint32_t *indices, uint32_t index_count, float *tangents_out)
{
	const uint8_t *bytes = vertices;
	float *bitangents = SDL_calloc((size_t)vertex_count + 1, sizeof(float) * 3);

	SDL_memset(tangents_out, 0, sizeof(float) * 4 * vertex_count);
	if (bitangents == NULL)
	{
		return;
	}
	for (uint32_t i = 0; i + 2 < index_count; i += 3)
	{
		const float *p[3];
		const float *uv[3];
		float e1[3];
		float e2[3];
		float du1;
		float dv1;
		float du2;
		float dv2;
		float determinant;

		for (int k = 0; k < 3; k++)
		{
			p[k] = (const float *)(bytes + (size_t)indices[i + k] * stride + position_offset);
			uv[k] = (const float *)(bytes + (size_t)indices[i + k] * stride + uv_offset);
		}
		for (int k = 0; k < 3; k++)
		{
			e1[k] = p[1][k] - p[0][k];
			e2[k] = p[2][k] - p[0][k];
		}
		du1 = uv[1][0] - uv[0][0];
		dv1 = uv[1][1] - uv[0][1];
		du2 = uv[2][0] - uv[0][0];
		dv2 = uv[2][1] - uv[0][1];
		determinant = du1 * dv2 - du2 * dv1;
		if (SDL_fabsf(determinant) < 1e-12f)
		{
			continue;
		}
		determinant = 1.0f / determinant;
		for (int c = 0; c < 3; c++)
		{
			uint32_t v = indices[i + c];

			for (int k = 0; k < 3; k++)
			{
				tangents_out[v * 4 + (uint32_t)k] += (e1[k] * dv2 - e2[k] * dv1) * determinant;
				bitangents[v * 3 + (uint32_t)k] += (e2[k] * du1 - e1[k] * du2) * determinant;
			}
		}
	}
	for (uint32_t v = 0; v < vertex_count; v++)
	{
		const float *n = (const float *)(bytes + (size_t)v * stride + normal_offset);
		float *t = &tangents_out[v * 4];
		float dot = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
		float length;
		float cross[3];

		for (int k = 0; k < 3; k++)
		{
			t[k] -= n[k] * dot;
		}
		length = SDL_sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
		if (length < 1e-10f)
		{
			// No texture direction here: any square to the normal
			float axis[3] = { SDL_fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, SDL_fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
			float along = axis[0] * n[0] + axis[1] * n[1];

			for (int k = 0; k < 3; k++)
			{
				t[k] = axis[k] - n[k] * along;
			}
			length = SDL_max(SDL_sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]), 1e-10f);
		}
		for (int k = 0; k < 3; k++)
		{
			t[k] /= length;
		}
		cross[0] = n[1] * t[2] - n[2] * t[1];
		cross[1] = n[2] * t[0] - n[0] * t[2];
		cross[2] = n[0] * t[1] - n[1] * t[0];
		t[3] = cross[0] * bitangents[v * 3] + cross[1] * bitangents[v * 3 + 1] + cross[2] * bitangents[v * 3 + 2] < 0.0f ? -1.0f : 1.0f;
	}
	SDL_free(bitangents);
}

// The tangent buffer of a model (from its vertices, or as given)
static bool upload_tangents(object_model_t *model, const float *tangents, uint32_t vertex_count)
{
	uint32_t bytes = (uint32_t)(sizeof(float) * 4 * vertex_count);

	model->tangent_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, bytes);
	if (model->tangent_buffer == NULL)
	{
		return false;
	}
	gpu_upload_buffer(model->tangent_buffer, 0, tangents, bytes);
	return true;
}

// When a file under data/ was last written (0: no such file)
static SDL_Time data_file_time(const char *relative_path)
{
	char path[1024];
	SDL_PathInfo info;

	file_io_data_path(relative_path, path, sizeof(path));
	return SDL_GetPathInfo(path, &info) ? info.modify_time : 0;
}

void object_texture_cache_initialize(object_texture_cache_t *cache, float anisotropy)
{
	SDL_zerop(cache);
	cache->anisotropy = anisotropy;
}

void object_texture_cache_destroy(object_texture_cache_t *cache)
{
	for (int i = 0; i < cache->count; i++)
	{
		SDL_ReleaseGPUTexture(gpu.device, cache->entries[i].texture);
	}
	SDL_free(cache->entries);
	SDL_zerop(cache);
}

// Keeps a texture in the cache under a path (the cache owns it from now
// on); NULL if there is no room (the texture is released)
static SDL_GPUTexture *cache_add(object_texture_cache_t *cache, const char *path, SDL_GPUTexture *texture)
{
	if (cache->count == cache->capacity)
	{
		int capacity = SDL_max(32, cache->capacity * 2);
		object_texture_entry_t *grown = SDL_realloc(cache->entries, sizeof(object_texture_entry_t) * (size_t)capacity);

		if (grown == NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, texture);
			return NULL;
		}
		cache->entries = grown;
		cache->capacity = capacity;
	}
	SDL_zero(cache->entries[cache->count]);
	SDL_strlcpy(cache->entries[cache->count].path, path, sizeof(cache->entries[cache->count].path));
	cache->entries[cache->count].texture = texture;
	cache->entries[cache->count].file_time = 0;
	cache->count++;
	return texture;
}

// The texture at the path, loaded now if this is its first use; NULL if it
// cannot be loaded. Texture names in model files are not always written
// in the same case as the files, so the lookup and the file name are
// lower case.
static SDL_GPUTexture *cached_texture_named(object_texture_cache_t *cache, const char *relative_path, bool lower_case, bool data)
{
	char path[OBJECT_MODEL_NAME_LENGTH + 128];
	SDL_GPUTexture *texture;

	SDL_strlcpy(path, relative_path, sizeof(path));
	for (char *character = path; lower_case && *character != '\0'; character++)
	{
		*character = (char)SDL_tolower((unsigned char)*character);
	}
	for (int i = 0; i < cache->count; i++)
	{
		if (SDL_strcmp(cache->entries[i].path, path) == 0)
		{
			return cache->entries[i].texture;
		}
	}
	texture = load_texture(path, data);
	if (texture == NULL)
	{
		return NULL;
	}
	texture = cache_add(cache, path, texture);
	if (texture != NULL)
	{
		cache->entries[cache->count - 1].file_time = data_file_time(path);
		cache->entries[cache->count - 1].data = data;
	}
	return texture;
}

static SDL_GPUTexture *cached_texture(object_texture_cache_t *cache, const char *relative_path)
{
	return cached_texture_named(cache, relative_path, true, false);
}

// A .pobj's texture: a reference names a file (shared, as written); else
// NULL (the pixels are in the model)
static SDL_GPUTexture *referenced_texture(object_texture_cache_t *cache, const object_file_texture_t *info, const uint8_t *data,
	const char *relative_path)
{
	char name[OBJECT_MODEL_NAME_LENGTH + 128];
	size_t length;

	if ((info->kind & OBJECT_TEXTURE_REFERENCE) == 0)
	{
		return NULL;
	}
	length = SDL_min((size_t)info->compressed_size, sizeof(name) - 1);
	SDL_memcpy(name, data, length);
	name[length] = '\0';
	if (name[0] == '\0')
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Model '%s' names a texture without a name", relative_path);
		return NULL;
	}
	return cached_texture_named(cache, name, false, (info->kind & OBJECT_TEXTURE_DATA) != 0);
}

int object_texture_cache_reload_changed(object_texture_cache_t *cache, SDL_GPUTexture **old_out, SDL_GPUTexture **new_out, int max)
{
	SDL_Time now = 0;
	int count = 0;

	SDL_GetCurrentTime(&now);
	for (int i = 0; i < cache->count && count < max; i++)
	{
		object_texture_entry_t *entry = &cache->entries[i];
		SDL_Time written;
		SDL_GPUTexture *texture;

		if (entry->file_time == 0)
		{
			continue;
		}
		written = data_file_time(entry->path);
		// Written, and not in the last half second (still being written)
		if (written == 0 || written == entry->file_time || now - written < SDL_NS_PER_SECOND / 2)
		{
			continue;
		}
		entry->file_time = written;
		texture = load_texture(entry->path, entry->data);
		if (texture == NULL)
		{
			continue;
		}
		SDL_Log("Reloaded the texture '%s'", entry->path);
		old_out[count] = entry->texture;
		new_out[count] = texture;
		entry->texture = texture;
		count++;
	}
	return count;
}

void object_texture_cache_drop(object_texture_cache_t *cache, SDL_GPUTexture *texture)
{
	for (int i = 0; i < cache->count; i++)
	{
		// Only a model's own (its name and a #): files are shared
		if (cache->entries[i].texture == texture && SDL_strchr(cache->entries[i].path, '#') != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, texture);
			cache->entries[i] = cache->entries[cache->count - 1];
			cache->count--;
			return;
		}
	}
}

bool object_model_file_changed(const object_model_t *model)
{
	SDL_Time now = 0;
	SDL_Time written = model->file_path[0] != '\0' ? data_file_time(model->file_path) : 0;

	SDL_GetCurrentTime(&now);
	return written != 0 && written != model->file_time && now - written >= SDL_NS_PER_SECOND / 2;
}

// The game's own objects (see object_format.h). Only the base color is
// drawn for now: the other maps are not even uploaded
// Whether the triangle starting at index t belongs to a solid material
// (triangles of no material count as solid)
static bool triangle_solid(const object_model_t *model, int t)
{
	for (int m = 0; m < model->material_count; m++)
	{
		const object_material_t *material = &model->materials[m];

		if (t >= material->index_offset && t < material->index_offset + material->index_count)
		{
			return material->solid;
		}
		// (its levels of detail's own meshes: drawn only)
		for (int k = 0; material->lod_meshes && k < 3; k++)
		{
			if (t >= material->lod_index_offset[k] && t < material->lod_index_offset[k] + material->lod_index_count[k])
			{
				return false;
			}
		}
	}
	return true;
}

// Puts the solid materials' triangles first in the collision indices: those
// are the collision shape (collision_index_count), the rest are only picked
// with the mouse (up to pick_index_count)
static bool solid_triangles_first(object_model_t *model)
{
	int count = model->collision_index_count - model->collision_index_count % 3;
	uint32_t *sorted;
	int filled = 0;
	int solid_count;

	model->collision_index_count = count;
	model->pick_index_count = count;
	for (int m = 0; m < model->material_count; m++)
	{
		if (!model->materials[m].solid)
		{
			break;
		}
		if (m == model->material_count - 1)
		{
			return true;               // all solid: as it is
		}
	}
	if (count == 0)
	{
		return true;
	}
	sorted = SDL_malloc(sizeof(uint32_t) * (size_t)count);
	if (sorted == NULL)
	{
		return false;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		for (int t = 0; t + 2 < count; t += 3)
		{
			if (triangle_solid(model, t) == (pass == 0))
			{
				SDL_memcpy(&sorted[filled], &model->collision_indices[t], sizeof(uint32_t) * 3);
				filled += 3;
			}
		}
		if (pass == 0)
		{
			solid_count = filled;
		}
	}
	SDL_memcpy(model->collision_indices, sorted, sizeof(uint32_t) * (size_t)count);
	SDL_free(sorted);
	model->collision_index_count = solid_count;
	return true;
}

// The materials that are only collision shapes (OBJECT_MATERIAL_COLLISION_ONLY),
// taken out once the collision shape is made: never drawn (no textures
// were made for them), their indices stay in the buffer unused
static void drop_collision_only_materials(object_model_t *model)
{
	int kept = 0;

	for (int m = 0; m < model->material_count; m++)
	{
		object_material_t *material = &model->materials[m];

		if (material->collision_only)
		{
			continue;
		}
		if (kept != m)
		{
			model->materials[kept] = *material;
		}
		kept++;
	}
	model->material_count = kept;
}

static bool load_pobj(object_model_t *model, const char *relative_path, object_texture_cache_t *cache)
{
	size_t file_size = 0;
	uint8_t *file_data = file_io_load_data_file(relative_path, &file_size);
	size_t offset = 0;
	const object_file_header_t *header;
	const object_file_vertex_t *source_vertices;
	const uint32_t *source_indices;
	const object_file_material_t *materials;
	const object_file_plant_vertex_t *plant_vertices = NULL;
	const object_file_snow_vertex_t *snow_vertices = NULL;
	const object_file_weathering_vertex_t *weathering_vertices = NULL;
	const object_file_plant_material_t *plant_materials = NULL;
	const object_file_month_cell_t *month_cells = NULL;
	const object_file_texture_t *textures[256];
	const uint8_t *texture_data[256];
	object_vertex_t *vertices = NULL;
	uint32_t *indices = NULL;
	float *tangents = NULL;
	bool ok = false;

	SDL_zerop(model);
	SDL_strlcpy(model->name, relative_path, sizeof(model->name));
	if (file_data == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot read model '%s'", relative_path);
		return false;
	}

#define FAIL(...) \
	do \
	{ \
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__); \
		goto done; \
	} \
	while (0)
#define TAKE(pointer, bytes) \
	do \
	{ \
		if ((bytes) > file_size - offset) \
		{ \
			FAIL("Model '%s' is truncated", relative_path); \
		} \
		(pointer) = (const void *)(file_data + offset); \
		offset += (bytes); \
	} \
	while (0)

	if (file_size < sizeof(object_file_header_t))
	{
		FAIL("Model '%s' is truncated", relative_path);
	}
	TAKE(header, sizeof(object_file_header_t));
	if (SDL_memcmp(header->magic, OBJECT_FILE_MAGIC, 4) != 0)
	{
		FAIL("Model '%s' is not a .pobj file", relative_path);
	}
	if (header->version < OBJECT_FILE_OLDEST_VERSION || header->version > OBJECT_FILE_VERSION)
	{
		FAIL("Model '%s' was made by another version of the converter: convert it again", relative_path);
	}
	if (header->vertex_count == 0 || header->index_count == 0 || header->material_count == 0
		|| header->material_count > OBJECT_MODEL_MAX_MATERIALS || header->texture_count > 256)
	{
		FAIL("Model '%s' has %u vertices, %u indices, %u materials, %u textures", relative_path, header->vertex_count, header->index_count,
			header->material_count, header->texture_count);
	}
	TAKE(source_vertices, sizeof(object_file_vertex_t) * (size_t)header->vertex_count);
	model->still = (header->flags & OBJECT_FILE_STILL) != 0;
	model->moss = SDL_clamp(header->moss, 0.0f, 1.0f);
	model->lichen = SDL_clamp(header->lichen, 0.0f, 1.0f);
	for (int k = 0; k < 3; k++)
	{
		model->lod_errors[k] = SDL_max(header->lod_errors[k], 0.0f);
	}
	if (header->version >= 3u && (header->flags & OBJECT_FILE_PLANT) != 0)
	{
		TAKE(plant_vertices, sizeof(object_file_plant_vertex_t) * (size_t)header->vertex_count);
		if (header->version >= 6u && (header->flags & OBJECT_FILE_SNOW) != 0)
		{
			TAKE(snow_vertices, sizeof(object_file_snow_vertex_t) * (size_t)header->vertex_count);
		}
	}
	if (header->version >= 7u && (header->flags & OBJECT_FILE_WEATHERING) != 0)
	{
		TAKE(weathering_vertices, sizeof(object_file_weathering_vertex_t) * (size_t)header->vertex_count);
	}
	TAKE(source_indices, sizeof(uint32_t) * (size_t)header->index_count);
	TAKE(materials, sizeof(object_file_material_t) * (size_t)header->material_count);
	if (plant_vertices != NULL && header->version < 5u)
	{
		// A plant made before the months: drawn as any object (make it again)
		const uint8_t *skipped;

		TAKE(skipped, (header->version == 3u ? OBJECT_FILE_PLANT_MATERIAL_V3_BYTES : OBJECT_FILE_PLANT_MATERIAL_V4_BYTES)
			* (size_t)header->material_count);
		(void)skipped;
		plant_vertices = NULL;
		SDL_Log("Model '%s' is a plant made before the months: drawn without the seasons (make it again)", relative_path);
	}
	else if (plant_vertices != NULL)
	{
		TAKE(plant_materials, sizeof(object_file_plant_material_t) * (size_t)header->material_count);
		if (header->month_cell_count > OBJECT_MODEL_MAX_MONTH_CELLS)
		{
			FAIL("Model '%s' has %u pictures with months of their own (%d at most)", relative_path, header->month_cell_count,
				OBJECT_MODEL_MAX_MONTH_CELLS);
		}
		TAKE(month_cells, sizeof(object_file_month_cell_t) * (size_t)header->month_cell_count);
		model->month_cell_count = (int)header->month_cell_count;
		SDL_memcpy(model->month_cells, month_cells, sizeof(object_file_month_cell_t) * (size_t)header->month_cell_count);
	}
	for (uint32_t t = 0; t < header->texture_count; t++)
	{
		TAKE(textures[t], sizeof(object_file_texture_t));
		TAKE(texture_data[t], (size_t)textures[t]->compressed_size);
	}
	if (header->version >= 8u && (header->flags & OBJECT_FILE_EMISSIVE_GROUPS) != 0)
	{
		const uint32_t *group_count;
		const object_file_emissive_group_t *groups;

		TAKE(group_count, sizeof(uint32_t));
		if (*group_count > OBJECT_FILE_MAX_EMISSIVE_GROUPS)
		{
			FAIL("Model '%s' has %u emissive groups (%d at most)", relative_path, *group_count, OBJECT_FILE_MAX_EMISSIVE_GROUPS);
		}
		TAKE(groups, sizeof(object_file_emissive_group_t) * (size_t)*group_count);
		model->emissive_group_count = (int)*group_count;
		SDL_memcpy(model->emissive_groups, groups, sizeof(object_file_emissive_group_t) * (size_t)*group_count);
		for (int g = 0; g < model->emissive_group_count; g++)
		{
			model->emissive_groups[g].name[OBJECT_FILE_NAME_LENGTH - 1] = '\0';
		}
	}
	if (header->version >= 9u && (header->flags & OBJECT_FILE_EFFECTS) != 0)
	{
		const uint32_t *effect_count;
		const object_file_effect_t *effects;

		TAKE(effect_count, sizeof(uint32_t));
		if (*effect_count > OBJECT_FILE_MAX_EFFECTS)
		{
			FAIL("Model '%s' has %u particle effects (%d at most)", relative_path, *effect_count, OBJECT_FILE_MAX_EFFECTS);
		}
		TAKE(effects, sizeof(object_file_effect_t) * (size_t)*effect_count);
		model->effect_count = (int)*effect_count;
		SDL_memcpy(model->effects, effects, sizeof(object_file_effect_t) * (size_t)*effect_count);
		for (int e = 0; e < model->effect_count; e++)
		{
			model->effects[e].effect[OBJECT_FILE_NAME_LENGTH - 1] = '\0';
		}
	}
	if (header->version >= 10u && (header->flags & OBJECT_FILE_FOOTPRINT) != 0)
	{
		const uint32_t *footprint_count;
		const object_file_footprint_t *footprints;

		TAKE(footprint_count, sizeof(uint32_t));
		if (*footprint_count > OBJECT_FILE_MAX_FOOTPRINTS)
		{
			FAIL("Model '%s' has %u footprints (%d at most)", relative_path, *footprint_count, OBJECT_FILE_MAX_FOOTPRINTS);
		}
		model->footprint_count = (int)*footprint_count;
		if (header->version >= 11u)
		{
			TAKE(footprints, sizeof(object_file_footprint_t) * (size_t)*footprint_count);
			SDL_memcpy(model->footprints, footprints, sizeof(object_file_footprint_t) * (size_t)*footprint_count);
		}
		else
		{
			const uint8_t *old;

			TAKE(old, OBJECT_FILE_FOOTPRINT_V10_BYTES * (size_t)*footprint_count);
			for (int f = 0; f < model->footprint_count; f++)
			{
				SDL_zero(model->footprints[f]);
				SDL_memcpy(&model->footprints[f], old + OBJECT_FILE_FOOTPRINT_V10_BYTES * (size_t)f, OBJECT_FILE_FOOTPRINT_V10_BYTES);
			}
		}
		for (int f = 0; f < model->footprint_count; f++)
		{
			model->footprints[f].cover[OBJECT_FILE_NAME_LENGTH - 1] = '\0';
			if (model->footprints[f].point_count > OBJECT_FILE_MAX_FOOTPRINT_POINTS)
			{
				model->footprints[f].point_count = OBJECT_FILE_MAX_FOOTPRINT_POINTS;
			}
		}
	}

	if (header->version >= 12u && (header->flags & OBJECT_FILE_SPOTS) != 0)
	{
		const uint32_t *spot_count;
		const object_file_spot_t *spots;

		TAKE(spot_count, sizeof(uint32_t));
		if (*spot_count > OBJECT_FILE_MAX_SPOTS)
		{
			FAIL("Model '%s' has %u spots (%d at most)", relative_path, *spot_count, OBJECT_FILE_MAX_SPOTS);
		}
		TAKE(spots, sizeof(object_file_spot_t) * (size_t)*spot_count);
		model->spot_count = (int)*spot_count;
		SDL_memcpy(model->spots, spots, sizeof(object_file_spot_t) * (size_t)*spot_count);
		for (int k = 0; k < model->spot_count; k++)
		{
			model->spots[k].name[OBJECT_FILE_NAME_LENGTH - 1] = '\0';
			for (int l = 0; l < OBJECT_FILE_SPOT_LINKS; l++)
			{
				if (model->spots[k].links[l] >= model->spot_count)
				{
					model->spots[k].links[l] = -1;
				}
			}
		}
	}
	vertices = SDL_malloc(sizeof(object_vertex_t) * header->vertex_count);
	indices = SDL_malloc(sizeof(uint32_t) * header->index_count);
	tangents = SDL_malloc(sizeof(float) * 4 * header->vertex_count);
	if (vertices == NULL || indices == NULL || tangents == NULL)
	{
		FAIL("Out of memory loading '%s'", relative_path);
	}
	model->bounds_minimum = vec3_make(1e10f, 1e10f, 1e10f);
	model->bounds_maximum = vec3_make(-1e10f, -1e10f, -1e10f);
	for (uint32_t i = 0; i < header->vertex_count; i++)
	{
		const object_file_vertex_t *source = &source_vertices[i];
		vec3_t position = vec3_make(source->position[0], source->position[1], source->position[2]);

		SDL_memcpy(vertices[i].position, source->position, sizeof(vertices[i].position));
		SDL_memcpy(vertices[i].normal, source->normal, sizeof(vertices[i].normal));
		SDL_memcpy(vertices[i].uv, source->uv, sizeof(vertices[i].uv));
		SDL_memcpy(&tangents[i * 4], source->tangent, sizeof(float) * 4);
		model->bounds_minimum = vec3_make(SDL_min(model->bounds_minimum.x, position.x), SDL_min(model->bounds_minimum.y, position.y),
			SDL_min(model->bounds_minimum.z, position.z));
		model->bounds_maximum = vec3_make(SDL_max(model->bounds_maximum.x, position.x), SDL_max(model->bounds_maximum.y, position.y),
			SDL_max(model->bounds_maximum.z, position.z));
		model->horizontal_radius = SDL_max(model->horizontal_radius, SDL_sqrtf(position.x * position.x + position.z * position.z));
	}
	for (uint32_t i = 0; i < header->index_count; i++)
	{
		if (source_indices[i] >= header->vertex_count)
		{
			FAIL("Model '%s' has an index out of range", relative_path);
		}
		indices[i] = source_indices[i];
	}

	// Materials: the base color, its texture times its factor (or the factor
	// alone), kept in the cache under the model's name and its index
	model->material_count = (int)header->material_count;
	for (uint32_t m = 0; m < header->material_count; m++)
	{
		const object_file_material_t *source = &materials[m];
		object_material_t *material = &model->materials[m];
		int32_t texture_index = source->base_color_texture;
		uint8_t *pixels = NULL;
		int width = 4;
		int height = 4;
		char key[OBJECT_MODEL_NAME_LENGTH + 128];
		SDL_GPUTexture *texture;

		if ((size_t)source->index_offset + source->index_count > header->index_count)
		{
			FAIL("Model '%s' material %u has indices out of range", relative_path, m);
		}
		material->alpha_tested = (source->flags & OBJECT_MATERIAL_ALPHA_TESTED) != 0;
		material->double_sided = (source->flags & OBJECT_MATERIAL_DOUBLE_SIDED) != 0;
		material->deciduous = (source->flags & OBJECT_MATERIAL_DECIDUOUS) != 0;
		material->solid = (source->flags & (OBJECT_MATERIAL_ALPHA_TESTED | OBJECT_MATERIAL_PASSABLE)) == 0;
		material->collision_only = (source->flags & OBJECT_MATERIAL_COLLISION_ONLY) != 0;
		material->solid = material->solid || material->collision_only;
		material->lighting_normals = (source->flags & OBJECT_MATERIAL_LIGHTING_NORMALS) != 0;
		material->triplanar = (source->flags & OBJECT_MATERIAL_TRIPLANAR) != 0;
		material->weathered = (source->flags & OBJECT_MATERIAL_WEATHERED) != 0;
		material->aged = (source->flags & OBJECT_MATERIAL_AGED) != 0 && (source->flags & OBJECT_MATERIAL_ALPHA_TESTED) == 0;
		material->thatch = material->aged && (source->flags & OBJECT_MATERIAL_THATCH) != 0;
		material->triplanar_meters = source->triplanar_meters > 0.01f ? source->triplanar_meters : 1.0f;
		material->index_offset = (int)source->index_offset;
		material->index_count = (int)source->index_count;
		material->shuffled = (source->flags & OBJECT_MATERIAL_SHUFFLED) != 0;
		if (plant_materials != NULL)
		{
			const object_file_plant_material_t *plant = &plant_materials[m];

			material->plant = true;
			SDL_memcpy(material->autumn_colors, plant->autumn_colors, sizeof(material->autumn_colors));
			material->autumn = plant->autumn;
			material->falls = plant->falls;
			material->moss = plant->moss;
			material->tips = plant->tips;
			material->sag = plant->sag;
			material->snow_cover = snow_vertices != NULL && SDL_strncmp(source->name, OBJECT_FILE_SNOW_MATERIAL, sizeof(source->name)) == 0;
			material->autumn_first_month = plant->autumn_first_month;
			material->autumn_last_month = plant->autumn_last_month;
			material->leaves_month = plant->leaves_month;
			if (plant->wind_given > 0.5f && !model->wind_given)
			{
				model->wind_given = true;
				model->sway = plant->sway;
				model->flutter = plant->flutter;
				model->stiffness = plant->stiffness;
			}
		}
		material->lods = (source->flags & OBJECT_MATERIAL_LODS) != 0;
		material->lod_meshes = material->lods && (source->flags & OBJECT_MATERIAL_LOD_MESHES) != 0;
		if (material->lod_meshes)
		{
			// The levels' own meshes, one after the other after its own
			size_t next = (size_t)source->index_offset + source->index_count;

			for (int k = 0; k < 3; k++)
			{
				uint32_t count = source->lod_index_count[k] - source->lod_index_count[k] % 3u;

				if (next + count > header->index_count)
				{
					FAIL("Model '%s' material %u: its levels of detail have indices out of range", relative_path, m);
				}
				material->lod_index_offset[k] = (int)next;
				material->lod_index_count[k] = (int)count;
				material->lod_keep_level[k] = 0;
				next += count;
			}
		}
		for (int k = 0; k < 3 && !material->lod_meshes; k++)
		{
			uint32_t kept = material->lods ? SDL_min(source->lod_index_count[k], source->index_count) : source->index_count;
			int deepest = 0;

			material->lod_index_count[k] = (int)(kept - kept % 3u);
			// The deepest branch left (the bark's tangents carry it; a cut out
			// material's tangent size is its shade, not a level)
			for (uint32_t i = 0; material->lods && !material->alpha_tested && i < kept; i++)
			{
				const float *tangent = &tangents[indices[source->index_offset + i] * 4];
				int level = (int)SDL_lroundf(SDL_fabsf(tangent[3]));

				deepest = SDL_max(deepest, level);
			}
			material->lod_keep_level[k] = deepest;
		}
		model->lods = model->lods || material->lods;
		// A collision shape alone: no textures (it is never drawn)
		if (material->collision_only)
		{
			continue;
		}
		if (texture_index >= 0 && (uint32_t)texture_index < header->texture_count
			&& (textures[texture_index]->kind & OBJECT_TEXTURE_REFERENCE) != 0)
		{
			// A file of its own, shared (its alpha cut where it is drawn)
			material->texture = referenced_texture(cache, textures[texture_index], texture_data[texture_index], relative_path);
			if (material->texture == NULL)
			{
				FAIL("Model '%s' material %u: no texture", relative_path, m);
			}
			goto shading_maps;
		}
		if (texture_index >= 0 && (uint32_t)texture_index < header->texture_count)
		{
			const object_file_texture_t *info = textures[texture_index];
			mz_ulong size = (mz_ulong)info->width * (mz_ulong)info->height * 4u;

			width = (int)info->width;
			height = (int)info->height;
			pixels = SDL_malloc((size_t)size);
			if (pixels == NULL || width <= 0 || height <= 0
				|| mz_uncompress(pixels, &size, texture_data[texture_index], (mz_ulong)info->compressed_size) != MZ_OK
				|| size != (mz_ulong)width * (mz_ulong)height * 4u)
			{
				SDL_free(pixels);
				FAIL("Model '%s' texture %d cannot be unpacked", relative_path, texture_index);
			}
		}
		else
		{
			pixels = SDL_malloc((size_t)width * (size_t)height * 4);
			if (pixels == NULL)
			{
				FAIL("Out of memory loading '%s'", relative_path);
			}
			SDL_memset(pixels, 255, (size_t)width * (size_t)height * 4);
		}
		// The factor (linear, on sRGB texels: near enough for a base color)
		for (int t = 0; t < width * height; t++)
		{
			for (int k = 0; k < 4; k++)
			{
				float value = (float)pixels[t * 4 + k] * SDL_clamp(source->base_color[k], 0.0f, 1.0f);

				pixels[t * 4 + k] = (uint8_t)SDL_clamp((int)(value + 0.5f), 0, 255);
			}
			if (material->alpha_tested)
			{
				pixels[t * 4 + 3] = (float)pixels[t * 4 + 3] >= source->alpha_cutoff * 255.0f ? 255 : 0;
			}
			else
			{
				pixels[t * 4 + 3] = 255;
			}
		}
		if (material->alpha_tested)
		{
			object_texture_bleed_transparent_colors(pixels, width, height);
		}
		texture = object_texture_upload(pixels, width, height);
		SDL_free(pixels);
		SDL_snprintf(key, sizeof(key), "%s#%u", relative_path, m);
		material->texture = texture != NULL ? cache_add(cache, key, texture) : NULL;
		if (material->texture == NULL)
		{
			FAIL("Model '%s' material %u: no texture", relative_path, m);
		}
	shading_maps:
		// The shading maps, linear (normals, orm) or sRGB (emissive) as they
		// come; a factor alone makes a small texture of its own, nothing at
		// all the neutral one
		{
			const int32_t indices_of[3] = { source->normal_texture, source->orm_texture, source->emissive_texture };
			SDL_GPUTexture **targets[3] = { &material->normal_texture, &material->orm_texture, &material->emissive_texture };

			for (int map = 0; map < 3; map++)
			{
				int32_t index = indices_of[map];
				SDL_GPUTexture *made = NULL;

				if (index >= 0 && (uint32_t)index < header->texture_count && (textures[index]->kind & OBJECT_TEXTURE_REFERENCE) != 0)
				{
					SDL_GPUTexture *shared = referenced_texture(cache, textures[index], texture_data[index], relative_path);

					*targets[map] = shared != NULL ? shared : object_default_texture((object_default_texture_t)map);
					material->shaded = material->shaded || shared != NULL;
					continue;
				}
				if (index >= 0 && (uint32_t)index < header->texture_count)
				{
					const object_file_texture_t *info = textures[index];
					mz_ulong size = (mz_ulong)info->width * (mz_ulong)info->height * 4u;
					uint8_t *map_pixels = SDL_malloc((size_t)size);

					if (map_pixels != NULL && info->width > 0 && info->height > 0
						&& mz_uncompress(map_pixels, &size, texture_data[index], (mz_ulong)info->compressed_size) == MZ_OK
						&& size == (mz_ulong)info->width * (mz_ulong)info->height * 4u)
					{
						made = object_texture_upload(map_pixels, (int)info->width, (int)info->height);
					}
					SDL_free(map_pixels);
				}
				else if (map == 1)
				{
					uint8_t color[4] = { 255, (uint8_t)SDL_clamp((int)(source->roughness * 255.0f + 0.5f), 0, 255),
						(uint8_t)SDL_clamp((int)(source->metallic * 255.0f + 0.5f), 0, 255), 255 };

					made = object_texture_solid(color);
				}
				else if (map == 2 && (source->emissive[0] > 0.0f || source->emissive[1] > 0.0f || source->emissive[2] > 0.0f))
				{
					uint8_t color[4];

					for (int k = 0; k < 3; k++)
					{
						color[k] = (uint8_t)SDL_clamp((int)(SDL_powf(SDL_clamp(source->emissive[k], 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f + 0.5f), 0, 255);
					}
					color[3] = 255;
					made = object_texture_solid(color);
				}
				if (made != NULL)
				{
					material->shaded = true;
					SDL_snprintf(key, sizeof(key), "%s#%u.%d", relative_path, m, map + 1);
					made = cache_add(cache, key, made);
				}
				*targets[map] = made != NULL ? made : object_default_texture((object_default_texture_t)map);
			}
		}
	}

	// The shape kept for collisions
	model->collision_positions = SDL_malloc(sizeof(float) * 3 * header->vertex_count);
	model->collision_indices = SDL_malloc(sizeof(uint32_t) * header->index_count);
	if (model->collision_positions == NULL || model->collision_indices == NULL)
	{
		FAIL("Out of memory loading '%s'", relative_path);
	}
	for (uint32_t i = 0; i < header->vertex_count; i++)
	{
		SDL_memcpy(&model->collision_positions[3 * i], vertices[i].position, sizeof(float) * 3);
	}
	SDL_memcpy(model->collision_indices, indices, sizeof(uint32_t) * header->index_count);
	model->collision_vertex_count = (int)header->vertex_count;
	model->collision_index_count = (int)header->index_count;
	if (!solid_triangles_first(model))
	{
		FAIL("Out of memory loading '%s'", relative_path);
	}
	drop_collision_only_materials(model);

	// GPU buffers (32 bit indices: such models are seldom small)
	model->index_size = SDL_GPU_INDEXELEMENTSIZE_32BIT;
	if (!upload_tangents(model, tangents, header->vertex_count))
	{
		FAIL("Model '%s': no room for its tangents", relative_path);
	}
	// A plant's: what each vertex is on the plant (plant.vert)
	if (plant_vertices != NULL)
	{
		uint32_t bytes = (uint32_t)(sizeof(float) * (size_t)OBJECT_PLANT_VERTEX_FLOATS * header->vertex_count);
		float *gpu_vertices = SDL_calloc(header->vertex_count, sizeof(float) * (size_t)OBJECT_PLANT_VERTEX_FLOATS);

		model->plant_buffer = gpu_vertices != NULL ? gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, bytes) : NULL;
		if (model->plant_buffer == NULL)
		{
			SDL_free(gpu_vertices);
			FAIL("Model '%s': no room for its plant data", relative_path);
		}
		// (without its snow: it stays where it is)
		for (uint32_t v = 0; v < header->vertex_count; v++)
		{
			SDL_memcpy(&gpu_vertices[(size_t)v * (size_t)OBJECT_PLANT_VERTEX_FLOATS], &plant_vertices[v], sizeof(object_file_plant_vertex_t));
			if (snow_vertices != NULL)
			{
				SDL_memcpy(&gpu_vertices[(size_t)v * (size_t)OBJECT_PLANT_VERTEX_FLOATS + 4], &snow_vertices[v], sizeof(object_file_snow_vertex_t));
			}
			// (its snow's flag and its month cell: object_plant_snow_word)
			gpu_vertices[(size_t)v * (size_t)OBJECT_PLANT_VERTEX_FLOATS + 7] = object_plant_snow_word(
				gpu_vertices[(size_t)v * (size_t)OBJECT_PLANT_VERTEX_FLOATS + 7],
				object_month_cell_of(model->month_cells, model->month_cell_count, source_vertices[v].uv));
		}
		gpu_upload_buffer(model->plant_buffer, 0, gpu_vertices, bytes);
		SDL_free(gpu_vertices);
	}
	// A building's weathering (building.vert): as baked, or open to the sky,
	// no edge, no shelter where the file has none
	{
		bool any_aged = false;

		for (int m = 0; m < model->material_count; m++)
		{
			any_aged = any_aged || model->materials[m].aged;
		}
		if (any_aged)
		{
			uint32_t bytes = (uint32_t)(sizeof(object_file_weathering_vertex_t) * header->vertex_count);
			object_file_weathering_vertex_t *gpu_weathering = SDL_malloc(bytes);

			model->weathering_buffer = gpu_weathering != NULL ? gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, bytes) : NULL;
			if (model->weathering_buffer == NULL)
			{
				SDL_free(gpu_weathering);
				FAIL("Model '%s': no room for its weathering", relative_path);
			}
			for (uint32_t v = 0; v < header->vertex_count; v++)
			{
				if (weathering_vertices != NULL)
				{
					gpu_weathering[v].sky_open = SDL_clamp(weathering_vertices[v].sky_open, 0.0f, 1.0f);
					gpu_weathering[v].edge = SDL_clamp(weathering_vertices[v].edge, 0.0f, 1.0f);
					gpu_weathering[v].shelter = SDL_clamp(weathering_vertices[v].shelter, 0.0f, 1.0f);
					gpu_weathering[v].kind = weathering_vertices[v].kind;
				}
				else
				{
					gpu_weathering[v].sky_open = 1.0f;
					gpu_weathering[v].edge = 0.0f;
					gpu_weathering[v].shelter = 0.0f;
					gpu_weathering[v].kind = OBJECT_WEATHERING_WOOD;
				}
			}
			gpu_upload_buffer(model->weathering_buffer, 0, gpu_weathering, bytes);
			SDL_free(gpu_weathering);
		}
	}
	{
		uint32_t vertex_bytes = (uint32_t)(sizeof(object_vertex_t) * header->vertex_count);
		uint32_t index_bytes = (uint32_t)(sizeof(uint32_t) * header->index_count);

		model->vertex_total = header->vertex_count;
		model->index_total = header->index_count;
		model->vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertex_bytes);
		model->index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, index_bytes);
		gpu_upload_buffer(model->vertex_buffer, 0, vertices, vertex_bytes);
		gpu_upload_buffer(model->index_buffer, 0, indices, index_bytes);
		ok = model->vertex_buffer != NULL && model->index_buffer != NULL;
	}
#undef TAKE
#undef FAIL

done:
	SDL_free(vertices);
	SDL_free(indices);
	SDL_free(tangents);
	SDL_free(file_data);
	if (!ok)
	{
		object_model_destroy(model);
	}
	return ok;
}

static bool load_e3d(object_model_t *model, const char *relative_path, object_texture_cache_t *cache)
{
	size_t file_size = 0;
	uint8_t *file_data = file_io_load_data_file(relative_path, &file_size);
	const uint8_t *header;
	uint32_t header_offset;
	uint32_t vertex_count;
	uint32_t vertex_size;
	uint32_t vertex_offset;
	uint32_t index_count;
	uint32_t index_size;
	uint32_t index_offset;
	uint32_t material_count;
	uint32_t material_size;
	uint32_t material_offset;
	uint32_t options;
	uint32_t format;
	object_vertex_t *vertices = NULL;
	void *indices = NULL;
	bool short_indices;
	char directory[OBJECT_MODEL_NAME_LENGTH];
	const char *last_slash;
	bool ok = false;

	SDL_zerop(model);
	SDL_strlcpy(model->name, relative_path, sizeof(model->name));
	if (file_data == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot read model '%s'", relative_path);
		return false;
	}

#define FAIL(...) \
	do \
	{ \
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__); \
		goto done; \
	} \
	while (0)

	if (file_size < ELC_HEADER_SIZE || SDL_memcmp(file_data, "e3dx", 4) != 0)
	{
		FAIL("Model '%s' is not an e3d file", relative_path);
	}
	header_offset = read_u32(file_data + 24);
	if ((size_t)header_offset + E3D_HEADER_SIZE > file_size)
	{
		FAIL("Model '%s' is truncated", relative_path);
	}
	header = file_data + header_offset;
	vertex_count = read_u32(header + 0);
	vertex_size = read_u32(header + 4);
	vertex_offset = read_u32(header + 8);
	index_count = read_u32(header + 12);
	index_size = read_u32(header + 16);
	index_offset = read_u32(header + 20);
	material_count = read_u32(header + 24);
	material_size = read_u32(header + 28);
	material_offset = read_u32(header + 32);
	options = header[36];
	format = header[37];

	// Version 1.0 had no format byte and the normal bit inverted
	if (file_data[4] == 1 && file_data[5] == 0)
	{
		format = 0;
		options = (options ^ E3D_HAS_NORMAL) & 0x07;
	}
	else if (file_data[4] == 1 && file_data[5] == 1)
	{
		options &= 0x0f;
		format &= 0x1f;
	}
	else
	{
		FAIL("Model '%s' has unknown version %d.%d", relative_path, file_data[4], file_data[5]);
	}
	if ((int)vertex_size != expected_vertex_size(options, format))
	{
		FAIL("Model '%s' has vertex size %u, expected %d", relative_path, vertex_size, expected_vertex_size(options, format));
	}
	if (index_size != 2 && index_size != 4)
	{
		FAIL("Model '%s' has index size %u", relative_path, index_size);
	}
	if (material_size < E3D_MATERIAL_SIZE || material_count == 0 || material_count > OBJECT_MODEL_MAX_MATERIALS)
	{
		FAIL("Model '%s' has %u materials of %u bytes", relative_path, material_count, material_size);
	}
	if ((size_t)vertex_offset + (size_t)vertex_count * vertex_size > file_size
		|| (size_t)index_offset + (size_t)index_count * index_size > file_size
		|| (size_t)material_offset + (size_t)material_count * material_size > file_size)
	{
		FAIL("Model '%s' is truncated", relative_path);
	}

	// Vertices
	vertices = SDL_malloc(sizeof(object_vertex_t) * vertex_count);
	if (vertices == NULL)
	{
		FAIL("Out of memory loading '%s'", relative_path);
	}
	for (uint32_t i = 0; i < vertex_count; i++)
	{
		read_vertex(file_data + vertex_offset + (size_t)i * vertex_size, options, format, &vertices[i]);
	}

	// Indices, 32 bit (16 would do for most, but the objects' shared buffer,
	// objects.c, takes every model's as they are, in one kind)
	short_indices = false;
	indices = SDL_malloc((size_t)index_count * (short_indices ? 2 : 4));
	if (indices == NULL)
	{
		FAIL("Out of memory loading '%s'", relative_path);
	}
	for (uint32_t i = 0; i < index_count; i++)
	{
		const uint8_t *source = file_data + index_offset + (size_t)i * index_size;
		uint32_t value = index_size == 2 ? read_u16(source) : read_u32(source);

		if (value >= vertex_count)
		{
			FAIL("Model '%s' has an index out of range", relative_path);
		}
		if (short_indices)
		{
			((uint16_t *)indices)[i] = (uint16_t)value;
		}
		else
		{
			((uint32_t *)indices)[i] = value;
		}
	}

	// Materials: textures live next to the model
	SDL_strlcpy(directory, relative_path, sizeof(directory));
	last_slash = SDL_strrchr(directory, '/');
	if (last_slash != NULL)
	{
		directory[last_slash - directory + 1] = '\0';
	}
	else
	{
		directory[0] = '\0';
	}
	model->bounds_minimum = vec3_make(1e10f, 1e10f, 1e10f);
	model->bounds_maximum = vec3_make(-1e10f, -1e10f, -1e10f);
	model->material_count = (int)material_count;
	for (uint32_t i = 0; i < material_count; i++)
	{
		const uint8_t *source = file_data + material_offset + (size_t)i * material_size;
		object_material_t *material = &model->materials[i];
		char texture_name[129];
		char texture_path[OBJECT_MODEL_NAME_LENGTH + 128];
		float minimum[3] = { read_f32(source + 132), read_f32(source + 136), read_f32(source + 140) };
		float maximum[3] = { read_f32(source + 144), read_f32(source + 148), read_f32(source + 152) };
		float corner_a[3];
		float corner_b[3];

		SDL_memcpy(texture_name, source + 4, 128);
		texture_name[128] = '\0';
		material->alpha_tested = read_u32(source) != 0;
		material->solid = !material->alpha_tested;
		material->index_offset = (int)read_u32(source + 164);
		material->index_count = (int)read_u32(source + 168);
		if ((uint32_t)material->index_offset + (uint32_t)material->index_count > index_count)
		{
			FAIL("Model '%s' material %u has indices out of range", relative_path, i);
		}
		SDL_snprintf(texture_path, sizeof(texture_path), "%s%s", directory, texture_name);
		material->texture = cached_texture(cache, texture_path);
		if (material->texture == 0)
		{
			goto done;
		}
		material->normal_texture = object_default_texture(OBJECT_DEFAULT_NORMAL);
		material->orm_texture = object_default_texture(OBJECT_DEFAULT_ORM);
		material->emissive_texture = object_default_texture(OBJECT_DEFAULT_EMISSIVE);
		material->shaded = false;
		// The box in game axes: y and z swap, z flips sign
		model_to_game(minimum, corner_a);
		model_to_game(maximum, corner_b);
		model->bounds_minimum = vec3_make(SDL_min(model->bounds_minimum.x, SDL_min(corner_a[0], corner_b[0])),
			SDL_min(model->bounds_minimum.y, SDL_min(corner_a[1], corner_b[1])),
			SDL_min(model->bounds_minimum.z, SDL_min(corner_a[2], corner_b[2])));
		model->bounds_maximum = vec3_make(SDL_max(model->bounds_maximum.x, SDL_max(corner_a[0], corner_b[0])),
			SDL_max(model->bounds_maximum.y, SDL_max(corner_a[1], corner_b[1])),
			SDL_max(model->bounds_maximum.z, SDL_max(corner_a[2], corner_b[2])));
	}
	for (uint32_t i = 0; i < vertex_count; i++)
	{
		float distance = SDL_sqrtf(vertices[i].position[0] * vertices[i].position[0] + vertices[i].position[2] * vertices[i].position[2]);

		model->horizontal_radius = SDL_max(model->horizontal_radius, distance);
	}

	// The shape kept for collisions
	model->collision_positions = SDL_malloc(sizeof(float) * 3 * vertex_count);
	model->collision_indices = SDL_malloc(sizeof(uint32_t) * index_count);
	if (model->collision_positions == NULL || model->collision_indices == NULL)
	{
		FAIL("Out of memory loading '%s'", relative_path);
	}
	for (uint32_t i = 0; i < vertex_count; i++)
	{
		SDL_memcpy(&model->collision_positions[3 * i], vertices[i].position, sizeof(float) * 3);
	}
	for (uint32_t i = 0; i < index_count; i++)
	{
		model->collision_indices[i] = short_indices ? ((uint16_t *)indices)[i] : ((uint32_t *)indices)[i];
	}
	model->collision_vertex_count = (int)vertex_count;
	model->collision_index_count = (int)index_count;
	if (!solid_triangles_first(model))
	{
		FAIL("Out of memory loading '%s'", relative_path);
	}

	// Tangents: .e3d has none the game reads (they would only matter for a
	// normal map, which it does not have either; made all the same, so
	// every object draws alike)
	{
		float *tangents = SDL_malloc(sizeof(float) * 4 * vertex_count);

		if (tangents == NULL)
		{
			FAIL("Out of memory loading '%s'", relative_path);
		}
		object_compute_tangents(vertices, sizeof(object_vertex_t), offsetof(object_vertex_t, position), offsetof(object_vertex_t, normal),
			offsetof(object_vertex_t, uv), vertex_count, model->collision_indices, index_count, tangents);
		if (!upload_tangents(model, tangents, vertex_count))
		{
			SDL_free(tangents);
			FAIL("Model '%s': no room for its tangents", relative_path);
		}
		SDL_free(tangents);
	}

	// GPU buffers
	model->index_size = short_indices ? SDL_GPU_INDEXELEMENTSIZE_16BIT : SDL_GPU_INDEXELEMENTSIZE_32BIT;
	{
		uint32_t vertex_bytes = (uint32_t)(sizeof(object_vertex_t) * vertex_count);
		uint32_t index_bytes = (uint32_t)((size_t)index_count * (short_indices ? 2 : 4));

		model->vertex_total = vertex_count;
		model->index_total = index_count;
		model->vertex_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertex_bytes);
		model->index_buffer = gpu_create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, index_bytes);
		gpu_upload_buffer(model->vertex_buffer, 0, vertices, vertex_bytes);
		gpu_upload_buffer(model->index_buffer, 0, indices, index_bytes);
		ok = model->vertex_buffer != NULL && model->index_buffer != NULL;
	}
#undef FAIL

done:
	SDL_free(vertices);
	SDL_free(indices);
	SDL_free(file_data);
	if (!ok)
	{
		object_model_destroy(model);
	}
	return ok;
}

bool object_model_load(object_model_t *model, const char *relative_path, object_texture_cache_t *cache)
{
	size_t length = SDL_strlen(relative_path);

	if (length > 5 && SDL_strcmp(relative_path + length - 5, ".pobj") == 0)
	{
		bool loaded = load_pobj(model, relative_path, cache);

		if (loaded)
		{
			SDL_strlcpy(model->file_path, relative_path, sizeof(model->file_path));
			model->file_time = data_file_time(relative_path);
			model->own_textures = true;
		}
		return loaded;
	}
	// An object named in objects.ini is looked for as .e3d: a .pobj of the
	// same name, there too, comes first
	if (length > 4 && SDL_strcmp(relative_path + length - 4, ".e3d") == 0 && length < OBJECT_MODEL_NAME_LENGTH)
	{
		char own[OBJECT_MODEL_NAME_LENGTH + 8];

		SDL_snprintf(own, sizeof(own), "%.*s.pobj", (int)(length - 4), relative_path);
		if (file_io_data_file_exists(own))
		{
			bool loaded = load_pobj(model, own, cache);

			// Known by the name it was asked for (models are shared by it)
			if (loaded)
			{
				SDL_strlcpy(model->name, relative_path, sizeof(model->name));
				SDL_strlcpy(model->file_path, own, sizeof(model->file_path));
				model->file_time = data_file_time(own);
				model->own_textures = true;
			}
			return loaded;
		}
	}
	if (!load_e3d(model, relative_path, cache))
	{
		return false;
	}
	SDL_strlcpy(model->file_path, relative_path, sizeof(model->file_path));
	model->file_time = data_file_time(relative_path);
	model->own_textures = false;
	return true;
}

void object_model_destroy(object_model_t *model)
{
	SDL_free(model->collision_positions);
	SDL_free(model->collision_indices);
	if (model->vertex_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, model->vertex_buffer);
	}
	if (model->weathering_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, model->weathering_buffer);
		model->weathering_buffer = NULL;
	}
	if (model->plant_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, model->plant_buffer);
		model->plant_buffer = NULL;
	}
	if (model->tangent_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, model->tangent_buffer);
	}
	if (model->index_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, model->index_buffer);
	}
	SDL_zerop(model);
}
