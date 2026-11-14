#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A shell of a thatch's straws (thatch_shell.frag): the building as
// building.vert has it, lifted off its roof along the normal and slid down
// the slope, one layer of the straws' height a draw. Only near the eye:
// farther buildings are put out of view whole.

#define BUILDING
#define THATCH_SHELL
#include "prop_vertex.glsl"
