// The damage model: a building as pieces held together by links, on top
// of Box3D (see DESIGN.md).
//
// The pieces and the links are one graph, made once. A link has a
// threshold (what it shrugs off) and hit points (what it takes before it
// breaks); a broken link is only marked so. A group is an island of that
// graph, the pieces a walk over whole links reaches, and is ONE Box3D
// body with a shape a piece: static while a link holds it to the ground
// (so an untouched building costs nothing), moving once it has come
// loose. When links break the islands are worked out again and the
// bodies made anew for the groups that changed; a small loose group is
// rubble, left alone from then on.
//
// Every body of a building keeps the building's own frame: a piece's
// place in its body never changes, whichever body that is.
//
// Damage comes from hits (what Box3D reports as faster than a speed,
// with the impulse of that contact) and from blasts. A hit piece shares
// the impulse among its links; each link passes what it got, weakened, to
// the piece at its other end, which shares it again.
//
// What holds what up is worked out whenever the graph changed (the
// support pass), for a whole building at once: the pieces stay rigid,
// every link is a short stiff spring against the two pieces moving or
// turning apart where it is, gravity pulls on every piece, and where it
// all settles gives what each link bears. So the weight finds all the
// ways down there are, each as far as it is stiff, and pieces that hold
// one another (a frame) are seen to. Each link is checked against what
// it bears pressed, pulled, sideways and as a cantilever, each piece
// against what crushes it (a crushed piece lets go of everything and is
// gone). An overloaded
// link does not snap at once: it loses hit points while it is overloaded
// (so what a hit has hurt gives way sooner), and the building stands a
// moment before it goes.
//
// A piece itself takes hits (hit points of its material: a pane breaks,
// not only the putty round it) and, if it is long, bends: the forces
// its links put on it and its own weight give a bending moment along
// it, and where that is more than its material and thickness bear it
// is cut in two, straight across. A hit on a long piece bends it too,
// between the links nearest on either side. So the graph also gains
// pieces.
//
// Not here yet (step 6 of DESIGN.md): notched corners and sliding;
// plates, which bend two ways. What lies on a building (a fallen log, a
// heap of rubble) counts only as the hit it gave, not as weight after.

#ifndef DAMAGE_MODEL_H
#define DAMAGE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "box3d/box3d.h"

#include "soft_render.h"

#define DAMAGE_PIECES_MAX 1024
#define DAMAGE_LINKS_MAX 4096
#define DAMAGE_PROJECTILES_MAX 256
#define DAMAGE_RECENT_HITS_MAX 32
// A link's other end when it holds a piece to the ground
#define DAMAGE_GROUND -1
// The group of a piece that is no more (crushed): it has no body and is
// not drawn
#define DAMAGE_GONE -1

typedef enum
{
	DAMAGE_MATERIAL_WOOD,
	DAMAGE_MATERIAL_BRICK,
	DAMAGE_MATERIAL_GLASS,
	DAMAGE_MATERIAL_COUNT
} damage_material_t;

typedef enum
{
	DAMAGE_LINK_NAILS,                 // wood to wood
	DAMAGE_LINK_MORTAR,                // brick to brick
	DAMAGE_LINK_FOUNDATION,            // anything to the ground
	DAMAGE_LINK_GLAZING,               // a pane to what frames it
	DAMAGE_LINK_NOTCH,                 // wood lying in wood with no nails: held by what presses on it
	DAMAGE_LINK_TYPE_COUNT
} damage_link_type_t;

typedef enum
{
	DAMAGE_PIECE_BOX,
	DAMAGE_PIECE_LOG                   // a prism of 12 sides along its own x
} damage_piece_shape_t;

typedef struct
{
	float threshold;                   // newton seconds: a share of an impulse under this does nothing
	float hit_points;                  // newton seconds over the threshold it takes, in all, before it breaks
	// What it bears standing still, in newtons (a kilogram weighs about
	// ten) and, as a cantilever, newton meters (a weight times how far to
	// the side it is held)
	float compression;                 // pressed together
	float pull;                        // pulled apart
	float slide;                       // held from the side
	float cantilever;
	// What being pressed adds, for each newton it is pressed with (from
	// the last support pass): newtons it bears from the side (and that,
	// at joint_arm, turning), and newton seconds to its threshold
	float grip;
	float preload;
} damage_link_settings_t;

// The numbers of damage.ini (damage_settings_load)
typedef struct
{
	float densities[DAMAGE_MATERIAL_COUNT];                  // kilograms a cubic meter
	float crush[DAMAGE_MATERIAL_COUNT];                      // newtons a piece bears before it is crushed
	// A piece against hits, as a link is: what it shrugs off and what it
	// takes over that, in all, before it is no more (newton seconds)
	float piece_threshold[DAMAGE_MATERIAL_COUNT];
	float piece_hit_points[DAMAGE_MATERIAL_COUNT];
	// A long piece against bending: times its thickness squared times
	// its width (meters) this is the bending moment it bears standing
	// still (bend, giving newton meters) and from a hit (snap, giving
	// newton second meters)
	float bend[DAMAGE_MATERIAL_COUNT];
	float snap[DAMAGE_MATERIAL_COUNT];
	damage_link_settings_t links[DAMAGE_LINK_TYPE_COUNT];
	float hop_falloff;                 // what a link passes on to the piece at its other end, of what it got
	float hit_speed;                   // meters a second: slower contacts are not hits
	int rubble_pieces;                 // a loose group of fewer pieces than this is rubble
	float breakthrough_speed_kept;     // of its speed before the hit, what a thing keeps when its hit broke links
	float blast_radius;                // meters at full strength
	float blast_falloff;               // meters beyond that over which it fades to nothing
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
	// The blasts the viewer sets off: newton seconds a square meter of
	// what faces them
	float blast_strength;
	float blast_big_strength;
} damage_settings_t;

typedef struct
{
	damage_piece_shape_t shape;
	damage_material_t material;
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
} damage_piece_t;

typedef struct
{
	int piece_a;
	int piece_b;                       // DAMAGE_GROUND: holds piece_a to the ground
	damage_link_type_t type;
	b3Vec3 local_point;                // where it is, in the building's frame
	b3Vec3 local_normal;               // across the joint, from piece_b towards piece_a (up for the ground)
	float hit_points_left;
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
} damage_link_t;

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
} damage_group_t;

// A thing thrown at the building (not a piece of it)
typedef struct
{
	b3BodyId body;
	float radius;
	b3Vec3 linear_velocity_before_step;
	b3Vec3 angular_velocity_before_step;
} damage_projectile_t;

typedef struct
{
	int frame_index;
	b3Vec3 point;
	float impulse;                     // newton seconds
	bool broke_links;
} damage_hit_t;

typedef struct
{
	b3WorldId world;
	damage_settings_t settings;
	bool finished;

	damage_piece_t pieces[DAMAGE_PIECES_MAX];
	int piece_count;
	damage_link_t links[DAMAGE_LINKS_MAX];
	int link_count;
	damage_group_t groups[DAMAGE_PIECES_MAX];
	damage_projectile_t projectiles[DAMAGE_PROJECTILES_MAX];
	int projectile_count;

	// Every piece's links: piece_links[piece_link_start[p]] up to
	// piece_links[piece_link_start[p + 1]] (made by damage_model_finish)
	int piece_link_start[DAMAGE_PIECES_MAX + 1];
	int piece_links[DAMAGE_LINKS_MAX * 2];

	bool group_dirty[DAMAGE_PIECES_MAX];
	bool any_group_dirty;

	// The support pass is due (the graph changed); the links it found
	// overloaded; and where it found every piece to settle (how far it
	// moved, how far it turned: six numbers a piece), which the next
	// pass starts from
	bool support_dirty;
	int overloaded_links[DAMAGE_LINKS_MAX];
	int overloaded_count;
	double support_solution[DAMAGE_PIECES_MAX * 6];
	int support_iterations;            // the last pass took this many

	// The cuts waiting to be made at the end of the step's work (a piece
	// is not cut while the graph is being walked), and all that were made
	int pending_cut_pieces[DAMAGE_PIECES_MAX];
	float pending_cut_positions[DAMAGE_PIECES_MAX];
	int pending_cut_count;
	int cut_count;

	damage_hit_t recent_hits[DAMAGE_RECENT_HITS_MAX];
	int hit_count;                     // all there were; the last DAMAGE_RECENT_HITS_MAX are kept
	float largest_hit_impulse;
	int broken_link_count;
} damage_model_t;

void damage_settings_default(damage_settings_t *settings);
// Reads the ini over what the settings hold. False when there is no such
// file (the settings are then left as they were).
bool damage_settings_load(damage_settings_t *settings, const char *path);

// An empty model in a world. The world's hit event threshold should be
// the settings' hit_speed.
void damage_model_create(damage_model_t *model, b3WorldId world, const damage_settings_t *settings);

// Building: pieces where they stand, then links between them, then
// damage_model_finish, which finds the islands and makes their bodies.
// The adders return the piece's or link's number, -1 when full.
//
// A link's direction across its joint is worked out from where it is:
// up when it is under piece_a's middle and over piece_b's (piece_a lies
// on piece_b), down the other way round, else level and away from the
// link towards piece_a's middle. So for a joint at the side, piece_a is
// the piece that meets it squarely (the log that stops against the
// other's side, not the one running past).
int damage_model_add_box(damage_model_t *model, b3Vec3 position, b3Quat rotation, b3Vec3 half_extents, damage_material_t material, uint32_t color);
int damage_model_add_log(damage_model_t *model, b3Vec3 position, b3Quat rotation, float radius, float half_length, damage_material_t material, uint32_t color);
int damage_model_add_link(damage_model_t *model, int piece_a, int piece_b, b3Vec3 point, damage_link_type_t type);
void damage_model_finish(damage_model_t *model);

// A body thrown at the building (a ball): the model minds its speed, so
// that it goes on through what it breaks instead of bouncing off it
void damage_model_add_projectile(damage_model_t *model, b3BodyId body, float radius);

// Round a step of the world: before it, and after it (the hits of the
// step, the links they break, the groups made anew, the support pass if
// the graph changed, the overloaded links giving way a little more)
void damage_model_before_step(damage_model_t *model);
void damage_model_after_step(damage_model_t *model, int frame_index, float step_seconds);

// A blast: damage to the links of every piece it reaches, then a push
// on what is loose. The strength is newton seconds on a square meter
// facing it.
void damage_model_blast(damage_model_t *model, b3Vec3 position, float strength, int frame_index);

// The pieces (each its own object, numbered from first_object_id) and the
// projectiles; with draw_debug the links (green whole, yellow hurt,
// orange overloaded and giving way, red just broken) and the hits with
// their impulses, shown through the solids
void damage_model_draw_solids(const damage_model_t *model, soft_render_t *renderer, uint32_t first_object_id);
void damage_model_draw_marks(const damage_model_t *model, soft_render_t *renderer, int frame_index, bool draw_debug);

// The piece a shape belongs to (a ray cast's hit, say), -1 for a shape
// that is no piece
int damage_model_shape_piece(b3ShapeId shape);

// One piece looked into: its links drawn through the solids as lines to
// what they hold it to (green lightly loaded, yellow over half its
// limit, orange overloaded), and written out from the given corner what
// it is and what each of its links bears
void damage_model_draw_inspection(const damage_model_t *model, soft_render_t *renderer, int piece_index, int text_x, int text_y, int text_scale);

// A line about the model's state
void damage_model_describe(const damage_model_t *model, char *text, int text_size);
uint32_t damage_model_checksum(const damage_model_t *model);

#endif
