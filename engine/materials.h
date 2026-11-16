// Terrain surface materials and their textures.
//
// A material is what you SEE on the ground (grass, bare dirt, rock, sand,
// clay, snow, silt). It is different from the soil type stored per cell, which is
// what the ground is MADE of and what the simulation cares about. The
// terrain shader decides which materials to show from the soil type, slope,
// sun exposure and height.
//
// Each material has one to MATERIAL_MAX_VARIANTS textures, listed in
// data/materials.ini. For a texture named "grass_01" the loader reads:
//   data/textures/grass_01.png         color (RGB)
//   data/textures/grass_01_normal.png  its normal map, optional (green up the
//                                      picture, OpenGL's; normal_green_down = 1: DirectX's)
//   data/textures/grass_01_height.png  height (grayscale), optional: only for
//                                      the normals where there is no _normal file
// (.jpg and .bmp are tried as well). Without either the normals come from the
// color's brightness. Materials without any loadable texture get a
// procedural fallback, so the game runs with an empty textures folder.
//
// All textures end up in two texture arrays: color + height (in alpha), and
// the normal maps (their own, or derived from the height). Where two
// materials meet, each breaks into patches of the other (patch_meters,
// patch_edge, patch_strength: terrain.frag), not a blend by height.
//
// Each material is drawn at two scales: tiling_meters for the detail seen up
// close, and macro_tiling_meters for the large scale variation seen from
// afar. The shader fades from one to the other with distance.

#ifndef MATERIALS_H
#define MATERIALS_H

#include <stdbool.h>

#include "gpu.h"
#include "settings.h"

// The order is shared with the terrain shader (see the MATERIAL_ defines it
// receives).
typedef enum
{
	MATERIAL_GRASS = 0,
	MATERIAL_DIRT,
	MATERIAL_ROCK,
	MATERIAL_SAND,
	MATERIAL_CLAY,
	MATERIAL_SNOW,
	MATERIAL_SILT,                 // mud the water left behind
	// Covers (the material map's: material_map.h): what fell or was worn
	// over the ground where something stands
	MATERIAL_NEEDLES,              // under conifers
	MATERIAL_LEAVES,               // under broadleaved trees
	MATERIAL_TRAMPLED,             // round houses, along paths
	// An overlay: in amounts, over whatever lies there, in its season
	MATERIAL_PETALS,               // under blooming trees
	// A cover a building lays on the ground under it (its footprint:
	// ground_footprints.h): a smithy's floor, beaten earth, ash and slag
	MATERIAL_TRAMPLED_DIRT,
	MATERIAL_COUNT
} material_type_t;

// No material (the material map's cover and overlay where there is none)
#define MATERIAL_NONE 255

#define MATERIAL_MAX_VARIANTS 3
// (each its variants, and a macro texture, a relief and a wet texture of its own)
#define MATERIAL_MAX_LAYERS (MATERIAL_COUNT * (MATERIAL_MAX_VARIANTS + 3))

typedef struct
{
	SDL_GPUTexture *color_height_array; // RGBA8: color in RGB, roughness in A
	SDL_GPUTexture *normal_array;  // RGBA8: tangent space normal in RGB
	// Tileable cloudy grayscale noise (red), used for the global variation
	// over all materials and for the moving cloud shadows. Loaded from
	// data/textures/variation.png if present, generated otherwise. Green
	// and blue: the value noise's lattice (noise_lattice.h; read at mip 0)
	SDL_GPUTexture *variation_texture;
	// Its pixels, kept for the material map's bake (material_map.c)
	uint8_t *variation_pixels;
	int variation_size;
	int texture_size;
	int layer_count;

	// Per material: where its variants start in the arrays, how many there
	// are, and how many texture repeats per meter
	int first_layer[MATERIAL_COUNT];
	int variant_count[MATERIAL_COUNT];
	float repeats_per_meter[MATERIAL_COUNT];
	// The same textures sampled at a much larger scale, used at a distance
	// where the fine detail would only turn into mush or shimmer
	float macro_repeats_per_meter[MATERIAL_COUNT];
	// Where it meets another (terrain.frag): its patches' size (repeats per
	// meter), their edges' softness (meters), how far they break the border
	float patch_repeats_per_meter[MATERIAL_COUNT];
	float patch_edge_meters[MATERIAL_COUNT];
	float patch_strength[MATERIAL_COUNT];
	// Its own texture for the macro scale (macro_texture: a rock's beds seen
	// from afar, over a massive stone up close; -1: its variants), and its
	// relief (relief: a big tileable normal map with its height in alpha,
	// gullies and buttresses on its steep faces, relief_meters across; -1:
	// none), layers in the arrays
	int macro_layer[MATERIAL_COUNT];
	int relief_layer[MATERIAL_COUNT];
	float relief_meters[MATERIAL_COUNT];
	// What it looks like soaked (wet_texture: clay's smooth mud, its cracks
	// closed; -1: itself, only darker), a layer in the arrays
	int wet_layer[MATERIAL_COUNT];
} material_set_t;

const char *materials_name(material_type_t material);

// Reads data/materials.ini, loads (or generates) every texture and uploads
// the texture arrays. Requires a current GL context.
bool materials_load(material_set_t *materials, const render_settings_t *settings);

void materials_destroy(material_set_t *materials);

#endif
