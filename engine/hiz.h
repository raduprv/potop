// The depth pyramid for occlusion culling (Hi-Z).
//
// When the scene's terrain, objects and sky are drawn (before the water,
// which may write depth but hides nothing), the pass pauses and its depth
// is folded into a pyramid: each level half the size of the one below,
// each texel the farthest depth of what it covers. The next frame's
// culling (objects_cull.comp) tests each object's box against it, with the
// camera matrix the pyramid was made with (kept here): a box whose nearest
// point is farther than the pyramid's texels over its outline is hidden,
// whole, behind a mountain or a house. Being a frame old, what moves can
// hide what is behind it for a frame; what comes into view by the camera
// turning is outside the old pyramid, so it is drawn.
//
// The culling happens before the scene's pass, not in a pause between the
// terrain and the objects: done there (the culling's compute passes, then
// the pass resumed and the objects drawn at once), the first draws now and
// then saw what the culling wrote the frame before on some drivers
// (NVIDIA, full screen): objects drawn for a frame with another's place
// and size. The barriers SDL puts there look right; why it happened is
// not known.
//
// Level 0 is half the scene's size.
//
// How it is built keeps clear of a trap: SDL_GPU's Vulkan backend leaves a
// texture, after it is written (a render pass, a copy, a compute pass), in
// its sampled state with a barrier that covers only the vertex and fragment
// shaders. A compute shader that samples it next is not ordered after the
// write, and may read it half done (the objects then flickered: hidden or
// shown wrongly for a frame, most at high resolutions). So level 0 is made
// by a fragment shader (which samples the depth safely) into a color
// target; a compute pass reads that as a storage image (SDL orders those
// reads) into a storage buffer, and the other levels are made in that
// buffer, one compute pass each (buffers are ordered between passes). The
// culling reads the buffer.

#ifndef HIZ_H
#define HIZ_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"

#define HIZ_MAX_LEVELS 16

typedef struct
{
	SDL_GPUTexture *first_level;              // level 0, from the scene's depth (a color target)
	SDL_GPUBuffer *buffer;                    // every level, one after the other, row by row (floats)
	uint32_t buffer_capacity;                 // bytes
	SDL_GPUGraphicsPipeline *first_pipeline;  // depth into level 0
	SDL_GPUComputePipeline *build_pipeline;   // level 0 into the buffer, each level into the next
	uint32_t width;                           // level 0
	uint32_t height;
	uint32_t level_count;
	bool ready;                               // built (last frame), for view_projection
	float view_projection[16];                // the camera's when it was built (as camera_t has it)
} hiz_t;

bool hiz_create(hiz_t *hiz);
void hiz_destroy(hiz_t *hiz);

// Builds the pyramid from the scene's depth (width x height), seen with
// view_projection (kept with it), outside any pass, in the frame's command
// buffer
void hiz_build(hiz_t *hiz, SDL_GPUTexture *depth_texture, uint32_t width, uint32_t height, const float view_projection[16]);

// The buffer the culling reads (always one, if hiz_create went well; what
// is in it only means something when ready)
SDL_GPUBuffer *hiz_buffer(const hiz_t *hiz);

#endif
