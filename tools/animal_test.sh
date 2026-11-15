#!/bin/sh
# The animals' minds tested without the picture (docs/claude/00_START_HERE.md,
# "Testing the animals' minds"): an evening, a night and a morning at the
# hen house, the cow farm, the stana and the pigsty, then the numbers and
# the picture.
#
#     tools/animal_test.sh [SECONDS] [HOUR] [OUT_DIRECTORY]
#
# Needs objects.ini sections hen_house, cow_farm, stana, pigsty and actors.ini's
# [hen], [cow], [ram], [pig] (the test's ini files are never shipped: if one is
# missing, it says which and stops).
SECONDS_TO_RUN=${1:-1200}
HOUR=${2:-16}
OUT=${3:-.}
cd "$(dirname "$0")/.." || exit 1
missing=""
for type in hen_house cow_farm stana pigsty; do
	grep -q "^\[$type\]" data/objects.ini || missing="$missing objects.ini:[$type]"
done
for actor in hen cow ram pig; do
	grep -q "^\[$actor\]" data/actors.ini || missing="$missing actors.ini:[$actor]"
done
if [ -n "$missing" ]; then
	echo "missing:$missing (see docs/claude/01_engine_map.md for the sections)"
	exit 1
fi
mkdir -p "$OUT"
xvfb-run -a ./potop --houses hen_house,cow_farm,stana,pigsty --width 320 --height 240 \
	--set render.day_cycle=true --set render.day_length_minutes=24 --hour "$HOUR" \
	--sim "$SECONDS_TO_RUN" --trace "$OUT/trace.txt" > "$OUT/run.log" 2>&1
python3 tools/animal_trace.py "$OUT/trace.txt" --out "$OUT/trace.png"
python3 tools/animal_trace.py "$OUT/trace.txt" --walk --out "$OUT/trace_walk.png" > /dev/null
