# side_projects/common

Code the side projects share (WreckingBall, the damage sandbox), kept out
of the engine on purpose.

- `shallow_water.c/h`: water on a heightmap ("virtual pipes", no
  erosion): pour, flood, step, and sample depth / surface / flow at a
  point. Plain C11 plus SDL3 for memory and math; no GPU.
