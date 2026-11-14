// Weapons. See weapons.h.

#include "weapons.h"

#include <stddef.h>
#include <SDL3/SDL.h>

#include "ini_parser.h"

// How fast the kick and the flash die away (1 / seconds)
static const float KICK_FADE = 9.0f;
static const float FLASH_FADE = 30.0f;

typedef struct
{
	weapons_t *weapons;
	bool ammo_file;
} load_state_t;

static float random_unit(weapons_t *weapons)
{
	weapons->random_state = weapons->random_state * 1664525u + 1013904223u;
	return (float)(weapons->random_state >> 8) / 16777216.0f;
}

weapon_modifiers_t weapons_default_modifiers(void)
{
	weapon_modifiers_t modifiers = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0 };

	return modifiers;
}

static void weapon_defaults(weapon_type_t *type, const char *name)
{
	SDL_zerop(type);
	SDL_strlcpy(type->name, name, sizeof(type->name));
	SDL_strlcpy(type->shown_name, name, sizeof(type->shown_name));
	type->ammo = -1;
	type->fire_mode = WEAPON_FIRE_SEMI;
	type->burst_count = 3;
	type->rounds_per_minute = 600.0f;
	type->magazine = 30;
	type->reload_seconds = 2.2f;
	type->ready_seconds = 0.5f;
	type->spread_degrees = 0.3f;
	type->spread_per_shot_degrees = 0.2f;
	type->spread_most_degrees = 3.0f;
	type->spread_recovery_degrees = 6.0f;
	type->recoil_degrees = 0.8f;
	type->recoil_sideways_degrees = 0.3f;
	SDL_strlcpy(type->model, "procedural:rifle", sizeof(type->model));
	type->aim_seconds = 0.2f;
	type->aim_zoom = 1.35f;
	type->scope_zoom = 0.0f;
	type->aim_spread = 0.4f;
	type->aim_recoil = 0.7f;
	type->noise_meters = 300.0f;
}

static void ammo_defaults(weapon_ammo_t *ammo, const char *name)
{
	SDL_zerop(ammo);
	SDL_strlcpy(ammo->name, name, sizeof(ammo->name));
	ammo->muzzle_velocity = 700.0f;
	ammo->mass_grams = 8.0f;
	ammo->drag = 0.001f;
	ammo->damage = 30.0f;
	ammo->pellets = 1;
	ammo->push = 1.0f;
	ammo->life_seconds = 4.0f;
}

static weapon_type_t *weapon_named(weapons_t *weapons, const char *name)
{
	for (int i = 0; i < weapons->type_count; i++)
	{
		if (SDL_strcmp(weapons->types[i].name, name) == 0)
		{
			return &weapons->types[i];
		}
	}
	if (weapons->type_count >= WEAPONS_MAX)
	{
		return NULL;
	}
	weapon_defaults(&weapons->types[weapons->type_count], name);
	return &weapons->types[weapons->type_count++];
}

static weapon_ammo_t *ammo_named(weapons_t *weapons, const char *name)
{
	for (int i = 0; i < weapons->ammo_count; i++)
	{
		if (SDL_strcmp(weapons->ammo[i].name, name) == 0)
		{
			return &weapons->ammo[i];
		}
	}
	if (weapons->ammo_count >= WEAPONS_AMMO_MAX)
	{
		return NULL;
	}
	ammo_defaults(&weapons->ammo[weapons->ammo_count], name);
	return &weapons->ammo[weapons->ammo_count++];
}

static bool weapon_key(weapon_type_t *type, const char *key, const char *value)
{
	if (SDL_strcmp(key, "name") == 0)
	{
		SDL_strlcpy(type->shown_name, value, sizeof(type->shown_name));
		return true;
	}
	if (SDL_strcmp(key, "ammo") == 0)
	{
		SDL_strlcpy(type->ammo_name, value, sizeof(type->ammo_name));
		return true;
	}
	if (SDL_strcmp(key, "fire") == 0)
	{
		if (SDL_strcasecmp(value, "semi") == 0)
		{
			type->fire_mode = WEAPON_FIRE_SEMI;
		}
		else if (SDL_strcasecmp(value, "burst") == 0)
		{
			type->fire_mode = WEAPON_FIRE_BURST;
		}
		else if (SDL_strcasecmp(value, "auto") == 0)
		{
			type->fire_mode = WEAPON_FIRE_AUTO;
		}
		else
		{
			return false;
		}
		return true;
	}
	if (SDL_strcmp(key, "infinite_ammo") == 0)
	{
		return ini_value_to_bool(value, &type->infinite_ammo);
	}
	if (SDL_strcmp(key, "muzzle_effect") == 0)
	{
		SDL_strlcpy(type->muzzle_effect, value, sizeof(type->muzzle_effect));
		return true;
	}
	if (SDL_strcmp(key, "tracer_effect") == 0)
	{
		SDL_strlcpy(type->tracer_effect, value, sizeof(type->tracer_effect));
		return true;
	}
	if (SDL_strcmp(key, "model") == 0)
	{
		SDL_strlcpy(type->model, value, sizeof(type->model));
		return true;
	}
	if (SDL_strcmp(key, "burst_count") == 0)
	{
		return ini_value_to_int(value, &type->burst_count);
	}
	if (SDL_strcmp(key, "magazine") == 0)
	{
		return ini_value_to_int(value, &type->magazine);
	}
	if (SDL_strcmp(key, "tracer_every") == 0)
	{
		return ini_value_to_int(value, &type->tracer_every);
	}
	{
		static const struct
		{
			const char *key;
			size_t offset;
		} floats[] = {
			{ "rounds_per_minute", offsetof(weapon_type_t, rounds_per_minute) },
			{ "reload_seconds", offsetof(weapon_type_t, reload_seconds) },
			{ "ready_seconds", offsetof(weapon_type_t, ready_seconds) },
			{ "spread_degrees", offsetof(weapon_type_t, spread_degrees) },
			{ "spread_per_shot_degrees", offsetof(weapon_type_t, spread_per_shot_degrees) },
			{ "spread_most_degrees", offsetof(weapon_type_t, spread_most_degrees) },
			{ "spread_recovery_degrees", offsetof(weapon_type_t, spread_recovery_degrees) },
			{ "recoil_degrees", offsetof(weapon_type_t, recoil_degrees) },
			{ "recoil_sideways_degrees", offsetof(weapon_type_t, recoil_sideways_degrees) },
			{ "aim_seconds", offsetof(weapon_type_t, aim_seconds) },
			{ "aim_zoom", offsetof(weapon_type_t, aim_zoom) },
			{ "scope_zoom", offsetof(weapon_type_t, scope_zoom) },
			{ "aim_spread", offsetof(weapon_type_t, aim_spread) },
			{ "aim_recoil", offsetof(weapon_type_t, aim_recoil) },
			{ "noise_meters", offsetof(weapon_type_t, noise_meters) },
		};

		for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); i++)
		{
			if (SDL_strcmp(key, floats[i].key) == 0)
			{
				return ini_value_to_float(value, (float *)((char *)type + floats[i].offset));
			}
		}
	}
	return false;
}

static bool ammo_key(weapon_ammo_t *ammo, const char *key, const char *value)
{
	static const struct
	{
		const char *key;
		size_t offset;
	} floats[] = {
		{ "muzzle_velocity", offsetof(weapon_ammo_t, muzzle_velocity) },
		{ "mass_grams", offsetof(weapon_ammo_t, mass_grams) },
		{ "drag", offsetof(weapon_ammo_t, drag) },
		{ "damage", offsetof(weapon_ammo_t, damage) },
		{ "pellet_spread_degrees", offsetof(weapon_ammo_t, pellet_spread_degrees) },
		{ "push", offsetof(weapon_ammo_t, push) },
		{ "life_seconds", offsetof(weapon_ammo_t, life_seconds) },
	};

	if (SDL_strcmp(key, "pellets") == 0)
	{
		return ini_value_to_int(value, &ammo->pellets);
	}
	if (SDL_strcmp(key, "damage_type") == 0)
	{
		return damage_type_from_name(value, &ammo->damage_type);
	}
	for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); i++)
	{
		if (SDL_strcmp(key, floats[i].key) == 0)
		{
			return ini_value_to_float(value, (float *)((char *)ammo + floats[i].offset));
		}
	}
	return false;
}

static bool load_key(void *user_data, const char *section, const char *key, const char *value)
{
	load_state_t *state = user_data;

	if (section == NULL || section[0] == '\0')
	{
		return false;
	}
	if (state->ammo_file)
	{
		weapon_ammo_t *ammo = ammo_named(state->weapons, section);

		return ammo != NULL && ammo_key(ammo, key, value);
	}
	{
		weapon_type_t *type = weapon_named(state->weapons, section);

		return type != NULL && weapon_key(type, key, value);
	}
}

bool weapons_load(weapons_t *weapons)
{
	load_state_t state;

	SDL_zerop(weapons);
	weapons->random_state = 0x9e3779b9u;
	state.weapons = weapons;
	state.ammo_file = true;
	if (!ini_parse_data_file("ammo.ini", load_key, &state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Weapons: no data/ammo.ini");
	}
	state.ammo_file = false;
	if (!ini_parse_data_file("weapons.ini", load_key, &state))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Weapons: no data/weapons.ini");
	}
	for (int i = 0; i < weapons->type_count; i++)
	{
		weapon_type_t *type = &weapons->types[i];

		for (int k = 0; k < weapons->ammo_count; k++)
		{
			if (SDL_strcmp(weapons->ammo[k].name, type->ammo_name) == 0)
			{
				type->ammo = k;
			}
		}
		if (type->ammo < 0)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Weapons: %s fires \"%s\", which ammo.ini does not have", type->name, type->ammo_name);
		}
		type->magazine = SDL_max(type->magazine, 1);
		weapons->rounds[i] = type->magazine;
		weapons->modifiers[i] = weapons_default_modifiers();
	}
	weapons->state = WEAPON_STATE_AWAY;
	weapons->next = -1;
	for (int k = 0; k < WEAPONS_AMMO_MAX; k++)
	{
		weapons->ammo_reserve[k] = -1;
	}
	SDL_Log("Weapons: %d, %d kinds of ammo", weapons->type_count, weapons->ammo_count);
	return weapons->type_count > 0;
}

static int magazine_of(const weapons_t *weapons, int weapon)
{
	return SDL_max(weapons->types[weapon].magazine + weapons->modifiers[weapon].magazine_bonus, 1);
}

void weapons_toggle(weapons_t *weapons)
{
	if (weapons->type_count == 0)
	{
		return;
	}
	if (weapons->state == WEAPON_STATE_AWAY || weapons->state == WEAPON_STATE_LOWERING)
	{
		weapons->state = WEAPON_STATE_RAISING;
	}
	else
	{
		weapons->state = WEAPON_STATE_LOWERING;
	}
	weapons->state_seconds = 0.0f;
	weapons->burst_left = 0;
}

void weapons_select(weapons_t *weapons, int weapon)
{
	if (weapon < 0 || weapon >= weapons->type_count)
	{
		return;
	}
	if (weapons->state == WEAPON_STATE_AWAY)
	{
		weapons->current = weapon;
		weapons->next = -1;
		weapons_toggle(weapons);
		return;
	}
	if (weapon == weapons->current)
	{
		weapons->next = -1;
		if (weapons->state != WEAPON_STATE_LOWERING)
		{
			weapons_toggle(weapons);
		}
		return;
	}
	// (the other one: this one down first, then that one up)
	weapons->next = weapon;
	if (weapons->state != WEAPON_STATE_LOWERING)
	{
		weapons_toggle(weapons);
	}
}

bool weapons_in_hand(const weapons_t *weapons)
{
	return weapons->state != WEAPON_STATE_AWAY;
}

void weapons_set_reserve(weapons_t *weapons, int ammo, int rounds)
{
	if (ammo >= 0 && ammo < WEAPONS_AMMO_MAX)
	{
		weapons->ammo_reserve[ammo] = rounds;
	}
}

int weapons_take_loaded(weapons_t *weapons, int ammo)
{
	int loaded;

	if (ammo < 0 || ammo >= WEAPONS_AMMO_MAX)
	{
		return 0;
	}
	loaded = weapons->ammo_loaded[ammo];
	weapons->ammo_loaded[ammo] = 0;
	return loaded;
}

void weapons_reload(weapons_t *weapons)
{
	int ammo = weapons->type_count > 0 ? weapons->types[weapons->current].ammo : -1;

	if (weapons->state != WEAPON_STATE_READY || weapons->rounds[weapons->current] >= magazine_of(weapons, weapons->current)
		|| (ammo >= 0 && weapons->ammo_reserve[ammo] == 0))
	{
		return;
	}
	weapons->state = WEAPON_STATE_RELOADING;
	weapons->state_seconds = 0.0f;
	weapons->burst_left = 0;
}

void weapons_set_modifiers(weapons_t *weapons, int weapon, const weapon_modifiers_t *modifiers)
{
	if (weapon >= 0 && weapon < weapons->type_count && modifiers != NULL)
	{
		weapons->modifiers[weapon] = *modifiers;
	}
}

// A direction within a cone of half angle degrees around axis (the
// camera's forward, with its right and up)
static void cone_direction(weapons_t *weapons, const float forward[3], const float right[3], const float up[3], float degrees, float out[3])
{
	float angle = random_unit(weapons) * 2.0f * SDL_PI_F;
	// (more in the middle: a disc's radius squared)
	float off = SDL_tanf(degrees * SDL_PI_F / 180.0f) * SDL_sqrtf(random_unit(weapons));
	float side = SDL_cosf(angle) * off;
	float lift = SDL_sinf(angle) * off;
	float length;

	for (int k = 0; k < 3; k++)
	{
		out[k] = forward[k] + right[k] * side + up[k] * lift;
	}
	length = SDL_sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	for (int k = 0; k < 3; k++)
	{
		out[k] /= SDL_max(length, 1e-6f);
	}
}

int weapons_update(weapons_t *weapons, float seconds, bool trigger_held, bool aim_held, const float eye[3], const float forward[3],
	const float right[3], const float up[3], weapon_shot_t *shots_out, int max_shots, float *kick_up_degrees_out, float *kick_right_degrees_out)
{
	int shots = 0;
	weapon_type_t *type;
	const weapon_modifiers_t *modifiers;
	bool pressed = trigger_held && !weapons->trigger_was_held;

	*kick_up_degrees_out = 0.0f;
	*kick_right_degrees_out = 0.0f;
	weapons->kick *= SDL_expf(-KICK_FADE * seconds);
	weapons->flash *= SDL_expf(-FLASH_FADE * seconds);
	weapons->trigger_was_held = trigger_held;
	if (weapons->type_count == 0 || seconds <= 0.0f)
	{
		return 0;
	}
	type = &weapons->types[weapons->current];
	modifiers = &weapons->modifiers[weapons->current];
	weapons->state_seconds += seconds;
	weapons->cooldown_seconds = SDL_max(weapons->cooldown_seconds - seconds, 0.0f);
	weapons->spread_now_degrees = SDL_max(weapons->spread_now_degrees - type->spread_recovery_degrees * seconds, 0.0f);
	{
		// Up to the eye while the button is held, the weapon ready
		float target = aim_held && weapons->state == WEAPON_STATE_READY ? 1.0f : 0.0f;
		float step = seconds / SDL_max(type->aim_seconds, 0.01f);

		weapons->aim = target > weapons->aim ? SDL_min(weapons->aim + step, target) : SDL_max(weapons->aim - step, target);
	}
	switch (weapons->state)
	{
		case WEAPON_STATE_RAISING:
			if (weapons->state_seconds >= type->ready_seconds)
			{
				weapons->state = WEAPON_STATE_READY;
				weapons->state_seconds = 0.0f;
			}
			return 0;
		case WEAPON_STATE_LOWERING:
			if (weapons->state_seconds >= type->ready_seconds)
			{
				weapons->state = WEAPON_STATE_AWAY;
				weapons->state_seconds = 0.0f;
				weapons->aim = 0.0f;
				if (weapons->next >= 0)
				{
					weapons->current = weapons->next;
					weapons->next = -1;
					weapons->state = WEAPON_STATE_RAISING;
				}
			}
			return 0;
		case WEAPON_STATE_RELOADING:
			if (weapons->state_seconds >= type->reload_seconds * modifiers->reload_time)
			{
				// As many as there are to load
				int need = magazine_of(weapons, weapons->current) - weapons->rounds[weapons->current];
				int taken = need;

				if (type->ammo >= 0 && weapons->ammo_reserve[type->ammo] >= 0)
				{
					taken = SDL_min(need, weapons->ammo_reserve[type->ammo]);
					weapons->ammo_reserve[type->ammo] -= taken;
				}
				if (type->ammo >= 0)
				{
					weapons->ammo_loaded[type->ammo] += SDL_max(taken, 0);
				}
				weapons->rounds[weapons->current] += SDL_max(taken, 0);
				weapons->state = WEAPON_STATE_READY;
				weapons->state_seconds = 0.0f;
			}
			return 0;
		case WEAPON_STATE_AWAY:
			return 0;
		case WEAPON_STATE_READY:
		default:
			break;
	}
	// What the trigger asks for
	if (pressed)
	{
		if (weapons->rounds[weapons->current] <= 0)
		{
			// (empty: a click asks for a new magazine)
			weapons_reload(weapons);
			return 0;
		}
		weapons->burst_left = type->fire_mode == WEAPON_FIRE_BURST ? SDL_max(type->burst_count, 1) : 1;
	}
	if (type->fire_mode == WEAPON_FIRE_AUTO && trigger_held && weapons->burst_left == 0)
	{
		weapons->burst_left = 1;
	}
	while (weapons->burst_left > 0 && weapons->cooldown_seconds <= 0.0f && weapons->rounds[weapons->current] > 0 && shots < max_shots
		&& type->ammo >= 0)
	{
		const weapon_ammo_t *ammo = &weapons->ammo[type->ammo];
		float aimed_share = weapons_aim(weapons);
		float spread = (type->spread_degrees + weapons->spread_now_degrees) * modifiers->spread * (1.0f + (type->aim_spread - 1.0f) * aimed_share);
		float recoil = modifiers->recoil * (1.0f + (type->aim_recoil - 1.0f) * aimed_share);
		float aimed[3];
		int pellets = SDL_max(ammo->pellets, 1);
		bool tracer = type->tracer_every > 0 && (weapons->shots_fired % type->tracer_every) == 0;

		cone_direction(weapons, forward, right, up, spread, aimed);
		for (int p = 0; p < pellets && shots < max_shots; p++)
		{
			weapon_shot_t *shot = &shots_out[shots++];

			if (pellets > 1)
			{
				// (around the aimed one; its right and up near enough the camera's)
				cone_direction(weapons, aimed, right, up, ammo->pellet_spread_degrees, shot->direction);
			}
			else
			{
				SDL_memcpy(shot->direction, aimed, sizeof(aimed));
			}
			SDL_memcpy(shot->origin, eye, sizeof(shot->origin));
			shot->speed = ammo->muzzle_velocity * modifiers->muzzle_velocity;
			shot->ammo = type->ammo;
			shot->damage_scale = modifiers->damage;
			shot->tracer = tracer && p == 0;
		}
		if (!type->infinite_ammo)
		{
			weapons->rounds[weapons->current]--;
		}
		weapons->shots_fired++;
		weapons->burst_left--;
		weapons->cooldown_seconds += 60.0f / SDL_max(type->rounds_per_minute * modifiers->fire_rate, 1.0f);
		weapons->spread_now_degrees = SDL_min(weapons->spread_now_degrees + type->spread_per_shot_degrees, type->spread_most_degrees);
		*kick_up_degrees_out += type->recoil_degrees * recoil * (0.8f + 0.4f * random_unit(weapons));
		*kick_right_degrees_out += type->recoil_sideways_degrees * recoil * (random_unit(weapons) * 2.0f - 1.0f);
		weapons->kick = 1.0f;
		weapons->flash = 1.0f;
	}
	if (weapons->rounds[weapons->current] <= 0)
	{
		weapons->burst_left = 0;
	}
	return shots;
}

float weapons_raised(const weapons_t *weapons)
{
	float ready = weapons->type_count > 0 ? SDL_max(weapons->types[weapons->current].ready_seconds, 0.01f) : 1.0f;
	float t = SDL_clamp(weapons->state_seconds / ready, 0.0f, 1.0f);

	switch (weapons->state)
	{
		case WEAPON_STATE_AWAY:
			return 0.0f;
		case WEAPON_STATE_RAISING:
			return t;
		case WEAPON_STATE_LOWERING:
			return 1.0f - t;
		default:
			return 1.0f;
	}
}

float weapons_reload_progress(const weapons_t *weapons)
{
	if (weapons->state != WEAPON_STATE_RELOADING || weapons->type_count == 0)
	{
		return 0.0f;
	}
	return SDL_clamp(weapons->state_seconds / SDL_max(weapons->types[weapons->current].reload_seconds * weapons->modifiers[weapons->current].reload_time,
		0.01f), 0.0f, 1.0f);
}

void weapons_describe(const weapons_t *weapons, char *text, int text_size)
{
	static const char *modes[] = { "semi", "burst", "auto" };
	const weapon_type_t *type;

	if (text_size <= 0)
	{
		return;
	}
	text[0] = '\0';
	if (weapons->type_count == 0 || weapons->state == WEAPON_STATE_AWAY)
	{
		return;
	}
	type = &weapons->types[weapons->current];
	{
		char reserve[24] = "";

		if (type->ammo >= 0 && weapons->ammo_reserve[type->ammo] >= 0)
		{
			SDL_snprintf(reserve, sizeof(reserve), " +%d", weapons->ammo_reserve[type->ammo]);
		}
		if (type->infinite_ammo)
		{
			SDL_snprintf(text, (size_t)text_size, "%s (endless) %s", type->shown_name, modes[type->fire_mode]);
			return;
		}
		SDL_snprintf(text, (size_t)text_size, "%s %d/%d%s %s%s", type->shown_name, weapons->rounds[weapons->current],
			magazine_of(weapons, weapons->current), reserve, modes[type->fire_mode], weapons->state == WEAPON_STATE_RELOADING ? " (reloading)" : "");
	}
}

float weapons_aim(const weapons_t *weapons)
{
	float t = SDL_clamp(weapons->aim, 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

bool weapons_scoped(const weapons_t *weapons)
{
	return weapons->type_count > 0 && weapons->types[weapons->current].scope_zoom > 1.0f;
}

float weapons_zoom(const weapons_t *weapons)
{
	const weapon_type_t *type;
	float zoom;

	if (weapons->type_count == 0 || weapons->state == WEAPON_STATE_AWAY)
	{
		return 1.0f;
	}
	type = &weapons->types[weapons->current];
	zoom = weapons_scoped(weapons) ? type->scope_zoom : SDL_max(type->aim_zoom, 1.0f);
	// (a scope's view only once the eye is at it)
	if (weapons_scoped(weapons))
	{
		return weapons_through_scope(weapons) ? zoom : 1.0f + (SDL_max(type->aim_zoom, 1.0f) - 1.0f) * weapons_aim(weapons);
	}
	return 1.0f + (zoom - 1.0f) * weapons_aim(weapons);
}

bool weapons_through_scope(const weapons_t *weapons)
{
	return weapons_scoped(weapons) && weapons->aim >= 0.95f;
}
