// The .pobj file: one object in one file (mesh, materials, textures),
// written by tools/potop_convert from glTF and read by object_model.c (which
// reads Eternal Lands' .e3d too). Physically based: each material has a
// base color, a normal map, occlusion-roughness-metal and emissive, as glTF
// has them, though the game draws only the base color for now.
//
// Everything is little endian, 4 byte aligned, in game axes (Y up, meters,
// +Z forward) and ready to use.
//
// Layout, in this order:
//   object_file_header_t
//   object_file_vertex_t      [vertex_count]
//   object_file_plant_vertex_t [vertex_count]    only with OBJECT_FILE_PLANT (version 3 on)
//   object_file_snow_vertex_t [vertex_count]     only with OBJECT_FILE_SNOW (version 6 on)
//   object_file_weathering_vertex_t [vertex_count] only with OBJECT_FILE_WEATHERING (version 7 on)
//   uint32_t                  [index_count]      three per triangle, counter clockwise
//   object_file_material_t    [material_count]   each a range of the indices
//   object_file_plant_material_t [material_count]  only with OBJECT_FILE_PLANT
//   object_file_month_cell_t  [month_cell_count] only with OBJECT_FILE_PLANT (version 5)
//   for each texture:
//     object_file_texture_t
//     compressed_size bytes   RGBA8 rows, top row first, compressed with
//                             zlib (deflate); or, for a reference, the
//                             file's name (see OBJECT_TEXTURE_REFERENCE)
//   uint32_t                  group count        only with OBJECT_FILE_EMISSIVE_GROUPS (version 8 on)
//   object_file_emissive_group_t [group count]
//   uint32_t                  effect count       only with OBJECT_FILE_EFFECTS (version 9 on)
//   object_file_effect_t      [effect count]
//   uint32_t                  footprint count    only with OBJECT_FILE_FOOTPRINT (version 10 on)
//   object_file_footprint_t   [footprint count]
//   uint32_t                  spot count         only with OBJECT_FILE_SPOTS (version 12 on)
//   object_file_spot_t        [spot count]
//
// The textures (a material refers to them by index, -1: none):
//   base color         sRGB; alpha: cut out below the material's cutoff
//   normal             tangent space, RGB = XYZ * 0.5 + 0.5
//   orm                R occlusion (1: none), G roughness, B metalness
//   emissive           sRGB
// A material's factors multiply its textures (1 where the converter
// already put them in); without a texture the factor alone is the value.

#ifndef OBJECT_FORMAT_H
#define OBJECT_FORMAT_H

#include <stdint.h>

#define OBJECT_FILE_MAGIC "POBJ"
// 12: places on it for the creatures and people (OBJECT_FILE_SPOTS, object_file_spot_t); 11: a footprint's cover (object_file_footprint_t.cover); 10: where it covers the ground (OBJECT_FILE_FOOTPRINT, object_file_footprint_t); 9: particle effects on it (OBJECT_FILE_EFFECTS, object_file_effect_t); 8: emissive groups (OBJECT_FILE_EMISSIVE_GROUPS, object_file_emissive_group_t); 7: a building's weathering baked in (OBJECT_FILE_WEATHERING, object_file_weathering_vertex_t); 6: a plant's snow baked in
// (OBJECT_FILE_SNOW, object_file_snow_vertex_t); 5: a plant's months (object_file_plant_material_t, object_file_month_cell_t); 3 and 4:
// plants before those (read as plain objects now); 2: textures may be references (OBJECT_TEXTURE_REFERENCE); 1 to 6 are read too
#define OBJECT_FILE_VERSION 12u
#define OBJECT_FILE_PLANT_MATERIAL_V4_BYTES 72u          // a version 4 plant material (18 floats), skipped
#define OBJECT_FILE_PLANT_MATERIAL_V3_BYTES 56u          // a version 3 plant material (14 floats), skipped
#define OBJECT_FILE_OLDEST_VERSION 1u
#define OBJECT_FILE_MAX_MATERIALS 16
#define OBJECT_FILE_NAME_LENGTH 32

// header flags
// A plant (tools/potop_plant): what its vertices are on the plant and how
// its materials look through the seasons follow the vertices and the
// materials (object_file_plant_vertex_t, object_file_plant_material_t).
// The game draws it with the plants' shaders (plant.vert, plant.frag)
#define OBJECT_FILE_PLANT 1u
// The wind leaves it be, whatever its type's sway and flutter (a mushroom:
// stiff, solid)
#define OBJECT_FILE_STILL 2u
// A plant with its winter's snow baked in (tools/potop_plant, version 6):
// where each vertex goes under all of it (object_file_snow_vertex_t: its
// branch sagging under the load, resting on the one below), and a material
// named "snow" (the snow lying on its boughs, OBJECT_FILE_SNOW_MATERIAL),
// as thin as nothing without it, its full depth under all of it. The game
// moves every vertex that far times the snow on things (plant.vert); the
// branches' sag by the material's (object_file_plant_material_t.sag) is
// then 0
#define OBJECT_FILE_SNOW 4u
#define OBJECT_FILE_SNOW_MATERIAL "snow"
// A building with its vertices' weathering baked in (version 7; the house
// generator): how open to the sky, how much of an edge, how sheltered from
// the rain each is (object_file_weathering_vertex_t), for its
// OBJECT_MATERIAL_AGED materials
#define OBJECT_FILE_WEATHERING 8u
// Emissive groups (version 8): parts of its emissive the game lights or puts
// out each on its own, per object (a house's windows, a panel's lamps:
// objects_set_emissive). A vertex's group is the whole part of its U
// coordinate (1 .. OBJECT_FILE_MAX_EMISSIVE_GROUPS; 0: none, always lit as
// it is): its textures repeat, so the whole part changes nothing of how it
// looks; the generator adds the group to the U of the faces it puts in
// one. The groups listed after the textures (object_file_emissive_group_t,
// the first is group 1)
#define OBJECT_FILE_EMISSIVE_GROUPS 16u
#define OBJECT_FILE_MAX_EMISSIVE_GROUPS 64
// Particle effects it carries (version 9): a forge's fire, a chimney's
// smoke, each a particles.ini effect at a place on the model
// (object_file_effect_t, after the emissive groups), played by the game
// wherever the object stands (object_effects.h) while the eye is near it
#define OBJECT_FILE_EFFECTS 32u
#define OBJECT_FILE_MAX_EFFECTS 16
// Where it covers the ground (version 10): outlines on the ground, model
// space, inside which the terrain is not drawn (a building's floor, its
// base: the ground under them is never seen) and no grass grows (an open
// shed's floor), worked into the material map wherever it stands
// (ground_footprints.h)
#define OBJECT_FILE_FOOTPRINT 64u
#define OBJECT_FILE_MAX_FOOTPRINTS 4
#define OBJECT_FILE_MAX_FOOTPRINT_POINTS 16
// Places on it for the creatures and people (version 12): where the hens
// peck and roost, where a smith stands at his anvil, its door outside and
// in; each named for what is done there, a point (or an area: a yard), a
// facing, and the places it leads to straight with nothing in the way
// (object_file_spot_t, after the footprints): the ways in and about it,
// baked (no search for a path at all). The game's minds go by them
// (potop_object_spot_get)
#define OBJECT_FILE_SPOTS 128u
#define OBJECT_FILE_MAX_SPOTS 32
#define OBJECT_FILE_SPOT_LINKS 4

// material flags
#define OBJECT_MATERIAL_ALPHA_TESTED 1u   // cut out: base color alpha below alpha_cutoff is not there
#define OBJECT_MATERIAL_DOUBLE_SIDED 2u
// Drawn but not in the way: physics, navigation and the water's roofs leave
// it out (a tree's branches; alpha tested materials are never solid)
#define OBJECT_MATERIAL_PASSABLE 4u
// Its normals light it rather than tell which way its faces look (a
// plant's cards: lit as the crown is): the same seen from either side, not
// turned round on the back of a face as a real leaf's are
#define OBJECT_MATERIAL_LIGHTING_NORMALS 8u
// Levels of detail (tools/potop_plant: a tree's branches): its indices are
// in the order they go away with distance, and the first lod_index_count[k]
// of them draw it at level k + 1 (fewer and fewer; 0: none of it). Not
// set: all of it at every level
#define OBJECT_MATERIAL_LODS 16u
// Its triangles in a random order, whole cards (two triangles, six indices)
// together: any first part of them (a multiple of six) is the whole
// thinned out evenly (a tree's foliage, for the distance)
#define OBJECT_MATERIAL_SHUFFLED 32u
// Its textures laid on it from the sides and from above and below, by the
// model's own axes (triplanar), triplanar_meters one repeat (tools/potop_prop:
// rocks: no texture coordinates). Its vertices' tangents are then no
// tangents but, with OBJECT_MATERIAL_WEATHERED, how weathered each is
// (x: in a crevice, y: on an edge, z: open to the sky, 0..1)
#define OBJECT_MATERIAL_TRIPLANAR 64u
// Moss, lichen and snow grow on it as the world has it (prop.frag): where
// it faces up and north, wet, in the snow's months; triplanar ones by their
// vertices' weathering (above)
#define OBJECT_MATERIAL_WEATHERED 128u
// With OBJECT_MATERIAL_LODS: its levels of detail are meshes of their own
// (tools/potop_prop: simplified, their own vertices), not a first part of
// its indices: level 1's lod_index_count[0] indices follow its own
// (index_offset + index_count), level 2's follow those, then level 3's.
// Only its own are solid (collisions); the levels are only drawn
#define OBJECT_MATERIAL_LOD_MESHES 256u
// Weathered as a building (building.frag): by its vertices' weathering
// (OBJECT_FILE_WEATHERING; without it, open to the sky, no edge, no
// shelter) and the world (the ground under it, the rain, the snow, which way
// north is), aged by its type's age (objects.ini). With its own texture
// coordinates and tangents, unlike OBJECT_MATERIAL_WEATHERED (triplanar
// props, their weathering in the tangents)
#define OBJECT_MATERIAL_AGED 1024u
// Only a collision shape, never drawn (a building's simple shell around its
// detailed beams; tools/potop_house): solid however else it is marked
#define OBJECT_MATERIAL_COLLISION_ONLY 512u
// With OBJECT_MATERIAL_AGED: thatch, its texture's relief marched into
// (prop_body.glsl's parallax_uv; a building's reed roof, tools/potop_house)
#define OBJECT_MATERIAL_THATCH 2048u
// A climber that drops its leaves (Virginia creeper on a wall): its green
// reddens in the autumn and is gone over the winter, its brown (the vines)
// stays (object.frag, by the month)
#define OBJECT_MATERIAL_DECIDUOUS 4096u

// texture kinds
#define OBJECT_TEXTURE_COLOR 0u           // sRGB
#define OBJECT_TEXTURE_DATA 1u            // linear values (normals, orm)
// Or'ed into the kind (version 2): not pixels but a texture file's name,
// relative to data/ (compressed_size bytes, zero terminated and padded to
// 4; width and height 0). The game loads such a file once for every model
// that names it (atlases, bark shared by many trees: tools/potop_plant),
// and again when it is written while the game runs
#define OBJECT_TEXTURE_REFERENCE 0x100u

typedef struct
{
	char magic[4];                        // OBJECT_FILE_MAGIC
	uint32_t version;                     // OBJECT_FILE_VERSION
	uint32_t vertex_count;
	uint32_t index_count;
	uint32_t material_count;
	uint32_t texture_count;
	uint32_t flags;                       // OBJECT_FILE_PLANT
	float bounds_minimum[3];
	float bounds_maximum[3];
	uint32_t month_cell_count;            // a plant's (object_file_month_cell_t)
	// OBJECT_MATERIAL_WEATHERED materials: how much moss and lichen grow on
	// them (0..1; tools/potop_prop; 0 before: was reserved)
	float moss;
	float lichen;
	// OBJECT_MATERIAL_LOD_MESHES: how far each level's surface is from the
	// real one (meters at size 1, its worst; tools/potop_prop): the game
	// draws a level once that is less than objects.prop_lod_pixels on
	// screen (0 before: was reserved)
	float lod_errors[3];
} object_file_header_t;

typedef struct
{
	float position[3];
	float normal[3];
	float tangent[4];                     // xyz along U, w the bitangent's sign
	float uv[2];
} object_file_vertex_t;

typedef struct
{
	char name[OBJECT_FILE_NAME_LENGTH];   // zero terminated, zero padded
	uint32_t flags;                       // OBJECT_MATERIAL_*
	uint32_t index_offset;
	uint32_t index_count;
	float base_color[4];                  // linear factor (RGBA)
	float emissive[3];
	float metallic;
	float roughness;
	float normal_scale;
	float occlusion_strength;
	float alpha_cutoff;
	int32_t base_color_texture;
	int32_t normal_texture;
	int32_t orm_texture;
	int32_t emissive_texture;
	uint32_t lod_index_count[3];          // OBJECT_MATERIAL_LODS: its indices drawn at levels of detail 1, 2, 3
	float triplanar_meters;               // OBJECT_MATERIAL_TRIPLANAR: how far one repeat of its textures goes (0 before: was reserved)
} object_file_material_t;

// A plant's vertex, what it is on the plant (OBJECT_FILE_PLANT)
typedef struct
{
	float along;                          // 0 at the foot of its branch, card or stalk .. 1 at its tip
	float height;                         // 0 at the plant's foot .. 1 at its top
	float random;                         // 0..1, one per card (foliage), per branch (bark)
	float extra;                          // foliage: how far out in the crown (0 its middle .. 1 its outside); bark: how thick (1 the trunk's foot)
} object_file_plant_vertex_t;

// Where a plant's vertex goes under all of the winter's snow (OBJECT_FILE_SNOW)
typedef struct
{
	float offset[3];                      // model space, meters at size 1, from where it is (0: it stays)
	float snow;                           // 1: a vertex of the snow lying on it (drawn only while there is snow), 0: of the plant
} object_file_snow_vertex_t;

// How weathered a vertex of a building is (OBJECT_FILE_WEATHERING), each
// 0..1, baked by the generator
typedef struct
{
	float sky_open;                       // 0: closed in (a joint, under the eaves) .. 1: open to the sky
	float edge;                           // 0: on a flat face .. 1: on a sharp outer edge (worn there), labeled by the generator
	float shelter;                        // 0: the rain beats on it .. 1: always dry (under a roof)
	float kind;                           // OBJECT_WEATHERING_*: what it is, to age by (its whole part, not used yet); its fraction /0.99: how close something stands out over it (a jetty, the eaves: 1 right under it)
} object_file_weathering_vertex_t;

// What a building's vertex is (object_file_weathering_vertex_t kind)
#define OBJECT_WEATHERING_WOOD 0.0f
#define OBJECT_WEATHERING_PLASTER 1.0f
#define OBJECT_WEATHERING_STONE 2.0f
#define OBJECT_WEATHERING_ROOF 3.0f
// Blackened by smoke (a smithy's hood, its chimney): the fraction is then
// how much soot, not how close something stands out over it
#define OBJECT_WEATHERING_SOOTED_PLASTER 4.0f
#define OBJECT_WEATHERING_SOOTED_STONE 5.0f

// A plant's material through the seasons (OBJECT_FILE_PLANT; colors linear)
typedef struct
{
	float autumn_colors[2][3];            // foliage: what it turns to in the autumn, a card somewhere between the two
	float autumn;                         // how much it turns (0: evergreen)
	float falls;                          // 1: it falls once turned (deciduous)
	float moss;                           // bark: how much moss its north side grows (0: none)
	float tips;                           // foliage: how much lighter its tips are (the new growth)
	float sag;                            // how far its branches sag under the winter's snow (of the plant's height, at the crown's edge)
	// The months (1..12) of its autumn: it turns from the start of the
	// first, falls (where it does) by the end of the last, and stays bare
	// until the leaves' month starts
	float autumn_first_month;
	float autumn_last_month;
	float leaves_month;
	// The wind in it (the same in all its materials; 0 before: was
	// reserved): wind_given 1 for these, else its type's alone
	// (objects.ini) decides; a type's own sway, flutter, stiffness there
	// still win
	float wind_given;
	float sway;                           // its top's lean in a gale (meters at size 1)
	float flutter;                        // its leaves' shaking (meters at size 1)
	float stiffness;                      // how hard the wind must blow before it bends (0: at once .. 1: never)
} object_file_plant_material_t;

// A cell of a plant's atlas whose picture changes with the months (its
// months' own pictures are in cells of their own: tools/plant_grow.c):
// where it is (0..1 of the atlas: u0, v0, u1, v1) and, each month (January
// first), how far that month's picture is from it (u, v; 0, 0: itself). A
// card whose texture coordinates fall in the cell is moved there
typedef struct
{
	float cell[4];
	float month_offsets[12][2];
} object_file_month_cell_t;

// The first of a plant's month cells (at most the shaders' 32: plant.glsl's
// PLANT_MONTH_CELLS) a vertex's texture coordinates fall in, -1 none: fixed
// for every vertex, so worked out once as it is loaded and handed to
// plant.vert with its snow (plant_snow.w: its flag, 0 or 1, plus 2 times
// the cell plus one), not looked for among them all at every vertex
static inline int object_month_cell_of(const object_file_month_cell_t *cells, int count, const float uv[2])
{
	for (int c = 0; c < count && c < 32; c++)
	{
		if (uv[0] >= cells[c].cell[0] && uv[1] >= cells[c].cell[1] && uv[0] <= cells[c].cell[2] && uv[1] <= cells[c].cell[3])
		{
			return c;
		}
	}
	return -1;
}

// plant_snow.w as plant.vert takes it: the snow lying on it (0 or 1) and
// its month cell (object_month_cell_of)
static inline float object_plant_snow_word(float snow_flag, int cell)
{
	return (snow_flag > 0.5f ? 1.0f : 0.0f) + 2.0f * (float)(cell + 1);
}

typedef struct
{
	uint32_t width;
	uint32_t height;
	uint32_t kind;                        // OBJECT_TEXTURE_*
	uint32_t compressed_size;
} object_file_texture_t;

// An emissive group (OBJECT_FILE_EMISSIVE_GROUPS): what the game knows of it
// to choose which to light (a house's windows: which floor, which side)
typedef struct
{
	char name[OBJECT_FILE_NAME_LENGTH];   // zero terminated, zero padded (a house's windows: "window"), the same for groups alike
	float center[3];                      // model space (meters at size 1)
	int32_t floor;                        // a house's: which floor (0: the ground floor); -1: none
	int32_t side;                         // a house's: which face (0 front, 1 right, 2 back, 3 left); -1: none
	uint32_t flags;                       // OBJECT_EMISSIVE_*
} object_file_emissive_group_t;

// emissive group flags
#define OBJECT_EMISSIVE_OFF_AT_FIRST 1u   // put out until the game lights it (else lit, as without groups)
#define OBJECT_EMISSIVE_FACING 2u         // the way it faces out is in the flags' second byte (else unknown)
#define OBJECT_EMISSIVE_FACING_SHIFT 8u   // that byte: its yaw, 0..255 for a whole turn from +z towards +x (x = sin, z = cos)
#define OBJECT_EMISSIVE_FACING_MASK 0xff00u

// An outline where it covers the ground (OBJECT_FILE_FOOTPRINT): its
// points round it (model x, z, meters at size 1; either way round, convex
// or not), what is done inside
typedef struct
{
	uint32_t flags;                       // OBJECT_FOOTPRINT_*
	uint32_t point_count;                 // 3 .. OBJECT_FILE_MAX_FOOTPRINT_POINTS
	float points[OBJECT_FILE_MAX_FOOTPRINT_POINTS][2];
	// (version 11) OBJECT_FOOTPRINT_COVER: the terrain material laid on
	// the cells it touches (materials.h's name: "trampled_dirt"), zero
	// terminated, zero padded
	char cover[OBJECT_FILE_NAME_LENGTH];
} object_file_footprint_t;

// A version 10 footprint: without the cover
#define OBJECT_FILE_FOOTPRINT_V10_BYTES (8u + 8u * OBJECT_FILE_MAX_FOOTPRINT_POINTS)

// footprint flags
#define OBJECT_FOOTPRINT_NO_GRASS 1u      // no grass in the cells it touches at all
#define OBJECT_FOOTPRINT_NO_TERRAIN 2u    // the terrain not drawn in the cells it covers whole (the floor over them hides them)
#define OBJECT_FOOTPRINT_COVER 4u         // its cover (a terrain material) on the cells it touches: a floor on the ground itself
// The water's wall: only the cells inside such outlines are walls for the
// water simulation (the building itself, not its yard, fences, pens or
// eaves: physics.c's rebuild_walls). None: those that hide the terrain
// (NO_TERRAIN) stand for it; none of those either: the whole model
#define OBJECT_FOOTPRINT_WATER_WALL 8u

// A place on the object (OBJECT_FILE_SPOTS)
typedef struct
{
	char name[OBJECT_FILE_NAME_LENGTH];   // what it is ("yard", "hen_hole", "peck"...), zero terminated, zero padded
	float position[3];                    // model space (meters at size 1); an area's middle
	float yaw_degrees;                    // which way one faces there (round Y, 0 looking down +Z); an area's turn
	float extents[2];                     // an area: half its size along its own x and z (0, 0: a point)
	int32_t links[OBJECT_FILE_SPOT_LINKS]; // the spots it leads to straight (indices; -1: none), both ways
	uint32_t flags;                       // none yet: 0
} object_file_spot_t;

// A particle effect on the object (OBJECT_FILE_EFFECTS)
typedef struct
{
	char effect[OBJECT_FILE_NAME_LENGTH]; // the particles.ini section, zero terminated, zero padded
	float center[3];                      // model space (meters at size 1): the middle of where it comes from
	float extents[3];                     // half its size there, model axes (0, 0, 0: a point)
	float rate_scale;                     // its rate times this (0: 1)
	uint32_t flags;                       // OBJECT_EFFECT_* (none yet: 0)
} object_file_effect_t;


#endif
