// The editor's tools: which one is chosen and how it is set (the brush's
// size, the soil, the stamp, what is planted or placed), where the pointer
// is on the ground, a stamp being dragged. What a tool does to the world
// is the engine's (potop.h: potop_edit_*); the testbed calls it with this.
//
// The brushes are applied from the fixed simulation step, not per rendered
// frame, so a stroke does the same amount of work at any frame rate.

#ifndef TOOLS_H
#define TOOLS_H

#include <stdbool.h>

#include "math3d.h"
#include "object_types.h"
#include "settings.h"
#include "stamps.h"
#include "terrain.h"

typedef enum
{
	TOOL_RAISE = 0,       // adds soil of the selected type
	TOOL_DIG,
	TOOL_PAINT_SOIL,
	TOOL_SMOOTH,
	TOOL_STAMP,           // the current stamp, selected with T
	TOOL_PLANT,           // plants objects of the current category (trees, grass, ...), selected with G
	TOOL_CLEAR,           // removes the objects under the brush, selected with H
	TOOL_LAYOUT,          // places the current layout (homestead, camp, ...), selected with L
	TOOL_WATER_ADD,       // pours water (also shift + left mouse with any tool)
	TOOL_WATER_REMOVE,    // the sponge (also ctrl + left mouse)
	TOOL_SPRING,          // a click places or removes a spring (also F)
	TOOL_REMOVE_LAYOUT,   // a click removes the layout nearest to it (also Delete)
	TOOL_OBJECT,          // a click places one object of the chosen type (dropped from a height, if set)
	TOOL_INSPECT,         // shows everything about the cell under the pointer; a click pins it (see inspector.h)
	TOOL_EFFECT,          // a click plays the chosen particle effect: on an actor, an object, or at the point
	TOOL_LIGHT,           // a click places a light (or removes the one next to it): see lights.h
	TOOL_AREA,            // a drag draws an area, a click in one picks it (its name, size, triggers in the panel): see areas.h
	TOOL_NONE,            // nothing: clicks only pick (the start; a click on the chosen tool's button again)
	TOOL_COUNT
} tool_type_t;

// The light tool's kinds of light
enum
{
	TOOL_LIGHT_LAMP = 0,
	TOOL_LIGHT_FIRE,
	TOOL_LIGHT_COLD,
	TOOL_LIGHT_WHITE,
	TOOL_LIGHT_KIND_COUNT
};

typedef struct
{
	tool_type_t current_tool;
	soil_type_t current_soil;
	float brush_radius_meters;
	stamp_type_t current_stamp;
	int plant_category;            // what the plant tool plants, index into object_types_t.categories
	bool clear_categories[OBJECT_CATEGORIES_MAX]; // what the clear tool removes, each category (object_types_t's; all to start with)
	int layout;                    // what the layout tool places, index into layouts_t.layouts
	int object_category;           // the object tool: category shown in its list
	int object_type;               // ...and the type it places (index into object_types_t.types)
	int effect;                    // the effect tool: index into particles_t.effects
	int light_kind;                // the light tool: lamp, fire, cold, white (TOOL_LIGHT_KIND_*)
	float light_intensity;
	float light_radius_meters;
	float light_height_meters;     // above the ground where it is placed
	bool object_awake;             // loose things the physics takes at once (they settle, roll); else put down asleep
	float stamp_height_meters;

	// Line stamps (dam, channel) are dragged: where the drag started
	bool stamp_dragging;
	vec3_t stamp_drag_start;

	// The area tool: the area picked (0: none), an area being drawn from
	// where the drag started
	uint32_t area_selected;
	bool area_dragging;
	vec3_t area_drag_start;

	// Where the pointer hits the terrain, updated every frame
	bool hover_valid;
	vec3_t hover_point;
} tool_state_t;

void tools_initialize(tool_state_t *tools, const tool_settings_t *settings);
void tools_shutdown(tool_state_t *tools);

const char *tools_tool_name(tool_type_t tool);
const char *tools_soil_name(soil_type_t soil);

#endif
