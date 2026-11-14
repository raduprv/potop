#version 450
#extension GL_GOOGLE_include_directive : require
// The water's surface (water_surface.glsl), its reflections left for
// water_reflection.comp to march at half the size
#define WATER_SPLIT_REFLECTIONS
#include "water_surface.glsl"
