#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include <stdbool.h>

// What the machine is doing, for the debug window (F3): the processor (this
// process's share, the whole system's, its temperature) and the graphics
// card (its load, its memory, its power, its temperature). Read by a thread
// of its own once a second, and only while someone asks (the debug window
// open): NVIDIA's cards through NVML (libnvidia-ml, loaded if it is there),
// AMD's through the amdgpu driver's files. Linux only: elsewhere nothing is
// known. Whatever cannot be read is left unknown (its flag false).

typedef struct system_sample_t
{
	bool process_cpu_known;
	float process_cpu_percent;     // this process, in percent of one core (above 100: more than one)
	bool system_cpu_known;
	float system_cpu_percent;      // every core together, 0..100
	int cpu_threads;               // the processor's logical cores
	bool cpu_temperature_known;
	float cpu_celsius;
	bool gpu_load_known;
	float gpu_load_percent;
	bool gpu_memory_known;
	float gpu_memory_used_megabytes;
	float gpu_memory_total_megabytes;
	bool gpu_power_known;
	float gpu_watts;
	bool gpu_temperature_known;
	float gpu_celsius;
	const char *gpu_source;        // where the card's numbers come from ("NVML", "amdgpu"), NULL: nowhere
} system_sample_t;

// The latest sample (false: none yet). Asking also keeps the sampling going
// for the next couple of seconds; the first call starts the thread
bool system_monitor_get(system_sample_t *sample);

// The thread stopped (at the end; harmless if it never started)
void system_monitor_stop(void);

#endif
