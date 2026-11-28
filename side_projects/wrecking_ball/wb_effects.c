// How explosions look: see wb_effects.h

#include "wb_effects.h"

#include <SDL3/SDL.h>

#define FIRE_SECONDS 0.5f
#define SMOKE_SECONDS 3.0f
#define FLASH_SECONDS 0.15f
#define SHOCK_SECONDS 0.6f
// Much slower than a real one (some 340 m/s), or it would be gone before
// it is seen
#define SHOCK_METERS_A_SECOND 60.0f
#define FIRE_BALLS 4
#define SMOKE_PUFFS 7

// A number from 0 to 1 made from two others (the same each frame)
static float hash_unit(int first, int second)
{
	uint32_t value = (uint32_t)first * 73856093u ^ (uint32_t)second * 19349663u;

	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return (float)(value & 0xffffff) / (float)0xffffff;
}

// A direction made from two numbers, leaning upwards
static void puff_direction(int explosion, int puff, float direction[3])
{
	float around = 2.0f * SDL_PI_F * hash_unit(explosion, puff * 3);
	float up = 0.2f + 0.8f * hash_unit(explosion, puff * 3 + 1);
	float across = SDL_sqrtf(1.0f - up * up);

	direction[0] = SDL_cosf(around) * across;
	direction[1] = up;
	direction[2] = SDL_sinf(around) * across;
}

void wb_effects_start(wb_effects_t *effects, const float position[3], float kilograms)
{
	wb_explosion_t *slot = &effects->explosions[0];

	for (int i = 0; i < WB_EXPLOSIONS_MAX; i++)
	{
		wb_explosion_t *explosion = &effects->explosions[i];

		if (!explosion->used)
		{
			slot = explosion;
			break;
		}
		if (explosion->age_seconds > slot->age_seconds)
		{
			slot = explosion;
		}
	}
	slot->used = true;
	SDL_memcpy(slot->position, position, sizeof(slot->position));
	slot->kilograms = kilograms;
	slot->age_seconds = 0.0f;
}

void wb_effects_update(wb_effects_t *effects, float seconds)
{
	for (int i = 0; i < WB_EXPLOSIONS_MAX; i++)
	{
		wb_explosion_t *explosion = &effects->explosions[i];

		if (!explosion->used)
		{
			continue;
		}
		explosion->age_seconds += seconds;
		if (explosion->age_seconds > SMOKE_SECONDS)
		{
			explosion->used = false;
		}
	}
}

void wb_effects_clear(wb_effects_t *effects)
{
	SDL_zerop(effects);
}

static void draw_fire(const wb_explosion_t *explosion, int number, float size, wb_render_t *render)
{
	float age = explosion->age_seconds;
	float life = age / FIRE_SECONDS;
	float swell = 1.0f - SDL_expf(-age / 0.05f);
	// White hot, through yellow, to a dull orange red
	float heat = SDL_clamp(1.0f - life * 1.6f, 0.0f, 1.0f);
	float color[3] = { 4.0f, 1.0f + 2.4f * heat, 0.25f + 1.6f * heat * heat };
	float alpha = (1.0f - life) * (1.0f - life);

	if (life >= 1.0f)
	{
		return;
	}
	for (int ball = 0; ball < FIRE_BALLS; ball++)
	{
		float direction[3];
		float position[3];
		float radius = size * (0.55f + 0.2f * (float)ball) * swell;
		float spread = ball == 0 ? 0.0f : size * 0.35f * swell;

		puff_direction(number, ball + 100, direction);
		for (int k = 0; k < 3; k++)
		{
			position[k] = explosion->position[k] + direction[k] * spread;
		}
		// (risen a little as it burns)
		position[1] += age * 1.5f;
		wb_render_add_effect(render, WB_EFFECT_FIRE, position, radius, color, alpha * (ball == 0 ? 1.0f : 0.6f));
	}
}

static void draw_smoke(const wb_explosion_t *explosion, int number, float size, wb_render_t *render)
{
	float age = explosion->age_seconds;
	float life = age / SMOKE_SECONDS;
	float thrown = 1.0f - SDL_expf(-age / 0.25f);
	// (none in the fire's first moment, then thinning)
	float alpha = 0.55f * (1.0f - life) * (1.0f - life) * SDL_clamp(age / 0.12f, 0.0f, 1.0f);

	for (int puff = 0; puff < SMOKE_PUFFS; puff++)
	{
		float direction[3];
		float position[3];
		float darkness = 0.05f + 0.05f * hash_unit(number, puff * 3 + 2);
		const float color[3] = { darkness * 1.1f, darkness, darkness * 0.9f };
		float radius = size * (0.45f + 0.75f * SDL_sqrtf(life)) * (0.8f + 0.4f * hash_unit(number, puff + 50));

		puff_direction(number, puff, direction);
		for (int k = 0; k < 3; k++)
		{
			position[k] = explosion->position[k] + direction[k] * size * 1.1f * thrown;
		}
		position[1] += age * (0.8f + 0.6f * hash_unit(number, puff + 70));
		wb_render_add_effect(render, WB_EFFECT_SMOKE, position, radius, color, alpha);
	}
}

void wb_effects_draw(const wb_effects_t *effects, wb_render_t *render)
{
	float brightest = 0.0f;

	for (int i = 0; i < WB_EXPLOSIONS_MAX; i++)
	{
		const wb_explosion_t *explosion = &effects->explosions[i];
		float size;
		float age;

		if (!explosion->used)
		{
			continue;
		}
		age = explosion->age_seconds;
		// (a ball's size grows as the cube root of the explosive: 3 kg, 1.4 m)
		size = SDL_powf(SDL_max(explosion->kilograms, 0.01f), 1.0f / 3.0f);
		draw_smoke(explosion, i, size, render);
		draw_fire(explosion, i, size, render);
		if (age < FLASH_SECONDS)
		{
			float strength = size * 45.0f * (1.0f - age / FLASH_SECONDS);

			if (strength > brightest)
			{
				const float color[3] = { strength, strength * 0.75f, strength * 0.45f };
				const float position[3] = { explosion->position[0], explosion->position[1] + 0.5f, explosion->position[2] };

				wb_render_set_flash(render, position, color);
				brightest = strength;
			}
		}
		if (age < SHOCK_SECONDS)
		{
			float fade = 1.0f - age / SHOCK_SECONDS;

			wb_render_add_shock_wave(render, explosion->position, 0.5f + SHOCK_METERS_A_SECOND * age, fade * SDL_sqrtf(fade) * size / 1.44f);
		}
	}
}
