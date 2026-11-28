// What the person at the keys did to a scene: balls thrown and blasts
// set off, each noted with the frame it happened at. A scene sets them
// off again whenever it comes to that frame, so going back and forward
// replays them, and the list saved to a file lets damage_shot draw the
// very wreck that was on the screen in damage_view.
//
// The file is text, an event a line (lines starting with # are skipped):
//
//   ball FRAME position_x position_y position_z velocity_x velocity_y velocity_z
//   blast FRAME position_x position_y position_z strength

#ifndef DAMAGE_EVENTS_H
#define DAMAGE_EVENTS_H

#include <stdbool.h>

#define DAMAGE_EVENTS_MAX 256

typedef enum
{
	DAMAGE_EVENT_BALL,
	DAMAGE_EVENT_BLAST,
	DAMAGE_EVENT_POUR,                 // water poured on a disc of ground
	DAMAGE_EVENT_FLOOD,                // water everywhere up to a height
	DAMAGE_EVENT_DRY                   // all the water gone
} damage_event_kind_t;

typedef struct
{
	int frame_index;                   // it happens just before the step that leaves this frame
	damage_event_kind_t kind;
	float position[3];                 // where the ball starts or the blast is
	float velocity[3];                 // the ball's, meters a second
	float strength;                    // the blast's impulse on a square meter facing it, newton seconds
	// The water: where it is poured (position, its height unused), over
	// what radius and how many cubic meters; or the height of a flood
	float radius;
	float cubic_meters;
	float height;
} damage_event_t;

int damage_events_count(void);
const damage_event_t *damage_events_get(int event_index);
// False when the list is full
bool damage_events_add(const damage_event_t *event);
// Takes away the one added last. False when there is none.
bool damage_events_remove_last(void);
void damage_events_clear(void);
// Replaces the list with a file's. False (and the list empty) when the
// file cannot be read or a line of it makes no sense.
bool damage_events_load(const char *path);
bool damage_events_save(const char *path, const char *scene_name);

#endif
