#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// The buildings' fragment shader (OBJECT_MATERIAL_AGED): the props', and
// besides aged by the weathering baked per vertex and the world (prop_body.glsl)

#define BUILDING
#include "prop_body.glsl"
