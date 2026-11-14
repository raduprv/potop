// A small OpenVDB reader: see vdb_read.h.
//
// The file, as far as this reader goes (all little endian):
//   header       magic 0x56444220 (int64), format version (uint32), library
//                version (2 x uint32), whether there are grid offsets (char),
//                a UUID (36 chars), the file's metadata, the grid count
//   per grid     its name, its tree type, its instance parent (format 216+),
//                where the grid, its buffers and its end are (3 x int64)
//   at the grid  compression flags (uint32, format 222+), the grid's
//                metadata, its transform (a map's name and numbers), then the
//                tree's topology, then the leaves' values
//   the tree     a root (background, tiles, children) over internal nodes of
//                32^3 children, over internal nodes of 16^3, over leaves of
//                8^3 voxels; each node keeps bit masks of which children and
//                which values it has, and its values (as floats or halves)
//                behind a byte saying which of them are written
//
// A metadata entry is a name, a type name and a value, each as a uint32
// length and that many bytes.

#include "vdb_read.h"

#include "miniz.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VDB_MAGIC 0x56444220
#define VDB_OLDEST_VERSION 222
// The compression flags
#define VDB_COMPRESS_ZIP 0x1
#define VDB_COMPRESS_ACTIVE_MASK 0x2
#define VDB_COMPRESS_BLOSC 0x4
// What a node's value block starts with: which values are written
#define VDB_NO_MASK_OR_INACTIVE_VALUES 0
#define VDB_NO_MASK_AND_MINUS_BACKGROUND 1
#define VDB_NO_MASK_AND_ONE_INACTIVE_VALUE 2
#define VDB_MASK_AND_NO_INACTIVE_VALUES 3
#define VDB_MASK_AND_ONE_INACTIVE_VALUE 4
#define VDB_MASK_AND_TWO_INACTIVE_VALUES 5
#define VDB_NO_MASK_AND_ALL_VALUES 6
// The 5-4-3 tree: each level's log2 of children a side, and how many voxels
// a side its nodes span
#define VDB_UPPER_LOG2 5
#define VDB_LOWER_LOG2 4
#define VDB_LEAF_LOG2 3
#define VDB_LEAF_SPAN 8
#define VDB_LOWER_SPAN (16 * VDB_LEAF_SPAN)
#define VDB_UPPER_SPAN (32 * VDB_LOWER_SPAN)
#define VDB_LEAF_VALUES 512

typedef struct
{
	const uint8_t *data;
	size_t size;
	size_t at;
	bool failed;
	char *error;
	size_t error_size;
	uint32_t version;
	uint32_t compression;
	bool half;
	float background;
	uint8_t *unpacked;    // a compressed block's bytes, unpacked
	size_t unpacked_capacity;
} vdb_reader_t;

// A part of the volume with one value: an active tile of an internal node
// (or of the root)
typedef struct
{
	int origin[3];
	int span;
	float value;
} vdb_tile_t;

typedef struct
{
	int (*leaf_origins)[3];
	size_t leaf_count;
	size_t leaf_capacity;
	vdb_tile_t *tiles;
	size_t tile_count;
	size_t tile_capacity;
	int low[3];
	int high[3];          // past the end
	bool any;
} vdb_topology_t;

static void fail(vdb_reader_t *reader, const char *format, ...)
{
	va_list arguments;

	if (reader->failed)
	{
		return;
	}
	reader->failed = true;
	va_start(arguments, format);
	vsnprintf(reader->error, reader->error_size, format, arguments);
	va_end(arguments);
}

static const uint8_t *take(vdb_reader_t *reader, size_t count)
{
	const uint8_t *bytes;

	if (reader->failed || count > reader->size - reader->at)
	{
		fail(reader, "the file ends too soon (at byte %zu, %zu more wanted)", reader->at, count);
		return NULL;
	}
	bytes = reader->data + reader->at;
	reader->at += count;
	return bytes;
}

static uint32_t read_uint32(vdb_reader_t *reader)
{
	const uint8_t *bytes = take(reader, 4);
	uint32_t value = 0;

	if (bytes != NULL)
	{
		memcpy(&value, bytes, 4);
	}
	return value;
}

static int32_t read_int32(vdb_reader_t *reader)
{
	return (int32_t)read_uint32(reader);
}

static int64_t read_int64(vdb_reader_t *reader)
{
	const uint8_t *bytes = take(reader, 8);
	int64_t value = 0;

	if (bytes != NULL)
	{
		memcpy(&value, bytes, 8);
	}
	return value;
}

static float read_float(vdb_reader_t *reader)
{
	const uint8_t *bytes = take(reader, 4);
	float value = 0.0f;

	if (bytes != NULL)
	{
		memcpy(&value, bytes, 4);
	}
	return value;
}

static double read_double(vdb_reader_t *reader)
{
	const uint8_t *bytes = take(reader, 8);
	double value = 0.0;

	if (bytes != NULL)
	{
		memcpy(&value, bytes, 8);
	}
	return value;
}

// A length and that many characters, into text (cut to fit); the length
static uint32_t read_string(vdb_reader_t *reader, char *text, size_t text_size)
{
	uint32_t length = read_uint32(reader);
	const uint8_t *bytes = take(reader, length);

	if (text_size > 0)
	{
		size_t kept = length < text_size - 1 ? length : text_size - 1;

		text[0] = '\0';
		if (bytes != NULL)
		{
			memcpy(text, bytes, kept);
			text[kept] = '\0';
		}
	}
	return length;
}

// Metadata: only the count matters here, the entries are skipped
static void skip_metadata(vdb_reader_t *reader)
{
	uint32_t count = read_uint32(reader);

	for (uint32_t m = 0; m < count && !reader->failed; m++)
	{
		read_string(reader, NULL, 0);
		read_string(reader, NULL, 0);
		read_string(reader, NULL, 0);
	}
}

// ---------------------------------------------------------------------------
// Compressed values: each block is an int64 of how many bytes follow (zero
// or less: that many, not compressed), then zlib's or Blosc's bytes

// An LZ4 block (no frame): runs of literals, each followed by a copy of
// earlier output; false if it does not fit out exactly
static bool lz4_unpack(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size)
{
	const uint8_t *in_end = in + in_size;
	size_t written = 0;

	while (in < in_end)
	{
		uint8_t token = *in++;
		size_t literals = token >> 4;
		size_t match;
		size_t offset;

		if (literals == 15)
		{
			uint8_t more;

			do
			{
				if (in >= in_end)
				{
					return false;
				}
				more = *in++;
				literals += more;
			} while (more == 255);
		}
		if (literals > (size_t)(in_end - in) || literals > out_size - written)
		{
			return false;
		}
		memcpy(out + written, in, literals);
		in += literals;
		written += literals;
		if (in >= in_end)
		{
			// The last run has no copy after it
			break;
		}
		if (in_end - in < 2)
		{
			return false;
		}
		offset = (size_t)in[0] | ((size_t)in[1] << 8);
		in += 2;
		match = token & 15;
		if (match == 15)
		{
			uint8_t more;

			do
			{
				if (in >= in_end)
				{
					return false;
				}
				more = *in++;
				match += more;
			} while (more == 255);
		}
		match += 4;
		if (offset == 0 || offset > written || match > out_size - written)
		{
			return false;
		}
		// Byte by byte: the copy may overlap what it writes
		for (size_t b = 0; b < match; b++)
		{
			out[written + b] = out[written - offset + b];
		}
		written += match;
	}
	return written == out_size;
}

// BloscLZ (Blosc's own LZ77): a control byte, below 32 a run of that many
// plus one literals, else a copy (length in its top bits, distance below)
static bool blosclz_unpack(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size)
{
	const uint8_t *in_end = in + in_size;
	size_t written = 0;
	uint32_t control;

	if (in_size == 0)
	{
		return out_size == 0;
	}
	control = *in++ & 31;
	for (;;)
	{
		if (control >= 32)
		{
			size_t length = (control >> 5) - 1;
			size_t distance = (control & 31) << 8;
			uint8_t code;

			if (length == 6)
			{
				do
				{
					if (in >= in_end)
					{
						return false;
					}
					code = *in++;
					length += code;
				} while (code == 255);
			}
			if (in >= in_end)
			{
				return false;
			}
			code = *in++;
			distance += code;
			if (code == 255 && distance == (31u << 8) + 255u)
			{
				// A far distance: two more bytes
				if (in_end - in < 2)
				{
					return false;
				}
				distance = ((size_t)in[0] << 8) + in[1] + 8191u;
				in += 2;
			}
			distance += 1;
			length += 3;
			if (distance > written || length > out_size - written)
			{
				return false;
			}
			for (size_t b = 0; b < length; b++)
			{
				out[written + b] = out[written - distance + b];
			}
			written += length;
		}
		else
		{
			size_t literals = control + 1;

			if (literals > (size_t)(in_end - in) || literals > out_size - written)
			{
				return false;
			}
			memcpy(out + written, in, literals);
			in += literals;
			written += literals;
		}
		if (in >= in_end)
		{
			break;
		}
		control = *in++;
	}
	return written == out_size;
}

// A Blosc (version 1) buffer: a 16 byte header (versions, flags, the type's
// size, the bytes it holds, its blocks' size, its own size), the blocks'
// starts, then each block, split into a stream for each byte of the type
// and compressed, the bytes shuffled (all the values' first bytes, then
// their second ...)
static bool blosc_unpack(vdb_reader_t *reader, const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size)
{
	uint8_t flags;
	size_t type_size;
	size_t total;
	size_t block_size;
	size_t block_count;
	int codec;
	bool shuffled;
	uint8_t *block_bytes;

	if (in_size < 16)
	{
		fail(reader, "a Blosc block is too short");
		return false;
	}
	flags = in[2];
	type_size = in[3] > 0 ? in[3] : 1;
	total = (size_t)in[4] | ((size_t)in[5] << 8) | ((size_t)in[6] << 16) | ((size_t)in[7] << 24);
	block_size = (size_t)in[8] | ((size_t)in[9] << 8) | ((size_t)in[10] << 16) | ((size_t)in[11] << 24);
	codec = (flags >> 5) & 7;
	shuffled = (flags & 0x1) != 0 && type_size > 1;
	if (total != out_size)
	{
		fail(reader, "a Blosc block holds %zu bytes, %zu expected", total, out_size);
		return false;
	}
	if ((flags & 0x2) != 0)
	{
		// Stored as it was
		if (in_size < 16 + total)
		{
			fail(reader, "a Blosc block is too short");
			return false;
		}
		memcpy(out, in + 16, total);
		return true;
	}
	if ((flags & 0x4) != 0)
	{
		fail(reader, "Blosc's bit shuffle is not read: save the file with zip or uncompressed");
		return false;
	}
	if (codec != 0 && codec != 1 && codec != 3)
	{
		fail(reader, "Blosc with codec %d is not read (BloscLZ, LZ4 and zlib are)", codec);
		return false;
	}
	if (block_size == 0)
	{
		fail(reader, "a Blosc block of blocks of no size");
		return false;
	}
	block_count = (total + block_size - 1) / block_size;
	if (in_size < 16 + block_count * 4)
	{
		fail(reader, "a Blosc block is too short");
		return false;
	}
	block_bytes = malloc(block_size);
	for (size_t b = 0; b < block_count; b++)
	{
		size_t start = (size_t)in[16 + b * 4] | ((size_t)in[17 + b * 4] << 8) | ((size_t)in[18 + b * 4] << 16) | ((size_t)in[19 + b * 4] << 24);
		size_t this_size = b == block_count - 1 && total % block_size != 0 ? total % block_size : block_size;
		bool leftover = this_size != block_size;
		bool split = (flags & 0x10) == 0 && type_size <= 16 && this_size / type_size >= 128 && !leftover;
		size_t streams = split ? type_size : 1;
		size_t stream_size = this_size / streams;
		uint8_t *target = shuffled ? block_bytes : out + b * block_size;
		size_t at = start;

		for (size_t stream = 0; stream < streams; stream++)
		{
			size_t packed;
			bool ok;

			if (at + 4 > in_size)
			{
				free(block_bytes);
				fail(reader, "a Blosc block is too short");
				return false;
			}
			packed = (size_t)in[at] | ((size_t)in[at + 1] << 8) | ((size_t)in[at + 2] << 16) | ((size_t)in[at + 3] << 24);
			at += 4;
			if (packed > in_size - at)
			{
				free(block_bytes);
				fail(reader, "a Blosc block is too short");
				return false;
			}
			if (packed == stream_size)
			{
				memcpy(target + stream * stream_size, in + at, packed);
				ok = true;
			}
			else if (codec == 0)
			{
				ok = blosclz_unpack(in + at, packed, target + stream * stream_size, stream_size);
			}
			else if (codec == 1)
			{
				ok = lz4_unpack(in + at, packed, target + stream * stream_size, stream_size);
			}
			else
			{
				mz_ulong unpacked = (mz_ulong)stream_size;

				ok = mz_uncompress(target + stream * stream_size, &unpacked, in + at, (mz_ulong)packed) == MZ_OK && unpacked == stream_size;
			}
			if (!ok)
			{
				free(block_bytes);
				fail(reader, "a Blosc block does not unpack (at byte %zu)", reader->at);
				return false;
			}
			at += packed;
		}
		if (shuffled)
		{
			size_t values = this_size / type_size;
			uint8_t *destination = out + b * block_size;

			for (size_t v = 0; v < values; v++)
			{
				for (size_t byte = 0; byte < type_size; byte++)
				{
					destination[v * type_size + byte] = block_bytes[byte * values + v];
				}
			}
			// What does not make a whole value is not shuffled
			memcpy(destination + values * type_size, block_bytes + values * type_size, this_size - values * type_size);
		}
	}
	free(block_bytes);
	return true;
}

// count bytes of values, compressed as the grid says: a pointer to them (in
// the file, or unpacked into the reader's buffer)
static const uint8_t *read_data(vdb_reader_t *reader, size_t count)
{
	int64_t packed;
	const uint8_t *bytes;

	if ((reader->compression & (VDB_COMPRESS_ZIP | VDB_COMPRESS_BLOSC)) == 0)
	{
		return take(reader, count);
	}
	packed = read_int64(reader);
	if (packed <= 0)
	{
		// Too small to be worth it: stored as they are
		if ((size_t)(-packed) != count)
		{
			fail(reader, "a block of %lld bytes where %zu were expected", (long long)-packed, count);
			return NULL;
		}
		return take(reader, count);
	}
	bytes = take(reader, (size_t)packed);
	if (bytes == NULL)
	{
		return NULL;
	}
	if (count == 0)
	{
		return bytes;
	}
	if (reader->unpacked_capacity < count)
	{
		free(reader->unpacked);
		reader->unpacked_capacity = count * 2;
		reader->unpacked = malloc(reader->unpacked_capacity);
	}
	if ((reader->compression & VDB_COMPRESS_BLOSC) != 0)
	{
		if (!blosc_unpack(reader, bytes, (size_t)packed, reader->unpacked, count))
		{
			return NULL;
		}
	}
	else
	{
		mz_ulong unpacked = (mz_ulong)count;

		if (mz_uncompress(reader->unpacked, &unpacked, bytes, (mz_ulong)packed) != MZ_OK || unpacked != count)
		{
			fail(reader, "a zip block does not unpack (at byte %zu)", reader->at);
			return NULL;
		}
	}
	return reader->unpacked;
}

static float half_to_float(uint16_t half)
{
	uint32_t sign = (uint32_t)(half >> 15) << 31;
	int exponent = (half >> 10) & 0x1f;
	uint32_t mantissa = half & 0x3ffu;
	uint32_t bits;
	float value;

	if (exponent == 0)
	{
		// Zero or subnormal
		value = ldexpf((float)mantissa, -24);
		return sign != 0 ? -value : value;
	}
	if (exponent == 31)
	{
		bits = sign | 0x7f800000u | (mantissa << 13);
	}
	else
	{
		bits = sign | ((uint32_t)(exponent - 15 + 127) << 23) | (mantissa << 13);
	}
	memcpy(&value, &bits, 4);
	return value;
}

// A node's bit mask of size bits (whole uint64 words)
static const uint8_t *read_mask(vdb_reader_t *reader, int bits)
{
	return take(reader, (size_t)bits / 8);
}

static bool mask_on(const uint8_t *mask, int bit)
{
	return (mask[bit >> 3] >> (bit & 7)) & 1;
}

static int mask_count(const uint8_t *mask, int bits)
{
	int count = 0;

	for (int b = 0; b < bits; b++)
	{
		count += mask_on(mask, b) ? 1 : 0;
	}
	return count;
}

// A node's values (count of them, values_out may be NULL to skip them), as
// OpenVDB's readCompressedValues writes them; value_mask: the node's active
// values (count bits)
static void read_values(vdb_reader_t *reader, float *values_out, int count, const uint8_t *value_mask)
{
	uint8_t metadata = VDB_NO_MASK_AND_ALL_VALUES;
	float inactive_zero = reader->background;
	float inactive_one = reader->background;
	const uint8_t *selection = NULL;
	int stored = count;
	size_t value_bytes = reader->half ? 2 : 4;
	const uint8_t *bytes;

	{
		const uint8_t *flag = take(reader, 1);

		metadata = flag != NULL ? *flag : VDB_NO_MASK_AND_ALL_VALUES;
	}
	if (metadata > VDB_NO_MASK_AND_ALL_VALUES)
	{
		fail(reader, "a node's values start with an unknown flag %d (at byte %zu)", metadata, reader->at - 1);
		return;
	}
	if (metadata != VDB_NO_MASK_OR_INACTIVE_VALUES)
	{
		inactive_zero = -reader->background;
	}
	if (metadata == VDB_NO_MASK_AND_ONE_INACTIVE_VALUE || metadata == VDB_MASK_AND_ONE_INACTIVE_VALUE
		|| metadata == VDB_MASK_AND_TWO_INACTIVE_VALUES)
	{
		inactive_zero = read_float(reader);
		if (metadata == VDB_MASK_AND_TWO_INACTIVE_VALUES)
		{
			inactive_one = read_float(reader);
		}
	}
	if (metadata == VDB_MASK_AND_NO_INACTIVE_VALUES || metadata == VDB_MASK_AND_ONE_INACTIVE_VALUE
		|| metadata == VDB_MASK_AND_TWO_INACTIVE_VALUES)
	{
		selection = read_mask(reader, count);
	}
	if ((reader->compression & VDB_COMPRESS_ACTIVE_MASK) != 0 && metadata != VDB_NO_MASK_AND_ALL_VALUES)
	{
		// Only the active values are written
		stored = mask_count(value_mask, count);
	}
	// Even none have their size written, when compressed
	bytes = read_data(reader, (size_t)stored * value_bytes);
	if (values_out == NULL || (bytes == NULL && stored > 0))
	{
		return;
	}
	for (int v = 0, s = 0; v < count; v++)
	{
		if (stored == count || mask_on(value_mask, v))
		{
			if (reader->half)
			{
				uint16_t half;

				memcpy(&half, bytes + (size_t)s * 2, 2);
				values_out[v] = half_to_float(half);
			}
			else
			{
				memcpy(&values_out[v], bytes + (size_t)s * 4, 4);
			}
			s++;
		}
		else
		{
			values_out[v] = selection != NULL && mask_on(selection, v) ? inactive_one : inactive_zero;
		}
	}
}

static void grow_bounds(vdb_topology_t *topology, const int origin[3], int span)
{
	for (int a = 0; a < 3; a++)
	{
		if (!topology->any || origin[a] < topology->low[a])
		{
			topology->low[a] = origin[a];
		}
		if (!topology->any || origin[a] + span > topology->high[a])
		{
			topology->high[a] = origin[a] + span;
		}
	}
	topology->any = true;
}

static void add_tile(vdb_topology_t *topology, const int origin[3], int span, float value)
{
	if (value == 0.0f)
	{
		return;
	}
	if (topology->tile_count == topology->tile_capacity)
	{
		topology->tile_capacity = topology->tile_capacity * 2 + 64;
		topology->tiles = realloc(topology->tiles, topology->tile_capacity * sizeof(*topology->tiles));
	}
	memcpy(topology->tiles[topology->tile_count].origin, origin, sizeof(int) * 3);
	topology->tiles[topology->tile_count].span = span;
	topology->tiles[topology->tile_count].value = value;
	topology->tile_count++;
	grow_bounds(topology, origin, span);
}

static void add_leaf(vdb_topology_t *topology, const int origin[3])
{
	if (topology->leaf_count == topology->leaf_capacity)
	{
		topology->leaf_capacity = topology->leaf_capacity * 2 + 1024;
		topology->leaf_origins = realloc(topology->leaf_origins, topology->leaf_capacity * sizeof(*topology->leaf_origins));
	}
	memcpy(topology->leaf_origins[topology->leaf_count], origin, sizeof(int) * 3);
	topology->leaf_count++;
	grow_bounds(topology, origin, VDB_LEAF_SPAN);
}

// An internal node's topology (log2: its children a side, 5 or 4), its
// children's with it
static void read_internal_topology(vdb_reader_t *reader, vdb_topology_t *topology, const int origin[3], int log2)
{
	int count = 1 << (3 * log2);
	int child_span = log2 == VDB_UPPER_LOG2 ? VDB_LOWER_SPAN : VDB_LEAF_SPAN;
	const uint8_t *child_mask = read_mask(reader, count);
	const uint8_t *value_mask = read_mask(reader, count);
	float *values;

	if (reader->failed)
	{
		return;
	}
	values = malloc((size_t)count * sizeof(float));
	read_values(reader, values, count, value_mask);
	for (int n = 0; n < count && !reader->failed; n++)
	{
		// Children are numbered x major: n = (x << 2 log2) + (y << log2) + z
		int child_origin[3] = {
			origin[0] + (n >> (2 * log2)) * child_span,
			origin[1] + ((n >> log2) & ((1 << log2) - 1)) * child_span,
			origin[2] + (n & ((1 << log2) - 1)) * child_span,
		};

		if (!mask_on(child_mask, n))
		{
			if (mask_on(value_mask, n))
			{
				add_tile(topology, child_origin, child_span, values[n]);
			}
			continue;
		}
		if (log2 == VDB_UPPER_LOG2)
		{
			read_internal_topology(reader, topology, child_origin, VDB_LOWER_LOG2);
		}
		else
		{
			// A leaf's topology is only its value mask (read again with its values)
			read_mask(reader, VDB_LEAF_VALUES);
			add_leaf(topology, child_origin);
		}
	}
	free(values);
}

static void read_topology(vdb_reader_t *reader, vdb_topology_t *topology)
{
	int32_t buffer_count = read_int32(reader);
	uint32_t tile_count;
	uint32_t child_count;

	if (buffer_count != 1)
	{
		fail(reader, "a tree with %d buffers (1 expected)", buffer_count);
		return;
	}
	reader->background = read_float(reader);
	tile_count = read_uint32(reader);
	child_count = read_uint32(reader);
	for (uint32_t t = 0; t < tile_count && !reader->failed; t++)
	{
		int origin[3];
		float value;
		const uint8_t *active;

		for (int a = 0; a < 3; a++)
		{
			origin[a] = read_int32(reader);
		}
		value = read_float(reader);
		active = take(reader, 1);
		if (active != NULL && *active != 0)
		{
			add_tile(topology, origin, VDB_UPPER_SPAN, value);
		}
	}
	for (uint32_t c = 0; c < child_count && !reader->failed; c++)
	{
		int origin[3];

		for (int a = 0; a < 3; a++)
		{
			origin[a] = read_int32(reader);
		}
		read_internal_topology(reader, topology, origin, VDB_UPPER_LOG2);
	}
}

// The transform: only the voxel size is kept (the maps EmberGen, Houdini and
// Blender write)
static double read_transform(vdb_reader_t *reader)
{
	char map[128];
	double voxel_size = 1.0;

	read_string(reader, map, sizeof(map));
	if (strcmp(map, "UniformScaleTranslateMap") == 0 || strcmp(map, "ScaleTranslateMap") == 0)
	{
		// The translation, then as a scale map
		for (int a = 0; a < 3; a++)
		{
			read_double(reader);
		}
	}
	if (strcmp(map, "UniformScaleTranslateMap") == 0 || strcmp(map, "ScaleTranslateMap") == 0 || strcmp(map, "UniformScaleMap") == 0
		|| strcmp(map, "ScaleMap") == 0)
	{
		// The scale, the voxel size, the inverse scale, its square, half of it
		double numbers[15];

		for (int n = 0; n < 15; n++)
		{
			numbers[n] = read_double(reader);
		}
		voxel_size = numbers[3];
	}
	else if (strcmp(map, "TranslationMap") == 0)
	{
		for (int a = 0; a < 3; a++)
		{
			read_double(reader);
		}
	}
	else
	{
		fail(reader, "a grid placed with a %s (only scales and translations are read)", map);
	}
	return voxel_size;
}

void vdb_volume_free(vdb_volume_t *volume)
{
	free(volume->values);
	memset(volume, 0, sizeof(*volume));
}

// The voxels in the box: the grid's tiles and leaves into a dense box, then
// the box cropped to where it is not zero
static bool fill_volume(vdb_reader_t *reader, vdb_topology_t *topology, vdb_volume_t *volume)
{
	int size[3];
	size_t total;
	float *dense;
	float leaf_values[VDB_LEAF_VALUES];
	int low[3] = { 0, 0, 0 };
	int high[3] = { -1, -1, -1 };
	bool any = false;

	for (int a = 0; a < 3; a++)
	{
		size[a] = topology->high[a] - topology->low[a];
	}
	total = (size_t)size[0] * size[1] * size[2];
	if (total > (size_t)1 << 31)
	{
		fail(reader, "a grid of %d x %d x %d voxels is too big", size[0], size[1], size[2]);
		return false;
	}
	dense = calloc(total, sizeof(float));
	if (dense == NULL)
	{
		fail(reader, "no memory for %d x %d x %d voxels", size[0], size[1], size[2]);
		return false;
	}
#define DENSE(x, y, z) dense[((size_t)(z) * size[1] + (size_t)(y)) * size[0] + (size_t)(x)]
	for (size_t t = 0; t < topology->tile_count; t++)
	{
		const vdb_tile_t *tile = &topology->tiles[t];

		for (int z = 0; z < tile->span; z++)
		{
			for (int y = 0; y < tile->span; y++)
			{
				for (int x = 0; x < tile->span; x++)
				{
					DENSE(tile->origin[0] - topology->low[0] + x, tile->origin[1] - topology->low[1] + y, tile->origin[2] - topology->low[2] + z) = tile->value;
				}
			}
		}
	}
	// The leaves' values, in the order of their topology
	for (size_t l = 0; l < topology->leaf_count && !reader->failed; l++)
	{
		const int *origin = topology->leaf_origins[l];
		const uint8_t *value_mask = read_mask(reader, VDB_LEAF_VALUES);

		if (value_mask == NULL)
		{
			break;
		}
		read_values(reader, leaf_values, VDB_LEAF_VALUES, value_mask);
		for (int n = 0; n < VDB_LEAF_VALUES; n++)
		{
			// Voxels are numbered x major: n = (x << 6) + (y << 3) + z
			DENSE(origin[0] - topology->low[0] + (n >> 6), origin[1] - topology->low[1] + ((n >> 3) & 7), origin[2] - topology->low[2] + (n & 7))
				= leaf_values[n];
		}
	}
	if (reader->failed)
	{
		free(dense);
		return false;
	}
	// Cropped to the voxels with something in them
	volume->largest = 0.0f;
	for (int z = 0; z < size[2]; z++)
	{
		for (int y = 0; y < size[1]; y++)
		{
			for (int x = 0; x < size[0]; x++)
			{
				float value = DENSE(x, y, z);
				int at[3] = { x, y, z };

				if (value <= 0.0f)
				{
					continue;
				}
				volume->largest = fmaxf(volume->largest, value);
				for (int a = 0; a < 3; a++)
				{
					if (!any || at[a] < low[a])
					{
						low[a] = at[a];
					}
					if (!any || at[a] > high[a])
					{
						high[a] = at[a];
					}
				}
				any = true;
			}
		}
	}
	if (!any)
	{
		free(dense);
		fail(reader, "the grid is empty");
		return false;
	}
	volume->nx = high[0] - low[0] + 1;
	volume->ny = high[1] - low[1] + 1;
	volume->nz = high[2] - low[2] + 1;
	for (int a = 0; a < 3; a++)
	{
		volume->origin[a] = topology->low[a] + low[a];
	}
	volume->values = malloc((size_t)volume->nx * volume->ny * volume->nz * sizeof(float));
	for (int z = 0; z < volume->nz; z++)
	{
		for (int y = 0; y < volume->ny; y++)
		{
			memcpy(&volume->values[((size_t)z * volume->ny + (size_t)y) * volume->nx], &DENSE(low[0], low[1] + y, low[2] + z),
				(size_t)volume->nx * sizeof(float));
		}
	}
#undef DENSE
	free(dense);
	return true;
}

bool vdb_read(const char *path, const char *grid_name, vdb_volume_t *volume_out, char *error, size_t error_size)
{
	vdb_reader_t reader;
	vdb_topology_t topology;
	FILE *file = fopen(path, "rb");
	uint8_t *data;
	long length;
	int64_t magic;
	uint32_t grid_count;
	bool found = false;
	bool done = false;

	memset(volume_out, 0, sizeof(*volume_out));
	memset(&reader, 0, sizeof(reader));
	memset(&topology, 0, sizeof(topology));
	reader.error = error;
	reader.error_size = error_size;
	if (file == NULL)
	{
		snprintf(error, error_size, "cannot open %s", path);
		return false;
	}
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc(length > 0 ? (size_t)length : 1);
	if (data == NULL || length <= 0 || fread(data, 1, (size_t)length, file) != (size_t)length)
	{
		fclose(file);
		free(data);
		snprintf(error, error_size, "cannot read %s", path);
		return false;
	}
	fclose(file);
	reader.data = data;
	reader.size = (size_t)length;

	magic = read_int64(&reader);
	reader.version = read_uint32(&reader);
	if (magic != VDB_MAGIC)
	{
		fail(&reader, "not an OpenVDB file");
	}
	else if (reader.version < VDB_OLDEST_VERSION)
	{
		fail(&reader, "OpenVDB format %u (only %d and newer are read)", reader.version, VDB_OLDEST_VERSION);
	}
	read_uint32(&reader);
	read_uint32(&reader);
	{
		const uint8_t *has_offsets = take(&reader, 1);

		if (has_offsets != NULL && *has_offsets == 0)
		{
			fail(&reader, "a file without grid offsets (an old writer)");
		}
	}
	take(&reader, 36);
	skip_metadata(&reader);
	grid_count = read_uint32(&reader);
	for (uint32_t g = 0; g < grid_count && !reader.failed && !done; g++)
	{
		char name[256];
		char type[256];
		char parent[256];
		int64_t grid_at;
		int64_t end_at;
		char *separator;

		read_string(&reader, name, sizeof(name));
		read_string(&reader, type, sizeof(type));
		read_string(&reader, parent, sizeof(parent));
		grid_at = read_int64(&reader);
		read_int64(&reader);
		end_at = read_int64(&reader);
		// Names made unique carry a suffix after a record separator
		separator = strchr(name, 0x1e);
		if (separator != NULL)
		{
			*separator = '\0';
		}
		if (reader.failed || strncmp(type, "Tree_float_5_4_3", 16) != 0 || parent[0] != '\0'
			|| (grid_name != NULL && strcmp(grid_name, name) != 0))
		{
			// Not this one: the next descriptor follows its end
			if (end_at > 0 && (size_t)end_at <= reader.size)
			{
				reader.at = (size_t)end_at;
			}
			continue;
		}
		found = true;
		reader.half = strstr(type, "_HalfFloat") != NULL;
		if (grid_at <= 0 || (size_t)grid_at >= reader.size)
		{
			fail(&reader, "grid %s is placed past the file's end", name);
			break;
		}
		reader.at = (size_t)grid_at;
		reader.compression = read_uint32(&reader);
		skip_metadata(&reader);
		volume_out->voxel_size = read_transform(&reader);
		read_topology(&reader, &topology);
		if (!reader.failed && !topology.any)
		{
			fail(&reader, "grid %s is empty", name);
		}
		if (!reader.failed)
		{
			fill_volume(&reader, &topology, volume_out);
		}
		if (!reader.failed && end_at > 0 && reader.at != (size_t)end_at)
		{
			fail(&reader, "grid %s was misread: it ends at byte %zu, the file says %lld", name, reader.at, (long long)end_at);
		}
		done = true;
	}
	if (!reader.failed && !found)
	{
		if (grid_name != NULL)
		{
			fail(&reader, "no float grid named %s", grid_name);
		}
		else
		{
			fail(&reader, "no float grid");
		}
	}
	free(topology.leaf_origins);
	free(topology.tiles);
	free(reader.unpacked);
	free(data);
	if (reader.failed)
	{
		vdb_volume_free(volume_out);
		return false;
	}
	return true;
}
