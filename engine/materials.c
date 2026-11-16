// Terrain surface materials. See materials.h.

#include "materials.h"
#include "noise_lattice.h"
#include "file_io.h"
#include "ini_parser.h"
#include "math3d.h"
#include "noise.h"
#include "texture_cache.h"

#include <SDL3/SDL.h>
#include "stb_image.h"

typedef struct
{
	char texture_names[MATERIAL_MAX_VARIANTS][64];
	int texture_name_count;
	float tiling_meters;           // one texture repeat covers this many meters
	float macro_tiling_meters;     // the same, for the large scale seen from afar
	bool macro_tiling_given;       // false: derived from tiling_meters
	float fallback_dark[3];        // procedural fallback colors (0..1, sRGB)
	float fallback_light[3];
	int fallback_period;           // noise periods across the fallback texture
	// Where it meets another: patches of it in the other (terrain.frag)
	float patch_meters;            // how big its patches (meters)
	float patch_edge_meters;       // how soft their edges
	float patch_strength;          // how far they break the border (0: a plain blend)
	bool normal_green_down;        // its _normal files' green points down the picture (DirectX's); else up (OpenGL's)
	char macro_texture[64];        // its own texture for the macro scale (empty: its variants)
	char relief[64];               // its relief's normal map (NAME_normal.png, its height NAME_height.png; empty: none)
	char wet_texture[64];          // what it looks like soaked (empty: itself)
	float roughness;               // without a _roughness file (0 mirror .. 1 matte)
	float wet_roughness;           // its wet texture's, the same
	float relief_meters;           // how far one repeat of it goes
} material_description_t;

static const char *material_names[MATERIAL_COUNT] = { "grass", "dirt", "rock", "sand", "clay", "snow", "silt", "needles", "leaves", "trampled",
	"petals", "trampled_dirt" };

const char *materials_name(material_type_t material)
{
	return (material >= 0 && material < MATERIAL_COUNT) ? material_names[material] : "?";
}

static void set_color(float color_out[3], int red, int green, int blue)
{
	color_out[0] = (float)red / 255.0f;
	color_out[1] = (float)green / 255.0f;
	color_out[2] = (float)blue / 255.0f;
}

static void set_default_descriptions(material_description_t descriptions[MATERIAL_COUNT])
{
	SDL_memset(descriptions, 0, sizeof(material_description_t) * MATERIAL_COUNT);

	descriptions[MATERIAL_GRASS].tiling_meters = 3.0f;
	set_color(descriptions[MATERIAL_GRASS].fallback_dark, 48, 74, 30);
	set_color(descriptions[MATERIAL_GRASS].fallback_light, 104, 134, 58);
	descriptions[MATERIAL_GRASS].fallback_period = 16;

	descriptions[MATERIAL_DIRT].tiling_meters = 3.0f;
	set_color(descriptions[MATERIAL_DIRT].fallback_dark, 74, 56, 40);
	set_color(descriptions[MATERIAL_DIRT].fallback_light, 128, 102, 74);
	descriptions[MATERIAL_DIRT].fallback_period = 8;

	descriptions[MATERIAL_ROCK].tiling_meters = 5.0f;
	set_color(descriptions[MATERIAL_ROCK].fallback_dark, 84, 82, 80);
	set_color(descriptions[MATERIAL_ROCK].fallback_light, 150, 146, 138);
	descriptions[MATERIAL_ROCK].fallback_period = 4;

	descriptions[MATERIAL_SAND].tiling_meters = 3.0f;
	set_color(descriptions[MATERIAL_SAND].fallback_dark, 170, 150, 108);
	set_color(descriptions[MATERIAL_SAND].fallback_light, 214, 198, 154);
	descriptions[MATERIAL_SAND].fallback_period = 16;

	descriptions[MATERIAL_CLAY].tiling_meters = 4.0f;
	set_color(descriptions[MATERIAL_CLAY].fallback_dark, 112, 82, 60);
	set_color(descriptions[MATERIAL_CLAY].fallback_light, 156, 118, 86);
	descriptions[MATERIAL_CLAY].fallback_period = 8;

	descriptions[MATERIAL_SNOW].tiling_meters = 4.0f;
	set_color(descriptions[MATERIAL_SNOW].fallback_dark, 212, 220, 232);
	set_color(descriptions[MATERIAL_SNOW].fallback_light, 248, 250, 255);
	descriptions[MATERIAL_SNOW].fallback_period = 8;

	descriptions[MATERIAL_SILT].tiling_meters = 3.0f;
	set_color(descriptions[MATERIAL_SILT].fallback_dark, 88, 78, 62);
	set_color(descriptions[MATERIAL_SILT].fallback_light, 128, 116, 94);
	descriptions[MATERIAL_SILT].fallback_period = 16;

	// The covers and the petals: textures by the names make_textures.sh
	// gives them (tools/potop_texture), else the fallback
	{
		static const char *const names[] = { "needles_01", "leaves_01", "trampled_01", "petals_01" };

		for (int i = 0; i < 4; i++)
		{
			SDL_strlcpy(descriptions[MATERIAL_NEEDLES + i].texture_names[0], names[i], sizeof(descriptions[MATERIAL_NEEDLES + i].texture_names[0]));
			descriptions[MATERIAL_NEEDLES + i].texture_name_count = 1;
			descriptions[MATERIAL_NEEDLES + i].tiling_meters = 2.0f;
			descriptions[MATERIAL_NEEDLES + i].fallback_period = 16;
		}
	}
	// (snow: make_textures.sh's, with its mask)
	SDL_strlcpy(descriptions[MATERIAL_SNOW].texture_names[0], "snow_01", sizeof(descriptions[MATERIAL_SNOW].texture_names[0]));
	descriptions[MATERIAL_SNOW].texture_name_count = 1;
	set_color(descriptions[MATERIAL_NEEDLES].fallback_dark, 70, 46, 28);
	set_color(descriptions[MATERIAL_NEEDLES].fallback_light, 132, 88, 50);
	set_color(descriptions[MATERIAL_LEAVES].fallback_dark, 92, 54, 24);
	set_color(descriptions[MATERIAL_LEAVES].fallback_light, 168, 110, 44);
	set_color(descriptions[MATERIAL_TRAMPLED].fallback_dark, 84, 70, 52);
	set_color(descriptions[MATERIAL_TRAMPLED].fallback_light, 124, 108, 82);
	set_color(descriptions[MATERIAL_PETALS].fallback_dark, 226, 170, 186);
	set_color(descriptions[MATERIAL_PETALS].fallback_light, 252, 226, 234);
	// (a building's floor on the ground: data/textures/trampled_dirt.png)
	SDL_strlcpy(descriptions[MATERIAL_TRAMPLED_DIRT].texture_names[0], "trampled_dirt", sizeof(descriptions[MATERIAL_TRAMPLED_DIRT].texture_names[0]));
	descriptions[MATERIAL_TRAMPLED_DIRT].texture_name_count = 1;
	descriptions[MATERIAL_TRAMPLED_DIRT].tiling_meters = 2.0f;
	descriptions[MATERIAL_TRAMPLED_DIRT].fallback_period = 8;
	set_color(descriptions[MATERIAL_TRAMPLED_DIRT].fallback_dark, 70, 52, 36);
	set_color(descriptions[MATERIAL_TRAMPLED_DIRT].fallback_light, 116, 90, 64);

	// Patches: big soft ones of grass and snow, smaller ragged ones of the
	// bare grounds
	{
		static const float patch_meters[MATERIAL_COUNT] = { 4.0f, 2.0f, 3.0f, 3.0f, 2.5f, 5.0f, 2.0f, 2.0f, 2.0f, 3.0f, 2.0f, 1.5f };

		for (int i = 0; i < MATERIAL_COUNT; i++)
		{
			descriptions[i].patch_meters = patch_meters[i];
			descriptions[i].patch_edge_meters = 0.03f;
			descriptions[i].patch_strength = 0.6f;
			descriptions[i].relief_meters = 96.0f;
			descriptions[i].roughness = i == MATERIAL_SNOW ? 0.6f : (i == MATERIAL_ROCK ? 0.75f : 0.9f);
			descriptions[i].wet_roughness = 0.35f;
		}
	}
}

static bool material_key_callback(void *user_data, const char *section, const char *key, const char *value)
{
	material_description_t *descriptions = user_data;
	material_description_t *description = NULL;

	for (int i = 0; i < MATERIAL_COUNT; i++)
	{
		if (SDL_strcmp(section, material_names[i]) == 0)
		{
			description = &descriptions[i];
		}
	}
	if (description == NULL)
	{
		return false;
	}

	if (SDL_strcmp(key, "textures") == 0)
	{
		char copy[512];
		char *items[MATERIAL_MAX_VARIANTS];
		int item_count;

		SDL_strlcpy(copy, value, sizeof(copy));
		item_count = ini_split_list(copy, items, MATERIAL_MAX_VARIANTS);
		for (int i = 0; i < item_count; i++)
		{
			SDL_strlcpy(description->texture_names[i], items[i], sizeof(description->texture_names[i]));
		}
		description->texture_name_count = item_count;
		return true;
	}
	if (SDL_strcmp(key, "tiling_meters") == 0)
	{
		return ini_value_to_float(value, &description->tiling_meters) && description->tiling_meters > 0.0f;
	}
	if (SDL_strcmp(key, "macro_tiling_meters") == 0)
	{
		description->macro_tiling_given = true;
		return ini_value_to_float(value, &description->macro_tiling_meters) && description->macro_tiling_meters > 0.0f;
	}
	if (SDL_strcmp(key, "fallback_dark") == 0)
	{
		return ini_value_to_color(value, description->fallback_dark);
	}
	if (SDL_strcmp(key, "fallback_light") == 0)
	{
		return ini_value_to_color(value, description->fallback_light);
	}
	if (SDL_strcmp(key, "patch_meters") == 0)
	{
		return ini_value_to_float(value, &description->patch_meters) && description->patch_meters > 0.01f;
	}
	if (SDL_strcmp(key, "patch_edge") == 0)
	{
		return ini_value_to_float(value, &description->patch_edge_meters) && description->patch_edge_meters >= 0.0f;
	}
	if (SDL_strcmp(key, "patch_strength") == 0)
	{
		return ini_value_to_float(value, &description->patch_strength) && description->patch_strength >= 0.0f;
	}
	if (SDL_strcmp(key, "macro_texture") == 0)
	{
		SDL_strlcpy(description->macro_texture, value, sizeof(description->macro_texture));
		return true;
	}
	if (SDL_strcmp(key, "roughness") == 0)
	{
		return ini_value_to_float(value, &description->roughness);
	}
	if (SDL_strcmp(key, "wet_roughness") == 0)
	{
		return ini_value_to_float(value, &description->wet_roughness);
	}
	if (SDL_strcmp(key, "wet_texture") == 0)
	{
		SDL_strlcpy(description->wet_texture, value, sizeof(description->wet_texture));
		return true;
	}
	if (SDL_strcmp(key, "relief") == 0)
	{
		SDL_strlcpy(description->relief, value, sizeof(description->relief));
		return true;
	}
	if (SDL_strcmp(key, "relief_meters") == 0)
	{
		return ini_value_to_float(value, &description->relief_meters) && description->relief_meters > 0.5f;
	}
	if (SDL_strcmp(key, "normal_green_down") == 0)
	{
		return ini_value_to_bool(value, &description->normal_green_down);
	}
	if (SDL_strcmp(key, "fallback_period") == 0)
	{
		return ini_value_to_int(value, &description->fallback_period) && description->fallback_period >= 1;
	}
	return false;
}

// Bilinear resampling with wrap-around, so tileable textures stay tileable
// after resizing. Works on 8 bit images with any channel count.
static void resample_wrapping(const uint8_t *source, int source_width, int source_height, int channels,
	uint8_t *destination, int destination_size, int destination_stride, int destination_channel_offset)
{
	for (int y = 0; y < destination_size; y++)
	{
		float source_y = ((float)y + 0.5f) * (float)source_height / (float)destination_size - 0.5f;
		int y0 = (int)SDL_floorf(source_y);
		float fraction_y = source_y - (float)y0;
		int row_0 = ((y0 % source_height) + source_height) % source_height;
		int row_1 = (row_0 + 1) % source_height;

		for (int x = 0; x < destination_size; x++)
		{
			float source_x = ((float)x + 0.5f) * (float)source_width / (float)destination_size - 0.5f;
			int x0 = (int)SDL_floorf(source_x);
			float fraction_x = source_x - (float)x0;
			int column_0 = ((x0 % source_width) + source_width) % source_width;
			int column_1 = (column_0 + 1) % source_width;

			for (int channel = 0; channel < channels; channel++)
			{
				float value_00 = source[(row_0 * source_width + column_0) * channels + channel];
				float value_10 = source[(row_0 * source_width + column_1) * channels + channel];
				float value_01 = source[(row_1 * source_width + column_0) * channels + channel];
				float value_11 = source[(row_1 * source_width + column_1) * channels + channel];
				float top = value_00 + (value_10 - value_00) * fraction_x;
				float bottom = value_01 + (value_11 - value_01) * fraction_x;
				float value = top + (bottom - top) * fraction_y;

				destination[(y * destination_size + x) * destination_stride + destination_channel_offset + channel] =
					(uint8_t)clamp_float(value + 0.5f, 0.0f, 255.0f);
			}
		}
	}
}

// Tries name.png, name.jpg, name.bmp in data/textures/. Returns stb pixels
// (free with stbi_image_free) with the requested channel count.
static uint8_t *load_image_any_format(const char *base_name, int wanted_channels, int *width_out, int *height_out)
{
	static const char *extensions[] = { ".png", ".jpg", ".bmp" };

	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
	{
		char path[256];
		size_t file_size = 0;
		void *file_data;
		uint8_t *pixels;
		int channels_in_file;

		SDL_snprintf(path, sizeof(path), "textures/%s%s", base_name, extensions[i]);
		if (!file_io_data_file_exists(path))
		{
			continue;
		}
		file_data = file_io_load_data_file(path, &file_size);
		if (file_data == NULL)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Cannot read %s: %s", path, SDL_GetError());
			continue;
		}
		pixels = stbi_load_from_memory(file_data, (int)file_size, width_out, height_out, &channels_in_file, wanted_channels);
		SDL_free(file_data);
		if (pixels != NULL)
		{
			SDL_Log("Read %s (%dx%d)", path, *width_out, *height_out);
		}
		if (pixels == NULL)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Cannot decode %s: %s", path, stbi_failure_reason());
			continue;
		}
		return pixels;
	}
	return NULL;
}

// Loads one texture variant into an RGBA layer (height in alpha). Returns
// false if the color image is missing.
// Its normal map, textures/<name>_normal.png, if there is one: into
// normal_out (RGBA), its green turned to the arrays' way (down the picture,
// along +v: as build_normal_layer makes them); false if none
static bool load_normal_layer(const char *name, int size, bool green_down, uint8_t *normal_out)
{
	int width;
	int height;
	char normal_name[96];
	uint8_t *pixels;

	SDL_snprintf(normal_name, sizeof(normal_name), "%s_normal", name);
	pixels = load_image_any_format(normal_name, 3, &width, &height);
	if (pixels == NULL)
	{
		return false;
	}
	resample_wrapping(pixels, width, height, 3, normal_out, size, 4, 0);
	stbi_image_free(pixels);
	for (int i = 0; i < size * size; i++)
	{
		uint8_t *pixel = &normal_out[i * 4];

		if (!green_down)
		{
			pixel[1] = (uint8_t)(255 - pixel[1]);
		}
		pixel[3] = 255;
	}
	SDL_Log("Texture %s: normal map %s loaded%s", name, normal_name, green_down ? " (green down)" : "");
	return true;
}

// A relief: its normal map (green up, OpenGL's) and its height in alpha
// (NAME_height.png; without one, flat: 128)
static bool load_relief_layer(const char *name, int size, uint8_t *layer_out)
{
	int width;
	int height;
	char height_name[96];
	uint8_t *pixels;

	if (!load_normal_layer(name, size, false, layer_out))
	{
		return false;
	}
	SDL_snprintf(height_name, sizeof(height_name), "%s_height", name);
	pixels = load_image_any_format(height_name, 1, &width, &height);
	if (pixels != NULL)
	{
		resample_wrapping(pixels, width, height, 1, layer_out, size, 4, 3);
		stbi_image_free(pixels);
	}
	else
	{
		for (int i = 0; i < size * size; i++)
		{
			layer_out[i * 4 + 3] = 128;
		}
	}
	return true;
}

// A layer's mask into the normals' alpha: NAME_mask.png (gray), else the
// color layer's alpha (its height, before the roughness goes there)
static void load_mask_layer(const char *name, int size, const uint8_t *color_layer, uint8_t *normal_layer)
{
	int width;
	int height;
	char mask_name[96];
	uint8_t *pixels = NULL;

	if (name[0] != '\0')
	{
		SDL_snprintf(mask_name, sizeof(mask_name), "%s_mask", name);
		pixels = load_image_any_format(mask_name, 1, &width, &height);
	}
	if (pixels != NULL)
	{
		resample_wrapping(pixels, width, height, 1, normal_layer, size, 4, 3);
		stbi_image_free(pixels);
		return;
	}
	for (int i = 0; i < size * size; i++)
	{
		normal_layer[i * 4 + 3] = color_layer[i * 4 + 3];
	}
}

// A layer's roughness into its alpha: NAME_roughness.png (gray, or an ARM
// picture's green: NAME_arm.png), else the material's own all over
static void load_roughness_layer(const char *name, float roughness, int size, uint8_t *layer_out)
{
	int width;
	int height;
	char file_name[96];
	uint8_t *pixels = NULL;
	uint8_t level = (uint8_t)SDL_clamp((int)(roughness * 255.0f + 0.5f), 0, 255);

	if (name[0] != '\0')
	{
		SDL_snprintf(file_name, sizeof(file_name), "%s_roughness", name);
		pixels = load_image_any_format(file_name, 1, &width, &height);
		if (pixels != NULL)
		{
			resample_wrapping(pixels, width, height, 1, layer_out, size, 4, 3);
			stbi_image_free(pixels);
			SDL_Log("Texture %s: roughness %s loaded", name, file_name);
			return;
		}
		SDL_snprintf(file_name, sizeof(file_name), "%s_arm", name);
		pixels = load_image_any_format(file_name, 3, &width, &height);
		if (pixels != NULL)
		{
			// (its green into a gray picture first)
			for (int i = 0; i < width * height; i++)
			{
				pixels[i] = pixels[i * 3 + 1];
			}
			resample_wrapping(pixels, width, height, 1, layer_out, size, 4, 3);
			stbi_image_free(pixels);
			SDL_Log("Texture %s: roughness from %s loaded", name, file_name);
			return;
		}
	}
	for (int i = 0; i < size * size; i++)
	{
		layer_out[i * 4 + 3] = level;
	}
}

// want_height: its height into alpha (for normals from it); false where
// it has a normal map of its own (the alpha becomes the roughness anyway)
static bool load_texture_layer(const char *name, int size, uint8_t *layer_out, bool want_height)
{
	int width;
	int height;
	uint8_t *color = load_image_any_format(name, 3, &width, &height);
	char height_name[96];
	uint8_t *height_pixels;

	if (color == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Texture %s: no readable textures/%s.png, .jpg or .bmp in the data directory", name, name);
		return false;
	}
	resample_wrapping(color, width, height, 3, layer_out, size, 4, 0);
	stbi_image_free(color);
	if (!want_height)
	{
		SDL_Log("Loaded texture %s", name);
		return true;
	}

	SDL_snprintf(height_name, sizeof(height_name), "%s_height", name);
	height_pixels = load_image_any_format(height_name, 1, &width, &height);
	if (height_pixels != NULL)
	{
		resample_wrapping(height_pixels, width, height, 1, layer_out, size, 4, 3);
		stbi_image_free(height_pixels);
		SDL_Log("Texture %s: height map %s loaded", name, height_name);
	}
	else
	{
		// (its normals from it only where it has no _normal file)
		SDL_Log("Texture %s: no height map (%s), one from brightness", name, height_name);
		// No height map: use brightness, stretched to the full range, as a
		// stand-in (bright = high works surprisingly often for stones)
		int minimum = 255;
		int maximum = 0;

		for (int i = 0; i < size * size; i++)
		{
			uint8_t *pixel = &layer_out[i * 4];
			int luminance = (pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8;

			pixel[3] = (uint8_t)luminance;
			minimum = SDL_min(minimum, luminance);
			maximum = SDL_max(maximum, luminance);
		}
		if (maximum > minimum)
		{
			for (int i = 0; i < size * size; i++)
			{
				uint8_t *pixel = &layer_out[i * 4];

				pixel[3] = (uint8_t)((pixel[3] - minimum) * 255 / (maximum - minimum));
			}
		}
	}
	SDL_Log("Loaded texture %s", name);
	return true;
}

// Procedural stand-in texture, tileable. Different materials get a slightly
// different recipe so they read as what they are even without real art.
static void generate_fallback_layer(material_type_t material, const material_description_t *description, int size, uint8_t *layer_out)
{
	noise_context_t noise;
	int period = SDL_max(1, SDL_min(description->fallback_period, 64));

	noise_initialize(&noise, 1000 + (uint64_t)material);

#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float u = (float)x / (float)size;
			float v = (float)y / (float)size;
			float value = noise_periodic_fbm(&noise, u * (float)period, v * (float)period, period, 5, 0.55f) * 0.5f + 0.5f;
			float speckle = noise_periodic(&noise, u * 64.0f + 17.0f, v * 64.0f + 5.0f, 64) * 0.5f + 0.5f;
			uint8_t *pixel = &layer_out[(y * size + x) * 4];
			float height;
			float shade;

			switch (material)
			{
				case MATERIAL_ROCK:
					// Ridged look: sharp creases between smooth lumps
					value = 1.0f - SDL_fabsf(value * 2.0f - 1.0f);
					height = value;
					shade = value * 0.8f + speckle * 0.2f;
					break;
				case MATERIAL_SAND:
				{
					// Wind ripples: a whole number of waves across the tile,
					// bent by the noise, keeps it seamless
					float ripple = 0.5f + 0.5f * SDL_sinf(2.0f * MATH3D_PI * (v * 24.0f + value * 1.5f));

					height = ripple * 0.6f + value * 0.4f;
					shade = height * 0.6f + speckle * 0.4f;
					break;
				}
				case MATERIAL_GRASS:
					height = value * 0.5f + speckle * 0.5f;
					shade = value * 0.5f + speckle * 0.5f;
					break;
				default:
					height = value;
					shade = value * 0.75f + speckle * 0.25f;
					break;
			}

			shade = clamp_float(shade, 0.0f, 1.0f);
			for (int channel = 0; channel < 3; channel++)
			{
				float color = lerp_float(description->fallback_dark[channel], description->fallback_light[channel], shade);

				pixel[channel] = (uint8_t)clamp_float(color * 255.0f + 0.5f, 0.0f, 255.0f);
			}
			pixel[3] = (uint8_t)clamp_float(height * 255.0f + 0.5f, 0.0f, 255.0f);
		}
	}
}

// Tangent space normal map from the height in the alpha channel. Wraps at
// the borders so tileable textures stay seamless.
static void build_normal_layer(const uint8_t *color_height, int size, float strength, uint8_t *normal_out)
{
	// Scale so that the relief looks about the same at any texture size
	float scale = strength * (float)size / (128.0f * 255.0f);

#pragma omp parallel for schedule(static)
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			int left = (x + size - 1) % size;
			int right = (x + 1) % size;
			int up = (y + size - 1) % size;
			int down = (y + 1) % size;
			float slope_x = (float)color_height[(y * size + right) * 4 + 3] - (float)color_height[(y * size + left) * 4 + 3];
			float slope_y = (float)color_height[(down * size + x) * 4 + 3] - (float)color_height[(up * size + x) * 4 + 3];
			vec3_t normal = vec3_normalize(vec3_make(-slope_x * scale, -slope_y * scale, 1.0f));
			uint8_t *pixel = &normal_out[(y * size + x) * 4];

			pixel[0] = (uint8_t)clamp_float((normal.x * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f);
			pixel[1] = (uint8_t)clamp_float((normal.y * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f);
			pixel[2] = (uint8_t)clamp_float((normal.z * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f);
			pixel[3] = 255;
		}
	}
}

#define VARIATION_TEXTURE_SIZE 512

// Builds the variation texture: a tileable, cloudy fBm. Several octaves of
// periodic gradient noise give soft blobs at several scales, which is what
// both the ground variation and the clouds want.
static SDL_GPUTexture *create_variation_texture(uint8_t **pixels_out)
{
	int size = VARIATION_TEXTURE_SIZE;
	uint8_t *pixels = SDL_malloc((size_t)size * (size_t)size);
	int width;
	int height;
	uint8_t *loaded = load_image_any_format("variation", 1, &width, &height);
	SDL_GPUTexture *texture;

	if (pixels == NULL)
	{
		SDL_free(loaded);
		return 0;
	}
	if (loaded != NULL)
	{
		resample_wrapping(loaded, width, height, 1, pixels, size, 1, 0);
		stbi_image_free(loaded);
		SDL_Log("Variation texture: loaded textures/variation");
	}
	else
	{
		noise_context_t noise;
		float minimum = 1e30f;
		float maximum = -1e30f;
		float *values = SDL_malloc(sizeof(float) * (size_t)size * (size_t)size);

		if (values == NULL)
		{
			SDL_free(pixels);
			return 0;
		}
		noise_initialize(&noise, 4242);
#pragma omp parallel for schedule(static)
		for (int y = 0; y < size; y++)
		{
			for (int x = 0; x < size; x++)
			{
				float u = (float)x / (float)size * 4.0f;
				float v = (float)y / (float)size * 4.0f;

				values[y * size + x] = noise_periodic_fbm(&noise, u, v, 4, 6, 0.55f);
			}
		}
		for (int i = 0; i < size * size; i++)
		{
			minimum = SDL_min(minimum, values[i]);
			maximum = SDL_max(maximum, values[i]);
		}
		// Stretch to the full 0..255 range so coverage thresholds behave
		for (int i = 0; i < size * size; i++)
		{
			float normalized = maximum > minimum ? (values[i] - minimum) / (maximum - minimum) : 0.5f;

			pixels[i] = (uint8_t)clamp_float(normalized * 255.0f + 0.5f, 0.0f, 255.0f);
		}
		SDL_free(values);
		SDL_Log("Variation texture: no textures/variation.png, generated one");
	}

	// Red: the cloudy noise; green: the value noise's lattice; blue: the
	// same moved by a slice (the 3D noise's next slice up, noise_lattice.h)
	{
		const uint8_t *points = noise_lattice();
		uint8_t *texels = SDL_malloc((size_t)size * (size_t)size * 4);

		if (texels == NULL || points == NULL || size != NOISE_LATTICE_SIZE)
		{
			SDL_free(texels);
			SDL_free(pixels);
			return NULL;
		}
		for (int y = 0; y < size; y++)
		{
			for (int x = 0; x < size; x++)
			{
				uint8_t *texel = &texels[((size_t)y * (size_t)size + (size_t)x) * 4];

				texel[0] = pixels[y * size + x];
				texel[1] = points[y * size + x];
				texel[2] = points[((y + NOISE_LATTICE_SLICE_Y) & (size - 1)) * size + ((x + NOISE_LATTICE_SLICE_X) & (size - 1))];
				texel[3] = 255;
			}
		}
		texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (uint32_t)size, (uint32_t)size, 1, 0,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		gpu_upload_texture(texture, 0, 0, 0, 0, (uint32_t)size, (uint32_t)size, texels, (size_t)size * (size_t)size * 4);
		SDL_free(texels);
	}
	gpu_generate_mipmaps(texture);
	*pixels_out = pixels;
	return texture;
}

// Where one of a layer's files is: its name with each of the extensions
// load_image_any_format tries, the first that exists (empty: none)
static void find_image(const char *base_name, char *path_out, size_t path_out_size)
{
	static const char *extensions[] = { ".png", ".jpg", ".bmp" };

	path_out[0] = '\0';
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
	{
		char path[256];

		SDL_snprintf(path, sizeof(path), "textures/%s%s", base_name, extensions[i]);
		if (file_io_data_file_exists(path))
		{
			SDL_strlcpy(path_out, path, path_out_size);
			return;
		}
	}
}

// The files a named layer is made from, as the loading goes about it (a
// height only where it has no normal map, or a relief): appended to paths
static void layer_source_files(const char *name, bool relief, char (*paths)[256], int *count, int capacity)
{
	static const char *suffixes[] = { "", "_normal", "_height", "_roughness", "_arm", "_mask" };
	char normal_path[256];
	char base[96];

	SDL_snprintf(base, sizeof(base), "%s_normal", name);
	find_image(base, normal_path, sizeof(normal_path));
	for (size_t k = 0; k < sizeof(suffixes) / sizeof(suffixes[0]) && *count < capacity; k++)
	{
		if (k == 2 && !relief && normal_path[0] != '\0')
		{
			continue;
		}
		SDL_snprintf(base, sizeof(base), "%s%s", name, suffixes[k]);
		// (a missing one counts too: one appearing makes it again)
		find_image(base, paths[*count], sizeof(paths[*count]));
		if (paths[*count][0] == '\0')
		{
			SDL_snprintf(paths[*count], sizeof(paths[*count]), "(none) %s", base);
		}
		(*count)++;
	}
}

// What the terrain's textures are made from: the version of the making,
// the settings that change them, materials.ini and every source file's
// bytes (read on every core)
static uint32_t textures_key(const material_description_t descriptions[MATERIAL_COUNT], int size, float normal_strength)
{
	enum { MOST_FILES = MATERIAL_COUNT * (MATERIAL_MAX_VARIANTS + 3) * 6 };
	char (*paths)[256] = SDL_malloc(sizeof(*paths) * MOST_FILES);
	uint32_t file_keys[MOST_FILES];
	int count = 0;
	uint32_t key = 0;

	if (paths == NULL)
	{
		return 0;
	}
	key = texture_cache_key_int(key, (int32_t)TEXTURE_CACHE_VERSION);
	key = texture_cache_key_int(key, size);
	key = texture_cache_key_float(key, normal_strength);
	key = texture_cache_key_data_file(key, "materials.ini");
	// (the normals' alpha holds the masks)
	key = texture_cache_key_string(key, "masks 1");
	for (int material = 0; material < MATERIAL_COUNT; material++)
	{
		const material_description_t *description = &descriptions[material];

		for (int i = 0; i < description->texture_name_count; i++)
		{
			layer_source_files(description->texture_names[i], false, paths, &count, MOST_FILES);
		}
		if (description->wet_texture[0] != '\0')
		{
			layer_source_files(description->wet_texture, false, paths, &count, MOST_FILES);
		}
		if (description->macro_texture[0] != '\0')
		{
			layer_source_files(description->macro_texture, false, paths, &count, MOST_FILES);
		}
		if (description->relief[0] != '\0')
		{
			layer_source_files(description->relief, true, paths, &count, MOST_FILES);
		}
	}
#pragma omp parallel for schedule(dynamic, 1)
	for (int i = 0; i < count; i++)
	{
		file_keys[i] = SDL_strncmp(paths[i], "(none)", 6) == 0 ? texture_cache_key_string(0, paths[i])
			: texture_cache_key_data_file(0, paths[i]);
	}
	for (int i = 0; i < count; i++)
	{
		key = texture_cache_key_int(key, (int32_t)file_keys[i]);
	}
	SDL_free(paths);
	return key;
}

// Which layer is what, kept beside the cached layers
typedef struct
{
	char magic[4];                 // "PTXM"
	uint32_t version;
	uint32_t key;
	int32_t layer_count;
	int32_t first_layer[MATERIAL_COUNT];
	int32_t variant_count[MATERIAL_COUNT];
	int32_t macro_layer[MATERIAL_COUNT];
	int32_t relief_layer[MATERIAL_COUNT];
	int32_t wet_layer[MATERIAL_COUNT];
} layers_record_t;

static void layers_record_path(char *path_out, size_t path_out_size)
{
	file_io_data_path("cache/textures/terrain_layers.ptm", path_out, path_out_size);
}

static bool load_layers_record(material_set_t *materials, uint32_t key)
{
	char path[1024];
	size_t size = 0;
	layers_record_t *record;
	bool ok;

	layers_record_path(path, sizeof(path));
	record = file_io_load_file(path, &size);
	if (record == NULL)
	{
		return false;
	}
	ok = size == sizeof(*record) && SDL_memcmp(record->magic, "PTXM", 4) == 0 && record->version == TEXTURE_CACHE_VERSION
		&& record->key == key && record->layer_count > 0 && record->layer_count <= MATERIAL_MAX_LAYERS;
	if (ok)
	{
		materials->layer_count = record->layer_count;
		for (int material = 0; material < MATERIAL_COUNT; material++)
		{
			materials->first_layer[material] = record->first_layer[material];
			materials->variant_count[material] = record->variant_count[material];
			materials->macro_layer[material] = record->macro_layer[material];
			materials->relief_layer[material] = record->relief_layer[material];
			materials->wet_layer[material] = record->wet_layer[material];
		}
	}
	SDL_free(record);
	return ok;
}

static void save_layers_record(const material_set_t *materials, uint32_t key)
{
	char path[1024];
	char directory[1024];
	layers_record_t record;

	SDL_zero(record);
	SDL_memcpy(record.magic, "PTXM", 4);
	record.version = TEXTURE_CACHE_VERSION;
	record.key = key;
	record.layer_count = materials->layer_count;
	for (int material = 0; material < MATERIAL_COUNT; material++)
	{
		record.first_layer[material] = materials->first_layer[material];
		record.variant_count[material] = materials->variant_count[material];
		record.macro_layer[material] = materials->macro_layer[material];
		record.relief_layer[material] = materials->relief_layer[material];
		record.wet_layer[material] = materials->wet_layer[material];
	}
	file_io_data_path("cache/textures", directory, sizeof(directory));
	SDL_CreateDirectory(directory);
	layers_record_path(path, sizeof(path));
	file_io_save_file(path, &record, sizeof(record));
}

// The layers made from their files (the slow way: decoding, normals,
// roughness) and compressed: color_blocks and normal_blocks, every layer
// one after the other, each with its levels (SDL_free them). False if
// there was not the memory
static bool make_layers(material_set_t *materials, material_description_t descriptions[MATERIAL_COUNT], const render_settings_t *settings,
	int size, uint32_t level_count, uint8_t **color_blocks_out, uint8_t **normal_blocks_out)
{
	size_t layer_bytes = (size_t)size * (size_t)size * 4;
	size_t layer_block_bytes = texture_cache_layer_bytes((uint32_t)size, (uint32_t)size, level_count);
	uint8_t *color_layers;
	uint8_t *normal_layer;
	// The layers' own normal maps (_normal files), where they have them
	uint8_t *normal_layers;
	bool has_normal[MATERIAL_MAX_LAYERS];
	// Each layer's texture name (its _roughness file; empty: none) and its
	// material's roughness without one
	char layer_names[MATERIAL_MAX_LAYERS][64];
	float layer_roughness[MATERIAL_MAX_LAYERS];
	int layer = 0;
	uint8_t *color_blocks;
	uint8_t *normal_blocks;

	color_layers = SDL_malloc(layer_bytes * MATERIAL_MAX_LAYERS);
	normal_layer = SDL_malloc(layer_bytes);
	// (as many as there can be layers: their textures, or one stand in each)
	{
		int most_layers = 0;

		for (int material = 0; material < MATERIAL_COUNT; material++)
		{
			most_layers += SDL_max(descriptions[material].texture_name_count, 1) + 3;
		}
		normal_layers = SDL_malloc(layer_bytes * (size_t)SDL_min(most_layers, MATERIAL_MAX_LAYERS));
	}
	SDL_zeroa(has_normal);
	SDL_memset(layer_names, 0, sizeof(layer_names));
	for (int i = 0; i < MATERIAL_MAX_LAYERS; i++)
	{
		layer_roughness[i] = 0.9f;
	}
	if (color_layers == NULL || normal_layer == NULL || normal_layers == NULL)
	{
		SDL_free(color_layers);
		SDL_free(normal_layer);
		SDL_free(normal_layers);
		return false;
	}

	// Fill the CPU side layers: loaded textures where possible, otherwise
	// one procedural layer per material. The normal map first: with one,
	// the height is not wanted
	for (int material = 0; material < MATERIAL_COUNT; material++)
	{
		material_description_t *description = &descriptions[material];

		materials->first_layer[material] = layer;
		for (int i = 0; i < description->texture_name_count; i++)
		{
			bool normal = load_normal_layer(description->texture_names[i], size, description->normal_green_down,
				normal_layers + layer_bytes * (size_t)layer);

			if (load_texture_layer(description->texture_names[i], size, color_layers + layer_bytes * (size_t)layer, !normal))
			{
				has_normal[layer] = normal;
				SDL_strlcpy(layer_names[layer], description->texture_names[i], sizeof(layer_names[layer]));
				layer_roughness[layer] = description->roughness;
				layer++;
			}
		}
		if (layer == materials->first_layer[material])
		{
			if (description->texture_name_count == 0)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Material %s: no textures listed in materials.ini, using the PROCEDURAL FALLBACK",
					material_names[material]);
			}
			else
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Material %s: none of its %d textures could be loaded, using the PROCEDURAL FALLBACK",
					material_names[material], description->texture_name_count);
			}
			generate_fallback_layer((material_type_t)material, description, size, color_layers + layer_bytes * (size_t)layer);
			layer_roughness[layer] = description->roughness;
			layer++;
		}
		else
		{
			SDL_Log("Material %s: %d texture variant(s), tiling %.1f m, macro tiling %.1f m", material_names[material],
				layer - materials->first_layer[material], description->tiling_meters, description->macro_tiling_meters);
		}
		materials->variant_count[material] = layer - materials->first_layer[material];
		// Its macro texture and its relief, layers of their own after its
		// variants
		materials->macro_layer[material] = -1;
		materials->relief_layer[material] = -1;
		materials->wet_layer[material] = -1;
		if (description->wet_texture[0] != '\0')
		{
			bool normal = load_normal_layer(description->wet_texture, size, description->normal_green_down,
				normal_layers + layer_bytes * (size_t)layer);

			if (load_texture_layer(description->wet_texture, size, color_layers + layer_bytes * (size_t)layer, !normal))
			{
				has_normal[layer] = normal;
				SDL_strlcpy(layer_names[layer], description->wet_texture, sizeof(layer_names[layer]));
				layer_roughness[layer] = description->wet_roughness;
				materials->wet_layer[material] = layer++;
				SDL_Log("Material %s: wet texture %s", material_names[material], description->wet_texture);
			}
		}
		if (description->macro_texture[0] != '\0')
		{
			bool normal = load_normal_layer(description->macro_texture, size, description->normal_green_down,
				normal_layers + layer_bytes * (size_t)layer);

			if (load_texture_layer(description->macro_texture, size, color_layers + layer_bytes * (size_t)layer, !normal))
			{
				has_normal[layer] = normal;
				SDL_strlcpy(layer_names[layer], description->macro_texture, sizeof(layer_names[layer]));
				layer_roughness[layer] = description->roughness;
				materials->macro_layer[material] = layer++;
				SDL_Log("Material %s: macro texture %s", material_names[material], description->macro_texture);
			}
		}
		if (description->relief[0] != '\0')
		{
			if (load_relief_layer(description->relief, size, normal_layers + layer_bytes * (size_t)layer))
			{
				// (nothing to see in the color array: gray)
				SDL_memset(color_layers + layer_bytes * (size_t)layer, 128, layer_bytes);
				has_normal[layer] = true;
				materials->relief_layer[material] = layer++;
				SDL_Log("Material %s: relief %s, %.0f m across", material_names[material], description->relief, description->relief_meters);
			}
			else
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Material %s: no relief %s_normal.png", material_names[material], description->relief);
			}
		}
	}
	materials->layer_count = layer;

	// The normals first (derived ones from the height in the color layers'
	// alpha), then the alpha becomes the roughness (NAME_roughness.png, else
	// the material's roughness); each layer compressed with its levels
	color_blocks = SDL_malloc(layer_block_bytes * (size_t)layer);
	normal_blocks = SDL_malloc(layer_block_bytes * (size_t)layer);
	if (color_blocks == NULL || normal_blocks == NULL)
	{
		SDL_free(color_blocks);
		SDL_free(normal_blocks);
		SDL_free(color_layers);
		SDL_free(normal_layer);
		SDL_free(normal_layers);
		return false;
	}
	for (int i = 0; i < layer; i++)
	{
		uint8_t *normals = normal_layer;
		bool relief = false;

		for (int material = 0; material < MATERIAL_COUNT; material++)
		{
			relief |= materials->relief_layer[material] == i;
		}
		if (has_normal[i])
		{
			normals = normal_layers + layer_bytes * (size_t)i;
		}
		else
		{
			build_normal_layer(color_layers + layer_bytes * (size_t)i, size, settings->normal_strength, normal_layer);
		}
		// The mask into the normals' alpha (a relief keeps its height
		// there): how an overlay (snow, petals) covers the ground as it
		// grows: where its mask is below its amount. NAME_mask.png, else
		// the height (low first: it settles in the hollows)
		if (!relief)
		{
			load_mask_layer(layer_names[i], size, color_layers + layer_bytes * (size_t)i, normals);
		}
		texture_cache_encode_layer(normals, (uint32_t)size, (uint32_t)size, level_count, TEXTURE_CACHE_NORMAL, normal_blocks + layer_block_bytes * (size_t)i);
		load_roughness_layer(layer_names[i], layer_roughness[i], size, color_layers + layer_bytes * (size_t)i);
		texture_cache_encode_layer(color_layers + layer_bytes * (size_t)i, (uint32_t)size, (uint32_t)size, level_count, TEXTURE_CACHE_COLOR,
			color_blocks + layer_block_bytes * (size_t)i);
	}
	SDL_free(color_layers);
	SDL_free(normal_layer);
	SDL_free(normal_layers);
	*color_blocks_out = color_blocks;
	*normal_blocks_out = normal_blocks;
	return true;
}

bool materials_load(material_set_t *materials, const render_settings_t *settings)
{
	material_description_t descriptions[MATERIAL_COUNT];
	int size = clamp_int(settings->texture_size, 32, 4096);
	uint32_t level_count = texture_cache_level_count((uint32_t)size, (uint32_t)size);
	size_t layer_block_bytes = texture_cache_layer_bytes((uint32_t)size, (uint32_t)size, level_count);
	uint8_t *color_blocks = NULL;
	uint8_t *normal_blocks = NULL;
	uint32_t key;
	uint64_t start = SDL_GetPerformanceCounter();
	bool cached = false;

	SDL_zerop(materials);
	set_default_descriptions(descriptions);
	ini_parse_data_file("materials.ini", material_key_callback, descriptions);
	for (int material = 0; material < MATERIAL_COUNT; material++)
	{
		material_description_t *description = &descriptions[material];

		// The macro scale is meant for distance, so it must not be finer
		// than the detail scale; without an explicit value it is ten times
		// the detail tiling
		if (!description->macro_tiling_given)
		{
			description->macro_tiling_meters = description->tiling_meters * 10.0f;
		}
		else if (description->macro_tiling_meters < description->tiling_meters)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Material %s: macro_tiling_meters (%.1f) is smaller than tiling_meters (%.1f); "
				"from afar the texture will repeat more often than up close", material_names[material],
				description->macro_tiling_meters, description->tiling_meters);
		}
		materials->relief_meters[material] = description->relief_meters;
		materials->repeats_per_meter[material] = 1.0f / description->tiling_meters;
		materials->macro_repeats_per_meter[material] = 1.0f / description->macro_tiling_meters;
		materials->patch_repeats_per_meter[material] = 1.0f / description->patch_meters;
		materials->patch_edge_meters[material] = description->patch_edge_meters;
		materials->patch_strength[material] = description->patch_strength;
	}
	materials->texture_size = size;

	// Made before and unchanged since: from the cache, as they are
	key = textures_key(descriptions, size, settings->normal_strength);
	if (load_layers_record(materials, key))
	{
		color_blocks = texture_cache_load("terrain_color", key, (uint32_t)size, (uint32_t)size, (uint32_t)materials->layer_count, level_count);
		normal_blocks = texture_cache_load("terrain_normal", key, (uint32_t)size, (uint32_t)size, (uint32_t)materials->layer_count,
			level_count);
		cached = color_blocks != NULL && normal_blocks != NULL;
	}
	if (!cached)
	{
		SDL_free(color_blocks);
		SDL_free(normal_blocks);
		color_blocks = NULL;
		normal_blocks = NULL;
		SDL_Log("Terrain textures: made anew and compressed (they go to the cache; a few seconds, this time only)");
		if (!make_layers(materials, descriptions, settings, size, level_count, &color_blocks, &normal_blocks))
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Not enough memory for the terrain's textures");
			return false;
		}
		texture_cache_save("terrain_color", key, (uint32_t)size, (uint32_t)size, (uint32_t)materials->layer_count, level_count, color_blocks);
		texture_cache_save("terrain_normal", key, (uint32_t)size, (uint32_t)size, (uint32_t)materials->layer_count, level_count,
			normal_blocks);
		save_layers_record(materials, key);
	}

	// Up as they are (BC7; decoded where the GPU has none)
	materials->color_height_array = texture_cache_create((uint32_t)size, (uint32_t)size, (uint32_t)materials->layer_count, level_count);
	materials->normal_array = texture_cache_create((uint32_t)size, (uint32_t)size, (uint32_t)materials->layer_count, level_count);
	for (int i = 0; i < materials->layer_count; i++)
	{
		texture_cache_upload_layer(materials->color_height_array, (uint32_t)i, (uint32_t)size, (uint32_t)size, level_count,
			color_blocks + layer_block_bytes * (size_t)i);
		texture_cache_upload_layer(materials->normal_array, (uint32_t)i, (uint32_t)size, (uint32_t)size, level_count,
			normal_blocks + layer_block_bytes * (size_t)i);
	}
	// The big layers wait in the staging area: off to the GPU now
	gpu_flush_uploads();
	SDL_free(color_blocks);
	SDL_free(normal_blocks);
	SDL_Log("Terrain textures: %d layers of %d x %d, %s in %.2f s", materials->layer_count, size, size,
		cached ? "from the cache" : "made", (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency());

	materials->variation_texture = create_variation_texture(&materials->variation_pixels);
	materials->variation_size = VARIATION_TEXTURE_SIZE;
	if (materials->variation_texture == NULL || materials->color_height_array == NULL || materials->normal_array == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "The terrain's textures could not be made");
		return false;
	}
	return true;
}

void materials_destroy(material_set_t *materials)
{
	if (materials->color_height_array != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, materials->color_height_array);
	}
	if (materials->normal_array != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, materials->normal_array);
	}
	if (materials->variation_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, materials->variation_texture);
	}
	SDL_free(materials->variation_pixels);
	SDL_zerop(materials);
}
