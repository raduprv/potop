// The GPU, through SDL_GPU (Vulkan on Linux, Android and Windows; Metal on
// Macs; D3D12 possible). One device for the whole game, kept in `gpu`
// below: the renderers draw into the passes the frame opens, and their
// uploads wait in a staging area until the next copy pass.
//
// A frame:
//   gpu_begin_frame         the command buffer, the window's texture
//   uploads                 gpu_upload_texture / gpu_upload_buffer: copied
//                           into the staging area, nothing reaches the GPU yet
//   gpu_flush_uploads       one copy pass with everything waiting (render
//                           and compute passes call it before they begin)
//   passes                  shadow maps, compute, the scene, the interface
//   gpu_end_frame           the final picture to the window (and a
//                           screenshot, if asked), submit
//
// Coordinates: clip space depth is 0..1 (not OpenGL's -1..1: see
// gpu_clip_matrix) and +Y is up; a texture's first row is its top, so a
// picture rendered into a texture is sampled with v = 0.5 - 0.5 * y.
//
// Shaders are GLSL, compiled to SPIR-V by `make shaders` into
// data/shaders/spirv/ (see the Makefile); their resources follow SDL_GPU's
// layout: vertex shaders take their textures from set 0 and uniform
// blocks from set 1, fragment shaders from sets 2 and 3 (common.glsl has
// macros for both).

#ifndef GPU_H
#define GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL.h>

typedef struct
{
	SDL_GPUTexture *texture;       // NULL: a buffer
	SDL_GPUBuffer *buffer;
	uint32_t level;
	uint32_t layer;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t depth;                // 3D textures: slices (0 or 1: one)
	uint32_t offset;               // into the buffer
	size_t staging_offset;
	size_t size;
} gpu_pending_upload_t;

typedef struct
{
	SDL_GPUDevice *device;
	SDL_Window *window;
	SDL_GPUTextureFormat swapchain_format;
	SDL_GPUTextureFormat depth_format;  // for the scene
	SDL_GPUTextureFormat shadow_depth_format; // for the shadow maps (depth_format, or 16 bits: render.shadow_depth_16_bit)
	// The scene's multisampling (render.msaa; render_target.h): its samples
	// per pixel, as the device can do them (1: none)
	SDL_GPUSampleCount scene_sample_count;
	uint32_t scene_samples;
	bool vsync;
	// Vulkan 1.1 asked for (SDL 3.4's Vulkan options; not with older SDL,
	// or if the device refused): the compute shaders' subgroup variants
	// (*.comp.subgroups.spv: one atomic a subgroup, not one a thread) may
	// be loaded
	bool vulkan_11;

	// The frame being recorded (NULL between frames)
	SDL_GPUCommandBuffer *command_buffer;
	// Work recorded between frames (the simulation, uploads), submitted
	// before the next frame begins or when someone needs its results
	SDL_GPUCommandBuffer *work_command_buffer;
	SDL_GPUTexture *swapchain_texture;  // NULL if the window cannot be drawn into now
	uint32_t window_width;
	uint32_t window_height;
	// The frame's picture, at the window's size: the scene goes in, the
	// interface on top, then it is copied to the window (and is what a
	// screenshot downloads). Drawn into straight (direct_present), it is
	// the window's own picture once gpu_final_target() asked for it; draw
	// into it only after that call
	SDL_GPUTexture *final_texture;
	SDL_GPUTextureFormat final_format;  // its format now: GPU_FINAL_FORMAT, or the window's straight
	uint32_t final_width;
	uint32_t final_height;
	SDL_GPUTexture *own_final_texture;  // the one copied to the window
	uint32_t own_final_width;
	uint32_t own_final_height;
	// The frame's picture drawn straight into the window's, not into our
	// own and copied there at the end: one full screen copy less (on a
	// laptop's shared memory or a phone's tiles worth a millisecond), but
	// the window's picture asked for before the post pass and the
	// interface, not after them (gpu_set_direct_present). Not for a frame
	// a screenshot is taken of (capture_next), nor where the window's
	// format is sRGB (the picture is already encoded)
	bool direct_present;
	bool direct_possible;
	bool capture_next;                  // the caller will want this frame's pixels: set before gpu_begin_frame
	bool frame_direct;                  // this frame is drawn straight
	bool frame_acquired;                // ...and the window's picture was asked for
	// Where in it the scene goes (pixels; a width of 0: all of it): the
	// editor's bars round it (gpu_set_scene_rect)
	int32_t scene_x;
	int32_t scene_y;
	int32_t scene_width;
	int32_t scene_height;

	// Uploads waiting for the next copy pass: their bytes in staging, in order
	uint8_t *staging;
	size_t staging_used;
	size_t staging_capacity;
	gpu_pending_upload_t *pending;
	int pending_count;
	int pending_capacity;
	SDL_GPUTexture **mipmap_requests;  // mipmaps to make after the uploads
	int mipmap_count;
	int mipmap_capacity;
	SDL_GPUTransferBuffer *transfer;
	uint32_t transfer_capacity;
	// ...and a small one for what a frame sends up (the interface's
	// vertices, a chunk, the objects touched): the big one grows to the
	// largest load ever staged (the textures at the start, 64 MB), and
	// being mapped cycled at every flush, several a frame, the driver was
	// asked for copies of all of that
	SDL_GPUTransferBuffer *transfer_small;

	// Samplers everyone uses
	SDL_GPUSampler *linear_repeat;      // trilinear, anisotropic (textures of things)
	SDL_GPUSampler *linear_clamp;
	SDL_GPUSampler *nearest_clamp;      // exact texels (maps of the world, integer textures)
	SDL_GPUSampler *shadow_compare;     // depth comparison, 2x2 filtered
	float maximum_anisotropy;
	// The same with its levels of detail biased (sharper: the buildings'
	// textures under the temporal anti-aliasing, which softens them;
	// gpu_biased_sampler), made for the bias asked last
	SDL_GPUSampler *linear_repeat_biased;
	float linear_repeat_bias;

	// Statistics
	uint64_t uploaded_bytes;            // since the start
} gpu_t;

extern gpu_t gpu;

// Makes the device and claims the window (NULL: no window, for the tests
// that run without one). False if no driver can do it (the game cannot run
// then)
bool gpu_create(SDL_Window *window, bool vsync, float anisotropy);
void gpu_destroy(void);
void gpu_set_vsync(bool vsync);
// The textures' sampler made again when the anisotropy changed (1..16)
void gpu_set_anisotropy(float anisotropy);

// The scene's multisampling: 1 (none), 2, 4 or 8 samples per pixel, or
// fewer if the device cannot do as many. Before the renderers make their
// pipelines (they are made for it: a change takes effect at the next start)
void gpu_set_scene_samples(int samples);
// The scene drawn into this part of the frame's picture (pixels from its
// top left), the rest left to the interface; a width of 0: all of it
void gpu_set_scene_rect(int32_t x, int32_t y, int32_t width, int32_t height);

// gpu.linear_repeat with its levels of detail biased by bias (negative:
// sharper); 0, or none made: gpu.linear_repeat itself
SDL_GPUSampler *gpu_biased_sampler(float bias);

// A shader from data/shaders/spirv/<name>.spv (name with its extension,
// "terrain.vert"), with the number of each kind of resource it uses
SDL_GPUShader *gpu_load_shader(const char *name, uint32_t samplers, uint32_t storage_buffers, uint32_t uniform_buffers);

typedef struct
{
	uint32_t samplers;
	uint32_t readonly_storage_buffers;
	uint32_t readwrite_storage_buffers;
	uint32_t uniform_buffers;
	uint32_t threads_x;            // its local size
	uint32_t readonly_storage_textures;
	uint32_t readwrite_storage_textures;
} gpu_compute_layout_t;

SDL_GPUComputePipeline *gpu_load_compute(const char *name, const gpu_compute_layout_t *layout);

// Textures: levels 0 means a full mipmap chain
SDL_GPUTexture *gpu_create_texture(SDL_GPUTextureType type, SDL_GPUTextureFormat format, uint32_t width, uint32_t height,
	uint32_t layers, uint32_t levels, SDL_GPUTextureUsageFlags usage);
SDL_GPUBuffer *gpu_create_buffer(SDL_GPUBufferUsageFlags usage, uint32_t size);
uint32_t gpu_mip_count(uint32_t width, uint32_t height);

// Uploads: the data is copied now (it can be freed at once) and reaches
// the GPU at the next flush. Rows are tightly packed
// A whole 3D texture's level 0 (slices one after the other)
void gpu_upload_volume(SDL_GPUTexture *texture, uint32_t width, uint32_t height, uint32_t depth, const void *data, size_t size);
void gpu_upload_texture(SDL_GPUTexture *texture, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
	const void *data, size_t size);
void gpu_upload_buffer(SDL_GPUBuffer *buffer, uint32_t offset, const void *data, size_t size);
void gpu_generate_mipmaps(SDL_GPUTexture *texture);
// Everything waiting goes to the GPU: recorded into gpu_commands()
void gpu_flush_uploads(void);

// Where GPU work goes now: the frame's command buffer while a frame is
// recorded, else the work command buffer (made when first asked for)
SDL_GPUCommandBuffer *gpu_commands(void);
// Submits the work recorded between frames (if any); with fence_out, a
// fence to wait for it (NULL if nothing was submitted; release it)
void gpu_submit_work(SDL_GPUFence **fence_out);

// The frame. begin: false if there is nothing to draw into (minimized);
// the passes are skipped then, but uploads still flush at the end
bool gpu_begin_frame(void);
// The final picture goes to the window; with pixels_out, it is also
// downloaded (RGBA8, rows from the top, width * height * 4 bytes, SDL_free
// it), which waits for the GPU
void gpu_end_frame(uint8_t **pixels_out, uint32_t *width_out, uint32_t *height_out);

// Draws the frame straight into the window's picture (see direct_present)
void gpu_set_direct_present(bool direct);
// The frame's picture, to draw into now (the window's own drawn straight:
// asked for here, at the first call); NULL if there is none (minimized).
// gpu.final_format is the format to draw with
SDL_GPUTexture *gpu_final_target(void);

// The frame's own picture's format (the passes into it made for it, and
// for gpu.swapchain_format when drawn straight)
#define GPU_FINAL_FORMAT SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
// Formats the passes draw into: the scene is linear light in floats
// (brighter than white is kept, for post.h)
#define GPU_SCENE_COLOR_FORMAT SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
// The multisampled scene's second target: each sample's depth, which its
// resolve averages into the plain depth (render_target.h; scene_depth.glsl)
#define GPU_SCENE_DEPTH_VALUE_FORMAT SDL_GPU_TEXTUREFORMAT_R32_FLOAT

typedef enum
{
	GPU_BLEND_NONE = 0,
	GPU_BLEND_ALPHA,               // straight alpha (the interface)
	GPU_BLEND_PREMULTIPLIED,       // premultiplied: alpha 0 adds (particles)
	GPU_BLEND_ADD,                 // added to what is there (the color only; negative takes away)
	GPU_BLEND_MULTIPLY             // what is there times the color (the color only: contact shadows)
} gpu_blend_t;

// Most of what a graphics pipeline needs, with the game's usual defaults
// for the rest: triangle lists, counterclockwise front faces, depth 0..1
typedef struct
{
	SDL_GPUShader *vertex_shader;
	SDL_GPUShader *fragment_shader;
	const SDL_GPUVertexBufferDescription *vertex_buffers;
	uint32_t vertex_buffer_count;
	const SDL_GPUVertexAttribute *vertex_attributes;
	uint32_t vertex_attribute_count;
	SDL_GPUPrimitiveType primitive;       // 0: triangle list
	SDL_GPUCullMode cull;
	SDL_GPUFillMode fill;
	bool depth_test;
	bool depth_write;
	SDL_GPUCompareOp depth_compare;       // 0: less or equal
	float depth_bias_constant;            // polygon offset (shadow maps)
	float depth_bias_slope;
	gpu_blend_t blend;
	bool depth_only;                      // a shadow map: no color target
	SDL_GPUTextureFormat color_format;    // 0: the scene's
	bool no_depth;                        // the interface: no depth target
	// More color targets after the first, unblended, all of this format
	// (the water's rays for its reflections)
	uint32_t extra_color_target_count;
	SDL_GPUTextureFormat extra_color_format;
	// Drawn in the scene's first part (the land, the things on it, the
	// sky: render_target.h): with multisampling, gpu.scene_sample_count
	// samples and the depth value target after the color (its shaders
	// write it: scene_depth.glsl). Nothing changes without multisampling
	bool multisampled;
	// ... and the cut out leaves' edges smoothed by their alpha (its
	// shaders write their coverage as the alpha: alpha_cutout in
	// scene_depth.glsl)
	bool alpha_to_coverage;
	// depth_only: a depth target of this format (0: the shadow maps')
	SDL_GPUTextureFormat depth_format;
} gpu_pipeline_info_t;

SDL_GPUGraphicsPipeline *gpu_make_pipeline(const gpu_pipeline_info_t *info, const char *name);

// OpenGL style clip space (depth -1..1, what math3d makes) to SDL_GPU's
// (depth 0..1): for matrices that place vertices on the screen or in a
// shadow map. Matrices only used to look things up keep OpenGL's
void gpu_clip_matrix(const float gl[16], float out[16]);

#endif
