// Potop: the engine's interface for a game (and for an editor).
//
// A game holds a potop_t and talks to it through these functions: it asks
// (where is an actor, what is in a container), it tells (put an actor
// down, give an item, open a conversation), and it is told (an actor
// arrived, an item was picked, an answer was chosen). Things are named by
// numbers that stay (actor ids, item numbers, container ids), never by
// pointers into the engine; what the engine says about something comes
// back as a small struct, a copy.
//
// The engine reports, the game decides: who dies, what a quest is, what E
// does with what the eye is on.
//
// The engine's insides are open (potop_engine.h and the rest of engine/):
// whoever needs more than this reaches in, knowing it may change.
//
// Distances are meters; x and z are on the map (north is -z), y is up.

#ifndef POTOP_H
#define POTOP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct potop_s potop_t;

// The engine's instance, empty (potop_start makes it ready); NULL if out
// of memory
potop_t *potop_create(void);
void potop_destroy(potop_t *potop);

// A light (a lamp, a fire, a torch): local lights cast no shadows
typedef struct
{
	float color[3];                // 0..1, as the eye sees it
	float intensity;               // 1 lights a white wall a meter away about as the noon sun
	float radius_meters;           // nothing beyond this
	float flicker;                 // 0: steady .. 1: dances (fires)
	float core_meters;             // a glow that big where it is (a flame, a lantern's glass); 0: none
	bool switched;                 // lit at dusk and put out at dawn (lamps); else it burns on (fires)
} potop_light_t;

// ---------------------------------------------------------------------------
// A game's application: the window, the settings (data/settings.ini), a
// world and the player on foot, the HUD; then a frame at a time, the game
// in between:
//
//     potop_start(potop, &options);
//     while (potop_frame_begin(potop, &input, &seconds))
//     {
//         ...the game: what the input does, its rules...
//         potop_frame_end(potop);
//     }
//     potop_stop(potop);

#define POTOP_OPTIONS_SETTINGS_MAX 16

typedef struct
{
	const char *title;             // the window's (NULL: "Potop")
	int width;                     // the window's (0: 1280 x 720)
	int height;
	const char *map;               // a map to load (NULL: a new one from the settings)
	uint32_t seed;                 // a new map's (0: the settings', or a random one)
	const char *houses;            // not NULL: instead of a map, small flat ground with every object type of
	                               //   this category on it in rows ("": buildings, else fable)
	// Settings set over the files' ("render.day_length_minutes=24"), in turn
	const char *settings_set[POTOP_OPTIONS_SETTINGS_MAX];
	int settings_set_count;
	bool start_hour_set;           // the day starts at start_hour (0..24)
	float start_hour;
	// A test without the picture (sim_seconds > 0): fixed steps of
	// sim_step seconds (0: 0.05) as fast as they go, nothing drawn, the
	// keys not read; potop_frame_begin says false after sim_seconds
	float sim_seconds;
	float sim_step;
} potop_options_t;

// What the player did this frame, as a game needs it (the engine reads
// the keys; Tab, the conversation's answers 1..9 and Escape on an open
// window it takes care of itself)
typedef struct
{
	bool interact;                 // E
	bool inventory;                // I
	bool journal;                  // J
	bool map;                      // M
	bool reload;                   // R
	bool debug;                    // F3 (the game's debug window)
	bool throw_primary;            // G (a grenade)
	bool throw_secondary;          // H (another kind)
	bool place_primary;            // O (a mine)
	bool place_secondary;          // Shift+O (another kind)
	int select;                    // 1..9 as 0..8 (-1: none)
	bool trigger;                  // the left button held (not over the HUD)
	bool aim;                      // the right one
	bool escape;                   // Escape with nothing open to close
	bool pointer_free;             // Tab: the pointer is out for the HUD
	bool map_loaded;               // the console loaded a map (the game's numbers for its actors: found again)
} potop_input_t;

// False if it could not start (see the log); potop_stop all the same
bool potop_start(potop_t *potop, const potop_options_t *options);
void potop_stop(potop_t *potop);
// The frame's events and clocks; false once the player quits (the window
// closed, Ctrl+Q). seconds: since the last frame
bool potop_frame_begin(potop_t *potop, potop_input_t *input_out, float *seconds_out);
// The rest of it: the HUD, the walker, the world's steps, the weapon, the
// picture
void potop_frame_end(potop_t *potop);
// The game's seconds since the start (a test without the picture: its
// steps added up)
double potop_elapsed_seconds(const potop_t *potop);
// The time of day, 0..24
float potop_day_hour(const potop_t *potop);
// What physics stands in a square of the land (a test's map): cells x
// cells bytes, row by row from (x0, z0) along +x then +z, each 1 where a
// level line at height meters over the ground across the cell (along x or
// along z) hits a body, else 0
void potop_debug_obstacles(potop_t *potop, float x0, float z0, float cell_meters, int cells, float height, uint8_t *out);
// The navigation mesh's rectangles an actor type's searches go over (those
// built) reaching into the box: x, z, width, depth (meters), kind (0 the
// ground, 1 an object's top), its highest height; how many (at most most; 0: no such type)
int potop_debug_navmesh_rects(potop_t *potop, const char *type_name, float minimum_x, float minimum_z, float maximum_x, float maximum_z,
	float (*rects_out)[6], int most);
// The weapon in hand loaded again (from the pack)
void potop_player_reload(potop_t *potop);
// What the player carries that lights round (a torch, a lantern), at the
// hand; NULL puts it out (the console: torch)
void potop_player_set_light(potop_t *potop, const potop_light_t *light);
// An actor carries a light (a guard's lantern); NULL puts it out (it goes
// out when the actor dies)
void potop_actor_set_light(potop_t *potop, uint32_t id, const potop_light_t *light);
// A flash of light at a point, fading out over seconds (an explosion; a
// shot's flash comes from its muzzle effect, particles.ini)
void potop_flash(potop_t *potop, float x, float y, float z, const potop_light_t *light, float seconds);
// The player stays put (talking): the keys do not walk, nor jump
void potop_player_set_held(potop_t *potop, bool held);
// A console command of the game's own (` opens the console; help lists
// it, Tab completes it): function gets the rest of the line. False if
// there is no room for more
bool potop_console_add_command(potop_t *potop, const char *name, const char *usage, const char *help,
	void (*function)(void *user, const char *arguments), void *user);

// The settings window (graphics, the interface, the controls...); Escape
// closes it
void potop_settings_open(potop_t *potop, bool open);
bool potop_settings_is_open(const potop_t *potop);

// ---------------------------------------------------------------------------
// The world

// How far the map goes (it is square, from 0 to this on x and z)
float potop_world_extent_meters(const potop_t *potop);
// The ground's height at (x, z)
float potop_world_ground_height(const potop_t *potop, float x, float z);

// ---------------------------------------------------------------------------
// Maps (in the preference directory's maps folder: map_file.h). A name is
// without its kind (.active: to go on editing; .baked: for play, the ground
// and water as they are)

typedef enum
{
	POTOP_LOAD_FAILED = 0,         // nothing loaded (the world is as it was, or empty: see the log)
	POTOP_LOAD_DAMAGED,            // loaded, but parts of it are missing
	POTOP_LOAD_DONE
} potop_load_result_t;

// The map's file name found for a name (as given, active, baked); false if
// there is none
bool potop_map_find(const char *name, char *file_name_out, int file_name_size);
// Saves the world (and the player's pack) under the name, baked or not
// (a baked map saves baked only); false if it could not
bool potop_map_save(potop_t *potop, const char *name, bool baked);
// Loads a map (by its name or file name) at its size and kind; the camera
// as the map's (on foot it stays on foot)
potop_load_result_t potop_map_load(potop_t *potop, const char *name);
// The file name the map was last loaded or saved as (empty: none)
const char *potop_map_name(const potop_t *potop);
// A new map as the settings make it (their size; seed 0: a random one),
// the camera in its middle (on foot it stays on foot); false if out of
// memory
bool potop_map_new(potop_t *potop, uint32_t seed);

// ---------------------------------------------------------------------------
// Object types (data/objects.ini: trees, plants, rocks, houses)

typedef struct
{
	char name[64];
	char category[32];             // objects.ini's category ("flowers"; empty: none)
	char harvest_item[64];         // what picking it gives (empty: nothing)
} potop_object_type_info_t;

int potop_object_type_count(const potop_t *potop);
// The type named so (objects.ini's section; -1: none)
int potop_object_type_find(const potop_t *potop, const char *name);
bool potop_object_type_get(const potop_t *potop, int type, potop_object_type_info_t *info_out);
// What picking one of these gives (item: items.ini's name; NULL or empty:
// nothing): least..most each time, times before it is picked out (removes:
// the object goes then), back after regrow_hours of game time (0: never)
void potop_object_type_set_harvest(potop_t *potop, int type, const char *item, int least, int most, bool removes, int times,
	float regrow_hours);

// ---------------------------------------------------------------------------
// Objects on the map. Each has an id that stays as long as it is on the
// map (kept in the map's file; the editor shows it): a game names its
// door or its chest by it.

typedef struct
{
	uint32_t id;
	int type;                      // potop_object_type_get's
	char type_name[64];
	float x;
	float y;                       // where its origin is
	float z;
	float yaw_radians;
	float scale;
	int group;                     // the village (layout) it belongs to (0: none)
	uint32_t owner;                // whose it is: an actor's game number (0: no one's)
} potop_object_info_t;

// False if there is no such object (any more)
bool potop_object_get(potop_t *potop, uint32_t id, potop_object_info_t *info_out);
// Takes it off the map; false if there was none
bool potop_object_remove(potop_t *potop, uint32_t id);
// One object of a type put on the map; its id (0: not put down). awake: a
// loose thing (a type with a body) the physics takes at once (it settles,
// rolls down a slope); else it stands as put, asleep until something moves
// it
uint32_t potop_object_place(potop_t *potop, int type, float x, float z, float yaw_radians, float scale, bool awake);
// The game can do something with it (a door, a chest): the player's
// target sees it (POTOP_TARGET_OBJECT), and E on it is the game's. None
// is until the game says, again after each map load (what can be picked or
// lies on the ground, and the actors, are targets all the same); false if
// there is no such object
bool potop_object_set_interactive(potop_t *potop, uint32_t id, bool interactive);
// Whose it is: an actor's game number (potop_actor_set_game_id: it stays
// the same through saves; 0: no one's). Kept with the map; it goes with
// the thing as it is picked and grows again. The engine only says it
// (potop_target_t.owner): stealing is the game's
bool potop_object_set_owner(potop_t *potop, uint32_t id, uint32_t owner);
// A module's state (a building whose parts move, data/modules: a smithy's
// bellows "pump_down", "still"; a mill's "working"): it goes there (and on
// by itself, if the state says so); false if it is no module or has no
// such state
bool potop_object_set_state(potop_t *potop, uint32_t id, const char *state);
// The objects within radius of (x, z) (at most maximum); how many
int potop_objects_near(const potop_t *potop, float x, float z, float radius, uint32_t *ids_out, int maximum);
// A number that changes whenever the object list is sorted again (a map
// loaded, the editor's changes, a save): what was looked for among all the
// objects is to be looked for again then
uint32_t potop_objects_generation(const potop_t *potop);
// Those of them with a place (potop_spot_t) of that name: the smithies
// (smith_anvil), the dwellings (door), the inns (inn_door)
int potop_objects_with_spot(const potop_t *potop, float x, float z, float radius, const char *spot_name, uint32_t *ids_out, int maximum);

// Places on an object for the creatures and people, its model's (the house
// generator writes them: a hen house's yard, its feeders, the holes and
// the roosts inside; a smithy's anvil): what is done there, where, which
// way one faces, an area's size, and the spots it leads to straight with
// nothing in the way (the ways in and about it, baked: walk them with
// potop_actor_walk_straight, no search)
#define POTOP_SPOT_LINKS 4
typedef struct
{
	char name[32];                 // "yard", "peck", "hen_hole"...
	float x;                       // where (an area's middle)
	float y;
	float z;
	float yaw_radians;             // the way one faces there (0 down +Z, as the actors' yaw); an area's turn
	float half_width;              // an area: half its size along its own x and z (0, 0: a point)
	float half_depth;
	int links[POTOP_SPOT_LINKS];   // the spots it leads to (indices; -1: none)
} potop_spot_t;

// How many places it has (0: none, or no such object)
int potop_object_spot_count(potop_t *potop, uint32_t id);
// One of them as it stands in the world; false if there is no such one
bool potop_object_spot_get(potop_t *potop, uint32_t id, int index, potop_spot_t *spot_out);
// The first of its places of that name from index on (-1: none)
int potop_object_spot_find(potop_t *potop, uint32_t id, const char *name, int from);

// ---------------------------------------------------------------------------
// The camera (the player's eye on foot)

// How the player goes on foot (what the animals hear and see of it)
typedef enum
{
	POTOP_GAIT_STILL = 0,
	POTOP_GAIT_CRAWL,              // Ctrl on dry ground: low, slow, hardly heard or seen
	POTOP_GAIT_WALK,
	POTOP_GAIT_RUN                 // Shift
} potop_gait_t;

typedef struct
{
	bool first_person;             // on foot (else the orbit camera of the editor)
	float eye_x;                   // where the eye is
	float eye_y;
	float eye_z;
	float heading_degrees;         // which way it looks: 0 north (-z), 90 east (+x)
	float walker_x;                // on foot: where the feet are, which way they face
	float walker_z;
	float walker_yaw_degrees;
	potop_gait_t gait;             // on foot: how it goes now
	bool crawling;                 // low (moving or not)
	// On foot, for the debug window: the feet's height, the ground's there
	// (what they stand on) and the terrain's; what pushed the walker in
	// its last move (physics: planes that pushed it, those let be, how far,
	// the deepest one's normal, the height over the feet it touched, how deep)
	float walker_feet;
	float walker_ground;
	float walker_terrain;
	bool walker_on_ground;
	int push_planes_kept;
	int push_planes_skipped;
	float push_x;
	float push_z;
	float push_normal[3];
	float push_height;
	float push_depth;
	// Whether the objects hidden in the last frame's depth (terrain, houses,
	// the rest: hiz.h) are left out (console: objects occlusion on|off)
	bool occlusion_culling;
} potop_camera_info_t;

void potop_camera_get(const potop_t *potop, potop_camera_info_t *info_out);

// ---------------------------------------------------------------------------
// Actors (animals, people). An actor's id stays while it lives in this
// map; the game's own number for it (game_id) stays with the map's file.

typedef struct
{
	uint32_t id;
	uint32_t game_id;              // 0: none
	char type_name[32];            // actors.ini's ("bear")
	char name[32];                 // shown over its head (empty: nothing)
	bool hostile;                  // the name in red
	bool dead;
	bool flies;                    // its type flies
	bool moving;                   // on its way somewhere
	bool wandering;                // on a walk with no goal (potop_actor_wander), not told to stand
	float x;
	float y;                       // the ground (or the air) under it
	float z;
	float top_y;                   // the top of its model, where it is now
	float yaw_radians;             // which way it faces: 0 = +z
} potop_actor_info_t;

typedef enum
{
	POTOP_ACTOR_ARRIVED = 1,       // got where it was sent
	POTOP_ACTOR_GOT_NEAR,          // the goal cannot be reached: it stopped as near as it could
	POTOP_ACTOR_NO_PATH            // it could not move towards the goal at all
} potop_actor_event_type_t;

typedef struct
{
	potop_actor_event_type_t type;
	uint32_t id;
	uint32_t game_id;
	float x;                       // where it stands now
	float z;
} potop_actor_event_t;

// An actor of a type of actors.ini put down; 0 if there is no such type
// (or it has no model)
uint32_t potop_actor_spawn(potop_t *potop, const char *type_name, float x, float z, float yaw_radians);
// False if there is no such actor (any more)
bool potop_actor_get(const potop_t *potop, uint32_t id, potop_actor_info_t *info_out);
// The actor with the game's number (0: none)
uint32_t potop_actor_find_by_game_id(const potop_t *potop, uint32_t game_id);
// The actors within radius of (x, z), the nearest first (at most
// maximum); how many
int potop_actors_near(const potop_t *potop, float x, float z, float radius, uint32_t *ids_out, int maximum);
void potop_actor_set_name(potop_t *potop, uint32_t id, const char *name, bool hostile);
void potop_actor_set_game_id(potop_t *potop, uint32_t id, uint32_t game_id);
// Sent somewhere (it finds its way); false if it cannot set out
bool potop_actor_go_to(potop_t *potop, uint32_t id, float x, float z);
// Walks through the points in turn, straight from one to the next, with no
// search for a way (a short way about a yard, a building's baked ways:
// potop_spot_t): what stands in its way stops it (it slides along it if it
// can), and it says so as if it found no way (POTOP_ACTOR_NO_PATH); false
// if it cannot set out. points: x, z pairs
bool potop_actor_walk_straight(potop_t *potop, uint32_t id, const float (*points)[2], int count);
// Wanders (a walk with no goal, a bored cow's): on the way heading_radians
// says (0 = +z) at share of its walk (0: stands; below 0: backs) till told
// otherwise or given a walk; it turns to a new heading at its own turning
// speed. A body or another just ahead stops it where it is
bool potop_actor_wander(potop_t *potop, uint32_t id, float heading_radians, float share);
// Whether its way heading so is free for meters past its nose: no body
// (the physics' shapes, along its middle and both sides), no other there
// now or in half a second
bool potop_actor_heading_free(potop_t *potop, uint32_t id, float heading_radians, float meters);
// The same, ending on the last point within a few centimeters (a worker
// onto his place) and turned at once to face_yaw_radians there; a point a
// step or two away: no walk, turned on the spot onto it, sliding as it
// turns. Done when it stands there facing so
bool potop_actor_walk_straight_exact(potop_t *potop, uint32_t id, const float (*points)[2], int count, float face_yaw_radians);
// Takes it away (gone into its hen house for the night); false if there
// was none
bool potop_actor_remove(potop_t *potop, uint32_t id);
// Whether the actor could stand at (x, z) without swimming: room for it,
// dry or shallow enough to wade (not in deep water, not inside a rock).
// Cheap: worth asking before potop_actor_go_to, whose search for a way to
// somewhere it cannot be costs the most
bool potop_actor_can_stand(potop_t *potop, uint32_t id, float x, float z);
// A way from (from_x, from_z) to (to_x, to_z) for an actor of that type
// (actors.ini's) as it would find it, with no actor: its points (x, z, the
// start left out) into points_out, at most maximum; how many (0: no way,
// or no such type). complete_out (may be NULL): it reaches the goal (not
// only as near as it gets, nor cut short by maximum). A game keeps it for
// someone it moves along by itself while there is no actor for him
int potop_find_path(potop_t *potop, const char *actor_type, float from_x, float from_z, float to_x, float to_z, float (*points_out)[2],
	int maximum, bool *complete_out);
void potop_actor_stop(potop_t *potop, uint32_t id);
// Goes at its type's run speed (actors.ini run_meters_per_second), or walks
// again
void potop_actor_set_running(potop_t *potop, uint32_t id, bool running);
// One of its model's actions (sleeping, attacking...); false if it has
// none of that name. Walking, it walks again
bool potop_actor_play(potop_t *potop, uint32_t id, const char *action);

// ---------------------------------------------------------------------------
// Senses: what the actors see and hear (the game's animals and guards go
// by these; what they do about it is the game's)

typedef enum
{
	POTOP_NOISE_STEP = 0,          // the player's feet
	POTOP_NOISE_SHOT,
	POTOP_NOISE_EXPLOSION,
	POTOP_NOISE_OTHER              // the game's own (potop_make_noise)
} potop_noise_kind_t;

typedef struct
{
	potop_noise_kind_t kind;
	float x;
	float y;
	float z;
	float radius_meters;           // heard within this
	uint32_t source;               // an actor's id (0: the player, or no one)
	double time_seconds;           // potop_world_time's when it was made
} potop_noise_t;

// How loud the player's steps are (heard within, meters) and how visible
// it is (1: standing in the open), crawling, walking, running
typedef struct
{
	float step_meters[3];          // crawl, walk, run: 2, 10, 25
	float visibility[3];           // 0.4, 1, 1.2
} potop_stealth_t;

// The world's clock (seconds): what the noises are stamped with
double potop_world_time(const potop_t *potop);
// How well the actor sees a point (0: not .. 1: right in front, in full
// light): its sight (actors.ini sight_meters, sight_degrees), the dark
// there, what stands between; visibility: how visible what is there is (1:
// someone standing)
float potop_actor_sight(potop_t *potop, uint32_t id, float x, float y, float z, float visibility);
// The same for the player (where it is, how it goes: crawling it is less
// seen); 0 when not on foot
float potop_actor_sees_player(potop_t *potop, uint32_t id);
// The noises made after since_seconds that carry to (x, y, z), the newest
// first (at most maximum); how many
int potop_noises_heard(const potop_t *potop, float x, float y, float z, double since_seconds, potop_noise_t *noises_out, int maximum);
// A noise of the game's own (a door slamming, a shout), heard within radius
void potop_make_noise(potop_t *potop, float x, float y, float z, float radius_meters, uint32_t source);
void potop_player_set_stealth(potop_t *potop, const potop_stealth_t *stealth);
void potop_player_get_stealth(const potop_t *potop, potop_stealth_t *stealth_out);
// Turns to look at a place while it stands, until told to stop
void potop_actor_look_at(potop_t *potop, uint32_t id, float x, float z);
void potop_actor_stop_looking(potop_t *potop, uint32_t id);
// Its death (the game decides it): the death animation, it falls
bool potop_actor_kill(potop_t *potop, uint32_t id);
// Its pain animation (the game decides when)
bool potop_actor_hurt(potop_t *potop, uint32_t id);
bool potop_actor_poll_event(potop_t *potop, potop_actor_event_t *event_out);

// ---------------------------------------------------------------------------
// Items (data/items.ini) and containers. An item is its number in
// items.ini; a container (a pack, a chest, a dead animal's) is an id.

typedef enum
{
	POTOP_ITEM_MISC = 0,
	POTOP_ITEM_WEAPON,
	POTOP_ITEM_AMMO,
	POTOP_ITEM_FOOD,
	POTOP_ITEM_MATERIAL,
	POTOP_ITEM_CLOTHING,
	POTOP_ITEM_TOOL,
	POTOP_ITEM_QUEST,
	POTOP_ITEM_MONEY
} potop_item_category_t;

typedef struct
{
	char name[32];                 // the section's
	char shown_name[48];
	char description[160];
	potop_item_category_t category;
	float weight_kilograms;        // one
	float value;                   // one
	int stack;
	bool usable;
	int slot;                      // the body's slot it is worn in (-1: none)
	int weapon;                    // weapons.ini's (-1: not a weapon)
	int ammo;                      // ammo.ini's (-1: not ammo)
} potop_item_info_t;

typedef enum
{
	POTOP_INVENTORY_ADDED = 1,
	POTOP_INVENTORY_REMOVED,
	POTOP_INVENTORY_EQUIPPED,
	POTOP_INVENTORY_UNEQUIPPED,
	POTOP_INVENTORY_USED           // the player used it (what that does is the game's; it is still there)
} potop_inventory_event_type_t;

typedef struct
{
	potop_inventory_event_type_t type;
	uint32_t container;
	int item;
	int count;
} potop_inventory_event_t;

typedef struct
{
	char name[48];                 // shown over its list
	int entry_count;               // its rows
	float weight_kilograms;        // what is in it
	float max_weight_kilograms;    // 0: no limit
	bool locked;                   // (what that keeps out is the game's)
	int key_item;                  // the item that opens it (-1: none)
} potop_inventory_info_t;

// The item named so in items.ini (-1: none)
int potop_item_find(const potop_t *potop, const char *name);
bool potop_item_get(const potop_t *potop, int item, potop_item_info_t *info_out);
// The item that is weapons.ini's weapon (-1: none)
int potop_item_of_weapon(const potop_t *potop, int weapon);

// A container (max_weight 0: no limit); 0 if there is no room for more
uint32_t potop_inventory_create(potop_t *potop, const char *name, float max_weight_kilograms);
void potop_inventory_destroy(potop_t *potop, uint32_t container);
// Locked or not, and the item that opens it (-1: none); what a lock keeps
// out is the game's (the loot window opens all the same)
void potop_inventory_set_lock(potop_t *potop, uint32_t container, bool locked, int key_item);
// The player's pack (the game makes it and says which it is)
uint32_t potop_player_inventory(const potop_t *potop);
void potop_set_player_inventory(potop_t *potop, uint32_t container);
// How many went in (the weight limit), came out
int potop_inventory_add(potop_t *potop, uint32_t container, int item, int count);
int potop_inventory_remove(potop_t *potop, uint32_t container, int item, int count);
// How many there are (worn_too: worn ones too)
int potop_inventory_count(const potop_t *potop, uint32_t container, int item, bool worn_too);
// The item worn in its slot (it must be in the container)
bool potop_inventory_equip(potop_t *potop, uint32_t container, int item);
bool potop_inventory_get(const potop_t *potop, uint32_t container, potop_inventory_info_t *info_out);
// A row of it: the item, how many
bool potop_inventory_entry(const potop_t *potop, uint32_t container, int row, int *item_out, int *count_out);
bool potop_inventory_poll_event(potop_t *potop, potop_inventory_event_t *event_out);

// ---------------------------------------------------------------------------
// The journal (the quest log). The game keeps its quests; the journal
// shows what the game tells it. Quests and objectives are named by the
// game's ids (strings, under 32 bytes).

typedef enum
{
	POTOP_QUEST_ACTIVE = 0,
	POTOP_QUEST_INACTIVE,          // put aside (it can go on later)
	POTOP_QUEST_COMPLETED,
	POTOP_QUEST_FAILED
} potop_quest_state_t;

typedef enum
{
	POTOP_OBJECTIVE_OPEN = 0,
	POTOP_OBJECTIVE_DONE,
	POTOP_OBJECTIVE_FAILED
} potop_objective_state_t;

typedef struct
{
	char text[160];
	potop_objective_state_t state;
	char progress[24];             // "3/10" (empty: none)
	bool has_place;
	float x;
	float z;
} potop_objective_info_t;

// Named for the first time: added. NULL text: as it was
void potop_journal_set_quest(potop_t *potop, const char *quest, const char *title, const char *description, potop_quest_state_t state);
void potop_journal_set_objective(potop_t *potop, const char *quest, const char *objective, const char *text, potop_objective_state_t state,
	const char *progress);
// Where it is on the map (the compass and the map show it while tracked)
void potop_journal_set_place(potop_t *potop, const char *quest, const char *objective, float x, float z);
void potop_journal_clear_place(potop_t *potop, const char *quest, const char *objective);
// False if too many are tracked already
bool potop_journal_track(potop_t *potop, const char *quest, bool tracked);
// That objective in focus (NULL: the quest's first open one with a place)
bool potop_journal_focus(potop_t *potop, const char *quest, const char *objective);
bool potop_journal_has_quest(const potop_t *potop, const char *quest);
bool potop_journal_get_objective(const potop_t *potop, const char *quest, const char *objective, potop_objective_info_t *info_out);

// ---------------------------------------------------------------------------
// Conversations: the engine shows them, the game holds them. Answers are
// named by the game's ids (not 0: 0 is Continue, a line without answers).

typedef enum
{
	POTOP_DIALOG_CHOSEN = 1,       // the player picked an answer
	POTOP_DIALOG_CLOSED            // the player walked off or pressed Escape
} potop_dialog_event_type_t;

typedef struct
{
	potop_dialog_event_type_t type;
	uint32_t actor;
	uint32_t choice;               // the answer's id
} potop_dialog_event_t;

void potop_dialog_open(potop_t *potop, uint32_t actor, const char *speaker);
// The line said (the answers cleared)
void potop_dialog_say(potop_t *potop, const char *text);
// An answer (an id already there: its text changes)
void potop_dialog_add_choice(potop_t *potop, uint32_t id, const char *text);
void potop_dialog_remove_choice(potop_t *potop, uint32_t id);
void potop_dialog_close(potop_t *potop);
bool potop_dialog_is_open(const potop_t *potop);
bool potop_dialog_poll_event(potop_t *potop, potop_dialog_event_t *event_out);

// ---------------------------------------------------------------------------
// The HUD (what the player sees on foot)

typedef enum
{
	POTOP_NOTIFY_INFO = 0,
	POTOP_NOTIFY_QUEST,            // gold: a quest's news
	POTOP_NOTIFY_WARNING           // red
} potop_notify_style_t;

typedef enum
{
	POTOP_HUD_INVENTORY = 0,
	POTOP_HUD_JOURNAL,
	POTOP_HUD_MAP
} potop_hud_window_t;

// What the game says, for that many seconds, under the compass
void potop_notify(potop_t *potop, const char *text, float seconds, potop_notify_style_t style);
bool potop_hud_is_open(const potop_t *potop, potop_hud_window_t window);
void potop_hud_set_open(potop_t *potop, potop_hud_window_t window, bool open);
// What E would do, under the crosshair ("[E] Talk to ..."; NULL or empty:
// nothing); the game says it each frame
void potop_hud_set_prompt(potop_t *potop, const char *text);
// The game's debug window, in the top left corner: shown or not, and its
// lines (the game gives them again each frame it is shown: cleared, then
// added one by one, at most 16)
void potop_debug_show(potop_t *potop, bool shown);
bool potop_debug_shown(const potop_t *potop);
void potop_debug_clear(potop_t *potop);
void potop_debug_line(potop_t *potop, const char *text);
// The loot window on a container, next to the player's pack (0: closed)
void potop_hud_open_loot(potop_t *potop, uint32_t container);
uint32_t potop_hud_loot(const potop_t *potop);

// ---------------------------------------------------------------------------
// The player on foot

typedef enum
{
	POTOP_TARGET_NONE = 0,
	POTOP_TARGET_HARVEST,          // something to pick from (a plant, a tree): item, any_left
	POTOP_TARGET_GROUND_ITEM,      // something lying there: container (what it holds)
	POTOP_TARGET_ACTOR,            // actor_id, actor_dead
	POTOP_TARGET_OBJECT,           // something on the map the game can do something with (a chest, a mine): object_id
	POTOP_TARGET_DOOR              // a building's door or gate: object_id (the building's), door, door_open
} potop_target_kind_t;

typedef struct
{
	potop_target_kind_t kind;
	float distance;
	int item;                      // HARVEST: what it gives
	bool any_left;                 // HARVEST: something to pick now
	uint32_t container;            // GROUND_ITEM
	uint32_t object_id;            // HARVEST, GROUND_ITEM, OBJECT: the object's id
	uint32_t actor_id;             // ACTOR
	bool actor_dead;
	uint32_t owner;                // HARVEST, GROUND_ITEM, OBJECT, DOOR: whose it is (an actor's game number; 0: no one's)
	int door;                      // DOOR: which of the building's doors (for potop_door_set)
	bool door_open;                // DOOR: open (or opening)
} potop_target_t;

typedef enum
{
	POTOP_PICK_NOTHING = 0,        // not something to pick
	POTOP_PICK_PICKED,             // picked (some at least: the pack may be full)
	POTOP_PICK_PICKED_OUT,         // nothing left on it (yet)
	POTOP_PICK_TOO_HEAVY           // nothing fits in the pack
} potop_pick_result_t;

typedef enum
{
	POTOP_HIT_GROUND = 0,
	POTOP_HIT_OBJECT,
	POTOP_HIT_WATER,
	POTOP_HIT_ACTOR,
	POTOP_HIT_PLAYER               // the player (caught in an explosion)
} potop_hit_kind_t;

typedef enum
{
	POTOP_PART_BODY = 0,
	POTOP_PART_HEAD,
	POTOP_PART_LIMB
} potop_body_part_t;

// The kinds of damage a hit carries (weapons.ini's ammo and explosives.ini
// say which: damage_type = ballistic, explosive, fire, cold, poison, magic)
typedef enum
{
	POTOP_DAMAGE_BALLISTIC = 0,
	POTOP_DAMAGE_EXPLOSIVE,
	POTOP_DAMAGE_FIRE,
	POTOP_DAMAGE_COLD,
	POTOP_DAMAGE_POISON,
	POTOP_DAMAGE_MAGIC
} potop_damage_type_t;

// Where a projectile hit (the game decides what it does)
typedef struct
{
	potop_hit_kind_t kind;
	float x;
	float y;
	float z;
	uint32_t actor_id;             // POTOP_HIT_ACTOR
	uint32_t object_id;            // POTOP_HIT_OBJECT (potop_object_get; a grenade or a mine: potop_explosive_set_off)
	potop_body_part_t part;        // where on it
	int bone;
	float damage;                  // as the energy left goes
	potop_damage_type_t damage_type; // what kind (what it does is the game's)
	int ammo;                      // ammo.ini's it was (potop_ammo_find; -1: none, a blast's)
	int owner;                     // who fired it (-1: the player)
} potop_hit_t;

// What the eye is on now (looked for again by each call)
void potop_player_find_target(potop_t *potop, potop_target_t *target_out);

// Doors and gates: a building's (a module's parts made doors: a farm's
// byre, pigsty, hen house, stana), each closed or open on its own; by the
// building's object id and which door (potop_target_t.door, or
// potop_door_find). The player's E and any actor (a farmer) the same way.
// Set: it swings there; false if there is no such door
bool potop_door_set(potop_t *potop, uint32_t object_id, int door, bool open);
// 1 open (or opening), 0 closed (or closing), -1 no such door
int potop_door_get(potop_t *potop, uint32_t object_id, int door);
// A building's door by its name ("door1", "door2", ... in the order the
// house generator made them); -1 if none
int potop_door_find(potop_t *potop, uint32_t object_id, const char *name);

typedef struct
{
	float hinge[3];                // its hinges' foot (world, meters)
	float along[3];                // from the hinges across the leaf, shut (horizontal, unit)
	float swing[3];                // the side it swings open to (horizontal, unit)
	float width;                   // the leaf's (meters)
	float height;
	bool open;                     // open or opening
	bool moving;                   // swinging now
	bool passage;                  // a way through when open (not a door drawn on a wall)
} potop_door_info_t;

// Where a building's door is and how it swings (for someone to stand by
// it); false if there is no such door
bool potop_door_info(potop_t *potop, uint32_t object_id, int door, potop_door_info_t *info_out);
// Picks from the target found last (POTOP_TARGET_HARVEST) into the pack;
// the item and how many went in
potop_pick_result_t potop_player_pick(potop_t *potop, int *item_out, int *count_out);
// Picks up what lies there (the target found last); false if some of it
// is left (too heavy) or there was nothing
bool potop_player_pick_up(potop_t *potop);
// The weapons.ini weapon in hand (worn if it is an item); false if it is
// an item the pack does not have
bool potop_player_select_weapon(potop_t *potop, int weapon);
// weapons.ini's weapon, ammo.ini's ammo named so (-1: none)
int potop_weapon_find(const potop_t *potop, const char *name);
int potop_ammo_find(const potop_t *potop, const char *name);
// A particles.ini effect played once at a point (an impact, a spell's);
// false if there is no such effect
bool potop_effect_play(potop_t *potop, const char *effect, float x, float y, float z);
// The weapon's buttons this frame
void potop_player_set_buttons(potop_t *potop, bool trigger, bool aim);
// Throws a grenade from the pack (an item that is an explosive,
// explosives.ini) from the hand along the eye; false if the pack has none
bool potop_player_throw(potop_t *potop, int item);
// An explosion of a kind of explosives.ini at a point (a mine the game
// set off, a barrel): the push, the crater, the flash, its effects; who was
// caught in it comes with the hits
bool potop_explode(potop_t *potop, float x, float y, float z, const char *explosive);
// Puts down one of an item that is an explosive (a mine: explosives.ini)
// where the eye looks on the ground within reach, else at the feet; its
// object's id (0: none in the pack). Its fuse is armed after arm_seconds;
// E on it is the game's (potop_object_set_interactive, then
// potop_explosive_get and potop_explosive_disarm)
uint32_t potop_player_place(potop_t *potop, int item);

typedef struct
{
	char explosive[32];            // explosives.ini's
	bool armed;                    // a mine's fuse is (a grenade on a timer: never)
	bool placed;                   // put down (a mine), not thrown
	bool buried;
	int owner;                     // -1: the player's
	int item;                      // items.ini's item that it is (-1: none)
} potop_explosive_info_t;

// The grenade or mine that is this object; false if it is none
bool potop_explosive_get(const potop_t *potop, uint32_t object_id, potop_explosive_info_t *info_out);
// The grenade or mine lying or flying as this object (a shot hit it) goes
// off now; false if the object is none
bool potop_explosive_set_off(potop_t *potop, uint32_t object_id);
// It taken off the map unexploded (disarmed: whether that works, and what
// the player gets for it, is the game's); the items.ini item it is (-1:
// the object is none, or no item is it)
int potop_explosive_disarm(potop_t *potop, uint32_t object_id);
// The hits since the last call (at most maximum); how many
int potop_take_hits(potop_t *potop, potop_hit_t *hits_out, int maximum);

// ---------------------------------------------------------------------------
// Light: how lit a place is (stealth, an animal seeking the dark). How
// much sky it sees (under the trees, in a gorge, little) times how much
// day it is, and the lights near it (fires, lamps, torches, flashes); no
// rays, nothing through walls is looked for

typedef struct
{
	float sky_open;                // how much of the sky the ground there sees, 0..1
	float daylight;                // how much day it is now, 0..1 (night: the moon's, a little)
	float local;                   // the lights near it, as the noon sun would light it (1)
	float total;                   // sky_open * daylight + local, at most 1
} potop_light_info_t;

// How much of the sky the ground at (x, z) sees, 0..1 (1 until the map's
// sky occlusion is there)
float potop_sky_open_at(const potop_t *potop, float x, float z);
void potop_light_at(const potop_t *potop, float x, float y, float z, potop_light_info_t *light_out);

// ---------------------------------------------------------------------------
// Areas: rectangles on the map, turned any way, kept with it (a city's
// gate, the enchanted forest). Each says who it is for (its triggers);
// when one of them comes in or goes out an event says so, and what that
// does is the game's. An area turned off (triggers 0) says nothing; turned
// on again, those already in it are taken as there. An actor that dies or
// is taken away leaves without a word. An area's id is its own for good.

typedef enum
{
	POTOP_AREA_TRIGGER_NONE = 0,       // no one (turned off)
	POTOP_AREA_TRIGGER_PLAYER = 1,     // bits
	POTOP_AREA_TRIGGER_ACTORS = 2
} potop_area_trigger_t;

typedef struct
{
	uint32_t id;
	char name[32];
	float x;                       // its middle, meters
	float z;
	float width_meters;            // along its own x
	float depth_meters;            // along its own z
	float yaw_radians;             // turned this much
	float bottom_meters;           // how high it goes; top <= bottom: from the ground to the sky
	float top_meters;
	uint8_t triggers;              // potop_area_trigger_t bits
	uint8_t blocks;                // the actors it keeps out (bits of their path_groups, actors.ini; 0: none): a locked gate
} potop_area_info_t;

typedef enum
{
	POTOP_AREA_ENTER = 1,
	POTOP_AREA_EXIT
} potop_area_event_type_t;

typedef struct
{
	potop_area_event_type_t type;
	uint32_t area_id;
	bool player;                   // the player came or went; else the actor
	uint32_t actor_id;
} potop_area_event_t;

// The areas' ids (at most maximum); how many there are
int potop_area_list(const potop_t *potop, uint32_t *ids_out, int maximum);
// The first area named so (0: none)
uint32_t potop_area_find(const potop_t *potop, const char *name);
// False if there is no such area
bool potop_area_get(const potop_t *potop, uint32_t id, potop_area_info_t *info_out);
// A new area (its id: 0 if there is no room)
uint32_t potop_area_add(potop_t *potop, const char *name, float x, float z, float width_meters, float depth_meters, float yaw_radians,
	uint8_t triggers);
// Its name, place, size, heights and triggers as these (the id is kept)
bool potop_area_set(potop_t *potop, uint32_t id, const potop_area_info_t *info);
bool potop_area_set_triggers(potop_t *potop, uint32_t id, uint8_t triggers);
// The actors whose path_groups (actors.ini, 1 by default) have one of
// these bits find no way through it (0: open again); those on their way
// look again. The player is not stopped (what stands there does that)
bool potop_area_set_blocks(potop_t *potop, uint32_t id, uint8_t blocks);
bool potop_area_remove(potop_t *potop, uint32_t id);
// Whether a point is in it
bool potop_area_contains(const potop_t *potop, uint32_t id, float x, float y, float z);
// Who is in it now (whatever its triggers; the living actors): their ids
// (at most maximum), how many; player_inside_out: the player too (on foot)
int potop_area_occupants(const potop_t *potop, uint32_t id, uint32_t *actor_ids_out, int maximum, bool *player_inside_out);
bool potop_area_poll_event(potop_t *potop, potop_area_event_t *event_out);

// ---------------------------------------------------------------------------
// Changing the world by hand (an editor's tools, a game's spells). Each
// call is one stroke at a point; the caller keeps the brush. On a baked
// map the ground and the water stay as they are: those calls return false.

typedef enum
{
	POTOP_BRUSH_RAISE = 0,         // adds soil (of the soil given) under a bell
	POTOP_BRUSH_DIG,
	POTOP_BRUSH_PAINT_SOIL,        // the top's soil, a dithered edge
	POTOP_BRUSH_SMOOTH             // towards the neighbors' average
} potop_brush_t;

typedef enum
{
	POTOP_SOIL_SAND = 0,
	POTOP_SOIL_EARTH,
	POTOP_SOIL_CLAY,
	POTOP_SOIL_ROCK
} potop_soil_t;

typedef enum
{
	POTOP_STAMP_CRATER = 0,
	POTOP_STAMP_DAM,               // a line: from start to end
	POTOP_STAMP_HILL,
	POTOP_STAMP_CHANNEL,           // a line
	POTOP_STAMP_LAKE,
	POTOP_STAMP_PLATEAU
} potop_stamp_type_t;

typedef struct
{
	potop_stamp_type_t type;
	potop_soil_t soil;             // what raised stamps are made of
	float radius_meters;
	float height_meters;
	float start_x;                 // the point (or a line's start)
	float start_y;
	float start_z;
	float end_x;                   // a line's end (the start again for a point)
	float end_y;
	float end_z;
} potop_stamp_t;


// A brush stroke for seconds: rate is meters a second (raise, dig) or the
// share a second it smooths by (smooth)
bool potop_edit_terrain(potop_t *potop, potop_brush_t brush, float x, float z, float radius_meters, float rate, potop_soil_t soil,
	float seconds);
// Water poured (meters_per_second > 0) or sponged (< 0)
bool potop_edit_water(potop_t *potop, float x, float z, float radius_meters, float meters_per_second, float seconds);
bool potop_edit_stamp(potop_t *potop, const potop_stamp_t *stamp);
// How far a stamp reaches from its line (for an outline)
float potop_stamp_footprint_meters(const potop_t *potop, const potop_stamp_t *stamp);
// A spring there, or the one there taken away
bool potop_edit_toggle_spring(potop_t *potop, float x, float z);
// That much water (cubic meters) at once round (x, z), most in the middle,
// gone at radius_meters (as the water brush); false on a baked map
bool potop_edit_add_water(potop_t *potop, float x, float z, float cubic_meters, float radius_meters);
// Plants of a category (objects.ini's) under the brush for seconds; how many
int potop_edit_plant(potop_t *potop, int category, float x, float z, float radius_meters, float seconds);
// Every object under the brush away; how many
int potop_edit_clear(potop_t *potop, float x, float z, float radius_meters);
// A village's layout there; the one nearest (30 m) away: how many objects went
bool potop_edit_place_layout(potop_t *potop, int layout, float x, float z);
int potop_edit_remove_layout(potop_t *potop, float x, float z);
// A light at a point (false: no room); the one within radius away
bool potop_edit_place_light(potop_t *potop, float x, float y, float z, const potop_light_t *light);
bool potop_edit_remove_light(potop_t *potop, float x, float y, float z, float radius_meters);

#endif
