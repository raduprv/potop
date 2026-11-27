// Wreck: buildings that come apart. A building is pieces held together
// by links, on top of Box3D. (How it came to be as it is, step by step,
// with what was tried and found: side_projects/damage/DESIGN.md. That
// directory is also its test bench: a software renderer, test buildings,
// a viewer; nothing else in the engine uses this yet.)
//
// It depends on Box3D and the C library only: it draws nothing and reads
// no files. What uses it makes the Box3D world, steps it, gives the
// settings key by key, and looks at the pieces, links, groups and hits
// in wreck_model_t to draw them.
//
// The pieces and the links are one graph, made once. A broken link is
// only marked so. A group is an island of that graph, the pieces a walk
// over whole links reaches, and is ONE Box3D body with a shape a piece:
// static while a link holds it to the ground (so an untouched building
// costs nothing), moving once it has come loose. When links break the
// islands are worked out again and the bodies made anew for the groups
// that changed; a small loose group is rubble, left alone from then on.
//
// Every body of a building keeps the building's own frame: a piece's
// place in its body never changes, whichever body that is.
//
// What holds what up is worked out whenever the graph changed (the
// support pass), for a whole building at once: the pieces stay rigid,
// every link is a short stiff spring against the two pieces moving or
// turning apart where it is, gravity pulls on every piece, and where it
// all settles gives what each link bears. Each link is checked against
// what it bears pressed, pulled, sideways and as a cantilever, each
// piece against what crushes it (a crushed piece lets go of everything
// and is gone). An overloaded link does not snap at once: it wears away
// while it is overloaded, and the building stands a moment before it
// goes. A long piece bends: where its links and its weight bend it more
// than its material and section bear it is cut in two, straight across
// (so the graph also gains pieces).
//
// Sudden pushes break links by the work they do on them: what a link
// bears times how far it gives, against the push squared over twice the
// reduced mass of the two pieces it joins. They come three ways:
//   - a hit (what Box3D reports as faster than a speed): the hit piece
//     shares the impulse among its links, each passing some on;
//   - a blast: a wave that pushes every piece by what of it faces the
//     blast and how far it is, solved through all the links at once with
//     the same springs, in slices, links breaking in between; then the
//     gas that is left, if the blast is shut in;
//   - the shock of a fall: when a loose group hits something, each of
//     its pieces is pushed by its mass times the speed it lost, solved
//     through the group's links.
// Loose moving pieces are slowed by the air.
//
// One model at a time and one thread: the solver's scratch arrays are
// the file's own. Shapes and bodies carry the model's numbers in their
// user data (a piece's index plus one, a group's plus one).
//
// What lies on a building (a fallen log, a heap of rubble) counts only
// as the hit it gave, not as weight after.

#ifndef WRECK_H
#define WRECK_H

#include <stdbool.h>
#include <stdint.h>

#include "box3d/box3d.h"


#define WRECK_PIECES_MAX 1024
#define WRECK_LINKS_MAX 4096
#define WRECK_PROJECTILES_MAX 256
#define WRECK_RECENT_HITS_MAX 32
// A link's other end when it holds a piece to the ground
#define WRECK_GROUND -1
// The group of a piece that is no more (crushed): it has no body and is
// not drawn
#define WRECK_GONE -1

typedef enum
{
	WRECK_MATERIAL_WOOD,
	WRECK_MATERIAL_BRICK,
	WRECK_MATERIAL_GLASS,
	WRECK_MATERIAL_COUNT
} wreck_material_t;

typedef enum
{
	WRECK_LINK_NAILS,                 // wood to wood
	WRECK_LINK_MORTAR,                // brick to brick
	WRECK_LINK_FOUNDATION,            // anything to the ground
	WRECK_LINK_GLAZING,               // a pane to what frames it
	WRECK_LINK_NOTCH,                 // wood lying in wood with no nails: held by what presses on it
	WRECK_LINK_TYPE_COUNT
} wreck_link_type_t;

typedef enum
{
	WRECK_PIECE_BOX,
	WRECK_PIECE_LOG                   // a prism of 12 sides along its own x
} wreck_piece_shape_t;

typedef struct
{
	// What it bears standing still, in newtons (a kilogram weighs about
	// ten) and, as a cantilever, newton meters (a weight times how far to
	// the side it is held)
	float compression;                 // pressed together
	float pull;                        // pulled apart
	float slide;                       // held from the side
	float cantilever;
	// How far it gives before it lets go, meters: with what it bears,
	// the work a sudden push must do on it to break it (link_harm)
	float give;
	// What being pressed adds, for each newton it is pressed with (from
	// the last support pass): newtons it bears from the side (and that,
	// at joint_arm, turning)
	float grip;
} wreck_link_settings_t;

// The settings (wreck_settings_set gives them key by key)
typedef struct
{
	float densities[WRECK_MATERIAL_COUNT];                  // kilograms a cubic meter
	float crush[WRECK_MATERIAL_COUNT];                      // newtons a piece bears before it is crushed
	// A piece against hits, as a link is: what it shrugs off and what it
	// takes over that, in all, before it is no more (newton seconds)
	float piece_threshold[WRECK_MATERIAL_COUNT];
	float piece_hit_points[WRECK_MATERIAL_COUNT];
	// A long piece against bending: times its thickness squared times
	// its width (meters) this is the bending moment it bears standing
	// still (bend, giving newton meters) and from a hit (snap, giving
	// newton second meters)
	float bend[WRECK_MATERIAL_COUNT];
	float snap[WRECK_MATERIAL_COUNT];
	wreck_link_settings_t links[WRECK_LINK_TYPE_COUNT];
	float hop_falloff;                 // what a link passes on to the piece at its other end, of what it got
	float hit_speed;                   // meters a second: slower contacts are not hits
	int rubble_pieces;                 // a loose group of fewer pieces than this is rubble
	float breakthrough_speed_kept;     // of its speed before the hit, what a thing keeps when its hit broke links
	// A blast's wave: what a kilogram of explosive gives a square meter
	// that faces it at 1 m (newton seconds; by the kilograms to the
	// power 2/3), the power of the distance it falls off by, and how
	// dense the explosive is (kilograms a cubic meter: how big the bomb
	// is, nearer than which the wave grows no more)
	float blast_impulse_per_kilogram;
	float blast_falloff_power;
	float blast_charge_density;
	float link_elastic;                // of what a link can take of a sudden push, the part that springs back and costs it nothing
	float blast_inertia;               // for each kilogram of a piece, how many links' worth of a blast's push its own mass takes up
	float air_drag;                    // newtons the air pushes back with for each square meter that faces the way a piece goes, at 1 m/s (and by the speed squared)
	int blast_slices;                  // a blast's push comes on what stands in this many parts, links breaking between them
	// The gas left by a blast that is shut in: how hard it presses, as a
	// part of the blast's strength, when it has no way out at all; the
	// power of the shut-in part of the rays that it falls off by as ways
	// out open; and how much more a piece wholly beside a way out gets
	float blast_gas;
	float blast_gas_venting;
	float blast_gas_rush;
	float blast_shielding;             // of its push, what a piece loses when another is between its middle and the blast (0: nothing shields)
	float long_ratio;                  // a piece this many times longer than it is thick or wide is long: it can bend
	float cut_min_length;              // meters: no piece is cut into one shorter than this
	float cut_clearance;               // meters: a cut is at least this far past the edge of a piece linked to the one cut
	float collapse_seconds;            // how long a link loaded to just its limit lasts (twice the load: half as long)
	// How stiff a link is against the pieces turning apart, as a length
	// (meters): it resists turning as two springs this far apart would.
	// Longer: links are more like clamps and carry more as cantilevers.
	float joint_arm;
	float shape_gap;                   // meters a collision shape is smaller than its piece, all round
	float ball_radius;                 // the ball the viewer throws: meters
	float ball_density;                // kilograms a cubic meter
	// The blasts the viewer sets off: kilograms of explosive
	float blast_kilograms;
	float blast_big_kilograms;
} wreck_settings_t;

typedef struct
{
	wreck_piece_shape_t shape;
	wreck_material_t material;
	b3Vec3 half_extents;               // a box's
	float radius;                      // a log's
	float half_length;                 // a log's
	b3Transform local;                 // its place in the building's frame, which is its place in any body
	int group;
	uint32_t color;
	float mass;                        // kilograms
	bool crushed;                      // crushed by what lay on it or broken by hits: its links broke and it is gone
	float pressed;                     // newtons through it, from the last support pass (against its material's crush)
	float hit_points_left;
	int long_axis;                     // which of its own axes (0 x, 1 y, 2 z) it is long along; -1: it is not long
	float bent;                        // how many times what it bears it is bent, from the last support pass
	float gas_push_left;               // of the same blast's gas, what is still to push the piece, as a part of the blast's strength
	float blast_push_left;             // of the blast being worked out, the part of its push that came on after the piece came loose
} wreck_piece_t;

typedef struct
{
	int piece_a;
	int piece_b;                       // WRECK_GROUND: holds piece_a to the ground
	wreck_link_type_t type;
	b3Vec3 local_point;                // where it is, in the building's frame
	b3Vec3 local_normal;               // across the joint, from piece_b towards piece_a (up for the ground)
	float hit_points_left;             // what is left of it: 1 whole, 0 broken (sudden pushes and overloads use it up)
	bool broken;
	int broken_frame;
	// From the last support pass: how many times its limit it bears (over
	// 1: giving way), and how hard it is pressed, in newtons
	float overload;
	float static_compression;
	// The rest of what it bears, for looking at: pulled apart and
	// sideways (newtons), turning (newton meters)
	float static_pull;
	float static_sideways;
	float static_moment;
	// Which of its two pieces it holds up (pushes or pulls upwards), from
	// the last support pass; -1 when neither stood then
	int held_piece;
} wreck_link_t;

typedef struct
{
	bool used;
	b3BodyId body;
	bool anchored;                     // a whole link holds it to the ground: its body is static
	bool rubble;
	bool rebuilt;                      // made in the step being worked on
	bool reshaped;                     // a piece of it was cut: its body must be made anew even if it is still one island
	int piece_count;
	b3Vec3 linear_velocity_before_step;
	b3Vec3 angular_velocity_before_step;
} wreck_group_t;

// A thing thrown at the building (not a piece of it)
typedef struct
{
	b3BodyId body;
	float radius;
	b3Vec3 linear_velocity_before_step;
	b3Vec3 angular_velocity_before_step;
} wreck_projectile_t;

typedef struct
{
	int frame_index;
	b3Vec3 point;
	float impulse;                     // newton seconds
	bool broke_links;
} wreck_hit_t;

typedef struct
{
	b3WorldId world;
	wreck_settings_t settings;
	bool finished;

	wreck_piece_t pieces[WRECK_PIECES_MAX];
	int piece_count;
	wreck_link_t links[WRECK_LINKS_MAX];
	int link_count;
	wreck_group_t groups[WRECK_PIECES_MAX];
	wreck_projectile_t projectiles[WRECK_PROJECTILES_MAX];
	int projectile_count;

	// Every piece's links: piece_links[piece_link_start[p]] up to
	// piece_links[piece_link_start[p + 1]] (made by wreck_model_finish)
	int piece_link_start[WRECK_PIECES_MAX + 1];
	int piece_links[WRECK_LINKS_MAX * 2];

	bool group_dirty[WRECK_PIECES_MAX];
	bool any_group_dirty;

	// The support pass is due (the graph changed); the links it found
	// overloaded; and where it found every piece to settle (how far it
	// moved, how far it turned: six numbers a piece), which the next
	// pass starts from
	bool support_dirty;
	int overloaded_links[WRECK_LINKS_MAX];
	int overloaded_count;
	double support_solution[WRECK_PIECES_MAX * 6];
	int support_iterations;            // the last pass took this many

	// The cuts waiting to be made at the end of the step's work (a piece
	// is not cut while the graph is being walked), and all that were made
	int pending_cut_pieces[WRECK_PIECES_MAX];
	float pending_cut_positions[WRECK_PIECES_MAX];
	int pending_cut_count;
	int cut_count;

	wreck_hit_t recent_hits[WRECK_RECENT_HITS_MAX];
	int hit_count;                     // all there were; the last WRECK_RECENT_HITS_MAX are kept
	float largest_hit_impulse;
	int broken_link_count;
} wreck_model_t;

void wreck_settings_default(wreck_settings_t *settings);
// One key of the settings' ini (damage.ini in side_projects/damage is
// one, with every number explained): user is the wreck_settings_t.
// False for a section, a key or a value it does not know. The shape is
// that of an ini parser's callback (the engine's ini_parser.h, say), so
// it can be handed to one as it is; the model reads no files itself.
bool wreck_settings_set(void *user, const char *section, const char *key, const char *value);

// An empty model in a world. The world's hit event threshold should be
// the settings' hit_speed.
void wreck_model_create(wreck_model_t *model, b3WorldId world, const wreck_settings_t *settings);

// Building: pieces where they stand, then links between them, then
// wreck_model_finish, which finds the islands and makes their bodies.
// The adders return the piece's or link's number, -1 when full.
//
// A link's direction across its joint is worked out from where it is:
// up when it is under piece_a's middle and over piece_b's (piece_a lies
// on piece_b), down the other way round, else level and away from the
// link towards piece_a's middle. So for a joint at the side, piece_a is
// the piece that meets it squarely (the log that stops against the
// other's side, not the one running past).
int wreck_model_add_box(wreck_model_t *model, b3Vec3 position, b3Quat rotation, b3Vec3 half_extents, wreck_material_t material, uint32_t color);
int wreck_model_add_log(wreck_model_t *model, b3Vec3 position, b3Quat rotation, float radius, float half_length, wreck_material_t material, uint32_t color);
int wreck_model_add_link(wreck_model_t *model, int piece_a, int piece_b, b3Vec3 point, wreck_link_type_t type);
void wreck_model_finish(wreck_model_t *model);

// A body thrown at the building (a ball): the model minds its speed, so
// that it goes on through what it breaks instead of bouncing off it
void wreck_model_add_projectile(wreck_model_t *model, b3BodyId body, float radius);

// Round a step of the world: before it, and after it (the hits of the
// step, the links they break, the groups made anew, the support pass if
// the graph changed, the overloaded links giving way a little more)
void wreck_model_before_step(wreck_model_t *model);
void wreck_model_after_step(wreck_model_t *model, int frame_index, float step_seconds);

// A blast: damage to the links of every piece it reaches, then a push
// on what is loose. The strength is newton seconds on a square meter
// facing it.
void wreck_model_blast(wreck_model_t *model, b3Vec3 position, float strength, int frame_index);

// The piece a shape belongs to (a ray cast's hit, say), -1 for a shape
// that is no piece
int wreck_model_shape_piece(b3ShapeId shape);

// Where a piece is now, in the world (its body's place and its own place
// in that body)
b3Transform wreck_model_piece_transform(const wreck_model_t *model, int piece_index);

// A line about the model's state
void wreck_model_describe(const wreck_model_t *model, char *text, int text_size);
uint32_t wreck_model_checksum(const wreck_model_t *model);

#endif
