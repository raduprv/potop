# side_projects/common

Code the side projects share (WreckingBall, the damage sandbox), kept out
of the engine on purpose.

- `shallow_water.c/h`: water on a heightmap ("virtual pipes", no
  erosion): pour, flood, step, and sample depth / surface / flow at a
  point. Plain C11 and the C library; no SDL, no GPU (the damage bench's
  headless damage_shot builds without SDL).
