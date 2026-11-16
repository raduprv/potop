// The sky's occlusion and the light the land round sends back, worked out
// per vertex (sky_occlusion.glsl): for what is drawn over and over on the
// same pixels (the grass, the plants' leaves, the impostors), where they
// change slowly (a meter or so) and a pixel's own would cost many lookups.
// SKY_VERTEX_BINDING and the next three: the occlusion, its layers, the
// bounce texture, the far shadow map. Wants the scene block.

layout(set = TEXTURE_SET, binding = SKY_VERTEX_BINDING) uniform sampler2D sky_occlusion_texture;      // R8 a cell (sky_occlusion.h)
layout(set = TEXTURE_SET, binding = SKY_VERTEX_BINDING + 1) uniform sampler2D sky_layers_texture;     // RGBA8: the four layers in the air
layout(set = TEXTURE_SET, binding = SKY_VERTEX_BINDING + 2) uniform sampler2D sky_bounce_texture;     // RGBA8: what hides the sky
layout(set = TEXTURE_SET, binding = SKY_VERTEX_BINDING + 3) uniform sampler2DShadow shadow_texture;   // the far shadow map
#define SKY_OCCLUSION_NO_GROUND
#define SKY_BOUNCE
#include "sky_occlusion.glsl"
