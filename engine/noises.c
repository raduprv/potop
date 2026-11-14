// Noises. See noises.h.

#include "noises.h"

#include <SDL3/SDL.h>

void noises_clear(noises_t *noises)
{
	SDL_zerop(noises);
}

void noises_make(noises_t *noises, const float position[3], float radius_meters, noise_kind_t kind, uint32_t source, double now_seconds)
{
	noise_t *noise = &noises->noises[noises->next];

	SDL_memcpy(noise->position, position, sizeof(noise->position));
	noise->radius_meters = radius_meters;
	noise->kind = kind;
	noise->source = source;
	noise->time_seconds = now_seconds;
	noises->next = (noises->next + 1) % NOISES_MAX;
	noises->count = SDL_min(noises->count + 1, NOISES_MAX);
}

int noises_heard(const noises_t *noises, float x, float y, float z, double since_seconds, noise_t *out, int maximum)
{
	int found = 0;

	// (newest first: back from the last made)
	for (int k = 1; k <= noises->count && found < maximum; k++)
	{
		const noise_t *noise = &noises->noises[(noises->next - k + NOISES_MAX) % NOISES_MAX];
		float dx = noise->position[0] - x;
		float dy = noise->position[1] - y;
		float dz = noise->position[2] - z;

		if (noise->time_seconds <= since_seconds)
		{
			break;
		}
		if (dx * dx + dy * dy + dz * dz < noise->radius_meters * noise->radius_meters)
		{
			out[found++] = *noise;
		}
	}
	return found;
}
