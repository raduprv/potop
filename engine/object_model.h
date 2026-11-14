// 3D object models: the game's own .pobj files (see object_format.h, made
// by tools/potop_convert from glTF) and Eternal Lands' .e3d. An object
// named "objects/tree.e3d" is loaded from objects/tree.pobj when there is
// one.
//
// An .e3d file holds one vertex buffer, one index buffer and a list of
// materials, each a texture plus a range of the indices. Textures sit next
// to the model and are named in the file (usually DDS, see dds.h).
//
// Eternal Lands has Z up; the game has Y up with the map's rows along +Z,
// so a model point (x, y, z) becomes (x, z, -y): a rotation, not a mirror,
// so triangle winding and lighting stay correct. Units are meters in both.

#ifndef OBJECT_MODEL_H
#define OBJECT_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"
#include "math3d.h"
#include "object_format.h"

#define OBJECT_MODEL_MAX_MATERIALS 16
// object.frag's samplers (objects and actors): the shadow maps (0, 1), the
// variation (2), the base color (3), the clouds' shadows (4), the normal
// map (5), occlusion-roughness-metal (6), emissive (7)
#define OBJECT_FRAGMENT_SAMPLERS 14
#define OBJECT_MODEL_NAME_LENGTH 128
#define OBJECT_MODEL_MAX_MONTH_CELLS 32
// A vertex in its vertex buffer: position, normal, uv
#define OBJECT_MODEL_VERTEX_BYTES (8 * (int)sizeof(float))

typedef struct
{
	SDL_GPUTexture *texture;       // RGBA8, mipmapped, owned by the texture cache: the base color (sRGB)
	// What the physically based shading takes (see object_format.h); a
	// shared neutral one (object_default_texture) where the model has none
	SDL_GPUTexture *normal_texture;
	SDL_GPUTexture *orm_texture;   // occlusion, roughness, metal
	SDL_GPUTexture *emissive_texture;
	// It has maps of its own (.pobj): lit physically when render.pbr_shading
	// is on. Without (.e3d), the plain diffuse light, which costs less and
	// looks the same with the neutral maps
	bool shaded;
	bool alpha_tested;             // leaves and such: cut out by alpha, drawn two sided
	bool double_sided;             // OBJECT_MATERIAL_DOUBLE_SIDED (a building's cut out details without it: one sided)
	bool deciduous;                // OBJECT_MATERIAL_DECIDUOUS: its green reddens in the autumn, gone in the winter
	bool solid;                    // in the collision shape (not alpha tested nor OBJECT_MATERIAL_PASSABLE)
	bool lighting_normals;         // OBJECT_MATERIAL_LIGHTING_NORMALS: not turned round on a face's back
	bool triplanar;                // OBJECT_MATERIAL_TRIPLANAR: its textures from the sides, triplanar_meters a repeat
	bool weathered;                // OBJECT_MATERIAL_WEATHERED: moss, lichen, snow on it (prop.frag)
	bool collision_only;           // OBJECT_MATERIAL_COLLISION_ONLY: in the collision shape, not drawn (dropped once that is made)
	bool aged;                     // OBJECT_MATERIAL_AGED: weathered as a building (building.frag)
	bool thatch;                   // OBJECT_MATERIAL_THATCH: its relief marched into (building.frag)
	float triplanar_meters;
	int index_offset;              // first index of the material
	int index_count;
	// Levels of detail (object_format.h, OBJECT_MATERIAL_LODS): the first
	// lod_index_count[k] indices draw it at level k + 1. lod_keep_level[k]:
	// the deepest branch level left at level k + 1 (a plant's bark: its
	// vertices carry their branch's level in the tangent's size), so that
	// what goes away there can fade out before (0: not known)
	bool lods;
	bool shuffled;                 // OBJECT_MATERIAL_SHUFFLED: any first part (whole cards) is the whole thinned out
	int lod_index_count[3];
	int lod_keep_level[3];
	// OBJECT_MATERIAL_LOD_MESHES: the levels are meshes of their own, here
	// (their first indices; else the material's own index_offset)
	bool lod_meshes;
	int lod_index_offset[3];
	// A plant's (object_format.h, OBJECT_FILE_PLANT): drawn with the plants'
	// shaders, through the seasons
	bool plant;
	float autumn_colors[2][3];
	float autumn;
	float falls;
	float moss;
	float tips;
	float sag;                     // how far its branches sag under the winter's snow (plant.glsl)
	bool snow_cover;               // a plant's snow baked in (OBJECT_FILE_SNOW_MATERIAL): the snow lying on its boughs
	float autumn_first_month;      // its autumn's months (1..12; object_format.h), and when its leaves come again
	float autumn_last_month;
	float leaves_month;
} object_material_t;

// A plant's vertex on the GPU: object_file_plant_vertex_t, then
// object_file_snow_vertex_t (plant.vert's plant_data, plant_snow)
static const int OBJECT_PLANT_VERTEX_FLOATS = 8;

typedef struct
{
	char name[OBJECT_MODEL_NAME_LENGTH]; // file path relative to data/
	SDL_GPUBuffer *vertex_buffer;  // object_vertex_t: position, normal, uv (8 floats)
	SDL_GPUBuffer *tangent_buffer; // float[4] per vertex: along the texture's U, w the bitangent's sign
	// A plant's: float[8] per vertex (OBJECT_PLANT_VERTEX_FLOATS): what it
	// is on the plant (object_file_plant_vertex_t), where its baked snow
	// takes it (object_file_snow_vertex_t; 0 without); NULL: not a plant
	SDL_GPUBuffer *plant_buffer;
	// A building's (any OBJECT_MATERIAL_AGED material): float[4] per vertex,
	// its weathering (object_file_weathering_vertex_t; open, no edge, no
	// shelter where the file has none); NULL: none aged
	SDL_GPUBuffer *weathering_buffer;
	// Its type's age (objects.ini, set by objects.c): how weathered its aged
	// materials look, how much that differs from one to the next, how high
	// the dirt splashes up its walls (meters)
	float age;
	float age_random;
	float dirt_meters;
	SDL_GPUBuffer *index_buffer;
	SDL_GPUIndexElementSize index_size; // 16 or 32 bit
	// How many vertices and indices its buffers hold (the objects' shared
	// buffers copy them, objects.c)
	uint32_t vertex_total;
	uint32_t index_total;
	int material_count;
	object_material_t materials[OBJECT_MODEL_MAX_MATERIALS];
	vec3_t bounds_minimum;         // in game axes (Y up), meters
	vec3_t bounds_maximum;
	float horizontal_radius;       // farthest point from the vertical axis
	// A plant's pictures that show others in some months (object_format.h)
	object_file_month_cell_t month_cells[OBJECT_MODEL_MAX_MONTH_CELLS];
	// Emissive groups (object_format.h, OBJECT_FILE_EMISSIVE_GROUPS): the
	// first is group 1 (the whole part of a vertex's U)
	int emissive_group_count;
	object_file_emissive_group_t emissive_groups[OBJECT_FILE_MAX_EMISSIVE_GROUPS];
	// Its particle effects (object_format.h, OBJECT_FILE_EFFECTS), played
	// where each of its objects stands (object_effects.h)
	int effect_count;
	object_file_effect_t effects[OBJECT_FILE_MAX_EFFECTS];
	// Where it covers the ground (object_format.h, OBJECT_FILE_FOOTPRINT):
	// worked into the material map where its objects stand (ground_footprints.h)
	int footprint_count;
	object_file_footprint_t footprints[OBJECT_FILE_MAX_FOOTPRINTS];
	// Its places for the creatures and people (object_format.h,
	// OBJECT_FILE_SPOTS)
	int spot_count;
	object_file_spot_t spots[OBJECT_FILE_MAX_SPOTS];
	int month_cell_count;
	bool lods;                     // any of its materials has levels of detail
	bool still;                    // OBJECT_FILE_STILL: the wind leaves it be
	// A plant's own wind (its first plant material's, object_format.h): its
	// type's in objects.ini win where they are given
	bool wind_given;
	float sway;
	float flutter;
	float stiffness;
	float moss;                    // its weathered materials': how much moss, lichen grow (0..1, object_format.h)
	float lichen;
	float lod_errors[3];           // its levels' own meshes: how far off each is (meters at size 1, object_format.h)
	// The shape on the CPU side, for collision shapes (game axes, meters):
	// x, y, z per vertex, three indices per triangle. The solid materials'
	// triangles first (collision_index_count of them: physics, navigation,
	// water), then the rest (leaves, branches) up to pick_index_count, which
	// the mouse picks too
	float *collision_positions;
	int collision_vertex_count;
	uint32_t *collision_indices;
	int collision_index_count;
	int pick_index_count;
	// The file it came from (relative to data/: the .pobj or the .e3d) and
	// when that was last written, for reloading it when it changes
	char file_path[OBJECT_MODEL_NAME_LENGTH + 8];
	SDL_Time file_time;
	bool own_textures;             // its textures are its own (a .pobj's): released with it on a reload
} object_model_t;

// Textures shared between models: many models use the same few texture
// atlases, which are loaded once and looked up by path here
typedef struct
{
	char path[OBJECT_MODEL_NAME_LENGTH + 128];
	SDL_GPUTexture *texture;
	SDL_Time file_time;            // when its file was written (0: not a file of its own, a .pobj's)
	bool data;                     // values, not colors (normals, orm): its alpha is data too (a height), never bled into
} object_texture_entry_t;

typedef struct
{
	object_texture_entry_t *entries;
	int count;
	int capacity;
	float anisotropy;              // applied to every texture loaded
} object_texture_cache_t;

void object_texture_cache_initialize(object_texture_cache_t *cache, float anisotropy);
void object_texture_cache_destroy(object_texture_cache_t *cache);

// Helpers for textures (also used by actor_model.c). Bleeding gives the
// transparent pixels of a cut out RGBA8 texture their opaque neighbors'
// color, so mipmaps do not grow dark fringes; the upload makes a mipmapped
// RGBA8 texture of the pixels (NULL if the GPU refuses)
void object_texture_bleed_transparent_colors(uint8_t *pixels, int width, int height);
SDL_GPUTexture *object_texture_upload(const uint8_t *pixels, int width, int height);

// Loads a model from the data directory; its textures come from the cache
// (loaded on first use). Returns false (after logging why) if anything is
// missing or malformed.
bool object_model_load(object_model_t *model, const char *relative_path, object_texture_cache_t *cache);
// Frees the model's buffers; the textures belong to the cache
void object_model_destroy(object_model_t *model);

// Reloading what changed on disk (objects_reload_changed): true if the
// model's file was written since it was loaded (and a moment ago, so it is
// not half written)
bool object_model_file_changed(const object_model_t *model);

// The cache's texture files written since they were loaded: each loaded
// anew in place, the old one handed out (old_out, new_out, up to max) for
// the models to be pointed at the new one, then released by the caller.
// Returns how many
int object_texture_cache_reload_changed(object_texture_cache_t *cache, SDL_GPUTexture **old_out, SDL_GPUTexture **new_out, int max);

// Takes a texture out of the cache and releases it (nothing if it is not
// in it)
void object_texture_cache_drop(object_texture_cache_t *cache, SDL_GPUTexture *texture);

// The neutral maps for what a model does not have: a flat normal, no
// occlusion, rough and not metal and reflecting nothing (its alpha 0: the
// diffuse light alone, as such models were made for), no light of its own
// (made on first use; released by object_default_textures_release at the
// end)
typedef enum
{
	OBJECT_DEFAULT_NORMAL = 0,
	OBJECT_DEFAULT_ORM,
	OBJECT_DEFAULT_EMISSIVE,
	OBJECT_DEFAULT_COUNT
} object_default_texture_t;

SDL_GPUTexture *object_default_texture(object_default_texture_t which);
void object_default_textures_release(void);

// A texture of a single color (RGBA8, 4x4), for a material's factor alone;
// NULL if the GPU refuses
SDL_GPUTexture *object_texture_solid(const uint8_t color[4]);

// Tangents from texture coordinates (summed over the triangles, made square
// to the normals; w the bitangent's sign), for vertices of any layout:
// stride bytes apart, the position, normal (float[3]) and uv (float[2]) at
// those offsets in each. tangents_out: float[4] per vertex
void object_compute_tangents(const void *vertices, size_t stride, size_t position_offset, size_t normal_offset, size_t uv_offset,
	uint32_t vertex_count, const uint32_t *indices, uint32_t index_count, float *tangents_out);

#endif
