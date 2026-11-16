// What the plants' shaders share (plant.vert, plant_shadow.vert): the
// autumn, a tree and a card at a time (see plant.frag), and the branches
// sagging under the winter's snow (below).
//
// The season's autumn (render.autumn, 0 summer .. 1 the end of it) comes
// to each tree a little earlier or later (its place decides by how much),
// and within a tree to each card of its foliage in its own order: the
// outside of the crown and its top first. A card turns to its autumn color
// over the first part of it; where the foliage falls, a card falls (it is
// not drawn) once the autumn has gone past its own moment, the outside
// first again, until the tree is bare.

// The season as the plant's draws get it: x the autumn (0..1), y how far
// apart the trees are in it, z how much the material turns (0: evergreen),
// w 1 if it falls once turned

// A tree's own autumn: the season's, earlier or later by its place
float plant_autumn_of(vec4 season, vec3 tree_origin)
{
	float own = fract(sin(dot(tree_origin.xz, vec2(41.37, 17.93))) * 43758.5453);

	// At 0 none has begun, at 1 every one is through (the latest too)
	return season.x * (1.0 + 2.0 * season.y) - 2.0 * season.y * own;
}

// The order in which a card turns and falls (0 first .. 1 last): its own
// number, the outside of the crown and the top first
float plant_card_order(vec4 plant_data)
{
	return clamp(0.55 * plant_data.z + 0.3 * (1.0 - plant_data.w) + 0.15 * (1.0 - plant_data.y), 0.0, 1.0);
}

// How far a card has turned (0 green .. 1 its autumn color)
float plant_card_turned(vec4 season, float tree_autumn, vec4 plant_data)
{
	float order = plant_card_order(plant_data);
	float start = 0.05 + 0.45 * order;

	return season.z * smoothstep(start, start + 0.12, tree_autumn);
}

// Whether a card has fallen
bool plant_card_fallen(vec4 season, float tree_autumn, vec4 plant_data)
{
	if (season.w < 0.5 || season.z <= 0.0)
	{
		return false;
	}
	float order = clamp(0.6 * fract(plant_data.z * 7.31) + 0.4 * plant_card_order(plant_data), 0.0, 1.0);

	return tree_autumn > 0.55 + 0.44 * order;
}

// The winter's snow as the plant's draws get it: x how much (0 none .. 1
// all of it), y how far the branches sag under all of it (of the plant's
// height, at the crown's edge), w the crown's reach (meters, the model's).
// The foliage's own snow is in its pictures (the atlas' winter layers);
// the branches sag the more the farther out (the square of it: the trunk
// stays).

// Below this much snow on things, the snow baked in lying on the boughs is
// not drawn (OBJECT_FILE_SNOW: it grows from nothing)
const float PLANT_SNOW_SHOWN = 0.02;

// Where the snow's weight puts a vertex (model space, before the wind)
vec3 plant_sagged(vec4 snow, float height, vec3 position)
{
	if (snow.x > 0.0 && snow.y > 0.0 && snow.w > 0.0)
	{
		float reach = min(length(position.xz) / snow.w, 1.0);

		position.y -= snow.x * snow.y * height * reach * reach;
	}
	return position;
}

// The months' pictures (tools/potop_plant): a card's picture that shows
// another in this month (in a cell of its own in the atlas) is moved there.
// cells: the cells that do (u0, v0, u1, v1), offsets: how far (xy), count
// of them
//
// Through the snow's months the pictures go by the snow on things instead
// (count below 0, its size the count): four of them, the least snow first,
// each offset two halves in a float's bits (x, y, z, w: the four); each
// card (card_random, 0..1) a little ahead of the others or behind, so the
// snow comes and goes branch by branch, a step at a time, none of them
// blended
#define PLANT_MONTH_CELLS 32

// The vertex's own cell is known (its plant_snow.w: plant_cell_of, -1
// none: the cells' order the model's, object_month_cell_of), offsets that
// cell's
vec2 plant_month_uv(vec2 uv, int cell, vec4 offsets, int count, float snow_amount, float card_random)
{
	if (count == 0 || cell < 0 || cell >= PLANT_MONTH_CELLS)
	{
		return uv;
	}
	if (count > 0)
	{
		return uv + offsets.xy;
	}
	// (none: green; 1 .. 4 its pictures)
	int level = clamp(int(floor(snow_amount * 4.0 + 0.4 + (card_random - 0.5) * 0.9)), 0, 4);
	uvec4 words = floatBitsToUint(offsets);

	if (level == 0)
	{
		return uv;
	}
	return uv + unpackHalf2x16(words[level - 1]);
}

// From a plant vertex's plant_snow.w (object_plant_snow_word): its month
// cell (-1 none), and whether it is of the snow lying on it
int plant_cell_of(float snow_word)
{
	return int(snow_word + 0.5) / 2 - 1;
}

bool plant_snow_lying(float snow_word)
{
	return (int(snow_word + 0.5) & 1) != 0;
}
