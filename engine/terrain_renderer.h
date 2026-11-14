// Terrain rendering.
//
// The mesh is one small static grid (TERRAIN_CHUNK_SIZE quads per side) that
// is drawn once per visible chunk with a different offset. Heights come from
// a float texture holding the whole map, read in the vertex shader, so the
// geometry never has to be rebuilt when the terrain changes: only the
// modified chunks of the texture are re-uploaded.
//
// Culling is a plain loop over chunks with a box-in-frustum test; visible
// chunks are drawn front to back so the depth test rejects hidden pixels
// early.
//
// Level of detail: the index buffer holds the grid at several steps (every
// vertex, every second, every fourth, ...). Each chunk picks a step from
// its distance to the camera, and vertices that the next coarser step drops
// slide smoothly onto the coarse surface as the distance grows
// ("geomorphing"), so levels change without popping and neighboring chunks
// of different levels meet without cracks.

#ifndef TERRAIN_RENDERER_H
#define TERRAIN_RENDERER_H

#include <stdbool.h>

#include "camera.h"
#include "gpu.h"
#include "materials.h"
#include "settings.h"
#include "shadow_map.h"
#include "soils.h"
#include "terrain.h"
#include "water.h"

// Widest reach of the material normal, in cells
#define TERRAIN_NORMAL_REACH_LIMIT 16

// Chunks prepared for upload at once
#define TERRAIN_UPLOAD_BATCH 32

// Steps 1, 2, 4, 8, 16, 32 cells between vertices
#define TERRAIN_LOD_LEVEL_COUNT 6

typedef enum
{
	TERRAIN_DEBUG_VIEW_NONE = 0,
	TERRAIN_DEBUG_VIEW_SOIL,
	TERRAIN_DEBUG_VIEW_MATERIALS,  // what the material map shows, flat colors
	TERRAIN_DEBUG_VIEW_COUNT
} terrain_debug_view_t;

typedef struct
{
	SDL_GPUGraphicsPipeline *pipeline;
	SDL_GPUGraphicsPipeline *wireframe_pipeline;
	// The same, the materials from the material map (terrain_map.frag)
	SDL_GPUGraphicsPipeline *map_pipeline;
	SDL_GPUGraphicsPipeline *map_wireframe_pipeline;
	SDL_GPUBuffer *vertex_buffer;  // the chunk grid: cell offsets (x, y), floats
	SDL_GPUBuffer *index_buffer;   // 16 bit, every level of detail one after the other
	int index_count;               // full detail grid, at the start of the buffer
	int lod_index_offset[TERRAIN_LOD_LEVEL_COUNT]; // in indices
	int lod_index_count[TERRAIN_LOD_LEVEL_COUNT];

	SDL_GPUTexture *height_texture;  // R32F, meters
	// RGBA8, filtered: silt cover, ground wetness, bare bedrock (0..1 each)
	SDL_GPUTexture *surface_texture;
	// R8UI, one texel per water tile: terrain_tile_overlay_t, for the overlay
	SDL_GPUTexture *tile_texture;
	int tile_texture_size;
	uint8_t *tile_staging;
	int texture_map_size;          // size the textures were created for
	// RGBA8, filtered, one texel per soil_cells_per_texel cells: blurred
	// soil weights (sand, earth, clay, rock), so the shader needs a single
	// lookup instead of a few dozen for soft borders between soils
	SDL_GPUTexture *soil_weight_texture;
	int soil_cells_per_texel;
	uint16_t *soil_box_counts;     // per texel and soil: cells of that soil
	uint8_t *soil_weight_staging;
	float soil_blend_meters;       // from the settings, decides soil_cells_per_texel
	// RGBA8, filtered: per cell surface normal (x, z in rg) and the wider
	// "material" normal that decides grass, dirt and rock (x, z in ba).
	// Per pixel normals keep the lighting detail on far, coarse chunks.
	SDL_GPUTexture *normal_texture;
	// RGBA8, one texel per cell: the material map (material_map.h), read
	// exactly (its bytes are material ids)
	SDL_GPUTexture *material_texture;
	uint8_t *material_staging;     // one chunk's
	// What settles on it (ground_overlay.h: snow, petals), set from outside
	// before drawing (NULL: nothing)
	SDL_GPUTexture *overlay_texture;
	SDL_GPUTexture *sky_occlusion_texture; // R8 a cell: the sky it sees (sky_occlusion.h; NULL: none, set every frame)
	SDL_GPUTexture *sky_layers_texture;    // and in the air (its layers; NULL with it)
	SDL_GPUTexture *sky_bounce_texture;    // and the light the land round sends back (NULL with it)
	float material_sun_azimuth_degrees; // the noon sun the bake goes by
	int material_normal_cells;     // reach of the material normal
	float material_normal_meters;  // from the settings

	material_set_t materials;

	// Chunks are prepared for upload in batches, in parallel, each in its
	// own slot; the uploads then go out one by one
	struct terrain_upload_slot_s *upload_slots;
	bool *chunk_needs_soil_blur;   // per chunk: its soil counts changed
	// Per chunk: changed but not uploaded yet (far away, see
	// far_terrain_upload_seconds), and when it last went up
	bool *chunk_pending_upload;
	// Per chunk, worked out every frame by terrain_renderer_upload: beyond
	// full detail or out of view, so its changes can wait (the water
	// renderer goes by it too; its tiles are the same squares)
	bool *chunk_far;
	bool *chunk_pending_shape;
	double *chunk_uploaded_seconds;
	bool any_chunk_pending;
	int64_t deferred_chunk_count;  // statistics: chunk uploads put off
	// Statistics since the last reset, for the F4 diagnostics
	double upload_milliseconds_sum;
	int64_t uploaded_chunk_count;
	int upload_frame_count;

	// Visible chunk list, rebuilt every frame
	struct visible_chunk_s
	{
		int chunk_x;
		int chunk_y;
		float distance_squared;
		int lod_level;
	} *visible_chunks;
	int visible_chunk_capacity;
	int visible_chunk_count;
	// The visible chunks' origins (cells, two int32 each), by level of
	// detail then near to far: the per instance vertex buffer (one draw per
	// level, instead of one per chunk), and where each level's run starts
	SDL_GPUBuffer *instance_buffer;
	int32_t *instance_staging;
	int instance_capacity;
	int level_first[TERRAIN_LOD_LEVEL_COUNT];
	int level_count[TERRAIN_LOD_LEVEL_COUNT];
	bool instances_ready;          // prepared this frame (terrain_renderer_prepare_draw)
	// The chunks with ground hidden under buildings (the material map's
	// MATERIAL_GROUND_HIDDEN, ground_footprints.h): at full detail each
	// drawn on its own with its quads but those left out (a quad whose four
	// cells are all hidden). Per chunk its indices (NULL: none hidden),
	// all of them one after the other in hole_index_buffer
	uint16_t **hole_indices;
	int *hole_index_counts;
	int *hole_index_offsets;
	int hole_chunk_slots;
	SDL_GPUBuffer *hole_index_buffer;
	bool holes_changed;
	int *holed_instances;          // this frame: the full detail chunks drawn with holes, their instances' places (after the rest)
	int *holed_chunks;             //   and which chunks
	int holed_count;
	int total_chunk_count;
	int chunks_per_lod_level[TERRAIN_LOD_LEVEL_COUNT]; // drawn last frame
	int triangles_drawn;           // last frame

	// 1x1 stand-ins while the overlays have nothing (the shader binds them anyway)
	SDL_GPUTexture *empty_integer_texture;

	bool wireframe;
	bool tile_overlay;             // tint the water tiles by state
	// The navigation overlay (F8): R8UI, one texel per navigation cell,
	// navigation_overlay_t (see navigation.h)
	bool navigation_overlay;
	SDL_GPUTexture *navigation_texture;
	int navigation_texture_size;
	float navigation_cell_meters;
	terrain_debug_view_t debug_view;
	// The bare bedrock and the silt as shown, per cell (two bytes, 0..255
	// each), and whether a chunk's
	// is there yet: it follows the ground a step at a time, so ground worn
	// down to the rock and covered again over and over (water running over
	// it) fades instead of flickering
	uint8_t *bare_shown;
	bool *chunk_bare_ready;
	// A baked map's whole terrain at once (a load): its normals made from
	// the heights on the GPU (terrain_normals.comp)
	SDL_GPUComputePipeline *normals_pipeline;
} terrain_renderer_t;

// The material map's bases worked out where the terrain asks for it
// (material_map_update by the renderer's rules): before every upload, and
// before a map is saved
void terrain_renderer_bake_materials(const terrain_renderer_t *renderer, terrain_t *terrain, const render_settings_t *settings);

// What the surface texture shows of a cell (silt, the ground's wetness,
// the bare bedrock; 0..255 each), as of now: from the layers, or a baked
// map's baked_surface
void terrain_surface_bytes(const terrain_t *terrain, const water_t *water, const soil_settings_t *soil_settings, int32_t index,
	uint8_t bytes_out[TERRAIN_BAKED_SURFACE_BYTES]);

// Brush outline drawn on the terrain: everything within radius of the
// segment start-end (a circle when they are the same point, a capsule for
// line stamps); radius <= 0 hides it
typedef struct
{
	float start_x;
	float start_z;
	float end_x;
	float end_z;
	float radius;
} brush_outline_t;

bool terrain_renderer_create(terrain_renderer_t *renderer, const render_settings_t *settings);
void terrain_renderer_destroy(terrain_renderer_t *renderer);

// Uploads every chunk flagged as changed: heights, soil types and the
// surface state (silt, wetness, bare bedrock, which needs the water's
// moisture). Recreates the textures if the map size changed. Returns true
// if anything was uploaded.
// Sends changed chunks to the GPU. Chunks beyond full detail or out of
// view go at most every settings->far_terrain_upload_seconds (now_seconds
// is the game's clock); the terrain's change flags are cleared either way
// (the renderer remembers what it still owes).
bool terrain_renderer_upload(terrain_renderer_t *renderer, terrain_t *terrain, water_t *water, const soil_settings_t *soil_settings,
	const camera_t *camera, const render_settings_t *settings, double now_seconds);

// What the tile overlay shows for a tile (values of tile_texture)
typedef enum
{
	TERRAIN_TILE_OVERLAY_DRY = 0,
	TERRAIN_TILE_OVERLAY_SLEEPING,
	TERRAIN_TILE_OVERLAY_ACTIVE,
	TERRAIN_TILE_OVERLAY_ACTIVE_WITH_MUD,
	TERRAIN_TILE_OVERLAY_SLEEPING_WITH_MUD
} terrain_tile_overlay_t;

// Uploads the water tile states for the overlay (only while it is on).
void terrain_renderer_upload_tiles(terrain_renderer_t *renderer, const water_t *water);

// Uploads the navigation overlay: size * size values of navigation_overlay_t
void terrain_renderer_upload_navigation(terrain_renderer_t *renderer, int size, float cell_meters, const uint8_t *values);

// Direction towards the sun from the render settings.
vec3_t terrain_renderer_sun_direction(const render_settings_t *settings);


// Cloud shadow uniforms for the given time: parameters = coverage,
// softness, strength, 1 / tiling; motion = wind offset x, z, cloud height.
void terrain_renderer_cloud_uniforms(const render_settings_t *settings, float time_seconds, float parameters_out[4], float motion_out[3]);

// Before the scene pass: the visible chunks found and their origins sent
// up (a pass cannot send anything)
void terrain_renderer_prepare_draw(terrain_renderer_t *renderer, const terrain_t *terrain, const camera_t *camera,
	const render_settings_t *settings);

// Draws the visible chunks into the scene pass (the scene's uniforms are
// already pushed): one instanced draw per level of detail
void terrain_renderer_draw(terrain_renderer_t *renderer, SDL_GPURenderPass *pass, const terrain_t *terrain, const camera_t *camera,
	const render_settings_t *settings, const brush_outline_t *brush, const shadow_map_t *shadow, const shadow_map_t *near_shadow);

#endif
