#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A shell of the moss that grows under water on the rocks (prop_shell.vert
// lifts it off the surface): its strands cut out of it, lit as a prop

#define MOSS_SHELL
#include "prop_body.glsl"
