# Particles

Fire, smoke, sparks, magic: the effects are sections of
`data/particles.ini`, which documents every key at its top (texture, blend,
continuous or burst, the emitter's shape, life, speed, direction and spread,
gravity, drag, turbulence, spin, stretch, collisions, flashing, and color and size
gradients over a particle's life). The **Effects** tool (Debug, in the brush
panel) lists them: a click plays the chosen one on the first thing the ray
under the pointer meets (as Alt shows it): an actor (it follows the actor),
else an object (fitted to its box;
`shape = outline` goes around it, like the sparkles of a finished building;
it follows the object if the water carries it off), else on the ground.
"Stop all" ends them, "Reload ini" reads the file again while they play.

Everything runs on the GPU, in compute shaders (`data/shaders/particles_*`):
a pool of 262 144 particles (31 MB), a list of free slots and two lists of
living ones used in turn. Each frame the CPU sends only "this many of effect
E, here, shaped so"; new particles take free slots, every living one ages,
moves (gravity, drag, swirls, bouncing off or dying on the ground and the
water, read from the textures the terrain and water already have on the
GPU) and is drawn as one quad facing the camera, the count straight from the
GPU (indirect draw), nothing read back. Glowing particles add to what is
behind them and lit ones (smoke) cover it, in one draw with one blend mode,
unsorted. Particles fade where they go into the ground or the water instead
of ending in a hard line. One thinner than a pixel (a tracer far off) is drawn a pixel wide and as much dimmer, so it does not fall between the pixels. The textures are made at start, no image files: a
soft dot, a spark, a star, a ring, four smoke puffs and eight flame frames. An effect with `light` lights its surroundings too (see Local lights,
under Tuning).
