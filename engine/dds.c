// DDS decoding. See dds.h.

#include "dds.h"

#include <SDL3/SDL.h>

#define DDS_HEADER_SIZE 128           // magic + 124 byte header
#define DDS_PIXEL_FORMAT_FOURCC 0x4
#define DDS_PIXEL_FORMAT_RGB 0x40
#define DDS_PIXEL_FORMAT_ALPHA_PIXELS 0x1

static uint32_t read_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint16_t read_u16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

// RGB565 to 8 bit channels
static void unpack_565(uint16_t color, uint8_t rgb_out[3])
{
	int red = (color >> 11) & 31;
	int green = (color >> 5) & 63;
	int blue = color & 31;

	rgb_out[0] = (uint8_t)((red << 3) | (red >> 2));
	rgb_out[1] = (uint8_t)((green << 2) | (green >> 4));
	rgb_out[2] = (uint8_t)((blue << 3) | (blue >> 2));
}

// The color part of a DXT block (8 bytes) into 16 RGBA texels. DXT1 blocks
// whose first color is not larger than the second have a transparent
// fourth color; DXT3 and DXT5 always use four opaque colors.
static void decode_color_block(const uint8_t *block, bool four_colors_always, uint8_t texels_out[16][4])
{
	uint16_t color_0 = read_u16(block);
	uint16_t color_1 = read_u16(block + 2);
	uint32_t selectors = read_u32(block + 4);
	uint8_t palette[4][4];

	unpack_565(color_0, palette[0]);
	unpack_565(color_1, palette[1]);
	palette[0][3] = 255;
	palette[1][3] = 255;
	if (color_0 > color_1 || four_colors_always)
	{
		for (int k = 0; k < 3; k++)
		{
			palette[2][k] = (uint8_t)((2 * palette[0][k] + palette[1][k] + 1) / 3);
			palette[3][k] = (uint8_t)((palette[0][k] + 2 * palette[1][k] + 1) / 3);
		}
		palette[2][3] = 255;
		palette[3][3] = 255;
	}
	else
	{
		for (int k = 0; k < 3; k++)
		{
			palette[2][k] = (uint8_t)((palette[0][k] + palette[1][k]) / 2);
			palette[3][k] = 0;
		}
		palette[2][3] = 255;
		palette[3][3] = 0;
	}
	for (int i = 0; i < 16; i++)
	{
		int selector = (int)((selectors >> (2 * i)) & 3);

		SDL_memcpy(texels_out[i], palette[selector], 4);
	}
}

// DXT3 alpha: 16 explicit 4 bit values
static void decode_explicit_alpha(const uint8_t *block, uint8_t texels[16][4])
{
	for (int i = 0; i < 16; i++)
	{
		int value = (block[i / 2] >> ((i % 2) * 4)) & 15;

		texels[i][3] = (uint8_t)(value * 17);
	}
}

// DXT5 alpha: two end points and 3 bit selectors
static void decode_interpolated_alpha(const uint8_t *block, uint8_t texels[16][4])
{
	int alpha_0 = block[0];
	int alpha_1 = block[1];
	int palette[8];
	uint64_t selectors = 0;

	for (int i = 0; i < 6; i++)
	{
		selectors |= (uint64_t)block[2 + i] << (8 * i);
	}
	palette[0] = alpha_0;
	palette[1] = alpha_1;
	if (alpha_0 > alpha_1)
	{
		for (int i = 1; i < 7; i++)
		{
			palette[1 + i] = ((7 - i) * alpha_0 + i * alpha_1 + 3) / 7;
		}
	}
	else
	{
		for (int i = 1; i < 5; i++)
		{
			palette[1 + i] = ((5 - i) * alpha_0 + i * alpha_1 + 2) / 5;
		}
		palette[6] = 0;
		palette[7] = 255;
	}
	for (int i = 0; i < 16; i++)
	{
		texels[i][3] = (uint8_t)palette[(selectors >> (3 * i)) & 7];
	}
}

// Position and width of the lowest set bit of a channel mask
static void mask_shift(uint32_t mask, int *shift_out, int *bits_out)
{
	int shift = 0;
	int bits = 0;

	if (mask != 0)
	{
		while (((mask >> shift) & 1) == 0)
		{
			shift++;
		}
		while (((mask >> (shift + bits)) & 1) != 0)
		{
			bits++;
		}
	}
	*shift_out = shift;
	*bits_out = bits;
}

static uint8_t extract_channel(uint32_t pixel, uint32_t mask, uint8_t missing)
{
	int shift;
	int bits;
	uint32_t value;

	if (mask == 0)
	{
		return missing;
	}
	mask_shift(mask, &shift, &bits);
	value = (pixel & mask) >> shift;
	if (bits >= 8)
	{
		return (uint8_t)(value >> (bits - 8));
	}
	return (uint8_t)(value * 255u / ((1u << bits) - 1u));
}

bool dds_decode(const void *data, size_t size, uint8_t **pixels_out, int *width_out, int *height_out, bool *has_alpha_out)
{
	const uint8_t *bytes = data;
	const uint8_t *source;
	uint32_t height;
	uint32_t width;
	uint32_t format_flags;
	uint32_t four_cc;
	uint8_t *pixels;
	size_t source_size;

	*pixels_out = NULL;
	if (size < DDS_HEADER_SIZE || SDL_memcmp(bytes, "DDS ", 4) != 0 || read_u32(bytes + 4) != 124)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Not a DDS file");
		return false;
	}
	height = read_u32(bytes + 12);
	width = read_u32(bytes + 16);
	format_flags = read_u32(bytes + 80);
	four_cc = read_u32(bytes + 84);
	if (width == 0 || height == 0 || width > 16384 || height > 16384)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DDS file has a strange size: %u x %u", width, height);
		return false;
	}
	source = bytes + DDS_HEADER_SIZE;
	source_size = size - DDS_HEADER_SIZE;
	pixels = SDL_malloc((size_t)width * height * 4);
	if (pixels == NULL)
	{
		return false;
	}

	if (format_flags & DDS_PIXEL_FORMAT_FOURCC)
	{
		bool is_dxt1 = four_cc == read_u32((const uint8_t *)"DXT1");
		bool is_dxt3 = four_cc == read_u32((const uint8_t *)"DXT3");
		bool is_dxt5 = four_cc == read_u32((const uint8_t *)"DXT5");
		uint32_t blocks_wide = (width + 3) / 4;
		uint32_t blocks_high = (height + 3) / 4;
		size_t block_size = is_dxt1 ? 8 : 16;

		if (!is_dxt1 && !is_dxt3 && !is_dxt5)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DDS compression '%.4s' is not supported", (const char *)(bytes + 84));
			SDL_free(pixels);
			return false;
		}
		if (source_size < (size_t)blocks_wide * blocks_high * block_size)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DDS file is truncated");
			SDL_free(pixels);
			return false;
		}
		for (uint32_t block_y = 0; block_y < blocks_high; block_y++)
		{
			for (uint32_t block_x = 0; block_x < blocks_wide; block_x++)
			{
				const uint8_t *block = source + ((size_t)block_y * blocks_wide + block_x) * block_size;
				uint8_t texels[16][4];

				if (is_dxt1)
				{
					decode_color_block(block, false, texels);
				}
				else
				{
					decode_color_block(block + 8, true, texels);
					if (is_dxt3)
					{
						decode_explicit_alpha(block, texels);
					}
					else
					{
						decode_interpolated_alpha(block, texels);
					}
				}
				for (int y = 0; y < 4; y++)
				{
					for (int x = 0; x < 4; x++)
					{
						uint32_t pixel_x = block_x * 4 + (uint32_t)x;
						uint32_t pixel_y = block_y * 4 + (uint32_t)y;

						if (pixel_x < width && pixel_y < height)
						{
							SDL_memcpy(&pixels[((size_t)pixel_y * width + pixel_x) * 4], texels[y * 4 + x], 4);
						}
					}
				}
			}
		}
		*has_alpha_out = !is_dxt1;
	}
	else if (format_flags & DDS_PIXEL_FORMAT_RGB)
	{
		uint32_t bit_count = read_u32(bytes + 88);
		uint32_t red_mask = read_u32(bytes + 92);
		uint32_t green_mask = read_u32(bytes + 96);
		uint32_t blue_mask = read_u32(bytes + 100);
		uint32_t alpha_mask = (format_flags & DDS_PIXEL_FORMAT_ALPHA_PIXELS) ? read_u32(bytes + 104) : 0;
		uint32_t bytes_per_pixel = bit_count / 8;

		if ((bit_count != 24 && bit_count != 32) || source_size < (size_t)width * height * bytes_per_pixel)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DDS pixel format (%u bits) is not supported", bit_count);
			SDL_free(pixels);
			return false;
		}
		for (size_t i = 0; i < (size_t)width * height; i++)
		{
			const uint8_t *texel = source + i * bytes_per_pixel;
			uint32_t pixel = texel[0] | ((uint32_t)texel[1] << 8) | ((uint32_t)texel[2] << 16)
				| (bytes_per_pixel == 4 ? ((uint32_t)texel[3] << 24) : 0u);

			pixels[i * 4 + 0] = extract_channel(pixel, red_mask, 0);
			pixels[i * 4 + 1] = extract_channel(pixel, green_mask, 0);
			pixels[i * 4 + 2] = extract_channel(pixel, blue_mask, 0);
			pixels[i * 4 + 3] = extract_channel(pixel, alpha_mask, 255);
		}
		*has_alpha_out = alpha_mask != 0;
	}
	else
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DDS pixel format is not supported");
		SDL_free(pixels);
		return false;
	}
	*pixels_out = pixels;
	*width_out = (int)width;
	*height_out = (int)height;
	return true;
}
