#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A shell of a thatch's straws (thatch_shell.vert lifts it off the roof):
// the straws standing up to it cut out of it, lit as the building

#define BUILDING
#define THATCH_SHELL
#include "prop_body.glsl"
