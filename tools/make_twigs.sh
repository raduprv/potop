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
for seed in 1 2 3; do
	tools/potop_twig --width 128 --seed $seed pine_needles plant_plans/twigs/mountain_pine_needles_$seed.png
done
for seed in 1 2 3; do
	tools/potop_twig --width 512 --seed $seed spruce plant_plans/twigs/fir_$seed.png
done
for seed in 1 2 3; do
	tools/potop_twig --width 512 --seed $seed juniper plant_plans/twigs/juniper_$seed.png
done
for seed in 1 2 3; do
	tools/potop_twig --width 512 --seed $seed juniper_spread plant_plans/twigs/juniper_spread_$seed.png
	tools/potop_twig --width 512 --seed $seed juniper_awl plant_plans/twigs/juniper_awl_$seed.png
done
for seed in 1 2 3; do
	tools/potop_twig --width 512 --seed $seed bald_cypress plant_plans/twigs/bald_cypress_$seed.png
done
for seed in 1 2 3; do
	tools/potop_twig --width 512 --seed $seed cranberry plant_plans/twigs/cranberry_$seed.png
done
for seed in 1 2 3; do
	tools/potop_twig --width 512 --seed $seed cranberry_leaves plant_plans/twigs/cranberry_leaves_$seed.png
done
for seed in 1 2; do
	tools/potop_twig --width 256 --seed $seed berry_skin plant_plans/twigs/berry_skin_$seed.png
done
for seed in 1 2 3; do
	tools/potop_twig --width 512 --seed $seed blueberry_leaves plant_plans/twigs/blueberry_leaves_$seed.png
done
for seed in 1 2; do
	tools/potop_twig --width 256 --seed $seed blueberry_skin plant_plans/twigs/blueberry_skin_$seed.png
done
