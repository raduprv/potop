// Modules in memory, read and written. See module_file.h.

#include "module_file.h"

#include <stdlib.h>
#include <string.h>

static void copy_name(char *out, size_t size, const char *name)
{
	size_t length = name != NULL ? strlen(name) : 0;

	memset(out, 0, size);
	if (length >= size)
	{
		length = size - 1;
	}
	if (length > 0)
	{
		memcpy(out, name, length);
	}
}

void module_file_clear(module_file_t *module)
{
	memset(module, 0, sizeof(*module));
}

int module_file_add_part(module_file_t *module, const char *name, const char *model, const float position[3], float yaw_degrees,
	uint32_t flags)
{
	module_file_part_t *part;
	int index = module->part_count;

	if (index >= MODULE_FILE_MAX_PARTS)
	{
		return -1;
	}
	part = &module->parts[index];
	memset(part, 0, sizeof(*part));
	copy_name(part->name, sizeof(part->name), name);
	copy_name(part->model, sizeof(part->model), model);
	for (int k = 0; k < 3; k++)
	{
		part->position[k] = position != NULL ? position[k] : 0.0f;
	}
	part->yaw_degrees = yaw_degrees;
	part->parent = -1;
	part->flags = flags;
	for (int s = 0; s < MODULE_FILE_MAX_STATES; s++)
	{
		memset(&module->poses[s][index], 0, sizeof(module_file_pose_t));
	}
	module->part_count++;
	return index;
}

int module_file_add_state(module_file_t *module, const char *name, float seconds)
{
	module_file_state_t *state;
	int index = module->state_count;

	if (index >= MODULE_FILE_MAX_STATES)
	{
		return -1;
	}
	state = &module->states[index];
	memset(state, 0, sizeof(*state));
	copy_name(state->name, sizeof(state->name), name);
	state->seconds = seconds;
	state->next = -1;
	if (index > 0)
	{
		memcpy(module->poses[index], module->poses[0], sizeof(module->poses[0]));
	}
	module->state_count++;
	return index;
}

int module_file_find_part(const module_file_t *module, const char *name)
{
	for (int p = 0; p < module->part_count; p++)
	{
		if (strncmp(module->parts[p].name, name, MODULE_FILE_NAME_LENGTH) == 0)
		{
			return p;
		}
	}
	return -1;
}

int module_file_find_state(const module_file_t *module, const char *name)
{
	for (int s = 0; s < module->state_count; s++)
	{
		if (strncmp(module->states[s].name, name, MODULE_FILE_NAME_LENGTH) == 0)
		{
			return s;
		}
	}
	return -1;
}

static bool fail(char *error, size_t error_size, const char *why)
{
	if (error != NULL && error_size > 0)
	{
		copy_name(error, error_size, why);
	}
	return false;
}

bool module_file_parse(const void *data, size_t size, module_file_t *out, char *error, size_t error_size)
{
	const unsigned char *bytes = data;
	module_file_header_t header;
	size_t needed;
	size_t at;

	module_file_clear(out);
	if (data == NULL || size < sizeof(header))
	{
		return fail(error, error_size, "too short for a module");
	}
	memcpy(&header, bytes, sizeof(header));
	if (memcmp(header.magic, MODULE_FILE_MAGIC, 4) != 0)
	{
		return fail(error, error_size, "not a module (no PMOD at its start)");
	}
	if (header.version != MODULE_FILE_VERSION)
	{
		return fail(error, error_size, "a module version this code does not read");
	}
	if (header.part_count < 1 || header.part_count > MODULE_FILE_MAX_PARTS || header.state_count < 1
		|| header.state_count > MODULE_FILE_MAX_STATES || header.first_state >= header.state_count)
	{
		return fail(error, error_size, "a module with no parts, no states or too many");
	}
	needed = sizeof(header) + sizeof(module_file_part_t) * header.part_count + sizeof(module_file_state_t) * header.state_count
		+ sizeof(module_file_pose_t) * header.part_count * header.state_count;
	if (size < needed)
	{
		return fail(error, error_size, "a module cut short");
	}
	out->part_count = (int)header.part_count;
	out->state_count = (int)header.state_count;
	out->first_state = (int)header.first_state;
	at = sizeof(header);
	memcpy(out->parts, bytes + at, sizeof(module_file_part_t) * header.part_count);
	at += sizeof(module_file_part_t) * header.part_count;
	memcpy(out->states, bytes + at, sizeof(module_file_state_t) * header.state_count);
	at += sizeof(module_file_state_t) * header.state_count;
	for (uint32_t s = 0; s < header.state_count; s++)
	{
		memcpy(out->poses[s], bytes + at, sizeof(module_file_pose_t) * header.part_count);
		at += sizeof(module_file_pose_t) * header.part_count;
	}
	// (names always end; a next that is not a state: none)
	for (int p = 0; p < out->part_count; p++)
	{
		out->parts[p].name[MODULE_FILE_NAME_LENGTH - 1] = '\0';
		out->parts[p].model[MODULE_FILE_MODEL_LENGTH - 1] = '\0';
	}
	for (int s = 0; s < out->state_count; s++)
	{
		out->states[s].name[MODULE_FILE_NAME_LENGTH - 1] = '\0';
		if (out->states[s].next < -1 || out->states[s].next >= out->state_count)
		{
			out->states[s].next = -1;
		}
	}
	return true;
}

void *module_file_serialize(const module_file_t *module, size_t *size_out)
{
	module_file_header_t header;
	unsigned char *bytes;
	size_t size;
	size_t at;

	*size_out = 0;
	if (module->part_count < 1 || module->part_count > MODULE_FILE_MAX_PARTS || module->state_count < 1
		|| module->state_count > MODULE_FILE_MAX_STATES || module->first_state < 0 || module->first_state >= module->state_count)
	{
		return NULL;
	}
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, MODULE_FILE_MAGIC, 4);
	header.version = MODULE_FILE_VERSION;
	header.part_count = (uint32_t)module->part_count;
	header.state_count = (uint32_t)module->state_count;
	header.first_state = (uint32_t)module->first_state;
	size = sizeof(header) + sizeof(module_file_part_t) * header.part_count + sizeof(module_file_state_t) * header.state_count
		+ sizeof(module_file_pose_t) * header.part_count * header.state_count;
	bytes = malloc(size);
	if (bytes == NULL)
	{
		return NULL;
	}
	memcpy(bytes, &header, sizeof(header));
	at = sizeof(header);
	memcpy(bytes + at, module->parts, sizeof(module_file_part_t) * header.part_count);
	at += sizeof(module_file_part_t) * header.part_count;
	memcpy(bytes + at, module->states, sizeof(module_file_state_t) * header.state_count);
	at += sizeof(module_file_state_t) * header.state_count;
	for (uint32_t s = 0; s < header.state_count; s++)
	{
		memcpy(bytes + at, module->poses[s], sizeof(module_file_pose_t) * header.part_count);
		at += sizeof(module_file_pose_t) * header.part_count;
	}
	*size_out = size;
	return bytes;
}

bool module_file_write(const char *path, const module_file_t *module)
{
	size_t size;
	void *bytes = module_file_serialize(module, &size);
	FILE *file;
	bool ok;

	if (bytes == NULL)
	{
		return false;
	}
	file = fopen(path, "wb");
	if (file == NULL)
	{
		free(bytes);
		return false;
	}
	ok = fwrite(bytes, 1, size, file) == size;
	ok = fclose(file) == 0 && ok;
	free(bytes);
	return ok;
}

bool module_file_read(const char *path, module_file_t *out, char *error, size_t error_size)
{
	FILE *file = fopen(path, "rb");
	unsigned char *bytes;
	long size;
	bool ok;

	if (file == NULL)
	{
		module_file_clear(out);
		return fail(error, error_size, "cannot open it");
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size <= 0)
	{
		fclose(file);
		module_file_clear(out);
		return fail(error, error_size, "empty");
	}
	bytes = malloc((size_t)size);
	if (bytes == NULL)
	{
		fclose(file);
		module_file_clear(out);
		return fail(error, error_size, "out of memory");
	}
	ok = fread(bytes, 1, (size_t)size, file) == (size_t)size;
	fclose(file);
	ok = ok && module_file_parse(bytes, (size_t)size, out, error, error_size);
	free(bytes);
	return ok;
}

void module_file_dump(const module_file_t *module, FILE *stream)
{
	static const char *const part_flags[] = { "collision", "shadow", "moves", "impostor" };

	fprintf(stream, "%d parts, %d states, placed in '%s'\n", module->part_count, module->state_count,
		module->first_state >= 0 && module->first_state < module->state_count ? module->states[module->first_state].name : "?");
	for (int p = 0; p < module->part_count; p++)
	{
		const module_file_part_t *part = &module->parts[p];

		fprintf(stream, "part %d '%s': model '%s' at (%.3f, %.3f, %.3f), yaw %.1f, axis (%.4f, %.4f, %.4f), %.2f rpm,", p, part->name,
			part->model, (double)part->position[0], (double)part->position[1], (double)part->position[2], (double)part->yaw_degrees,
			(double)part->axis[0], (double)part->axis[1], (double)part->axis[2], (double)part->spin_rpm);
		for (int f = 0; f < 4; f++)
		{
			if ((part->flags & (1u << f)) != 0)
			{
				fprintf(stream, " %s", part_flags[f]);
			}
		}
		fprintf(stream, "\n");
	}
	for (int s = 0; s < module->state_count; s++)
	{
		const module_file_state_t *state = &module->states[s];

		fprintf(stream, "state %d '%s': %.2f s to get there", s, state->name, (double)state->seconds);
		if (state->next >= 0)
		{
			fprintf(stream, ", after %.2f s on to '%s'", (double)state->hold_seconds, module->states[state->next].name);
		}
		fprintf(stream, "\n");
		for (int p = 0; p < module->part_count; p++)
		{
			const module_file_pose_t *pose = &module->poses[s][p];

			if (pose->offset[0] == 0.0f && pose->offset[1] == 0.0f && pose->offset[2] == 0.0f && pose->angle_degrees == 0.0f && pose->spin == 0.0f
				&& pose->flags == 0u)
			{
				continue;
			}
			fprintf(stream, "  %-12s offset (%.3f, %.3f, %.3f), angle %.1f, spin %.2f%s\n", module->parts[p].name, (double)pose->offset[0],
				(double)pose->offset[1], (double)pose->offset[2], (double)pose->angle_degrees, (double)pose->spin,
				(pose->flags & MODULE_POSE_HIDDEN) != 0 ? ", hidden" : "");
		}
	}
}
