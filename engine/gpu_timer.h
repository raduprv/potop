// GPU time of each render pass. SDL_GPU has no timer queries (yet), so
// for now everything here quietly does nothing and reports zeros; the
// interface keeps its place for when it can measure again.

#ifndef GPU_TIMER_H
#define GPU_TIMER_H

#include <stdbool.h>
#include <stdint.h>


typedef enum
{
	GPU_PASS_SHADOW = 0,
	GPU_PASS_NEAR_SHADOW,
	GPU_PASS_TERRAIN,
	GPU_PASS_OBJECTS,
	GPU_PASS_SKY,
	GPU_PASS_WATER,
	GPU_PASS_UPSCALE,
	GPU_PASS_COUNT
} gpu_pass_t;

// Frames between issuing a query and reading it back
#define GPU_TIMER_LATENCY 4

typedef struct
{
	bool available;
	// Accumulated since the last reset
	double milliseconds_sum[GPU_PASS_COUNT];
	int frame_count;
	// The same over the last moments only (the status line, cleared each
	// time it is shown): the long average above follows a change slowly
	double recent_milliseconds_sum[GPU_PASS_COUNT];
	int recent_frame_count;
} gpu_timer_t;

void gpu_timer_create(gpu_timer_t *timer);
void gpu_timer_destroy(gpu_timer_t *timer);

// At the start of a frame: collects the results of the oldest frame.
void gpu_timer_begin_frame(gpu_timer_t *timer);

// Around one pass; passes must not nest.
void gpu_timer_begin(gpu_timer_t *timer, gpu_pass_t pass);
void gpu_timer_end(gpu_timer_t *timer, gpu_pass_t pass);

// Average milliseconds per frame of a pass since the last reset (a pass that
// runs only now and then, like the shadow map, is averaged over all frames).
double gpu_timer_average(const gpu_timer_t *timer, gpu_pass_t pass);
void gpu_timer_reset(gpu_timer_t *timer);

// Milliseconds per frame since gpu_timer_reset_recent, and that reset.
double gpu_timer_recent_average(const gpu_timer_t *timer, gpu_pass_t pass);
void gpu_timer_reset_recent(gpu_timer_t *timer);

const char *gpu_timer_pass_name(gpu_pass_t pass);

#endif
