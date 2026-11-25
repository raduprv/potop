// Growing plants (see plant_grow.h): a plant, procedurally, into an
// object for the game (.pobj, see engine/object_format.h): trees, bushes, and
// the small things on the ground (grass tufts, rosettes like dandelions,
// ferns, flowers). For the desktop tools (potop_plant, plant_editor): not
// part of the game, never shipped with it.
//
// Trees are grown the way Weber and Penn's "Creation and rendering of
// realistic trees" (1995) does it, simplified: a trunk, branches out of it,
// smaller ones out of those, each a bent tube that tapers; they spiral
// round their parent by the golden angle, lean out from it, droop or reach
// up, and are longest where the crown is widest. On the finest branches go
// leaves, cards cut out by their texture's alpha; or a twig texture (a
// sprig with its leaves) stands in for the finest branches, two crossed
// cards each; or some of each (--twig-share), and leaves along the other
// branches as well (--loose-leaves). Fruit hangs from the finest branches
// or sits on them (--fruit): cherries, nuts, cones, pods, small solids
// (spheres, spindles, pods) wrapped in their picture, or crossed cards; on
// a stalk, in clusters, where in the crown they grow. The small plants are
// cards too: in a star (tufts), lying in a
// ring with stalks and heads (rosettes), arched (ferns), on stems (flowers).
// The same seed grows the same plant; another seed, another of the kind.
// The plant stands on the origin, its foot at 0, in meters, Y up.
//
// The leaves, sprigs, fronds, blades and heads all live in atlases shared
// by every plant of the game: data/textures/conifers, broadleaved (trees,
// bushes) and small_plants (the rest), each a .dds (BC7, a layer for each
// part of the year that looks its own: engine/plant_atlas_file.h) with an
// .ini that lists what is in which cell. A picture's months are pictures
// of their own beside it (twig_m1.png .. twig_m12.png: a month shows its
// own, or the latest one before it round the year), all of one size, cut
// to one box so they lie on one another exactly. A leaf given here that is not in its atlas yet goes in
// the first free cell that fits (square ones, and tall ones for long
// leaves: see the .ini); one that is there already keeps its cell. The
// atlases are made again from the .ini's list each time (so a line taken
// out frees its cell, a source drawn again shows), and written only if
// they changed. The objects name the atlases and the bark rather than
// carry their pixels: the game loads each once for every plant, and again
// when it is written while the game runs (so a plant made again shows at
// once beside the open game).
//

// realpath (POSIX)
#define _XOPEN_SOURCE 700

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>

#include <stdarg.h>

#include <pthread.h>
#include <unistd.h>

#include "bc7enc/bc7enc.h"
#include "plant_atlas_file.h"
#include "plant_grow.h"
#include "stb_image.h"
#include "stb_image_write.h"

#define PI_F 3.14159265f
#define GOLDEN_ANGLE_DEGREES 137.5f
#define MAX_LEVELS PLANT_MAX_LEVELS
#define PATH_LENGTH PLANT_PATH_LENGTH
#define MAX_SOURCES PLANT_MAX_PICTURES

#define MATERIAL_TRUNK PLANT_MATERIAL_TRUNK
#define MATERIAL_BARK PLANT_MATERIAL_BARK
#define MATERIAL_FOLIAGE PLANT_MATERIAL_FOLIAGE
#define MATERIAL_FRUIT PLANT_MATERIAL_FRUIT
#define MATERIAL_SNOW PLANT_MATERIAL_SNOW
#define MATERIAL_COUNT PLANT_MATERIAL_COUNT

// What went wrong last (plant_last_error); also said on stderr
static char error_text[1024];

static void plant_error(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(error_text, sizeof(error_text), format, arguments);
	va_end(arguments);
	fputs(error_text, stderr);
	// Without the line's end, for a window to show
	{
		size_t length = strlen(error_text);

		while (length > 0 && (error_text[length - 1] == '\n' || error_text[length - 1] == '\r'))
		{
			error_text[--length] = '\0';
		}
	}
}

const char *plant_last_error(void)
{
	return error_text;
}

// ---------------------------------------------------------------------------
// The kinds

// How fruit grows, and where in the crown, and its shape (plant_grow.h)
typedef plant_fruit_hang_t fruit_hang_t;
typedef plant_fruit_zone_t fruit_zone_t;
typedef plant_fruit_shape_t fruit_shape_t;
#define FRUIT_DOWN PLANT_FRUIT_DOWN
#define FRUIT_UP PLANT_FRUIT_UP
#define FRUIT_OUT PLANT_FRUIT_OUT
#define FRUIT_OUTER PLANT_FRUIT_OUTER
#define FRUIT_TOP PLANT_FRUIT_TOP
#define FRUIT_ALL PLANT_FRUIT_ALL
#define FRUIT_SPHERE PLANT_FRUIT_SPHERE
#define FRUIT_SPINDLE PLANT_FRUIT_SPINDLE
#define FRUIT_POD PLANT_FRUIT_POD
#define FRUIT_CARD PLANT_FRUIT_CARD

typedef enum
{
	SHAPE_TREE,
	SHAPE_TUFT,
	SHAPE_ROSETTE,
	SHAPE_FERN,
	SHAPE_FLOWER,
	SHAPE_MUSHROOM
} shape_t;

// The outline of a tree's crown: how long the trunk's branches are up it
typedef enum
{
	CROWN_ROUND,                        // widest a little above the middle (oak, birch)
	CROWN_CONE,                         // widest at the foot, to a point (spruce, fir)
	CROWN_COLUMN,                       // about as wide all the way, rounded at the top (thuja, juniper)
	CROWN_DOME                          // a bare trunk, the crown high and wide (pine)
} crown_t;

typedef struct
{
	const char *name;
	const char *alias;                  // an older name for it
	shape_t shape;
	const char *atlas;
	float height_meters;
	// Trees
	int stems;                          // trunks out of the ground (bushes have several)
	float stem_lean_degrees;            // how far the stems lean out from upright
	// The stems lie out each its own way (stem_lean_degrees from upright),
	// for lean_length of their length, then turn up again (straighten), as
	// the wind's lean does a trunk: a mountain pine's, a creeping shrub's
	bool sprawl;
	int levels;                         // branch levels under the trunk (1..MAX_LEVELS)
	float trunk_radius_ratio;           // the trunk's radius at its foot, of the height
	float taper;                        // a branch's radius at its tip, of its foot's
	float trunk_taper;                  // the trunk's (0: taper)
	float thinnest_radius;              // no branch thinner than this (m; 0: THINNEST_BRANCH_RADIUS)
	float bend;                         // random bend, radians per meter
	float crown_start;                  // the trunk's first branches, as a fraction of its height
	int children[MAX_LEVELS];           // branches out of each one of the level above
	float length_ratio[MAX_LEVELS];     // their length, of their parent's
	float down_degrees[MAX_LEVELS];     // how far they lean out from their parent
	float down_spread_degrees;          // and give or take this much
	float reach[MAX_LEVELS];            // up (+) or down (-) they curve, per meter of their length
	float child_radius_ratio;           // a branch's radius at its foot, of its parent's there
	crown_t crown;
	float child_start;                  // the first branches out of a branch (not the trunk), as a fraction of its length
	int leaves_per_branch;              // on each of the finest branches
	// Twigs (--twigs): a sprig picture for each finest branch, and
	float twig_scale;                   // its length, of the branch it stands for (above 1: they overlap)
	float side_twigs;                   // more along the other branches (not the trunk), per meter of them
	bool tip_twig;                      // one more at the end of each of those
	float twig_flat;                    // 0: the two crossed cards at random; 1: lying flat, the way fir sprays do
	float twig_meters;                  // a finest branch longer than this has twigs one after the other along it (0: one)
	bool twig_curved;                   // one card each, arched across, drooping, a little twisted (add_curved_twig) instead of two crossed
	bool branch_cards;                  // the finest branches are whole boughs, each a picture of one seen from above, along it (add_branch_card)
	// Each twig a small spray of real shoots (a pine's): a leader and shoots
	// round it, each a thin bark tube with its needles in open cones round
	// it, the shoot through their middles (the twig picture: needles
	// standing side by side); no flat picture of a twig
	bool brushes;
	int brush_strips;                   // strips round each cone
	float brush_spacing;                // cones along a shoot this many needle lengths apart
	fruit_hang_t fruit_hang;            // its fruit (--fruit): how it grows
	fruit_zone_t fruit_zone;            // and where
	// The branches' shape under their own weight: how far they bend down
	// (radians per meter per meter of the branch: bent more the longer it
	// is, more towards its tip, as a loaded beam), and how far their tips
	// turn up again (radians per meter, over their outer third)
	float droop;
	float tip_lift;
	// Thicker where they join: a branch's foot swells by this much of its
	// radius (the collar), the trunk's foot too (towards the roots)
	float collar;
	float root_flare;
	// Foliage only above this much of the plant's height (below it, the
	// branches are bare but for their tips, a little way down)
	float leaves_from;
	// Dead branches on the bare trunk under the crown (a pine's, shaded out
	// as it grew up): this many to a stem, bare, thin, mostly broken short,
	// the longer the nearer the crown
	float dead_branches;
	// The trunk kept straight the way it set out (0: it wanders with bend
	// and gnarl; 1: straight, a conifer's)
	float trunk_upright;
	// The light's middle (crown_normal: the foliage's normals out from it)
	// this many times the height below the crown's foot, in the middle: a
	// mat on the ground lit from above, not out to its sides (0: as is)
	float light_from_below;
	// Stems grown together: one trunk up to fork_height (of the height),
	// then stems parting (0: apart from the ground); one stem more than
	// stems by this chance (by the seed)
	float fork_height;
	float extra_stem_chance;
	// Gnarled: the branches wander, swell and narrow (0: smooth); kinked:
	// a branch turns away from each one that leaves it (sympodial growth)
	float gnarl;
	float kinks;
	float gnarl_branches;               // the branches' gnarl, of the trunk's (0: as much; pollards' shoots are straight)
	// Bent by the wind: the trunk leaves the ground lean_degrees from
	// upright, goes so for lean_length of its length, then turns up again
	// (straighten: 1 upright by its top, 0 not at all); the branches on the
	// side the wind comes from shorter and fewer (wind_side)
	float lean_degrees;
	float lean_length;
	float straighten;
	float wind_side;
	// The trunk's head swells into a knob (pollards, where the shoots are cut back year after year)
	float knob;
	// Through the seasons (plant.frag): the moss on the bark's north side;
	// how much lighter the foliage's tips are (the new growth); what the
	// foliage turns to in the autumn (sRGB, a card somewhere between the
	// two), how much (0: evergreen), and whether it falls then
	float moss;
	float tips;
	float autumn_colors[2][3];
	float autumn;
	bool falls;
	// The months (1..12) the autumn takes: it turns from the start of the
	// first, falls (where it does) by the end of the last, and stays bare
	// until leaves_month comes (0: October, November and April)
	int autumn_first_month;
	int autumn_last_month;
	int leaves_month;
	// How far the branches sag under the winter's snow (of the height, at
	// the crown's edge)
	float snow_sag;
	// All
	float leaf_size;                    // meters
	int parts;                          // small plants: blades, leaves, fronds or stems
} plant_kind_t;

// Each kind is a species, roughly; trees take their twigs (or leaves) from
// the options. A first guess at each, from how they grow: tune with the
// options (--branches, --leaves-per-branch, --twig-scale, --branch-length)
static const plant_kind_t kinds[] =
{
	// Broadleaved
	{
		.name = "oak", .alias = "deciduous", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 11.0f, .stems = 1,
		.stem_lean_degrees = 3.0f, .levels = 3, .trunk_radius_ratio = 0.03f, .taper = 0.35f, .bend = 0.18f, .crown_start = 0.3f,
		.children = { 9, 7, 6 }, .length_ratio = { 0.6f, 0.45f, 0.4f }, .down_degrees = { 58.0f, 48.0f, 40.0f },
		.down_spread_degrees = 14.0f, .reach = { 0.03f, 0.02f, 0.0f }, .child_radius_ratio = 0.6f, .crown = CROWN_ROUND,
		.child_start = 0.15f, .leaves_per_branch = 10, .leaf_size = 0.16f,
		.twig_scale = 1.2f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 0.0f,
		.droop = 0.015f, .tip_lift = 0.0f, .collar = 0.6f, .root_flare = 0.5f, .leaves_from = 0.0f,
		.fork_height = 0.25f, .extra_stem_chance = 0.15f, .gnarl = 0.5f, .kinks = 0.5f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.60f, .tips = 0.25f, .autumn_colors = { { 0.55f, 0.36f, 0.12f }, { 0.72f, 0.52f, 0.18f } }, .autumn = 1.0f, .falls = true,
		.snow_sag = 0.030f,
	},
	{
		.name = "birch", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 14.0f, .stems = 1, .stem_lean_degrees = 4.0f,
		.levels = 3, .trunk_radius_ratio = 0.016f, .taper = 0.3f, .bend = 0.15f, .crown_start = 0.35f,
		.children = { 11, 8, 7 }, .length_ratio = { 0.42f, 0.5f, 0.55f }, .down_degrees = { 38.0f, 45.0f, 55.0f },
		.down_spread_degrees = 10.0f, .reach = { 0.04f, -0.06f, -0.2f }, .child_radius_ratio = 0.55f, .crown = CROWN_ROUND,
		.child_start = 0.15f, .leaves_per_branch = 12, .leaf_size = 0.07f,
		.twig_scale = 1.2f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 0.0f,
		.droop = 0.035f, .tip_lift = 0.0f, .collar = 0.35f, .root_flare = 0.2f, .leaves_from = 0.0f,
		.fork_height = 0.03f, .extra_stem_chance = 0.2f, .gnarl = 0.15f, .kinks = 0.1f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.25f, .tips = 0.25f, .autumn_colors = { { 0.95f, 0.78f, 0.18f }, { 0.85f, 0.62f, 0.10f } }, .autumn = 1.0f, .falls = true,
		.snow_sag = 0.040f,
	},
	{
		.name = "willow", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 10.0f, .stems = 1, .stem_lean_degrees = 6.0f,
		.levels = 3, .trunk_radius_ratio = 0.035f, .taper = 0.4f, .bend = 0.2f, .crown_start = 0.28f,
		.children = { 8, 7, 9 }, .length_ratio = { 0.6f, 0.5f, 0.8f }, .down_degrees = { 50.0f, 40.0f, 30.0f },
		.down_spread_degrees = 12.0f, .reach = { 0.06f, -0.1f, -0.6f }, .child_radius_ratio = 0.55f, .crown = CROWN_ROUND,
		.child_start = 0.1f, .leaves_per_branch = 14, .leaf_size = 0.1f,
		.twig_scale = 1.2f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 0.0f,
		.droop = 0.04f, .tip_lift = 0.0f, .collar = 0.5f, .root_flare = 0.4f, .leaves_from = 0.0f,
		.fork_height = 0.2f, .extra_stem_chance = 0.25f, .gnarl = 0.4f, .kinks = 0.3f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.60f, .tips = 0.30f, .autumn_colors = { { 0.78f, 0.72f, 0.25f }, { 0.62f, 0.55f, 0.18f } }, .autumn = 0.8f, .falls = true,
		.snow_sag = 0.030f,
	},
	{
		// Salcie pletoasa: by the water, its thick limbs arch out and long thin
		// whips hang from them almost to the ground, leafy all along
		.name = "weeping_willow", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 11.0f, .stems = 1,
		.stem_lean_degrees = 5.0f, .levels = 3, .trunk_radius_ratio = 0.04f, .taper = 0.15f, .bend = 0.15f, .crown_start = 0.25f,
		.children = { 7, 6, 10 }, .length_ratio = { 0.55f, 0.55f, 1.6f }, .down_degrees = { 45.0f, 40.0f, 70.0f },
		.down_spread_degrees = 12.0f, .reach = { 0.05f, -0.15f, -2.5f }, .child_radius_ratio = 0.5f, .crown = CROWN_ROUND,
		.child_start = 0.1f, .leaves_per_branch = 30, .leaf_size = 0.15f,
		.twig_scale = 1.0f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 0.0f, .twig_meters = 0.6f,
		.droop = 0.06f, .tip_lift = 0.0f, .collar = 0.4f, .root_flare = 0.5f, .leaves_from = 0.0f,
		.fork_height = 0.2f, .extra_stem_chance = 0.2f, .gnarl = 0.35f, .kinks = 0.2f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.60f, .tips = 0.30f, .autumn_colors = { { 0.80f, 0.74f, 0.25f }, { 0.66f, 0.60f, 0.20f } }, .autumn = 0.8f, .falls = true,
		.snow_sag = 0.030f,
	},
	{
		// Salcie capatanata: cut back year after year, a short thick gnarled
		// trunk with a knob for a head and a broom of straight shoots out of it
		.name = "pollard_willow", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 3.0f, .stems = 1,
		.stem_lean_degrees = 8.0f, .levels = 2, .trunk_radius_ratio = 0.1f, .taper = 0.85f, .bend = 0.2f, .crown_start = 0.84f,
		.children = { 34, 10, 0 }, .length_ratio = { 1.0f, 0.22f, 0.0f }, .down_degrees = { 24.0f, 50.0f, 0.0f },
		.down_spread_degrees = 14.0f, .reach = { 0.0f, 0.0f, 0.0f }, .child_radius_ratio = 0.05f, .crown = CROWN_COLUMN,
		.child_start = 0.15f, .leaves_per_branch = 12, .leaf_size = 0.13f,
		.twig_scale = 1.2f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 0.0f,
		.droop = 0.01f, .tip_lift = 0.0f, .collar = 0.3f, .root_flare = 0.5f, .leaves_from = 0.0f,
		.fork_height = 0.0f, .extra_stem_chance = 0.0f, .gnarl = 0.6f, .kinks = 0.0f, .gnarl_branches = 0.12f,
		.lean_length = 0.3f, .straighten = 0.8f, .knob = 0.7f,
		.moss = 0.80f, .tips = 0.30f, .autumn_colors = { { 0.78f, 0.72f, 0.25f }, { 0.62f, 0.55f, 0.18f } }, .autumn = 0.8f, .falls = true,
		.snow_sag = 0.010f,
	},
	{
		.name = "bush", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 1.8f, .stems = 6, .stem_lean_degrees = 28.0f,
		.levels = 2, .trunk_radius_ratio = 0.012f, .taper = 0.3f, .bend = 0.6f, .crown_start = 0.2f,
		.children = { 6, 5, 0 }, .length_ratio = { 0.5f, 0.45f, 0.0f }, .down_degrees = { 40.0f, 40.0f, 0.0f },
		.down_spread_degrees = 15.0f, .reach = { 0.1f, 0.05f, 0.0f }, .child_radius_ratio = 0.7f, .crown = CROWN_ROUND,
		.child_start = 0.15f, .leaves_per_branch = 9, .leaf_size = 0.09f,
		.twig_scale = 1.3f, .side_twigs = 2.0f, .tip_twig = true, .twig_flat = 0.0f,
		.droop = 0.03f, .tip_lift = 0.0f, .collar = 0.3f, .root_flare = 0.0f, .leaves_from = 0.0f,
		.fork_height = 0.0f, .extra_stem_chance = 0.3f, .gnarl = 0.3f, .kinks = 0.2f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.00f, .tips = 0.30f, .autumn_colors = { { 0.70f, 0.40f, 0.16f }, { 0.80f, 0.62f, 0.24f } }, .autumn = 1.0f, .falls = true,
		.snow_sag = 0.030f,
	},
	// Conifers: the needles come as twigs; sprigs along the branches too,
	// bigger than the branch they stand for, so the crown closes
	{
		// Molid: a tall cone, the branches hang and turn up at their tips
		.name = "spruce", .alias = "conifer", .shape = SHAPE_TREE, .atlas = "conifers", .height_meters = 16.0f, .stems = 1,
		.stem_lean_degrees = 1.0f, .levels = 2, .trunk_radius_ratio = 0.02f, .taper = 0.08f, .bend = 0.05f, .crown_start = 0.1f,
		.children = { 50, 14, 0 }, .length_ratio = { 0.28f, 0.32f, 0.0f }, .down_degrees = { 98.0f, 62.0f, 0.0f },
		.down_spread_degrees = 6.0f, .reach = { 0.05f, -0.12f, 0.0f }, .child_radius_ratio = 0.35f, .crown = CROWN_CONE,
		.child_start = 0.05f, .leaves_per_branch = 5, .leaf_size = 0.45f,
		.twig_scale = 1.7f, .side_twigs = 4.0f, .tip_twig = true, .twig_flat = 0.35f, .fruit_hang = FRUIT_DOWN, .fruit_zone = FRUIT_TOP,
		.droop = 0.05f, .tip_lift = 0.5f, .collar = 0.1f, .root_flare = 0.3f, .leaves_from = 0.3f, .trunk_upright = 1.0f,
		.fork_height = 0.4f, .extra_stem_chance = 0.02f, .gnarl = 0.03f, .kinks = 0.0f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.50f, .tips = 1.00f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.070f,
	},
	{
		// Brad: a cone, the branches level in tiers, the sprays flat
		.name = "fir", .shape = SHAPE_TREE, .atlas = "conifers", .height_meters = 15.0f, .stems = 1, .stem_lean_degrees = 1.0f,
		.levels = 2, .trunk_radius_ratio = 0.022f, .taper = 0.08f, .bend = 0.03f, .crown_start = 0.1f,
		.children = { 45, 12, 0 }, .length_ratio = { 0.3f, 0.3f, 0.0f }, .down_degrees = { 88.0f, 60.0f, 0.0f },
		.down_spread_degrees = 4.0f, .reach = { 0.02f, 0.0f, 0.0f }, .child_radius_ratio = 0.35f, .crown = CROWN_CONE,
		.child_start = 0.05f, .leaves_per_branch = 5, .leaf_size = 0.45f,
		.twig_scale = 1.8f, .side_twigs = 4.0f, .tip_twig = true, .twig_flat = 0.85f, .fruit_hang = FRUIT_UP, .fruit_zone = FRUIT_TOP,
		.droop = 0.02f, .tip_lift = 0.2f, .collar = 0.1f, .root_flare = 0.3f, .leaves_from = 0.25f, .trunk_upright = 1.0f,
		.fork_height = 0.4f, .extra_stem_chance = 0.02f, .gnarl = 0.03f, .kinks = 0.0f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.50f, .tips = 1.00f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.060f,
	},
	{
		// Brad of whole boughs: the fir's tiers, each bough out of the trunk
		// one picture of a whole bough seen from above (and one under it,
		// add_branch_card), its own atlas of big squares
		.name = "fir_squares", .shape = SHAPE_TREE, .atlas = "fir_branches", .height_meters = 15.0f, .stems = 1, .stem_lean_degrees = 1.0f,
		.levels = 1, .trunk_radius_ratio = 0.022f, .taper = 0.08f, .bend = 0.03f, .crown_start = 0.1f,
		.children = { 90, 0, 0 }, .length_ratio = { 0.24f, 0.0f, 0.0f }, .down_degrees = { 88.0f, 0.0f, 0.0f },
		.down_spread_degrees = 4.0f, .reach = { 0.02f, 0.0f, 0.0f }, .child_radius_ratio = 0.35f, .crown = CROWN_CONE,
		.child_start = 0.05f, .leaves_per_branch = 5, .leaf_size = 0.45f,
		.twig_scale = 1.1f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 1.0f, .branch_cards = true, .fruit_hang = FRUIT_UP, .fruit_zone = FRUIT_TOP,
		.droop = 0.04f, .tip_lift = 0.3f, .collar = 0.1f, .root_flare = 0.3f, .leaves_from = 0.25f, .trunk_upright = 1.0f,
		.fork_height = 0.4f, .extra_stem_chance = 0.02f, .gnarl = 0.03f, .kinks = 0.0f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.50f, .tips = 1.00f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.060f,
	},
	{
		// Pin: a long bare trunk, a high crown of few long branches reaching up
		.name = "pine", .shape = SHAPE_TREE, .atlas = "conifers", .height_meters = 18.0f, .stems = 1, .stem_lean_degrees = 5.0f,
		.levels = 2, .trunk_radius_ratio = 0.022f, .taper = 0.15f, .bend = 0.15f, .crown_start = 0.55f,
		.children = { 16, 9, 0 }, .length_ratio = { 0.32f, 0.45f, 0.0f }, .down_degrees = { 62.0f, 50.0f, 0.0f },
		.down_spread_degrees = 12.0f, .reach = { 0.1f, 0.08f, 0.0f }, .child_radius_ratio = 0.45f, .crown = CROWN_DOME,
		.child_start = 0.25f, .leaves_per_branch = 5, .leaf_size = 0.45f,
		.twig_scale = 1.6f, .side_twigs = 2.0f, .tip_twig = true, .twig_flat = 0.2f, .fruit_hang = FRUIT_OUT, .fruit_zone = FRUIT_OUTER,
		.droop = 0.01f, .tip_lift = 0.15f, .collar = 0.15f, .root_flare = 0.25f, .leaves_from = 0.0f, .trunk_upright = 0.5f,
		.fork_height = 0.5f, .extra_stem_chance = 0.05f, .gnarl = 0.25f, .kinks = 0.15f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.40f, .tips = 0.80f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.050f,
	},
	{
		// Pin silvestru (Scots pine, Pinus sylvestris): grown up in a stand,
		// a tall straight bare trunk with dead branches under the crown; the
		// crown in its top third, irregular, rounded to flat on top, of few
		// thick crooked branches reaching out and up, their needles in tufts
		// at their ends (the branches bare towards the trunk)
		.name = "scots_pine", .shape = SHAPE_TREE, .atlas = "conifers", .height_meters = 20.0f, .stems = 1, .stem_lean_degrees = 2.0f,
		.levels = 2, .trunk_radius_ratio = 0.026f, .taper = 0.2f, .trunk_taper = 0.12f, .bend = 0.1f, .crown_start = 0.6f,
		.children = { 11, 7, 0 }, .length_ratio = { 0.42f, 0.45f, 0.0f }, .down_degrees = { 72.0f, 45.0f, 0.0f },
		.down_spread_degrees = 18.0f, .reach = { 0.07f, 0.12f, 0.0f }, .child_radius_ratio = 0.5f, .crown = CROWN_DOME,
		.child_start = 0.4f, .leaves_per_branch = 5, .leaf_size = 0.5f,
		.twig_scale = 1.8f, .side_twigs = 0.7f, .tip_twig = true, .twig_flat = 0.15f, .fruit_hang = FRUIT_OUT, .fruit_zone = FRUIT_OUTER,
		.droop = 0.012f, .tip_lift = 0.35f, .collar = 0.25f, .root_flare = 0.3f, .leaves_from = 0.62f, .trunk_upright = 0.6f, .dead_branches = 24.0f,
		.fork_height = 0.75f, .extra_stem_chance = 0.04f, .gnarl = 0.3f, .kinks = 0.35f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.30f, .tips = 0.70f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.040f,
	},
	{
		// Tuia: a narrow column, many short branches pointing up
		.name = "thuja", .shape = SHAPE_TREE, .atlas = "conifers", .height_meters = 5.0f, .stems = 1, .stem_lean_degrees = 1.0f,
		.levels = 2, .trunk_radius_ratio = 0.02f, .taper = 0.1f, .bend = 0.08f, .crown_start = 0.04f,
		.children = { 60, 10, 0 }, .length_ratio = { 0.18f, 0.5f, 0.0f }, .down_degrees = { 35.0f, 30.0f, 0.0f },
		.down_spread_degrees = 8.0f, .reach = { 0.15f, 0.05f, 0.0f }, .child_radius_ratio = 0.4f, .crown = CROWN_COLUMN,
		.child_start = 0.05f, .leaves_per_branch = 5, .leaf_size = 0.3f,
		.twig_scale = 1.6f, .side_twigs = 5.0f, .tip_twig = true, .twig_flat = 0.0f, .fruit_hang = FRUIT_UP, .fruit_zone = FRUIT_TOP,
		.droop = 0.005f, .tip_lift = 0.0f, .collar = 0.05f, .root_flare = 0.1f, .leaves_from = 0.0f, .trunk_upright = 1.0f,
		.fork_height = 0.1f, .extra_stem_chance = 0.1f, .gnarl = 0.05f, .kinks = 0.0f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.20f, .tips = 0.60f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.030f,
	},
	{
		// Ienupar: a few stems out of the ground, a dense narrow shrub
		.name = "juniper", .shape = SHAPE_TREE, .atlas = "conifers", .height_meters = 3.0f, .stems = 5, .stem_lean_degrees = 12.0f,
		.levels = 2, .trunk_radius_ratio = 0.015f, .taper = 0.2f, .bend = 0.25f, .crown_start = 0.06f,
		.children = { 24, 8, 0 }, .length_ratio = { 0.25f, 0.45f, 0.0f }, .down_degrees = { 45.0f, 45.0f, 0.0f },
		.down_spread_degrees = 12.0f, .reach = { 0.12f, 0.05f, 0.0f }, .child_radius_ratio = 0.5f, .crown = CROWN_COLUMN,
		.child_start = 0.05f, .leaves_per_branch = 5, .leaf_size = 0.2f,
		.twig_scale = 1.5f, .side_twigs = 5.0f, .tip_twig = true, .twig_flat = 0.1f, .fruit_hang = FRUIT_OUT, .fruit_zone = FRUIT_OUTER,
		.droop = 0.01f, .tip_lift = 0.0f, .collar = 0.05f, .root_flare = 0.0f, .leaves_from = 0.0f,
		.fork_height = 0.0f, .extra_stem_chance = 0.3f, .gnarl = 0.3f, .kinks = 0.1f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.00f, .tips = 0.50f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.020f,
	},
	{
		// Taratoare (a creeping shrub: creeping juniper, cranberry, bearberry):
		// many stems out of one middle lying flat on the ground, each its
		// own way, long and wandering, hardly turning up; short side shoots
		// out of them along the ground, their tips lifting a little;
		// foliage all along: a mat a few meters across, ankle high.
		// height_meters is the stems' length (the mat's reach out)
		.name = "creeping", .shape = SHAPE_TREE, .atlas = "conifers", .height_meters = 2.2f, .stems = 11, .stem_lean_degrees = 84.0f,
		.sprawl = true,
		.levels = 2, .trunk_radius_ratio = 0.008f, .taper = 0.35f, .bend = 0.45f, .crown_start = 0.04f,
		.children = { 16, 6, 0 }, .length_ratio = { 0.32f, 0.4f, 0.0f }, .down_degrees = { 70.0f, 55.0f, 0.0f },
		.down_spread_degrees = 12.0f, .reach = { 0.0f, 0.0f, 0.0f }, .child_radius_ratio = 0.55f, .crown = CROWN_COLUMN,
		.child_start = 0.06f, .leaves_per_branch = 6, .leaf_size = 0.25f,
		.twig_scale = 1.1f, .side_twigs = 3.0f, .tip_twig = true, .twig_flat = 0.6f, .fruit_hang = FRUIT_UP, .fruit_zone = FRUIT_OUTER,
		.droop = 0.02f, .tip_lift = 0.25f, .collar = 0.0f, .root_flare = 0.0f, .leaves_from = 0.05f,
		.fork_height = 0.0f, .extra_stem_chance = 0.4f, .gnarl = 0.25f, .kinks = 0.15f, .lean_length = 0.92f, .straighten = 0.2f, .light_from_below = 1.5f,
		.moss = 0.00f, .tips = 0.50f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.010f,
	},
	{
		// Jneapan (mountain pine, Pinus mugo): many crooked stems out of
		// the ground, lying out low and turning up, few branches, the
		// needles in brushes at their ends: a low dome on the rocks
		.name = "mountain_pine", .shape = SHAPE_TREE, .atlas = "conifers", .height_meters = 1.6f, .stems = 8, .stem_lean_degrees = 66.0f, .sprawl = true,
		.levels = 2, .trunk_radius_ratio = 0.03f, .taper = 0.3f, .bend = 0.55f, .crown_start = 0.3f,
		.children = { 10, 4, 0 }, .length_ratio = { 0.34f, 0.45f, 0.0f }, .down_degrees = { 25.0f, 25.0f, 0.0f },
		.down_spread_degrees = 10.0f, .reach = { 0.25f, 0.15f, 0.0f }, .child_radius_ratio = 0.55f, .crown = CROWN_DOME,
		.child_start = 0.35f, .leaves_per_branch = 6, .leaf_size = 0.5f,
		.twig_scale = 1.5f, .side_twigs = 2.0f, .tip_twig = true, .twig_flat = 0.2f, .fruit_hang = FRUIT_OUT, .fruit_zone = FRUIT_OUTER,
		.droop = 0.01f, .tip_lift = 0.4f, .collar = 0.15f, .root_flare = 0.1f, .leaves_from = 0.4f,
		.fork_height = 0.0f, .extra_stem_chance = 0.3f, .gnarl = 0.2f, .kinks = 0.1f, .lean_length = 0.3f, .straighten = 0.9f,
		.moss = 0.20f, .tips = 0.60f, .autumn_colors = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, .autumn = 0.0f, .falls = false,
		.snow_sag = 0.020f,
	},
	// Fruit trees of the plum's family (Prunus)
	{
		// Cires: tall, a leader up the middle, the branches in tiers, reaching up
		.name = "cherry", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 9.0f, .stems = 1, .stem_lean_degrees = 3.0f,
		.levels = 3, .trunk_radius_ratio = 0.028f, .taper = 0.35f, .bend = 0.14f, .crown_start = 0.3f,
		.children = { 8, 6, 6 }, .length_ratio = { 0.5f, 0.5f, 0.4f }, .down_degrees = { 42.0f, 45.0f, 45.0f },
		.down_spread_degrees = 10.0f, .reach = { 0.05f, 0.02f, 0.0f }, .child_radius_ratio = 0.6f, .crown = CROWN_ROUND,
		.child_start = 0.15f, .leaves_per_branch = 15, .leaf_size = 0.11f,
		.twig_scale = 1.2f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 0.0f, .fruit_hang = FRUIT_DOWN, .fruit_zone = FRUIT_OUTER,
		.droop = 0.025f, .tip_lift = 0.0f, .collar = 0.5f, .root_flare = 0.35f, .leaves_from = 0.0f,
		.fork_height = 0.3f, .extra_stem_chance = 0.1f, .gnarl = 0.3f, .kinks = 0.35f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.40f, .tips = 0.25f, .autumn_colors = { { 0.74f, 0.42f, 0.18f }, { 0.86f, 0.66f, 0.26f } }, .autumn = 1.0f, .falls = true,
		.snow_sag = 0.030f,
	},
	{
		// Prun, corcodus: lower, round and dense, thin tangled branches, often two or three stems
		.name = "plum", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 5.5f, .stems = 2, .stem_lean_degrees = 14.0f,
		.levels = 3, .trunk_radius_ratio = 0.03f, .taper = 0.3f, .bend = 0.3f, .crown_start = 0.25f,
		.children = { 7, 7, 6 }, .length_ratio = { 0.6f, 0.5f, 0.45f }, .down_degrees = { 48.0f, 50.0f, 45.0f },
		.down_spread_degrees = 18.0f, .reach = { 0.04f, 0.02f, 0.0f }, .child_radius_ratio = 0.6f, .crown = CROWN_ROUND,
		.child_start = 0.12f, .leaves_per_branch = 16, .leaf_size = 0.08f,
		.twig_scale = 1.2f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 0.0f, .fruit_hang = FRUIT_DOWN, .fruit_zone = FRUIT_OUTER,
		.droop = 0.04f, .tip_lift = 0.0f, .collar = 0.5f, .root_flare = 0.3f, .leaves_from = 0.0f,
		.fork_height = 0.15f, .extra_stem_chance = 0.3f, .gnarl = 0.4f, .kinks = 0.5f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.50f, .tips = 0.25f, .autumn_colors = { { 0.62f, 0.44f, 0.20f }, { 0.80f, 0.68f, 0.28f } }, .autumn = 1.0f, .falls = true,
		.snow_sag = 0.030f,
	},
	{
		// Visin: small and dense, its thin branches hanging
		.name = "sour_cherry", .shape = SHAPE_TREE, .atlas = "broadleaved", .height_meters = 4.0f, .stems = 1, .stem_lean_degrees = 6.0f,
		.levels = 3, .trunk_radius_ratio = 0.03f, .taper = 0.3f, .bend = 0.25f, .crown_start = 0.25f,
		.children = { 8, 7, 7 }, .length_ratio = { 0.6f, 0.55f, 0.5f }, .down_degrees = { 55.0f, 50.0f, 45.0f },
		.down_spread_degrees = 15.0f, .reach = { 0.0f, -0.08f, -0.25f }, .child_radius_ratio = 0.55f, .crown = CROWN_ROUND,
		.child_start = 0.12f, .leaves_per_branch = 15, .leaf_size = 0.08f,
		.twig_scale = 1.2f, .side_twigs = 0.0f, .tip_twig = false, .twig_flat = 0.0f, .fruit_hang = FRUIT_DOWN, .fruit_zone = FRUIT_OUTER,
		.droop = 0.08f, .tip_lift = 0.0f, .collar = 0.4f, .root_flare = 0.3f, .leaves_from = 0.0f,
		.fork_height = 0.2f, .extra_stem_chance = 0.2f, .gnarl = 0.35f, .kinks = 0.45f, .lean_length = 0.3f, .straighten = 0.8f,
		.moss = 0.40f, .tips = 0.25f, .autumn_colors = { { 0.72f, 0.38f, 0.16f }, { 0.84f, 0.62f, 0.22f } }, .autumn = 1.0f, .falls = true,
		.snow_sag = 0.040f,
	},
	// Small plants
	{ .name = "tuft", .shape = SHAPE_TUFT, .atlas = "small_plants", .height_meters = 0.35f, .leaf_size = 0.35f, .parts = 7,
		.tips = 0.2f, .autumn_colors = { { 0.70f, 0.60f, 0.30f }, { 0.58f, 0.48f, 0.24f } }, .autumn = 0.7f },
	{ .name = "rosette", .shape = SHAPE_ROSETTE, .atlas = "small_plants", .height_meters = 0.3f, .leaf_size = 0.14f, .parts = 9 },
	{ .name = "fern", .shape = SHAPE_FERN, .atlas = "small_plants", .height_meters = 0.8f, .leaf_size = 0.75f, .parts = 11,
		.tips = 0.2f, .autumn_colors = { { 0.62f, 0.38f, 0.14f }, { 0.50f, 0.30f, 0.12f } }, .autumn = 0.9f },
	{ .name = "flower", .shape = SHAPE_FLOWER, .atlas = "small_plants", .height_meters = 0.45f, .leaf_size = 0.07f, .parts = 3 },
	// Mushrooms: a stem and a cap, as the recipe shapes them (see grow_mushroom)
	{ .name = "mushroom", .shape = SHAPE_MUSHROOM, .atlas = "small_plants", .height_meters = 0.15f, .leaf_size = 0.15f, .parts = 1 },
};

// ---------------------------------------------------------------------------
// Small helpers

typedef struct
{
	float x, y, z;
} vec3;

static vec3 v3(float x, float y, float z)
{
	vec3 v = { x, y, z };
	return v;
}

static vec3 add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 cross(vec3 a, vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

static vec3 normalized(vec3 a)
{
	float length = sqrtf(dot(a, a));

	return length > 1e-8f ? scale(a, 1.0f / length) : v3(0.0f, 1.0f, 0.0f);
}

// Any unit vector square to a
static vec3 perpendicular(vec3 a)
{
	return normalized(fabsf(a.y) < 0.9f ? cross(a, v3(0.0f, 1.0f, 0.0f)) : cross(a, v3(1.0f, 0.0f, 0.0f)));
}

// a turned by angle (radians) about the unit axis
static vec3 rotated(vec3 a, vec3 axis, float angle)
{
	float c = cosf(angle);
	float s = sinf(angle);

	return add(add(scale(a, c), scale(cross(axis, a), s)), scale(axis, dot(axis, a) * (1.0f - c)));
}

static uint32_t random_state = 1u;

static float random_unit(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return (float)(random_state >> 8) * (1.0f / 16777216.0f);
}

static float random_between(float low, float high)
{
	return low + (high - low) * random_unit();
}

static int random_index(int count)
{
	int index = (int)(random_unit() * (float)count);

	return index < count ? index : count - 1;
}

static vec3 random_direction(void)
{
	float z = random_between(-1.0f, 1.0f);
	float angle = random_between(0.0f, 2.0f * PI_F);
	float r = sqrtf(fmaxf(1.0f - z * z, 0.0f));

	return v3(r * cosf(angle), z, r * sinf(angle));
}

static void *checked_realloc(void *pointer, size_t size)
{
	pointer = realloc(pointer, size);
	if (pointer == NULL && size > 0)
	{
		plant_error("Out of memory\n");
		exit(1);
	}
	return pointer;
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
	FILE *file = fopen(path, "rb");
	uint8_t *data;
	long size;

	if (file == NULL)
	{
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = checked_realloc(NULL, (size_t)size + 1);
	if (size < 0 || fread(data, 1, (size_t)size, file) != (size_t)size)
	{
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	data[size] = 0;
	*size_out = (size_t)size;
	return data;
}

// An image as RGBA8 (NULL, after saying why)
static uint8_t *load_image(const char *path, int *width_out, int *height_out)
{
	size_t size = 0;
	uint8_t *bytes = read_file(path, &size);
	int channels = 0;
	uint8_t *pixels;

	if (bytes == NULL)
	{
		plant_error("Cannot read '%s'\n", path);
		return NULL;
	}
	pixels = stbi_load_from_memory(bytes, (int)size, width_out, height_out, &channels, 4);
	free(bytes);
	if (pixels == NULL)
	{
		plant_error("Cannot decode '%s' (PNG and JPEG are read): %s\n", path, stbi_failure_reason());
	}
	return pixels;
}

// What of a picture is there (alpha above a trace): its box, x, y, width,
// height (the whole picture if nothing is), and where its stem is across
// that box (0..1 from the left): the middle of what is there in the
// bottom few rows. Pictures come with empty borders, and a stem off the
// middle; cut to the box, nothing of the atlas goes to the borders, and the
// card is put down with its stem (not its middle) on the branch
static void picture_box(const uint8_t *picture, int width, int height, int box_out[4], float *anchor_out)
{
	const uint8_t threshold = 8;
	int left = width;
	int right = -1;
	int top = height;
	int bottom = -1;
	int bottom_rows;
	double stem_sum = 0.0;
	double stem_weight = 0.0;

	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			if (picture[((size_t)y * (size_t)width + (size_t)x) * 4 + 3] > threshold)
			{
				left = x < left ? x : left;
				right = x > right ? x : right;
				top = y < top ? y : top;
				bottom = y > bottom ? y : bottom;
			}
		}
	}
	if (right < 0)
	{
		box_out[0] = 0;
		box_out[1] = 0;
		box_out[2] = width;
		box_out[3] = height;
		*anchor_out = 0.5f;
		return;
	}
	box_out[0] = left;
	box_out[1] = top;
	box_out[2] = right - left + 1;
	box_out[3] = bottom - top + 1;
	// The stem: the bottom 3% of what is there, weighed by alpha
	bottom_rows = box_out[3] * 3 / 100 > 2 ? box_out[3] * 3 / 100 : 2;
	for (int y = bottom - bottom_rows + 1; y <= bottom; y++)
	{
		for (int x = left; x <= right; x++)
		{
			float alpha = (float)picture[((size_t)y * (size_t)width + (size_t)x) * 4 + 3];

			stem_sum += ((double)(x - left) + 0.5) * alpha;
			stem_weight += alpha;
		}
	}
	*anchor_out = stem_weight > 0.0 ? (float)(stem_sum / stem_weight / (double)box_out[2]) : 0.5f;
}

// A box of a picture as a picture of its own (freed with free)
static uint8_t *picture_crop(const uint8_t *picture, int width, const int box[4])
{
	uint8_t *cropped = checked_realloc(NULL, (size_t)box[2] * (size_t)box[3] * 4);

	for (int y = 0; y < box[3]; y++)
	{
		memcpy(&cropped[(size_t)y * (size_t)box[2] * 4], &picture[((size_t)(box[1] + y) * (size_t)width + (size_t)box[0]) * 4],
			(size_t)box[2] * 4);
	}
	return cropped;
}

// ---------------------------------------------------------------------------
// The atlases

typedef enum
{
	CELL_SQUARE,
	CELL_TALL,
	CELL_SMALL                          // small squares: little flowers' heads and leaves
} cell_shape_t;

#define CELL_SHAPES 3

static const char *cell_shape_name(cell_shape_t shape)
{
	return shape == CELL_SQUARE ? "square" : (shape == CELL_TALL ? "tall" : "small");
}

#define MAX_CELLS 1024

typedef struct
{
	char color[PATH_LENGTH];            // the source picture, as given
	char months[12][PATH_LENGTH];       // a month's own, chosen by hand (empty: found by name)
} cell_sources_t;

typedef struct
{
	char name[64];
	char ini_path[PATH_LENGTH];
	char data_directory[PATH_LENGTH];
	int size;                           // pixels a side
	int square;                         // square cells: pixels a side
	int tall_width;                     // tall cells
	int tall_height;
	int tall_rows;                      // rows of tall cells at the bottom
	int small;                          // small square cells: pixels a side
	int small_rows;                     // rows of them, above the tall ones
	char file_name[PATH_LENGTH];        // relative to data/ (engine/plant_atlas_file.h)
	bool used[CELL_SHAPES][MAX_CELLS];
	cell_sources_t cells[CELL_SHAPES][MAX_CELLS];
	// Where each cell's picture went (pixels in the atlas): x, y, width, height
	int placed[CELL_SHAPES][MAX_CELLS][4];
	// Where its stem is across it (0..1 of its width, from the left)
	float anchor[CELL_SHAPES][MAX_CELLS];
	// Its pixels, once built (atlas_build): RGBA8, size x size
	uint8_t *pixels;
	// The cell (of the same shape) each cell shows each month (January
	// first): its months' own pictures, or itself
	int month_cells[CELL_SHAPES][MAX_CELLS][12];
} atlas_t;

static int cell_count(const atlas_t *atlas, cell_shape_t shape)
{
	int tall_band = atlas->tall_rows * atlas->tall_height;
	int small_band = atlas->small_rows * atlas->small;
	int count;

	if (shape == CELL_SQUARE)
	{
		count = (atlas->size / atlas->square) * ((atlas->size - tall_band - small_band) / atlas->square);
	}
	else if (shape == CELL_TALL)
	{
		count = (atlas->size / atlas->tall_width) * atlas->tall_rows;
	}
	else
	{
		count = atlas->small > 0 ? (atlas->size / atlas->small) * atlas->small_rows : 0;
	}
	return count < MAX_CELLS ? count : MAX_CELLS;
}

// A cell's rectangle in the atlas: x, y, width, height. From the top: the
// squares, the small ones, the tall ones
static void cell_rectangle(const atlas_t *atlas, cell_shape_t shape, int index, int out[4])
{
	if (shape == CELL_SQUARE)
	{
		int across = atlas->size / atlas->square;

		out[0] = (index % across) * atlas->square;
		out[1] = (index / across) * atlas->square;
		out[2] = atlas->square;
		out[3] = atlas->square;
	}
	else if (shape == CELL_TALL)
	{
		int across = atlas->size / atlas->tall_width;

		out[0] = (index % across) * atlas->tall_width;
		out[1] = atlas->size - atlas->tall_rows * atlas->tall_height + (index / across) * atlas->tall_height;
		out[2] = atlas->tall_width;
		out[3] = atlas->tall_height;
	}
	else
	{
		int across = atlas->size / atlas->small;

		out[0] = (index % across) * atlas->small;
		out[1] = atlas->size - atlas->tall_rows * atlas->tall_height - atlas->small_rows * atlas->small + (index / across) * atlas->small;
		out[2] = atlas->small;
		out[3] = atlas->small;
	}
}

static char *trimmed(char *text)
{
	char *end;

	while (*text == ' ' || *text == '\t')
	{
		text++;
	}
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
	{
		*--end = '\0';
	}
	return text;
}

static bool file_exists(const char *path)
{
	FILE *file = fopen(path, "rb");

	if (file == NULL)
	{
		return false;
	}
	fclose(file);
	return true;
}

// A month (1..12) written as at the end of a picture's name: "_m3" (0 if
// it is not one)
static int month_of_suffix(const char *suffix)
{
	int month;

	if (suffix[0] != '_' || suffix[1] != 'm' || suffix[2] < '0' || suffix[2] > '9')
	{
		return 0;
	}
	if (suffix[3] != '\0' && (suffix[3] < '0' || suffix[3] > '9' || suffix[4] != '\0'))
	{
		return 0;
	}
	month = atoi(suffix + 2);
	return month >= 1 && month <= 12 ? month : 0;
}

// A picture's name without its extension and its month (twig_m3.png:
// twig), and its extension (.png)
static void picture_stem(const char *path, char *stem_out, size_t stem_size, char *extension_out, size_t extension_size)
{
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');
	size_t length;
	char *underscore;

	if (dot == NULL || (slash != NULL && dot < slash))
	{
		dot = path + strlen(path);
	}
	length = (size_t)(dot - path);
	length = length < stem_size ? length : stem_size - 1;
	memcpy(stem_out, path, length);
	stem_out[length] = '\0';
	snprintf(extension_out, extension_size, "%s", dot);
	underscore = strrchr(stem_out, '_');
	if (underscore != NULL && month_of_suffix(underscore) > 0)
	{
		*underscore = '\0';
	}
}

// What a picture shows each month (January first): a month's own (chosen
// by hand, or found beside it by name); a month without: the picture
// itself if it is a plain one (twig.png: what the months without their own
// show), else (twig_m3.png) the latest one before it round the year
static void resolve_months(const char *color, const char chosen[12][PATH_LENGTH], char sources_out[12][PATH_LENGTH], bool own_out[12])
{
	char stem[PATH_LENGTH];
	char extension[32];
	int own_count = 0;
	bool plain;

	picture_stem(color, stem, sizeof(stem), extension, sizeof(extension));
	// (no month at the end of its name)
	plain = strlen(color) == strlen(stem) + strlen(extension);
	for (int m = 0; m < 12; m++)
	{
		char candidate[PATH_LENGTH + 48];

		own_out[m] = false;
		sources_out[m][0] = '\0';
		if (chosen != NULL && chosen[m][0] != '\0')
		{
			snprintf(sources_out[m], PATH_LENGTH, "%s", chosen[m]);
			own_out[m] = true;
		}
		else
		{
			snprintf(candidate, sizeof(candidate), "%s_m%d%s", stem, m + 1, extension);
			if (file_exists(candidate))
			{
				snprintf(sources_out[m], PATH_LENGTH, "%s", candidate);
				own_out[m] = true;
			}
		}
		own_count += own_out[m] ? 1 : 0;
	}
	for (int m = 0; m < 12; m++)
	{
		if (own_out[m])
		{
			continue;
		}
		if (own_count == 0 || plain)
		{
			snprintf(sources_out[m], PATH_LENGTH, "%s", color);
			continue;
		}
		for (int back = 1; back < 12; back++)
		{
			int before = (m - back + 12) % 12;

			if (own_out[before])
			{
				snprintf(sources_out[m], PATH_LENGTH, "%s", sources_out[before]);
				break;
			}
		}
	}
}

// The .ini (made with the defaults if there is none)
static bool atlas_open(atlas_t *atlas, const char *name, const char *data_directory)
{
	size_t size = 0;
	char *text;
	bool small_given = false;

	memset(atlas, 0, sizeof(*atlas));
	snprintf(atlas->name, sizeof(atlas->name), "%s", name);
	snprintf(atlas->data_directory, sizeof(atlas->data_directory), "%s", data_directory);
	snprintf(atlas->ini_path, sizeof(atlas->ini_path), "%s/textures/%s.ini", data_directory, name);
	atlas->size = 4096;
	atlas->square = strcmp(name, "small_plants") == 0 ? 256 : 512;
	atlas->tall_width = atlas->square / 2;
	atlas->tall_height = atlas->square;
	atlas->tall_rows = strcmp(name, "small_plants") == 0 ? 2 : 1;
	// The small plants' little ones (flowers' heads, mushrooms) in small
	// squares of their own
	atlas->small = 128;
	atlas->small_rows = strcmp(name, "small_plants") == 0 ? 4 : 0;
	// The conifers' sprays are all long: tall cells only
	if (strcmp(name, "conifers") == 0)
	{
		atlas->tall_rows = atlas->size / atlas->tall_height;
	}
	// Whole boughs (fir_squares): a few big squares
	if (strcmp(name, "fir_branches") == 0)
	{
		atlas->size = 2048;
		atlas->square = 1024;
		atlas->tall_width = 512;
		atlas->tall_height = 1024;
		atlas->tall_rows = 0;
	}
	snprintf(atlas->file_name, sizeof(atlas->file_name), "textures/%s.dds", name);
	text = (char *)read_file(atlas->ini_path, &size);
	if (text == NULL)
	{
		return true;
	}
	for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n"))
	{
		char *comment = strchr(line, ';');
		char *equals;
		char *key;
		char *value;
		int index;

		if (comment != NULL)
		{
			*comment = '\0';
		}
		equals = strchr(line, '=');
		if (equals == NULL)
		{
			continue;
		}
		*equals = '\0';
		key = trimmed(line);
		value = trimmed(equals + 1);
		if (strcmp(key, "size") == 0)
		{
			atlas->size = atoi(value);
		}
		else if (strcmp(key, "square") == 0)
		{
			atlas->square = atoi(value);
		}
		else if (strcmp(key, "tall") == 0)
		{
			sscanf(value, "%dx%d", &atlas->tall_width, &atlas->tall_height);
		}
		else if (strcmp(key, "tall_rows") == 0)
		{
			atlas->tall_rows = atoi(value);
		}
		else if (strcmp(key, "small") == 0)
		{
			atlas->small = atoi(value);
		}
		else if (strcmp(key, "small_rows") == 0)
		{
			atlas->small_rows = atoi(value);
			small_given = true;
		}
		else if (strcmp(key, "file") == 0)
		{
			snprintf(atlas->file_name, sizeof(atlas->file_name), "%s", value);
		}
		else if ((sscanf(key, "square%d", &index) == 1 || sscanf(key, "tall%d", &index) == 1 || sscanf(key, "small%d", &index) == 1)
			&& index >= 0 && index < MAX_CELLS && value[0] != '\0')
		{
			// Its picture, then | m<month> = <picture> for each chosen by hand
			// (anything else after a | is left out: the maps of before)
			cell_shape_t shape = strncmp(key, "square", 6) == 0 ? CELL_SQUARE : (strncmp(key, "tall", 4) == 0 ? CELL_TALL : CELL_SMALL);
			cell_sources_t *cell = &atlas->cells[shape][index];
			char *part = value;
			bool first = true;

			memset(cell, 0, sizeof(*cell));
			while (part != NULL)
			{
				char *bar = strchr(part, '|');
				char *field;

				if (bar != NULL)
				{
					*bar = '\0';
				}
				field = trimmed(part);
				if (first)
				{
					snprintf(cell->color, sizeof(cell->color), "%s", field);
				}
				else
				{
					int month = 0;
					char *month_equals = strchr(field, '=');

					if (month_equals != NULL && field[0] == 'm' && sscanf(field + 1, "%d", &month) == 1 && month >= 1 && month <= 12)
					{
						snprintf(cell->months[month - 1], PATH_LENGTH, "%s", trimmed(month_equals + 1));
					}
				}
				first = false;
				part = bar != NULL ? bar + 1 : NULL;
			}
			atlas->used[shape][index] = cell->color[0] != '\0';
		}
	}
	free(text);
	// An atlas made before there were small cells: none (its squares stay
	// where they are)
	if (!small_given)
	{
		atlas->small_rows = 0;
	}
	if (atlas->size < 64 || atlas->square < 16 || atlas->tall_width < 8 || atlas->tall_height < 16 || atlas->tall_rows < 0
		|| atlas->small_rows < 0 || (atlas->small_rows > 0 && atlas->small < 8)
		|| atlas->tall_rows * atlas->tall_height + atlas->small_rows * atlas->small > atlas->size)
	{
		plant_error("'%s': the size and the cells do not fit together\n", atlas->ini_path);
		return false;
	}
	return true;
}

static bool atlas_save(const atlas_t *atlas)
{
	FILE *file = fopen(atlas->ini_path, "w");

	if (file == NULL)
	{
		plant_error("Cannot write '%s'\n", atlas->ini_path);
		return false;
	}
	fprintf(file, "; The %s atlas, kept by tools/potop_plant: what is in which cell.\n", atlas->name);
	fprintf(file, "; A cell: its picture, then | m<month> = <picture> for a month's own chosen\n");
	fprintf(file, "; by hand (those named <picture>_m1 .. _m12 beside it are found anyway).\n");
	fprintf(file, "; Take a line out to free its cell; the atlas is made again at the next run.\n");
	fprintf(file, "; Changing the size or the cells moves everything: the plants made before\n");
	fprintf(file, "; then show the wrong pictures until they are made again.\n\n");
	fprintf(file, "size = %d\nsquare = %d\ntall = %dx%d\ntall_rows = %d\n", atlas->size, atlas->square, atlas->tall_width,
		atlas->tall_height, atlas->tall_rows);
	fprintf(file, "small = %d\nsmall_rows = %d\n", atlas->small, atlas->small_rows);
	fprintf(file, "file = %s\n\n", atlas->file_name);
	for (int shape = 0; shape < CELL_SHAPES; shape++)
	{
		for (int i = 0; i < cell_count(atlas, (cell_shape_t)shape); i++)
		{
			const cell_sources_t *cell = &atlas->cells[shape][i];

			if (!atlas->used[shape][i])
			{
				continue;
			}
			fprintf(file, "%s%d = %s", cell_shape_name((cell_shape_t)shape), i, cell->color);
			for (int m = 0; m < 12; m++)
			{
				if (cell->months[m][0] != '\0')
				{
					fprintf(file, " | m%d = %s", m + 1, cell->months[m]);
				}
			}
			fprintf(file, "\n");
		}
	}
	fclose(file);
	return true;
}

// The cell holding this picture, or a free one of the shape it fits (put
// there; forced_shape: that shape, -1 the one it fits; small: a small one
// rather than a square, if there are any); -1 if the atlas is full. shape_out: the cell's shape. Its months chosen by hand: as the
// picture has them now
static int atlas_place(atlas_t *atlas, const plant_picture_t *picture, int forced_shape, bool small, cell_shape_t *shape_out)
{
	int width = 0;
	int height = 0;
	int box[4];
	float anchor;
	uint8_t *pixels;
	cell_shape_t shape;

	for (int s = 0; s < CELL_SHAPES; s++)
	{
		for (int i = 0; i < MAX_CELLS; i++)
		{
			if (atlas->used[s][i] && strcmp(atlas->cells[s][i].color, picture->color) == 0)
			{
				memcpy(atlas->cells[s][i].months, picture->months, sizeof(picture->months));
				*shape_out = (cell_shape_t)s;
				return i;
			}
		}
	}
	pixels = load_image(picture->color, &width, &height);
	if (pixels == NULL)
	{
		plant_error("Cannot read the picture '%s'\n", picture->color);
		return -1;
	}
	// Its shape as it goes in: what is there of it
	picture_box(pixels, width, height, box, &anchor);
	stbi_image_free(pixels);
	// Long ones (half again as tall as wide) in the tall cells, if there are
	// any; all in the tall ones if there are no square ones (the conifers')
	shape = box[3] >= box[2] * 3 / 2 && atlas->tall_rows > 0 ? CELL_TALL : CELL_SQUARE;
	shape = shape == CELL_SQUARE && cell_count(atlas, CELL_SQUARE) == 0 ? CELL_TALL : shape;
	shape = shape == CELL_SQUARE && small && cell_count(atlas, CELL_SMALL) > 0 ? CELL_SMALL : shape;
	shape = forced_shape >= 0 ? (cell_shape_t)forced_shape : shape;
	for (int i = 0; i < cell_count(atlas, shape); i++)
	{
		if (!atlas->used[shape][i])
		{
			cell_sources_t *cell = &atlas->cells[shape][i];

			atlas->used[shape][i] = true;
			snprintf(cell->color, sizeof(cell->color), "%s", picture->color);
			memcpy(cell->months, picture->months, sizeof(picture->months));
			printf("'%s' goes in the %s atlas, %s cell %d\n", picture->color, atlas->name, cell_shape_name(shape), i);
			*shape_out = shape;
			return i;
		}
	}
	plant_error("The %s atlas has no free %s cell for '%s' (see %s)\n", atlas->name, cell_shape_name(shape), picture->color,
		atlas->ini_path);
	return -1;
}

// Alpha below this in a picture is taken for none (made pictures come with a
// faint dark haze round what is in them; on the card it would darken the
// cut out edges)
static const int PICTURE_ALPHA_NOISE = 16;

// A picture into a rectangle of the atlas: scaled to fit inside it (kept in
// shape, a margin round it), its bottom middle at the bottom middle (where
// the stem is), averaged down (each atlas texel the mean of the picture's
// texels it covers). Where none of them is there (alpha 0): empty (black,
// bled into later). placed_out: where it went
static void blit_scaled(uint8_t *atlas_pixels, int atlas_size, const int cell[4], const uint8_t *picture, int width, int height,
	int placed_out[4])
{
	int margin = cell[2] / 32 > 2 ? cell[2] / 32 : 2;
	float fit = fminf((float)(cell[2] - 2 * margin) / (float)width, (float)(cell[3] - 2 * margin) / (float)height);
	int out_width = (int)((float)width * fit);
	int out_height = (int)((float)height * fit);
	int x0 = cell[0] + (cell[2] - out_width) / 2;
	int y0 = cell[1] + cell[3] - margin - out_height;

	out_width = out_width < 1 ? 1 : out_width;
	out_height = out_height < 1 ? 1 : out_height;
	for (int y = 0; y < out_height; y++)
	{
		int source_y0 = (int)((float)y / fit);
		int source_y1 = (int)((float)(y + 1) / fit);

		source_y1 = source_y1 > source_y0 ? source_y1 : source_y0 + 1;
		source_y1 = source_y1 < height ? source_y1 : height;
		for (int x = 0; x < out_width; x++)
		{
			int source_x0 = (int)((float)x / fit);
			int source_x1 = (int)((float)(x + 1) / fit);
			float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			float weight = 0.0f;
			uint8_t *texel = &atlas_pixels[((size_t)(y0 + y) * (size_t)atlas_size + (size_t)(x0 + x)) * 4];

			source_x1 = source_x1 > source_x0 ? source_x1 : source_x0 + 1;
			source_x1 = source_x1 < width ? source_x1 : width;
			for (int sy = source_y0; sy < source_y1; sy++)
			{
				for (int sx = source_x0; sx < source_x1; sx++)
				{
					const uint8_t *from = &picture[((size_t)sy * (size_t)width + (size_t)sx) * 4];
					// (a faint haze round what is there, as made pictures
					// have it: nothing; the bleeding fills it from the edge)
					float alpha = from[3] >= PICTURE_ALPHA_NOISE ? (float)from[3] / 255.0f : 0.0f;

					// Color weighed by alpha, so the edge does not take the
					// color of what is cut away
					for (int k = 0; k < 3; k++)
					{
						sum[k] += (float)from[k] * alpha;
					}
					sum[3] += alpha * 255.0f;
					weight += alpha;
				}
			}
			{
				float count = (float)((source_x1 - source_x0) * (source_y1 - source_y0));

				for (int k = 0; k < 3; k++)
				{
					texel[k] = weight > 0.0f ? (uint8_t)(sum[k] / weight + 0.5f) : 0;
				}
				texel[3] = (uint8_t)(sum[3] / count + 0.5f);
			}
		}
	}
	placed_out[0] = x0;
	placed_out[1] = y0;
	placed_out[2] = out_width;
	placed_out[3] = out_height;
}

// Transparent texels of a cell take the color of their opaque neighbors,
// spreading out a few texels, so the mipmaps do not darken the edges
static void bleed_cell(uint8_t *pixels, int atlas_size, const int cell[4])
{
	for (int pass = 0; pass < 8; pass++)
	{
		for (int y = cell[1]; y < cell[1] + cell[3]; y++)
		{
			for (int x = cell[0]; x < cell[0] + cell[2]; x++)
			{
				uint8_t *texel = &pixels[((size_t)y * (size_t)atlas_size + (size_t)x) * 4];
				int sum[3] = { 0, 0, 0 };
				int count = 0;

				if (texel[3] > 0 || (texel[0] | texel[1] | texel[2]) != 0)
				{
					continue;
				}
				for (int dy = -1; dy <= 1; dy++)
				{
					for (int dx = -1; dx <= 1; dx++)
					{
						int nx = x + dx;
						int ny = y + dy;
						const uint8_t *neighbor;

						if (nx < cell[0] || ny < cell[1] || nx >= cell[0] + cell[2] || ny >= cell[1] + cell[3])
						{
							continue;
						}
						neighbor = &pixels[((size_t)ny * (size_t)atlas_size + (size_t)nx) * 4];
						if ((neighbor[0] | neighbor[1] | neighbor[2]) != 0)
						{
							sum[0] += neighbor[0];
							sum[1] += neighbor[1];
							sum[2] += neighbor[2];
							count++;
						}
					}
				}
				if (count > 0)
				{
					// 1 instead of 0, so this pass's texels are not taken for
					// empty by the next texel (they spread next pass)
					texel[0] = (uint8_t)(sum[0] / count > 0 ? sum[0] / count : 1);
					texel[1] = (uint8_t)(sum[1] / count > 0 ? sum[1] / count : 1);
					texel[2] = (uint8_t)(sum[2] / count > 0 ? sum[2] / count : 1);
				}
			}
		}
	}
}

// Where a picture's stem is across a box of it (0..1 from the box's left):
// the middle of what is there in the box's bottom few rows
static float stem_anchor(const uint8_t *picture, int width, const int box[4])
{
	int bottom_rows = box[3] * 3 / 100 > 2 ? box[3] * 3 / 100 : 2;
	double stem_sum = 0.0;
	double stem_weight = 0.0;

	for (int y = box[1] + box[3] - bottom_rows; y < box[1] + box[3]; y++)
	{
		for (int x = box[0]; x < box[0] + box[2]; x++)
		{
			float alpha = (float)picture[((size_t)y * (size_t)width + (size_t)x) * 4 + 3];

			stem_sum += ((double)(x - box[0]) + 0.5) * alpha;
			stem_weight += alpha;
		}
	}
	return stem_weight > 0.0 ? (float)(stem_sum / stem_weight / (double)box[2]) : 0.5f;
}

// The cell a picture is in (by its color's path); -1 if none
static int atlas_find(const atlas_t *atlas, const char *color, cell_shape_t *shape_out)
{
	for (int s = 0; s < CELL_SHAPES; s++)
	{
		for (int i = 0; i < MAX_CELLS; i++)
		{
			if (atlas->used[s][i] && strcmp(atlas->cells[s][i].color, color) == 0)
			{
				*shape_out = (cell_shape_t)s;
				return i;
			}
		}
	}
	return -1;
}

// The atlas, made again from the cells' pictures. A cell's months are
// pictures in cells of their own (placed with it, atlas_place_months); a
// picture and those it shows in its months (all of one size) are cut to
// the box of what is there in any of them, so that they lie on one
// another exactly on the card; month_cells: each cell's cell for each month
static bool atlas_build(atlas_t *atlas)
{
	size_t texels = (size_t)atlas->size * (size_t)atlas->size;
	typedef struct
	{
		char sources[12][PATH_LENGTH];
		bool own[12];
		int box[4];                     // what is there of its own picture
		int width;
		int height;
		int cut[4];                     // the box it is cut to: its own and its months'
	} cell_build_t;
	cell_build_t *build[CELL_SHAPES] = { NULL, NULL, NULL };
	bool ok = true;

	atlas->pixels = calloc(texels, 4);
	if (atlas->pixels == NULL)
	{
		plant_error("Out of memory for the %s atlas\n", atlas->name);
		return false;
	}
	// Each cell's picture: its size and its box, and its months
	for (int shape = 0; shape < CELL_SHAPES && ok; shape++)
	{
		build[shape] = checked_realloc(NULL, sizeof(cell_build_t) * (size_t)(cell_count(atlas, (cell_shape_t)shape) + 1));
		for (int i = 0; i < cell_count(atlas, (cell_shape_t)shape) && ok; i++)
		{
			cell_build_t *cell = &build[shape][i];
			uint8_t *picture;
			float unused_anchor;

			if (!atlas->used[shape][i])
			{
				continue;
			}
			resolve_months(atlas->cells[shape][i].color, (const char (*)[PATH_LENGTH])atlas->cells[shape][i].months, cell->sources, cell->own);
			picture = load_image(atlas->cells[shape][i].color, &cell->width, &cell->height);
			if (picture == NULL)
			{
				ok = false;
				break;
			}
			picture_box(picture, cell->width, cell->height, cell->box, &unused_anchor);
			memcpy(cell->cut, cell->box, sizeof(cell->box));
			stbi_image_free(picture);
		}
	}
	if (!ok)
	{
		for (int shape = 0; shape < CELL_SHAPES; shape++)
		{
			free(build[shape]);
		}
		return false;
	}
	// Each cell's months: the cells that show them, and the box round all
	// of them, which each of them is cut to
	for (int shape = 0; shape < CELL_SHAPES && ok; shape++)
	{
		for (int i = 0; i < cell_count(atlas, (cell_shape_t)shape) && ok; i++)
		{
			const cell_build_t *cell = &build[shape][i];
			int members[12];
			int around[4];

			if (!atlas->used[shape][i])
			{
				continue;
			}
			memcpy(around, cell->box, sizeof(around));
			for (int m = 0; m < 12; m++)
			{
				cell_shape_t member_shape;
				int member = atlas_find(atlas, cell->sources[m], &member_shape);

				// Not in the atlas (or of another shape): the cell itself
				members[m] = member >= 0 && member_shape == (cell_shape_t)shape ? member : i;
				atlas->month_cells[shape][i][m] = members[m];
				if (members[m] != i)
				{
					const cell_build_t *other = &build[shape][members[m]];
					int right = around[0] + around[2] > other->box[0] + other->box[2] ? around[0] + around[2] : other->box[0] + other->box[2];
					int bottom = around[1] + around[3] > other->box[1] + other->box[3] ? around[1] + around[3] : other->box[1] + other->box[3];

					if (other->width != cell->width || other->height != cell->height)
					{
						plant_error("The months' pictures of '%s' are not all of one size ('%s' is %dx%d, '%s' %dx%d)\n",
							atlas->cells[shape][i].color, atlas->cells[shape][i].color, cell->width, cell->height, cell->sources[m],
							other->width, other->height);
						ok = false;
						break;
					}
					around[0] = around[0] < other->box[0] ? around[0] : other->box[0];
					around[1] = around[1] < other->box[1] ? around[1] : other->box[1];
					around[2] = right - around[0];
					around[3] = bottom - around[1];
				}
			}
			for (int m = 0; m < 12 && ok; m++)
			{
				cell_build_t *member = &build[shape][members[m]];
				int right = member->cut[0] + member->cut[2] > around[0] + around[2] ? member->cut[0] + member->cut[2] : around[0] + around[2];
				int bottom = member->cut[1] + member->cut[3] > around[1] + around[3] ? member->cut[1] + member->cut[3] : around[1] + around[3];

				member->cut[0] = member->cut[0] < around[0] ? member->cut[0] : around[0];
				member->cut[1] = member->cut[1] < around[1] ? member->cut[1] : around[1];
				member->cut[2] = right - member->cut[0];
				member->cut[3] = bottom - member->cut[1];
			}
		}
	}
	// The pictures in, each cut to its box
	for (int shape = 0; shape < CELL_SHAPES && ok; shape++)
	{
		for (int i = 0; i < cell_count(atlas, (cell_shape_t)shape) && ok; i++)
		{
			const cell_build_t *cell = &build[shape][i];
			int width = 0;
			int height = 0;
			uint8_t *picture;
			uint8_t *cropped;
			int rectangle[4];

			if (!atlas->used[shape][i])
			{
				continue;
			}
			picture = load_image(atlas->cells[shape][i].color, &width, &height);
			if (picture == NULL)
			{
				ok = false;
				break;
			}
			cell_rectangle(atlas, (cell_shape_t)shape, i, rectangle);
			atlas->anchor[shape][i] = stem_anchor(picture, width, cell->cut);
			cropped = picture_crop(picture, width, cell->cut);
			blit_scaled(atlas->pixels, atlas->size, rectangle, cropped, cell->cut[2], cell->cut[3], atlas->placed[shape][i]);
			free(cropped);
			stbi_image_free(picture);
			bleed_cell(atlas->pixels, atlas->size, rectangle);
		}
	}
	for (int shape = 0; shape < CELL_SHAPES; shape++)
	{
		free(build[shape]);
	}
	return ok;
}

// FNV-1a, 64 bits
static uint64_t hash_bytes(uint64_t hash, const uint8_t *bytes, size_t count)
{
	for (size_t i = 0; i < count; i++)
	{
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	}
	return hash;
}

// The next level down: each texel the mean of four, the color weighed by
// alpha (so the cut away does not bleed in)
static uint8_t *level_down(const uint8_t *pixels, int width, int height, int *width_out, int *height_out)
{
	int next_width = width > 1 ? width / 2 : 1;
	int next_height = height > 1 ? height / 2 : 1;
	uint8_t *next = checked_realloc(NULL, (size_t)next_width * (size_t)next_height * 4);

	for (int y = 0; y < next_height; y++)
	{
		for (int x = 0; x < next_width; x++)
		{
			float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			float weight = 0.0f;
			uint8_t *texel = &next[((size_t)y * (size_t)next_width + (size_t)x) * 4];

			for (int dy = 0; dy < 2; dy++)
			{
				for (int dx = 0; dx < 2; dx++)
				{
					int sx = x * 2 + dx < width ? x * 2 + dx : width - 1;
					int sy = y * 2 + dy < height ? y * 2 + dy : height - 1;
					const uint8_t *from = &pixels[((size_t)sy * (size_t)width + (size_t)sx) * 4];
					float alpha = (float)from[3] / 255.0f + 1.0f / 1024.0f;

					for (int k = 0; k < 3; k++)
					{
						sum[k] += (float)from[k] * alpha;
					}
					sum[3] += (float)from[3];
					weight += alpha;
				}
			}
			for (int k = 0; k < 3; k++)
			{
				texel[k] = (uint8_t)(sum[k] / weight + 0.5f);
			}
			texel[3] = (uint8_t)(sum[3] / 4.0f + 0.5f);
		}
	}
	*width_out = next_width;
	*height_out = next_height;
	return next;
}

// BC7 encoding, a share of the block rows each thread
typedef struct
{
	const uint8_t *pixels;
	int width;
	int height;
	int first_row;
	int row_count;
	uint8_t *blocks;
} encode_job_t;

static bc7enc_compress_block_params encode_parameters;

static void *encode_rows(void *argument)
{
	const encode_job_t *job = argument;
	int blocks_across = (job->width + 3) / 4;

	for (int by = job->first_row; by < job->first_row + job->row_count; by++)
	{
		for (int bx = 0; bx < blocks_across; bx++)
		{
			uint8_t texels[16 * 4];

			for (int y = 0; y < 4; y++)
			{
				int source_y = by * 4 + y < job->height ? by * 4 + y : job->height - 1;

				for (int x = 0; x < 4; x++)
				{
					int source_x = bx * 4 + x < job->width ? bx * 4 + x : job->width - 1;

					memcpy(&texels[(y * 4 + x) * 4], &job->pixels[((size_t)source_y * (size_t)job->width + (size_t)source_x) * 4], 4);
				}
			}
			bc7enc_compress_block(&job->blocks[((size_t)by * (size_t)blocks_across + (size_t)bx) * 16], texels, &encode_parameters);
		}
	}
	return NULL;
}

static void encode_bc7(const uint8_t *pixels, int width, int height, uint8_t *blocks_out)
{
	static bool initialized = false;
	int rows = (height + 3) / 4;
	long processors = sysconf(_SC_NPROCESSORS_ONLN);
	int thread_count = processors < 1 ? 1 : (processors > 32 ? 32 : (int)processors);
	pthread_t threads[32];
	encode_job_t jobs[32];
	int started = 0;

	if (!initialized)
	{
		bc7enc_compress_block_init();
		memset(&encode_parameters, 0, sizeof(encode_parameters));
		encode_parameters.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;
		encode_parameters.m_try_least_squares = BC7ENC_TRUE;
		encode_parameters.m_mode_partition_estimation_filterbank = BC7ENC_TRUE;
		encode_parameters.m_use_mode5_for_alpha = BC7ENC_TRUE;
		encode_parameters.m_use_mode7_for_alpha = BC7ENC_TRUE;
		for (int k = 0; k < 4; k++)
		{
			encode_parameters.m_weights[k] = 1;
		}
		initialized = true;
	}
	thread_count = thread_count < rows ? thread_count : rows;
	for (int t = 0; t < thread_count; t++)
	{
		jobs[t].pixels = pixels;
		jobs[t].width = width;
		jobs[t].height = height;
		jobs[t].first_row = rows * t / thread_count;
		jobs[t].row_count = rows * (t + 1) / thread_count - jobs[t].first_row;
		jobs[t].blocks = blocks_out;
		if (t > 0 && pthread_create(&threads[t], NULL, encode_rows, &jobs[t]) == 0)
		{
			started |= 1 << t;
		}
		else if (t > 0)
		{
			encode_rows(&jobs[t]);
		}
	}
	encode_rows(&jobs[0]);
	for (int t = 1; t < thread_count; t++)
	{
		if (started & (1 << t))
		{
			pthread_join(threads[t], NULL);
		}
	}
}

static size_t level_bytes(int width, int height)
{
	return (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * 16;
}

// Its file (engine/plant_atlas_file.h): written if what it is made from
// changed
static bool atlas_write_file(const atlas_t *atlas)
{
	char path[PATH_LENGTH * 2];
	plant_atlas_file_header_t header;
	uint64_t hash = 14695981039346656037ull;
	int level_count = 1;
	size_t total = 0;
	uint8_t *data;
	size_t offset = 0;
	FILE *file;

	_Static_assert(sizeof(plant_atlas_file_header_t) == 148, "the DDS header with DX10 is 148 bytes");
	hash = hash_bytes(hash, atlas->pixels, (size_t)atlas->size * (size_t)atlas->size * 4);
	snprintf(path, sizeof(path), "%s/%s", atlas->data_directory, atlas->file_name);
	// The same as it is: left alone
	file = fopen(path, "rb");
	if (file != NULL)
	{
		plant_atlas_file_header_t old;
		bool same = fread(&old, sizeof(old), 1, file) == 1 && old.tag == PLANT_ATLAS_TAG && old.hash[0] == (uint32_t)hash
			&& old.hash[1] == (uint32_t)(hash >> 32) && (int)old.width == atlas->size;

		fclose(file);
		if (same)
		{
			return true;
		}
	}
	for (int size = atlas->size; size > 1; size /= 2)
	{
		level_count++;
	}
	for (int k = 0, size = atlas->size; k < level_count; k++, size = size > 1 ? size / 2 : 1)
	{
		total += level_bytes(size, size);
	}
	data = checked_realloc(NULL, total);
	printf("Compressing the %s atlas (%dx%d, BC7)...\n", atlas->name, atlas->size, atlas->size);
	fflush(stdout);
	{
		const uint8_t *level = atlas->pixels;
		uint8_t *owned = NULL;
		int width = atlas->size;
		int height = atlas->size;

		for (int k = 0; k < level_count; k++)
		{
			encode_bc7(level, width, height, data + offset);
			offset += level_bytes(width, height);
			if (k + 1 < level_count)
			{
				uint8_t *next = level_down(level, width, height, &width, &height);

				free(owned);
				owned = next;
				level = next;
			}
		}
		free(owned);
	}
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, "DDS ", 4);
	header.header_size = 124;
	header.flags = 0x1u | 0x2u | 0x4u | 0x1000u | 0x20000u | 0x80000u;
	header.height = (uint32_t)atlas->size;
	header.width = (uint32_t)atlas->size;
	header.linear_size = (uint32_t)level_bytes(atlas->size, atlas->size);
	header.level_count = (uint32_t)level_count;
	header.tag = PLANT_ATLAS_TAG;
	header.hash[0] = (uint32_t)hash;
	header.hash[1] = (uint32_t)(hash >> 32);
	header.format_size = 32;
	header.format_flags = 0x4u;
	memcpy(&header.four_cc, "DX10", 4);
	header.caps = 0x1000u | 0x400000u | 0x8u;
	header.dxgi_format = PLANT_ATLAS_DXGI_BC7_UNORM;
	header.resource_dimension = PLANT_ATLAS_DIMENSION_TEXTURE_2D;
	header.layer_count = 1;
	file = fopen(path, "wb");
	if (file == NULL)
	{
		plant_error("Cannot write '%s'\n", path);
		free(data);
		return false;
	}
	fwrite(&header, sizeof(header), 1, file);
	fwrite(data, 1, total, file);
	fclose(file);
	free(data);
	printf("Wrote %s\n", path);
	return true;
}

// Where in the atlas (0..1) a cell's picture is: u0, v0, u1, v1 (v0 the
// top), and its shape (width over height)
typedef struct
{
	float uv[4];
	float aspect;
	float anchor;                       // where its stem is across it (0..1 from u0)
} cell_place_t;

static cell_place_t atlas_cell_place(const atlas_t *atlas, cell_shape_t shape, int index)
{
	const int *placed = atlas->placed[shape][index];
	float size = (float)atlas->size;
	cell_place_t place;

	place.uv[0] = (float)placed[0] / size;
	place.uv[1] = (float)placed[1] / size;
	place.uv[2] = (float)(placed[0] + placed[2]) / size;
	place.uv[3] = (float)(placed[1] + placed[3]) / size;
	place.aspect = placed[3] > 0 ? (float)placed[2] / (float)placed[3] : 1.0f;
	place.anchor = atlas->anchor[shape][index];
	return place;
}

// ---------------------------------------------------------------------------
// The mesh being built: one set of vertices, a list of triangles per material

typedef struct
{
	object_file_vertex_t *vertices;
	size_t vertex_count;
	size_t vertex_capacity;
	uint32_t *indices[MATERIAL_COUNT];
	size_t index_count[MATERIAL_COUNT];
	size_t index_capacity[MATERIAL_COUNT];
	vec3 minimum;
	vec3 maximum;
	// Each bark triangle's level (a tree's: 1 the trunk's branches, 2 theirs,
	// ...; BARK_LEVEL_STALK fruit stalks), set before it is added: the
	// levels of detail drop them deepest first (finish_lods)
	uint8_t bark_level;
	uint8_t *bark_levels;
	size_t bark_levels_capacity;
	// Each foliage triangle's rank in thinning (0 kept longest), set before
	// it is added: a needle cone's strips ranked so that any first share of
	// them is spread evenly round it (finish_lods)
	uint8_t foliage_rank;
	uint8_t *foliage_ranks;
	size_t foliage_ranks_capacity;
	// What each vertex is on the plant (object_file_plant_vertex_t), one per
	// vertex: the next ones added take plant_along, plant_random and
	// plant_extra (their height and a card's place in the crown are filled
	// in at the end, finish_plant_vertices)
	object_file_plant_vertex_t *plant_vertices;
	float plant_along;
	float plant_random;
	float plant_extra;
	// The snow baked in (OBJECT_FILE_SNOW), one per vertex; NULL: none
	object_file_snow_vertex_t *snow_vertices;
	// Written into the materials (OBJECT_MATERIAL_LODS, _SHUFFLED)
	bool lods[MATERIAL_COUNT];
	bool shuffled[MATERIAL_COUNT];
	uint32_t lod_index_count[MATERIAL_COUNT][3];
} mesh_t;

#define BARK_LEVEL_STALK 255

// 0..1 from a number, without touching the plant's random sequence (a
// card's own number for the seasons: the shapes stay as the seed grows them)
static float hash_unit(uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return (float)(value >> 8) * (1.0f / 16777216.0f);
}

// tangent: the way u grows; bitangent: the way v grows (its sign is kept)
static uint32_t add_vertex(mesh_t *mesh, vec3 position, vec3 normal, vec3 tangent, vec3 bitangent, float u, float v)
{
	object_file_vertex_t *vertex;

	if (mesh->vertex_count == mesh->vertex_capacity)
	{
		mesh->vertex_capacity = mesh->vertex_capacity > 0 ? mesh->vertex_capacity * 2 : 4096;
		mesh->vertices = checked_realloc(mesh->vertices, mesh->vertex_capacity * sizeof(object_file_vertex_t));
		mesh->plant_vertices = checked_realloc(mesh->plant_vertices, mesh->vertex_capacity * sizeof(object_file_plant_vertex_t));
	}
	mesh->plant_vertices[mesh->vertex_count].along = mesh->plant_along;
	mesh->plant_vertices[mesh->vertex_count].height = 0.0f;
	mesh->plant_vertices[mesh->vertex_count].random = mesh->plant_random;
	mesh->plant_vertices[mesh->vertex_count].extra = mesh->plant_extra;
	normal = normalized(normal);
	// The tangent square to the normal (Gram-Schmidt)
	tangent = normalized(sub(tangent, scale(normal, dot(normal, tangent))));
	vertex = &mesh->vertices[mesh->vertex_count];
	memcpy(vertex->position, &position, sizeof(float) * 3);
	memcpy(vertex->normal, &normal, sizeof(float) * 3);
	memcpy(vertex->tangent, &tangent, sizeof(float) * 3);
	vertex->tangent[3] = dot(cross(normal, tangent), bitangent) >= 0.0f ? 1.0f : -1.0f;
	vertex->uv[0] = u;
	vertex->uv[1] = v;
	if (mesh->vertex_count == 0)
	{
		mesh->minimum = position;
		mesh->maximum = position;
	}
	mesh->minimum = v3(fminf(mesh->minimum.x, position.x), fminf(mesh->minimum.y, position.y), fminf(mesh->minimum.z, position.z));
	mesh->maximum = v3(fmaxf(mesh->maximum.x, position.x), fmaxf(mesh->maximum.y, position.y), fmaxf(mesh->maximum.z, position.z));
	return (uint32_t)mesh->vertex_count++;
}

static void add_triangle(mesh_t *mesh, int material, uint32_t a, uint32_t b, uint32_t c)
{
	if (mesh->index_count[material] + 3 > mesh->index_capacity[material])
	{
		mesh->index_capacity[material] = mesh->index_capacity[material] > 0 ? mesh->index_capacity[material] * 2 : 4096;
		mesh->indices[material] = checked_realloc(mesh->indices[material], mesh->index_capacity[material] * sizeof(uint32_t));
	}
	mesh->indices[material][mesh->index_count[material]++] = a;
	mesh->indices[material][mesh->index_count[material]++] = b;
	mesh->indices[material][mesh->index_count[material]++] = c;
	if (material == MATERIAL_BARK)
	{
		size_t triangle = mesh->index_count[material] / 3 - 1;

		if (triangle >= mesh->bark_levels_capacity)
		{
			mesh->bark_levels_capacity = mesh->bark_levels_capacity > 0 ? mesh->bark_levels_capacity * 2 : 4096;
			mesh->bark_levels = checked_realloc(mesh->bark_levels, mesh->bark_levels_capacity);
		}
		mesh->bark_levels[triangle] = mesh->bark_level;
	}
	if (material == MATERIAL_FOLIAGE)
	{
		size_t triangle = mesh->index_count[material] / 3 - 1;

		if (triangle >= mesh->foliage_ranks_capacity)
		{
			mesh->foliage_ranks_capacity = mesh->foliage_ranks_capacity > 0 ? mesh->foliage_ranks_capacity * 2 : 4096;
			mesh->foliage_ranks = checked_realloc(mesh->foliage_ranks, mesh->foliage_ranks_capacity);
		}
		mesh->foliage_ranks[triangle] = mesh->foliage_rank;
	}
}

// The tangent's w is the bitangent's sign; its size is how lit the vertex
// is (object.frag: 1 as it was, less in the shade of a crown; above 1 a
// little brighter). Each card a little lighter or darker than the next, so
// a crown seen from afar shows its clumps rather than one flat green
static void shade_vertices(mesh_t *mesh, uint32_t first, uint32_t count, float shade)
{
	for (uint32_t i = first; i < first + count && i < mesh->vertex_count; i++)
	{
		mesh->vertices[i].tangent[3] *= shade;
	}
}

// A card: a picture of the atlas on a quad, its foot (the picture's stem,
// at the bottom) at base, along its length (bottom to top), across its width; lit
// with normal, not by its own facing (cards lit by their facing look flat
// and flicker as the plant turns: they take the crown's outward direction,
// or the sky's).
//
// The foliage is marked OBJECT_MATERIAL_LIGHTING_NORMALS, so the game lights
// a card's back as its front (a real leaf's back has its normal turned
// round). Each card's front still faces the way its normal does, for an
// older game that turns them round all the same: mirrored to turn it round
// (a leaf mirrored is still a leaf)
static void add_card(mesh_t *mesh, vec3 base, vec3 along, vec3 across, vec3 normal, const cell_place_t *place)
{
	vec3 left;
	vec3 right;
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	vec3 down = scale(along, -1.0f);

	if (dot(cross(across, along), normal) < 0.0f)
	{
		across = scale(across, -1.0f);
	}
	// The stem on base, the picture's sides as far from it as they are
	left = scale(across, -place->anchor);
	right = scale(across, 1.0f - place->anchor);
	// Its own number, its foot and its tip (for the seasons, the new growth)
	mesh->plant_random = hash_unit((uint32_t)mesh->vertex_count * 2654435761u + 17u);
	mesh->plant_extra = 0.0f;
	mesh->plant_along = 0.0f;
	a = add_vertex(mesh, add(base, left), normal, across, down, place->uv[0], place->uv[3]);
	b = add_vertex(mesh, add(base, right), normal, across, down, place->uv[2], place->uv[3]);
	mesh->plant_along = 1.0f;
	c = add_vertex(mesh, add(add(base, right), along), normal, across, down, place->uv[2], place->uv[1]);
	d = add_vertex(mesh, add(add(base, left), along), normal, across, down, place->uv[0], place->uv[1]);
	mesh->plant_along = 0.0f;
	add_triangle(mesh, MATERIAL_FOLIAGE, a, b, c);
	add_triangle(mesh, MATERIAL_FOLIAGE, a, c, d);
	shade_vertices(mesh, a, 4, random_between(0.8f, 1.12f));
}

// A card of a picture, its width from the picture's shape
static void add_picture(mesh_t *mesh, vec3 base, vec3 along_unit, vec3 across_unit, float length, vec3 normal, const cell_place_t *place)
{
	add_card(mesh, base, scale(along_unit, length), scale(across_unit, length * place->aspect), normal, place);
}

// A strip of cards along a bent line (a fern's frond): the picture cut in
// as many pieces along its length, each piece on one segment
// The same folded along its middle, a V (fold: its edges up its face's way
// this much of its half width); folded, each piece is two cards, a half
// each side of the middle
static void add_folded_strip(mesh_t *mesh, const vec3 *points, int count, vec3 across_unit, float width, vec3 normal,
	const cell_place_t *place, float fold)
{
	uint32_t previous_left = 0;
	uint32_t previous_right = 0;
	uint32_t previous_middle = 0;
	vec3 side = across_unit;

	// Its face the normal's way, once for all of it (by where it goes on the
	// whole: decided at each point, an arching one flipped over where it
	// turns down, twisted into a bow tie there)
	if (count > 1 && dot(cross(side, sub(points[count - 1], points[0])), normal) < 0.0f)
	{
		side = scale(side, -1.0f);
	}
	if (fold > 0.0f && count > 1)
	{
		vec3 face = normalized(cross(side, sub(points[count - 1], points[0])));
		vec3 lift = scale(face, fminf(fold, 1.0f) * width * 0.5f);

		for (int i = 0; i < count; i++)
		{
			float t = (float)i / (float)(count - 1);
			float v = place->uv[3] + (place->uv[1] - place->uv[3]) * t;
			vec3 along = i + 1 < count ? sub(points[i + 1], points[i]) : sub(points[i], points[i - 1]);
			// (the edges come in a little as they rise: as wide across)
			vec3 half = scale(side, width * 0.5f * sqrtf(fmaxf(1.0f - fminf(fold, 1.0f) * fminf(fold, 1.0f) * 0.5f, 0.3f)));
			uint32_t left;
			uint32_t middle;
			uint32_t right;

			if (i == 0)
			{
				mesh->plant_random = hash_unit((uint32_t)mesh->vertex_count * 2654435761u + 17u);
				mesh->plant_extra = 0.0f;
			}
			mesh->plant_along = t;
			left = add_vertex(mesh, add(sub(points[i], half), lift), normal, side, scale(along, -1.0f), place->uv[0], v);
			middle = add_vertex(mesh, points[i], normal, side, scale(along, -1.0f), (place->uv[0] + place->uv[2]) * 0.5f, v);
			right = add_vertex(mesh, add(add(points[i], half), lift), normal, side, scale(along, -1.0f), place->uv[2], v);
			if (i > 0)
			{
				add_triangle(mesh, MATERIAL_FOLIAGE, previous_left, previous_middle, middle);
				add_triangle(mesh, MATERIAL_FOLIAGE, previous_left, middle, left);
				add_triangle(mesh, MATERIAL_FOLIAGE, previous_middle, previous_right, right);
				add_triangle(mesh, MATERIAL_FOLIAGE, previous_middle, right, middle);
			}
			previous_left = left;
			previous_middle = middle;
			previous_right = right;
		}
		shade_vertices(mesh, previous_right + 1 - 3 * (uint32_t)count, 3 * (uint32_t)count, random_between(0.85f, 1.1f));
		return;
	}
	for (int i = 0; i < count; i++)
	{
		float t = (float)i / (float)(count - 1);
		float v = place->uv[3] + (place->uv[1] - place->uv[3]) * t;
		vec3 along = i + 1 < count ? sub(points[i + 1], points[i]) : sub(points[i], points[i - 1]);
		vec3 half;
		uint32_t left;
		uint32_t right;

		half = scale(side, width * 0.5f);
		if (i == 0)
		{
			mesh->plant_random = hash_unit((uint32_t)mesh->vertex_count * 2654435761u + 17u);
			mesh->plant_extra = 0.0f;
		}
		mesh->plant_along = t;
		left = add_vertex(mesh, sub(points[i], half), normal, side, scale(along, -1.0f), place->uv[0], v);
		right = add_vertex(mesh, add(points[i], half), normal, side, scale(along, -1.0f), place->uv[2], v);
		if (i > 0)
		{
			add_triangle(mesh, MATERIAL_FOLIAGE, previous_left, previous_right, right);
			add_triangle(mesh, MATERIAL_FOLIAGE, previous_left, right, left);
		}
		previous_left = left;
		previous_right = right;
	}
	if (count > 0)
	{
		shade_vertices(mesh, previous_right + 1 - 2 * (uint32_t)count, 2 * (uint32_t)count, random_between(0.85f, 1.1f));
	}
}

static void add_strip(mesh_t *mesh, const vec3 *points, int count, vec3 across_unit, float width, vec3 normal, const cell_place_t *place)
{
	add_folded_strip(mesh, points, count, across_unit, width, normal, place, 0.0f);
}

// ---------------------------------------------------------------------------
// Growing

#define MAX_POINTS 64

typedef struct bough bough_t;

typedef struct
{
	const plant_kind_t *kind;
	mesh_t mesh;
	// The boughs (kind branch_cards), for the snow baked in (bake_snow)
	bough_t *boughs;
	int bough_count;
	int bough_capacity;
	float height;
	float bark_meters;
	float leaf_size;
	cell_place_t leaves[MAX_SOURCES];
	int leaf_count;
	cell_place_t twigs[MAX_SOURCES];
	int twig_count;
	cell_place_t heads[MAX_SOURCES];
	int head_count;
	// Each head's picture from below (name_under.png beside it; a cup's)
	cell_place_t heads_under[MAX_SOURCES];
	bool head_has_under[MAX_SOURCES];
	vec3 crown_center;
	// Twigs and leaves together (--twig-share), leaves along the other branches (--loose-leaves)
	float twig_share;
	float loose_leaves;
	// Fruit (--fruit ...)
	cell_place_t fruits[MAX_SOURCES];
	int fruit_count;
	fruit_shape_t fruit_shape;
	float fruit_length;
	float fruit_width;                  // 0: from the picture
	fruit_hang_t fruit_hang;
	fruit_zone_t fruit_zone;
	float fruit_stalk;
	int cluster_minimum;
	int cluster_maximum;
	float fruit_per_branch;
	float fruit_bunch;                  // 0: a cluster's stalks from one point, fanned; above: packed on a short hanging stalk
	float fruit_tip;                    // 0: fruit anywhere on a finest branch's outer part; 1: at its tip
	int fruits_grown;
	// The trunk
	float stem_crown_start;             // a stem's first branches, of its length (a forked one starts higher)
	float trunk_u0;                     // which part of the whole trunk a stem is (forked: the part above the fork)
	float trunk_u1;
	bool kinked_trunk;
	bool crown_center_set;
	float trunk_taper;                  // a stem's radius at its top, of its foot's
	float trunk_reference;              // the trunk's radius at its foot: a branch's thickness is of this (the moss)
	float trunk_children;               // its branches, of the kind's (a fork's trunk has its share)
	vec3 trunk_end;                     // where the last trunk grown ends, and heads
	vec3 trunk_end_direction;
	float wind_side;
	vec3 downwind;                      // level
	// A mushroom: its shape and where its texture is in the atlas (the
	// template's places on it: mushroom_uv)
	float frond_angle;                  // ferns: how steep their fronds start (degrees up from level)
	float frond_droop;                  // and how far they arch over (1: much)
	plant_herb_t herb;                  // a small plant's shape, if given (grow_herb)
	plant_mushroom_t mushroom;
	cell_place_t mushroom_place;
	float mushroom_crop[4];             // what of its texture went in the atlas: x, y, width, height (0..1 of it)
} plant_t;

// A branch as it was grown: its points, and its radius at each
typedef struct
{
	vec3 points[MAX_POINTS];
	vec3 directions[MAX_POINTS];
	float radii[MAX_POINTS];
	int count;
	float length;
} branch_t;

// The snow on a bough (bake_snow): a grid over its card, this many cells
// across it, along it
enum
{
	SNOW_CELLS_ACROSS = 12,
	SNOW_CELLS_ALONG = 24
};

// A bough as grown (kind branch_cards), for the snow baked in
struct bough
{
	branch_t branch;
	float card_length;
	float width;                        // its top card's
	cell_place_t place;                 // the top card's picture
	float top_offset;                   // the top card lifted along its face this far
	float gap;                          // between its two cards
	uint32_t first_vertex;              // its bark's first, then
	uint32_t top_first;                 // its top card's first (a grid: BRANCH_CARD_ALONG + 1 rows of BRANCH_CARD_ACROSS + 1)
	uint32_t vertex_end;                // past its last (the card under, its fruit)
	// The bake's
	float depth[SNOW_CELLS_ALONG][SNOW_CELLS_ACROSS]; // the snow on it at all of the snow (m)
	float load;                         // its own snow (kg)
	float carried;                      // of the one above it, resting on it (kg)
	float drop;                         // how far its tip sags under all of it (m)
	uint32_t snow_first;                // the snow lying on it: its vertices
	uint32_t snow_end;
};

// Where along the branch (0..1) a point is, and its direction there
static void branch_at(const branch_t *branch, float t, vec3 *point_out, vec3 *direction_out, float *radius_out)
{
	float place = t * (float)(branch->count - 1);
	int i = (int)place;
	float f;

	if (i >= branch->count - 1)
	{
		i = branch->count - 2;
	}
	f = place - (float)i;
	*point_out = add(scale(branch->points[i], 1.0f - f), scale(branch->points[i + 1], f));
	*direction_out = normalized(add(scale(branch->directions[i], 1.0f - f), scale(branch->directions[i + 1], f)));
	*radius_out = branch->radii[i] * (1.0f - f) + branch->radii[i + 1] * f;
}

// The light a card takes: out from a point low in the crown (its bottom, on
// the axis), so the leaves under the middle face out sideways rather than
// down at the dark ground; and leaning up towards the sky that lights a
// crown from above
static vec3 crown_normal(const plant_t *plant, vec3 point)
{
	return normalized(add(normalized(sub(point, plant->crown_center)), v3(0.0f, 0.7f, 0.0f)));
}

// A tube along a branch: a ring at each point, the bark tiled round and
// along
static void add_tube(plant_t *plant, const branch_t *branch, int sides, float base_radius, int material)
{
	float circumference = 2.0f * PI_F * base_radius;
	int around_repeats = (int)(circumference / plant->bark_meters + 0.5f);
	vec3 side = perpendicular(branch->directions[0]);
	uint32_t first = (uint32_t)plant->mesh.vertex_count;
	float along = 0.0f;

	around_repeats = around_repeats < 1 ? 1 : around_repeats;
	// For the seasons and the moss: one number per branch, how thick it is
	// (of the trunk's foot), and along it
	plant->mesh.plant_random = hash_unit((uint32_t)plant->mesh.vertex_count * 2654435761u + 91u);
	plant->mesh.plant_extra = plant->trunk_reference > 0.0f ? fminf(base_radius / plant->trunk_reference, 1.0f) : 0.0f;
	for (int i = 0; i < branch->count; i++)
	{
		vec3 heading = branch->directions[i];

		plant->mesh.plant_along = branch->count > 1 ? (float)i / (float)(branch->count - 1) : 0.0f;

		// Carried along (parallel transport), so the rings do not twist
		side = normalized(sub(side, scale(heading, dot(side, heading))));
		if (i > 0)
		{
			along += sqrtf(dot(sub(branch->points[i], branch->points[i - 1]), sub(branch->points[i], branch->points[i - 1])));
		}
		for (int k = 0; k <= sides; k++)
		{
			float angle = 2.0f * PI_F * (float)k / (float)sides;
			vec3 out = rotated(side, heading, angle);

			add_vertex(&plant->mesh, add(branch->points[i], scale(out, branch->radii[i])), out, cross(heading, out), heading,
				(float)around_repeats * (float)k / (float)sides, along / plant->bark_meters);
		}
	}
	// The bark's vertices carry their branch's level in the tangent's size
	// (its sign is the bitangent's): the game fades a level out before its
	// level of detail drops it (object_model.h, lod_keep_level)
	if (material == MATERIAL_BARK)
	{
		float level = plant->mesh.bark_level == BARK_LEVEL_STALK ? 9.0f : (float)(plant->mesh.bark_level > 0 ? plant->mesh.bark_level : 1);

		for (size_t v = first; v < plant->mesh.vertex_count; v++)
		{
			plant->mesh.vertices[v].tangent[3] *= level;
		}
	}
	for (int i = 0; i + 1 < branch->count; i++)
	{
		for (int k = 0; k < sides; k++)
		{
			uint32_t a = first + (uint32_t)(i * (sides + 1) + k);
			uint32_t b = a + 1;
			uint32_t c = a + (uint32_t)(sides + 1);
			uint32_t d = c + 1;

			// Counterclockwise seen from outside
			add_triangle(&plant->mesh, material, a, b, c);
			add_triangle(&plant->mesh, material, b, d, c);
		}
	}
}

static float smooth_step(float low, float high, float value)
{
	float t = fminf(fmaxf((value - low) / (high - low), 0.0f), 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

// How a branch is shaped besides its bend and reach (plant_kind_t: droop,
// tip_lift; flare: its foot swelling, the collar or the trunk's roots;
// gnarl, kinks; the trunk's lean and knob)
#define MAX_KINKS 100
#define THINNEST_BRANCH_RADIUS 0.002f

typedef struct
{
	float droop;
	float tip_lift;
	float flare;
	float gnarl;
	float thinnest;                     // its radius no less than this (m; 0: THINNEST_BRANCH_RADIUS)
	// Where the branches out of it leave it (0..1, rising) and round it
	// (radians); trace_shaped_branch turns it away from each by
	// kink_radians and gives each its axis to lean out about (kink_axes)
	int kink_count;
	const float *kink_t;
	const float *kink_azimuth;
	float kink_radians;
	vec3 *kink_axes;
	// The trunk bent by the wind: lean_radians from upright towards
	// lean_toward (level), for lean_length of the whole trunk, then up
	// again (straighten); u0..u1: which part of the whole trunk this is
	// (a forked stem goes on from the trunk below it); part: its own lean
	// out from the others (level, its length the tangent)
	float lean_radians;
	vec3 lean_toward;
	float lean_length;
	float straighten;
	float u0;
	float u1;
	vec3 part;
	float knob;
	// A trunk kept to the way it set out (its random bend and gnarl pulled
	// back, per meter): a conifer's leader grows straight up
	float upright;
	bool trunk;
} branch_shape_t;

// Where the wind-bent trunk heads at u (0..1 of the whole trunk)
static vec3 lean_heading(const branch_shape_t *shape, float u)
{
	float rest = 1.0f - shape->lean_length;
	float angle = shape->lean_radians * (1.0f - shape->straighten * smooth_step(shape->lean_length, shape->lean_length + rest * 0.7f, u));

	return normalized(add(v3(sinf(angle) * shape->lean_toward.x, cosf(angle), sinf(angle) * shape->lean_toward.z), shape->part));
}

// A bent, tapering line from start: its points (segments of segment_meters)
static void trace_shaped_branch(branch_t *branch, vec3 start, vec3 direction, float length, float radius, float taper, float bend,
	float reach, float segment_meters, branch_shape_t *shape)
{
	float gnarl = shape != NULL ? shape->gnarl : 0.0f;
	float thinnest = shape != NULL && shape->thinnest > 0.0f ? shape->thinnest : THINNEST_BRANCH_RADIUS;
	int segments;
	vec3 point = start;
	vec3 heading = direction;
	float step;
	// The swelling at its foot fades over a few of its radii
	float flare_meters = fmaxf(radius * 2.5f, 0.01f);
	// Gnarl: a slowly turning push sideways, and the radius swelling and
	// narrowing (each a random walk, smoothed)
	vec3 wander = v3(0.0f, 0.0f, 0.0f);
	float swell = 0.0f;
	int kink = 0;
	bool leaning = shape != NULL && shape->trunk && shape->lean_radians > 0.0f;

	// Crooked ones need more points, or they turn in corners
	segment_meters /= 1.0f + 1.5f * gnarl;
	segments = (int)(length / segment_meters);
	segments = segments < 3 ? 3 : (segments > MAX_POINTS - 1 ? MAX_POINTS - 1 : segments);
	step = length / (float)segments;
	branch->count = segments + 1;
	branch->length = length;
	if (gnarl > 0.0f)
	{
		wander = scale(perpendicular(heading), random_between(-1.0f, 1.0f));
		swell = random_between(-1.0f, 1.0f);
	}
	for (int i = 0; i <= segments; i++)
	{
		float t = (float)i / (float)segments;
		float bow = 0.0f;

		branch->points[i] = point;
		branch->directions[i] = heading;
		branch->radii[i] = fmaxf(radius * (1.0f - (1.0f - taper) * t), thinnest);
		if (shape != NULL)
		{
			branch->radii[i] *= 1.0f + shape->flare * expf(-t * length / flare_meters);
			// A pollard's head: swollen, rounded off on top
			if (shape->knob > 0.0f)
			{
				float top = smooth_step(0.9f, 1.0f, t);

				branch->radii[i] *= (1.0f + shape->knob * smooth_step(0.65f, 0.88f, t)) * sqrtf(fmaxf(1.0f - top * top, 0.0f));
			}
			if (gnarl > 0.0f)
			{
				swell = 0.75f * swell + 0.25f * random_between(-1.6f, 1.6f);
				branch->radii[i] *= 1.0f + 0.3f * gnarl * swell;
			}
			// Down under its weight, more the longer it is and the farther
			// out; its tip up again
			bow = -shape->droop * length * t + shape->tip_lift * smooth_step(0.6f, 1.0f, t);
		}
		if (i == segments)
		{
			break;
		}
		// Bent at random, and up or down
		heading = normalized(add(add(heading, scale(random_direction(), bend * step)), v3(0.0f, (reach + bow) * step, 0.0f)));
		if (gnarl > 0.0f)
		{
			wander = add(scale(wander, 0.8f), scale(random_direction(), 0.45f));
			wander = sub(wander, scale(heading, dot(wander, heading)));
			// A radian and more of wander all along at gnarl 1, however long
			heading = normalized(add(heading, scale(wander, gnarl * 0.7f / sqrtf((float)segments))));
		}
		if (leaning)
		{
			// Kept to its bent path
			heading = normalized(add(scale(heading, 0.4f), scale(lean_heading(shape, shape->u0 + (shape->u1 - shape->u0) * t), 0.6f)));
		}
		else if (shape != NULL && shape->trunk && gnarl > 0.0f)
		{
			// A crooked trunk still grows up, on the whole
			heading = normalized(add(heading, v3(0.0f, 3.0f * step / fmaxf(length, 0.5f), 0.0f)));
		}
		if (!leaning && shape != NULL && shape->trunk && shape->upright > 0.0f)
		{
			heading = normalized(add(heading, scale(direction, shape->upright * 1.5f * step)));
		}
		// Turned away from the branches that leave it here
		while (shape != NULL && kink < shape->kink_count && shape->kink_t[kink] <= (float)(i + 1) / (float)segments)
		{
			vec3 axis = rotated(perpendicular(heading), heading, shape->kink_azimuth[kink]);

			shape->kink_axes[kink] = axis;
			if (shape->kink_radians > 0.0f)
			{
				heading = normalized(rotated(heading, axis, -shape->kink_radians * random_between(0.5f, 1.0f)));
			}
			kink++;
		}
		// Never past hanging straight down
		if (heading.y < -0.97f)
		{
			heading = normalized(v3(heading.x, -0.97f, heading.z));
		}
		// Nor into the ground (a weeping willow's whips trail on it)
		if (point.y < 0.3f && heading.y < 0.0f && (shape == NULL || !shape->trunk))
		{
			heading = normalized(v3(heading.x, 0.0f, heading.z));
		}
		point = add(point, scale(heading, step));
	}
	// The axes of those past its end (none, normally)
	for (; shape != NULL && kink < shape->kink_count; kink++)
	{
		shape->kink_axes[kink] = rotated(perpendicular(heading), heading, shape->kink_azimuth[kink]);
	}
}

static void trace_branch(branch_t *branch, vec3 start, vec3 direction, float length, float radius, float taper, float bend,
	float reach, float segment_meters)
{
	trace_shaped_branch(branch, start, direction, length, radius, taper, bend, reach, segment_meters, NULL);
}

// Whether foliage grows at a point (kind's leaves_from): above it, always;
// under it only out at the branches' tips (the light reaches there), less
// and less the farther down
static bool foliage_here(const plant_t *plant, vec3 point)
{
	float from = plant->kind->leaves_from;
	float up;
	float out;
	float above;
	float tip;

	if (from <= 0.0f || plant->kind->shape != SHAPE_TREE)
	{
		return true;
	}
	up = point.y / fmaxf(plant->height, 0.1f);
	// Out from the crown's foot (a leaning trunk's crown is not over its root)
	out = sqrtf((point.x - plant->crown_center.x) * (point.x - plant->crown_center.x)
		+ (point.z - plant->crown_center.z) * (point.z - plant->crown_center.z)) / fmaxf(0.3f * plant->height, 0.05f);
	above = smooth_step(from - 0.04f, from + 0.04f, up);
	tip = smooth_step(0.55f, 0.85f, out) * smooth_step(from - 0.3f, from, up);
	return random_unit() < fmaxf(above, tip);
}

static void add_leaves(plant_t *plant, const branch_t *branch)
{
	int count = plant->kind->leaves_per_branch;

	for (int i = 0; i < count && plant->leaf_count > 0; i++)
	{
		float t = 0.25f + 0.75f * ((float)i + random_unit()) / (float)count;
		vec3 point;
		vec3 direction;
		float radius;
		vec3 out;
		vec3 along;
		vec3 across;

		branch_at(branch, t, &point, &direction, &radius);
		if (!foliage_here(plant, point))
		{
			continue;
		}
		// Out from the branch, round it at random, a little forward
		out = rotated(perpendicular(direction), direction, random_between(0.0f, 2.0f * PI_F));
		along = normalized(add(add(out, scale(direction, 0.6f)), v3(0.0f, random_between(-0.2f, 0.3f), 0.0f)));
		across = normalized(cross(along, random_direction()));
		add_picture(&plant->mesh, add(point, scale(out, radius)), along, across, plant->leaf_size * random_between(0.8f, 1.2f),
			crown_normal(plant, point), &plant->leaves[random_index(plant->leaf_count)]);
	}
}

// ---------------------------------------------------------------------------
// Fruit

// The radius of a fruit's solid at s (0: the end it hangs from or sits on,
// 1: the other), of its half width
static float fruit_profile(fruit_shape_t shape, float s)
{
	float middle = 2.0f * s - 1.0f;

	switch (shape)
	{
		case FRUIT_SPINDLE:
			// Fattest a little nearer the foot, pointed at the far end
			return powf(fmaxf(sinf(PI_F * powf(s, 0.8f)), 0.0f), 0.75f);
		case FRUIT_POD:
			return powf(fmaxf(sinf(PI_F * s), 0.0f), 0.4f);
		case FRUIT_SPHERE:
		default:
			return sqrtf(fmaxf(1.0f - middle * middle, 0.0f));
	}
}

// A fruit from foot along axis: a solid of revolution (a pod: flattened,
// and bent a little), its picture wrapped round it: each ring takes the
// picture's row at its height, as wide as the solid is there, so the
// picture's outline is the solid's (the back shows the picture mirrored).
// A card fruit: two crossed cards
static void add_fruit_body(plant_t *plant, vec3 foot, vec3 axis, float length, float width, bool hanging, bool on_stalk, const cell_place_t *place)
{
	// Small things, but round: about a hundred triangles each (a berry of
	// a few dozen looked a lumpy pentagon)
	const int rings = plant->fruit_shape == FRUIT_SPHERE ? 7 : 8;
	const int sides = plant->fruit_shape == FRUIT_SPHERE ? 8 : (width < 0.03f ? 5 : 6);
	vec3 side = perpendicular(axis);
	vec3 other = rotated(side, axis, 0.5f * PI_F);
	float flat = plant->fruit_shape == FRUIT_POD ? 0.3f : 1.0f;
	float bow = plant->fruit_shape == FRUIT_POD ? 0.12f * length : 0.0f;
	uint32_t first = (uint32_t)plant->mesh.vertex_count;

	// (one number for all of it, its stalk's if it has one: it shakes in
	// the wind as one, as much as its stalk's end, plant.vert)
	if (!on_stalk)
	{
		plant->mesh.plant_random = hash_unit((uint32_t)plant->mesh.vertex_count * 2654435761u + 53u);
	}
	plant->mesh.plant_along = 1.0f;

	if (plant->fruit_shape == FRUIT_CARD)
	{
		// The cards stand on the foot (foliage: lit as the leaves are); the
	// picture's top at the foot for a hanging one
		vec3 base = hanging ? add(foot, scale(axis, length)) : foot;
		vec3 up = hanging ? scale(axis, -1.0f) : axis;
		vec3 normal = crown_normal(plant, foot);

		add_card(&plant->mesh, base, scale(up, length), scale(side, width), normal, place);
		add_card(&plant->mesh, base, scale(up, length), scale(other, width), normal, place);
		return;
	}
	for (int i = 0; i <= rings; i++)
	{
		// (a sphere's rings evenly round it, not evenly up it: as many
		// near its poles as round its middle)
		float s_value = plant->fruit_shape == FRUIT_SPHERE ? 0.5f - 0.5f * cosf(PI_F * (float)i / (float)rings) : (float)i / (float)rings;
		float radius = 0.5f * width * fruit_profile(plant->fruit_shape, s_value);
		float slope = 0.5f * width * (fruit_profile(plant->fruit_shape, fminf(s_value + 0.02f, 1.0f))
			- fruit_profile(plant->fruit_shape, fmaxf(s_value - 0.02f, 0.0f))) / (0.04f * length);
		// The picture's row: from the foot's end of it
		float row = hanging ? place->uv[1] + (place->uv[3] - place->uv[1]) * s_value : place->uv[3] + (place->uv[1] - place->uv[3]) * s_value;
		float spread = width > 0.0f ? radius / (0.5f * width) : 0.0f;
		vec3 center = add(add(foot, scale(axis, length * s_value)), scale(other, bow * sinf(PI_F * s_value)));

		for (int k = 0; k <= sides; k++)
		{
			float angle = 2.0f * PI_F * (float)k / (float)sides;
			vec3 out = add(scale(side, cosf(angle)), scale(other, sinf(angle) * flat));
			vec3 normal = add(add(scale(side, cosf(angle) * flat), scale(other, sinf(angle))), scale(axis, -slope));
			float across = 0.5f + 0.45f * spread * sinf(angle);

			if (dot(normal, normal) < 1e-8f)
			{
				normal = i == 0 ? scale(axis, -1.0f) : axis;
			}
			add_vertex(&plant->mesh, add(center, scale(out, radius)), normal, cross(axis, out), axis,
				place->uv[0] + (place->uv[2] - place->uv[0]) * across, row);
		}
	}
	for (int i = 0; i < rings; i++)
	{
		for (int k = 0; k < sides; k++)
		{
			uint32_t a = first + (uint32_t)(i * (sides + 1) + k);
			uint32_t b = a + 1;
			uint32_t c = a + (uint32_t)(sides + 1);
			uint32_t d = c + 1;

			add_triangle(&plant->mesh, MATERIAL_FRUIT, a, b, c);
			add_triangle(&plant->mesh, MATERIAL_FRUIT, b, d, c);
		}
	}
}

// A fruit's stalk: a thin bark tube from a point along a direction
// A fruit's stalk: a thin tube from a point along a direction, the
// fruit's own (its material: it shakes in the wind with the fruit, from
// still at the branch to as much as the fruit at its end, plant.vert),
// colored from the middle of the fruit picture's top (its stalk's end)
// A piece of a fruit stalk, its shaking from along_start to along_end
// (0: still, at the branch; 1: as much as the fruit) with one random
// number (the shaking's own: those of a bunch shake together)
static void add_fruit_stalk_piece(plant_t *plant, vec3 start, vec3 direction, float length, float radius, const cell_place_t *place,
	float along_start, float along_end, float random_value)
{
	const int rings = 3;
	const int sides = 4;
	vec3 side = perpendicular(direction);
	vec3 other = cross(direction, side);
	float u = 0.5f * (place->uv[0] + place->uv[2]);
	float row = place->uv[1] + 0.01f * (place->uv[3] - place->uv[1]);
	uint32_t first = (uint32_t)plant->mesh.vertex_count;

	for (int i = 0; i <= rings; i++)
	{
		float t = (float)i / (float)rings;
		// (bending a little down at its end, under the fruit's weight)
		vec3 center = add(start, add(scale(direction, length * t), v3(0.0f, -0.15f * length * t * t, 0.0f)));

		plant->mesh.plant_along = along_start + (along_end - along_start) * t;
		plant->mesh.plant_random = random_value;
		for (int k = 0; k <= sides; k++)
		{
			float angle = 2.0f * PI_F * (float)k / (float)sides;
			vec3 out = add(scale(side, cosf(angle)), scale(other, sinf(angle)));

			add_vertex(&plant->mesh, add(center, scale(out, radius * (1.0f - 0.3f * t))), out, direction, cross(out, direction), u, row);
		}
	}
	for (int i = 0; i < rings; i++)
	{
		for (int k = 0; k < sides; k++)
		{
			uint32_t a = first + (uint32_t)(i * (sides + 1) + k);
			uint32_t b = a + 1;
			uint32_t c = a + (uint32_t)(sides + 1);
			uint32_t d = c + 1;

			add_triangle(&plant->mesh, MATERIAL_FRUIT, a, b, c);
			add_triangle(&plant->mesh, MATERIAL_FRUIT, b, d, c);
		}
	}
	plant->mesh.plant_along = 1.0f;
}

static void add_fruit_stalk(plant_t *plant, vec3 start, vec3 direction, float length, float radius, const cell_place_t *place)
{
	add_fruit_stalk_piece(plant, start, direction, length, radius, place, 0.0f, 1.0f,
		hash_unit((uint32_t)plant->mesh.vertex_count * 2654435761u + 53u));
}

// How likely fruit grows at a point of the crown (--fruit-zone)
static float fruit_zone_chance(const plant_t *plant, vec3 point)
{
	float height = fmaxf(plant->height, 0.1f);

	switch (plant->fruit_zone)
	{
		case FRUIT_TOP:
		{
			float up = point.y / height;

			return fminf(fmaxf((up - 0.55f) / 0.2f, 0.0f), 1.0f);
		}
		case FRUIT_OUTER:
		{
			float out = sqrtf((point.x - plant->crown_center.x) * (point.x - plant->crown_center.x)
				+ (point.z - plant->crown_center.z) * (point.z - plant->crown_center.z)) / (0.3f * height);

			return fminf(fmaxf((out - 0.3f) / 0.5f, 0.15f), 1.0f);
		}
		case FRUIT_ALL:
		default:
			return 1.0f;
	}
}

// A bunch of fruit at a point of a branch going along direction: a short
// stalk hanging from it (out along the branch a little, more down as
// bunch says), the fruit round it on their own short stalks, packed
// close, touching, none in another (each tried in a few places round it,
// the first that fits kept, or the one least in the others)
#define BUNCH_MAXIMUM 20

static void add_fruit_bunch(plant_t *plant, vec3 point, vec3 direction, int count)
{
	vec3 centers[BUNCH_MAXIMUM];
	float radii[BUNCH_MAXIMUM];
	float lengths[BUNCH_MAXIMUM];
	float widths[BUNCH_MAXIMUM];
	const cell_place_t *places[BUNCH_MAXIMUM];
	vec3 down = v3(0.0f, -1.0f, 0.0f);
	vec3 axis = normalized(add(scale(down, 0.4f + 0.6f * plant->fruit_bunch), scale(direction, 1.0f - 0.6f * plant->fruit_bunch)));
	float average_radius = 0.0f;
	float axis_length;
	float stalk_radius;
	float random_value = hash_unit((uint32_t)plant->mesh.vertex_count * 2654435761u + 71u);

	count = count > BUNCH_MAXIMUM ? BUNCH_MAXIMUM : count;
	for (int n = 0; n < count; n++)
	{
		float size = random_between(0.85f, 1.15f);

		places[n] = &plant->fruits[random_index(plant->fruit_count)];
		lengths[n] = plant->fruit_length * size;
		widths[n] = (plant->fruit_width > 0.0f ? plant->fruit_width : plant->fruit_length * places[n]->aspect) * size;
		radii[n] = 0.5f * fmaxf(lengths[n], widths[n]);
		average_radius += radii[n] / (float)count;
	}
	// (its axis as long as the fruit need round it, a bit over)
	axis_length = average_radius * (0.5f + 0.45f * (float)count) + plant->fruit_stalk;
	stalk_radius = fmaxf(0.0012f, 0.035f * 2.0f * average_radius);
	add_fruit_stalk_piece(plant, point, axis, axis_length, stalk_radius, places[0], 0.0f, 0.5f, random_value);
	for (int n = 0; n < count; n++)
	{
		vec3 best_center = point;
		vec3 best_attach = point;
		float best_overlap = 1e9f;
		// (its own stalk: the bunch's stalk length, short)
		float pedicel = fmaxf(plant->fruit_stalk * 0.6f, radii[n] * 0.4f);

		for (int attempt = 0; attempt < 32; attempt++)
		{
			// (along the axis, the first at its end; round it; out from it)
			float along = n == 0 ? 1.0f : random_between(0.25f, 1.0f);
			vec3 attach = add(point, add(scale(axis, axis_length * along), v3(0.0f, -0.15f * axis_length * along * along, 0.0f)));
			vec3 out = rotated(perpendicular(axis), axis, random_between(0.0f, 2.0f * PI_F));
			// (hanging: out from the axis and down, the end one along it)
			vec3 hang = n == 0 ? normalized(add(axis, scale(down, 0.5f))) : normalized(add(scale(out, 0.9f), scale(down, 0.7f)));
			vec3 center = add(attach, scale(hang, pedicel + radii[n]));
			float overlap = 0.0f;

			for (int m = 0; m < n; m++)
			{
				vec3 apart = sub(center, centers[m]);
				float gap = sqrtf(dot(apart, apart)) - 0.97f * (radii[n] + radii[m]);

				overlap += gap < 0.0f ? -gap : 0.0f;
			}
			if (overlap < best_overlap)
			{
				best_overlap = overlap;
				best_center = center;
				best_attach = attach;
			}
			if (overlap <= 0.0f)
			{
				break;
			}
		}
		centers[n] = best_center;
		{
			vec3 hang = normalized(sub(best_center, best_attach));
			vec3 foot = sub(best_center, scale(hang, lengths[n] * 0.5f));
			vec3 reach = sub(foot, best_attach);
			float reach_length = sqrtf(dot(reach, reach));

			// (its stalk from the axis to its foot: aimed a little above
			// it, for the stalk's droop)
			if (reach_length > 1e-4f)
			{
				vec3 aim = add(reach, v3(0.0f, 0.15f * reach_length, 0.0f));

				add_fruit_stalk_piece(plant, best_attach, normalized(aim), reach_length, stalk_radius * 0.8f, places[n], 0.5f, 1.0f, random_value);
			}
			plant->mesh.plant_random = random_value;
			add_fruit_body(plant, foot, hang, lengths[n], widths[n], true, true, places[n]);
			plant->fruits_grown++;
		}
	}
}

// A cluster of fruit at a point of a branch going along direction (its
// radius there)
static void add_fruit_cluster(plant_t *plant, vec3 point, vec3 direction, float branch_radius)
{
	int count = plant->cluster_minimum + random_index(plant->cluster_maximum - plant->cluster_minimum + 1);
	float azimuth = random_between(0.0f, 2.0f * PI_F);

	if (plant->fruit_bunch > 0.0f && count > 1)
	{
		add_fruit_bunch(plant, point, direction, count);
		return;
	}
	for (int n = 0; n < count; n++)
	{
		const cell_place_t *place = &plant->fruits[random_index(plant->fruit_count)];
		float size = random_between(0.85f, 1.15f);
		float length = plant->fruit_length * size;
		float width = (plant->fruit_width > 0.0f ? plant->fruit_width : plant->fruit_length * place->aspect) * size;
		// Each of a cluster leans its own way
		vec3 lean = rotated(perpendicular(direction), direction, azimuth + 2.0f * PI_F * (float)n / (float)(count > 0 ? count : 1));
		vec3 axis;
		vec3 foot = point;

		switch (plant->fruit_hang)
		{
			case FRUIT_UP:
				axis = normalized(add(v3(0.0f, 1.0f, 0.0f), scale(lean, count > 1 ? 0.35f : 0.12f)));
				foot = add(point, scale(axis, branch_radius * 0.5f));
				break;
			case FRUIT_OUT:
			{
				vec3 out = rotated(perpendicular(direction), direction, azimuth + 2.0f * PI_F * (float)n / (float)(count > 0 ? count : 1));

				axis = normalized(add(out, scale(direction, 0.4f)));
				foot = add(point, scale(out, branch_radius));
				break;
			}
			case FRUIT_DOWN:
			default:
				axis = normalized(add(v3(0.0f, -1.0f, 0.0f), scale(lean, count > 1 ? 0.45f : 0.15f)));
				break;
		}
		if (plant->fruit_stalk > 0.0f)
		{
			float stalk = plant->fruit_stalk * random_between(0.8f, 1.2f);

			add_fruit_stalk(plant, foot, axis, stalk, fmaxf(0.0012f, 0.04f * width), place);
			foot = add(add(foot, scale(axis, stalk)), v3(0.0f, -0.15f * stalk, 0.0f));
		}
		add_fruit_body(plant, foot, axis, length, width, plant->fruit_hang == FRUIT_DOWN, plant->fruit_stalk > 0.0f, place);
		plant->fruits_grown++;
	}
}

// Fruit on a finest branch or a twig (from start along direction, this
// long): --fruit-per-branch clusters on average, where the zone has them,
// on its outer part
static void fruit_along(plant_t *plant, vec3 start, vec3 direction, float length, float radius)
{
	float expected;
	int count;

	if (plant->fruit_count == 0 || plant->fruit_per_branch <= 0.0f)
	{
		return;
	}
	expected = plant->fruit_per_branch * fruit_zone_chance(plant, add(start, scale(direction, length * 0.7f)));
	count = (int)expected + (random_unit() < expected - floorf(expected) ? 1 : 0);
	for (int i = 0; i < count; i++)
	{
		// (towards its tip as fruit_tip says: at 1, on its last tenth)
		vec3 point = add(start, scale(direction, length * random_between(0.4f + 0.5f * plant->fruit_tip, 0.95f + 0.05f * plant->fruit_tip)));

		if (foliage_here(plant, point))
		{
			add_fruit_cluster(plant, point, direction, radius);
		}
	}
}

// Leaves along a branch that is not a finest one (--loose-leaves): they
// break the outline twigs would leave, and fill the crown's inside
static void add_loose_leaves(plant_t *plant, const branch_t *branch)
{
	int count = (int)(plant->loose_leaves * branch->length + random_unit());

	for (int i = 0; i < count && plant->leaf_count > 0; i++)
	{
		float t = 0.15f + 0.85f * random_unit();
		vec3 point;
		vec3 direction;
		float radius;
		vec3 out;
		vec3 along;

		branch_at(branch, t, &point, &direction, &radius);
		if (!foliage_here(plant, point))
		{
			continue;
		}
		out = rotated(perpendicular(direction), direction, random_between(0.0f, 2.0f * PI_F));
		along = normalized(add(add(out, scale(direction, 0.5f)), v3(0.0f, random_between(-0.2f, 0.3f), 0.0f)));
		add_picture(&plant->mesh, add(point, scale(out, radius)), along, normalized(cross(along, random_direction())),
			plant->leaf_size * random_between(0.8f, 1.2f), crown_normal(plant, point), &plant->leaves[random_index(plant->leaf_count)]);
	}
}

// Its grid: this many quads along it, across it
static const int CURVED_TWIG_ALONG = 4;
static const int CURVED_TWIG_ACROSS = 3;

// A twig as one card (kind twig_curved), a grid: arched across its width
// (its middle up, a fir's spray seen end on), its tip drooping, twisted a
// little along it; so seen edge on it still shows (two crossed cards are
// what a flat one needs for that). Lit with normal, as every card
static void add_curved_twig(mesh_t *mesh, vec3 base, vec3 along_unit, vec3 across_unit, float length, vec3 normal,
	const cell_place_t *place)
{
	float width = length * place->aspect;
	vec3 up_face = normalized(cross(along_unit, across_unit));
	float arch = width * random_between(0.10f, 0.18f);
	float droop = length * random_between(0.08f, 0.2f);
	float twist = random_between(-0.25f, 0.25f);
	uint32_t first = (uint32_t)mesh->vertex_count;
	int columns = CURVED_TWIG_ACROSS + 1;

	// (its face up: the arch's middle goes up)
	if (up_face.y < 0.0f)
	{
		up_face = scale(up_face, -1.0f);
		across_unit = scale(across_unit, -1.0f);
	}
	mesh->plant_random = hash_unit((uint32_t)mesh->vertex_count * 2654435761u + 17u);
	mesh->plant_extra = 0.0f;
	for (int row = 0; row <= CURVED_TWIG_ALONG; row++)
	{
		float t = (float)row / (float)CURVED_TWIG_ALONG;
		float angle = twist * t;
		vec3 side = rotated(across_unit, along_unit, angle);
		vec3 face = rotated(up_face, along_unit, angle);
		// (the droop grows to its tip; the arch flattens a little there)
		vec3 spine = add(add(base, scale(along_unit, length * t)), v3(0.0f, -droop * t * t, 0.0f));

		mesh->plant_along = t;
		for (int column = 0; column < columns; column++)
		{
			float s = (float)column / (float)CURVED_TWIG_ACROSS;
			float from_stem = s - place->anchor;
			float centered = 2.0f * s - 1.0f;
			float lift = arch * (1.0f - centered * centered) * (1.0f - 0.4f * t);
			vec3 position = add(add(spine, scale(side, width * from_stem)), scale(face, lift));

			add_vertex(mesh, position, normal, side, scale(along_unit, -1.0f), place->uv[0] + (place->uv[2] - place->uv[0]) * s,
				place->uv[3] + (place->uv[1] - place->uv[3]) * t);
		}
	}
	mesh->plant_along = 0.0f;
	for (int row = 0; row < CURVED_TWIG_ALONG; row++)
	{
		for (int column = 0; column < CURVED_TWIG_ACROSS; column++)
		{
			uint32_t a = first + (uint32_t)(row * columns + column);
			uint32_t b = a + 1;
			uint32_t c = b + (uint32_t)columns;
			uint32_t d = a + (uint32_t)columns;

			add_triangle(mesh, MATERIAL_FOLIAGE, a, b, c);
			add_triangle(mesh, MATERIAL_FOLIAGE, a, c, d);
		}
	}
	shade_vertices(mesh, first, (uint32_t)((CURVED_TWIG_ALONG + 1) * columns), random_between(0.8f, 1.12f));
}

// One needle cone (kind brushes): a whorl of needles round a shoot, the
// shoot through its middle: narrow strips (the twig picture: a few needles
// side by side, their feet at the bottom) from the shoot's surface, opening
// forward and out (spread from the shoot's way), needle_length long, all
// round it, so together they make an open cone. Strips, not a skin cut in
// sectors: each keeps its picture's shape (a sector's trapezoid bent every
// needle at its diagonal), and the gaps between them widen outwards as
// between real needles. Each a little off in its angles and length
static void add_needle_cone(plant_t *plant, vec3 foot, vec3 axis, float shoot_radius, float needle_length, float spread, vec3 normal)
{
	int strips = plant->kind->brush_strips;
	vec3 u = rotated(perpendicular(axis), axis, random_between(0.0f, 2.0f * PI_F));
	vec3 w = cross(axis, u);
	int order[32];
	bool taken[32];

	// The order the levels of detail thin them in: each next the farthest
	// round from those before it, so that any first share of them is
	// spread evenly round the shoot (not all gone from one side)
	memset(taken, 0, sizeof(taken));
	order[0] = 0;
	taken[0] = true;
	for (int k = 1; k < strips; k++)
	{
		int best = 0;
		int best_distance = -1;

		for (int i = 0; i < strips; i++)
		{
			int nearest = strips;

			if (taken[i])
			{
				continue;
			}
			for (int j = 0; j < k; j++)
			{
				int apart = abs(i - order[j]);

				apart = apart < strips - apart ? apart : strips - apart;
				nearest = apart < nearest ? apart : nearest;
			}
			if (nearest > best_distance)
			{
				best_distance = nearest;
				best = i;
			}
		}
		order[k] = best;
		taken[best] = true;
	}
	for (int k = 0; k < strips; k++)
	{
		int i = order[k];
		const cell_place_t *place = &plant->twigs[random_index(plant->twig_count)];
		float angle = ((float)i + random_between(-0.3f, 0.3f)) / (float)strips * 2.0f * PI_F;
		vec3 out = add(scale(u, cosf(angle)), scale(w, sinf(angle)));
		float here_spread = spread + random_between(-0.12f, 0.12f);
		float length = needle_length * random_between(0.85f, 1.05f);
		vec3 along = normalized(add(scale(axis, cosf(here_spread)), scale(out, sinf(here_spread))));
		vec3 across = cross(axis, out);

		plant->mesh.foliage_rank = (uint8_t)k;
		add_card(&plant->mesh, add(foot, scale(out, shoot_radius)), scale(along, length), scale(across, length * place->aspect), normal, place);
	}
	plant->mesh.foliage_rank = 0;
}

// One shoot with its needles (kind brushes): a thin bark tube, bare low
// down, its needles in cones along its upper part, those nearer its tip
// pointing more forward (closing round the bud)
static void add_brush(plant_t *plant, vec3 foot, vec3 axis, float length, vec3 normal)
{
	branch_t shoot;
	float radius = fmaxf(length * 0.02f, 0.002f);
	float needle_length = length * 0.32f;
	float needles_from = 0.38f;
	uint8_t level = plant->mesh.bark_level;
	int cones;

	trace_branch(&shoot, foot, axis, length, radius, 0.5f, random_between(0.05f, 0.2f), 0.0f, length / 3.0f);
	plant->mesh.bark_level = BARK_LEVEL_STALK;
	add_tube(plant, &shoot, 4, radius, MATERIAL_BARK);
	plant->mesh.bark_level = level;
	// (a cone each brush_spacing of a needle's length, half by default:
	// gaps between, the shoot showing through, as a pine's)
	cones = (int)ceilf((1.0f - needles_from) * length / (needle_length * plant->kind->brush_spacing));
	cones = cones < 2 ? 2 : (cones > 16 ? 16 : cones);
	for (int i = 0; i < cones; i++)
	{
		float t = needles_from + (0.97f - needles_from) * ((float)i + random_between(0.0f, 0.4f)) / (float)cones;
		float near_tip = (float)i / (float)(cones - 1);
		vec3 point;
		vec3 heading;
		float here_radius;

		branch_at(&shoot, t, &point, &heading, &here_radius);
		add_needle_cone(plant, point, heading, here_radius, needle_length * (1.0f - 0.25f * near_tip * near_tip),
			random_between(0.8f, 1.0f) * (1.0f - 0.4f * near_tip * near_tip), normal);
	}
}

// A twig as a spray of shoots (kind brushes): its leader along it, and
// shoots out of it round about (by the golden angle), the lower longer, all
// turning up at their ends (a pine's candles), each a brush
static void add_brush_twig(plant_t *plant, vec3 start, vec3 direction, float length, vec3 normal)
{
	int shoots = 1 + random_index(3);
	float turn = random_between(0.0f, 2.0f * PI_F);
	vec3 across;

	direction = normalized(add(direction, v3(0.0f, 0.35f, 0.0f)));
	across = perpendicular(direction);
	add_brush(plant, start, direction, length, normal);
	for (int i = 0; i < shoots; i++)
	{
		float t = 0.25f + 0.35f * ((float)i + random_between(0.0f, 0.6f)) / (float)shoots;
		float out_angle = random_between(0.55f, 0.9f);
		vec3 out = rotated(across, direction, turn + (float)i * 2.39996f);
		vec3 heading = normalized(add(add(scale(direction, cosf(out_angle)), scale(out, sinf(out_angle))), v3(0.0f, 0.3f, 0.0f)));

		add_brush(plant, add(start, scale(direction, length * t)), heading, length * random_between(0.5f, 0.68f) * (1.15f - 0.5f * t), normal);
	}
}

static void add_twig(plant_t *plant, vec3 start, vec3 direction, float length)
{
	// Two cards crossed, so it shows from every side; the flatter the kind,
	// the more both lie level (a shallow V seen end on), the way fir sprays
	// spread
	float flat = plant->kind->twig_flat;
	vec3 level = v3(direction.x, 0.0f, direction.z);

	// None low down in the shade (leaves_from)
	if (!foliage_here(plant, add(start, scale(direction, length * 0.5f))))
	{
		return;
	}
	vec3 side;
	vec3 other;
	vec3 normal;
	const cell_place_t *place = &plant->twigs[random_index(plant->twig_count)];
	float between;

	if (flat > 0.0f && dot(level, level) > 1e-4f)
	{
		direction = normalized(add(scale(direction, 1.0f - 0.7f * flat), scale(normalized(level), 0.7f * flat)));
		side = rotated(normalized(cross(v3(0.0f, 1.0f, 0.0f), direction)), direction, random_between(-0.3f, 0.3f) * (1.0f - flat));
	}
	else
	{
		side = rotated(perpendicular(direction), direction, random_between(0.0f, PI_F));
	}
	between = (0.5f * PI_F) * (1.0f - flat) + 0.35f * flat;
	other = rotated(side, direction, between);
	length *= plant->kind->twig_scale;
	normal = crown_normal(plant, add(start, scale(direction, length * 0.5f)));
	if (plant->kind->brushes)
	{
		add_brush_twig(plant, start, direction, length, normal);
		fruit_along(plant, start, direction, length * 0.8f, 0.004f);
		return;
	}
	if (plant->kind->twig_curved)
	{
		// One card: of the two, the one lying the flattest
		vec3 side_face = cross(direction, side);
		vec3 other_face = cross(direction, other);

		add_curved_twig(&plant->mesh, start, direction, fabsf(side_face.y) >= fabsf(other_face.y) ? side : other, length, normal, place);
	}
	else
	{
		add_picture(&plant->mesh, start, direction, side, length, normal, place);
		add_picture(&plant->mesh, start, direction, other, length, normal, place);
	}
	fruit_along(plant, start, direction, length * 0.8f, 0.004f);
}

// Its grid: this many quads along a bough, across it
static const int BRANCH_CARD_ALONG = 8;
static const int BRANCH_CARD_ACROSS = 4;
// How far its middle is arched up, of its width
static const float BRANCH_CARD_ARCH = 0.12f;

// Where a bough's card is at t along it (0 its foot .. 1 its end; the card
// card_length long, on the branch and past its tip straight on): the
// point on its middle line, the way it goes, level across it, its face (up)
static void bough_frame(const branch_t *branch, float card_length, float t, vec3 *spine_out, vec3 *direction_out, vec3 *side_out,
	vec3 *face_out)
{
	float along_meters = t * card_length;
	float unused_radius;
	vec3 side;

	if (along_meters <= branch->length || branch->length <= 1e-4f)
	{
		branch_at(branch, branch->length > 1e-4f ? along_meters / branch->length : 0.0f, spine_out, direction_out, &unused_radius);
	}
	else
	{
		*direction_out = branch->directions[branch->count - 1];
		*spine_out = add(branch->points[branch->count - 1], scale(*direction_out, along_meters - branch->length));
	}
	side = cross(v3(0.0f, 1.0f, 0.0f), *direction_out);
	side = dot(side, side) > 1e-4f ? normalized(side) : perpendicular(*direction_out);
	*side_out = side;
	*face_out = normalized(cross(*direction_out, side));
}

// A point of a bough's card at s across it (0 .. 1 of the picture's width,
// its stem at anchor), t along it, lifted along its face by offset; face_out:
// its face there
static vec3 bough_card_point(const branch_t *branch, float card_length, float width, float anchor, float offset, float s, float t,
	vec3 *face_out)
{
	vec3 spine;
	vec3 direction;
	vec3 side;
	vec3 face;
	float centered = 2.0f * s - 1.0f;
	float lift = width * BRANCH_CARD_ARCH * (1.0f - centered * centered) + offset;

	bough_frame(branch, card_length, t, &spine, &direction, &side, &face);
	if (face_out != NULL)
	{
		*face_out = face;
	}
	return add(add(spine, scale(side, width * (s - anchor))), scale(face, lift));
}

// One layer of a bough's card: along the branch from its foot (the
// picture's stem at the trunk) on past its tip (the picture this many
// times as long as the branch), across it level, arched up in its middle;
// lifted along its face by offset (the layer under it: below 0); mirrored:
// the picture turned over (the one under, seen from below)
static void add_branch_card_layer(plant_t *plant, const branch_t *branch, float card_length, const cell_place_t *place, float offset,
	bool mirrored)
{
	mesh_t *mesh = &plant->mesh;
	float width = card_length * place->aspect;
	uint32_t first = (uint32_t)mesh->vertex_count;
	int columns = BRANCH_CARD_ACROSS + 1;

	mesh->plant_random = hash_unit((uint32_t)mesh->vertex_count * 2654435761u + 17u);
	mesh->plant_extra = 0.0f;
	for (int row = 0; row <= BRANCH_CARD_ALONG; row++)
	{
		float t = (float)row / (float)BRANCH_CARD_ALONG;
		vec3 spine;
		vec3 direction;
		vec3 side;
		vec3 face;

		bough_frame(branch, card_length, t, &spine, &direction, &side, &face);
		mesh->plant_along = t;
		for (int column = 0; column < columns; column++)
		{
			float s = (float)column / (float)BRANCH_CARD_ACROSS;
			vec3 position = bough_card_point(branch, card_length, width, place->anchor, offset, s, t, NULL);
			float u = mirrored ? 1.0f - s : s;

			add_vertex(mesh, position, crown_normal(plant, position), side, scale(direction, -1.0f),
				place->uv[0] + (place->uv[2] - place->uv[0]) * u, place->uv[3] + (place->uv[1] - place->uv[3]) * t);
		}
	}
	mesh->plant_along = 0.0f;
	for (int row = 0; row < BRANCH_CARD_ALONG; row++)
	{
		for (int column = 0; column < BRANCH_CARD_ACROSS; column++)
		{
			uint32_t a = first + (uint32_t)(row * columns + column);
			uint32_t b = a + 1;
			uint32_t c = b + (uint32_t)columns;
			uint32_t d = a + (uint32_t)columns;

			add_triangle(mesh, MATERIAL_FOLIAGE, a, b, c);
			add_triangle(mesh, MATERIAL_FOLIAGE, a, c, d);
		}
	}
	shade_vertices(mesh, first, (uint32_t)((BRANCH_CARD_ALONG + 1) * columns), random_between(0.85f, 1.1f));
}

// A bough (kind branch_cards): its bark (seen from under it), and its
// picture along it in two layers a few centimeters apart: one seen from
// above, under it another (the picture turned over, another one if there
// are more), seen from below; between them, seen edge on, the bough has
// some thickness. Kept (plant->boughs) for the snow (bake_snow)
static void add_branch_card(plant_t *plant, const branch_t *branch, int sides, float radius)
{
	float card_length = branch->length * plant->kind->twig_scale;
	int top = random_index(plant->twig_count);
	int under = plant->twig_count > 1 ? (top + 1 + random_index(plant->twig_count - 1)) % plant->twig_count : top;
	float gap = fminf(fmaxf(card_length * 0.015f, 0.02f), 0.06f);
	bough_t *bough;

	plant->mesh.bark_level = 1;
	// A bare one (low in the shade): a dead stub, half as long, not sticking
	// out past the living ones round it
	if (!foliage_here(plant, branch->points[branch->count / 2]))
	{
		branch_t stub = *branch;

		stub.count = stub.count / 2 > 2 ? stub.count / 2 : 2;
		stub.length *= (float)(stub.count - 1) / (float)(branch->count - 1 > 0 ? branch->count - 1 : 1);
		add_tube(plant, &stub, sides, radius, MATERIAL_BARK);
		return;
	}
	if (plant->bough_count == plant->bough_capacity)
	{
		plant->bough_capacity = plant->bough_capacity > 0 ? plant->bough_capacity * 2 : 64;
		plant->boughs = checked_realloc(plant->boughs, sizeof(bough_t) * (size_t)plant->bough_capacity);
	}
	bough = &plant->boughs[plant->bough_count++];
	memset(bough, 0, sizeof(*bough));
	bough->branch = *branch;
	bough->card_length = card_length;
	bough->place = plant->twigs[top];
	bough->width = card_length * bough->place.aspect;
	bough->top_offset = 0.5f * gap;
	bough->gap = gap;
	bough->first_vertex = (uint32_t)plant->mesh.vertex_count;
	add_tube(plant, branch, sides, radius, MATERIAL_BARK);
	bough->top_first = (uint32_t)plant->mesh.vertex_count;
	add_branch_card_layer(plant, branch, card_length, &plant->twigs[top], 0.5f * gap, false);
	add_branch_card_layer(plant, branch, card_length, &plant->twigs[under], -0.5f * gap, true);
	fruit_along(plant, branch->points[branch->count / 2], branch->directions[branch->count / 2], branch->length * 0.5f, 0.004f);
	bough->vertex_end = (uint32_t)plant->mesh.vertex_count;
}

// ---------------------------------------------------------------------------
// The winter's snow baked in (kind branch_cards: whole boughs; object_format.h,
// OBJECT_FILE_SNOW). Snow falls straight down on the tree, a path every
// SNOW_RAY_SPACING: where it meets a bough's top card on its picture (not
// through the gaps between its shoots), most of it stays, the more the
// flatter the card; what is left falls on to the next one down. What each
// bough caught, smoothed (it bridges the gaps), is the snow lying on it, a
// mesh of its own over its card. Each bough sags under its load (and under
// what rests on it from the one above), the more the farther out (as a
// beam held at one end); where its end would go into the snow on the one
// below, it rests on it instead, and that one carries some of its load. A
// few rounds of that, top down, so they settle on one another. Not a
// physical simulation: a model tuned to look right (the sag: SNOW_BEND of a
// bough's length under SNOW_BEND_LOAD kilograms a meter, the kind's
// snow_sag over the fir's 0.06 times that)

static const float SNOW_RAY_SPACING = 0.025f;   // meters between the snow's paths down
static const float SNOW_CATCH = 0.55f;          // of what reaches a card's needles, what stays there
static const float SNOW_DEPTH = 0.13f;          // meters on a bough that caught all that fell on it (in lumps: some more, some less)
static const float SNOW_DENSITY = 150.0f;       // kilograms a cubic meter (fresh, a little settled)
static const float SNOW_BEND_LOAD = 15.0f;      // kilograms a meter of bough under which it sags
static const float SNOW_BEND = 0.3f;            // this much of its length (at its end)
static const float SNOW_MOST_BEND = 0.5f;       // of its length at the most
static const float SNOW_LIFT = 0.008f;          // the snow's foot over its card (meters)
static const float SNOW_CLEARANCE = 0.01f;      // a bough resting on the snow of the one below: this far over it
static const int SNOW_ROUNDS = 4;

// How far along a bough bends (0 its foot .. 1 its end), of how far its end
// does: a beam held at one end under a load all along it; past its end
// (the card goes on) straight on
static float snow_sag_shape(float x)
{
	if (x <= 0.0f)
	{
		return 0.0f;
	}
	if (x <= 1.0f)
	{
		return (6.0f * x * x - 4.0f * x * x * x + x * x * x * x) / 3.0f;
	}
	return 1.0f + (4.0f / 3.0f) * (x - 1.0f);
}

// How far along its branch (0 .. 1, past 1 beyond its tip) a point of a
// bough is, by the branch's chord from its foot
static float bough_along(const bough_t *bough, vec3 point)
{
	vec3 foot = bough->branch.points[0];
	vec3 chord = sub(bough->branch.points[bough->branch.count - 1], foot);
	float length = sqrtf(dot(chord, chord));

	if (length < 1e-4f)
	{
		return 0.0f;
	}
	return fmaxf(dot(sub(point, foot), chord) / (length * length), 0.0f);
}

// Where a point of a bough goes under all of its snow: down by its sag
// there, and in a little towards its foot (it bends, it does not stretch)
static vec3 bough_sag_offset(const bough_t *bough, vec3 point)
{
	float x = bough_along(bough, point);
	float down = bough->drop * snow_sag_shape(x);
	vec3 chord = sub(bough->branch.points[bough->branch.count - 1], bough->branch.points[0]);
	vec3 level = v3(chord.x, 0.0f, chord.z);
	float level_length = sqrtf(dot(level, level));
	float pull = down * down / (2.0f * fmaxf(x * bough->branch.length, 0.3f));
	vec3 offset = v3(0.0f, -down, 0.0f);

	if (level_length > 1e-4f)
	{
		offset = add(offset, scale(level, -fminf(pull, 0.5f * down) / level_length));
	}
	return offset;
}

// The atlas' alpha at u, v (0..1, v from its top)
static float atlas_alpha(const atlas_t *atlas, float u, float v)
{
	int x = (int)(u * (float)atlas->size);
	int y = (int)(v * (float)atlas->size);

	x = x < 0 ? 0 : (x >= atlas->size ? atlas->size - 1 : x);
	y = y < 0 ? 0 : (y >= atlas->size ? atlas->size - 1 : y);
	return (float)atlas->pixels[((size_t)y * (size_t)atlas->size + (size_t)x) * 4 + 3] / 255.0f;
}

// Where a path straight down at x, z meets a bough's top card (its
// vertices as they are, each moved by moved[] if given: index from the
// card's first), the highest place: false if it does not. Its height there,
// and where on the card (s across, t along; 0..1), the atlas there, how
// level the card is there (its face's up, 0..1)
typedef struct
{
	float height;
	float s;
	float t;
	float u;
	float v;
	float level;
} card_hit_t;

static bool bough_card_hit(const mesh_t *mesh, const bough_t *bough, const vec3 *moved, float x, float z, card_hit_t *hit)
{
	int columns = BRANCH_CARD_ACROSS + 1;
	bool found = false;

	for (int row = 0; row < BRANCH_CARD_ALONG; row++)
	{
		for (int column = 0; column < BRANCH_CARD_ACROSS; column++)
		{
			int corner[4] = { row * columns + column, row * columns + column + 1, (row + 1) * columns + column + 1, (row + 1) * columns + column };
			float corner_s[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
			float corner_t[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
			vec3 point[4];

			for (int k = 0; k < 4; k++)
			{
				const float *position = mesh->vertices[bough->top_first + (uint32_t)corner[k]].position;

				point[k] = v3(position[0], position[1], position[2]);
				if (moved != NULL)
				{
					point[k] = add(point[k], moved[corner[k]]);
				}
			}
			for (int half = 0; half < 2; half++)
			{
				int a = 0;
				int b = half == 0 ? 1 : 2;
				int c = half == 0 ? 2 : 3;
				// (barycentric, in x, z)
				float denominator = (point[b].z - point[c].z) * (point[a].x - point[c].x) + (point[c].x - point[b].x) * (point[a].z - point[c].z);
				float wa;
				float wb;
				float wc;
				float height;

				if (fabsf(denominator) < 1e-9f)
				{
					continue;
				}
				wa = ((point[b].z - point[c].z) * (x - point[c].x) + (point[c].x - point[b].x) * (z - point[c].z)) / denominator;
				wb = ((point[c].z - point[a].z) * (x - point[c].x) + (point[a].x - point[c].x) * (z - point[c].z)) / denominator;
				wc = 1.0f - wa - wb;
				if (wa < 0.0f || wb < 0.0f || wc < 0.0f)
				{
					continue;
				}
				height = wa * point[a].y + wb * point[b].y + wc * point[c].y;
				if (!found || height > hit->height)
				{
					const object_file_vertex_t *va = &mesh->vertices[bough->top_first + (uint32_t)corner[a]];
					const object_file_vertex_t *vb = &mesh->vertices[bough->top_first + (uint32_t)corner[b]];
					const object_file_vertex_t *vc = &mesh->vertices[bough->top_first + (uint32_t)corner[c]];
					vec3 face = normalized(cross(sub(point[b], point[a]), sub(point[c], point[a])));

					found = true;
					hit->height = height;
					hit->s = ((float)column + wa * corner_s[a] + wb * corner_s[b] + wc * corner_s[c]) / (float)BRANCH_CARD_ACROSS;
					hit->t = ((float)row + wa * corner_t[a] + wb * corner_t[b] + wc * corner_t[c]) / (float)BRANCH_CARD_ALONG;
					hit->u = wa * va->uv[0] + wb * vb->uv[0] + wc * vc->uv[0];
					hit->v = wa * va->uv[1] + wb * vb->uv[1] + wc * vc->uv[1];
					hit->level = fabsf(face.y);
				}
			}
		}
	}
	return found;
}

// The snow's depth on a bough at s, t (0..1 of its card), between its cells
static float bough_snow_depth(const bough_t *bough, float s, float t)
{
	float x = s * (float)SNOW_CELLS_ACROSS - 0.5f;
	float y = t * (float)SNOW_CELLS_ALONG - 0.5f;
	int x0 = (int)floorf(x);
	int y0 = (int)floorf(y);
	float fx = x - (float)x0;
	float fy = y - (float)y0;
	float sum = 0.0f;

	for (int k = 0; k < 4; k++)
	{
		int cx = x0 + (k & 1);
		int cy = y0 + (k >> 1);
		float weight = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);

		if (cx >= 0 && cy >= 0 && cx < SNOW_CELLS_ACROSS && cy < SNOW_CELLS_ALONG)
		{
			sum += weight * bough->depth[cy][cx];
		}
	}
	return sum;
}

// A bough's box seen from above (its top card's)
static void bough_box(const mesh_t *mesh, const bough_t *bough, float box[4])
{
	uint32_t count = (uint32_t)((BRANCH_CARD_ALONG + 1) * (BRANCH_CARD_ACROSS + 1));

	box[0] = box[2] = mesh->vertices[bough->top_first].position[0];
	box[1] = box[3] = mesh->vertices[bough->top_first].position[2];
	for (uint32_t v = bough->top_first; v < bough->top_first + count; v++)
	{
		box[0] = fminf(box[0], mesh->vertices[v].position[0]);
		box[2] = fmaxf(box[2], mesh->vertices[v].position[0]);
		box[1] = fminf(box[1], mesh->vertices[v].position[2]);
		box[3] = fmaxf(box[3], mesh->vertices[v].position[2]);
	}
}

static const plant_t *sorting_plant;

// Higher feet first
static int compare_bough_heights(const void *a, const void *b)
{
	float height_a = sorting_plant->boughs[*(const int *)a].branch.points[0].y;
	float height_b = sorting_plant->boughs[*(const int *)b].branch.points[0].y;

	return height_a > height_b ? -1 : (height_a < height_b ? 1 : 0);
}

// The snow falling: what each bough catches (its depth grid, its load)
static void snow_fall(plant_t *plant, const atlas_t *atlas, float (*boxes)[4])
{
	mesh_t *mesh = &plant->mesh;
	float minimum_x = mesh->minimum.x;
	float minimum_z = mesh->minimum.z;
	int steps_x = (int)((mesh->maximum.x - mesh->minimum.x) / SNOW_RAY_SPACING) + 1;
	int steps_z = (int)((mesh->maximum.z - mesh->minimum.z) / SNOW_RAY_SPACING) + 1;
	float ray_area = SNOW_RAY_SPACING * SNOW_RAY_SPACING;
	typedef struct
	{
		card_hit_t hit;
		int bough;
	} fall_hit_t;
	fall_hit_t *hits = checked_realloc(NULL, sizeof(fall_hit_t) * (size_t)plant->bough_count);

	for (int iz = 0; iz < steps_z; iz++)
	{
		for (int ix = 0; ix < steps_x; ix++)
		{
			// (a little off the grid: no rows of snow)
			float x = minimum_x + ((float)ix + random_between(0.2f, 0.8f)) * SNOW_RAY_SPACING;
			float z = minimum_z + ((float)iz + random_between(0.2f, 0.8f)) * SNOW_RAY_SPACING;
			int count = 0;
			float left = 1.0f;

			for (int b = 0; b < plant->bough_count; b++)
			{
				card_hit_t hit;

				if (x < boxes[b][0] || x > boxes[b][2] || z < boxes[b][1] || z > boxes[b][3])
				{
					continue;
				}
				if (bough_card_hit(mesh, &plant->boughs[b], NULL, x, z, &hit))
				{
					// (in order, the highest first)
					int k = count++;

					while (k > 0 && hits[k - 1].hit.height < hit.height)
					{
						hits[k] = hits[k - 1];
						k--;
					}
					hits[k].hit = hit;
					hits[k].bough = b;
				}
			}
			for (int k = 0; k < count && left > 0.02f; k++)
			{
				const card_hit_t *hit = &hits[k].hit;
				bough_t *bough = &plant->boughs[hits[k].bough];
				float caught;
				float cx;
				float cy;

				if (atlas_alpha(atlas, hit->u, hit->v) < 0.5f)
				{
					continue;
				}
				// (it slides off what leans)
				caught = left * SNOW_CATCH * smooth_step(0.3f, 0.75f, hit->level);
				left -= caught;
				cx = fminf(fmaxf(hit->s * (float)SNOW_CELLS_ACROSS, 0.0f), (float)SNOW_CELLS_ACROSS - 0.001f);
				cy = fminf(fmaxf(hit->t * (float)SNOW_CELLS_ALONG, 0.0f), (float)SNOW_CELLS_ALONG - 0.001f);
				bough->depth[(int)cy][(int)cx] += caught * ray_area;
			}
		}
	}
	free(hits);
	// What it caught, of each cell's area, smoothed (the snow bridges the
	// gaps between the shoots, rounds off at its edges): its depth; thinner
	// out at the end, where the shoots are thinner
	for (int b = 0; b < plant->bough_count; b++)
	{
		bough_t *bough = &plant->boughs[b];
		float cell_area = (bough->width / (float)SNOW_CELLS_ACROSS) * (bough->card_length / (float)SNOW_CELLS_ALONG);
		static float smoothed[SNOW_CELLS_ALONG][SNOW_CELLS_ACROSS];

		for (int y = 0; y < SNOW_CELLS_ALONG; y++)
		{
			for (int x = 0; x < SNOW_CELLS_ACROSS; x++)
			{
				bough->depth[y][x] = fminf(bough->depth[y][x] / fmaxf(cell_area, 1e-6f), 1.0f);
			}
		}
		for (int pass = 0; pass < 2; pass++)
		{
			for (int y = 0; y < SNOW_CELLS_ALONG; y++)
			{
				for (int x = 0; x < SNOW_CELLS_ACROSS; x++)
				{
					float sum = 0.0f;

					for (int dy = -1; dy <= 1; dy++)
					{
						for (int dx = -1; dx <= 1; dx++)
						{
							int nx = x + dx;
							int ny = y + dy;

							sum += nx >= 0 && ny >= 0 && nx < SNOW_CELLS_ACROSS && ny < SNOW_CELLS_ALONG ? bough->depth[ny][nx] : 0.0f;
						}
					}
					smoothed[y][x] = sum / 9.0f;
				}
			}
			memcpy(bough->depth, smoothed, sizeof(smoothed));
		}
		// Lumps: a little more here, less there (at random, smoothed)
		{
			static float lumps[SNOW_CELLS_ALONG][SNOW_CELLS_ACROSS];

			for (int y = 0; y < SNOW_CELLS_ALONG; y++)
			{
				for (int x = 0; x < SNOW_CELLS_ACROSS; x++)
				{
					smoothed[y][x] = random_between(0.0f, 1.0f);
				}
			}
			for (int y = 0; y < SNOW_CELLS_ALONG; y++)
			{
				for (int x = 0; x < SNOW_CELLS_ACROSS; x++)
				{
					float sum = 0.0f;
					int count = 0;

					for (int dy = -1; dy <= 1; dy++)
					{
						for (int dx = -1; dx <= 1; dx++)
						{
							int nx = x + dx;
							int ny = y + dy;

							if (nx >= 0 && ny >= 0 && nx < SNOW_CELLS_ACROSS && ny < SNOW_CELLS_ALONG)
							{
								sum += smoothed[ny][nx];
								count++;
							}
						}
					}
					lumps[y][x] = 0.55f + 0.9f * sum / (float)count;
				}
			}
			for (int y = 0; y < SNOW_CELLS_ALONG; y++)
			{
				for (int x = 0; x < SNOW_CELLS_ACROSS; x++)
				{
					bough->depth[y][x] *= lumps[y][x];
				}
			}
		}
		bough->load = 0.0f;
		for (int y = 0; y < SNOW_CELLS_ALONG; y++)
		{
			float t = ((float)y + 0.5f) / (float)SNOW_CELLS_ALONG;

			for (int x = 0; x < SNOW_CELLS_ACROSS; x++)
			{
				// (none where too little lies: no thin film all over)
				float covered = smooth_step(0.08f, 0.5f, bough->depth[y][x]);

				bough->depth[y][x] = SNOW_DEPTH * covered * (1.0f - 0.35f * t);
				bough->load += bough->depth[y][x] * cell_area * SNOW_DENSITY;
			}
		}
	}
}

// The snow lying on each bough: a mesh over its top card, as deep as it
// lies, its foot on the card (its own lift per vertex in lifts, from the
// first of them; it grows from nothing with the snow)
static void snow_cover_mesh(plant_t *plant, vec3 **lifts_out, uint32_t *first_out)
{
	mesh_t *mesh = &plant->mesh;
	enum
	{
		NODES_ACROSS = SNOW_CELLS_ACROSS + 1,
		NODES_ALONG = SNOW_CELLS_ALONG + 1
	};
	static vec3 base[NODES_ALONG][NODES_ACROSS];
	static vec3 full[NODES_ALONG][NODES_ACROSS];
	static vec3 faces[NODES_ALONG][NODES_ACROSS];
	static float depths[NODES_ALONG][NODES_ACROSS];
	static int32_t made[NODES_ALONG][NODES_ACROSS];
	vec3 *lifts = NULL;
	size_t lift_capacity = 0;
	uint32_t first = (uint32_t)mesh->vertex_count;

	for (int b = 0; b < plant->bough_count; b++)
	{
		bough_t *bough = &plant->boughs[b];

		bough->snow_first = (uint32_t)mesh->vertex_count;
		for (int row = 0; row < NODES_ALONG; row++)
		{
			for (int column = 0; column < NODES_ACROSS; column++)
			{
				float s = (float)column / (float)SNOW_CELLS_ACROSS;
				float t = (float)row / (float)SNOW_CELLS_ALONG;
				float depth = 0.0f;
				int around = 0;

				for (int k = 0; k < 4; k++)
				{
					int cx = column - 1 + (k & 1);
					int cy = row - 1 + (k >> 1);

					if (cx >= 0 && cy >= 0 && cx < SNOW_CELLS_ACROSS && cy < SNOW_CELLS_ALONG)
					{
						depth += bough->depth[cy][cx];
					}
					around++;
				}
				depths[row][column] = depth / (float)around;
				base[row][column] = bough_card_point(&bough->branch, bough->card_length, bough->width, bough->place.anchor,
					bough->top_offset + SNOW_LIFT, s, t, &faces[row][column]);
				full[row][column] = add(base[row][column], scale(faces[row][column], depths[row][column]));
				made[row][column] = -1;
			}
		}
		mesh->plant_random = hash_unit((uint32_t)mesh->vertex_count * 2654435761u + 29u);
		mesh->plant_extra = 0.0f;
		for (int row = 0; row < SNOW_CELLS_ALONG; row++)
		{
			for (int column = 0; column < SNOW_CELLS_ACROSS; column++)
			{
				int corner_rows[4] = { row, row, row + 1, row + 1 };
				int corner_columns[4] = { column, column + 1, column + 1, column };
				uint32_t index[4];
				bool any = false;

				for (int k = 0; k < 4; k++)
				{
					any |= depths[corner_rows[k]][corner_columns[k]] > 0.003f;
				}
				if (!any)
				{
					continue;
				}
				for (int k = 0; k < 4; k++)
				{
					int r = corner_rows[k];
					int c = corner_columns[k];

					if (made[r][c] < 0)
					{
						// Its normal under all of it (the snow's surface)
						int r0 = r > 0 ? r - 1 : r;
						int r1 = r < NODES_ALONG - 1 ? r + 1 : r;
						int c0 = c > 0 ? c - 1 : c;
						int c1 = c < NODES_ACROSS - 1 ? c + 1 : c;
						vec3 along = sub(full[r1][c], full[r0][c]);
						vec3 across = sub(full[r][c1], full[r][c0]);
						vec3 normal = cross(along, across);
						float s = (float)c / (float)SNOW_CELLS_ACROSS;
						float t = (float)r / (float)SNOW_CELLS_ALONG;

						normal = dot(normal, normal) > 1e-12f ? normalized(normal) : faces[r][c];
						if (normal.y < 0.0f)
						{
							normal = scale(normal, -1.0f);
						}
						// (how deep it is there, of the most: thin at its edges, where the needles show through)
						mesh->plant_along = t;
						mesh->plant_extra = fminf(depths[r][c] / SNOW_DEPTH, 1.0f);
						made[r][c] = (int32_t)add_vertex(mesh, base[r][c], normal, across, scale(along, -1.0f),
							bough->place.uv[0] + (bough->place.uv[2] - bough->place.uv[0]) * s,
							bough->place.uv[3] + (bough->place.uv[1] - bough->place.uv[3]) * t);
						if ((size_t)(made[r][c] - (int32_t)first) >= lift_capacity)
						{
							lift_capacity = lift_capacity > 0 ? lift_capacity * 2 : 4096;
							lifts = checked_realloc(lifts, sizeof(vec3) * lift_capacity);
						}
						lifts[made[r][c] - (int32_t)first] = scale(faces[r][c], depths[r][c]);
					}
					index[k] = (uint32_t)made[r][c];
				}
				// (counterclockwise seen from above)
				add_triangle(mesh, MATERIAL_SNOW, index[0], index[2], index[1]);
				add_triangle(mesh, MATERIAL_SNOW, index[0], index[3], index[2]);
			}
		}
		mesh->plant_along = 0.0f;
		mesh->plant_extra = 0.0f;
		bough->snow_end = (uint32_t)mesh->vertex_count;
	}
	*lifts_out = lifts;
	*first_out = first;
}

// How far each bough's end sags under its snow and what rests on it,
// resting on the one below where it would go into its snow
static void snow_sag(plant_t *plant, const float (*boxes)[4])
{
	mesh_t *mesh = &plant->mesh;
	int *order = checked_realloc(NULL, sizeof(int) * (size_t)plant->bough_count);
	float *carried = checked_realloc(NULL, sizeof(float) * (size_t)plant->bough_count);
	uint32_t card_vertices = (uint32_t)((BRANCH_CARD_ALONG + 1) * (BRANCH_CARD_ACROSS + 1));
	vec3 *moved = checked_realloc(NULL, sizeof(vec3) * card_vertices);
	float bend_scale = plant->kind->snow_sag / 0.06f;
	static const float sample_t[4] = { 0.55f, 0.7f, 0.85f, 1.0f };

	for (int b = 0; b < plant->bough_count; b++)
	{
		order[b] = b;
		plant->boughs[b].carried = 0.0f;
		plant->boughs[b].drop = 0.0f;
	}
	sorting_plant = plant;
	qsort(order, (size_t)plant->bough_count, sizeof(int), compare_bough_heights);
	for (int round = 0; round < SNOW_ROUNDS; round++)
	{
		for (int b = 0; b < plant->bough_count; b++)
		{
			carried[b] = 0.0f;
		}
		for (int k = 0; k < plant->bough_count; k++)
		{
			bough_t *bough = &plant->boughs[order[k]];
			float length = fmaxf(bough->branch.length, 0.3f);
			float per_meter = (bough->load + bough->carried) / length;
			float free_drop = fminf(SNOW_BEND * bend_scale * length * powf(per_meter / SNOW_BEND_LOAD, 0.7f), SNOW_MOST_BEND * length);
			int resting_on = -1;

			bough->drop = free_drop;
			for (int sample = 0; sample < 4; sample++)
			{
				// Its underside's middle there, sagged
				vec3 point = bough_card_point(&bough->branch, bough->card_length, bough->width, bough->place.anchor,
					bough->top_offset - bough->gap, bough->place.anchor, sample_t[sample], NULL);
				float shape = snow_sag_shape(bough_along(bough, point));
				float best_top = -1e9f;
				int best = -1;

				if (shape < 1e-4f)
				{
					continue;
				}
				for (int j = 0; j < plant->bough_count; j++)
				{
					const bough_t *other = &plant->boughs[j];
					card_hit_t hit;

					if (other == bough || other->branch.points[0].y >= bough->branch.points[0].y
						|| point.x < boxes[j][0] || point.x > boxes[j][2] || point.z < boxes[j][1] || point.z > boxes[j][3])
					{
						continue;
					}
					// (the other one as it sags now)
					for (uint32_t v = 0; v < card_vertices; v++)
					{
						const float *position = mesh->vertices[other->top_first + v].position;

						moved[v] = bough_sag_offset(other, v3(position[0], position[1], position[2]));
					}
					if (bough_card_hit(mesh, other, moved, point.x, point.z, &hit))
					{
						float top = hit.height + bough_snow_depth(other, hit.s, hit.t);

						// (only what is below it, not what it is under)
						if (top < point.y + 0.05f && top > best_top)
						{
							best_top = top;
							best = j;
						}
					}
				}
				if (best >= 0 && point.y - bough->drop * shape < best_top + SNOW_CLEARANCE)
				{
					bough->drop = fmaxf(fminf(bough->drop, (point.y - best_top - SNOW_CLEARANCE) / shape), 0.0f);
					resting_on = best;
				}
			}
			// What it does not carry itself rests on the one under it
			if (resting_on >= 0 && free_drop > 1e-4f)
			{
				carried[resting_on] += 0.6f * (bough->load + bough->carried) * (1.0f - bough->drop / free_drop);
			}
		}
		for (int b = 0; b < plant->bough_count; b++)
		{
			plant->boughs[b].carried = carried[b];
		}
	}
	if (getenv("POTOP_SNOW_DEBUG") != NULL)
	{
		for (int b = 0; b < plant->bough_count; b++)
		{
			const bough_t *bough = &plant->boughs[b];

			printf("bough %2d: foot %.2f m, %.2f m long, load %.1f kg, carried %.1f kg, drop %.3f m\n", b, bough->branch.points[0].y,
				bough->branch.length, bough->load, bough->carried, bough->drop);
		}
	}
	free(moved);
	free(carried);
	free(order);
}

// The snow baked in: falls, lies on the boughs, bends them; where each
// vertex goes under all of it (mesh snow_vertices)
static void bake_snow(plant_t *plant, const atlas_t *atlas)
{
	mesh_t *mesh = &plant->mesh;
	float (*boxes)[4] = checked_realloc(NULL, sizeof(float) * 4 * (size_t)plant->bough_count);
	vec3 *lifts = NULL;
	uint32_t lift_first = 0;
	float lowest = mesh->minimum.y;

	for (int b = 0; b < plant->bough_count; b++)
	{
		bough_box(mesh, &plant->boughs[b], boxes[b]);
	}
	snow_fall(plant, atlas, boxes);
	snow_cover_mesh(plant, &lifts, &lift_first);
	snow_sag(plant, (const float (*)[4])boxes);
	mesh->snow_vertices = checked_realloc(NULL, sizeof(object_file_snow_vertex_t) * mesh->vertex_capacity);
	memset(mesh->snow_vertices, 0, sizeof(object_file_snow_vertex_t) * mesh->vertex_count);
	for (int b = 0; b < plant->bough_count; b++)
	{
		const bough_t *bough = &plant->boughs[b];
		uint32_t ranges[2][2] = { { bough->first_vertex, bough->vertex_end }, { bough->snow_first, bough->snow_end } };

		for (int r = 0; r < 2; r++)
		{
			for (uint32_t v = ranges[r][0]; v < ranges[r][1]; v++)
			{
				const float *position = mesh->vertices[v].position;
				vec3 offset = bough_sag_offset(bough, v3(position[0], position[1], position[2]));
				object_file_snow_vertex_t *snow = &mesh->snow_vertices[v];

				if (r == 1)
				{
					offset = add(offset, lifts[v - lift_first]);
					snow->snow = 1.0f;
				}
				snow->offset[0] = offset.x;
				snow->offset[1] = offset.y;
				snow->offset[2] = offset.z;
				lowest = fminf(lowest, position[1] + offset.y);
			}
		}
	}
	// (its box: down to where the snow takes it)
	mesh->minimum.y = lowest;
	// The snow lying on it: gone at the last level of detail
	mesh->lods[MATERIAL_SNOW] = true;
	mesh->lod_index_count[MATERIAL_SNOW][0] = (uint32_t)mesh->index_count[MATERIAL_SNOW];
	mesh->lod_index_count[MATERIAL_SNOW][1] = (uint32_t)mesh->index_count[MATERIAL_SNOW];
	mesh->lod_index_count[MATERIAL_SNOW][2] = 0;
	free(lifts);
	free(boxes);
}

// Twigs for a finest branch as grown: one along it (from its foot to its
// end), or, where it is longer than the kind's twig_meters (a willow's
// whips), twigs one after the other along it, each the way it goes there
static void add_twigs_along(plant_t *plant, const branch_t *branch, int sides, float radius)
{
	vec3 foot = branch->points[0];
	vec3 end = branch->points[branch->count - 1];
	vec3 chord = sub(end, foot);
	float chord_length = sqrtf(dot(chord, chord));
	float twig_meters = plant->kind->twig_meters;
	int pieces;

	if (plant->kind->branch_cards)
	{
		add_branch_card(plant, branch, sides, radius);
		return;
	}

	if (twig_meters <= 0.0f || branch->length * plant->kind->twig_scale <= twig_meters * 1.3f)
	{
		add_twig(plant, foot, chord_length > 1e-4f ? scale(chord, 1.0f / chord_length) : branch->directions[0], chord_length);
		return;
	}
	// No bark: the twigs' own stems are the whip (a tube under them would
	// cost more than all of them)
	pieces = (int)ceilf(branch->length / twig_meters);
	pieces = pieces < 2 ? 2 : (pieces > 24 ? 24 : pieces);
	for (int i = 0; i < pieces; i++)
	{
		float t0 = (float)i / (float)pieces;
		float t1 = (float)(i + 1) / (float)pieces;
		vec3 from;
		vec3 to;
		vec3 heading;
		float unused;
		vec3 piece;
		float piece_length;

		branch_at(branch, t0, &from, &heading, &unused);
		branch_at(branch, t1, &to, &heading, &unused);
		piece = sub(to, from);
		piece_length = sqrtf(dot(piece, piece));
		if (piece_length < 1e-4f)
		{
			continue;
		}
		// Each a little longer than its piece, so they overlap (add_twig
		// takes the kind's twig_scale)
		add_twig(plant, from, scale(piece, 1.0f / piece_length), piece_length * 1.15f / plant->kind->twig_scale);
	}
}

// Sprigs along a branch that is not a finest one (conifers: their needles
// grow all along), to either side and forward; and one at its tip
static void add_side_twigs(plant_t *plant, const branch_t *branch, float child_length)
{
	int count = (int)(plant->kind->side_twigs * branch->length + 0.5f);
	float side_sign = random_unit() < 0.5f ? -1.0f : 1.0f;

	for (int i = 0; i < count; i++)
	{
		float t = 0.1f + 0.85f * ((float)i + random_between(0.2f, 0.8f)) / (float)count;
		vec3 point;
		vec3 heading;
		float radius;
		vec3 out;

		branch_at(branch, t, &point, &heading, &radius);
		// Alternately to the left and the right of the branch, a little at random round it
		out = cross(v3(0.0f, 1.0f, 0.0f), heading);
		out = dot(out, out) > 1e-4f ? normalized(out) : perpendicular(heading);
		out = scale(rotated(out, heading, random_between(-0.6f, 0.6f)), side_sign);
		side_sign = -side_sign;
		add_twig(plant, add(point, scale(out, radius)), normalized(add(out, scale(heading, 0.9f))),
			child_length * (1.0f - 0.5f * t) * random_between(0.7f, 1.0f));
	}
	if (plant->kind->tip_twig)
	{
		vec3 point = branch->points[branch->count - 1];
		vec3 heading = branch->directions[branch->count - 1];

		add_twig(plant, point, heading, child_length * 0.6f);
	}
}

// How long a branch out of the trunk is where it leaves it (t: 0 the foot,
// 1 the top): firs taper to a point, others are widest a little above the
// middle of the crown
static float crown_shape(const plant_t *plant, float t)
{
	float in_crown = fminf(fmaxf((t - plant->kind->crown_start) / (1.0f - plant->kind->crown_start), 0.0f), 1.0f);

	switch (plant->kind->crown)
	{
		case CROWN_CONE:
			return 0.2f + 1.0f * (1.0f - in_crown);
		case CROWN_COLUMN:
			// As wide all the way, rounded off in the top fifth
			return in_crown < 0.8f ? 1.0f - 0.15f * in_crown : 0.88f * sqrtf(fmaxf(1.0f - (in_crown - 0.8f) / 0.2f, 0.0f)) + 0.1f;
		case CROWN_DOME:
			// Widest in the lower middle of the crown, flat on top
			return 0.5f + 0.7f * sinf(PI_F * fminf(in_crown * 0.75f + 0.15f, 1.0f));
		case CROWN_ROUND:
		default:
			return 0.45f + 0.75f * sinf(PI_F * fminf(in_crown * 0.85f + 0.1f, 1.0f));
	}
}

// The dead branches on a stem's bare part under the crown (kind
// dead_branches): out round it by the golden angle, level or hanging a
// little, thin and tapering, most broken short (the lower the shorter:
// dead longer), a stub or two off the longer ones; no foliage. The
// deepest level, so the levels of detail drop them first
static void add_dead_branches(plant_t *plant, const branch_t *stem)
{
	const plant_kind_t *kind = plant->kind;
	float top = fminf(plant->stem_crown_start, 0.97f);
	int count = (int)(kind->dead_branches * top / fmaxf(kind->crown_start, 0.05f) + 0.5f);
	float azimuth = random_between(0.0f, 2.0f * PI_F);
	uint8_t level = plant->mesh.bark_level;

	if (top < 0.08f)
	{
		return;
	}
	plant->mesh.bark_level = (uint8_t)(kind->levels > 0 ? kind->levels : 1);
	for (int i = 0; i < count; i++)
	{
		float t = top * (0.12f + 0.88f * ((float)i + random_between(0.1f, 0.9f)) / (float)count);
		float up_share = t / top;
		vec3 point;
		vec3 heading;
		float stem_radius;
		float length = plant->height * 0.17f * (0.3f + 0.7f * up_share) * (random_unit() < 0.45f ? random_between(0.15f, 0.5f) : random_between(0.6f, 1.0f));
		float radius;
		float down = random_between(80.0f, 105.0f) * PI_F / 180.0f;
		vec3 out;
		vec3 direction;
		branch_t dead;

		azimuth += (GOLDEN_ANGLE_DEGREES + random_between(-25.0f, 25.0f)) * PI_F / 180.0f;
		branch_at(stem, t, &point, &heading, &stem_radius);
		radius = fmaxf(fminf(stem_radius * 0.3f, length * 0.05f), 0.008f);
		out = rotated(perpendicular(heading), heading, azimuth);
		direction = normalized(add(scale(heading, cosf(down)), scale(out, sinf(down))));
		trace_branch(&dead, add(point, scale(out, stem_radius * 0.7f)), direction, length, radius, 0.25f, 0.35f, -0.04f, fmaxf(0.15f, length / 5.0f));
		add_tube(plant, &dead, radius < 0.012f ? 3 : 4, radius, MATERIAL_BARK);
		// (a stub or two off the longer ones)
		if (length > plant->height * 0.05f)
		{
			int stubs = 1 + random_index(2);

			for (int k = 0; k < stubs; k++)
			{
				float along = random_between(0.35f, 0.8f);
				vec3 stub_point;
				vec3 stub_heading;
				float stub_radius;
				vec3 side;
				branch_t stub;

				branch_at(&dead, along, &stub_point, &stub_heading, &stub_radius);
				side = rotated(perpendicular(stub_heading), stub_heading, random_between(0.0f, 2.0f * PI_F));
				side = normalized(add(scale(stub_heading, 0.6f), side));
				trace_branch(&stub, stub_point, side, length * random_between(0.1f, 0.25f), stub_radius * 0.6f, 0.3f, 0.3f, 0.0f, 1.0f);
				add_tube(plant, &stub, 3, stub_radius * 0.6f, MATERIAL_BARK);
			}
		}
	}
	plant->mesh.bark_level = level;
}

// trunk: a trunk's (level 0) lean, its part of the whole trunk, its flare
static void grow(plant_t *plant, int level, vec3 start, vec3 direction, float length, float radius, const branch_shape_t *trunk)
{
	const plant_kind_t *kind = plant->kind;
	branch_t branch;
	// Fewer sides on the thin ones (they are a few pixels wide anyway)
	int sides = level == 0 ? 12 : (radius < 0.012f ? 3 : (radius < 0.03f ? 4 : (level == 1 ? 7 : 5)));
	bool finest = level >= kind->levels || level >= MAX_LEVELS;
	float reach = level > 0 && level <= MAX_LEVELS ? kind->reach[level - 1] : 0.0f;
	branch_shape_t shape;
	int count = finest ? 0 : (level == 0 ? (int)((float)kind->children[0] * plant->trunk_children + 0.5f) : kind->children[level]);
	float child_t[MAX_KINKS];
	float child_azimuth[MAX_KINKS];
	vec3 child_axis[MAX_KINKS];

	bool twig = finest && plant->twig_count > 0 && (plant->leaf_count == 0 || random_unit() < plant->twig_share);

	if (trunk != NULL)
	{
		shape = *trunk;
	}
	else
	{
		memset(&shape, 0, sizeof(shape));
		shape.flare = level > 0 ? kind->collar : kind->root_flare;
		shape.trunk = level == 0;
	}
	shape.droop = level > 0 ? kind->droop : 0.0f;
	shape.tip_lift = level > 0 ? kind->tip_lift : 0.0f;
	shape.gnarl = kind->gnarl * (level > 0 && kind->gnarl_branches > 0.0f ? kind->gnarl_branches : 1.0f);
	shape.kink_radians = level > 0 || plant->kinked_trunk ? kind->kinks * 0.6f : 0.0f;
	shape.knob = level == 0 ? kind->knob : 0.0f;
	shape.upright = level == 0 ? kind->trunk_upright : 0.0f;
	shape.thinnest = kind->thinnest_radius;
	// Where the branches out of this one leave it: round it by the golden
	// angle, from where the crown starts (on the trunk) or a little way
	// along (its kinks at the same places)
	{
		float first_t = level == 0 ? plant->stem_crown_start : kind->child_start;
		float azimuth = random_between(0.0f, 2.0f * PI_F);

		count = count < MAX_KINKS ? count : MAX_KINKS;
		for (int i = 0; i < count; i++)
		{
			child_t[i] = first_t + (1.0f - first_t) * ((float)i + random_between(0.1f, 0.9f)) / (float)count;
			azimuth += (GOLDEN_ANGLE_DEGREES + random_between(-15.0f, 15.0f)) * PI_F / 180.0f;
			child_azimuth[i] = azimuth;
		}
	}
	shape.kink_count = count;
	shape.kink_t = child_t;
	shape.kink_azimuth = child_azimuth;
	shape.kink_axes = child_axis;
	// The finest branches: twigs, or (with leaves too, past the twig
	// share) branches with their leaves. A twig lies along the way the
	// branch it stands for grows (bent down, hanging), not the way it
	// leaves its parent
	if (twig)
	{
		trace_shaped_branch(&branch, start, direction, length, radius, kind->taper, kind->bend, reach, fmaxf(0.1f, length / 8.0f), &shape);
		add_twigs_along(plant, &branch, sides, radius);
		return;
	}
	// Short thin branches need fewer segments
	trace_shaped_branch(&branch, start, direction, length, radius, level == 0 ? plant->trunk_taper : kind->taper, kind->bend, reach,
		level == 0 ? 0.5f : fmaxf(0.3f, length / 8.0f), &shape);
	if (level == 0)
	{
		plant->trunk_end = branch.points[branch.count - 1];
		plant->trunk_end_direction = branch.directions[branch.count - 1];
	}
	plant->mesh.bark_level = (uint8_t)level;
	add_tube(plant, &branch, sides, radius, level == 0 ? MATERIAL_TRUNK : MATERIAL_BARK);
	if (level == 0 && kind->dead_branches > 0.0f)
	{
		add_dead_branches(plant, &branch);
	}
	if (level == 0 && !plant->crown_center_set)
	{
		// The light comes out from the crown's foot, on the (first) trunk
		vec3 heading;
		float unused;

		branch_at(&branch, fminf(plant->stem_crown_start, 1.0f), &plant->crown_center, &heading, &unused);
		if (plant->kind->light_from_below > 0.0f)
		{
			// (a mat: from under its middle, so its normals mostly up)
			plant->crown_center = v3(0.0f, plant->crown_center.y - plant->kind->light_from_below * plant->height, 0.0f);
		}
		plant->crown_center_set = true;
	}
	if (finest)
	{
		add_leaves(plant, &branch);
		fruit_along(plant, start, direction, length, radius * 0.5f);
		return;
	}
	if (level > 0 && plant->loose_leaves > 0.0f)
	{
		add_loose_leaves(plant, &branch);
	}
	if (level > 0 && plant->twig_count > 0 && (kind->side_twigs > 0.0f || kind->tip_twig))
	{
		add_side_twigs(plant, &branch, length * kind->length_ratio[level]);
	}
	// The branches out of this one
	for (int i = 0; i < count; i++)
	{
		float t = child_t[i];
		vec3 point;
		vec3 heading;
		float parent_radius;
		float child_length = length * kind->length_ratio[level] * random_between(0.8f, 1.15f);
		float down = (kind->down_degrees[level] + random_between(-1.0f, 1.0f) * kind->down_spread_degrees) * PI_F / 180.0f;
		vec3 child_direction;

		if (t > 0.97f)
		{
			continue;
		}
		branch_at(&branch, t, &point, &heading, &parent_radius);
		child_length *= level == 0 ? crown_shape(plant, (plant->trunk_u0 + (plant->trunk_u1 - plant->trunk_u0) * t)) : 1.0f - 0.6f * t;
		child_direction = rotated(heading, child_axis[i], down);
		if (plant->wind_side > 0.0f)
		{
			// Short and few on the side the wind comes from
			vec3 level_direction = v3(child_direction.x, 0.0f, child_direction.z);
			float windward = dot(level_direction, level_direction) > 1e-4f
				? fmaxf(-dot(normalized(level_direction), plant->downwind), 0.0f) : 0.0f;

			if (random_unit() < 0.6f * plant->wind_side * windward)
			{
				continue;
			}
			child_length *= 1.0f - 0.7f * plant->wind_side * windward;
		}
		// The swollen foot of a collared branch sits deeper in its parent
		grow(plant, level + 1, add(point, scale(child_direction, parent_radius * (0.5f - 0.35f * fminf(kind->collar, 1.0f)))),
			child_direction, child_length,
			fminf(parent_radius * kind->child_radius_ratio, child_length * 0.05f), NULL);
	}
}

static void grow_tree(plant_t *plant)
{
	const plant_kind_t *kind = plant->kind;
	int stems = kind->stems;
	float fork = kind->fork_height;
	float crown_start = kind->crown_start;
	float trunk_radius = plant->height * kind->trunk_radius_ratio;
	float stem_taper = kind->trunk_taper > 0.0f ? kind->trunk_taper : kind->taper;
	branch_shape_t trunk;

	plant->trunk_reference = trunk_radius;
	vec3 start = v3(0.0f, -0.05f, 0.0f);
	vec3 fork_direction = v3(0.0f, 1.0f, 0.0f);
	float lean_azimuth = 0.0f;

	if (kind->extra_stem_chance > 0.0f && random_unit() < kind->extra_stem_chance)
	{
		stems++;
	}
	stems = stems < 1 ? 1 : (stems > 12 ? 12 : stems);
	fork = stems > 1 ? fminf(fmaxf(fork, 0.0f), 0.9f) : 0.0f;
	memset(&trunk, 0, sizeof(trunk));
	trunk.trunk = true;
	trunk.flare = kind->root_flare;
	trunk.lean_radians = kind->lean_degrees * PI_F / 180.0f;
	trunk.lean_length = kind->lean_length;
	trunk.straighten = kind->straighten;
	trunk.u1 = 1.0f;
	if (trunk.lean_radians > 0.0f || plant->wind_side > 0.0f)
	{
		lean_azimuth = random_between(0.0f, 2.0f * PI_F);
	}
	trunk.lean_toward = v3(cosf(lean_azimuth), 0.0f, sinf(lean_azimuth));
	// The wind blows the way the trunk leans
	plant->downwind = trunk.lean_toward;
	plant->crown_center = v3(0.0f, plant->height * crown_start, 0.0f);
	plant->crown_center_set = false;
	plant->kinked_trunk = false;
	plant->trunk_taper = stem_taper;
	plant->trunk_children = 1.0f;
	plant->stem_crown_start = crown_start;
	plant->trunk_u0 = 0.0f;
	plant->trunk_u1 = 1.0f;
	if (fork > 0.0f)
	{
		// One trunk up to the fork (with its share of the branches, if the
		// crown starts below it), the stems out of its top
		branch_shape_t base = trunk;
		float top_radius = 1.0f - (1.0f - stem_taper) * fork;

		base.u1 = fork;
		plant->trunk_u1 = fork;
		plant->trunk_taper = top_radius;
		plant->stem_crown_start = crown_start / fork;
		plant->trunk_children = fmaxf(fork - crown_start, 0.0f) / fmaxf(1.0f - crown_start, 0.05f);
		grow(plant, 0, start, trunk.lean_radians > 0.0f ? lean_heading(&trunk, 0.0f) : v3(0.0f, 1.0f, 0.0f), plant->height * fork,
			trunk_radius, &base);
		start = sub(plant->trunk_end, scale(plant->trunk_end_direction, trunk_radius * top_radius * 0.6f));
		fork_direction = plant->trunk_end_direction;
		trunk_radius *= top_radius * 1.05f / sqrtf((float)stems);
		trunk.flare = 0.15f;
		trunk.u0 = fork;
		plant->trunk_u0 = fork;
		plant->trunk_u1 = 1.0f;
		plant->trunk_taper = stem_taper / top_radius;
		plant->stem_crown_start = fmaxf((crown_start - fork) / (1.0f - fork), 0.0f);
		plant->trunk_children = fminf((1.0f - fork) / fmaxf(1.0f - crown_start, 0.05f), 1.0f);
	}
	for (int s = 0; s < stems; s++)
	{
		float azimuth = (float)s * GOLDEN_ANGLE_DEGREES * PI_F / 180.0f + random_between(0.0f, 0.5f);
		float spread = (stems > 1 ? (fork > 0.0f ? fmaxf(kind->stem_lean_degrees, 22.0f) : kind->stem_lean_degrees)
			: random_between(0.0f, kind->stem_lean_degrees)) * PI_F / 180.0f;
		vec3 out = v3(cosf(azimuth), 0.0f, sinf(azimuth));
		vec3 direction;
		float stem_height = plant->height * (1.0f - fork) * (stems > 1 ? random_between(0.75f, 1.0f) : 1.0f);
		branch_shape_t stem = trunk;

		stem.part = scale(out, tanf(spread));
		if (kind->sprawl && trunk.lean_radians <= 0.0f)
		{
			// (its own lean, its own way: out low, then up)
			stem.lean_radians = spread;
			stem.lean_toward = out;
			stem.part = v3(0.0f, 0.0f, 0.0f);
			direction = lean_heading(&stem, 0.0f);
		}
		else if (trunk.lean_radians > 0.0f)
		{
			direction = lean_heading(&stem, trunk.u0);
		}
		else
		{
			direction = normalized(add(fork_direction, stem.part));
			stem.part = v3(0.0f, 0.0f, 0.0f);
		}
		// Forked: each a little out of the middle, so the two grow out of one
		grow(plant, 0, fork > 0.0f ? add(start, scale(out, trunk_radius * 0.45f)) : start, direction, stem_height, trunk_radius, &stem);
	}
}

// The small plants are lit as from the sky: up, a little out from their
// middle
static vec3 low_normal(vec3 point)
{
	return normalized(add(normalized(v3(point.x, 0.0f, point.z)), v3(0.0f, 1.6f, 0.0f)));
}

// A fern's fronds: hardly out at all (a frond as wide and long as theirs,
// leaning out as much as low_normal's, takes the sun much less on the plant's
// far side: dark fronds among bright ones)
static vec3 frond_normal(vec3 out)
{
	return normalized(add(normalized(v3(out.x, 0.0f, out.z)), v3(0.0f, 4.0f, 0.0f)));
}

// Blades in a star, leaning out a little
static void grow_tuft(plant_t *plant)
{
	int count = plant->kind->parts + random_index(3) - 1;

	for (int i = 0; i < count && plant->leaf_count > 0; i++)
	{
		float azimuth = (float)i * PI_F / (float)count + random_between(-0.2f, 0.2f);
		vec3 across = v3(cosf(azimuth), 0.0f, sinf(azimuth));
		vec3 out = v3(-across.z, 0.0f, across.x);
		float lean = random_between(0.05f, 0.35f) * (random_unit() < 0.5f ? -1.0f : 1.0f);
		vec3 along = normalized(add(v3(0.0f, 1.0f, 0.0f), scale(out, lean)));
		vec3 base = add(scale(out, random_between(-0.03f, 0.03f) * plant->height), scale(across, random_between(-0.03f, 0.03f) * plant->height));

		add_picture(&plant->mesh, base, along, across, plant->height * random_between(0.7f, 1.1f), low_normal(add(base, scale(out, 0.1f))),
			&plant->leaves[random_index(plant->leaf_count)]);
	}
}

// A head at a point of a stalk: two cards crossed, its middle there (heads
// up a spike, round the stalk)
static void add_head_around(plant_t *plant, vec3 top, float size)
{
	const cell_place_t *place;
	vec3 side = v3(cosf(random_between(0.0f, PI_F)), 0.0f, sinf(random_between(0.0f, PI_F)));
	vec3 other = v3(-side.z, 0.0f, side.x);
	vec3 up = v3(0.0f, 1.0f, 0.0f);
	vec3 base = sub(top, scale(up, size * 0.5f));

	if (plant->head_count == 0)
	{
		return;
	}
	place = &plant->heads[random_index(plant->head_count)];
	add_picture(&plant->mesh, base, up, side, size, low_normal(top), place);
	add_picture(&plant->mesh, base, up, other, size, low_normal(top), place);
}

// A head out of a stalk's side (a plume of a panicle): two cards crossed,
// their foot on the stalk, leaning out this far
static void add_head_out(plant_t *plant, vec3 foot, float size, float lean_degrees)
{
	float azimuth = random_between(0.0f, 2.0f * PI_F);
	float lean = fminf(fmaxf(lean_degrees + random_between(-8.0f, 8.0f), 0.0f), 85.0f) * PI_F / 180.0f;
	vec3 out = v3(cosf(azimuth), 0.0f, sinf(azimuth));
	vec3 along = normalized(add(scale(out, sinf(lean)), v3(0.0f, cosf(lean), 0.0f)));
	vec3 side = v3(-out.z, 0.0f, out.x);
	vec3 other = normalized(cross(along, side));
	const cell_place_t *place;

	if (plant->head_count == 0)
	{
		return;
	}
	place = &plant->heads[random_index(plant->head_count)];
	// (a little down into the stalk: no gap)
	foot = sub(foot, scale(along, size * 0.04f));
	add_picture(&plant->mesh, foot, along, side, size, low_normal(add(foot, out)), place);
	add_picture(&plant->mesh, foot, along, other, size, low_normal(add(foot, out)), place);
}

// A head on top of a stalk: its picture's foot at the stalk's top (a little
// down it, no gap), so the stalk does not show through the flower
static void add_head(plant_t *plant, vec3 top, float size)
{
	add_head_around(plant, add(top, v3(0.0f, size * 0.4f, 0.0f)), size);
}

// A thin stalk up from base: a tube, bending this much at random on its
// way; returns its top
static vec3 add_bent_stalk(plant_t *plant, vec3 base, vec3 direction, float length, float radius, float bend)
{
	branch_t branch;

	trace_branch(&branch, base, direction, length, radius, 0.7f, bend, 0.4f, length / 5.0f);
	add_tube(plant, &branch, 4, radius, MATERIAL_BARK);
	return branch.points[branch.count - 1];
}

// The same, its points kept (where its leaves go)
static vec3 add_traced_stalk(plant_t *plant, vec3 base, vec3 direction, float length, float radius, float bend, branch_t *branch_out)
{
	trace_branch(branch_out, base, direction, length, radius, 0.7f, bend, 0.4f, length / 8.0f);
	add_tube(plant, branch_out, 6, radius, MATERIAL_BARK);
	return branch_out->points[branch_out->count - 1];
}

static vec3 add_stalk(plant_t *plant, vec3 base, vec3 direction, float length, float radius)
{
	return add_bent_stalk(plant, base, direction, length, radius, 0.8f);
}

// A leaf's stalk (petiole): nearly straight, thick where it leaves the plant
// and thinner to the leaf
static vec3 add_petiole(plant_t *plant, vec3 base, vec3 direction, float length, float radius)
{
	branch_t branch;

	trace_branch(&branch, base, direction, length, radius, 0.6f, 0.25f, 0.4f, length / 5.0f);
	add_tube(plant, &branch, 5, radius, MATERIAL_BARK);
	return branch.points[branch.count - 1];
}

// Leaves lying in a ring on the ground, stalks with heads out of the middle
// (dandelions, plantain)
static void grow_rosette(plant_t *plant)
{
	int count = plant->kind->parts + random_index(4) - 1;
	int stalks = 1 + random_index(3);

	for (int i = 0; i < count && plant->leaf_count > 0; i++)
	{
		float azimuth = (float)i * 2.0f * PI_F / (float)count + random_between(-0.25f, 0.25f);
		vec3 out = v3(cosf(azimuth), 0.0f, sinf(azimuth));
		vec3 across = v3(-out.z, 0.0f, out.x);
		vec3 along = normalized(add(out, v3(0.0f, random_between(0.1f, 0.45f), 0.0f)));

		add_picture(&plant->mesh, v3(0.0f, 0.01f, 0.0f), along, across, plant->leaf_size * random_between(0.8f, 1.25f),
			low_normal(out), &plant->leaves[random_index(plant->leaf_count)]);
	}
	for (int s = 0; s < stalks; s++)
	{
		vec3 direction = normalized(add(v3(0.0f, 1.0f, 0.0f), scale(random_direction(), 0.15f)));
		vec3 top = add_stalk(plant, v3(0.0f, 0.0f, 0.0f), direction, plant->height * random_between(0.75f, 1.0f), 0.003f);

		add_head(plant, top, plant->height * 0.22f);
	}
}

// Fronds arching out of the middle: long pictures (the atlas' tall cells)
// on bent strips
static void grow_fern(plant_t *plant)
{
	int count = plant->kind->parts + random_index(5) - 1;

	for (int i = 0; i < count && plant->leaf_count > 0; i++)
	{
		float azimuth = (float)i * 2.0f * PI_F / (float)count + random_between(-0.3f, 0.3f);
		vec3 out = v3(cosf(azimuth), 0.0f, sinf(azimuth));
		vec3 across = v3(-out.z, 0.0f, out.x);
		// A shuttlecock: the inner fronds young, shorter and steeper, the
		// outer ones longer, starting lower and arching over more. How
		// steep (the recipe's, give or take), how far it arches over
		// (radians each fifth of it)
		float outer = random_unit();
		float rise = (plant->frond_angle + 12.0f * (1.0f - outer) - 10.0f * outer + random_between(-5.0f, 5.0f)) * PI_F / 180.0f;
		float arch = plant->frond_droop * 0.45f * (0.7f + 0.6f * outer) * random_between(0.85f, 1.15f);
		float length = plant->leaf_size * (0.7f + 0.45f * outer) * random_between(0.9f, 1.1f);
		const cell_place_t *place = &plant->leaves[random_index(plant->leaf_count)];
		vec3 points[6];
		vec3 point = v3(0.0f, 0.0f, 0.0f);
		vec3 heading = normalized(add(scale(out, cosf(rise)), v3(0.0f, sinf(rise), 0.0f)));

		// Up and out, then over: bending down a little more each segment
		for (int k = 0; k < 6; k++)
		{
			points[k] = point;
			point = add(point, scale(heading, length / 5.0f));
			heading = normalized(add(heading, v3(0.0f, -arch, 0.0f)));
		}
		add_strip(&plant->mesh, points, 6, across, length * place->aspect, frond_normal(out), place);
	}
}

// Stalks with a few leaves up them and a head on top
static void grow_flower(plant_t *plant)
{
	int stalks = plant->kind->parts - 1 + random_index(3);

	for (int s = 0; s < stalks; s++)
	{
		vec3 base = v3(random_between(-0.04f, 0.04f), 0.0f, random_between(-0.04f, 0.04f));
		vec3 direction = normalized(add(v3(0.0f, 1.0f, 0.0f), scale(random_direction(), 0.2f)));
		float length = plant->height * random_between(0.7f, 1.0f);
		vec3 top = add_stalk(plant, base, direction, length, 0.0035f);
		int leaves = 2 + random_index(3);

		for (int k = 0; k < leaves && plant->leaf_count > 0; k++)
		{
			float t = random_between(0.1f, 0.6f);
			vec3 at = add(base, scale(sub(top, base), t));
			float azimuth = random_between(0.0f, 2.0f * PI_F);
			vec3 out = v3(cosf(azimuth), 0.0f, sinf(azimuth));
			vec3 along = normalized(add(out, v3(0.0f, random_between(0.3f, 0.9f), 0.0f)));

			add_picture(&plant->mesh, at, along, v3(-out.z, 0.0f, out.x), plant->leaf_size * random_between(0.8f, 1.2f), low_normal(out),
				&plant->leaves[random_index(plant->leaf_count)]);
		}
		add_head(plant, top, plant->height * 0.25f);
	}
}


// ---------------------------------------------------------------------------
// Small plants of a shape given (plant_herb_t): leaves round the foot on
// their stalks (petioles), bent strips steeper and smaller inside; flower
// stalks out of the middle with leaves up them and heads on top (or up
// their top part, a spike)

static const plant_kind_t *kind_at(int kind);

void plant_herb_kind_defaults(int kind, plant_herb_t *herb_out)
{
	shape_t shape = kind_at(kind)->shape;

	memset(herb_out, 0, sizeof(*herb_out));
	herb_out->leaves = kind_at(kind)->parts;
	herb_out->leaf_spread = 0.45f;
	herb_out->leaf_rise = 15.0f;
	herb_out->leaf_rise_spread = 10.0f;
	herb_out->stalk_height = 0.87f;
	herb_out->stalk_lean = 9.0f;
	herb_out->stalk_thickness = 0.003f;
	herb_out->head_size = 0.22f;
	herb_out->heads = 1;
	herb_out->head_tilt = 25.0f;
	herb_out->cup_size = 0.35f;
	herb_out->cup_depth = 0.7f;
	if (shape == SHAPE_ROSETTE)
	{
		herb_out->stalks_minimum = 1;
		herb_out->stalks_maximum = 3;
	}
	else if (shape == SHAPE_TUFT)
	{
		herb_out->leaf_spread = 0.3f;
		herb_out->leaf_rise = 78.0f;
		herb_out->leaf_arch = 0.25f;
	}
	else if (shape == SHAPE_FLOWER)
	{
		herb_out->leaves = 0;
		herb_out->stalks_minimum = kind_at(kind)->parts - 1;
		herb_out->stalks_maximum = kind_at(kind)->parts + 1;
		herb_out->stalk_height = 0.85f;
		herb_out->stalk_lean = 11.0f;
		herb_out->stalk_thickness = 0.0035f;
		herb_out->stalk_leaves = 3;
		herb_out->head_size = 0.25f;
	}
}

typedef struct
{
	const char *name;
	const char *kind;
	float leaf_size;
	float height;
	plant_herb_t herb;
	float stalk_color[3];                // sRGB (0: the kind's)
} herb_preset_t;

static const herb_preset_t herb_presets[] =
{
	// Arctium: its first year, a big rosette of heart shaped leaves on long
	// stalks, rising and drooping over, no flower stalk yet
	{ "burdock", "rosette", 0.42f, 0.8f, { .given = true, .leaves = 7, .leaf_spread = 0.3f, .leaf_rise = 38.0f, .leaf_rise_spread = 12.0f,
		.leaf_arch = 0.7f, .leaf_layers = 0.6f, .petiole = 0.22f, .petiole_rise = 55.0f, .stalk_height = 1.0f, .stalk_thickness = 0.004f,
		.head_size = 0.1f, .heads = 1 }, { 0.0f } },
	// Plantago major: broad leaves flat round its foot, thin spikes up
	{ "plantain", "rosette", 0.14f, 0.3f, { .given = true, .leaves = 9, .leaf_spread = 0.35f, .leaf_rise = 12.0f, .leaf_rise_spread = 8.0f,
		.leaf_arch = 0.15f, .leaf_layers = 0.3f, .petiole = 0.035f, .petiole_rise = 20.0f, .stalks_minimum = 2, .stalks_maximum = 5,
		.stalk_height = 1.0f, .stalk_lean = 12.0f, .stalk_thickness = 0.0022f, .head_size = 0.3f, .heads = 1 }, { 0.0f } },
	// Taraxacum: toothed leaves flat on the ground, a few hollow stalks
	{ "dandelion", "rosette", 0.16f, 0.25f, { .given = true, .leaves = 10, .leaf_spread = 0.4f, .leaf_rise = 10.0f, .leaf_rise_spread = 8.0f,
		.leaf_arch = 0.1f, .leaf_layers = 0.2f, .stalks_minimum = 1, .stalks_maximum = 3, .stalk_height = 0.9f, .stalk_lean = 14.0f,
		.stalk_thickness = 0.0025f, .head_size = 0.16f, .heads = 1, .head_shape = PLANT_HEAD_CUP, .head_tilt = 25.0f, .cup_size = 0.35f,
		.cup_depth = 0.7f }, { 0.0f } },
	// Leucanthemum: a few leaves at its foot, slender stalks with a few
	// leaves up them, a head each
	{ "daisy", "flower", 0.06f, 0.4f, { .given = true, .leaves = 6, .leaf_spread = 0.3f, .leaf_rise = 25.0f, .leaf_rise_spread = 10.0f,
		.leaf_arch = 0.2f, .stalks_minimum = 3, .stalks_maximum = 6, .stalk_height = 0.9f, .stalk_lean = 10.0f, .stalk_thickness = 0.0022f,
		.stalk_leaves = 2, .head_size = 0.13f, .heads = 1, .head_shape = PLANT_HEAD_CUP, .head_tilt = 20.0f, .cup_size = 0.3f,
		.cup_depth = 0.6f }, { 0.0f } },
	// Digitalis: a rosette of big leaves, one or two tall stalks with bells
	// all up their top half
	{ "foxglove", "flower", 0.24f, 1.2f, { .given = true, .leaves = 9, .leaf_spread = 0.3f, .leaf_rise = 32.0f, .leaf_rise_spread = 10.0f,
		.leaf_arch = 0.45f, .leaf_layers = 0.5f, .petiole = 0.05f, .petiole_rise = 30.0f, .stalks_minimum = 1, .stalks_maximum = 2,
		.stalk_height = 1.0f, .stalk_lean = 4.0f, .stalk_thickness = 0.006f, .stalk_leaves = 4, .head_size = 0.08f, .heads = 10,
		.head_spike = 0.45f }, { 0.0f } },
	// Helianthus annuus: one tall thick stalk, big heart shaped leaves on
	// stalks all the way up it (smaller up it), one big head facing the side
	{ "sunflower", "flower", 0.42f, 1.8f, { .given = true, .leaves = 0, .stalks_minimum = 1, .stalks_maximum = 1, .stalk_height = 1.0f,
		.stalk_lean = 4.0f, .stalk_thickness = 0.012f, .stalk_leaves = 14, .stalk_leaves_from = 0.08f, .stalk_leaves_to = 0.9f,
		.stalk_leaf_size = 1.0f, .stalk_leaf_taper = 0.5f, .stalk_leaf_rise = 25.0f, .stalk_leaf_arch = 0.3f, .stalk_petiole = 0.15f,
		.stalk_petiole_rise = 40.0f, .stalk_petiole_thickness = 0.006f, .stalk_leaf_droop = 50.0f, .stalk_leaf_spiral = 1,
		.stalk_leaf_scatter = 25.0f, .head_size = 0.16f, .heads = 1, .head_shape = PLANT_HEAD_CUP, .head_tilt = 70.0f,
		.head_tilt_minimum = 45.0f, .cup_size = 0.55f, .cup_depth = 0.3f, .disc_curve = -0.8f, .head_neck = 0.15f,
		.leaf_light = 0.6f, .light_spread = 35.0f, .head_light = 1.0f, .leaf_twist = 15.0f, .leaf_fold = 0.3f }, { 0.44f, 0.56f, 0.24f } },
	// Artemisia vulgaris: a clump of a few tall reddish stalks, cut leaves up
	// their lower half, plumes of tiny heads up their top part
	{ "mugwort", "flower", 0.24f, 1.4f, { .given = true, .leaves = 0, .stalks_minimum = 3, .stalks_maximum = 5, .stalk_height = 1.0f,
		.stalk_lean = 10.0f, .stalk_thickness = 0.0045f, .stalk_leaves = 20, .stalk_leaves_from = 0.03f, .stalk_leaves_to = 0.7f,
		.stalk_leaf_size = 1.0f, .stalk_leaf_taper = 0.5f, .stalk_leaf_rise = 30.0f, .stalk_leaf_arch = 0.25f, .stalk_petiole = 0.015f,
		.stalk_leaf_droop = 30.0f, .stalk_leaf_spiral = 1, .stalk_leaf_scatter = 30.0f, .head_size = 0.17f, .heads = 8, .head_spike = 0.42f,
		.head_lean = 30.0f, .head_shape = PLANT_HEAD_CROSSED, .leaf_light = 0.4f, .light_spread = 60.0f, .leaf_twist = 30.0f,
		.leaf_fold = 0.3f },
		{ 0.45f, 0.33f, 0.28f } },
	// Vaccinium vitis-idaea (lingonberry, merisor): a low mat of runners
	// winding over the ground, leafy sprigs with berries standing up along
	// them; the picture of a sprig its leaf
	{ "lingonberry", "rosette", 0.16f, 0.15f, { .given = true, .leaves = 0, .runners = 7, .runner_length = 0.45f, .runner_forks = 2,
		.runner_wander = 0.7f, .runner_thickness = 0.0018f, .runner_leaves = 9, .runner_leaf_size = 1.0f, .runner_leaf_rise = 55.0f,
		.runner_leaf_turn = 60.0f, .leaf_arch = 0.25f, .stalk_height = 1.0f, .stalk_thickness = 0.002f, .head_size = 0.1f, .heads = 1,
		.leaf_light = 0.3f, .light_spread = 90.0f, .leaf_twist = 25.0f, .leaf_fold = 0.15f }, { 0.42f, 0.30f, 0.22f } },
	// A clump of grass: many blades, upright, their tips bending over
	{ "grass_clump", "tuft", 0.45f, 0.45f, { .given = true, .leaves = 14, .leaf_spread = 0.35f, .leaf_rise = 74.0f, .leaf_rise_spread = 12.0f,
		.leaf_arch = 0.5f, .leaf_layers = 0.4f, .stalk_height = 1.0f, .stalk_thickness = 0.002f, .head_size = 0.2f, .heads = 1 }, { 0.0f } },
};

int plant_herb_preset_count(void)
{
	return (int)(sizeof(herb_presets) / sizeof(herb_presets[0]));
}

const char *plant_herb_preset_name(int preset)
{
	return preset >= 0 && preset < plant_herb_preset_count() ? herb_presets[preset].name : "";
}

const char *plant_herb_preset_kind(int preset)
{
	return preset >= 0 && preset < plant_herb_preset_count() ? herb_presets[preset].kind : "";
}

bool plant_herb_preset(plant_recipe_t *recipe, const char *name)
{
	for (int p = 0; p < plant_herb_preset_count(); p++)
	{
		if (strcmp(herb_presets[p].name, name) == 0)
		{
			snprintf(recipe->kind, sizeof(recipe->kind), "%s", herb_presets[p].kind);
			if (herb_presets[p].stalk_color[0] > 0.0f)
			{
				memcpy(recipe->stalk_color, herb_presets[p].stalk_color, sizeof(recipe->stalk_color));
			}
			else
			{
				recipe->stalk_color[0] = -1.0f;
			}
			recipe->leaf_size = herb_presets[p].leaf_size;
			recipe->height = herb_presets[p].height;
			recipe->herb = herb_presets[p].herb;
			return true;
		}
	}
	return false;
}

static const struct { const char *name; size_t offset; bool whole; } herb_fields[] =
{
	{ "leaves", offsetof(plant_herb_t, leaves), true },
	{ "leaf_spread", offsetof(plant_herb_t, leaf_spread), false },
	{ "leaf_rise", offsetof(plant_herb_t, leaf_rise), false },
	{ "leaf_rise_spread", offsetof(plant_herb_t, leaf_rise_spread), false },
	{ "leaf_arch", offsetof(plant_herb_t, leaf_arch), false },
	{ "leaf_layers", offsetof(plant_herb_t, leaf_layers), false },
	{ "petiole", offsetof(plant_herb_t, petiole), false },
	{ "petiole_rise", offsetof(plant_herb_t, petiole_rise), false },
	{ "petiole_thickness", offsetof(plant_herb_t, petiole_thickness), false },
	{ "runners", offsetof(plant_herb_t, runners), true },
	{ "runner_length", offsetof(plant_herb_t, runner_length), false },
	{ "runner_forks", offsetof(plant_herb_t, runner_forks), true },
	{ "runner_wander", offsetof(plant_herb_t, runner_wander), false },
	{ "runner_thickness", offsetof(plant_herb_t, runner_thickness), false },
	{ "runner_leaves", offsetof(plant_herb_t, runner_leaves), true },
	{ "runner_leaf_size", offsetof(plant_herb_t, runner_leaf_size), false },
	{ "runner_leaf_rise", offsetof(plant_herb_t, runner_leaf_rise), false },
	{ "runner_leaf_turn", offsetof(plant_herb_t, runner_leaf_turn), false },
	{ "stalks_minimum", offsetof(plant_herb_t, stalks_minimum), true },
	{ "stalks_maximum", offsetof(plant_herb_t, stalks_maximum), true },
	{ "stalk_height", offsetof(plant_herb_t, stalk_height), false },
	{ "stalk_lean", offsetof(plant_herb_t, stalk_lean), false },
	{ "stalk_thickness", offsetof(plant_herb_t, stalk_thickness), false },
	{ "stalk_leaves", offsetof(plant_herb_t, stalk_leaves), true },
	{ "stalk_leaves_from", offsetof(plant_herb_t, stalk_leaves_from), false },
	{ "stalk_leaves_to", offsetof(plant_herb_t, stalk_leaves_to), false },
	{ "stalk_leaf_size", offsetof(plant_herb_t, stalk_leaf_size), false },
	{ "stalk_leaf_taper", offsetof(plant_herb_t, stalk_leaf_taper), false },
	{ "stalk_leaf_rise", offsetof(plant_herb_t, stalk_leaf_rise), false },
	{ "stalk_leaf_arch", offsetof(plant_herb_t, stalk_leaf_arch), false },
	{ "stalk_petiole", offsetof(plant_herb_t, stalk_petiole), false },
	{ "stalk_petiole_rise", offsetof(plant_herb_t, stalk_petiole_rise), false },
	{ "stalk_petiole_thickness", offsetof(plant_herb_t, stalk_petiole_thickness), false },
	{ "stalk_leaf_droop", offsetof(plant_herb_t, stalk_leaf_droop), false },
	{ "stalk_leaf_spiral", offsetof(plant_herb_t, stalk_leaf_spiral), true },
	{ "stalk_leaf_scatter", offsetof(plant_herb_t, stalk_leaf_scatter), false },
	{ "head_size", offsetof(plant_herb_t, head_size), false },
	{ "heads", offsetof(plant_herb_t, heads), true },
	{ "head_spike", offsetof(plant_herb_t, head_spike), false },
	{ "head_lean", offsetof(plant_herb_t, head_lean), false },
	{ "head_shape", offsetof(plant_herb_t, head_shape), true },
	{ "head_tilt", offsetof(plant_herb_t, head_tilt), false },
	{ "head_tilt_minimum", offsetof(plant_herb_t, head_tilt_minimum), false },
	{ "cup_size", offsetof(plant_herb_t, cup_size), false },
	{ "cup_depth", offsetof(plant_herb_t, cup_depth), false },
	{ "disc_curve", offsetof(plant_herb_t, disc_curve), false },
	{ "head_neck", offsetof(plant_herb_t, head_neck), false },
	{ "leaf_light", offsetof(plant_herb_t, leaf_light), false },
	{ "light_spread", offsetof(plant_herb_t, light_spread), false },
	{ "head_light", offsetof(plant_herb_t, head_light), false },
	{ "leaf_twist", offsetof(plant_herb_t, leaf_twist), false },
	{ "leaf_fold", offsetof(plant_herb_t, leaf_fold), false },
};

bool plant_herb_set(plant_herb_t *herb, const char *key, const char *value)
{
	for (size_t f = 0; f < sizeof(herb_fields) / sizeof(herb_fields[0]); f++)
	{
		if (strcmp(herb_fields[f].name, key) == 0)
		{
			char *target = (char *)herb + herb_fields[f].offset;

			if (herb_fields[f].whole)
			{
				*(int *)(void *)target = (int)strtol(value, NULL, 10);
			}
			else
			{
				*(float *)(void *)target = strtof(value, NULL);
			}
			herb->given = true;
			return true;
		}
	}
	return false;
}

static void herb_save(FILE *file, const plant_herb_t *herb)
{
	if (!herb->given)
	{
		return;
	}
	for (size_t f = 0; f < sizeof(herb_fields) / sizeof(herb_fields[0]); f++)
	{
		const char *source = (const char *)herb + herb_fields[f].offset;

		if (herb_fields[f].whole)
		{
			fprintf(file, "herb_%s = %d\n", herb_fields[f].name, *(const int *)(const void *)source);
		}
		else
		{
			fprintf(file, "herb_%s = %g\n", herb_fields[f].name, (double)*(const float *)(const void *)source);
		}
	}
}

// The light the plant turns to: the object's +Z, up 45 degrees, give or take
// the spread round (each leaf, each head its own)
static vec3 herb_light(const plant_herb_t *herb)
{
	float spread = fminf(fmaxf(herb->light_spread, 0.0f), 180.0f) * PI_F / 180.0f;
	float azimuth = 0.5f * PI_F + (spread > 0.0f ? random_between(-spread, spread) : 0.0f);
	float level = cosf(PI_F * 0.25f);

	return v3(cosf(azimuth) * level, sinf(PI_F * 0.25f), sinf(azimuth) * level);
}

// A leaf's across, rolled round its middle (heading): its face towards the
// light as far as leaf_light says (never more than 75 degrees), give or
// take leaf_twist at random. Nothing drawn at random when both are 0 (the
// plants of before as they were)
static vec3 herb_leaf_across(const plant_herb_t *herb, vec3 heading, vec3 across)
{
	float roll = 0.0f;
	float most = 75.0f * PI_F / 180.0f;

	if (herb->leaf_light > 0.0f)
	{
		vec3 light = herb_light(herb);
		vec3 face = normalized(cross(across, heading));
		vec3 flat = sub(light, scale(heading, dot(light, heading)));
		float best = atan2f(dot(flat, cross(heading, face)), dot(flat, face));

		roll = fminf(fmaxf(best, -most), most) * fminf(herb->leaf_light, 1.0f);
	}
	if (herb->leaf_twist > 0.0f)
	{
		float twist = fminf(herb->leaf_twist, 90.0f) * PI_F / 180.0f;

		roll += random_between(-twist, twist);
	}
	return roll != 0.0f ? normalized(rotated(across, heading, roll)) : across;
}

// A leaf round the foot: up its stalk, then out along a bent strip
static void herb_leaf(plant_t *plant, const plant_herb_t *herb, float azimuth, float outer)
{
	const cell_place_t *place = &plant->leaves[random_index(plant->leaf_count)];
	vec3 out = v3(cosf(azimuth), 0.0f, sinf(azimuth));
	vec3 across = v3(-out.z, 0.0f, out.x);
	float layers = fminf(fmaxf(herb->leaf_layers, 0.0f), 1.0f);
	float size = plant->leaf_size * (1.0f + fminf(fmaxf(herb->leaf_spread, 0.0f), 1.0f) * random_between(-0.5f, 0.5f))
		* (1.0f - 0.45f * layers * (1.0f - outer));
	float rise = fminf(fmaxf(herb->leaf_rise + random_between(-herb->leaf_rise_spread, herb->leaf_rise_spread)
		+ 25.0f * layers * (1.0f - outer), 0.0f), 89.0f) * PI_F / 180.0f;
	float arch = fmaxf(herb->leaf_arch, 0.0f) * 0.45f * (0.7f + 0.6f * outer) * random_between(0.85f, 1.15f);
	vec3 point = v3(random_between(-0.01f, 0.01f) * plant->leaf_size, 0.01f, random_between(-0.01f, 0.01f) * plant->leaf_size);
	vec3 heading = normalized(add(scale(out, cosf(rise)), v3(0.0f, sinf(rise), 0.0f)));
	vec3 points[6];

	// Its stalk first (as long as the leaf is big)
	if (herb->petiole > 0.0f)
	{
		float petiole_rise = fminf(fmaxf(herb->petiole_rise + random_between(-8.0f, 8.0f), 0.0f), 89.0f) * PI_F / 180.0f;
		vec3 direction = normalized(add(scale(out, cosf(petiole_rise)), v3(0.0f, sinf(petiole_rise), 0.0f)));
		float length = herb->petiole * size / fmaxf(plant->leaf_size, 0.001f);
		float radius = herb->petiole_thickness > 0.0f ? herb->petiole_thickness * size / fmaxf(plant->leaf_size, 0.001f) : 0.008f * size;

		point = add_petiole(plant, point, direction, length, fmaxf(0.001f, radius));
	}
	across = herb_leaf_across(herb, heading, across);
	for (int k = 0; k < 6; k++)
	{
		points[k] = point;
		point = add(point, scale(heading, size / 5.0f));
		heading = normalized(add(heading, v3(0.0f, -arch, 0.0f)));
	}
	add_folded_strip(&plant->mesh, points, 6, across, size * place->aspect, low_normal(add(out, v3(0.0f, 0.3f, 0.0f))), place,
		fmaxf(herb->leaf_fold, 0.0f));
}

#define HEAD_SEGMENTS 10

// A point of a picture laid round a middle: its cell's middle, out to its
// edges at radius 1
static void place_round(const cell_place_t *place, float x, float y, float *u_out, float *v_out)
{
	*u_out = (place->uv[0] + place->uv[2]) * 0.5f + x * (place->uv[2] - place->uv[0]) * 0.5f;
	*v_out = (place->uv[1] + place->uv[3]) * 0.5f + y * (place->uv[3] - place->uv[1]) * 0.5f;
}

// A flat flower on its stalk's top: the flower from above on a disc (its
// middle up a little, its rim down), and, a cup, a cap of a sphere under
// it with the flower from below (the stalk into its foot)
// A disc's axis: tilted at random between the least and the most, round at
// random, or (head_light) towards the light give or take its spread
static vec3 herb_head_axis(const plant_herb_t *herb)
{
	float most = fminf(fmaxf(herb->head_tilt, 0.0f), 85.0f);
	float tilt = random_between(fminf(fmaxf(herb->head_tilt_minimum, 0.0f), most), most) * PI_F / 180.0f;
	float turn;

	if (herb->head_light > 0.0f)
	{
		vec3 light = herb_light(herb);

		turn = atan2f(light.z, light.x);
	}
	else
	{
		turn = random_between(0.0f, 2.0f * PI_F);
	}
	return normalized(v3(cosf(turn) * sinf(tilt), cosf(tilt), sinf(turn) * sinf(tilt)));
}

static void add_flower_disc(plant_t *plant, const plant_herb_t *herb, vec3 top, float size, int head, const vec3 *axis_given)
{
	const cell_place_t *place = &plant->heads[head];
	bool cup = herb->head_shape == PLANT_HEAD_CUP;
	// Under it: its own picture from below, else the leaves' green middle
	const cell_place_t *under = plant->head_has_under[head] ? &plant->heads_under[head] : (plant->leaf_count > 0 ? &plant->leaves[0] : place);
	// (the cup is the middle of the flower from below: as far out on its
	// picture as it is of the disc)
	float under_scale = plant->head_has_under[head] ? fminf(fmaxf(herb->cup_size, 0.05f), 1.0f) : 0.15f;
	vec3 axis = axis_given != NULL ? *axis_given : herb_head_axis(herb);
	vec3 first = normalized(cross(axis, fabsf(axis.x) < 0.9f ? v3(1.0f, 0.0f, 0.0f) : v3(0.0f, 0.0f, 1.0f)));
	vec3 second = cross(axis, first);
	float radius = size * 0.5f;
	float cup_radius = radius * fminf(fmaxf(herb->cup_size, 0.05f), 1.0f);
	float depth = cup_radius * fminf(fmaxf(herb->cup_depth, 0.05f), 2.0f);
	// (the sphere the cup is of: its radius, how far round from its foot)
	float sphere = (cup_radius * cup_radius + depth * depth) / (2.0f * depth);
	float reach = asinf(fminf(cup_radius / sphere, 1.0f));
	// The disc's top: its height over its plane out along it (0 its middle,
	// 1 its rim), its rim a little down and its middle up as much as it is
	// domed (below 0: hollow, its middle under its rim)
	float dome = radius * (0.16f + 0.3f * fminf(fmaxf(herb->disc_curve, -1.0f), 1.0f));
	float rim_height = -radius * 0.08f;
	// (its ring between them where the cup meets it)
	float ring = cup ? fminf(fmaxf(herb->cup_size, 0.2f), 0.8f) : 0.5f;
	float ring_height = rim_height + dome * (1.0f - ring * ring);
	float centre_height = rim_height + dome;
	// Its plane: the cup's rim just under its top there; or, a disc alone,
	// its middle over the stalk's top
	vec3 disc = cup ? add(top, scale(axis, depth - ring_height + radius * 0.01f))
		: add(top, scale(axis, fmaxf(size * 0.03f, radius * 0.02f - centre_height)));
	uint32_t centre;
	uint32_t rim[HEAD_SEGMENTS + 1];
	uint32_t middle[HEAD_SEGMENTS + 1];

	reach = depth > sphere ? PI_F - reach : reach;
	plant->mesh.plant_along = 1.0f;
	plant->mesh.plant_random = hash_unit((uint32_t)plant->mesh.vertex_count * 2654435761u + 29u);
	plant->mesh.plant_extra = 0.0f;
	// The disc: its middle, a ring halfway, its rim (cut out: the petals)
	{
		float u;
		float v;

		place_round(place, 0.0f, 0.0f, &u, &v);
		centre = add_vertex(&plant->mesh, add(disc, scale(axis, centre_height)), axis, first, second, u, v);
	}
	for (int k = 0; k <= HEAD_SEGMENTS; k++)
	{
		float angle = 2.0f * PI_F * (float)k / (float)HEAD_SEGMENTS;
		vec3 out = add(scale(first, cosf(angle)), scale(second, sinf(angle)));
		// (its slope there: out from a dome, in to a hollow's middle)
		float slope = 1.6f * dome / fmaxf(radius, 0.0001f);
		float u;
		float v;

		place_round(place, cosf(angle) * ring, sinf(angle) * ring, &u, &v);
		middle[k] = add_vertex(&plant->mesh, add(disc, add(scale(out, radius * ring), scale(axis, ring_height))),
			normalized(add(axis, scale(out, slope * ring))), first, second, u, v);
		place_round(place, cosf(angle), sinf(angle), &u, &v);
		rim[k] = add_vertex(&plant->mesh, add(disc, add(scale(out, radius), scale(axis, rim_height))),
			normalized(add(axis, scale(out, slope))), first, second, u, v);
	}
	for (int k = 0; k < HEAD_SEGMENTS; k++)
	{
		add_triangle(&plant->mesh, MATERIAL_FOLIAGE, centre, middle[k], middle[k + 1]);
		add_triangle(&plant->mesh, MATERIAL_FOLIAGE, middle[k], rim[k], rim[k + 1]);
		add_triangle(&plant->mesh, MATERIAL_FOLIAGE, middle[k], rim[k + 1], middle[k + 1]);
	}
	if (!cup)
	{
		return;
	}
	// The cup: from its foot (the stalk's top) round to the disc, the
	// flower from below on it as seen from below
	{
		vec3 sphere_middle = add(top, scale(axis, sphere));
		uint32_t rows[3][HEAD_SEGMENTS + 1];
		uint32_t foot;
		float u;
		float v;

		place_round(under, 0.0f, 0.0f, &u, &v);
		foot = add_vertex(&plant->mesh, top, scale(axis, -1.0f), first, second, u, v);
		for (int r = 0; r < 3; r++)
		{
			float round_angle = reach * (float)(r + 1) / 3.0f;

			for (int k = 0; k <= HEAD_SEGMENTS; k++)
			{
				float angle = 2.0f * PI_F * (float)k / (float)HEAD_SEGMENTS;
				vec3 out = add(scale(first, cosf(angle)), scale(second, sinf(angle)));
				vec3 normal = add(scale(axis, -cosf(round_angle)), scale(out, sinf(round_angle)));
				vec3 point = add(sphere_middle, scale(normal, sphere));
				float across = sphere * sinf(round_angle) / cup_radius * under_scale;

				// (from below its left is the other side: mirrored)
				place_round(under, cosf(angle) * across, -sinf(angle) * across, &u, &v);
				rows[r][k] = add_vertex(&plant->mesh, point, normal, first, second, u, v);
			}
		}
		for (int k = 0; k < HEAD_SEGMENTS; k++)
		{
			add_triangle(&plant->mesh, MATERIAL_FRUIT, foot, rows[0][k + 1], rows[0][k]);
			for (int r = 0; r + 1 < 3; r++)
			{
				add_triangle(&plant->mesh, MATERIAL_FRUIT, rows[r][k], rows[r][k + 1], rows[r + 1][k + 1]);
				add_triangle(&plant->mesh, MATERIAL_FRUIT, rows[r][k], rows[r + 1][k + 1], rows[r + 1][k]);
			}
		}
	}
}

// A leaf up a stalk, at a point of it: out (on its own stalk), along a
// bent strip; low: 1 the lowest of them .. 0 the topmost (they droop more
// the lower they are)
static void stalk_leaf(plant_t *plant, const plant_herb_t *herb, vec3 at, float azimuth, float size, float low)
{
	const cell_place_t *place = &plant->leaves[random_index(plant->leaf_count)];
	vec3 out = v3(cosf(azimuth), 0.0f, sinf(azimuth));
	vec3 across = v3(-out.z, 0.0f, out.x);
	float droop = fminf(fmaxf(herb->stalk_leaf_droop, 0.0f), 120.0f) * fminf(fmaxf(low, 0.0f), 1.0f);
	float rise = fminf(fmaxf((herb->stalk_leaf_rise > 0.0f ? herb->stalk_leaf_rise : 30.0f) - droop + random_between(-8.0f, 8.0f), -80.0f),
		89.0f) * PI_F / 180.0f;
	float arch = fmaxf(herb->stalk_leaf_arch, 0.0f) * 0.45f * random_between(0.85f, 1.15f);
	vec3 heading = normalized(add(scale(out, cosf(rise)), v3(0.0f, sinf(rise), 0.0f)));
	vec3 point = at;
	vec3 points[6];

	if (herb->stalk_petiole > 0.0f)
	{
		float petiole_rise = fminf(fmaxf((herb->stalk_petiole_rise > 0.0f ? herb->stalk_petiole_rise : 37.0f) - droop * 0.5f
			+ random_between(-6.0f, 6.0f), -60.0f), 89.0f) * PI_F / 180.0f;
		vec3 direction = normalized(add(scale(out, cosf(petiole_rise)), v3(0.0f, sinf(petiole_rise), 0.0f)));
		float radius = herb->stalk_petiole_thickness > 0.0f ? herb->stalk_petiole_thickness * size / fmaxf(plant->leaf_size, 0.001f)
			: 0.006f * size;

		point = add_petiole(plant, point, direction, herb->stalk_petiole * size / fmaxf(plant->leaf_size, 0.001f), fmaxf(0.001f, radius));
	}
	across = herb_leaf_across(herb, heading, across);
	for (int k = 0; k < 6; k++)
	{
		points[k] = point;
		point = add(point, scale(heading, size / 5.0f));
		heading = normalized(add(heading, v3(0.0f, -arch, 0.0f)));
	}
	add_folded_strip(&plant->mesh, points, 6, across, size * place->aspect, low_normal(add(out, v3(0.0f, 0.3f, 0.0f))), place,
		fmaxf(herb->leaf_fold, 0.0f));
}

// A leaf (or a sprig) out of a runner: at a point of it, out from its way
// (heading, level) to a side, rising, along a bent strip
static void runner_leaf(plant_t *plant, const plant_herb_t *herb, vec3 at, vec3 way, float size)
{
	const cell_place_t *place = &plant->leaves[random_index(plant->leaf_count)];
	float turn_most = (herb->runner_leaf_turn > 0.0f ? herb->runner_leaf_turn : 50.0f) * PI_F / 180.0f;
	float turn = random_between(0.3f, 1.0f) * turn_most * (random_unit() < 0.5f ? -1.0f : 1.0f);
	vec3 out = normalized(rotated(normalized(v3(way.x, 0.0f, way.z)), v3(0.0f, 1.0f, 0.0f), turn));
	vec3 across = normalized(v3(-out.z, 0.0f, out.x));
	float rise = fminf(fmaxf((herb->runner_leaf_rise > 0.0f ? herb->runner_leaf_rise : 35.0f) + random_between(-12.0f, 12.0f), 0.0f), 89.0f)
		* PI_F / 180.0f;
	float arch = fmaxf(herb->leaf_arch, 0.0f) * 0.45f * random_between(0.85f, 1.15f);
	vec3 heading = normalized(add(scale(out, cosf(rise)), v3(0.0f, sinf(rise), 0.0f)));
	vec3 point = at;
	vec3 points[6];

	across = herb_leaf_across(herb, heading, across);
	for (int k = 0; k < 6; k++)
	{
		points[k] = point;
		point = add(point, scale(heading, size / 5.0f));
		heading = normalized(add(heading, v3(0.0f, -arch, 0.0f)));
	}
	add_folded_strip(&plant->mesh, points, 6, across, size * place->aspect, low_normal(add(out, v3(0.0f, 0.3f, 0.0f))), place,
		fmaxf(herb->leaf_fold, 0.0f));
}

// A runner: a thin stem winding along the ground from start (its way at
// first: heading), just over it (now and then a little up), its leaves up
// along it; forks of it, shorter, turning off to the sides
static void grow_runner(plant_t *plant, const plant_herb_t *herb, vec3 start, float heading, float length, int forks, float leaves)
{
	branch_t runner;
	float wander = fminf(fmaxf(herb->runner_wander, 0.0f), 1.0f);
	float radius = fmaxf(herb->runner_thickness > 0.0f ? herb->runner_thickness : 0.002f, 0.0005f);
	int pieces = (int)fminf(fmaxf(length / 0.05f, 4.0f), (float)(MAX_POINTS - 1));
	float piece = length / (float)pieces;
	vec3 point = start;
	int count = (int)(leaves + random_unit());

	runner.count = pieces + 1;
	runner.length = length;
	for (int i = 0; i <= pieces; i++)
	{
		float t = (float)i / (float)pieces;
		vec3 way = v3(cosf(heading), 0.0f, sinf(heading));

		runner.points[i] = point;
		runner.directions[i] = way;
		runner.radii[i] = radius * (1.0f - 0.5f * t);
		heading += random_between(-1.0f, 1.0f) * wander * 0.6f;
		point = add(point, scale(way, piece));
		point.y = radius + fmaxf(0.0f, random_between(-0.5f, 1.0f)) * 0.01f * (1.0f - t);
	}
	for (int i = 1; i <= pieces; i++)
	{
		runner.directions[i] = normalized(sub(runner.points[i], runner.points[i - 1]));
	}
	runner.directions[0] = runner.directions[1];
	add_tube(plant, &runner, 4, radius, MATERIAL_BARK);
	// Its leaves, from a little out of its foot to its tip, smaller there
	for (int k = 0; k < count && plant->leaf_count > 0; k++)
	{
		float t = 0.05f + 0.95f * ((float)k + random_unit()) / (float)count;
		int at = (int)(t * (float)pieces + 0.5f);
		float size = plant->leaf_size * (herb->runner_leaf_size > 0.0f ? herb->runner_leaf_size : 1.0f) * random_between(0.8f, 1.15f)
			* (1.0f - 0.45f * t);

		runner_leaf(plant, herb, runner.points[at], runner.directions[at], size);
	}
	// Its forks: off to a side somewhere along it, about as long as is left
	// of it
	for (int f = 0; f < forks; f++)
	{
		float t = random_between(0.25f, 0.7f);
		int at = (int)(t * (float)pieces + 0.5f);
		float side = random_unit() < 0.5f ? -1.0f : 1.0f;
		float way = atan2f(runner.directions[at].z, runner.directions[at].x) + side * random_between(0.4f, 0.9f);
		float rest = length * (1.0f - t) * random_between(0.7f, 1.0f);

		grow_runner(plant, herb, runner.points[at], way, rest, forks - 1 - f > 0 ? 1 : 0, leaves * rest / fmaxf(length, 0.001f));
	}
}

static void grow_herb(plant_t *plant)
{
	const plant_herb_t *herb = &plant->herb;
	int count = herb->leaves > 2 ? herb->leaves + random_index(3) - 1 : herb->leaves;
	int stalk_minimum = herb->stalks_minimum < 0 ? 0 : herb->stalks_minimum;
	int stalk_maximum = herb->stalks_maximum < stalk_minimum ? stalk_minimum : herb->stalks_maximum;
	int stalks = stalk_minimum + random_index(stalk_maximum - stalk_minimum + 1);

	for (int i = 0; i < count && plant->leaf_count > 0; i++)
	{
		float azimuth = (float)i * 2.0f * PI_F / (float)count + random_between(-0.25f, 0.25f);

		herb_leaf(plant, herb, azimuth, random_unit());
	}
	// Its runners along the ground, round its foot
	for (int r = 0; r < herb->runners; r++)
	{
		float azimuth = (float)r * 2.0f * PI_F / (float)herb->runners + random_between(-0.3f, 0.3f);
		float length = fmaxf(herb->runner_length, 0.05f) * random_between(0.8f, 1.2f);
		int forks = herb->runner_forks < 0 ? 0 : (herb->runner_forks > 4 ? 4 : herb->runner_forks);

		// (out of a little patch round its foot, not one point: no star)
		float reach = random_unit() * fmaxf(herb->runner_length, 0.05f) * 0.15f;
		float place = random_between(0.0f, 2.0f * PI_F);

		grow_runner(plant, herb, v3(cosf(place) * reach, 0.0f, sinf(place) * reach), azimuth, length, forks, (float)herb->runner_leaves);
	}
	for (int s = 0; s < stalks; s++)
	{
		// One out of the very middle; several a little apart round it (as
		// thick as they are, not a fixed few centimeters: a small plant's
		// would stand off to one side of its leaves)
		float apart = stalks > 1 ? fmaxf(herb->stalk_thickness, 0.0005f) * 2.5f : 0.0f;
		vec3 base = v3(random_between(-apart, apart), 0.0f, random_between(-apart, apart));
		vec3 tilt = scale(random_direction(), tanf(fminf(fmaxf(herb->stalk_lean, 0.0f), 80.0f) * PI_F / 180.0f) * random_unit());
		vec3 direction = normalized(add(v3(0.0f, 1.0f, 0.0f), v3(tilt.x, 0.0f, tilt.z)));
		float length = plant->height * fmaxf(herb->stalk_height, 0.05f) * random_between(0.8f, 1.05f);
		// (as crooked as it leans: an upright one straight, its head over
		// its foot)
		branch_t stalk;
		float bend = 0.1f + 0.7f * fminf(fmaxf(herb->stalk_lean, 0.0f) / 20.0f, 1.0f);
		bool disc_head = herb->head_shape != PLANT_HEAD_CROSSED && plant->head_count > 0;
		// Its disc's tilt chosen first where the stalk bends into it or it
		// faces the light (else as it always was: when the disc is made)
		bool axis_first = disc_head && (herb->head_neck > 0.0f || herb->head_light > 0.0f);
		vec3 axis = v3(0.0f, 1.0f, 0.0f);
		vec3 top;
		int heads = herb->heads < 1 ? 1 : herb->heads;

		if (axis_first)
		{
			axis = herb_head_axis(herb);
		}
		if (disc_head && herb->head_neck > 0.0f)
		{
			// Its top part bending over into the disc's tilt (finer, to bend
			// smoothly): each piece's heading turned further towards it
			float neck = fminf(herb->head_neck, 0.5f);
			int start;

			trace_branch(&stalk, base, direction, length, fmaxf(herb->stalk_thickness, 0.0005f), 0.7f, bend, 0.4f, length / 20.0f);
			start = (int)((float)(stalk.count - 1) * (1.0f - neck));
			start = start < 1 ? 1 : start;
			{
				vec3 before = stalk.points[start - 1];

				for (int i = start; i < stalk.count; i++)
				{
					float share = (float)(i - start + 1) / (float)(stalk.count - start);
					float turn = share * share * (3.0f - 2.0f * share);
					vec3 grown = stalk.points[i];
					float piece = sqrtf(dot(sub(grown, before), sub(grown, before)));
					vec3 heading = normalized(add(scale(normalized(sub(grown, before)), 1.0f - turn), scale(axis, turn)));

					before = grown;
					stalk.directions[i] = heading;
					stalk.points[i] = add(stalk.points[i - 1], scale(heading, piece));
				}
			}
			add_tube(plant, &stalk, 6, fmaxf(herb->stalk_thickness, 0.0005f), MATERIAL_BARK);
			top = stalk.points[stalk.count - 1];
		}
		else
		{
			top = add_traced_stalk(plant, base, direction, length, fmaxf(herb->stalk_thickness, 0.0005f), bend, &stalk);
		}

		// Its leaves up it: between from and to, one after the other (round
		// it by the golden angle, or at random), smaller up it
		{
			bool given_range = herb->stalk_leaves_from > 0.0f || herb->stalk_leaves_to > 0.0f;
			float from = given_range ? fminf(fmaxf(herb->stalk_leaves_from, 0.0f), 1.0f) : 0.1f;
			float to = given_range ? fminf(fmaxf(herb->stalk_leaves_to, from), 1.0f) : 0.6f;
			float turn = random_between(0.0f, 2.0f * PI_F);

			for (int k = 0; k < herb->stalk_leaves && plant->leaf_count > 0; k++)
			{
				float share = herb->stalk_leaves > 1 ? (float)k / (float)(herb->stalk_leaves - 1) : 0.5f;
				float t = herb->stalk_leaf_spiral ? from + (to - from) * share + random_between(-0.02f, 0.02f) : random_between(from, to);
				int point = (int)(fminf(fmaxf(t, 0.0f), 1.0f) * (float)(stalk.count - 1) + 0.5f);
				float scatter = fminf(fmaxf(herb->stalk_leaf_scatter, 0.0f), 180.0f) * PI_F / 180.0f;
				float azimuth = (herb->stalk_leaf_spiral ? turn + (float)k * GOLDEN_ANGLE_DEGREES * PI_F / 180.0f : random_between(0.0f, 2.0f * PI_F))
					+ random_between(-scatter, scatter);
				float up = herb->stalk_leaf_spiral ? share : (t - from) / fmaxf(to - from, 0.01f);
				float size = plant->leaf_size * (herb->stalk_leaf_size > 0.0f ? herb->stalk_leaf_size : 0.55f) * random_between(0.85f, 1.15f)
					* (1.0f - fminf(fmaxf(herb->stalk_leaf_taper, 0.0f), 1.0f) * up);

				if (size > plant->leaf_size * 0.03f)
				{
					stalk_leaf(plant, herb, stalk.points[point], azimuth, size, 1.0f - up);
				}
			}
		}
		// Its head on top; or heads up its top part, smaller up it
		for (int h = 0; h < heads; h++)
		{
			float t = heads > 1 ? 1.0f - fminf(fmaxf(herb->head_spike, 0.0f), 1.0f) * (float)h / (float)(heads - 1) : 1.0f;
			vec3 at = add(base, scale(sub(top, base), t));

			float size = plant->height * fmaxf(herb->head_size, 0.01f) * (heads > 1 ? 0.75f + 0.5f * (1.0f - t) : 1.0f);

			// (the top one on the stalk's top, a disc or cards; the others round it)
			if (h == 0 && disc_head)
			{
				add_flower_disc(plant, herb, at, size, random_index(plant->head_count), axis_first ? &axis : NULL);
			}
			else if (h == 0)
			{
				add_head(plant, at, size);
			}
			else if (herb->head_lean > 0.0f)
			{
				// (on the stalk as it grew, not the line from its foot to its top)
				int point = (int)(fminf(fmaxf(t, 0.0f), 1.0f) * (float)(stalk.count - 1) + 0.5f);

				add_head_out(plant, stalk.points[point], size, herb->head_lean);
			}
			else
			{
				add_head_around(plant, at, size);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Mushrooms
//
// Made here, of the recipe's shape (plant_mushroom_t): a stem, a tube along
// an arc (bent towards the cap, a bulb at its foot, a skirt round it), and a
// cap, a solid of revolution of a profile (a cone or a dome, a funnel, a bump
// in its middle, its rim bent and wavy), the gills under it. Their texture is
// laid out as the template (plant_mushroom_template): the cap from above, the
// gills from below, the skirt, the stem round, each in its own place on it
// (MUSHROOM_CAP_ ...); without a picture, one made from the recipe's colors.

#define MUSHROOM_STEM_SIDES 10
#define MUSHROOM_CAP_SIDES 16
#define MUSHROOM_TEXTURE_SIZE 512

// The template's places (0..1 of the picture, its first row the top): the
// cap a circle, the gills a circle, the skirt and the stem bands (round them
// across, the band's left edge meeting its right)
static const float MUSHROOM_CAP_CIRCLE[3] = { 0.30f, 0.30f, 0.28f };      // middle x, y, radius
static const float MUSHROOM_GILLS_CIRCLE[3] = { 0.80f, 0.19f, 0.17f };
static const float MUSHROOM_RING_BAND[4] = { 0.62f, 0.40f, 0.98f, 0.58f };  // left, top, right, bottom
static const float MUSHROOM_STEM_BAND[4] = { 0.02f, 0.63f, 0.98f, 0.98f };

// The presets: what common mushrooms look like (all of a mushroom's
// height; colors sRGB)
typedef struct
{
	const char *name;
	plant_mushroom_t mushroom;
} mushroom_preset_t;

static const mushroom_preset_t mushroom_presets[] =
{
	// Amanita muscaria: a red dome, white warts, white gills, a skirt, a bulb
	{ "fly_agaric", {
		.stem_thickness = 0.07f, .stem_taper = 0.8f, .stem_bulb = 0.6f, .stem_bend = 0.15f,
		.ring_height = 0.6f, .ring_width = 0.8f, .ring_droop = 0.6f,
		.cap_width = 0.42f, .cap_height = 0.26f, .cap_dome = 0.8f, .cap_curl = 0.2f, .cap_wave = 0.3f,
		.cap_color = { 0.80f, 0.10f, 0.06f }, .cap_rim_color = { 0.90f, 0.38f, 0.12f }, .cap_center_color = { 0.75f, 0.08f, 0.05f },
		.cap_center = 0.2f, .spots = 0.7f, .spot_size = 0.5f, .spot_color = { 0.95f, 0.93f, 0.84f },
		.gills_color = { 0.94f, 0.91f, 0.82f }, .stem_color = { 0.93f, 0.91f, 0.85f }, .stem_pattern_color = { 0.85f, 0.83f, 0.76f },
		.stem_bands = 0.15f } },
	// Boletus edulis: a brown bun, paler at its rim, pores, a fat club of a
	// stem with a white net up it
	{ "porcini", {
		.stem_thickness = 0.14f, .stem_taper = 0.65f, .stem_bulb = 0.6f, .stem_bend = 0.05f,
		.cap_width = 0.45f, .cap_height = 0.3f, .cap_dome = 1.0f, .cap_curl = 0.15f, .cap_wave = 0.12f, .underside = PLANT_MUSHROOM_PORES,
		.cap_color = { 0.45f, 0.27f, 0.13f }, .cap_rim_color = { 0.62f, 0.45f, 0.28f }, .cap_center_color = { 0.40f, 0.24f, 0.11f },
		.cap_center = 0.5f, .spot_color = { 0.5f, 0.3f, 0.15f },
		.gills_color = { 0.86f, 0.84f, 0.62f }, .stem_color = { 0.88f, 0.84f, 0.72f }, .stem_pattern_color = { 0.97f, 0.96f, 0.92f },
		.stem_net = 0.6f } },
	// Cantharellus cibarius: an egg yellow funnel, its rim wavy, ridges
	// running down a stem that widens up into it
	{ "chanterelle", {
		.stem_thickness = 0.06f, .stem_taper = 1.8f, .stem_bend = 0.15f,
		.cap_width = 0.36f, .cap_height = 0.18f, .cap_dome = 0.5f, .cap_funnel = 0.9f, .cap_curl = 0.15f, .cap_wave = 0.7f,
		.underside = PLANT_MUSHROOM_RIDGES,
		.cap_color = { 0.96f, 0.66f, 0.12f }, .cap_rim_color = { 0.98f, 0.74f, 0.25f }, .cap_center_color = { 0.93f, 0.62f, 0.12f },
		.cap_center = 0.3f, .spot_color = { 0.9f, 0.6f, 0.1f },
		.gills_color = { 0.96f, 0.68f, 0.18f }, .stem_color = { 0.95f, 0.70f, 0.22f }, .stem_pattern_color = { 0.93f, 0.64f, 0.16f } } },
	// Macrolepiota procera: tall, thin, its broad flat cap pale with brown
	// scales in rings and a dark bump, a snakeskin stem, a skirt, a bulb
	{ "parasol", {
		.stem_length = 0.85f, .stem_thickness = 0.03f, .stem_taper = 0.75f, .stem_bulb = 1.0f, .stem_bend = 0.08f,
		.ring_height = 0.8f, .ring_width = 1.4f, .ring_droop = 0.25f,
		.cap_width = 0.42f, .cap_height = 0.09f, .cap_dome = 0.6f, .cap_umbo = 0.8f, .cap_curl = 0.2f, .cap_wave = 0.25f,
		.cap_color = { 0.86f, 0.80f, 0.70f }, .cap_rim_color = { 0.90f, 0.86f, 0.78f }, .cap_center_color = { 0.42f, 0.30f, 0.20f },
		.cap_center = 0.25f, .scales = 0.8f, .spot_color = { 0.50f, 0.37f, 0.26f },
		.gills_color = { 0.95f, 0.93f, 0.88f }, .stem_color = { 0.85f, 0.80f, 0.72f }, .stem_pattern_color = { 0.50f, 0.38f, 0.28f },
		.stem_bands = 0.8f } },
	// Armillaria mellea: in clumps on wood, honey colored, dark scales in
	// its middle, a skirt, bent stems
	{ "honey_fungus", {
		.stem_thickness = 0.05f, .stem_taper = 1.0f, .stem_bulb = 0.2f, .stem_bend = 0.45f,
		.ring_height = 0.75f, .ring_width = 0.6f, .ring_droop = 0.5f,
		.cap_width = 0.38f, .cap_height = 0.14f, .cap_dome = 0.6f, .cap_funnel = 0.1f, .cap_umbo = 0.3f, .cap_curl = 0.1f, .cap_wave = 0.35f,
		.cap_color = { 0.72f, 0.52f, 0.24f }, .cap_rim_color = { 0.82f, 0.66f, 0.36f }, .cap_center_color = { 0.42f, 0.28f, 0.14f },
		.cap_center = 0.35f, .scales = 0.4f, .spot_color = { 0.38f, 0.25f, 0.12f },
		.gills_color = { 0.90f, 0.84f, 0.70f }, .stem_color = { 0.78f, 0.62f, 0.38f }, .stem_pattern_color = { 0.55f, 0.40f, 0.22f },
		.stem_bands = 0.2f } },
	// Agaricus campestris: white, a dome flattening, pinkish brown gills,
	// a thin skirt
	{ "field_mushroom", {
		.stem_thickness = 0.09f, .stem_taper = 0.9f, .stem_bulb = 0.1f, .stem_bend = 0.05f,
		.ring_height = 0.6f, .ring_width = 0.5f, .ring_droop = 0.4f,
		.cap_width = 0.5f, .cap_height = 0.22f, .cap_dome = 0.9f, .cap_curl = 0.35f, .cap_wave = 0.1f,
		.cap_color = { 0.95f, 0.93f, 0.89f }, .cap_rim_color = { 0.96f, 0.95f, 0.92f }, .cap_center_color = { 0.88f, 0.84f, 0.78f },
		.cap_center = 0.2f, .streaks = 0.15f, .spot_color = { 0.85f, 0.8f, 0.74f },
		.gills_color = { 0.62f, 0.38f, 0.30f }, .stem_color = { 0.95f, 0.94f, 0.90f }, .stem_pattern_color = { 0.9f, 0.88f, 0.84f } } },
	// Amanita phalloides: olive green, streaked, white gills, a skirt, its
	// foot in a cup (deadly)
	{ "death_cap", {
		.stem_thickness = 0.065f, .stem_taper = 0.75f, .stem_bulb = 0.4f, .stem_volva = 0.8f, .stem_bend = 0.08f,
		.ring_height = 0.75f, .ring_width = 0.8f, .ring_droop = 0.7f,
		.cap_width = 0.42f, .cap_height = 0.18f, .cap_dome = 0.75f, .cap_curl = 0.05f, .cap_wave = 0.1f,
		.cap_color = { 0.62f, 0.64f, 0.38f }, .cap_rim_color = { 0.78f, 0.78f, 0.55f }, .cap_center_color = { 0.50f, 0.52f, 0.28f },
		.cap_center = 0.3f, .streaks = 0.6f, .spot_color = { 0.9f, 0.9f, 0.85f },
		.gills_color = { 0.96f, 0.95f, 0.90f }, .stem_color = { 0.93f, 0.93f, 0.88f }, .stem_pattern_color = { 0.75f, 0.77f, 0.60f },
		.stem_bands = 0.2f } },
	// Russula: a bright smooth cap, flattening and dipping, white gills and stem
	{ "russula", {
		.stem_length = 0.55f, .stem_thickness = 0.1f, .stem_taper = 0.95f, .stem_bend = 0.05f,
		.cap_width = 0.48f, .cap_height = 0.14f, .cap_dome = 0.7f, .cap_funnel = 0.35f, .cap_curl = -0.1f, .cap_wave = 0.2f,
		.cap_color = { 0.78f, 0.12f, 0.18f }, .cap_rim_color = { 0.86f, 0.30f, 0.32f }, .cap_center_color = { 0.55f, 0.07f, 0.12f },
		.cap_center = 0.3f, .spot_color = { 0.9f, 0.85f, 0.8f },
		.gills_color = { 0.97f, 0.95f, 0.88f }, .stem_color = { 0.96f, 0.95f, 0.92f }, .stem_pattern_color = { 0.9f, 0.88f, 0.84f } } },
	// Lactarius deliciosus: orange, rings of darker orange round its dip,
	// orange gills, its stem pitted
	{ "saffron_milk_cap", {
		.stem_length = 0.45f, .stem_thickness = 0.09f, .stem_taper = 0.9f, .stem_bend = 0.08f,
		.cap_width = 0.48f, .cap_height = 0.16f, .cap_dome = 0.5f, .cap_funnel = 0.6f, .cap_curl = 0.25f, .cap_wave = 0.25f,
		.cap_color = { 0.90f, 0.48f, 0.20f }, .cap_rim_color = { 0.92f, 0.55f, 0.28f }, .cap_center_color = { 0.75f, 0.36f, 0.14f },
		.cap_center = 0.15f, .zones = 0.8f, .spot_color = { 0.7f, 0.35f, 0.12f },
		.gills_color = { 0.92f, 0.50f, 0.22f }, .stem_color = { 0.90f, 0.55f, 0.28f }, .stem_pattern_color = { 0.82f, 0.40f, 0.16f },
		.stem_net = 0.25f } },
	// Coprinus comatus: a tall white bell round its stem, shaggy scales,
	// its gills going black
	{ "shaggy_ink_cap", {
		.stem_thickness = 0.05f, .stem_taper = 0.85f, .stem_bulb = 0.1f, .stem_bend = 0.05f,
		.ring_height = 0.3f, .ring_width = 0.4f, .ring_droop = 0.2f,
		.cap_width = 0.17f, .cap_height = 0.65f, .cap_dome = 1.0f, .cap_wave = 0.1f,
		.cap_color = { 0.95f, 0.94f, 0.90f }, .cap_rim_color = { 0.80f, 0.76f, 0.72f }, .cap_center_color = { 0.70f, 0.60f, 0.48f },
		.cap_center = 0.15f, .scales = 0.9f, .spot_color = { 0.86f, 0.80f, 0.70f },
		.gills_color = { 0.30f, 0.25f, 0.25f }, .stem_color = { 0.96f, 0.95f, 0.92f }, .stem_pattern_color = { 0.9f, 0.88f, 0.84f } } },
	// Lycoperdon: a pale ball on a short thick foot, warty
	{ "puffball", {
		.stem_length = 0.3f, .stem_thickness = 0.25f, .stem_taper = 1.3f,
		.cap_width = 0.48f, .cap_height = 0.5f, .cap_dome = 1.0f, .cap_curl = 0.7f, .cap_wave = 0.05f,
		.cap_color = { 0.95f, 0.94f, 0.90f }, .cap_rim_color = { 0.92f, 0.90f, 0.85f }, .cap_center_color = { 0.90f, 0.87f, 0.80f },
		.cap_center = 0.3f, .spots = 1.0f, .spot_size = 0.12f, .spot_color = { 0.84f, 0.82f, 0.75f },
		.gills_color = { 0.92f, 0.90f, 0.85f }, .stem_color = { 0.94f, 0.92f, 0.87f }, .stem_pattern_color = { 0.88f, 0.86f, 0.8f } } },
};

int plant_mushroom_preset_count(void)
{
	return (int)(sizeof(mushroom_presets) / sizeof(mushroom_presets[0]));
}

const char *plant_mushroom_preset_name(int preset)
{
	return preset >= 0 && preset < plant_mushroom_preset_count() ? mushroom_presets[preset].name : "";
}

bool plant_mushroom_preset(plant_mushroom_t *mushroom, const char *name)
{
	for (int i = 0; i < plant_mushroom_preset_count(); i++)
	{
		if (strcmp(mushroom_presets[i].name, name) == 0)
		{
			// (its size the one chosen: a preset is a shape)
			float size = mushroom->size;

			*mushroom = mushroom_presets[i].mushroom;
			mushroom->size = size;
			return true;
		}
	}
	return false;
}

void plant_mushroom_defaults(plant_mushroom_t *mushroom)
{
	*mushroom = mushroom_presets[0].mushroom;
}

// A recipe's key for a mushroom's shape or colors: true if it was one
static bool mushroom_key(plant_mushroom_t *mushroom, const char *key, const char *value)
{
	static const struct { const char *name; size_t offset; } numbers[] =
	{
		{ "size", offsetof(plant_mushroom_t, size) },
		{ "stem_length", offsetof(plant_mushroom_t, stem_length) }, { "stem_thickness", offsetof(plant_mushroom_t, stem_thickness) },
		{ "stem_taper", offsetof(plant_mushroom_t, stem_taper) }, { "stem_bulb", offsetof(plant_mushroom_t, stem_bulb) },
		{ "stem_volva", offsetof(plant_mushroom_t, stem_volva) }, { "stem_bend", offsetof(plant_mushroom_t, stem_bend) },
		{ "ring_height", offsetof(plant_mushroom_t, ring_height) }, { "ring_width", offsetof(plant_mushroom_t, ring_width) },
		{ "ring_droop", offsetof(plant_mushroom_t, ring_droop) }, { "cap_width", offsetof(plant_mushroom_t, cap_width) },
		{ "cap_height", offsetof(plant_mushroom_t, cap_height) }, { "cap_dome", offsetof(plant_mushroom_t, cap_dome) },
		{ "cap_funnel", offsetof(plant_mushroom_t, cap_funnel) }, { "cap_umbo", offsetof(plant_mushroom_t, cap_umbo) },
		{ "cap_curl", offsetof(plant_mushroom_t, cap_curl) }, { "cap_wave", offsetof(plant_mushroom_t, cap_wave) },
		{ "cap_center", offsetof(plant_mushroom_t, cap_center) }, { "spots", offsetof(plant_mushroom_t, spots) },
		{ "scales", offsetof(plant_mushroom_t, scales) }, { "streaks", offsetof(plant_mushroom_t, streaks) },
		{ "zones", offsetof(plant_mushroom_t, zones) }, { "stem_net", offsetof(plant_mushroom_t, stem_net) },
		{ "spot_size", offsetof(plant_mushroom_t, spot_size) },
		{ "stem_bands", offsetof(plant_mushroom_t, stem_bands) },
	};
	static const struct { const char *name; size_t offset; } colors[] =
	{
		{ "cap_color", offsetof(plant_mushroom_t, cap_color) }, { "cap_rim_color", offsetof(plant_mushroom_t, cap_rim_color) },
		{ "cap_center_color", offsetof(plant_mushroom_t, cap_center_color) }, { "spot_color", offsetof(plant_mushroom_t, spot_color) },
		{ "gills_color", offsetof(plant_mushroom_t, gills_color) }, { "stem_color", offsetof(plant_mushroom_t, stem_color) },
		{ "stem_pattern_color", offsetof(plant_mushroom_t, stem_pattern_color) },
	};
	static const char *const undersides[3] = { "gills", "pores", "ridges" };

	if (strcmp(key, "preset") == 0)
	{
		return plant_mushroom_preset(mushroom, value);
	}
	if (strcmp(key, "underside") == 0)
	{
		for (int i = 0; i < 3; i++)
		{
			if (strcmp(value, undersides[i]) == 0)
			{
				mushroom->underside = i;
				return true;
			}
		}
		return false;
	}
	for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++)
	{
		if (strcmp(key, numbers[i].name) == 0)
		{
			*(float *)((char *)mushroom + numbers[i].offset) = strtof(value, NULL);
			return true;
		}
	}
	for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++)
	{
		float *color = (float *)((char *)mushroom + colors[i].offset);

		if (strcmp(key, colors[i].name) == 0)
		{
			sscanf(value, "%f, %f, %f", &color[0], &color[1], &color[2]);
			return true;
		}
	}
	return false;
}

bool plant_mushroom_set(plant_mushroom_t *mushroom, const char *key, const char *value)
{
	return mushroom_key(mushroom, key, value);
}

static void save_color(FILE *file, const char *key, const float color[3])
{
	fprintf(file, "%s = %g, %g, %g\n", key, (double)color[0], (double)color[1], (double)color[2]);
}

static void mushroom_save(FILE *file, const plant_mushroom_t *mushroom)
{
	static const char *const undersides[3] = { "gills", "pores", "ridges" };

	fprintf(file, "size = %g\n", (double)(mushroom->size > 0.0f ? mushroom->size : 1.0f));
	fprintf(file, "stem_length = %g\nstem_thickness = %g\nstem_taper = %g\nstem_bulb = %g\nstem_volva = %g\nstem_bend = %g\n",
		(double)mushroom->stem_length, (double)mushroom->stem_thickness, (double)mushroom->stem_taper, (double)mushroom->stem_bulb,
		(double)mushroom->stem_volva, (double)mushroom->stem_bend);
	fprintf(file, "ring_height = %g\nring_width = %g\nring_droop = %g\n", (double)mushroom->ring_height, (double)mushroom->ring_width,
		(double)mushroom->ring_droop);
	fprintf(file, "cap_width = %g\ncap_height = %g\ncap_dome = %g\ncap_funnel = %g\ncap_umbo = %g\ncap_curl = %g\ncap_wave = %g\n",
		(double)mushroom->cap_width, (double)mushroom->cap_height, (double)mushroom->cap_dome, (double)mushroom->cap_funnel,
		(double)mushroom->cap_umbo, (double)mushroom->cap_curl, (double)mushroom->cap_wave);
	fprintf(file, "underside = %s\n", undersides[mushroom->underside >= 0 && mushroom->underside < 3 ? mushroom->underside : 0]);
	save_color(file, "cap_color", mushroom->cap_color);
	save_color(file, "cap_rim_color", mushroom->cap_rim_color);
	save_color(file, "cap_center_color", mushroom->cap_center_color);
	fprintf(file, "cap_center = %g\nspots = %g\nspot_size = %g\nscales = %g\nstreaks = %g\nzones = %g\n", (double)mushroom->cap_center,
		(double)mushroom->spots, (double)mushroom->spot_size, (double)mushroom->scales, (double)mushroom->streaks, (double)mushroom->zones);
	save_color(file, "spot_color", mushroom->spot_color);
	save_color(file, "gills_color", mushroom->gills_color);
	save_color(file, "stem_color", mushroom->stem_color);
	save_color(file, "stem_pattern_color", mushroom->stem_pattern_color);
	fprintf(file, "stem_net = %g\nstem_bands = %g\n", (double)mushroom->stem_net, (double)mushroom->stem_bands);
}

// A mushroom's texture: the recipe's first leaf picture, else the one made
// from its colors (data/mushrooms/<its object's name>_colors.png)
static void mushroom_texture_path(const plant_recipe_t *recipe, const char *data_directory, char *out, size_t out_size)
{
	const char *slash;
	const char *dot;
	const char *name;
	int length;

	if (recipe->leaves.count > 0)
	{
		snprintf(out, out_size, "%s", recipe->leaves.pictures[0].color);
		return;
	}
	slash = strrchr(recipe->output, '/');
	name = slash != NULL ? slash + 1 : recipe->output;
	dot = strrchr(name, '.');
	length = dot != NULL ? (int)(dot - name) : (int)strlen(name);
	if (length == 0)
	{
		name = "mushroom";
		length = 8;
	}
	snprintf(out, out_size, "%s/mushrooms/%.*s_colors.png", data_directory, length, name);
}

// What of a texture the atlas keeps (what is there of it, on alpha): x, y,
// width, height, 0..1 of it
static void mushroom_texture_crop(const char *path, float crop_out[4])
{
	int width = 0;
	int height = 0;
	uint8_t *pixels = load_image(path, &width, &height);
	int box[4];
	float anchor;

	crop_out[0] = 0.0f;
	crop_out[1] = 0.0f;
	crop_out[2] = 1.0f;
	crop_out[3] = 1.0f;
	if (pixels == NULL || width <= 0 || height <= 0)
	{
		return;
	}
	picture_box(pixels, width, height, box, &anchor);
	stbi_image_free(pixels);
	crop_out[0] = (float)box[0] / (float)width;
	crop_out[1] = (float)box[1] / (float)height;
	crop_out[2] = (float)box[2] / (float)width;
	crop_out[3] = (float)box[3] / (float)height;
}

// A place on the template (0..1, its first row the top) in the atlas
static void mushroom_uv(const plant_t *plant, float x, float y, float *u_out, float *v_out)
{
	const cell_place_t *place = &plant->mushroom_place;
	float across = fminf(fmaxf((x - plant->mushroom_crop[0]) / fmaxf(plant->mushroom_crop[2], 1e-4f), 0.0f), 1.0f);
	float down = fminf(fmaxf((y - plant->mushroom_crop[1]) / fmaxf(plant->mushroom_crop[3], 1e-4f), 0.0f), 1.0f);

	*u_out = place->uv[0] + (place->uv[2] - place->uv[0]) * across;
	*v_out = place->uv[1] + (place->uv[3] - place->uv[1]) * down;
}

// Smooth value noise on the picture (0..1), for the texture made here
static float mushroom_noise(float x, float y, uint32_t seed)
{
	int cell_x = (int)floorf(x);
	int cell_y = (int)floorf(y);
	float fraction_x = x - (float)cell_x;
	float fraction_y = y - (float)cell_y;
	float weight_x = fraction_x * fraction_x * (3.0f - 2.0f * fraction_x);
	float weight_y = fraction_y * fraction_y * (3.0f - 2.0f * fraction_y);
	float corner[4];

	for (int k = 0; k < 4; k++)
	{
		uint32_t value = (uint32_t)(cell_x + (k & 1)) * 73856093u ^ (uint32_t)(cell_y + (k >> 1)) * 19349663u ^ seed * 83492791u;

		corner[k] = hash_unit(value);
	}
	return (corner[0] * (1.0f - weight_x) + corner[1] * weight_x) * (1.0f - weight_y) + (corner[2] * (1.0f - weight_x) + corner[3] * weight_x)
		* weight_y;
}

static void mix_color(const float a[3], const float b[3], float t, float out[3])
{
	for (int k = 0; k < 3; k++)
	{
		out[k] = a[k] + (b[k] - a[k]) * t;
	}
}

// The cap's texture at a place (t: out from its middle, 0..1): its colors
// out to its rim, its streaks, rings, scales and warts
static void mushroom_cap_texel(const plant_mushroom_t *mushroom, float px, float py, float t, float (*spot)[4], int spot_count, float color[3])
{
	float angle = atan2f(py - MUSHROOM_CAP_CIRCLE[1], px - MUSHROOM_CAP_CIRCLE[0]);
	float round = (angle + PI_F) / (2.0f * PI_F);
	float grain = mushroom_noise(px * 90.0f, py * 90.0f, 1u) * 0.6f + mushroom_noise(px * 23.0f, py * 23.0f, 2u) * 0.4f;
	float middle[3];

	mix_color(mushroom->cap_center_color, mushroom->cap_color, smooth_step(0.0f, 0.08f + 0.85f * fminf(fmaxf(mushroom->cap_center, 0.0f), 1.0f), t),
		middle);
	mix_color(middle, mushroom->cap_rim_color, smooth_step(0.55f, 1.0f, t), color);
	// Rings of the middle's color round it
	if (mushroom->zones > 0.0f)
	{
		float ring = 0.5f + 0.5f * sinf(t * 30.0f + 2.0f * mushroom_noise(round * 7.0f, t * 3.0f, 12u));

		mix_color(color, mushroom->cap_center_color, fminf(mushroom->zones, 1.0f) * 0.6f * ring * ring * smooth_step(0.12f, 0.3f, t), color);
	}
	// Fine streaks out from the middle
	if (mushroom->streaks > 0.0f)
	{
		float fibril = mushroom_noise(round * 260.0f, t * 5.0f, 11u) * 0.7f + mushroom_noise(round * 90.0f, t * 2.0f, 13u) * 0.3f;
		float dark = fminf(mushroom->streaks, 1.0f) * 0.35f * (fibril - 0.45f) * 2.0f * smooth_step(0.08f, 0.6f, t);

		for (int k = 0; k < 3; k++)
		{
			color[k] *= 1.0f - dark;
		}
	}
	for (int k = 0; k < 3; k++)
	{
		color[k] *= 0.92f + 0.16f * grain;
	}
	// Scales in rings round it: more of them the farther out, each a ragged
	// patch, a little apart
	if (mushroom->scales > 0.0f && t < 0.97f)
	{
		const int rings = 8;
		int ring = (int)(t * (float)rings);
		int count = 5 + ring * 4;
		float offset = (float)ring * 0.37f;
		int sector = (int)floorf(round * (float)count + offset);
		float along = t * (float)rings - (float)ring;
		float across = round * (float)count + offset - (float)sector;
		uint32_t cell = (uint32_t)(ring * 131 + (sector % count + count) % count);

		if (ring >= 1 && hash_unit(cell * 2246822519u + 7u) < fminf(mushroom->scales, 1.0f))
		{
			float dx = (across - 0.5f) / 0.46f;
			float dy = (along - 0.45f) / 0.4f;
			float ragged = 0.75f + 0.35f * mushroom_noise(across * 6.0f + (float)cell, along * 6.0f, 15u);

			if (dx * dx + dy * dy < ragged * ragged)
			{
				float shade = 0.85f + 0.2f * mushroom_noise(px * 160.0f, py * 160.0f, 9u);

				for (int k = 0; k < 3; k++)
				{
					color[k] = mushroom->spot_color[k] * shade;
				}
			}
		}
	}
	// Warts
	for (int i = 0; i < spot_count; i++)
	{
		float dx = px - spot[i][0];
		float dy = py - spot[i][1];
		float distance = sqrtf(dx * dx + dy * dy);
		float ragged;

		if (distance > spot[i][2] * 1.4f)
		{
			continue;
		}
		ragged = spot[i][2] * (0.8f + 0.4f * mushroom_noise(atan2f(dy, dx) * 1.6f + spot[i][3] * 3.1f, spot[i][3], 7u));
		if (distance < ragged)
		{
			float shade = 0.85f + 0.15f * mushroom_noise(px * 200.0f, py * 200.0f, 9u);

			for (int k = 0; k < 3; k++)
			{
				color[k] = mushroom->spot_color[k] * shade;
			}
		}
	}
}

// Under the cap (t: out from its middle): gills, pores or ridges; the
// stem's stub in its middle, the cap's rim round it
static void mushroom_underside_texel(const plant_mushroom_t *mushroom, float px, float py, float t, float color[3])
{
	float angle = atan2f(py - MUSHROOM_GILLS_CIRCLE[1], px - MUSHROOM_GILLS_CIRCLE[0]);
	float round = (angle + PI_F) / (2.0f * PI_F);
	float shade = 1.0f;

	memcpy(color, mushroom->gills_color, sizeof(float) * 3);
	if (mushroom->underside == PLANT_MUSHROOM_PORES)
	{
		// A sponge: little round holes, a little dark
		float grid_x = px * (float)MUSHROOM_TEXTURE_SIZE / 3.4f;
		float grid_y = py * (float)MUSHROOM_TEXTURE_SIZE / 3.4f;
		int cell_x = (int)floorf(grid_x);
		int cell_y = (int)floorf(grid_y);
		float nearest = 9.0f;

		for (int dy = -1; dy <= 1; dy++)
		{
			for (int dx = -1; dx <= 1; dx++)
			{
				uint32_t cell = (uint32_t)(cell_x + dx) * 73856093u ^ (uint32_t)(cell_y + dy) * 19349663u;
				float hole_x = (float)(cell_x + dx) + 0.2f + 0.6f * hash_unit(cell);
				float hole_y = (float)(cell_y + dy) + 0.2f + 0.6f * hash_unit(cell * 3u + 1u);

				nearest = fminf(nearest, hypotf(grid_x - hole_x, grid_y - hole_y));
			}
		}
		shade = 0.72f + 0.28f * smooth_step(0.18f, 0.42f, nearest);
	}
	else if (mushroom->underside == PLANT_MUSHROOM_RIDGES)
	{
		// Low ridges, forking, wavy
		float lines = cosf((round * 26.0f + 0.9f * mushroom_noise(t * 4.0f, round * 9.0f, 17u)) * 2.0f * PI_F * (t > 0.6f ? 2.0f : 1.0f));

		shade = 0.8f + 0.2f * smooth_step(-0.3f, 0.8f, lines);
	}
	else
	{
		// Thin gills, short ones between the long ones out towards the rim
		float count = t > 0.55f ? 72.0f : 36.0f;
		float lines = 0.5f + 0.5f * cosf(round * count * 2.0f * PI_F);

		shade = 0.76f + 0.24f * smooth_step(0.15f, 0.85f, lines) * smooth_step(0.15f, 0.25f, t);
	}
	for (int k = 0; k < 3; k++)
	{
		color[k] *= shade * (0.94f + 0.06f * mushroom_noise(px * 120.0f, py * 120.0f, 3u));
	}
	if (t < 0.16f)
	{
		memcpy(color, mushroom->stem_color, sizeof(float) * 3);
	}
	if (t > 0.95f)
	{
		memcpy(color, mushroom->cap_rim_color, sizeof(float) * 3);
	}
}

// The stem (round: 0..1 round it, along: 0 its foot .. 1 its top): fibers
// along it, its net, its bands, the ridges running down onto it
static void mushroom_stem_texel(const plant_mushroom_t *mushroom, float round, float along, bool in_stem, float color[3])
{
	float fibers = mushroom_noise(round * 150.0f, along * 6.0f, 5u);

	memcpy(color, mushroom->stem_color, sizeof(float) * 3);
	if (in_stem)
	{
		// A net over its upper part (each round the stem the same at both edges)
		if (mushroom->stem_net > 0.0f)
		{
			float grid_x = round * 22.0f;
			float grid_y = along * 16.0f;
			float wobble_x = grid_x + 0.3f * mushroom_noise(grid_x, grid_y, 19u);
			float wobble_y = grid_y + 0.3f * mushroom_noise(grid_x + 5.0f, grid_y, 21u);
			float edge = fminf(fminf(wobble_x - floorf(wobble_x), 1.0f - (wobble_x - floorf(wobble_x))),
				fminf(wobble_y - floorf(wobble_y), 1.0f - (wobble_y - floorf(wobble_y))));
			float net = (1.0f - smooth_step(0.06f, 0.14f, edge)) * smooth_step(0.3f, 0.55f, along);

			mix_color(color, mushroom->stem_pattern_color, fminf(mushroom->stem_net, 1.0f) * net, color);
		}
		// Zigzag bands down it (none at its top)
		if (mushroom->stem_bands > 0.0f)
		{
			float phase = along * 15.0f + 0.18f * sinf(round * 2.0f * PI_F * 9.0f) + 0.4f * mushroom_noise(round * 18.0f, along * 9.0f, 23u);
			float band = 1.0f - smooth_step(0.25f, 0.45f, phase - floorf(phase));
			float broken = smooth_step(0.35f, 0.6f, mushroom_noise(round * 30.0f, along * 40.0f, 25u));

			mix_color(color, mushroom->stem_pattern_color, fminf(mushroom->stem_bands, 1.0f) * 0.85f * band * broken
				* (1.0f - smooth_step(0.75f, 0.9f, along)), color);
		}
		// The ridges running down onto it
		if (mushroom->underside == PLANT_MUSHROOM_RIDGES)
		{
			float lines = 0.5f + 0.5f * cosf(round * 2.0f * PI_F * 22.0f + 2.0f * mushroom_noise(along * 5.0f, round * 8.0f, 27u));

			for (int k = 0; k < 3; k++)
			{
				color[k] *= 1.0f - 0.18f * (1.0f - lines) * smooth_step(0.7f, 0.95f, along);
			}
		}
	}
	for (int k = 0; k < 3; k++)
	{
		color[k] *= (0.9f + 0.1f * fibers) * (1.0f - 0.18f * (in_stem ? 1.0f - smooth_step(0.0f, 0.12f, along) : 0.0f));
	}
}

// stb's writer into a file (it is built without one of its own)
static void write_to_file(void *file, void *data, int size)
{
	fwrite(data, 1, (size_t)size, (FILE *)file);
}

// The texture made from a mushroom's colors (or, template: the template:
// each place flat colored, outlined), written to path
static bool mushroom_make_texture(const plant_mushroom_t *mushroom, const char *path, bool template)
{
	enum { SPOTS = 640 };
	const int size = MUSHROOM_TEXTURE_SIZE;
	uint8_t *pixels = checked_realloc(NULL, (size_t)size * (size_t)size * 4);
	static float spot[SPOTS][4];                   // x, y, radius, its own number
	// (as many as they cover alike: more of them the smaller they are)
	float spot_size = mushroom->spot_size > 0.0f ? fminf(mushroom->spot_size, 1.0f) : 0.5f;
	float many = fminf(fmaxf(powf(0.5f / spot_size, 1.5f), 1.0f), (float)SPOTS / 120.0f);
	int spot_count = template ? 0 : (int)(fminf(fmaxf(mushroom->spots, 0.0f), 1.0f) * 120.0f * many + 0.5f);
	char directory[PATH_LENGTH];
	const char *slash = strrchr(path, '/');
	bool ok;

	// Spread over the cap, a little apart (smaller to its rim)
	for (int i = 0; i < spot_count; i++)
	{
		float t = sqrtf(hash_unit((uint32_t)i * 2654435761u + 17u)) * 0.9f;
		float angle = hash_unit((uint32_t)i * 40503u + 911u) * 2.0f * PI_F;

		spot[i][0] = MUSHROOM_CAP_CIRCLE[0] + cosf(angle) * t * MUSHROOM_CAP_CIRCLE[2];
		spot[i][1] = MUSHROOM_CAP_CIRCLE[1] + sinf(angle) * t * MUSHROOM_CAP_CIRCLE[2];
		spot[i][2] = (0.010f + 0.020f * hash_unit((uint32_t)i * 9176u + 3u)) * (1.0f - 0.45f * t)
			* 2.0f * (mushroom->spot_size > 0.0f ? fminf(mushroom->spot_size, 1.0f) : 0.5f);
		spot[i][3] = (float)i;
	}
	for (int y = 0; y < size; y++)
	{
		for (int x = 0; x < size; x++)
		{
			float px = ((float)x + 0.5f) / (float)size;
			float py = ((float)y + 0.5f) / (float)size;
			float cap_t = hypotf(px - MUSHROOM_CAP_CIRCLE[0], py - MUSHROOM_CAP_CIRCLE[1]) / MUSHROOM_CAP_CIRCLE[2];
			float gills_t = hypotf(px - MUSHROOM_GILLS_CIRCLE[0], py - MUSHROOM_GILLS_CIRCLE[1]) / MUSHROOM_GILLS_CIRCLE[2];
			bool in_ring = px >= MUSHROOM_RING_BAND[0] && px <= MUSHROOM_RING_BAND[2] && py >= MUSHROOM_RING_BAND[1] && py <= MUSHROOM_RING_BAND[3];
			bool in_stem = px >= MUSHROOM_STEM_BAND[0] && px <= MUSHROOM_STEM_BAND[2] && py >= MUSHROOM_STEM_BAND[1] && py <= MUSHROOM_STEM_BAND[3];
			float grain = mushroom_noise(px * 90.0f, py * 90.0f, 1u) * 0.6f + mushroom_noise(px * 23.0f, py * 23.0f, 2u) * 0.4f;
			float color[3];
			uint8_t *out = &pixels[((size_t)y * (size_t)size + (size_t)x) * 4];

			if (template)
			{
				static const float cap[3] = { 0.86f, 0.24f, 0.20f };
				static const float gills[3] = { 0.90f, 0.86f, 0.70f };
				static const float ring[3] = { 0.59f, 0.75f, 0.90f };
				static const float stem[3] = { 0.70f, 0.90f, 0.63f };
				static const float none[3] = { 0.5f, 0.5f, 0.5f };
				float edge = 3.0f / (float)size;
				bool outline = fabsf(cap_t - 1.0f) * MUSHROOM_CAP_CIRCLE[2] < edge || fabsf(gills_t - 1.0f) * MUSHROOM_GILLS_CIRCLE[2] < edge;

				memcpy(color, cap_t <= 1.0f ? cap : (gills_t <= 1.0f ? gills : (in_ring ? ring : (in_stem ? stem : none))), sizeof(color));
				// (the gills' middle: where the stem meets them)
				if (gills_t <= 0.16f)
				{
					memcpy(color, stem, sizeof(color));
				}
				outline = outline || ((in_ring || in_stem) && (fabsf(px - (in_ring ? MUSHROOM_RING_BAND[0] : MUSHROOM_STEM_BAND[0])) < edge
					|| fabsf(px - (in_ring ? MUSHROOM_RING_BAND[2] : MUSHROOM_STEM_BAND[2])) < edge
					|| fabsf(py - (in_ring ? MUSHROOM_RING_BAND[1] : MUSHROOM_STEM_BAND[1])) < edge
					|| fabsf(py - (in_ring ? MUSHROOM_RING_BAND[3] : MUSHROOM_STEM_BAND[3])) < edge));
				if (outline)
				{
					color[0] = color[1] = color[2] = 0.05f;
				}
			}
			else if (cap_t <= 1.02f)
			{
				mushroom_cap_texel(mushroom, px, py, cap_t, spot, spot_count, color);
			}
			else if (gills_t <= 1.02f)
			{
				mushroom_underside_texel(mushroom, px, py, gills_t, color);
			}
			else if (in_ring)
			{
				// Folds down the skirt (and the cup round the foot)
				memcpy(color, mushroom->stem_color, sizeof(color));
				for (int k = 0; k < 3; k++)
				{
					color[k] *= 0.86f + 0.1f * (0.5f + 0.5f * sinf(px * 260.0f)) + 0.04f * grain;
				}
			}
			else
			{
				// The stem (and round it all, so the atlas' levels blend into it)
				float along = in_stem ? (MUSHROOM_STEM_BAND[3] - py) / (MUSHROOM_STEM_BAND[3] - MUSHROOM_STEM_BAND[1]) : 0.5f;
				float round = (px - MUSHROOM_STEM_BAND[0]) / (MUSHROOM_STEM_BAND[2] - MUSHROOM_STEM_BAND[0]);

				mushroom_stem_texel(mushroom, in_stem ? round : 0.5f, along, in_stem, color);
			}
			for (int k = 0; k < 3; k++)
			{
				float value = fminf(fmaxf(color[k], 0.0f), 1.0f);

				out[k] = (uint8_t)(value * 255.0f + 0.5f);
			}
			out[3] = 255;
		}
	}
	// (its folder, if there is none)
	if (slash != NULL)
	{
		snprintf(directory, sizeof(directory), "%.*s", (int)(slash - path), path);
		mkdir(directory, 0755);
	}
	{
		FILE *file = fopen(path, "wb");

		ok = file != NULL && stbi_write_png_to_func(write_to_file, file, size, size, 4, pixels, size * 4) != 0;
		if (file != NULL)
		{
			ok = fclose(file) == 0 && ok;
		}
	}
	free(pixels);
	if (!ok)
	{
		plant_error("Cannot write '%s'\n", path);
	}
	return ok;
}

bool plant_mushroom_template(const char *path)
{
	plant_mushroom_t mushroom;

	plant_mushroom_defaults(&mushroom);
	return mushroom_make_texture(&mushroom, path, true);
}

// A surface of a mushroom: rows of points round it (columns + 1 each, the
// last the first again, for the texture's seam), their places on the
// template; its normals from the points, facing out (away from each row's
// middle: out_way 1; -1 in) and up along the axis as up says (+: up, -:
// down, 0: out)
static void mushroom_surface(plant_t *plant, const vec3 *points, const float (*places)[2], int rows, int columns, const vec3 *middles,
	vec3 axis, float out_way, float up)
{
	uint32_t first = (uint32_t)plant->mesh.vertex_count;
	int stride = columns + 1;

	for (int r = 0; r < rows; r++)
	{
		for (int c = 0; c <= columns; c++)
		{
			vec3 point = points[r * stride + c];
			vec3 around = sub(points[r * stride + (c + 1) % columns], points[r * stride + (c + columns - 1) % columns]);
			vec3 along = sub(points[(r + 1 < rows ? r + 1 : r) * stride + c], points[(r > 0 ? r - 1 : r) * stride + c]);

			// A row that is one point (the cap's middle): the next row's
			// way round it, as the surface goes there
			if (dot(around, around) < 1e-14f && r + 1 < rows)
			{
				int next = r + 1;

				around = sub(points[next * stride + (c + 1) % columns], points[next * stride + (c + columns - 1) % columns]);
			}
			vec3 out = sub(point, middles[r]);
			vec3 expected;
			vec3 normal = cross(around, along);
			float u;
			float v;

			out = sub(out, scale(axis, dot(out, axis)));
			expected = add(dot(out, out) > 1e-12f ? scale(normalized(out), out_way) : v3(0.0f, 0.0f, 0.0f), scale(axis, up));
			expected = dot(expected, expected) > 1e-8f ? normalized(expected) : axis;
			if (dot(normal, normal) < 1e-14f)
			{
				normal = expected;
			}
			normal = normalized(normal);
			if (dot(normal, expected) < 0.0f)
			{
				normal = scale(normal, -1.0f);
			}
			mushroom_uv(plant, places[r * stride + c][0], places[r * stride + c][1], &u, &v);
			add_vertex(&plant->mesh, point, normal, dot(around, around) > 1e-14f ? normalized(around) : perpendicular(axis), axis, u, v);
		}
	}
	for (int r = 0; r + 1 < rows; r++)
	{
		for (int c = 0; c < columns; c++)
		{
			uint32_t quad[4] = { first + (uint32_t)(r * stride + c), first + (uint32_t)(r * stride + c + 1),
				first + (uint32_t)((r + 1) * stride + c), first + (uint32_t)((r + 1) * stride + c + 1) };
			uint32_t triangles[2][3] = { { quad[0], quad[1], quad[2] }, { quad[1], quad[3], quad[2] } };

			for (int t = 0; t < 2; t++)
			{
				const object_file_vertex_t *a = &plant->mesh.vertices[triangles[t][0]];
				const object_file_vertex_t *b = &plant->mesh.vertices[triangles[t][1]];
				const object_file_vertex_t *corner = &plant->mesh.vertices[triangles[t][2]];
				vec3 pa = v3(a->position[0], a->position[1], a->position[2]);
				vec3 face = cross(sub(v3(b->position[0], b->position[1], b->position[2]), pa),
					sub(v3(corner->position[0], corner->position[1], corner->position[2]), pa));
				vec3 normals = v3(a->normal[0] + b->normal[0] + corner->normal[0], a->normal[1] + b->normal[1] + corner->normal[1],
					a->normal[2] + b->normal[2] + corner->normal[2]);

				// (none where a row is a point: the cap's middle)
				if (dot(face, face) < 1e-18f)
				{
					continue;
				}
				// Its front the way its normals face
				if (dot(face, normals) < 0.0f)
				{
					add_triangle(&plant->mesh, MATERIAL_FRUIT, triangles[t][0], triangles[t][2], triangles[t][1]);
				}
				else
				{
					add_triangle(&plant->mesh, MATERIAL_FRUIT, triangles[t][0], triangles[t][1], triangles[t][2]);
				}
			}
		}
	}
}

// The cap's top at t (0 its middle .. 1 its rim), of its height: a cone or a
// dome, a funnel, a bump, the rim bent
static float mushroom_cap_top(const plant_mushroom_t *mushroom, float t)
{
	float dome = fminf(fmaxf(mushroom->cap_dome, 0.0f), 1.0f);
	float funnel = fminf(fmaxf(mushroom->cap_funnel, 0.0f), 1.0f);
	// (the cone's tip rounded off: no nipple on it)
	float cone = (sqrtf(1.04f) - sqrtf(t * t + 0.04f)) / (sqrtf(1.04f) - 0.2f);
	float shape = cone * (1.0f - dome) + sqrtf(fmaxf(1.0f - t * t, 0.0f)) * dome;
	float rim = smooth_step(0.65f, 1.0f, t);

	shape = shape * (1.0f - funnel) + powf(t, 1.4f) * funnel;
	shape += fminf(fmaxf(mushroom->cap_umbo, 0.0f), 1.0f) * 0.35f * expf(-(t / 0.2f) * (t / 0.2f));
	// (the rim bent by as much of the cap's width as its height: a flat
	// cap's too)
	shape -= mushroom->cap_curl * 0.4f * rim * rim * fmaxf(1.0f, 0.6f * mushroom->cap_width / fmaxf(mushroom->cap_height, 0.01f));
	return shape;
}

// How thick the cap is at t, of its height (thick in its middle, thin at its rim)
static float mushroom_cap_flesh(const plant_mushroom_t *mushroom, float t)
{
	return (0.07f + 0.4f * (1.0f - t)) * (1.0f - 0.5f * fminf(fmaxf(mushroom->cap_funnel, 0.0f), 1.0f));
}

// How far out the cap is at t, round its rim as wavy as there (the middle
// round: the stem meets it there)
static float mushroom_cap_radius(float radius, float rim_radius, float t)
{
	return t * (radius + (rim_radius - radius) * smooth_step(0.4f, 1.0f, t));
}

// The stem's arc: where it is s along it (0 its foot .. 1 its top), and its way there
static void mushroom_stem_at(vec3 foot, vec3 axis, vec3 towards, float length, float bend, float s, vec3 *point_out, vec3 *heading_out)
{
	float angle = bend * s;

	if (fabsf(bend) < 1e-4f)
	{
		*point_out = add(foot, scale(axis, length * s));
	}
	else
	{
		*point_out = add(foot, add(scale(axis, length * sinf(angle) / bend), scale(towards, length * (1.0f - cosf(angle)) / bend)));
	}
	*heading_out = normalized(add(scale(axis, cosf(angle)), scale(towards, sinf(angle))));
}

// One mushroom from foot up axis, height tall (as the recipe's, a little of
// its own)
static void add_mushroom(plant_t *plant, vec3 foot, vec3 axis, float height)
{
	enum { STEM_ROWS = 8, CAP_TOP_ROWS = 8, UNDER_ROWS = 4, RING_ROWS = 3 };
	static const float stem_rows[STEM_ROWS] = { 0.0f, 0.06f, 0.15f, 0.3f, 0.5f, 0.7f, 0.88f, 1.0f };
	static const float top_rows[CAP_TOP_ROWS] = { 0.0f, 0.18f, 0.36f, 0.54f, 0.7f, 0.84f, 0.94f, 1.0f };
	const plant_mushroom_t *mushroom = &plant->mushroom;
	float stem_radius = fmaxf(mushroom->stem_thickness, 0.005f) * height;
	float taper = fminf(fmaxf(mushroom->stem_taper, 0.1f), 3.0f);
	float cap_radius = fmaxf(mushroom->cap_width, 0.02f) * height * random_between(0.9f, 1.1f);
	float cap_height = fmaxf(mushroom->cap_height, 0.01f) * height;
	float length = mushroom->stem_length > 0.0f ? mushroom->stem_length * height : fmaxf(height - 0.7f * cap_height, 0.2f * height);
	float bend = fmaxf(mushroom->stem_bend, 0.0f) * 0.8f * random_between(0.6f, 1.2f);
	float wave = fminf(fmaxf(mushroom->cap_wave, 0.0f), 1.0f);
	float phases[3] = { random_between(0.0f, 6.3f), random_between(0.0f, 6.3f), random_between(0.0f, 6.3f) };
	float towards_angle = random_between(0.0f, 2.0f * PI_F);
	vec3 towards = sub(v3(cosf(towards_angle), 0.0f, sinf(towards_angle)), scale(axis, dot(axis, v3(cosf(towards_angle), 0.0f,
		sinf(towards_angle)))));
	vec3 side;
	vec3 points[(UNDER_ROWS > CAP_TOP_ROWS ? UNDER_ROWS : CAP_TOP_ROWS) * (MUSHROOM_CAP_SIDES + 1)];
	float places[(UNDER_ROWS > CAP_TOP_ROWS ? UNDER_ROWS : CAP_TOP_ROWS) * (MUSHROOM_CAP_SIDES + 1)][2];
	vec3 middles[CAP_TOP_ROWS];
	vec3 stem_top;
	vec3 stem_heading;

	towards = dot(towards, towards) > 1e-8f ? normalized(towards) : perpendicular(axis);
	side = normalized(cross(towards, axis));
	// The stem: round its arc, swollen at its foot (its radius at its middle
	// stem_thickness), its foot a little in the ground
	foot = sub(foot, scale(axis, 0.03f * height));
	{
		vec3 stem_points[STEM_ROWS * (MUSHROOM_STEM_SIDES + 1)];
		float stem_places[STEM_ROWS * (MUSHROOM_STEM_SIDES + 1)][2];
		vec3 stem_middles[STEM_ROWS];

		for (int r = 0; r < STEM_ROWS; r++)
		{
			float s = stem_rows[r];
			float bulb = 1.0f - smooth_step(0.0f, 0.3f, s);
			float radius = stem_radius * 2.0f / (1.0f + taper) * (1.0f + (taper - 1.0f) * s) * (1.0f + fmaxf(mushroom->stem_bulb, 0.0f) * bulb * bulb);
			vec3 point;
			vec3 heading;
			vec3 across;

			mushroom_stem_at(foot, axis, towards, length, bend, s, &point, &heading);
			across = normalized(cross(side, heading));
			stem_middles[r] = point;
			for (int c = 0; c <= MUSHROOM_STEM_SIDES; c++)
			{
				float angle = 2.0f * PI_F * (float)c / (float)MUSHROOM_STEM_SIDES;
				int index = r * (MUSHROOM_STEM_SIDES + 1) + c;

				stem_points[index] = add(point, scale(add(scale(across, cosf(angle)), scale(side, sinf(angle))), radius));
				stem_places[index][0] = MUSHROOM_STEM_BAND[0] + (MUSHROOM_STEM_BAND[2] - MUSHROOM_STEM_BAND[0]) * (float)c / (float)MUSHROOM_STEM_SIDES;
				stem_places[index][1] = MUSHROOM_STEM_BAND[3] + (MUSHROOM_STEM_BAND[1] - MUSHROOM_STEM_BAND[3]) * s;
			}
		}
		mushroom_surface(plant, stem_points, (const float (*)[2])stem_places, STEM_ROWS, MUSHROOM_STEM_SIDES, stem_middles, axis, 1.0f, 0.0f);
	}
	// The cup round its foot: out from it and up, both its sides
	if (mushroom->stem_volva > 0.0f)
	{
		float volva = fminf(mushroom->stem_volva, 1.0f);
		float radius = stem_radius * 2.0f / (1.0f + taper) * (1.0f + fmaxf(mushroom->stem_bulb, 0.0f));
		vec3 cup_points[RING_ROWS * (MUSHROOM_STEM_SIDES + 1)];
		float cup_places[RING_ROWS * (MUSHROOM_STEM_SIDES + 1)][2];
		vec3 cup_middles[RING_ROWS];
		vec3 point;
		vec3 heading;
		vec3 across;

		mushroom_stem_at(foot, axis, towards, length, bend, 0.02f, &point, &heading);
		across = normalized(cross(side, heading));
		for (int r = 0; r < RING_ROWS; r++)
		{
			float out_share = (float)r / (float)(RING_ROWS - 1);
			float out = radius * (1.02f + 0.45f * volva * out_share);

			cup_middles[r] = add(point, scale(heading, length * 0.16f * volva * powf(out_share, 0.8f)));
			for (int c = 0; c <= MUSHROOM_STEM_SIDES; c++)
			{
				float angle = 2.0f * PI_F * (float)c / (float)MUSHROOM_STEM_SIDES;
				float torn = 1.0f + 0.12f * sinf(5.0f * angle + phases[1]) * out_share;
				int index = r * (MUSHROOM_STEM_SIDES + 1) + c;

				cup_points[index] = add(cup_middles[r], scale(add(scale(across, cosf(angle)), scale(side, sinf(angle))), out * torn));
				cup_places[index][0] = MUSHROOM_RING_BAND[0] + (MUSHROOM_RING_BAND[2] - MUSHROOM_RING_BAND[0]) * (float)c / (float)MUSHROOM_STEM_SIDES;
				cup_places[index][1] = MUSHROOM_RING_BAND[3] + (MUSHROOM_RING_BAND[1] - MUSHROOM_RING_BAND[3]) * out_share;
			}
		}
		mushroom_surface(plant, cup_points, (const float (*)[2])cup_places, RING_ROWS, MUSHROOM_STEM_SIDES, cup_middles, heading, 1.0f, -0.3f);
		mushroom_surface(plant, cup_points, (const float (*)[2])cup_places, RING_ROWS, MUSHROOM_STEM_SIDES, cup_middles, heading, -1.0f, 0.3f);
	}
	// The skirt round it: out from it and hanging, both its sides
	if (mushroom->ring_height > 0.0f && mushroom->ring_width > 0.0f)
	{
		float s = fminf(mushroom->ring_height, 0.97f);
		float radius = stem_radius * 2.0f / (1.0f + taper) * (1.0f + (taper - 1.0f) * s);
		vec3 ring_points[RING_ROWS * (MUSHROOM_STEM_SIDES + 1)];
		float ring_places[RING_ROWS * (MUSHROOM_STEM_SIDES + 1)][2];
		vec3 ring_middles[RING_ROWS];
		vec3 point;
		vec3 heading;
		vec3 across;

		mushroom_stem_at(foot, axis, towards, length, bend, s, &point, &heading);
		across = normalized(cross(side, heading));
		for (int r = 0; r < RING_ROWS; r++)
		{
			float out_share = (float)r / (float)(RING_ROWS - 1);
			float out = radius * (1.02f + fminf(mushroom->ring_width, 3.0f) * out_share);
			float drop = fminf(fmaxf(mushroom->ring_droop, 0.0f), 1.5f) * radius * mushroom->ring_width * 1.3f * powf(out_share, 1.5f);

			ring_middles[r] = sub(point, scale(heading, drop));
			for (int c = 0; c <= MUSHROOM_STEM_SIDES; c++)
			{
				float angle = 2.0f * PI_F * (float)c / (float)MUSHROOM_STEM_SIDES;
				float wobble = 1.0f + 0.08f * sinf(6.0f * angle + phases[0]) * out_share;
				int index = r * (MUSHROOM_STEM_SIDES + 1) + c;

				ring_points[index] = add(ring_middles[r], scale(add(scale(across, cosf(angle)), scale(side, sinf(angle))), out * wobble));
				ring_places[index][0] = MUSHROOM_RING_BAND[0] + (MUSHROOM_RING_BAND[2] - MUSHROOM_RING_BAND[0]) * (float)c / (float)MUSHROOM_STEM_SIDES;
				ring_places[index][1] = MUSHROOM_RING_BAND[1] + (MUSHROOM_RING_BAND[3] - MUSHROOM_RING_BAND[1]) * out_share;
			}
		}
		mushroom_surface(plant, ring_points, (const float (*)[2])ring_places, RING_ROWS, MUSHROOM_STEM_SIDES, ring_middles, heading, 0.3f, 1.0f);
		mushroom_surface(plant, ring_points, (const float (*)[2])ring_places, RING_ROWS, MUSHROOM_STEM_SIDES, ring_middles, heading, -0.3f, -1.0f);
	}
	// The cap on its top, the way it ends (a little of its own), its
	// underside's middle where the stem ends
	mushroom_stem_at(foot, axis, towards, length, bend, 1.0f, &stem_top, &stem_heading);
	{
		vec3 cap_axis = stem_heading;
		vec3 cap_across = normalized(cross(side, cap_axis));
		vec3 cap_side = normalized(cross(cap_axis, cap_across));
		float top_radius = stem_radius * 2.0f * taper / (1.0f + taper);
		// (the gills in to just inside the stem's top: they meet it there,
		// the stem ends at them, nothing of it in the cap)
		float inner = fminf(fmaxf(top_radius * 0.95f / cap_radius, 0.03f), 0.6f);
		float lift = mushroom_cap_top(mushroom, inner) - mushroom_cap_flesh(mushroom, inner);
		vec3 origin = sub(stem_top, scale(cap_axis, lift * cap_height));
		// (from the rim, 1, in to the stem, 0)
		static const float under_rows[UNDER_ROWS] = { 1.0f, 0.66f, 0.33f, 0.0f };
		float rim_radius[MUSHROOM_CAP_SIDES + 1];
		float rim_rise[MUSHROOM_CAP_SIDES + 1];

		for (int c = 0; c <= MUSHROOM_CAP_SIDES; c++)
		{
			float angle = 2.0f * PI_F * (float)(c % MUSHROOM_CAP_SIDES) / (float)MUSHROOM_CAP_SIDES;

			rim_radius[c] = cap_radius * (1.0f + wave * (0.06f * sinf(5.0f * angle + phases[0]) + 0.035f * sinf(9.0f * angle + phases[1])));
			rim_rise[c] = wave * 0.06f * cap_height * sinf(7.0f * angle + phases[2]);
		}
		// Its top
		for (int r = 0; r < CAP_TOP_ROWS; r++)
		{
			float t = top_rows[r];

			middles[r] = add(origin, scale(cap_axis, mushroom_cap_top(mushroom, t) * cap_height));
			for (int c = 0; c <= MUSHROOM_CAP_SIDES; c++)
			{
				float angle = 2.0f * PI_F * (float)c / (float)MUSHROOM_CAP_SIDES;
				vec3 out = add(scale(cap_across, cosf(angle)), scale(cap_side, sinf(angle)));
				int index = r * (MUSHROOM_CAP_SIDES + 1) + c;

				points[index] = add(add(middles[r], scale(cap_axis, rim_rise[c] * t * t)), scale(out, mushroom_cap_radius(cap_radius, rim_radius[c], t)));
				places[index][0] = MUSHROOM_CAP_CIRCLE[0] + MUSHROOM_CAP_CIRCLE[2] * 0.97f * t * cosf(angle);
				places[index][1] = MUSHROOM_CAP_CIRCLE[1] + MUSHROOM_CAP_CIRCLE[2] * 0.97f * t * sinf(angle);
			}
		}
		mushroom_surface(plant, points, (const float (*)[2])places, CAP_TOP_ROWS, MUSHROOM_CAP_SIDES, middles, cap_axis, 1.0f, 1.5f);
		// Its rim: the top's edge down to the underside's
		for (int r = 0; r < 2; r++)
		{
			float rise = mushroom_cap_top(mushroom, 1.0f) - (r == 1 ? mushroom_cap_flesh(mushroom, 1.0f) : 0.0f);

			middles[r] = add(origin, scale(cap_axis, rise * cap_height));
			for (int c = 0; c <= MUSHROOM_CAP_SIDES; c++)
			{
				float angle = 2.0f * PI_F * (float)c / (float)MUSHROOM_CAP_SIDES;
				vec3 out = add(scale(cap_across, cosf(angle)), scale(cap_side, sinf(angle)));
				int index = r * (MUSHROOM_CAP_SIDES + 1) + c;
				float ring = r == 0 ? 0.97f : 1.0f;

				points[index] = add(add(middles[r], scale(cap_axis, rim_rise[c])), scale(out, rim_radius[c]));
				places[index][0] = MUSHROOM_CAP_CIRCLE[0] + MUSHROOM_CAP_CIRCLE[2] * ring * cosf(angle);
				places[index][1] = MUSHROOM_CAP_CIRCLE[1] + MUSHROOM_CAP_CIRCLE[2] * ring * sinf(angle);
			}
		}
		mushroom_surface(plant, points, (const float (*)[2])places, 2, MUSHROOM_CAP_SIDES, middles, cap_axis, 1.0f, 0.0f);
		// Its underside, the gills: from the rim in to the stem (seen from
		// below: the gills' picture mirrored)
		for (int r = 0; r < UNDER_ROWS; r++)
		{
			float t = inner + (1.0f - inner) * under_rows[r];
			middles[r] = add(origin, scale(cap_axis, (mushroom_cap_top(mushroom, t) - mushroom_cap_flesh(mushroom, t)) * cap_height));
			for (int c = 0; c <= MUSHROOM_CAP_SIDES; c++)
			{
				float angle = 2.0f * PI_F * (float)c / (float)MUSHROOM_CAP_SIDES;
				vec3 out = add(scale(cap_across, cosf(angle)), scale(cap_side, sinf(angle)));
				int index = r * (MUSHROOM_CAP_SIDES + 1) + c;

				points[index] = add(add(middles[r], scale(cap_axis, rim_rise[c] * t * t)), scale(out, mushroom_cap_radius(cap_radius, rim_radius[c], t)));
				places[index][0] = MUSHROOM_GILLS_CIRCLE[0] - MUSHROOM_GILLS_CIRCLE[2] * 0.97f * t * cosf(angle);
				places[index][1] = MUSHROOM_GILLS_CIRCLE[1] + MUSHROOM_GILLS_CIRCLE[2] * 0.97f * t * sinf(angle);
			}
		}
		mushroom_surface(plant, points, (const float (*)[2])places, UNDER_ROWS, MUSHROOM_CAP_SIDES, middles, cap_axis, 1.0f, -3.0f);
	}
}

// A few mushrooms together (the recipe's cluster: --fruit-cluster), the
// first the biggest, upright in the middle, the others smaller round it,
// leaning out a little
static void grow_mushroom(plant_t *plant)
{
	int count = plant->cluster_minimum + random_index(plant->cluster_maximum - plant->cluster_minimum + 1);
	// (all of them bigger or smaller: the recipe's size)
	float scale_all = plant->mushroom.size > 0.0f ? fminf(plant->mushroom.size, 20.0f) : 1.0f;

	for (int m = 0; m < count; m++)
	{
		float size = m == 0 ? 1.0f : random_between(0.5f, 0.95f);
		float azimuth = random_between(0.0f, 2.0f * PI_F);
		vec3 out = v3(cosf(azimuth), 0.0f, sinf(azimuth));
		// (their caps not in one another: as far apart as they are wide,
		// a little more or less)
		float distance = m == 0 ? 0.0f : plant->height * fmaxf(plant->mushroom.cap_width, 0.05f) * (1.0f + size) * random_between(0.8f, 1.2f);
		float lean = m == 0 ? random_between(0.0f, 0.08f) : random_between(0.08f, 0.3f);
		vec3 axis = normalized(add(v3(0.0f, 1.0f, 0.0f), scale(out, lean)));

		add_mushroom(plant, scale(out, distance * scale_all), axis, plant->height * size * scale_all);
	}
}

// ---------------------------------------------------------------------------
// .pobj out

// A file's name relative to the data directory (as the game names it);
// false if it is not under it
// The part of path under the data directory: as given ("data/textures/x.png"
// under "data"), or named from the data directory ("textures/x.png"), or
// else as the files really are (an absolute path from a dialog,
// "../data/...", a link), each made absolute
static bool data_relative_as_given(const char *path, const char *directory_given, char *out, size_t out_size)
{
	char directory[PATH_LENGTH];
	size_t length;

	snprintf(directory, sizeof(directory), "%s", directory_given);
	length = strlen(directory);
	while (length > 0 && directory[length - 1] == '/')
	{
		directory[--length] = '\0';
	}
	while (path[0] == '.' && path[1] == '/')
	{
		path += 2;
	}
	if (length == 0 || strncmp(path, directory, length) != 0 || path[length] != '/')
	{
		return false;
	}
	snprintf(out, out_size, "%s", path + length + 1);
	return true;
}

static bool data_relative(const char *path, const char *data_directory, char *out, size_t out_size)
{
	char *real_path;
	char *real_directory;
	bool found;

	if (data_relative_as_given(path, data_directory, out, out_size))
	{
		return true;
	}
	// Named as the game names it, under the data directory ("textures/bark.png")
	{
		char under[PATH_LENGTH * 2];
		FILE *file;

		snprintf(under, sizeof(under), "%s/%s", data_directory, path);
		file = path[0] != '/' ? fopen(under, "rb") : NULL;
		if (file != NULL)
		{
			fclose(file);
			snprintf(out, out_size, "%s", path);
			return true;
		}
	}
	real_path = realpath(path, NULL);
	real_directory = realpath(data_directory, NULL);
	found = real_path != NULL && real_directory != NULL && data_relative_as_given(real_path, real_directory, out, out_size);
	free(real_path);
	free(real_directory);
	return found;
}

typedef struct
{
	char names[8][PATH_LENGTH];         // the texture files named (relative to data/)
	int count;
} references_t;

static int add_reference(references_t *references, const char *name)
{
	for (int i = 0; i < references->count; i++)
	{
		if (strcmp(references->names[i], name) == 0)
		{
			return i;
		}
	}
	snprintf(references->names[references->count], PATH_LENGTH, "%s", name);
	return references->count++;
}

static bool write_pobj(const char *path, const mesh_t *mesh, const references_t *references, const int material_textures[MATERIAL_COUNT][3],
	const float bark_color[3], const object_file_plant_material_t *plant_materials, const object_file_month_cell_t *month_cells,
	int month_cell_count, bool still)
{
	static const char *const names[MATERIAL_COUNT] = { "trunk", "bark", "foliage", "fruit", OBJECT_FILE_SNOW_MATERIAL };
	object_file_header_t header;
	FILE *file = fopen(path, "wb");
	uint32_t offset = 0;
	uint32_t material_count = 0;

	if (file == NULL)
	{
		plant_error("Cannot write '%s'\n", path);
		return false;
	}
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, OBJECT_FILE_MAGIC, 4);
	header.version = OBJECT_FILE_VERSION;
	header.vertex_count = (uint32_t)mesh->vertex_count;
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		header.index_count += (uint32_t)mesh->index_count[m];
		material_count += mesh->index_count[m] > 0 ? 1u : 0u;
	}
	header.material_count = material_count;
	header.texture_count = (uint32_t)references->count;
	memcpy(header.bounds_minimum, &mesh->minimum, sizeof(float) * 3);
	memcpy(header.bounds_maximum, &mesh->maximum, sizeof(float) * 3);
	// A plant: what each vertex is on it, and its materials through the seasons
	header.flags = mesh->plant_vertices != NULL && plant_materials != NULL ? OBJECT_FILE_PLANT : 0u;
	header.flags |= still ? OBJECT_FILE_STILL : 0u;
	header.flags |= (header.flags & OBJECT_FILE_PLANT) && mesh->snow_vertices != NULL ? OBJECT_FILE_SNOW : 0u;
	header.month_cell_count = (header.flags & OBJECT_FILE_PLANT) ? (uint32_t)month_cell_count : 0u;
	fwrite(&header, sizeof(header), 1, file);
	fwrite(mesh->vertices, sizeof(object_file_vertex_t), mesh->vertex_count, file);
	if (header.flags & OBJECT_FILE_PLANT)
	{
		fwrite(mesh->plant_vertices, sizeof(object_file_plant_vertex_t), mesh->vertex_count, file);
		if (header.flags & OBJECT_FILE_SNOW)
		{
			fwrite(mesh->snow_vertices, sizeof(object_file_snow_vertex_t), mesh->vertex_count, file);
		}
	}
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		fwrite(mesh->indices[m], sizeof(uint32_t), mesh->index_count[m], file);
	}
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		object_file_material_t material;

		if (mesh->index_count[m] == 0)
		{
			continue;
		}
		memset(&material, 0, sizeof(material));
		snprintf(material.name, sizeof(material.name), "%s", names[m]);
		// (a mushroom: a solid, its faces the right way round; no cut out
		// card that would shake in the wind or fall in the autumn)
		material.flags = m == MATERIAL_FOLIAGE ? OBJECT_MATERIAL_ALPHA_TESTED | OBJECT_MATERIAL_DOUBLE_SIDED | OBJECT_MATERIAL_LIGHTING_NORMALS
			: (m == MATERIAL_FRUIT && still ? OBJECT_MATERIAL_PASSABLE
			: (m == MATERIAL_FRUIT ? OBJECT_MATERIAL_ALPHA_TESTED | OBJECT_MATERIAL_DOUBLE_SIDED | OBJECT_MATERIAL_PASSABLE
			: (m == MATERIAL_BARK || m == MATERIAL_SNOW ? OBJECT_MATERIAL_PASSABLE : 0u)));
		material.flags |= (mesh->lods[m] ? OBJECT_MATERIAL_LODS : 0u) | (mesh->shuffled[m] ? OBJECT_MATERIAL_SHUFFLED : 0u);
		memcpy(material.lod_index_count, mesh->lod_index_count[m], sizeof(material.lod_index_count));
		material.index_offset = offset;
		material.index_count = (uint32_t)mesh->index_count[m];
		offset += material.index_count;
		for (int k = 0; k < 4; k++)
		{
			material.base_color[k] = 1.0f;
		}
		// Without a texture of its own, the bark is a plain color
		if (m != MATERIAL_FOLIAGE && m != MATERIAL_FRUIT && m != MATERIAL_SNOW && material_textures[m][0] < 0)
		{
			memcpy(material.base_color, bark_color, sizeof(float) * 3);
		}
		material.metallic = 1.0f;
		material.roughness = 1.0f;
		material.normal_scale = 1.0f;
		material.occlusion_strength = 1.0f;
		material.alpha_cutoff = 0.5f;
		material.base_color_texture = material_textures[m][0];
		material.normal_texture = material_textures[m][1];
		material.orm_texture = material_textures[m][2];
		material.emissive_texture = -1;
		if (material_textures[m][2] < 0)
		{
			// No arm map: rough, not metal (the factors alone)
			material.metallic = 0.0f;
			material.roughness = 0.9f;
		}
		fwrite(&material, sizeof(material), 1, file);
	}
	for (int m = 0; m < MATERIAL_COUNT && (header.flags & OBJECT_FILE_PLANT); m++)
	{
		if (mesh->index_count[m] > 0)
		{
			fwrite(&plant_materials[m], sizeof(object_file_plant_material_t), 1, file);
		}
	}
	if (header.month_cell_count > 0)
	{
		fwrite(month_cells, sizeof(object_file_month_cell_t), header.month_cell_count, file);
	}
	for (int t = 0; t < references->count; t++)
	{
		object_file_texture_t texture;
		size_t length = strlen(references->names[t]) + 1;
		size_t padded = (length + 3) & ~(size_t)3;
		char name[PATH_LENGTH + 4];

		memset(&texture, 0, sizeof(texture));
		texture.kind = OBJECT_TEXTURE_REFERENCE;
		texture.compressed_size = (uint32_t)padded;
		memset(name, 0, sizeof(name));
		memcpy(name, references->names[t], length);
		fwrite(&texture, sizeof(texture), 1, file);
		fwrite(name, 1, padded, file);
	}
	fclose(file);
	return true;
}

// The foliage deep in a crown is in the shade of the rest: darker the
// nearer the trunk it is, of how far the crown reaches out at its height,
// and a little darker low down. Only the foliage's vertices

// A material's triangles in the order of their branches' levels (counting,
// so each level keeps its order); level k + 1 of detail keeps the levels up
// to deepest - 1 - k
static void sort_by_level(mesh_t *mesh, int material, const uint8_t *levels, int deepest)
{
	size_t triangles = mesh->index_count[material] / 3;
	size_t level_count[256];
	size_t start[256];
	size_t running = 0;
	uint32_t *sorted;

	if (triangles == 0 || levels == NULL)
	{
		return;
	}
	memset(level_count, 0, sizeof(level_count));
	for (size_t t = 0; t < triangles; t++)
	{
		level_count[levels[t]]++;
	}
	sorted = checked_realloc(NULL, triangles * 3 * sizeof(uint32_t));
	for (int l = 0; l < 256; l++)
	{
		start[l] = running;
		running += level_count[l];
	}
	for (size_t t = 0; t < triangles; t++)
	{
		memcpy(&sorted[start[levels[t]]++ * 3], &mesh->indices[material][t * 3], 3 * sizeof(uint32_t));
	}
	memcpy(mesh->indices[material], sorted, triangles * 3 * sizeof(uint32_t));
	free(sorted);
	for (int k = 0; k < 3; k++)
	{
		size_t kept = 0;

		for (int l = 0; l <= deepest - 1 - k; l++)
		{
			kept += level_count[l];
		}
		mesh->lod_index_count[material][k] = (uint32_t)(kept * 3);
	}
	mesh->lods[material] = true;
}

// A tree's levels of detail (OBJECT_MATERIAL_LODS): its bark's triangles in
// the order of their levels, the trunk's branches first, so that the first
// part of them is the tree without its finest branches (those that hold the
// twigs and the leaves, and the fruit's stalks), a smaller part without the
// next ones too, and none; its foliage in a random order (whole cards), so
// any first part of it is the crown thinned out evenly; its fruit gone
// after the first level
static void finish_lods(mesh_t *mesh)
{
	size_t triangles = mesh->index_count[MATERIAL_BARK] / 3;
	int deepest = 0;

	for (size_t t = 0; t < triangles; t++)
	{
		if (mesh->bark_levels[t] != BARK_LEVEL_STALK && mesh->bark_levels[t] > deepest)
		{
			deepest = mesh->bark_levels[t];
		}
	}
	sort_by_level(mesh, MATERIAL_BARK, mesh->bark_levels, deepest);
	// The foliage's cards shuffled (each card is six indices in a row)
	{
		size_t cards = mesh->index_count[MATERIAL_FOLIAGE] / 6;
		uint32_t *indices = mesh->indices[MATERIAL_FOLIAGE];
		uint8_t *vertex_ranks = NULL;

		// The ranks (needle cones' strips) by vertex, before the shuffle
		// moves the triangles away from theirs
		if (mesh->foliage_ranks != NULL)
		{
			bool any = false;

			vertex_ranks = calloc(mesh->vertex_count > 0 ? mesh->vertex_count : 1, 1);
			for (size_t t = 0; t < mesh->index_count[MATERIAL_FOLIAGE] / 3; t++)
			{
				vertex_ranks[indices[t * 3]] = mesh->foliage_ranks[t];
				any = any || mesh->foliage_ranks[t] != 0;
			}
			if (!any)
			{
				free(vertex_ranks);
				vertex_ranks = NULL;
			}
		}
		if (cards > 1 && mesh->index_count[MATERIAL_FOLIAGE] % 6 == 0)
		{
			for (size_t c = cards - 1; c > 0; c--)
			{
				size_t other = (size_t)(random_unit() * (float)(c + 1));
				uint32_t swap[6];

				other = other > c ? c : other;
				memcpy(swap, &indices[c * 6], sizeof(swap));
				memcpy(&indices[c * 6], &indices[other * 6], sizeof(swap));
				memcpy(&indices[other * 6], swap, sizeof(swap));
			}
			mesh->shuffled[MATERIAL_FOLIAGE] = true;
			// Ranked cards (needle cones' strips): by their ranks, still
			// shuffled within each, so that a first share keeps every
			// cone, thinned evenly round it
			if (vertex_ranks != NULL)
			{
				size_t rank_count[256];
				size_t start[256];
				size_t running = 0;
				uint8_t *card_ranks = checked_realloc(NULL, cards);
				uint32_t *sorted = checked_realloc(NULL, cards * 6 * sizeof(uint32_t));

				memset(rank_count, 0, sizeof(rank_count));
				// (each card its own vertices: its rank by its first one's)
				for (size_t c = 0; c < cards; c++)
				{
					card_ranks[c] = vertex_ranks[indices[c * 6]];
				}
				for (size_t c = 0; c < cards; c++)
				{
					rank_count[card_ranks[c]]++;
				}
				for (int r = 0; r < 256; r++)
				{
					start[r] = running;
					running += rank_count[r];
				}
				for (size_t c = 0; c < cards; c++)
				{
					memcpy(&sorted[start[card_ranks[c]]++ * 6], &indices[c * 6], 6 * sizeof(uint32_t));
				}
				memcpy(indices, sorted, cards * 6 * sizeof(uint32_t));
				free(sorted);
				free(card_ranks);
			}
		}
		free(vertex_ranks);
	}
	if (mesh->index_count[MATERIAL_FRUIT] > 0)
	{
		mesh->lods[MATERIAL_FRUIT] = true;
		mesh->lod_index_count[MATERIAL_FRUIT][0] = (uint32_t)mesh->index_count[MATERIAL_FRUIT];
	}
}

#define SHADE_BANDS 24
#define CROWN_DEPTH_SHADE 0.55f         // how dark the innermost foliage is (1 - this)
#define CROWN_LOW_SHADE 0.15f           // and the lowest, besides

static void shade_crown(mesh_t *mesh)
{
	float reach[SHADE_BANDS];
	// Each band's middle (a leaning tree's crown is not over its foot)
	float middle_x[SHADE_BANDS];
	float middle_z[SHADE_BANDS];
	float middle_count[SHADE_BANDS];
	float bottom = mesh->minimum.y;
	float height = fmaxf(mesh->maximum.y - mesh->minimum.y, 1e-3f);
	bool *foliage = calloc(mesh->vertex_count > 0 ? mesh->vertex_count : 1, sizeof(bool));

	if (foliage == NULL)
	{
		return;
	}
	for (size_t i = 0; i < mesh->index_count[MATERIAL_FOLIAGE]; i++)
	{
		foliage[mesh->indices[MATERIAL_FOLIAGE][i]] = true;
	}
	for (int b = 0; b < SHADE_BANDS; b++)
	{
		reach[b] = 0.0f;
		middle_x[b] = 0.0f;
		middle_z[b] = 0.0f;
		middle_count[b] = 0.0f;
	}
	for (size_t i = 0; i < mesh->vertex_count; i++)
	{
		const float *p = mesh->vertices[i].position;
		int band = (int)((p[1] - bottom) / height * (float)SHADE_BANDS);

		if (foliage[i])
		{
			band = band < 0 ? 0 : (band >= SHADE_BANDS ? SHADE_BANDS - 1 : band);
			middle_x[band] += p[0];
			middle_z[band] += p[2];
			middle_count[band] += 1.0f;
		}
	}
	for (int b = 0; b < SHADE_BANDS; b++)
	{
		middle_x[b] = middle_count[b] > 0.0f ? middle_x[b] / middle_count[b] : 0.0f;
		middle_z[b] = middle_count[b] > 0.0f ? middle_z[b] / middle_count[b] : 0.0f;
	}
	for (size_t i = 0; i < mesh->vertex_count; i++)
	{
		const float *p = mesh->vertices[i].position;
		int band = (int)((p[1] - bottom) / height * (float)SHADE_BANDS);

		if (foliage[i])
		{
			float dx;
			float dz;

			band = band < 0 ? 0 : (band >= SHADE_BANDS ? SHADE_BANDS - 1 : band);
			dx = p[0] - middle_x[band];
			dz = p[2] - middle_z[band];
			reach[band] = fmaxf(reach[band], sqrtf(dx * dx + dz * dz));
		}
	}
	// Smoothed over the bands next to each (a thin band would be all "outside")
	{
		float smoothed[SHADE_BANDS];

		for (int b = 0; b < SHADE_BANDS; b++)
		{
			smoothed[b] = reach[b];
			if (b > 0)
			{
				smoothed[b] = fmaxf(smoothed[b], reach[b - 1]);
			}
			if (b + 1 < SHADE_BANDS)
			{
				smoothed[b] = fmaxf(smoothed[b], reach[b + 1]);
			}
		}
		memcpy(reach, smoothed, sizeof(reach));
	}
	{
		float lowest = 1e30f;
		float highest = -1e30f;

		for (size_t i = 0; i < mesh->vertex_count; i++)
		{
			if (foliage[i])
			{
				lowest = fminf(lowest, mesh->vertices[i].position[1]);
				highest = fmaxf(highest, mesh->vertices[i].position[1]);
			}
		}
		for (size_t i = 0; i < mesh->vertex_count; i++)
		{
			const float *p = mesh->vertices[i].position;
			int band = (int)((p[1] - bottom) / height * (float)SHADE_BANDS);
			float out;
			float up;

			if (!foliage[i])
			{
				continue;
			}
			band = band < 0 ? 0 : (band >= SHADE_BANDS ? SHADE_BANDS - 1 : band);
			{
				float dx = p[0] - middle_x[band];
				float dz = p[2] - middle_z[band];

				out = reach[band] > 1e-3f ? fminf(sqrtf(dx * dx + dz * dz) / reach[band], 1.0f) : 1.0f;
			}
			up = highest > lowest ? (p[1] - lowest) / (highest - lowest) : 1.0f;
			// How far out in the crown, for the seasons (the outside turns first)
			mesh->plant_vertices[i].extra = out;
			mesh->vertices[i].tangent[3] *= (1.0f - CROWN_DEPTH_SHADE * (1.0f - out))
				* (1.0f - CROWN_LOW_SHADE * (1.0f - up));
		}
	}
	free(foliage);
}

// ---------------------------------------------------------------------------

// The public side (plant_grow.h)

int plant_kind_count(void)
{
	return (int)(sizeof(kinds) / sizeof(kinds[0]));
}

static const plant_kind_t *kind_at(int kind)
{
	return &kinds[kind >= 0 && kind < plant_kind_count() ? kind : 0];
}

const char *plant_kind_name(int kind) { return kind_at(kind)->name; }
bool plant_kind_is_tree(int kind) { return kind_at(kind)->shape == SHAPE_TREE; }
bool plant_kind_is_mushroom(int kind) { return kind_at(kind)->shape == SHAPE_MUSHROOM; }
float plant_kind_height(int kind) { return kind_at(kind)->height_meters; }
float plant_kind_leaf_size(int kind) { return kind_at(kind)->leaf_size; }

void plant_kind_wind(int kind, float height, float leaf_size, float wind_out[3])
{
	shape_t shape = kind_at(kind)->shape;

	// Trees: the crown moves in a fair wind, the trunk hardly; a bush (a
	// tree under a few meters) stands still but in a storm; the small ones
	// (grass, ferns, flowers, rosettes) hardly bend but in a strong wind,
	// their leaves only stir a little (a centimeter at most: a fern's or a
	// blade's "leaf" is all of it)
	if (shape == SHAPE_TREE)
	{
		bool bush = height < 4.0f;

		wind_out[0] = (bush ? 0.06f : 0.07f) * height;
		wind_out[1] = 0.15f * leaf_size;
		wind_out[2] = bush ? 0.6f : 0.35f;
	}
	else if (shape == SHAPE_TUFT || shape == SHAPE_FLOWER || shape == SHAPE_ROSETTE || shape == SHAPE_FERN)
	{
		float sway = shape == SHAPE_TUFT ? 0.15f : (shape == SHAPE_FLOWER ? 0.12f : (shape == SHAPE_FERN ? 0.08f : 0.05f));
		float stiffness = shape == SHAPE_TUFT ? 0.45f : (shape == SHAPE_FLOWER ? 0.45f : (shape == SHAPE_FERN ? 0.55f : 0.6f));

		wind_out[0] = sway * height;
		wind_out[1] = fminf((shape == SHAPE_FLOWER ? 0.04f : 0.02f) * leaf_size, 0.01f);
		wind_out[2] = stiffness;
	}
	else
	{
		wind_out[0] = 0.0f;
		wind_out[1] = 0.0f;
		wind_out[2] = 0.0f;
	}
}

void plant_recipe_wind(const plant_recipe_t *recipe, int kind, float height, float leaf_size, float wind_out[3])
{
	plant_kind_wind(kind, height, leaf_size, wind_out);
	if (recipe->sway >= 0.0f)
	{
		wind_out[0] = recipe->sway;
	}
	if (recipe->flutter >= 0.0f)
	{
		wind_out[1] = recipe->flutter;
	}
	if (recipe->stiffness >= 0.0f)
	{
		wind_out[2] = fminf(recipe->stiffness, 1.0f);
	}
}
int plant_kind_leaves_per_branch(int kind) { return kind_at(kind)->leaves_per_branch; }
int plant_kind_levels(int kind) { return kind_at(kind)->levels; }
float plant_kind_twig_scale(int kind) { return kind_at(kind)->twig_scale; }
float plant_kind_twig_meters(int kind) { return kind_at(kind)->twig_meters; }
plant_fruit_hang_t plant_kind_fruit_hang(int kind) { return kind_at(kind)->fruit_hang; }
plant_fruit_zone_t plant_kind_fruit_zone(int kind) { return kind_at(kind)->fruit_zone; }
float plant_kind_crown_start(int kind) { return kind_at(kind)->crown_start; }
float plant_kind_leaves_from(int kind) { return kind_at(kind)->leaves_from; }
float plant_kind_dead_branches(int kind) { return kind_at(kind)->dead_branches; }
float plant_kind_trunk_upright(int kind) { return kind_at(kind)->trunk_upright; }
float plant_kind_light_from_below(int kind) { return kind_at(kind)->light_from_below; }
float plant_kind_droop(int kind) { return kind_at(kind)->droop; }
float plant_kind_trunk_thickness(int kind) { return kind_at(kind)->trunk_radius_ratio; }
float plant_kind_trunk_taper(int kind) { return kind_at(kind)->trunk_taper > 0.0f ? kind_at(kind)->trunk_taper : kind_at(kind)->taper; }
float plant_kind_branch_thickness(int kind) { return kind_at(kind)->child_radius_ratio; }
float plant_kind_branch_taper(int kind) { return kind_at(kind)->taper; }
float plant_kind_thinnest_branch(int kind) { return kind_at(kind)->thinnest_radius > 0.0f ? kind_at(kind)->thinnest_radius : THINNEST_BRANCH_RADIUS; }
float plant_kind_tip_lift(int kind) { return kind_at(kind)->tip_lift; }
float plant_kind_collar(int kind) { return kind_at(kind)->collar; }
float plant_kind_root_flare(int kind) { return kind_at(kind)->root_flare; }
int plant_kind_stems(int kind) { return kind_at(kind)->stems; }
float plant_kind_fork_height(int kind) { return kind_at(kind)->fork_height; }
float plant_kind_extra_stem_chance(int kind) { return kind_at(kind)->extra_stem_chance; }
float plant_kind_gnarl(int kind) { return kind_at(kind)->gnarl; }
float plant_kind_kinks(int kind) { return kind_at(kind)->kinks; }
float plant_kind_lean_degrees(int kind) { return kind_at(kind)->lean_degrees; }
float plant_kind_lean_length(int kind) { return kind_at(kind)->lean_length; }
float plant_kind_straighten(int kind) { return kind_at(kind)->straighten; }
float plant_kind_wind_side(int kind) { return kind_at(kind)->wind_side; }
float plant_kind_moss(int kind) { return kind_at(kind)->moss; }
float plant_kind_tips(int kind) { return kind_at(kind)->tips; }
float plant_kind_autumn(int kind) { return kind_at(kind)->autumn; }
int plant_kind_autumn_first_month(int kind) { return kind_at(kind)->autumn_first_month > 0 ? kind_at(kind)->autumn_first_month : 10; }
int plant_kind_autumn_last_month(int kind) { return kind_at(kind)->autumn_last_month > 0 ? kind_at(kind)->autumn_last_month : 11; }
int plant_kind_leaves_month(int kind) { return kind_at(kind)->leaves_month > 0 ? kind_at(kind)->leaves_month : 4; }
float plant_kind_snow_sag(int kind) { return kind_at(kind)->snow_sag; }
bool plant_kind_falls(int kind) { return kind_at(kind)->falls; }

void plant_kind_autumn_colors(int kind, float colors_out[2][3])
{
	memcpy(colors_out, kind_at(kind)->autumn_colors, sizeof(kind_at(kind)->autumn_colors));
}

int plant_kind_branches(int kind, int level)
{
	return level >= 0 && level < MAX_LEVELS ? kind_at(kind)->children[level] : 0;
}

int plant_kind_find(const char *name)
{
	for (int k = 0; k < plant_kind_count(); k++)
	{
		if (strcmp(kinds[k].name, name) == 0 || (kinds[k].alias != NULL && strcmp(kinds[k].alias, name) == 0))
		{
			return k;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Recipes

void plant_recipe_defaults(plant_recipe_t *recipe)
{
	memset(recipe, 0, sizeof(*recipe));
	snprintf(recipe->kind, sizeof(recipe->kind), "oak");
	recipe->seed = 1;
	recipe->bark_meters = 1.5f;
	recipe->stalk_color[0] = -1.0f;
	recipe->leaves_per_branch = -1;
	recipe->twig_share = 0.7f;
	recipe->curved_twigs = false;
	recipe->brushes = false;
	recipe->brush_strips = 10;
	recipe->brush_spacing = 0.5f;
	recipe->snow_bake = false;
	recipe->fruit_shape = PLANT_FRUIT_SPHERE;
	recipe->fruit_length = 0.03f;
	recipe->fruit_hang = PLANT_FRUIT_KINDS;
	recipe->fruit_zone = PLANT_FRUIT_KINDS;
	recipe->cluster_minimum = 1;
	recipe->cluster_maximum = 1;
	recipe->fruit_per_branch = 1.0f;
	recipe->crown_start = -1.0f;
	recipe->leaves_from = -1.0f;
	recipe->dead_branches = -1.0f;
	recipe->trunk_upright = -1.0f;
	recipe->light_from_below = -1.0f;
	recipe->droop = -1.0f;
	recipe->trunk_thickness = -1.0f;
	recipe->trunk_taper = -1.0f;
	recipe->branch_thickness = -1.0f;
	recipe->branch_taper = -1.0f;
	recipe->thinnest_branch = -1.0f;
	plant_mushroom_defaults(&recipe->mushroom);
	recipe->frond_angle = -1.0f;
	recipe->frond_droop = -1.0f;
	recipe->tip_lift = -1.0f;
	recipe->collar = -1.0f;
	recipe->root_flare = -1.0f;
	recipe->stems = -1;
	recipe->twig_meters = -1.0f;
	recipe->fork_height = -1.0f;
	recipe->extra_stem_chance = -1.0f;
	recipe->gnarl = -1.0f;
	recipe->kinks = -1.0f;
	recipe->lean_degrees = -1.0f;
	recipe->lean_length = -1.0f;
	recipe->straighten = -1.0f;
	recipe->wind_side = -1.0f;
	recipe->moss = -1.0f;
	recipe->tips = -1.0f;
	recipe->autumn_colors[0][0] = -1.0f;
	recipe->autumn = -1.0f;
	recipe->falls = -1;
	recipe->autumn_first_month = 0;
	recipe->autumn_last_month = 0;
	recipe->leaves_month = 0;
	recipe->snow_sag = -1.0f;
	recipe->sway = -1.0f;
	recipe->flutter = -1.0f;
	recipe->stiffness = -1.0f;
}

bool plant_pictures_add(plant_pictures_t *pictures, const char *color)
{
	plant_picture_t *picture;

	if (pictures->count >= PLANT_MAX_PICTURES || color == NULL || color[0] == '\0')
	{
		return false;
	}
	picture = &pictures->pictures[pictures->count++];
	memset(picture, 0, sizeof(*picture));
	snprintf(picture->color, sizeof(picture->color), "%s", color);
	return true;
}

void plant_picture_months(const plant_picture_t *picture, char sources_out[12][PLANT_PATH_LENGTH], bool own_out[12])
{
	resolve_months(picture->color, (const char (*)[PATH_LENGTH])picture->months, sources_out, own_out);
}

void plant_pictures_remove(plant_pictures_t *pictures, int index)
{
	if (index < 0 || index >= pictures->count)
	{
		return;
	}
	memmove(&pictures->pictures[index], &pictures->pictures[index + 1], sizeof(plant_picture_t) * (size_t)(pictures->count - index - 1));
	pictures->count--;
}

static const char *const fruit_shape_names[] = { "sphere", "spindle", "pod", "card" };
static const char *const fruit_hang_names[] = { "down", "up", "out" };
static const char *const fruit_zone_names[] = { "outer", "top", "all" };

static int name_index(const char *const *names, int count, const char *name)
{
	for (int i = 0; i < count; i++)
	{
		if (strcmp(names[i], name) == 0)
		{
			return i;
		}
	}
	return -1;
}

// "a | b | c" into a picture's three paths
static void split_picture(char *value, char out[3][PLANT_PATH_LENGTH])
{
	char *part = value;

	for (int k = 0; k < 3; k++)
	{
		char *bar = part != NULL ? strchr(part, '|') : NULL;

		if (bar != NULL)
		{
			*bar = '\0';
		}
		snprintf(out[k], PLANT_PATH_LENGTH, "%s", part != NULL ? trimmed(part) : "");
		part = bar != NULL ? bar + 1 : NULL;
	}
}

bool plant_recipe_load(plant_recipe_t *recipe, const char *path)
{
	size_t size = 0;
	char *text = (char *)read_file(path, &size);
	char *line;
	char *next;
	int line_number = 0;

	if (text == NULL)
	{
		plant_error("Cannot read the recipe '%s'\n", path);
		return false;
	}
	plant_recipe_defaults(recipe);
	for (line = text; line != NULL; line = next)
	{
		char *end = strchr(line, '\n');
		char *comment;
		char *equals;
		char *key;
		char *value;

		next = end != NULL ? end + 1 : NULL;
		if (end != NULL)
		{
			*end = '\0';
		}
		line_number++;
		comment = strchr(line, ';');
		if (comment != NULL)
		{
			*comment = '\0';
		}
		equals = strchr(line, '=');
		if (equals == NULL)
		{
			continue;
		}
		*equals = '\0';
		key = trimmed(line);
		value = trimmed(equals + 1);
		if (strcmp(key, "kind") == 0) { snprintf(recipe->kind, sizeof(recipe->kind), "%s", value); }
		else if (strcmp(key, "seed") == 0) { recipe->seed = (unsigned)strtoul(value, NULL, 10); }
		else if (strcmp(key, "height") == 0) { recipe->height = strtof(value, NULL); }
		else if (strcmp(key, "atlas") == 0) { snprintf(recipe->atlas, sizeof(recipe->atlas), "%s", value); }
		else if (strcmp(key, "output") == 0) { snprintf(recipe->output, sizeof(recipe->output), "%s", value); }
		else if (strcmp(key, "bark") == 0) { split_picture(value, recipe->bark); }
		else if (strcmp(key, "bark_meters") == 0) { recipe->bark_meters = fmaxf(strtof(value, NULL), 0.05f); }
		else if (strcmp(key, "leaf") == 0 || strcmp(key, "twig") == 0 || strcmp(key, "head") == 0 || strcmp(key, "fruit") == 0)
		{
			// The picture, then | m<month> = <picture> for each month's own
			// chosen by hand (anything else after a |: the maps of before, left out)
			plant_pictures_t *list = key[0] == 'l' ? &recipe->leaves : (key[0] == 't' ? &recipe->twigs
				: (key[0] == 'h' ? &recipe->heads : &recipe->fruit));
			char *part = value;
			bool first = true;

			while (part != NULL)
			{
				char *bar = strchr(part, '|');
				char *field;

				if (bar != NULL)
				{
					*bar = '\0';
				}
				field = trimmed(part);
				if (first)
				{
					if (!plant_pictures_add(list, field))
					{
						break;
					}
				}
				else
				{
					int month = 0;
					char *month_equals = strchr(field, '=');

					if (month_equals != NULL && field[0] == 'm' && sscanf(field + 1, "%d", &month) == 1 && month >= 1 && month <= 12)
					{
						snprintf(list->pictures[list->count - 1].months[month - 1], PLANT_PATH_LENGTH, "%s", trimmed(month_equals + 1));
					}
					else if (strcmp(field, "small") == 0)
					{
						list->pictures[list->count - 1].cell_size = PLANT_CELL_SMALL;
					}
					else if (strcmp(field, "big") == 0)
					{
						list->pictures[list->count - 1].cell_size = PLANT_CELL_BIG;
					}

				}
				first = false;
				part = bar != NULL ? bar + 1 : NULL;
			}
		}
		else if (strcmp(key, "leaf_size") == 0) { recipe->leaf_size = strtof(value, NULL); }
		else if (strcmp(key, "leaves_per_branch") == 0) { recipe->leaves_per_branch = (int)strtol(value, NULL, 10); }
		else if (strcmp(key, "branches") == 0)
		{
			char *cursor = value;

			recipe->branch_levels = 0;
			while (*cursor != '\0' && recipe->branch_levels < MAX_LEVELS)
			{
				char *after;
				long number = strtol(cursor, &after, 10);

				if (after == cursor)
				{
					break;
				}
				recipe->branches[recipe->branch_levels++] = (int)number;
				cursor = after;
				while (*cursor == ',' || *cursor == ' ')
				{
					cursor++;
				}
			}
		}
		else if (strcmp(key, "branch_length") == 0) { recipe->branch_length = strtof(value, NULL); }
		else if (strcmp(key, "twig_scale") == 0) { recipe->twig_scale = strtof(value, NULL); }
		else if (strcmp(key, "twig_share") == 0) { recipe->twig_share = strtof(value, NULL); }
		else if (strcmp(key, "loose_leaves") == 0) { recipe->loose_leaves = strtof(value, NULL); }
		else if (strcmp(key, "fruit_shape") == 0)
		{
			int index = name_index(fruit_shape_names, 4, value);

			recipe->fruit_shape = index >= 0 ? (plant_fruit_shape_t)index : PLANT_FRUIT_SPHERE;
		}
		else if (strcmp(key, "fruit_size") == 0)
		{
			char *comma = strchr(value, ',');

			recipe->fruit_length = strtof(value, NULL);
			recipe->fruit_width = comma != NULL ? strtof(comma + 1, NULL) : 0.0f;
		}
		else if (strcmp(key, "fruit_hang") == 0) { recipe->fruit_hang = name_index(fruit_hang_names, 3, value); }
		else if (strcmp(key, "fruit_zone") == 0) { recipe->fruit_zone = name_index(fruit_zone_names, 3, value); }
		else if (strcmp(key, "fruit_stalk") == 0) { recipe->fruit_stalk = strtof(value, NULL); }
		else if (strcmp(key, "fruit_cluster") == 0)
		{
			char *comma = strchr(value, ',');

			recipe->cluster_minimum = (int)strtol(value, NULL, 10);
			recipe->cluster_maximum = comma != NULL ? (int)strtol(comma + 1, NULL, 10) : recipe->cluster_minimum;
		}
		else if (strcmp(key, "fruit_per_branch") == 0) { recipe->fruit_per_branch = strtof(value, NULL); }
		else if (strcmp(key, "fruit_bunch") == 0) { recipe->fruit_bunch = strtof(value, NULL); }
		else if (strcmp(key, "fruit_tip") == 0) { recipe->fruit_tip = strtof(value, NULL); }
		else if (strcmp(key, "crown_start") == 0) { recipe->crown_start = strtof(value, NULL); }
		else if (strcmp(key, "leaves_from") == 0) { recipe->leaves_from = strtof(value, NULL); }
		else if (strcmp(key, "dead_branches") == 0) { recipe->dead_branches = strtof(value, NULL); }
		else if (strcmp(key, "trunk_upright") == 0) { recipe->trunk_upright = strtof(value, NULL); }
		else if (strcmp(key, "light_from_below") == 0) { recipe->light_from_below = strtof(value, NULL); }
		else if (strcmp(key, "droop") == 0) { recipe->droop = strtof(value, NULL); }
		else if (strcmp(key, "trunk_thickness") == 0) { recipe->trunk_thickness = strtof(value, NULL); }
		else if (strcmp(key, "trunk_taper") == 0) { recipe->trunk_taper = strtof(value, NULL); }
		else if (strcmp(key, "branch_thickness") == 0) { recipe->branch_thickness = strtof(value, NULL); }
		else if (strcmp(key, "branch_taper") == 0) { recipe->branch_taper = strtof(value, NULL); }
		else if (strcmp(key, "thinnest_branch") == 0) { recipe->thinnest_branch = strtof(value, NULL); }
		else if (strcmp(key, "frond_angle") == 0) { recipe->frond_angle = strtof(value, NULL); }
		else if (strcmp(key, "frond_droop") == 0) { recipe->frond_droop = strtof(value, NULL); }
		else if (mushroom_key(&recipe->mushroom, key, value)) { }
		else if (strncmp(key, "herb_", 5) == 0 && plant_herb_set(&recipe->herb, key + 5, value)) { }
		else if (strcmp(key, "tip_lift") == 0) { recipe->tip_lift = strtof(value, NULL); }
		else if (strcmp(key, "collar") == 0) { recipe->collar = strtof(value, NULL); }
		else if (strcmp(key, "root_flare") == 0) { recipe->root_flare = strtof(value, NULL); }
		else if (strcmp(key, "stems") == 0) { recipe->stems = atoi(value); }
		else if (strcmp(key, "twig_length") == 0) { recipe->twig_meters = strtof(value, NULL); }
		else if (strcmp(key, "curved_twigs") == 0) { recipe->curved_twigs = atoi(value) != 0; }
		else if (strcmp(key, "brushes") == 0) { recipe->brushes = atoi(value) != 0; }
		else if (strcmp(key, "brush_strips") == 0) { recipe->brush_strips = atoi(value); }
		else if (strcmp(key, "brush_spacing") == 0) { recipe->brush_spacing = strtof(value, NULL); }
		else if (strcmp(key, "snow_bake") == 0) { recipe->snow_bake = atoi(value) != 0; }
		else if (strcmp(key, "fork_height") == 0) { recipe->fork_height = strtof(value, NULL); }
		else if (strcmp(key, "extra_stem_chance") == 0) { recipe->extra_stem_chance = strtof(value, NULL); }
		else if (strcmp(key, "gnarl") == 0) { recipe->gnarl = strtof(value, NULL); }
		else if (strcmp(key, "kinks") == 0) { recipe->kinks = strtof(value, NULL); }
		else if (strcmp(key, "lean_degrees") == 0) { recipe->lean_degrees = strtof(value, NULL); }
		else if (strcmp(key, "lean_length") == 0) { recipe->lean_length = strtof(value, NULL); }
		else if (strcmp(key, "straighten") == 0) { recipe->straighten = strtof(value, NULL); }
		else if (strcmp(key, "wind_side") == 0) { recipe->wind_side = strtof(value, NULL); }
		else if (strcmp(key, "moss") == 0) { recipe->moss = strtof(value, NULL); }
		else if (strcmp(key, "tips") == 0) { recipe->tips = strtof(value, NULL); }
		else if (strcmp(key, "autumn") == 0) { recipe->autumn = strtof(value, NULL); }
		else if (strcmp(key, "falls") == 0) { recipe->falls = atoi(value); }
		else if (strcmp(key, "autumn_months") == 0)
		{
			int first = 0;
			int last = 0;

			if (sscanf(value, "%d , %d", &first, &last) == 2)
			{
				recipe->autumn_first_month = first >= 1 && first <= 12 ? first : 0;
				recipe->autumn_last_month = last >= 1 && last <= 12 ? last : 0;
			}
		}
		else if (strcmp(key, "leaves_month") == 0)
		{
			int month = atoi(value);

			recipe->leaves_month = month >= 1 && month <= 12 ? month : 0;
		}
		else if (strcmp(key, "snow_sag") == 0) { recipe->snow_sag = strtof(value, NULL); }
		else if (strcmp(key, "sway") == 0) { recipe->sway = strtof(value, NULL); }
		else if (strcmp(key, "flutter") == 0) { recipe->flutter = strtof(value, NULL); }
		else if (strcmp(key, "stiffness") == 0) { recipe->stiffness = strtof(value, NULL); }
		else if (strcmp(key, "snow_cover") == 0 || strcmp(key, "snow_thickness") == 0 || strcmp(key, "snow_width") == 0
			|| strcmp(key, "snow_length") == 0 || strcmp(key, "snow_from_radius") == 0)
		{
			// The snow's pillows of before: gone
		}
		else if (strcmp(key, "autumn_colors") == 0)
		{
			float c[6] = { -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

			if (sscanf(value, "%f, %f, %f, %f, %f, %f", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 6)
			{
				memcpy(recipe->autumn_colors, c, sizeof(c));
			}
		}
		else if (strcmp(key, "stalk_color") == 0)
		{
			float c[3] = { -1.0f, 0.0f, 0.0f };

			if (sscanf(value, "%f, %f, %f", &c[0], &c[1], &c[2]) == 3)
			{
				memcpy(recipe->stalk_color, c, sizeof(c));
			}
		}
		else
		{
			fprintf(stderr, "%s:%d: no setting '%s' (left out)\n", path, line_number, key);
		}
	}
	free(text);
	return true;
}

static void save_pictures(FILE *file, const char *key, const plant_pictures_t *pictures)
{
	for (int i = 0; i < pictures->count; i++)
	{
		const plant_picture_t *picture = &pictures->pictures[i];

		fprintf(file, "%s = %s", key, picture->color);
		if (picture->cell_size != PLANT_CELL_AS_KIND)
		{
			fprintf(file, " | %s", picture->cell_size == PLANT_CELL_SMALL ? "small" : "big");
		}

		for (int m = 0; m < 12; m++)
		{
			if (picture->months[m][0] != '\0')
			{
				fprintf(file, " | m%d = %s", m + 1, picture->months[m]);
			}
		}
		fprintf(file, "\n");
	}
}

bool plant_recipe_save(const plant_recipe_t *recipe, const char *path)
{
	FILE *file = fopen(path, "w");

	if (file == NULL)
	{
		plant_error("Cannot write the recipe '%s'\n", path);
		return false;
	}
	fprintf(file, "; A plant's recipe (tools/potop_plant --recipe, tools/plant_editor). Paths as seen from\n");
	fprintf(file, "; the project's directory; 0 (-1 for leaves_per_branch and the crown's and the trunk's shape: crown_start\n");
	fprintf(file, "; to wind_side; kind for fruit_hang and fruit_zone; nothing for branches) leaves a\n");
	fprintf(file, "; setting to the kind.\n\n");
	fprintf(file, "kind = %s\nseed = %u\nheight = %g\n", recipe->kind, recipe->seed, (double)recipe->height);
	fprintf(file, "output = %s\natlas = %s\n\n", recipe->output, recipe->atlas);
	if (recipe->bark[0][0] != '\0')
	{
		fprintf(file, "bark = %s", recipe->bark[0]);
		if (recipe->bark[1][0] != '\0' || recipe->bark[2][0] != '\0')
		{
			fprintf(file, " | %s", recipe->bark[1]);
		}
		if (recipe->bark[2][0] != '\0')
		{
			fprintf(file, " | %s", recipe->bark[2]);
		}
		fprintf(file, "\n");
	}
	fprintf(file, "bark_meters = %g\n", (double)recipe->bark_meters);
	if (recipe->stalk_color[0] >= 0.0f)
	{
		fprintf(file, "stalk_color = %g, %g, %g\n", (double)recipe->stalk_color[0], (double)recipe->stalk_color[1], (double)recipe->stalk_color[2]);
	}
	save_pictures(file, "leaf", &recipe->leaves);
	save_pictures(file, "twig", &recipe->twigs);
	save_pictures(file, "head", &recipe->heads);
	save_pictures(file, "fruit", &recipe->fruit);
	fprintf(file, "\nleaf_size = %g\nleaves_per_branch = %d\nbranches =", (double)recipe->leaf_size, recipe->leaves_per_branch);
	for (int i = 0; i < recipe->branch_levels; i++)
	{
		fprintf(file, "%s %d", i > 0 ? "," : "", recipe->branches[i]);
	}
	fprintf(file, "\nbranch_length = %g\ntwig_scale = %g\ntwig_share = %g\nloose_leaves = %g\n", (double)recipe->branch_length,
		(double)recipe->twig_scale, (double)recipe->twig_share, (double)recipe->loose_leaves);
	fprintf(file, "crown_start = %g\nleaves_from = %g\ndroop = %g\ntip_lift = %g\ncollar = %g\nroot_flare = %g\n",
		(double)recipe->crown_start, (double)recipe->leaves_from, (double)recipe->droop, (double)recipe->tip_lift, (double)recipe->collar,
		(double)recipe->root_flare);
	fprintf(file, "twig_length = %g\n", (double)recipe->twig_meters);
	fprintf(file, "curved_twigs = %d\n", recipe->curved_twigs ? 1 : 0);
	fprintf(file, "brushes = %d\n", recipe->brushes ? 1 : 0);
	fprintf(file, "brush_strips = %d\nbrush_spacing = %g\n", recipe->brush_strips, (double)recipe->brush_spacing);
	fprintf(file, "dead_branches = %g\ntrunk_upright = %g\nlight_from_below = %g\n", (double)recipe->dead_branches, (double)recipe->trunk_upright,
		(double)recipe->light_from_below);
	fprintf(file, "snow_bake = %d\n", recipe->snow_bake ? 1 : 0);
	fprintf(file, "trunk_thickness = %g\ntrunk_taper = %g\nbranch_thickness = %g\nbranch_taper = %g\nthinnest_branch = %g\n",
		(double)recipe->trunk_thickness, (double)recipe->trunk_taper, (double)recipe->branch_thickness, (double)recipe->branch_taper,
		(double)recipe->thinnest_branch);
	fprintf(file, "frond_angle = %g\nfrond_droop = %g\n", (double)recipe->frond_angle, (double)recipe->frond_droop);
	mushroom_save(file, &recipe->mushroom);
	herb_save(file, &recipe->herb);
	fprintf(file, "stems = %d\nfork_height = %g\nextra_stem_chance = %g\ngnarl = %g\nkinks = %g\n", recipe->stems,
		(double)recipe->fork_height, (double)recipe->extra_stem_chance, (double)recipe->gnarl, (double)recipe->kinks);
	fprintf(file, "lean_degrees = %g\nlean_length = %g\nstraighten = %g\nwind_side = %g\n", (double)recipe->lean_degrees,
		(double)recipe->lean_length, (double)recipe->straighten, (double)recipe->wind_side);
	fprintf(file, "moss = %g\ntips = %g\nautumn = %g\nfalls = %d\n", (double)recipe->moss, (double)recipe->tips, (double)recipe->autumn,
		recipe->falls);
	if (recipe->autumn_first_month > 0 && recipe->autumn_last_month > 0)
	{
		fprintf(file, "autumn_months = %d, %d\n", recipe->autumn_first_month, recipe->autumn_last_month);
	}
	if (recipe->leaves_month > 0)
	{
		fprintf(file, "leaves_month = %d\n", recipe->leaves_month);
	}
	fprintf(file, "snow_sag = %g\n", (double)recipe->snow_sag);
	fprintf(file, "sway = %g\nflutter = %g\nstiffness = %g\n", (double)recipe->sway, (double)recipe->flutter, (double)recipe->stiffness);
	if (recipe->autumn_colors[0][0] >= 0.0f)
	{
		fprintf(file, "autumn_colors = %g, %g, %g, %g, %g, %g\n", (double)recipe->autumn_colors[0][0], (double)recipe->autumn_colors[0][1],
			(double)recipe->autumn_colors[0][2], (double)recipe->autumn_colors[1][0], (double)recipe->autumn_colors[1][1],
			(double)recipe->autumn_colors[1][2]);
	}
	fprintf(file, "\nfruit_shape = %s\nfruit_size = %g, %g\nfruit_hang = %s\nfruit_zone = %s\nfruit_stalk = %g\n",
		fruit_shape_names[recipe->fruit_shape >= 0 && recipe->fruit_shape < 4 ? recipe->fruit_shape : 0], (double)recipe->fruit_length,
		(double)recipe->fruit_width, recipe->fruit_hang >= 0 && recipe->fruit_hang < 3 ? fruit_hang_names[recipe->fruit_hang] : "kind",
		recipe->fruit_zone >= 0 && recipe->fruit_zone < 3 ? fruit_zone_names[recipe->fruit_zone] : "kind", (double)recipe->fruit_stalk);
	fprintf(file, "fruit_cluster = %d, %d\nfruit_per_branch = %g\n", recipe->cluster_minimum, recipe->cluster_maximum,
		(double)recipe->fruit_per_branch);
	fprintf(file, "fruit_bunch = %g\nfruit_tip = %g\n", (double)recipe->fruit_bunch, (double)recipe->fruit_tip);
	fclose(file);
	return true;
}

// ---------------------------------------------------------------------------
// The atlas

struct plant_atlas
{
	atlas_t atlas;
};

// The pictures in the atlas, each with its months' own (in cells of their
// own, of its shape)
// (small_by_kind: in the small cells unless chosen otherwise, if not long)
// A head's picture from below: name_under.png beside name.png
static void under_picture(const char *color, char *out, size_t out_size)
{
	char stem[PATH_LENGTH];
	char extension[32];

	picture_stem(color, stem, sizeof(stem), extension, sizeof(extension));
	snprintf(out, out_size, "%s_under%s", stem, extension);
}

static bool place_pictures(atlas_t *atlas, const plant_pictures_t *pictures, bool small_by_kind)
{
	for (int i = 0; i < pictures->count; i++)
	{
		const plant_picture_t *picture = &pictures->pictures[i];
		char sources[12][PATH_LENGTH];
		bool own[12];
		cell_shape_t shape;
		cell_shape_t unused_shape;

		bool small = picture->cell_size == PLANT_CELL_SMALL || (picture->cell_size == PLANT_CELL_AS_KIND && small_by_kind);

		if (atlas_place(atlas, picture, -1, small, &shape) < 0)
		{
			return false;
		}
		// (a flower's from below, if there is one: in the same sort of cell)
		{
			plant_picture_t under;

			memset(&under, 0, sizeof(under));
			under_picture(picture->color, under.color, sizeof(under.color));
			under.cell_size = picture->cell_size;
			if (file_exists(under.color) && atlas_find(atlas, under.color, &unused_shape) < 0
				&& atlas_place(atlas, &under, -1, small, &unused_shape) < 0)
			{
				return false;
			}
		}
		resolve_months(picture->color, (const char (*)[PATH_LENGTH])picture->months, sources, own);
		for (int m = 0; m < 12; m++)
		{
			plant_picture_t month;

			if (!own[m] || strcmp(sources[m], picture->color) == 0)
			{
				continue;
			}
			memset(&month, 0, sizeof(month));
			snprintf(month.color, sizeof(month.color), "%s", sources[m]);
			if (atlas_find(atlas, month.color, &unused_shape) < 0 && atlas_place(atlas, &month, (int)shape, false, &unused_shape) < 0)
			{
				return false;
			}
		}
	}
	return true;
}

plant_atlas_t *plant_atlas_prepare(const plant_recipe_t *recipe, const char *data_directory)
{
	plant_atlas_t *result;
	int kind = plant_kind_find(recipe->kind);
	bool small_leaves;
	static plant_pictures_t mushroom_pictures;

	if (kind < 0)
	{
		plant_error("No kind '%s'\n", recipe->kind);
		return NULL;
	}
	// The small plants' little leaves go in the small cells
	{
		float leaf_size = recipe->leaf_size > 0.0f ? recipe->leaf_size : kinds[kind].leaf_size;

		small_leaves = kinds[kind].shape != SHAPE_TREE && kinds[kind].shape != SHAPE_MUSHROOM && leaf_size <= 0.1f;
	}
	result = calloc(1, sizeof(plant_atlas_t));
	if (result == NULL)
	{
		plant_error("Out of memory\n");
		return NULL;
	}
	// A mushroom's texture: its picture (as data/mushrooms/mushroom_template.png
	// lays it out), else one made from its colors
	memset(&mushroom_pictures, 0, sizeof(mushroom_pictures));
	if (kinds[kind].shape == SHAPE_MUSHROOM)
	{
		mushroom_pictures.count = 1;
		mushroom_pictures.pictures[0].cell_size = recipe->leaves.count > 0 ? recipe->leaves.pictures[0].cell_size : PLANT_CELL_AS_KIND;
		mushroom_texture_path(recipe, data_directory, mushroom_pictures.pictures[0].color, sizeof(mushroom_pictures.pictures[0].color));
		if (recipe->leaves.count == 0 && !mushroom_make_texture(&recipe->mushroom, mushroom_pictures.pictures[0].color, false))
		{
			plant_atlas_free(result);
			return NULL;
		}
	}
	if (!atlas_open(&result->atlas, recipe->atlas[0] != '\0' ? recipe->atlas : kinds[kind].atlas, data_directory)
		|| !place_pictures(&result->atlas, kinds[kind].shape == SHAPE_MUSHROOM ? &mushroom_pictures : &recipe->leaves, small_leaves)
		|| !place_pictures(&result->atlas, &recipe->twigs, false)
		|| !place_pictures(&result->atlas, &recipe->heads, kinds[kind].shape != SHAPE_TREE)
		|| !place_pictures(&result->atlas, &recipe->fruit, false)
		|| !atlas_build(&result->atlas))
	{
		plant_atlas_free(result);
		return NULL;
	}
	return result;
}

int plant_atlas_size(const plant_atlas_t *atlas) { return atlas->atlas.size; }
const uint8_t *plant_atlas_pixels(const plant_atlas_t *atlas) { return atlas->atlas.pixels; }
const char *plant_atlas_name(const plant_atlas_t *atlas) { return atlas->atlas.name; }

bool plant_atlas_write(plant_atlas_t *atlas)
{
	return atlas_save(&atlas->atlas) && atlas_write_file(&atlas->atlas);
}

void plant_atlas_free(plant_atlas_t *atlas)
{
	if (atlas == NULL)
	{
		return;
	}
	free(atlas->atlas.pixels);
	free(atlas);
}

// ---------------------------------------------------------------------------
// Growing one

static bool find_places(const atlas_t *atlas, const plant_pictures_t *pictures, cell_place_t *places, int *count_out)
{
	for (int i = 0; i < pictures->count; i++)
	{
		cell_shape_t shape;
		int cell = atlas_find(atlas, pictures->pictures[i].color, &shape);

		if (cell < 0)
		{
			plant_error("'%s' is not in the %s atlas (prepare it again)\n", pictures->pictures[i].color, atlas->name);
			return false;
		}
		places[i] = atlas_cell_place(atlas, shape, cell);
	}
	*count_out = pictures->count;
	return true;
}

// The heads' pictures from below, where there are some
static bool find_under_places(const atlas_t *atlas, const plant_pictures_t *pictures, plant_t *plant)
{
	for (int i = 0; i < pictures->count && i < MAX_SOURCES; i++)
	{
		char under[PATH_LENGTH + 16];
		cell_shape_t shape;
		int cell;

		under_picture(pictures->pictures[i].color, under, sizeof(under));
		cell = atlas_find(atlas, under, &shape);
		plant->head_has_under[i] = cell >= 0;
		if (cell >= 0)
		{
			plant->heads_under[i] = atlas_cell_place(atlas, shape, cell);
		}
	}
	return true;
}

// The cells of the plant's pictures that show others in some months
// (object_file_month_cell_t): where each is, and how far each month's is
static bool find_month_cells(const atlas_t *atlas, const plant_recipe_t *recipe, plant_mesh_t *mesh_out)
{
	const plant_pictures_t *lists[4] = { &recipe->leaves, &recipe->twigs, &recipe->heads, &recipe->fruit };
	float size = (float)atlas->size;

	mesh_out->month_cell_count = 0;
	for (int l = 0; l < 4; l++)
	{
		for (int p = 0; p < lists[l]->count; p++)
		{
			cell_shape_t shape;
			int cell = atlas_find(atlas, lists[l]->pictures[p].color, &shape);
			int rectangle[4];
			bool changes = false;
			bool listed = false;
			object_file_month_cell_t *entry;

			if (cell < 0)
			{
				continue;
			}
			for (int m = 0; m < 12; m++)
			{
				changes = changes || atlas->month_cells[shape][cell][m] != cell;
			}
			cell_rectangle(atlas, shape, cell, rectangle);
			for (int e = 0; e < mesh_out->month_cell_count; e++)
			{
				listed = listed || (mesh_out->month_cells[e].cell[0] == (float)rectangle[0] / size
					&& mesh_out->month_cells[e].cell[1] == (float)rectangle[1] / size);
			}
			if (!changes || listed)
			{
				continue;
			}
			if (mesh_out->month_cell_count >= PLANT_MAX_MONTH_CELLS)
			{
				plant_error("More than %d pictures with months of their own in one plant\n", PLANT_MAX_MONTH_CELLS);
				return false;
			}
			entry = &mesh_out->month_cells[mesh_out->month_cell_count++];
			entry->cell[0] = (float)rectangle[0] / size;
			entry->cell[1] = (float)rectangle[1] / size;
			entry->cell[2] = (float)(rectangle[0] + rectangle[2]) / size;
			entry->cell[3] = (float)(rectangle[1] + rectangle[3]) / size;
			for (int m = 0; m < 12; m++)
			{
				int other[4];

				cell_rectangle(atlas, shape, atlas->month_cells[shape][cell][m], other);
				entry->month_offsets[m][0] = (float)(other[0] - rectangle[0]) / size;
				entry->month_offsets[m][1] = (float)(other[1] - rectangle[1]) / size;
			}
		}
	}
	return true;
}

bool plant_grow(const plant_recipe_t *recipe, const plant_atlas_t *atlas, plant_mesh_t *mesh_out)
{
	static plant_t plant;
	static plant_kind_t kind;
	int kind_index = plant_kind_find(recipe->kind);

	memset(mesh_out, 0, sizeof(*mesh_out));
	if (kind_index < 0)
	{
		plant_error("No kind '%s'\n", recipe->kind);
		return false;
	}
	memset(&plant, 0, sizeof(plant));
	kind = kinds[kind_index];
	// What the recipe changes in the kind
	if (recipe->leaves_per_branch >= 0)
	{
		kind.leaves_per_branch = recipe->leaves_per_branch < 200 ? recipe->leaves_per_branch : 200;
	}
	for (int k = 0; k < recipe->branch_levels && k < kind.levels; k++)
	{
		kind.children[k] = recipe->branches[k] < 0 ? 0 : (recipe->branches[k] < 100 ? recipe->branches[k] : 100);
	}
	if (recipe->branch_length > 0.0f)
	{
		kind.length_ratio[0] *= fminf(recipe->branch_length, 4.0f);
	}
	if (recipe->twig_meters >= 0.0f)
	{
		kind.twig_meters = fminf(recipe->twig_meters, 10.0f);
	}
	if (recipe->twig_scale > 0.0f)
	{
		kind.twig_scale = fminf(recipe->twig_scale, 5.0f);
	}
	// (a test, for now the fir's alone)
	kind.twig_curved = recipe->curved_twigs && kind.name != NULL && strcmp(kind.name, "fir") == 0;
	kind.brushes = kind.brushes || recipe->brushes;
	kind.brush_strips = recipe->brush_strips < 3 ? 3 : (recipe->brush_strips > 32 ? 32 : recipe->brush_strips);
	kind.brush_spacing = fminf(fmaxf(recipe->brush_spacing, 0.1f), 2.0f);
	// Below 0: the kind's
	if (recipe->crown_start >= 0.0f)
	{
		kind.crown_start = fminf(recipe->crown_start, 0.95f);
	}
	if (recipe->light_from_below >= 0.0f)
	{
		kind.light_from_below = fminf(recipe->light_from_below, 10.0f);
	}
	if (recipe->trunk_upright >= 0.0f)
	{
		kind.trunk_upright = fminf(recipe->trunk_upright, 3.0f);
	}
	if (recipe->dead_branches >= 0.0f)
	{
		kind.dead_branches = fminf(recipe->dead_branches, 60.0f);
	}
	if (recipe->leaves_from >= 0.0f)
	{
		kind.leaves_from = fminf(recipe->leaves_from, 0.95f);
	}
	if (recipe->droop >= 0.0f)
	{
		kind.droop = fminf(recipe->droop, 1.0f);
	}
	if (recipe->tip_lift >= 0.0f)
	{
		kind.tip_lift = fminf(recipe->tip_lift, 5.0f);
	}
	// The thickness (the trunk's taper from the kind's own, or its
	// branches', before the branches' is changed)
	kind.trunk_taper = recipe->trunk_taper >= 0.0f ? fminf(fmaxf(recipe->trunk_taper, 0.01f), 1.0f)
		: (kind.trunk_taper > 0.0f ? kind.trunk_taper : kind.taper);
	if (recipe->trunk_thickness >= 0.0f)
	{
		kind.trunk_radius_ratio = fminf(fmaxf(recipe->trunk_thickness, 0.001f), 0.3f);
	}
	if (recipe->branch_thickness >= 0.0f)
	{
		kind.child_radius_ratio = fminf(fmaxf(recipe->branch_thickness, 0.01f), 1.0f);
	}
	if (recipe->branch_taper >= 0.0f)
	{
		kind.taper = fminf(fmaxf(recipe->branch_taper, 0.01f), 1.0f);
	}
	if (recipe->thinnest_branch >= 0.0f)
	{
		kind.thinnest_radius = fminf(fmaxf(recipe->thinnest_branch, 0.0005f), 0.2f);
	}
	if (recipe->collar >= 0.0f)
	{
		kind.collar = fminf(recipe->collar, 3.0f);
	}
	if (recipe->root_flare >= 0.0f)
	{
		kind.root_flare = fminf(recipe->root_flare, 3.0f);
	}
	if (recipe->stems >= 1)
	{
		kind.stems = recipe->stems < 12 ? recipe->stems : 12;
	}
	if (recipe->fork_height >= 0.0f)
	{
		kind.fork_height = fminf(recipe->fork_height, 0.9f);
	}
	if (recipe->extra_stem_chance >= 0.0f)
	{
		kind.extra_stem_chance = fminf(recipe->extra_stem_chance, 1.0f);
	}
	if (recipe->gnarl >= 0.0f)
	{
		kind.gnarl = fminf(recipe->gnarl, 3.0f);
	}
	if (recipe->kinks >= 0.0f)
	{
		kind.kinks = fminf(recipe->kinks, 2.0f);
	}
	if (recipe->lean_degrees >= 0.0f)
	{
		kind.lean_degrees = fminf(recipe->lean_degrees, 88.0f);
	}
	if (recipe->lean_length >= 0.0f)
	{
		kind.lean_length = fminf(recipe->lean_length, 1.0f);
	}
	if (recipe->straighten >= 0.0f)
	{
		kind.straighten = fminf(recipe->straighten, 1.0f);
	}
	if (recipe->wind_side >= 0.0f)
	{
		kind.wind_side = fminf(recipe->wind_side, 1.0f);
	}
	if (recipe->moss >= 0.0f)
	{
		kind.moss = fminf(recipe->moss, 1.0f);
	}
	if (recipe->tips >= 0.0f)
	{
		kind.tips = fminf(recipe->tips, 2.0f);
	}
	if (recipe->autumn_colors[0][0] >= 0.0f)
	{
		memcpy(kind.autumn_colors, recipe->autumn_colors, sizeof(kind.autumn_colors));
	}
	if (recipe->autumn >= 0.0f)
	{
		kind.autumn = fminf(recipe->autumn, 1.0f);
	}
	if (recipe->falls >= 0)
	{
		kind.falls = recipe->falls != 0;
	}
	if (recipe->autumn_first_month > 0 && recipe->autumn_last_month > 0)
	{
		kind.autumn_first_month = recipe->autumn_first_month;
		kind.autumn_last_month = recipe->autumn_last_month;
	}
	if (recipe->leaves_month > 0)
	{
		kind.leaves_month = recipe->leaves_month;
	}
	if (recipe->snow_sag >= 0.0f)
	{
		kind.snow_sag = fminf(recipe->snow_sag, 0.5f);
	}
	plant.kind = &kind;
	plant.wind_side = kind.wind_side;
	plant.bark_meters = fmaxf(recipe->bark_meters, 0.05f);
	plant.twig_share = fminf(fmaxf(recipe->twig_share, 0.0f), 1.0f);
	plant.loose_leaves = fminf(fmaxf(recipe->loose_leaves, 0.0f), 100.0f);
	plant.fruit_shape = recipe->fruit_shape >= PLANT_FRUIT_SPHERE && recipe->fruit_shape <= PLANT_FRUIT_CARD ? recipe->fruit_shape
		: PLANT_FRUIT_SPHERE;
	plant.fruit_length = fminf(fmaxf(recipe->fruit_length, 0.002f), 2.0f);
	plant.fruit_width = recipe->fruit_width > 0.0f ? fminf(fmaxf(recipe->fruit_width, 0.002f), 2.0f) : 0.0f;
	plant.fruit_hang = recipe->fruit_hang >= 0 && recipe->fruit_hang <= PLANT_FRUIT_OUT ? (fruit_hang_t)recipe->fruit_hang : kind.fruit_hang;
	plant.fruit_zone = recipe->fruit_zone >= 0 && recipe->fruit_zone <= PLANT_FRUIT_ALL ? (fruit_zone_t)recipe->fruit_zone : kind.fruit_zone;
	plant.fruit_stalk = fminf(fmaxf(recipe->fruit_stalk, 0.0f), 2.0f);
	plant.cluster_minimum = recipe->cluster_minimum < 1 ? 1 : (recipe->cluster_minimum > 20 ? 20 : recipe->cluster_minimum);
	plant.cluster_maximum = recipe->cluster_maximum < plant.cluster_minimum ? plant.cluster_minimum
		: (recipe->cluster_maximum > 20 ? 20 : recipe->cluster_maximum);
	plant.fruit_per_branch = fminf(fmaxf(recipe->fruit_per_branch, 0.0f), 50.0f);
	plant.fruit_bunch = fminf(fmaxf(recipe->fruit_bunch, 0.0f), 1.0f);
	plant.fruit_tip = fminf(fmaxf(recipe->fruit_tip, 0.0f), 1.0f);
	if (recipe->leaves.count == 0 && recipe->twigs.count == 0 && kind.shape != SHAPE_MUSHROOM)
	{
		plant_error("A plant needs leaves (or twigs, for trees)\n");
		return false;
	}
	if ((kind.shape != SHAPE_MUSHROOM && !find_places(&atlas->atlas, &recipe->leaves, plant.leaves, &plant.leaf_count))
		|| !find_places(&atlas->atlas, &recipe->twigs, plant.twigs, &plant.twig_count)
		|| !find_places(&atlas->atlas, &recipe->heads, plant.heads, &plant.head_count)
		|| !find_under_places(&atlas->atlas, &recipe->heads, &plant)
		|| !find_places(&atlas->atlas, &recipe->fruit, plant.fruits, &plant.fruit_count)
		|| !find_month_cells(&atlas->atlas, recipe, mesh_out))
	{
		return false;
	}
	if (kind.shape != SHAPE_TREE)
	{
		// Twigs and fruit are for trees
		plant.twig_count = 0;
		plant.fruit_count = 0;
	}
	// A mushroom's texture: where it is in the atlas, and what of it is
	// there (the atlas keeps what is there of a picture: on alpha, maybe not
	// all of it)
	if (kind.shape == SHAPE_MUSHROOM)
	{
		char texture[PATH_LENGTH];
		cell_shape_t shape;
		int cell;

		plant.mushroom = recipe->mushroom;
		mushroom_texture_path(recipe, atlas->atlas.data_directory, texture, sizeof(texture));
		cell = atlas_find(&atlas->atlas, texture, &shape);
		if (cell < 0)
		{
			plant_error("'%s' is not in the %s atlas (prepare it again)\n", texture, atlas->atlas.name);
			return false;
		}
		plant.mushroom_place = atlas_cell_place(&atlas->atlas, shape, cell);
		mushroom_texture_crop(texture, plant.mushroom_crop);
	}
	random_state = recipe->seed * 2654435761u + 0x9e3779b9u;
	random_state = random_state != 0 ? random_state : 1u;
	plant.height = recipe->height > 0.0f ? recipe->height : kind.height_meters * random_between(0.85f, 1.15f);
	plant.leaf_size = recipe->leaf_size > 0.0f ? recipe->leaf_size : kind.leaf_size;
	plant.frond_angle = recipe->frond_angle >= 0.0f ? fminf(recipe->frond_angle, 89.0f) : 58.0f;
	plant.frond_droop = recipe->frond_droop >= 0.0f ? fminf(recipe->frond_droop, 3.0f) : 0.7f;
	plant.herb = recipe->herb;
	switch (kind.shape)
	{
		case SHAPE_TREE: grow_tree(&plant); break;
		case SHAPE_TUFT: if (recipe->herb.given) { grow_herb(&plant); } else { grow_tuft(&plant); } break;
		case SHAPE_ROSETTE: if (recipe->herb.given) { grow_herb(&plant); } else { grow_rosette(&plant); } break;
		case SHAPE_FERN: grow_fern(&plant); break;
		case SHAPE_FLOWER: if (recipe->herb.given) { grow_herb(&plant); } else { grow_flower(&plant); } break;
		case SHAPE_MUSHROOM: grow_mushroom(&plant); break;
	}
	// The winter's snow baked in (whole boughs), before the levels of detail
	// shuffle the foliage
	if (kind.shape == SHAPE_TREE && kind.branch_cards && recipe->snow_bake && plant.bough_count > 0 && atlas->atlas.pixels != NULL)
	{
		bake_snow(&plant, &atlas->atlas);
	}
	free(plant.boughs);
	plant.boughs = NULL;
	plant.bough_count = 0;
	plant.bough_capacity = 0;
	if (plant.mesh.vertex_count == 0)
	{
		plant_error("Nothing grew (no leaves or heads for this kind?)\n");
		free(plant.mesh.snow_vertices);
		free(plant.mesh.bark_levels);
		free(plant.mesh.foliage_ranks);
		free(plant.mesh.plant_vertices);
		for (int m = 0; m < MATERIAL_COUNT; m++)
		{
			free(plant.mesh.indices[m]);
		}
		free(plant.mesh.vertices);
		return false;
	}
	if (kind.shape == SHAPE_TREE)
	{
		shade_crown(&plant.mesh);
		finish_lods(&plant.mesh);
	}
	else if (plant.mesh.index_count[MATERIAL_FRUIT] > 0 && kind.shape != SHAPE_MUSHROOM)
	{
		// A small plant's flowers' cups: gone far away (the discs stay)
		plant.mesh.lods[MATERIAL_FRUIT] = true;
		plant.mesh.lod_index_count[MATERIAL_FRUIT][0] = (uint32_t)plant.mesh.index_count[MATERIAL_FRUIT];
		plant.mesh.lod_index_count[MATERIAL_FRUIT][1] = (uint32_t)plant.mesh.index_count[MATERIAL_FRUIT];
		plant.mesh.lod_index_count[MATERIAL_FRUIT][2] = 0;
	}
	free(plant.mesh.bark_levels);
	plant.mesh.bark_levels = NULL;
	free(plant.mesh.foliage_ranks);
	plant.mesh.foliage_ranks = NULL;
	// Each vertex's height on the plant (0 its foot .. 1 its top)
	{
		float bottom = plant.mesh.minimum.y;
		float span = fmaxf(plant.mesh.maximum.y - bottom, 1e-3f);

		for (size_t v = 0; v < plant.mesh.vertex_count; v++)
		{
			plant.mesh.plant_vertices[v].height = fminf(fmaxf((plant.mesh.vertices[v].position[1] - bottom) / span, 0.0f), 1.0f);
		}
	}
	mesh_out->plant_vertices = plant.mesh.plant_vertices;
	mesh_out->snow_vertices = plant.mesh.snow_vertices;
	// The materials through the seasons (colors linear, as the shaders take them)
	memset(mesh_out->plant_materials, 0, sizeof(mesh_out->plant_materials));
	mesh_out->plant_materials[MATERIAL_TRUNK].moss = kind.moss;
	mesh_out->plant_materials[MATERIAL_BARK].moss = kind.moss;
	for (int c = 0; c < 2; c++)
	{
		for (int k = 0; k < 3; k++)
		{
			mesh_out->plant_materials[MATERIAL_FOLIAGE].autumn_colors[c][k] = powf(fmaxf(kind.autumn_colors[c][k], 0.0f), 2.2f);
		}
	}
	mesh_out->plant_materials[MATERIAL_FOLIAGE].autumn = kind.autumn;
	mesh_out->plant_materials[MATERIAL_FOLIAGE].falls = kind.falls ? 1.0f : 0.0f;
	mesh_out->plant_materials[MATERIAL_FOLIAGE].tips = kind.tips;
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		// (the snow baked in: its own sag, none besides)
		mesh_out->plant_materials[m].sag = plant.mesh.snow_vertices != NULL ? 0.0f : kind.snow_sag;
		mesh_out->plant_materials[m].autumn_first_month = (float)(kind.autumn_first_month > 0 ? kind.autumn_first_month : 10);
		mesh_out->plant_materials[m].autumn_last_month = (float)(kind.autumn_last_month > 0 ? kind.autumn_last_month : 11);
		mesh_out->plant_materials[m].leaves_month = (float)(kind.leaves_month > 0 ? kind.leaves_month : 4);
	}
	// The wind in it (the same in all its materials)
	plant_recipe_wind(recipe, kind_index, plant.mesh.maximum.y - plant.mesh.minimum.y, plant.leaf_size, mesh_out->wind);
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		mesh_out->plant_materials[m].wind_given = 1.0f;
		mesh_out->plant_materials[m].sway = mesh_out->wind[0];
		mesh_out->plant_materials[m].flutter = mesh_out->wind[1];
		mesh_out->plant_materials[m].stiffness = mesh_out->wind[2];
	}
	mesh_out->vertices = plant.mesh.vertices;
	mesh_out->vertex_count = plant.mesh.vertex_count;
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		mesh_out->indices[m] = plant.mesh.indices[m];
		mesh_out->index_count[m] = plant.mesh.index_count[m];
	}
	memcpy(mesh_out->minimum, &plant.mesh.minimum, sizeof(float) * 3);
	memcpy(mesh_out->maximum, &plant.mesh.maximum, sizeof(float) * 3);
	mesh_out->height = plant.height;
	mesh_out->fruit_count = plant.fruits_grown;
	mesh_out->tree = kind.shape == SHAPE_TREE;
	mesh_out->still = kind.shape == SHAPE_MUSHROOM;
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		mesh_out->lods[m] = plant.mesh.lods[m];
		mesh_out->shuffled[m] = plant.mesh.shuffled[m];
		memcpy(mesh_out->lod_index_count[m], plant.mesh.lod_index_count[m], sizeof(mesh_out->lod_index_count[m]));
	}
	return true;
}

void plant_mesh_free(plant_mesh_t *mesh)
{
	free(mesh->vertices);
	free(mesh->plant_vertices);
	free(mesh->snow_vertices);
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		free(mesh->indices[m]);
	}
	memset(mesh, 0, sizeof(*mesh));
}

// A bark picture from outside the data directory: copied to its textures
// (the game finds them only there), unless one of that name is there
// already that is not the same. name_out: as the game names it
static bool bark_into_data(const char *path, const char *data_directory, char *name_out, size_t name_size)
{
	const char *slash = strrchr(path, '/');
	const char *base = slash != NULL ? slash + 1 : path;
	char target[PATH_LENGTH * 2];
	size_t size = 0;
	size_t there_size = 0;
	uint8_t *bytes = read_file(path, &size);
	uint8_t *there;
	bool ok = true;

	if (bytes == NULL)
	{
		plant_error("Cannot read the bark '%s'\n", path);
		return false;
	}
	snprintf(target, sizeof(target), "%s/textures/%s", data_directory, base);
	there = read_file(target, &there_size);
	if (there != NULL && (there_size != size || memcmp(there, bytes, size) != 0))
	{
		plant_error("The bark '%s' is not under '%s/', and '%s' (where it would go) is another picture: copy it there yourself\n", path,
			data_directory, target);
		ok = false;
	}
	else if (there == NULL)
	{
		FILE *file = fopen(target, "wb");

		ok = file != NULL && fwrite(bytes, 1, size, file) == size;
		ok = file != NULL && fclose(file) == 0 && ok;
		if (ok)
		{
			printf("Copied the bark to %s (the game finds textures there)\n", target);
		}
		else
		{
			plant_error("Cannot copy the bark to '%s'\n", target);
		}
	}
	free(there);
	free(bytes);
	snprintf(name_out, name_size, "textures/%s", base);
	return ok;
}

// Without a bark picture: a tree's brown, a small plant's green (the factor
// on a white texel: as sRGB)
static const float plain_bark[3] = { 0.30f, 0.21f, 0.14f };
static const float plain_stalk[3] = { 0.22f, 0.38f, 0.12f };

void plant_stalk_color(const plant_recipe_t *recipe, int kind, float color_out[3])
{
	if (recipe->stalk_color[0] >= 0.0f)
	{
		memcpy(color_out, recipe->stalk_color, sizeof(float) * 3);
		return;
	}
	memcpy(color_out, kind >= 0 && plant_kind_is_tree(kind) ? plain_bark : plain_stalk, sizeof(float) * 3);
}

bool plant_write(const char *path, const plant_mesh_t *mesh, const plant_recipe_t *recipe, const plant_atlas_t *atlas,
	const char *data_directory)
{
	static references_t references;
	int material_textures[MATERIAL_COUNT][3];
	mesh_t inner;

	memset(&references, 0, sizeof(references));
	if (path == NULL || strlen(path) < 6 || strcmp(path + strlen(path) - 5, ".pobj") != 0)
	{
		plant_error("Where it goes must end in .pobj ('%s')\n", path != NULL ? path : "");
		return false;
	}
	// What the object names: the bark as given (under data/), the atlas
	for (int k = 0; k < 3; k++)
	{
		char name[PATH_LENGTH];

		material_textures[MATERIAL_BARK][k] = -1;
		if (recipe->bark[k][0] != '\0')
		{
			if (!data_relative(recipe->bark[k], data_directory, name, sizeof(name))
				&& !bark_into_data(recipe->bark[k], data_directory, name, sizeof(name)))
			{
				return false;
			}
			material_textures[MATERIAL_BARK][k] = add_reference(&references, name);
		}
		material_textures[MATERIAL_TRUNK][k] = material_textures[MATERIAL_BARK][k];
	}
	// The atlas, no maps of its own
	material_textures[MATERIAL_FOLIAGE][0] = add_reference(&references, atlas->atlas.file_name);
	material_textures[MATERIAL_FOLIAGE][1] = -1;
	material_textures[MATERIAL_FOLIAGE][2] = -1;
	for (int k = 0; k < 3; k++)
	{
		material_textures[MATERIAL_FRUIT][k] = material_textures[MATERIAL_FOLIAGE][k];
		// (the snow on the boughs: the atlas, for the edges of the boughs under it)
		material_textures[MATERIAL_SNOW][k] = material_textures[MATERIAL_FOLIAGE][k];
	}
	memset(&inner, 0, sizeof(inner));
	inner.vertices = mesh->vertices;
	inner.vertex_count = mesh->vertex_count;
	for (int m = 0; m < MATERIAL_COUNT; m++)
	{
		inner.indices[m] = mesh->indices[m];
		inner.index_count[m] = mesh->index_count[m];
		inner.lods[m] = mesh->lods[m];
		inner.shuffled[m] = mesh->shuffled[m];
		memcpy(inner.lod_index_count[m], mesh->lod_index_count[m], sizeof(inner.lod_index_count[m]));
	}
	memcpy(&inner.minimum, mesh->minimum, sizeof(float) * 3);
	memcpy(&inner.maximum, mesh->maximum, sizeof(float) * 3);
	inner.plant_vertices = mesh->plant_vertices;
	inner.snow_vertices = mesh->snow_vertices;
	return write_pobj(path, &inner, &references, material_textures,
		recipe->stalk_color[0] >= 0.0f ? recipe->stalk_color : (mesh->tree ? plain_bark : plain_stalk), mesh->plant_materials,
		mesh->month_cells, mesh->month_cell_count, mesh->still);
}
