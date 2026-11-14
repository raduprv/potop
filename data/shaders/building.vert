#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// The buildings' vertex shader (OBJECT_MATERIAL_AGED): the props', their
// weathering baked per vertex in a buffer of its own (prop_vertex.glsl)

#define BUILDING
#include "prop_vertex.glsl"
