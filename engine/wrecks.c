// The breakable buildings on the map: see wrecks.h

#include "wrecks.h"

#include <SDL3/SDL.h>

#include "ini_parser.h"
#include "object_model.h"

// A log's sides (wreck.h: a prism of 12 sides)
#define LOG_SIDES 12

static const char *const MATERIAL_NAMES[WRECK_MATERIAL_COUNT] = { "wood", "brick", "glass" };
static const char *const LINK_NAMES[WRECK_LINK_TYPE_COUNT] = { "nails", "mortar", "foundation", "glazing", "notch" };

// ---------------------------------------------------------------------------
// The settings

static void default_looks(wrecks_t *wrecks)
{
	SDL_strlcpy(wrecks->looks[WRECK_MATERIAL_WOOD].albedo, "textures/old_wood.png", sizeof(wrecks->looks[0].albedo));
	SDL_strlcpy(wrecks->looks[WRECK_MATERIAL_WOOD].normal, "textures/old_wood_normal.png", sizeof(wrecks->looks[0].normal));
	SDL_strlcpy(wrecks->looks[WRECK_MATERIAL_WOOD].orm, "textures/old_wood_arm.png", sizeof(wrecks->looks[0].orm));
	wrecks->looks[WRECK_MATERIAL_WOOD].texture_meters = 1.0f;
	SDL_strlcpy(wrecks->looks[WRECK_MATERIAL_BRICK].albedo, "textures/clay_01.png", sizeof(wrecks->looks[0].albedo));
	SDL_strlcpy(wrecks->looks[WRECK_MATERIAL_BRICK].normal, "textures/clay_01_normal.png", sizeof(wrecks->looks[0].normal));
	wrecks->looks[WRECK_MATERIAL_BRICK].orm[0] = '\0';
	wrecks->looks[WRECK_MATERIAL_BRICK].texture_meters = 0.5f;
	wrecks->looks[WRECK_MATERIAL_GLASS].albedo[0] = '\0';
	wrecks->looks[WRECK_MATERIAL_GLASS].normal[0] = '\0';
	wrecks->looks[WRECK_MATERIAL_GLASS].orm[0] = '\0';
	wrecks->looks[WRECK_MATERIAL_GLASS].texture_meters = 1.0f;
}

static int material_from_name(const char *name)
{
	for (int m = 0; m < WRECK_MATERIAL_COUNT; m++)
	{
		if (SDL_strcasecmp(name, MATERIAL_NAMES[m]) == 0)
		{
			return m;
		}
	}
	return -1;
}

// [looks]: wood_albedo, wood_normal, wood_orm, wood_meters (and brick_,
// glass_); every other section is the model's
static bool read_settings_key(void *user, const char *section, const char *key, const char *value)
{
	wrecks_t *wrecks = user;

	if (SDL_strcmp(section, "looks") == 0)
	{
		for (int m = 0; m < WRECK_MATERIAL_COUNT; m++)
		{
			size_t length = SDL_strlen(MATERIAL_NAMES[m]);
			wrecks_look_t *look = &wrecks->looks[m];
			const char *rest;

			if (SDL_strncmp(key, MATERIAL_NAMES[m], length) != 0 || key[length] != '_')
			{
				continue;
			}
			rest = key + length + 1;
			if (SDL_strcmp(rest, "albedo") == 0)
			{
				SDL_strlcpy(look->albedo, value, sizeof(look->albedo));
				return true;
			}
			if (SDL_strcmp(rest, "normal") == 0)
			{
				SDL_strlcpy(look->normal, value, sizeof(look->normal));
				return true;
			}
			if (SDL_strcmp(rest, "orm") == 0)
			{
				SDL_strlcpy(look->orm, value, sizeof(look->orm));
				return true;
			}
			if (SDL_strcmp(rest, "meters") == 0)
			{
				return ini_value_to_float(value, &look->texture_meters);
			}
		}
		return false;
	}
	return wreck_settings_set(&wrecks->settings, section, key, value);
}

void wrecks_initialize(wrecks_t *wrecks)
{
	SDL_zerop(wrecks);
	wreck_settings_default(&wrecks->settings);
	default_looks(wrecks);
	if (!ini_parse_data_file("wreck.ini", read_settings_key, wrecks))
	{
		SDL_Log("Wrecks: no data/wreck.ini, the model's own numbers");
	}
	for (int m = 0; m < WRECK_MATERIAL_COUNT; m++)
	{
		wrecks->looks[m].texture_meters = SDL_max(wrecks->looks[m].texture_meters, 0.05f);
	}
}

// ---------------------------------------------------------------------------
// The pieces' models

typedef struct
{
	float *vertices;                   // 8 floats each
	float *tangents;                   // 4 floats each
	uint16_t *indices;
	int vertex_count;
	int index_count;
} mesh_t;

static void cross_3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot_3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void add_vertex(mesh_t *mesh, const float position[3], const float normal[3], float u, float v, const float tangent[3])
{
	float *out = &mesh->vertices[mesh->vertex_count * 8];
	float *out_tangent = &mesh->tangents[mesh->vertex_count * 4];

	out[0] = position[0];
	out[1] = position[1];
	out[2] = position[2];
	out[3] = normal[0];
	out[4] = normal[1];
	out[5] = normal[2];
	out[6] = u;
	out[7] = v;
	out_tangent[0] = tangent[0];
	out_tangent[1] = tangent[1];
	out_tangent[2] = tangent[2];
	out_tangent[3] = 1.0f;
	mesh->vertex_count++;
}

// A quad, its corners counter clockwise seen from where its normal points
static void add_quad(mesh_t *mesh, const float corners[4][3], const float normal[3], const float tangent[3], const float bitangent[3],
	float texture_meters)
{
	int first = mesh->vertex_count;

	for (int c = 0; c < 4; c++)
	{
		add_vertex(mesh, corners[c], normal, dot_3(corners[c], tangent) / texture_meters, dot_3(corners[c], bitangent) / texture_meters, tangent);
	}
	mesh->indices[mesh->index_count++] = (uint16_t)first;
	mesh->indices[mesh->index_count++] = (uint16_t)(first + 1);
	mesh->indices[mesh->index_count++] = (uint16_t)(first + 2);
	mesh->indices[mesh->index_count++] = (uint16_t)first;
	mesh->indices[mesh->index_count++] = (uint16_t)(first + 2);
	mesh->indices[mesh->index_count++] = (uint16_t)(first + 3);
}

// A box of these half sizes round its middle: each face's texture runs
// along its longer side (a board's grain along the board)
static void make_box(mesh_t *mesh, const float half[3], float texture_meters)
{
	for (int axis = 0; axis < 3; axis++)
	{
		for (int sign = -1; sign <= 1; sign += 2)
		{
			int first_other = (axis + 1) % 3;
			int second_other = (axis + 2) % 3;
			int along = half[first_other] >= half[second_other] ? first_other : second_other;
			float normal[3] = { 0.0f, 0.0f, 0.0f };
			float tangent[3] = { 0.0f, 0.0f, 0.0f };
			float bitangent[3];
			float corners[4][3];
			static const float signs[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
			int across;

			normal[axis] = (float)sign;
			tangent[along] = 1.0f;
			cross_3(normal, tangent, bitangent);
			across = SDL_fabsf(bitangent[0]) > 0.5f ? 0 : (SDL_fabsf(bitangent[1]) > 0.5f ? 1 : 2);
			for (int c = 0; c < 4; c++)
			{
				for (int k = 0; k < 3; k++)
				{
					corners[c][k] = normal[k] * half[k] + tangent[k] * signs[c][0] * half[along] + bitangent[k] * signs[c][1] * half[across];
				}
			}
			add_quad(mesh, corners, normal, tangent, bitangent, texture_meters);
		}
	}
}

// A log: a prism of LOG_SIDES sides along X, round its middle, smooth on
// its sides (the texture along it and round it), flat at its ends
static void make_log(mesh_t *mesh, float radius, float half_length, float texture_meters)
{
	float along[3] = { 1.0f, 0.0f, 0.0f };
	int first = mesh->vertex_count;

	// The sides: a ring of vertices at each end
	for (int s = 0; s <= LOG_SIDES; s++)
	{
		float angle = 2.0f * SDL_PI_F * (float)s / (float)LOG_SIDES;
		float normal[3] = { 0.0f, SDL_cosf(angle), SDL_sinf(angle) };
		float v = angle * radius / texture_meters;

		for (int end = 0; end < 2; end++)
		{
			float position[3] = { end == 0 ? -half_length : half_length, normal[1] * radius, normal[2] * radius };

			add_vertex(mesh, position, normal, position[0] / texture_meters, v, along);
		}
	}
	for (int s = 0; s < LOG_SIDES; s++)
	{
		int a = first + s * 2;

		// (counter clockwise from outside: round from +Y towards +Z, seen
		// from outside, with X going one way)
		mesh->indices[mesh->index_count++] = (uint16_t)a;
		mesh->indices[mesh->index_count++] = (uint16_t)(a + 2);
		mesh->indices[mesh->index_count++] = (uint16_t)(a + 1);
		mesh->indices[mesh->index_count++] = (uint16_t)(a + 1);
		mesh->indices[mesh->index_count++] = (uint16_t)(a + 2);
		mesh->indices[mesh->index_count++] = (uint16_t)(a + 3);
	}
	// The ends: a fan each
	for (int end = 0; end < 2; end++)
	{
		float sign = end == 0 ? -1.0f : 1.0f;
		float normal[3] = { sign, 0.0f, 0.0f };
		float tangent[3] = { 0.0f, 0.0f, 1.0f };
		int middle = mesh->vertex_count;
		float center[3] = { sign * half_length, 0.0f, 0.0f };

		add_vertex(mesh, center, normal, 0.5f, 0.5f, tangent);
		for (int s = 0; s < LOG_SIDES; s++)
		{
			float angle = 2.0f * SDL_PI_F * (float)s / (float)LOG_SIDES;
			float position[3] = { sign * half_length, SDL_cosf(angle) * radius, SDL_sinf(angle) * radius };

			add_vertex(mesh, position, normal, 0.5f + 0.5f * SDL_sinf(angle), 0.5f + 0.5f * SDL_cosf(angle), tangent);
		}
		for (int s = 0; s < LOG_SIDES; s++)
		{
			int a = middle + 1 + s;
			int b = middle + 1 + (s + 1) % LOG_SIDES;

			mesh->indices[mesh->index_count++] = (uint16_t)middle;
			mesh->indices[mesh->index_count++] = (uint16_t)(end == 0 ? b : a);
			mesh->indices[mesh->index_count++] = (uint16_t)(end == 0 ? a : b);
		}
	}
}

static object_material_t material_of(objects_t *objects, const wrecks_look_t *look, wreck_material_t which)
{
	object_material_t material;

	SDL_zero(material);
	material.texture = look->albedo[0] != '\0' ? object_texture_cache_get(&objects->textures, look->albedo, false) : NULL;
	if (material.texture == NULL)
	{
		// (glass, or a texture that is not there: a plain color)
		static const uint8_t colors[WRECK_MATERIAL_COUNT][4] = { { 140, 100, 65, 255 }, { 150, 70, 50, 255 }, { 160, 190, 200, 255 } };

		material.texture = object_texture_solid(colors[which]);
	}
	material.normal_texture = look->normal[0] != '\0' ? object_texture_cache_get(&objects->textures, look->normal, true) : NULL;
	material.orm_texture = look->orm[0] != '\0' ? object_texture_cache_get(&objects->textures, look->orm, true) : NULL;
	if (material.normal_texture == NULL)
	{
		material.normal_texture = object_default_texture(OBJECT_DEFAULT_NORMAL);
	}
	if (material.orm_texture == NULL)
	{
		material.orm_texture = object_default_texture(OBJECT_DEFAULT_ORM);
	}
	material.emissive_texture = object_default_texture(OBJECT_DEFAULT_EMISSIVE);
	material.shaded = true;
	material.solid = true;
	return material;
}

// The procedural type a piece is drawn with (made now if it is the first
// of its shape, size and material); -1 if it cannot be
static int piece_type(wrecks_t *wrecks, objects_t *objects, const wreck_piece_t *piece)
{
	char key[OBJECT_MODEL_NAME_LENGTH];
	const wrecks_look_t *look = &wrecks->looks[piece->material];
	int type;
	mesh_t mesh;
	object_material_t material;

	if (piece->shape == WRECK_PIECE_LOG)
	{
		SDL_snprintf(key, sizeof(key), "wreck:log:%s:%.3f:%.3f", MATERIAL_NAMES[piece->material], (double)piece->radius,
			(double)piece->half_length);
	}
	else
	{
		SDL_snprintf(key, sizeof(key), "wreck:box:%s:%.3f:%.3f:%.3f", MATERIAL_NAMES[piece->material], (double)piece->half_extents.x,
			(double)piece->half_extents.y, (double)piece->half_extents.z);
	}
	type = objects_procedural_type(objects, key, NULL, NULL, 0, NULL, 0, NULL);
	if (type >= 0)
	{
		return type;
	}
	SDL_zero(mesh);
	mesh.vertices = SDL_malloc(sizeof(float) * 8 * 64);
	mesh.tangents = SDL_malloc(sizeof(float) * 4 * 64);
	mesh.indices = SDL_malloc(sizeof(uint16_t) * 160);
	if (mesh.vertices == NULL || mesh.tangents == NULL || mesh.indices == NULL)
	{
		SDL_free(mesh.vertices);
		SDL_free(mesh.tangents);
		SDL_free(mesh.indices);
		return -1;
	}
	if (piece->shape == WRECK_PIECE_LOG)
	{
		make_log(&mesh, piece->radius, piece->half_length, look->texture_meters);
	}
	else
	{
		float half[3] = { piece->half_extents.x, piece->half_extents.y, piece->half_extents.z };

		make_box(&mesh, half, look->texture_meters);
	}
	material = material_of(objects, look, piece->material);
	type = objects_procedural_type(objects, key, mesh.vertices, mesh.tangents, (uint32_t)mesh.vertex_count, mesh.indices,
		(uint32_t)mesh.index_count, &material);
	SDL_free(mesh.vertices);
	SDL_free(mesh.tangents);
	SDL_free(mesh.indices);
	return type;
}

// ---------------------------------------------------------------------------
// Reading a building

typedef struct
{
	wrecks_t *wrecks;
	wreck_model_t *model;
	const char *name;
	// Where the building stands: its foot, its turn round the vertical
	float origin[3];
	float turn[4];
	bool failed;
} reading_t;

static void turn_vector(const float rotation[4], const float vector[3], float out[3])
{
	float twice_cross[3];
	float second[3];

	cross_3(rotation, vector, twice_cross);
	for (int k = 0; k < 3; k++)
	{
		twice_cross[k] *= 2.0f;
	}
	cross_3(rotation, twice_cross, second);
	for (int k = 0; k < 3; k++)
	{
		out[k] = vector[k] + rotation[3] * twice_cross[k] + second[k];
	}
}

static b3Vec3 placed_point(const reading_t *reading, const float local[3])
{
	float turned[3];

	turn_vector(reading->turn, local, turned);
	return (b3Vec3){ reading->origin[0] + turned[0], reading->origin[1] + turned[1], reading->origin[2] + turned[2] };
}

// "a, b, c" into the numbers; how many were read
static int read_numbers(const char *text, float *numbers, int most)
{
	int count = 0;
	const char *at = text;

	while (count < most && *at != '\0')
	{
		char *end;
		float value = (float)SDL_strtod(at, &end);

		if (end == at)
		{
			break;
		}
		numbers[count++] = value;
		at = end;
		while (*at == ' ' || *at == '\t' || *at == ',')
		{
			at++;
		}
	}
	return count;
}

static bool read_wreck_key(void *user, const char *section, const char *key, const char *value)
{
	reading_t *reading = user;
	char word[32];
	const char *rest = value;
	size_t length = 0;

	// The first word, up to its comma
	while (*rest == ' ' || *rest == '\t')
	{
		rest++;
	}
	while (rest[length] != '\0' && rest[length] != ',' && length + 1 < sizeof(word))
	{
		word[length] = rest[length];
		length++;
	}
	word[length] = '\0';
	while (length > 0 && (word[length - 1] == ' ' || word[length - 1] == '\t'))
	{
		word[--length] = '\0';
	}
	rest += SDL_strlen(word);
	while (*rest == ' ' || *rest == '\t' || *rest == ',')
	{
		rest++;
	}
	if (SDL_strcmp(section, "pieces") == 0 && (SDL_strcmp(key, "box") == 0 || SDL_strcmp(key, "log") == 0))
	{
		int material = material_from_name(word);
		bool box = SDL_strcmp(key, "box") == 0;
		float numbers[10];
		float local_rotation[4];
		float rotation[4];
		b3Quat quaternion;
		int wanted = box ? 10 : 9;

		if (material < 0 || read_numbers(rest, numbers, wanted) != wanted)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Wrecks: %s: a %s I cannot read: '%s'", reading->name, key, value);
			reading->failed = true;
			return true;
		}
		// (its own turn, then the building's)
		SDL_memcpy(local_rotation, &numbers[3], sizeof(local_rotation));
		rotation[0] = reading->turn[3] * local_rotation[0] + reading->turn[0] * local_rotation[3] + reading->turn[1] * local_rotation[2]
			- reading->turn[2] * local_rotation[1];
		rotation[1] = reading->turn[3] * local_rotation[1] - reading->turn[0] * local_rotation[2] + reading->turn[1] * local_rotation[3]
			+ reading->turn[2] * local_rotation[0];
		rotation[2] = reading->turn[3] * local_rotation[2] + reading->turn[0] * local_rotation[1] - reading->turn[1] * local_rotation[0]
			+ reading->turn[2] * local_rotation[3];
		rotation[3] = reading->turn[3] * local_rotation[3] - reading->turn[0] * local_rotation[0] - reading->turn[1] * local_rotation[1]
			- reading->turn[2] * local_rotation[2];
		quaternion = (b3Quat){ { rotation[0], rotation[1], rotation[2] }, rotation[3] };
		quaternion = b3NormalizeQuat(quaternion);
		if (box)
		{
			wreck_model_add_box(reading->model, placed_point(reading, numbers), quaternion, (b3Vec3){ numbers[7], numbers[8], numbers[9] },
				(wreck_material_t)material, 0xffffffffu);
		}
		else
		{
			wreck_model_add_log(reading->model, placed_point(reading, numbers), quaternion, numbers[7], numbers[8], (wreck_material_t)material,
				0xffffffffu);
		}
		return true;
	}
	if (SDL_strcmp(section, "links") == 0 && SDL_strcmp(key, "link") == 0)
	{
		float numbers[5];
		int piece_a = SDL_atoi(word);
		int piece_b;
		int type = -1;
		char type_name[32];
		size_t type_length = 0;

		// piece_b, then the type's name, then the point
		piece_b = SDL_atoi(rest);
		while (*rest != '\0' && *rest != ',')
		{
			rest++;
		}
		while (*rest == ' ' || *rest == '\t' || *rest == ',')
		{
			rest++;
		}
		while (rest[type_length] != '\0' && rest[type_length] != ',' && rest[type_length] != ' ' && type_length + 1 < sizeof(type_name))
		{
			type_name[type_length] = rest[type_length];
			type_length++;
		}
		type_name[type_length] = '\0';
		rest += type_length;
		while (*rest == ' ' || *rest == '\t' || *rest == ',')
		{
			rest++;
		}
		for (int t = 0; t < WRECK_LINK_TYPE_COUNT; t++)
		{
			if (SDL_strcasecmp(type_name, LINK_NAMES[t]) == 0)
			{
				type = t;
			}
		}
		if (type < 0 || read_numbers(rest, numbers, 3) != 3 || piece_a < 0 || piece_a >= reading->model->piece_count
			|| piece_b < WRECK_GROUND || piece_b >= reading->model->piece_count)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Wrecks: %s: a link I cannot read: '%s'", reading->name, value);
			reading->failed = true;
			return true;
		}
		wreck_model_add_link(reading->model, piece_a, piece_b, placed_point(reading, numbers), (wreck_link_type_t)type);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// The buildings

static void take_objects(wrecks_building_t *building, objects_t *objects)
{
	for (int p = 0; p < WRECK_PIECES_MAX; p++)
	{
		if (building->piece_objects[p] != 0)
		{
			int index = objects_find_id(objects, building->piece_objects[p]);

			if (index >= 0)
			{
				objects_kill(objects, index);
			}
			building->piece_objects[p] = 0;
		}
	}
}

static void free_building(wrecks_building_t *building, objects_t *objects)
{
	if (objects != NULL)
	{
		take_objects(building, objects);
	}
	if (building->model != NULL)
	{
		wreck_model_destroy(building->model);
		SDL_free(building->model);
	}
	SDL_zerop(building);
}

void wrecks_place(wrecks_t *wrecks, b3WorldId world, objects_t *objects, const terrain_t *terrain, const char *name, float x, float z,
	float yaw_radians)
{
	wrecks_building_t *building = NULL;
	reading_t reading;
	char path[160];

	for (int b = 0; b < WRECKS_MAX; b++)
	{
		if (!wrecks->buildings[b].used)
		{
			building = &wrecks->buildings[b];
			break;
		}
	}
	if (building == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Wrecks: %d buildings already, no room for %s", WRECKS_MAX, name);
		return;
	}
	SDL_zerop(building);
	building->model = SDL_calloc(1, sizeof(wreck_model_t));
	if (building->model == NULL)
	{
		return;
	}
	SDL_strlcpy(building->name, name, sizeof(building->name));
	wreck_model_create(building->model, world, &wrecks->settings);
	SDL_zero(reading);
	reading.wrecks = wrecks;
	reading.model = building->model;
	reading.name = name;
	reading.origin[0] = x;
	reading.origin[1] = terrain_sample_height_meters(terrain, x, z);
	reading.origin[2] = z;
	reading.turn[1] = SDL_sinf(0.5f * yaw_radians);
	reading.turn[3] = SDL_cosf(0.5f * yaw_radians);
	SDL_snprintf(path, sizeof(path), "wrecks/%s.wreck", name);
	if (!ini_parse_data_file(path, read_wreck_key, &reading) || building->model->piece_count == 0)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Wrecks: no data/%s, or nothing in it", path);
		free_building(building, NULL);
		return;
	}
	wreck_model_finish(building->model);
	building->used = true;
	SDL_Log("Wrecks: %s placed at (%.1f, %.1f, %.1f): %d pieces, %d links%s", name, (double)x, (double)reading.origin[1], (double)z,
		building->model->piece_count, building->model->link_count, reading.failed ? " (some lines could not be read, see above)" : "");
	wrecks_update_objects(wrecks, objects, terrain);
}

void wrecks_clear(wrecks_t *wrecks, objects_t *objects)
{
	for (int b = 0; b < WRECKS_MAX; b++)
	{
		if (wrecks->buildings[b].used)
		{
			free_building(&wrecks->buildings[b], objects);
		}
	}
}

int wrecks_count(const wrecks_t *wrecks)
{
	int count = 0;

	for (int b = 0; b < WRECKS_MAX; b++)
	{
		count += wrecks->buildings[b].used ? 1 : 0;
	}
	return count;
}

void wrecks_before_step(wrecks_t *wrecks)
{
	for (int b = 0; b < WRECKS_MAX; b++)
	{
		if (wrecks->buildings[b].used)
		{
			wreck_model_before_step(wrecks->buildings[b].model);
		}
	}
}

void wrecks_take_hits(wrecks_t *wrecks, b3WorldId world)
{
	b3ContactEvents events;

	if (wrecks_count(wrecks) == 0)
	{
		return;
	}
	events = b3World_GetContactEvents(world);
	for (int e = 0; e < events.hitCount; e++)
	{
		const b3ContactHitEvent *event = &events.hitEvents[e];
		wreck_model_t *model_a = wreck_shape_model(event->shapeIdA);
		wreck_model_t *model_b = wreck_shape_model(event->shapeIdB);

		if (model_a != NULL)
		{
			wreck_model_take_hit(model_a, event, wrecks->frame_index);
		}
		if (model_b != NULL && model_b != model_a)
		{
			wreck_model_take_hit(model_b, event, wrecks->frame_index);
		}
	}
}

void wrecks_after_step(wrecks_t *wrecks, float step_seconds)
{
	for (int b = 0; b < WRECKS_MAX; b++)
	{
		if (wrecks->buildings[b].used && wreck_model_is_busy(wrecks->buildings[b].model))
		{
			wreck_model_after_step(wrecks->buildings[b].model, wrecks->frame_index, step_seconds);
		}
	}
	wrecks->frame_index++;
}

// A place that is a number (a piece thrown by a solve gone wrong would
// put an object nowhere: the objects' grid does not take that)
static bool finite_pose(const b3Transform *transform)
{
	const float numbers[7] = { transform->p.x, transform->p.y, transform->p.z, transform->q.v.x, transform->q.v.y, transform->q.v.z, transform->q.s };

	for (int k = 0; k < 7; k++)
	{
		if (SDL_isnan(numbers[k]) || SDL_isinf(numbers[k]))
		{
			return false;
		}
	}
	return true;
}

void wrecks_update_objects(wrecks_t *wrecks, objects_t *objects, const terrain_t *terrain)
{
	float map_meters = (float)terrain->size * terrain->cell_size_meters;

	for (int b = 0; b < WRECKS_MAX; b++)
	{
		wrecks_building_t *building = &wrecks->buildings[b];
		wreck_model_t *model = building->model;

		if (!building->used)
		{
			continue;
		}
		for (int p = 0; p < model->piece_count; p++)
		{
			const wreck_piece_t *piece = &model->pieces[p];
			int index;
			b3Transform transform;
			object_instance_t *instance;
			bool moving;
			bool moving_forced = false;

			if (building->piece_gone[p])
			{
				continue;
			}
			// Gone (crushed, broken by hits): its object with it
			if (piece->group == WRECK_GONE || piece->crushed)
			{
				if (building->piece_objects[p] != 0 && (index = objects_find_id(objects, building->piece_objects[p])) >= 0)
				{
					objects_kill(objects, index);
				}
				building->piece_objects[p] = 0;
				building->piece_gone[p] = true;
				building->piece_moving[p] = false;
				continue;
			}
			transform = wreck_model_piece_transform(model, p);
			index = building->piece_objects[p] != 0 ? objects_find_id(objects, building->piece_objects[p]) : -1;
			{
				float size[3] = { piece->shape == WRECK_PIECE_LOG ? piece->radius : piece->half_extents.x,
					piece->shape == WRECK_PIECE_LOG ? piece->half_length : piece->half_extents.y, piece->shape == WRECK_PIECE_LOG ? 0.0f : piece->half_extents.z };
				bool resized = SDL_memcmp(size, building->piece_sizes[p], sizeof(size)) != 0;

				SDL_memcpy(building->piece_sizes[p], size, sizeof(size));
				// Cut in two: the part that kept its number is shorter, and
				// somewhere else in its body (its object of its new size)
				if (resized && index >= 0)
				{
					int type = piece_type(wrecks, objects, piece);

					if (type >= 0)
					{
						objects_retype(objects, index, type);
					}
					moving_forced = true;
				}
			}
			// New (the building just placed, or a piece cut in two): an
			// object for it, of its size's procedural type
			if (index < 0)
			{
				int type = piece_type(wrecks, objects, piece);

				if (type < 0 || !finite_pose(&transform) || transform.p.x < 0.0f || transform.p.z < 0.0f || transform.p.x >= map_meters || transform.p.z >= map_meters
					|| !objects_add(objects, terrain, type, transform.p.x, transform.p.z, 0.0f, 1.0f, -1))
				{
					building->piece_gone[p] = true;
					continue;
				}
				index = objects->instance_count - 1;
				building->piece_objects[p] = objects->instances[index].id;
				objects_set_free(objects, index, true);
				// (where it stands, at once)
				objects->instances[index].x = transform.p.x;
				objects->instances[index].y = transform.p.y;
				objects->instances[index].z = transform.p.z;
				objects->instances[index].rotation[0] = transform.q.v.x;
				objects->instances[index].rotation[1] = transform.q.v.y;
				objects->instances[index].rotation[2] = transform.q.v.z;
				objects->instances[index].rotation[3] = transform.q.s;
				objects_touch(objects, index);
				building->piece_moving[p] = false;
				{
					float pose[7] = { transform.p.x, transform.p.y, transform.p.z, transform.q.v.x, transform.q.v.y, transform.q.v.z, transform.q.s };

					SDL_memcpy(building->pose_to[p], pose, sizeof(pose));
				}
				moving = false;
				{
					const wreck_group_t *group = &model->groups[piece->group];

					moving = !group->anchored && b3Body_IsValid(group->body) && b3Body_IsAwake(group->body);
				}
			}
			else
			{
				const wreck_group_t *group = &model->groups[piece->group];

				// (only what the physics moves: a standing building stays put)
				moving = moving_forced || (!group->anchored && b3Body_IsValid(group->body) && b3Body_IsAwake(group->body));
			}
			instance = &objects->instances[index];
			if (!moving)
			{
				// (come to rest: drawn where it lies, no longer between steps)
				if (building->piece_moving[p])
				{
					building->piece_moving[p] = false;
					instance->x = building->pose_to[p][0];
					instance->y = building->pose_to[p][1];
					instance->z = building->pose_to[p][2];
					SDL_memcpy(instance->rotation, &building->pose_to[p][3], sizeof(float) * 4);
					objects_touch(objects, index);
					objects_note_moved(objects, index);
				}
				continue;
			}
			// Off the map, or thrown to nowhere: gone
			if (finite_pose(&transform) && transform.p.x >= 0.0f && transform.p.z >= 0.0f && transform.p.x < map_meters && transform.p.z < map_meters
				&& transform.p.y > -1000.0f && transform.p.y < 10000.0f)
			{
				float pose[7] = { transform.p.x, transform.p.y, transform.p.z, transform.q.v.x, transform.q.v.y, transform.q.v.z, transform.q.s };

				// (from where it is drawn now if it was still, to where it is)
				if (!building->piece_moving[p])
				{
					building->pose_from[p][0] = instance->x;
					building->pose_from[p][1] = instance->y;
					building->pose_from[p][2] = instance->z;
					SDL_memcpy(&building->pose_from[p][3], instance->rotation, sizeof(float) * 4);
				}
				else
				{
					SDL_memcpy(building->pose_from[p], building->pose_to[p], sizeof(pose));
				}
				SDL_memcpy(building->pose_to[p], pose, sizeof(pose));
				building->piece_moving[p] = true;
				continue;
			}
			objects_kill(objects, index);
			building->piece_objects[p] = 0;
			building->piece_gone[p] = true;
			building->piece_moving[p] = false;
		}
	}
}

void wrecks_interpolate(wrecks_t *wrecks, objects_t *objects, float blend)
{
	blend = SDL_clamp(blend, 0.0f, 1.0f);
	for (int b = 0; b < WRECKS_MAX; b++)
	{
		wrecks_building_t *building = &wrecks->buildings[b];

		if (!building->used)
		{
			continue;
		}
		for (int p = 0; p < building->model->piece_count; p++)
		{
			const float *from = building->pose_from[p];
			const float *to = building->pose_to[p];
			object_instance_t *instance;
			float sign;
			float length = 0.0f;
			int index;

			if (!building->piece_moving[p] || building->piece_objects[p] == 0
				|| (index = objects_find_id(objects, building->piece_objects[p])) < 0)
			{
				continue;
			}
			instance = &objects->instances[index];
			instance->x = from[0] + (to[0] - from[0]) * blend;
			instance->y = from[1] + (to[1] - from[1]) * blend;
			instance->z = from[2] + (to[2] - from[2]) * blend;
			// (the shorter way round, normalized)
			sign = from[3] * to[3] + from[4] * to[4] + from[5] * to[5] + from[6] * to[6] < 0.0f ? -1.0f : 1.0f;
			for (int k = 0; k < 4; k++)
			{
				instance->rotation[k] = from[3 + k] + (sign * to[3 + k] - from[3 + k]) * blend;
				length += instance->rotation[k] * instance->rotation[k];
			}
			length = SDL_max(SDL_sqrtf(length), 1e-6f);
			for (int k = 0; k < 4; k++)
			{
				instance->rotation[k] /= length;
			}
			objects_touch(objects, index);
			objects_note_moved(objects, index);
		}
	}
}

void wrecks_blast(wrecks_t *wrecks, const float position[3], float kilograms)
{
	if (kilograms <= 0.0f)
	{
		return;
	}
	for (int b = 0; b < WRECKS_MAX; b++)
	{
		if (wrecks->buildings[b].used)
		{
			wreck_model_blast_kilograms(wrecks->buildings[b].model, (b3Vec3){ position[0], position[1], position[2] }, kilograms,
				wrecks->frame_index);
		}
	}
}

void wrecks_add_projectile(wrecks_t *wrecks, b3BodyId body, float radius)
{
	for (int b = 0; b < WRECKS_MAX; b++)
	{
		if (wrecks->buildings[b].used)
		{
			wreck_model_add_projectile(wrecks->buildings[b].model, body, radius);
		}
	}
}

void wrecks_remove_projectile(wrecks_t *wrecks, b3BodyId body)
{
	for (int b = 0; b < WRECKS_MAX; b++)
	{
		if (wrecks->buildings[b].used)
		{
			wreck_model_remove_projectile(wrecks->buildings[b].model, body);
		}
	}
}
