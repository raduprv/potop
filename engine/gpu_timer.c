// GPU pass timing. See gpu_timer.h.

#include "gpu_timer.h"

#include <SDL3/SDL.h>

void gpu_timer_create(gpu_timer_t *timer)
{
	SDL_zerop(timer);
	timer->available = false;
}

void gpu_timer_destroy(gpu_timer_t *timer)
{
	SDL_zerop(timer);
}

void gpu_timer_begin_frame(gpu_timer_t *timer)
{
	timer->frame_count++;
	timer->recent_frame_count++;
}

void gpu_timer_begin(gpu_timer_t *timer, gpu_pass_t pass)
{
	(void)timer;
	(void)pass;
}

void gpu_timer_end(gpu_timer_t *timer, gpu_pass_t pass)
{
	(void)timer;
	(void)pass;
}

double gpu_timer_average(const gpu_timer_t *timer, gpu_pass_t pass)
{
	return timer->frame_count > 0 ? timer->milliseconds_sum[pass] / (double)timer->frame_count : 0.0;
}

void gpu_timer_reset(gpu_timer_t *timer)
{
	SDL_zero(timer->milliseconds_sum);
	timer->frame_count = 0;
}

double gpu_timer_recent_average(const gpu_timer_t *timer, gpu_pass_t pass)
{
	return timer->recent_frame_count > 0 ? timer->recent_milliseconds_sum[pass] / (double)timer->recent_frame_count : 0.0;
}

void gpu_timer_reset_recent(gpu_timer_t *timer)
{
	SDL_zero(timer->recent_milliseconds_sum);
	timer->recent_frame_count = 0;
}

const char *gpu_timer_pass_name(gpu_pass_t pass)
{
	static const char *names[GPU_PASS_COUNT] = { "shadow", "near shadow", "terrain", "objects", "sky", "water", "upscale" };

	return (pass >= 0 && pass < GPU_PASS_COUNT) ? names[pass] : "?";
}
