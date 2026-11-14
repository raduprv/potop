// How many threads a parallel loop over tiles should use.
//
// Waking a thread and meeting it again at the end of a loop costs tens of
// microseconds or more (much more once the threads have gone to sleep, or
// the OS has parked them on slow cores), while one 64x64 tile takes about
// as long to simulate. With a dozen active tiles and sixteen threads most
// threads would get no tile at all and the loop would cost more than the
// work. So each thread gets at least a few tiles, and few tiles run on the
// calling thread alone.

#ifndef PARALLEL_H
#define PARALLEL_H

#include <stdint.h>

#ifdef _OPENMP
#include <omp.h>
#endif

// Each thread should get at least this many tiles
#define PARALLEL_MINIMUM_TILES_PER_THREAD 4

static inline int parallel_threads_for_tiles(int32_t tile_count)
{
#ifdef _OPENMP
	int wanted = (int)(tile_count / PARALLEL_MINIMUM_TILES_PER_THREAD);
	int available = omp_get_max_threads();

	return wanted < 1 ? 1 : (wanted > available ? available : wanted);
#else
	(void)tile_count;
	return 1;
#endif
}

// The same for work items of the given size relative to a tile: heavier
// items (a terrain chunk to prepare for upload is several times a tile's
// simulation) are worth a thread each.
static inline int parallel_threads_for_items(int32_t item_count, int32_t items_per_thread)
{
#ifdef _OPENMP
	int wanted = (int)(item_count / (items_per_thread < 1 ? 1 : items_per_thread));
	int available = omp_get_max_threads();

	return wanted < 1 ? 1 : (wanted > available ? available : wanted);
#else
	(void)item_count;
	(void)items_per_thread;
	return 1;
#endif
}

#endif
