#!/bin/sh
# Makes every terrain texture of data/materials.ini again with
# tools/potop_texture (color, normals, roughness).
#
#   tools/make_textures.sh [SIZE] [DATA_DIRECTORY]
#
# SIZE: pixels across (2048); the relief is made at half of it.
# Each texture's meters must match its tiling_meters (or
# macro_tiling_meters) in materials.ini. A wet texture uses the same kind
# options and seed as its dry one, so its cracks, ripples and clods lie in
# the same places (it turns wet in place). Change the names here to match
# your materials.ini.

set -e

size=${1:-2048}
data=${2:-data}
tool=$(dirname "$0")/potop_texture
relief_size=$((size / 2))

if [ ! -x "$tool" ]; then
	echo "make_textures.sh: $tool not built (make tools/potop_texture)" >&2
	exit 1
fi
mkdir -p "$data/textures"

make_texture()
{
	echo "== $*"
	"$tool" --data "$data" --size "$size" "$@" > /dev/null
}

# Sand: dry, and soaked (same seed: the same ripples)
make_texture --seed 1 --meters 2 sand sand_01
make_texture --seed 1 --meters 2 wet_sand wet_sand_01

# Clay: dry and cracked; mud is the same clay soaked (same seed, meters
# and plates: its seams lie on the cracks)
make_texture --seed 1 --meters 3 clay clay_01
make_texture --seed 1 --meters 3 mud mud_01

# Bare earth, dry and soaked
make_texture --seed 1 --meters 2 dirt dirt_01
make_texture --seed 1 --meters 2 wet_dirt wet_dirt_01

# Grass
make_texture --seed 1 --meters 2 grass grass_01

# What lies under things (the material map's covers): a conifer's needles,
# a broadleaved tree's leaves, trampled earth round houses; and a blooming
# tree's petals (with their _mask.png: the order they show in)
make_texture --seed 1 --meters 2 needles needles_01
make_texture --seed 1 --meters 2 leaves leaves_01
make_texture --seed 1 --meters 2 trampled trampled_01
make_texture --seed 1 --meters 2 petals petals_01

# Snow (an overlay too: its _mask.png, where it settles first)
make_texture --seed 1 --meters 4 snow snow_01

# A mountain's rock: the stone up close, the beds from afar (its macro
# texture), the big relief of its faces
make_texture --seed 1 --meters 4 stone stone_01
make_texture --seed 1 --meters 24 rock rock_01
echo "== relief rock_relief"
"$tool" --data "$data" --size "$relief_size" --seed 1 --meters 96 relief rock_relief > /dev/null

echo "Done: $data/textures"
