// Which material the ground shows where: the terrain draws it
// (terrain.frag), the grass grows where it shows grass (grass.comp). One
// code for both, or the blades grow on bare clay and miss the grass.
//
// Wants constants.glsl (MATERIAL_*) and value_noise() before it.

// The soil grid's lookup position, warped by noise so borders between
// soils wander instead of following the grid (blend_cells: their width)
vec2 soil_warped_position(vec2 grid_position, float blend_cells)
{
	float width = max(blend_cells, 1.0);
	vec2 warp_position = grid_position / (width * 2.5);
	vec2 warp = vec2(value_noise(warp_position), value_noise(warp_position + 19.7)) - 0.5
		+ 0.5 * (vec2(value_noise(warp_position * 2.7 + 5.1), value_noise(warp_position * 2.7 + 33.3)) - 0.5);

	return grid_position + warp * width * 1.2;
}

// Every material's weight here, from the soils (sand, earth, clay, rock),
// the surface (silt, wetness, bare rock, water), the slope, how it faces
// the sun, the noises, the snow (x: its line as a share of max_height, y:
// its shift with the sun, z: max_height) and slope_ranges (grass from,
// grass to, cliff from, cliff to)
void terrain_material_weights(vec3 position, vec4 soil, vec4 surface, float slope, float sun_facing, float detail_noise, float broad_noise,
	float variation_large, float variation_small, float relief_height, vec4 slope_ranges, vec3 snow_parameters,
	out float weights[MATERIAL_COUNT])
{
	for (int i = 0; i < MATERIAL_COUNT; i++)
	{
		weights[i] = 0.0;
	}
	// Earth: grass where it is gentle and not baked by the sun, bare dirt
	// elsewhere
	{
		float grass_limit = slope + sun_facing * 0.15 + (detail_noise - 0.5) * 0.15 + (variation_large - 0.5) * 0.12;
		float grass = 1.0 - smoothstep(slope_ranges.x, slope_ranges.y, grass_limit);

		weights[MATERIAL_GRASS] += soil.y * grass;
		weights[MATERIAL_DIRT] += soil.y * (1.0 - grass);
	}
	// Clay: mostly bare, with some grass on flat patches
	{
		float grass = (1.0 - smoothstep(0.08, 0.25, slope)) * smoothstep(0.45, 0.75, broad_noise) * 0.8;

		weights[MATERIAL_GRASS] += soil.z * grass;
		weights[MATERIAL_CLAY] += soil.z * (1.0 - grass);
	}
	weights[MATERIAL_SAND] += soil.x;
	weights[MATERIAL_ROCK] += soil.w;

	// Soil worn down to the bedrock shows rock; silt covers whatever lies
	// under it. Noise makes thin silt patchy instead of a uniform veil.
	{
		float bare = surface.b;
		float silt = clamp(surface.r * (0.8 + 0.6 * detail_noise), 0.0, 1.0);

		for (int i = 0; i < MATERIAL_COUNT; i++)
		{
			weights[i] *= 1.0 - bare;
		}
		weights[MATERIAL_ROCK] += bare;
		for (int i = 0; i < MATERIAL_COUNT; i++)
		{
			weights[i] *= 1.0 - silt;
		}
		weights[MATERIAL_SILT] += silt;
	}

	// Very steep ground shows rock whatever the soil says
	{
		float cliff = smoothstep(slope_ranges.z, slope_ranges.w, slope + (detail_noise - 0.5) * 0.1 + (variation_small - 0.5) * 0.1);

		for (int i = 0; i < MATERIAL_COUNT; i++)
		{
			weights[i] *= 1.0 - cliff;
		}
		weights[MATERIAL_ROCK] += cliff;
	}

	// Snow: the line drops on shady slopes and rises on sunny ones, and snow
	// does not stick to near vertical faces; down a gully it lies lower, and
	// on steeper ground (it drifts in and is shaded there)
	{
		float gully = clamp((0.5 - relief_height) * 2.5, 0.0, 1.0);
		float snow_line = (snow_parameters.x + snow_parameters.y * sun_facing) * snow_parameters.z - gully * snow_parameters.z * 0.06;
		float snow = smoothstep(snow_line - 2.0, snow_line + 2.0, position.y + (broad_noise - 0.5) * 10.0);

		snow *= 1.0 - smoothstep(0.4 + 0.25 * gully, 0.6 + 0.25 * gully, slope);
		for (int i = 0; i < MATERIAL_COUNT; i++)
		{
			weights[i] *= 1.0 - snow;
		}
		weights[MATERIAL_SNOW] += snow;
	}

	// Per material jitter at a few meters scale: borders between materials
	// get ragged and patchy instead of following smooth contour lines
	for (int i = 0; i < MATERIAL_COUNT; i++)
	{
		if (weights[i] > 0.001)
		{
			weights[i] *= 0.75 + 0.5 * value_noise(position.xz * 0.35 + float(i) * 7.3);
		}
	}
}

// Where materials meet, each breaks into patches of the others: a
// material's claim on the spot is its share of the weight, raised or
// lowered by its own noise (patches of its patch size, their edges ragged
// at an eighth of it), and only the claims within a narrow band of the
// strongest show: mostly one material, two along a patch's edge, as wide as
// its patch_edge (at least a pixel and a half: pixel_meters; far off it
// widens into a plain blend). The three strongest claims are chosen (only
// those are read): chosen by their claims, not their weights, or a faint
// material winning its patches was cut off along a straight line where a
// fourth crept past it. contributions: each one's part (summing to 1).
// patches: per material its repeats per meter, strength, edge (meters).
void terrain_material_patches(float weights[MATERIAL_COUNT], vec2 world_xz, float pixel_meters, vec4 patches[MATERIAL_COUNT],
	out int chosen[3], out float contributions[3])
{
	float total = 0.0;
	float scores[3];
	float band = 1e-3;
	float strongest_patches = 0.0;
	float contribution_total = 0.0;

	for (int k = 0; k < 3; k++)
	{
		chosen[k] = -1;
		scores[k] = -1e9;
		contributions[k] = 0.0;
	}
	for (int i = 0; i < MATERIAL_COUNT; i++)
	{
		total += weights[i];
	}
	total = max(total, 1e-4);
	for (int i = 0; i < MATERIAL_COUNT; i++)
	{
		float share = weights[i] / total;
		vec4 own = patches[i];
		vec2 at;
		float score;

		if (weights[i] <= 0.001)
		{
			continue;
		}
		at = world_xz * own.x + float(i) * 17.31;
		score = share + own.y * (0.7 * value_noise(at) + 0.3 * value_noise(at * 8.0 + 41.7) - 0.5);
		// (the band, and how patchy: from every material there, each by how
		// much of it there is)
		{
			float present = smoothstep(0.0, 0.15, share);
			float edge = max(own.z, pixel_meters * 1.5);

			band = max(band, present * own.y * own.x * 3.0 * edge);
			strongest_patches = max(strongest_patches, present * own.y);
		}
		if (score > scores[0])
		{
			chosen[2] = chosen[1]; scores[2] = scores[1];
			chosen[1] = chosen[0]; scores[1] = scores[0];
			chosen[0] = i; scores[0] = score;
		}
		else if (score > scores[1])
		{
			chosen[2] = chosen[1]; scores[2] = scores[1];
			chosen[1] = i; scores[1] = score;
		}
		else if (score > scores[2])
		{
			chosen[2] = i; scores[2] = score;
		}
	}
	// (no patches: a plain blend of the weights)
	band = mix(1.0, band, clamp(strongest_patches * 20.0, 0.0, 1.0));
	for (int k = 0; k < 3; k++)
	{
		if (chosen[k] >= 0)
		{
			// (without patches the weights themselves, as a plain blend)
			float claim = clamp((scores[k] - (scores[0] - band)) / band, 0.0, 1.0);

			contributions[k] = mix(weights[chosen[k]] / total, claim, clamp(strongest_patches * 20.0, 0.0, 1.0));
			contribution_total += contributions[k];
		}
	}
	for (int k = 0; k < 3; k++)
	{
		contributions[k] /= max(contribution_total, 1e-4);
	}
}

#ifdef MATERIAL_MAP
// --- The material map's way: a few fixed slots ----------------------------
//
// Slots 0..3: the map's four cells round the spot; 4: the bedrock the water
// laid bare; 5: the silt it left; 6: the snow by its line. Every loop over
// them has a fixed count and is unrolled: no array is indexed by anything
// worked out at run time (that would put them in slow memory).

#define MATERIAL_SLOTS 7
// The material map's "none" (materials.h's MATERIAL_NONE)
#define MATERIAL_NONE 255

// The map's materials round a spot (grid_position: in cells): the four
// cells about it, by how near (a cell's cover over its base); the same
// material in two of them counts once. The spot is moved about a cell and
// a half by noise first, so borders between cells wander instead of
// following the grid
void terrain_map_slots(sampler2D material_map, vec2 grid_position, float map_size, out int ids[MATERIAL_SLOTS],
	out float weights[MATERIAL_SLOTS])
{
	vec2 warped = soil_warped_position(grid_position, 1.5);
	vec2 low = floor(warped);
	vec2 along = warped - low;
	// (cell centers at whole grid positions, texel centers half a texel in:
	// the corner the four cells share)
	vec2 uv = (low + 1.0) / map_size;
	vec4 bases = textureGather(material_map, uv, 0) * 255.0 + 0.5;
	vec4 covers = textureGather(material_map, uv, 1) * 255.0 + 0.5;
	// (gather order: (0, 1), (1, 1), (1, 0), (0, 0))
	vec4 nearness = vec4((1.0 - along.x) * along.y, along.x * along.y, along.x * (1.0 - along.y), (1.0 - along.x) * (1.0 - along.y));

	[[unroll]] for (int i = 0; i < 4; i++)
	{
		int cover = int(covers[i]);

		ids[i] = cover != MATERIAL_NONE ? cover : min(int(bases[i]), MATERIAL_COUNT - 1);
		weights[i] = nearness[i];
	}
	[[unroll]] for (int i = 1; i < 4; i++)
	{
		bool merged = false;

		[[unroll]] for (int j = 0; j < i; j++)
		{
			if (!merged && ids[j] == ids[i])
			{
				weights[j] += weights[i];
				weights[i] = 0.0;
				merged = true;
			}
		}
	}
	ids[4] = MATERIAL_ROCK;
	ids[5] = MATERIAL_SILT;
	ids[6] = MATERIAL_SNOW;
	weights[4] = 0.0;
	weights[5] = 0.0;
	weights[6] = 0.0;
}

// A layer over what is there, amount of it (0..1): the rest scaled down to
// make room; into a cell's slot of the same material if there is one (it
// would only compete with itself), else into its own slot
void terrain_slot_layer(inout int ids[MATERIAL_SLOTS], inout float weights[MATERIAL_SLOTS], int slot, float amount)
{
	bool merged = false;

	[[unroll]] for (int k = 0; k < MATERIAL_SLOTS; k++)
	{
		weights[k] *= 1.0 - amount;
	}
	[[unroll]] for (int k = 0; k < 4; k++)
	{
		if (!merged && ids[k] == ids[slot] && weights[k] > 0.0)
		{
			weights[k] += amount;
			merged = true;
		}
	}
	weights[slot] = merged ? 0.0 : amount;
}

// Per material jitter at a few meters scale: borders between materials get
// ragged and patchy instead of following smooth contour lines
void terrain_slots_jitter(int ids[MATERIAL_SLOTS], inout float weights[MATERIAL_SLOTS], vec2 world_xz)
{
	[[unroll]] for (int k = 0; k < MATERIAL_SLOTS; k++)
	{
		if (weights[k] > 0.001)
		{
			weights[k] *= 0.75 + 0.5 * value_noise(world_xz * 0.35 + float(ids[k]) * 7.3);
		}
	}
}

// terrain_material_patches over the slots
void terrain_slot_patches(int ids[MATERIAL_SLOTS], float weights[MATERIAL_SLOTS], vec2 world_xz, float pixel_meters,
	vec4 patches[MATERIAL_COUNT], out int chosen[3], out float contributions[3])
{
	float total = 0.0;
	float scores[3];
	float chosen_weights[3];
	float band = 1e-3;
	float strongest_patches = 0.0;
	float contribution_total = 0.0;

	[[unroll]] for (int k = 0; k < 3; k++)
	{
		chosen[k] = -1;
		scores[k] = -1e9;
		chosen_weights[k] = 0.0;
		contributions[k] = 0.0;
	}
	[[unroll]] for (int k = 0; k < MATERIAL_SLOTS; k++)
	{
		total += weights[k];
	}
	total = max(total, 1e-4);
	[[unroll]] for (int k = 0; k < MATERIAL_SLOTS; k++)
	{
		int i = ids[k];
		float weight = weights[k];

		if (weight > 0.001)
		{
			float share = weight / total;
			vec4 own = patches[i];
			vec2 at = world_xz * own.x + float(i) * 17.31;
			float score = share + own.y * (0.7 * value_noise(at) + 0.3 * value_noise(at * 8.0 + 41.7) - 0.5);
			float present = smoothstep(0.0, 0.15, share);
			float edge = max(own.z, pixel_meters * 1.5);

			band = max(band, present * own.y * own.x * 3.0 * edge);
			strongest_patches = max(strongest_patches, present * own.y);
			if (score > scores[0])
			{
				chosen[2] = chosen[1]; scores[2] = scores[1]; chosen_weights[2] = chosen_weights[1];
				chosen[1] = chosen[0]; scores[1] = scores[0]; chosen_weights[1] = chosen_weights[0];
				chosen[0] = i; scores[0] = score; chosen_weights[0] = weight;
			}
			else if (score > scores[1])
			{
				chosen[2] = chosen[1]; scores[2] = scores[1]; chosen_weights[2] = chosen_weights[1];
				chosen[1] = i; scores[1] = score; chosen_weights[1] = weight;
			}
			else if (score > scores[2])
			{
				chosen[2] = i; scores[2] = score; chosen_weights[2] = weight;
			}
		}
	}
	band = mix(1.0, band, clamp(strongest_patches * 20.0, 0.0, 1.0));
	[[unroll]] for (int k = 0; k < 3; k++)
	{
		if (chosen[k] >= 0)
		{
			float claim = clamp((scores[k] - (scores[0] - band)) / band, 0.0, 1.0);

			contributions[k] = mix(chosen_weights[k] / total, claim, clamp(strongest_patches * 20.0, 0.0, 1.0));
			contribution_total += contributions[k];
		}
	}
	[[unroll]] for (int k = 0; k < 3; k++)
	{
		contributions[k] /= max(contribution_total, 1e-4);
	}
}
#endif

// Snow by its line: the line drops on shady slopes and rises on sunny ones,
// and snow does not stick to near vertical faces; down a gully it lies
// lower, and on steeper ground (it drifts in and is shaded there).
// snow_parameters: its line as a share of max_height, its shift with the
// sun, max_height
float terrain_snow_line_amount(vec3 position, float slope, float sun_facing, float broad_noise, float relief_height, vec3 snow_parameters)
{
	float gully = clamp((0.5 - relief_height) * 2.5, 0.0, 1.0);
	float snow_line = (snow_parameters.x + snow_parameters.y * sun_facing) * snow_parameters.z - gully * snow_parameters.z * 0.06;
	float snow = smoothstep(snow_line - 2.0, snow_line + 2.0, position.y + (broad_noise - 0.5) * 10.0);

	return snow * (1.0 - smoothstep(0.4 + 0.25 * gully, 0.6 + 0.25 * gully, slope));
}
