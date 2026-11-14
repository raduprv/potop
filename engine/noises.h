// Noises: who made a sound where, how far it carries (a shot hundreds of
// meters, a step a few, a crawl hardly at all). The engine makes them (the
// player's steps, shots, blasts); whoever listens (the game's animals, its
// guards) asks what was heard near it since it last asked. No sound is
// played: that is another thing (when there is sound, the same noises).

#ifndef NOISES_H
#define NOISES_H

#include <stdbool.h>
#include <stdint.h>

enum
{
	NOISES_MAX = 256
};

typedef enum
{
	NOISE_STEP = 0,                    // feet (the player's)
	NOISE_SHOT,
	NOISE_EXPLOSION,
	NOISE_OTHER                        // the game's own (a door, a shout)
} noise_kind_t;

typedef struct
{
	float position[3];
	float radius_meters;               // heard within this
	noise_kind_t kind;
	uint32_t source;                   // an actor's id (0: the player, or no one)
	double time_seconds;               // the world's clock when it was made
} noise_t;

typedef struct
{
	noise_t noises[NOISES_MAX];        // a ring: the oldest go
	int next;
	int count;
} noises_t;

void noises_clear(noises_t *noises);
void noises_make(noises_t *noises, const float position[3], float radius_meters, noise_kind_t kind, uint32_t source, double now_seconds);
// The noises made after since_seconds that carry to (x, y, z), the newest
// first (at most maximum); how many
int noises_heard(const noises_t *noises, float x, float y, float z, double since_seconds, noise_t *out, int maximum);

#endif
