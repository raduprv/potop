#!/bin/sh
# Draws the procedural twig and frond pictures the plant recipes take
# (plant_plans/twigs/: made here, not kept in git). Run from the project's
# root after make tools.
set -e
mkdir -p plant_plans/twigs
for seed in 1 2 3; do
	tools/potop_twig --width 768 --seed $seed pine plant_plans/twigs/mountain_pine_$seed.png
done
tools/potop_twig --width 887 fern plant_plans/twigs/fern_broad.png
tools/potop_twig --width 887 fern_lobed plant_plans/twigs/fern_lobed.png
