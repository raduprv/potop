// What the machine is doing, for the debug window (see system_monitor.h)

#include "system_monitor.h"

#include <SDL3/SDL.h>

#ifdef __linux__

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// How often it samples, and for how long after the last ask it keeps on
static const Uint64 SAMPLE_INTERVAL_NS = 1000000000ull;
static const Uint64 ASKED_LATELY_SECONDS = 3;

// NVML's, as nvml.h has them (only what is used): 0 is success
typedef struct nvml_utilization_t
{
	unsigned int gpu;
	unsigned int memory;
} nvml_utilization_t;

typedef struct nvml_memory_t
{
	unsigned long long total;
	unsigned long long free;
	unsigned long long used;
} nvml_memory_t;

typedef int (*nvml_init_function_t)(void);
typedef int (*nvml_shutdown_function_t)(void);
typedef int (*nvml_device_by_index_function_t)(unsigned int index, void **device);
typedef int (*nvml_utilization_function_t)(void *device, nvml_utilization_t *utilization);
typedef int (*nvml_memory_function_t)(void *device, nvml_memory_t *memory);
typedef int (*nvml_power_function_t)(void *device, unsigned int *milliwatts);
typedef int (*nvml_temperature_function_t)(void *device, int sensor, unsigned int *celsius);

typedef struct nvml_t
{
	SDL_SharedObject *library;
	void *device;
	nvml_shutdown_function_t shutdown;
	nvml_utilization_function_t utilization;
	nvml_memory_function_t memory;
	nvml_power_function_t power;
	nvml_temperature_function_t temperature;
} nvml_t;

// Where things are, found once (empty: not there)
typedef struct monitor_paths_t
{
	char cpu_temperature[256];     // millidegrees
	char amd_busy[256];            // percent
	char amd_vram_used[256];       // bytes
	char amd_vram_total[256];      // bytes
	char amd_power[256];           // microwatts
	char amd_temperature[256];     // millidegrees
} monitor_paths_t;

typedef struct monitor_t
{
	SDL_Thread *thread;
	SDL_Mutex *mutex;
	SDL_AtomicInt quit;
	SDL_AtomicU32 asked_seconds;   // when last asked (SDL_GetTicks / 1000, plus one: 0 never)
	system_sample_t sample;
	bool sample_ready;
	bool start_failed;
} monitor_t;

static monitor_t monitor;

// A file's first number (false: no file, no number)
static bool read_number(const char *path, double *value)
{
	FILE *file;
	bool read;

	if (path[0] == '\0')
	{
		return false;
	}
	file = fopen(path, "r");
	if (file == NULL)
	{
		return false;
	}
	read = fscanf(file, "%lf", value) == 1;
	fclose(file);
	return read;
}

// A file's first line, without its end (false: no file)
static bool read_line(const char *path, char *line, size_t size)
{
	FILE *file = fopen(path, "r");
	bool read;

	if (file == NULL)
	{
		return false;
	}
	read = fgets(line, (int)size, file) != NULL;
	fclose(file);
	if (read)
	{
		line[strcspn(line, "\n")] = '\0';
	}
	return read;
}

static bool file_exists(const char *path)
{
	return access(path, R_OK) == 0;
}

// The processor's temperature: the hwmon of its driver (k10temp's Tctl on
// AMD, coretemp's package on Intel), else a thermal zone named for it (ARM)
static void find_cpu_temperature(monitor_paths_t *paths)
{
	static const char *const driver_names[] = { "k10temp", "zenpower", "coretemp", "cpu_thermal" };
	char path[256];
	char name[64];

	for (size_t d = 0; d < SDL_arraysize(driver_names); d++)
	{
		for (int i = 0; i < 32; i++)
		{
			SDL_snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
			if (read_line(path, name, sizeof(name)) && SDL_strcmp(name, driver_names[d]) == 0)
			{
				SDL_snprintf(paths->cpu_temperature, sizeof(paths->cpu_temperature), "/sys/class/hwmon/hwmon%d/temp1_input", i);
				return;
			}
		}
	}
	for (int i = 0; i < 32; i++)
	{
		SDL_snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);
		if (read_line(path, name, sizeof(name)) && (SDL_strstr(name, "cpu") != NULL || SDL_strcmp(name, "x86_pkg_temp") == 0))
		{
			SDL_snprintf(paths->cpu_temperature, sizeof(paths->cpu_temperature), "/sys/class/thermal/thermal_zone%d/temp", i);
			return;
		}
	}
}

// An AMD card (amdgpu): its busy share, its memory, and in its hwmon its
// power and its (edge) temperature
static void find_amd_card(monitor_paths_t *paths)
{
	char device[128];
	char path[256];

	for (int card = 0; card < 8; card++)
	{
		DIR *directory;
		struct dirent *entry;

		SDL_snprintf(device, sizeof(device), "/sys/class/drm/card%d/device", card);
		SDL_snprintf(path, sizeof(path), "%s/gpu_busy_percent", device);
		if (!file_exists(path))
		{
			continue;
		}
		SDL_strlcpy(paths->amd_busy, path, sizeof(paths->amd_busy));
		SDL_snprintf(paths->amd_vram_used, sizeof(paths->amd_vram_used), "%s/mem_info_vram_used", device);
		SDL_snprintf(paths->amd_vram_total, sizeof(paths->amd_vram_total), "%s/mem_info_vram_total", device);
		SDL_snprintf(path, sizeof(path), "%s/hwmon", device);
		directory = opendir(path);
		if (directory == NULL)
		{
			return;
		}
		while ((entry = readdir(directory)) != NULL)
		{
			if (SDL_strncmp(entry->d_name, "hwmon", 5) != 0)
			{
				continue;
			}
			// (older kernels: the average; newer ones on some cards: the
			// input only)
			SDL_snprintf(paths->amd_power, sizeof(paths->amd_power), "%s/hwmon/%s/power1_average", device, entry->d_name);
			if (!file_exists(paths->amd_power))
			{
				SDL_snprintf(paths->amd_power, sizeof(paths->amd_power), "%s/hwmon/%s/power1_input", device, entry->d_name);
			}
			SDL_snprintf(paths->amd_temperature, sizeof(paths->amd_temperature), "%s/hwmon/%s/temp1_input", device, entry->d_name);
			break;
		}
		closedir(directory);
		return;
	}
}

// NVML loaded and the first card's handle taken (false: no NVIDIA driver)
static bool nvml_open(nvml_t *nvml)
{
	nvml_init_function_t init;
	nvml_device_by_index_function_t device_by_index;

	SDL_zerop(nvml);
	nvml->library = SDL_LoadObject("libnvidia-ml.so.1");
	if (nvml->library == NULL)
	{
		return false;
	}
	init = (nvml_init_function_t)SDL_LoadFunction(nvml->library, "nvmlInit_v2");
	device_by_index = (nvml_device_by_index_function_t)SDL_LoadFunction(nvml->library, "nvmlDeviceGetHandleByIndex_v2");
	nvml->shutdown = (nvml_shutdown_function_t)SDL_LoadFunction(nvml->library, "nvmlShutdown");
	nvml->utilization = (nvml_utilization_function_t)SDL_LoadFunction(nvml->library, "nvmlDeviceGetUtilizationRates");
	nvml->memory = (nvml_memory_function_t)SDL_LoadFunction(nvml->library, "nvmlDeviceGetMemoryInfo");
	nvml->power = (nvml_power_function_t)SDL_LoadFunction(nvml->library, "nvmlDeviceGetPowerUsage");
	nvml->temperature = (nvml_temperature_function_t)SDL_LoadFunction(nvml->library, "nvmlDeviceGetTemperature");
	if (init == NULL || device_by_index == NULL || init() != 0)
	{
		SDL_UnloadObject(nvml->library);
		SDL_zerop(nvml);
		return false;
	}
	if (device_by_index(0, &nvml->device) != 0)
	{
		if (nvml->shutdown != NULL)
		{
			nvml->shutdown();
		}
		SDL_UnloadObject(nvml->library);
		SDL_zerop(nvml);
		return false;
	}
	return true;
}

static void nvml_close(nvml_t *nvml)
{
	if (nvml->library == NULL)
	{
		return;
	}
	if (nvml->shutdown != NULL)
	{
		nvml->shutdown();
	}
	SDL_UnloadObject(nvml->library);
	SDL_zerop(nvml);
}

static void sample_nvml(nvml_t *nvml, system_sample_t *sample)
{
	nvml_utilization_t utilization;
	nvml_memory_t memory;
	unsigned int milliwatts;
	unsigned int celsius;

	sample->gpu_source = "NVML";
	if (nvml->utilization != NULL && nvml->utilization(nvml->device, &utilization) == 0)
	{
		sample->gpu_load_known = true;
		sample->gpu_load_percent = (float)utilization.gpu;
	}
	if (nvml->memory != NULL && nvml->memory(nvml->device, &memory) == 0)
	{
		sample->gpu_memory_known = true;
		sample->gpu_memory_used_megabytes = (float)((double)memory.used / (1024.0 * 1024.0));
		sample->gpu_memory_total_megabytes = (float)((double)memory.total / (1024.0 * 1024.0));
	}
	if (nvml->power != NULL && nvml->power(nvml->device, &milliwatts) == 0)
	{
		sample->gpu_power_known = true;
		sample->gpu_watts = (float)milliwatts / 1000.0f;
	}
	// (sensor 0: the GPU's own)
	if (nvml->temperature != NULL && nvml->temperature(nvml->device, 0, &celsius) == 0)
	{
		sample->gpu_temperature_known = true;
		sample->gpu_celsius = (float)celsius;
	}
}

static void sample_amd(const monitor_paths_t *paths, system_sample_t *sample)
{
	double value;
	double total;

	if (paths->amd_busy[0] == '\0')
	{
		return;
	}
	sample->gpu_source = "amdgpu";
	if (read_number(paths->amd_busy, &value))
	{
		sample->gpu_load_known = true;
		sample->gpu_load_percent = (float)value;
	}
	if (read_number(paths->amd_vram_used, &value) && read_number(paths->amd_vram_total, &total))
	{
		sample->gpu_memory_known = true;
		sample->gpu_memory_used_megabytes = (float)(value / (1024.0 * 1024.0));
		sample->gpu_memory_total_megabytes = (float)(total / (1024.0 * 1024.0));
	}
	if (read_number(paths->amd_power, &value))
	{
		sample->gpu_power_known = true;
		sample->gpu_watts = (float)(value / 1000000.0);
	}
	if (read_number(paths->amd_temperature, &value))
	{
		sample->gpu_temperature_known = true;
		sample->gpu_celsius = (float)(value / 1000.0);
	}
}

// This process's processor time so far (seconds; false: unreadable). In
// The core the process's first thread (the game's main one) last ran on:
// /proc/self/stat's 39th field; -1: not known
static int main_thread_core(void)
{
	char text[1024];
	FILE *file = fopen("/proc/self/stat", "r");
	size_t length;
	char *at;
	int field = 2;

	if (file == NULL)
	{
		return -1;
	}
	length = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[length] = '\0';
	at = strrchr(text, ')');
	if (at == NULL)
	{
		return -1;
	}
	at++;
	// (each field after the name follows a space)
	while (*at != '\0')
	{
		while (*at == ' ')
		{
			at++;
		}
		field++;
		if (field == 39)
		{
			return SDL_atoi(at);
		}
		while (*at != '\0' && *at != ' ')
		{
			at++;
		}
	}
	return -1;
}

// The cores' clocks (the cpufreq files, kHz): the main thread's core's
// now and at its most, the fastest of all now
static void sample_cpu_frequency(system_sample_t *sample, int cores)
{
	char path[128];
	double value;
	int core = main_thread_core();
	double fastest = 0.0;

	for (int k = 0; k < cores && k < 512; k++)
	{
		SDL_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", k);
		if (!read_number(path, &value))
		{
			continue;
		}
		fastest = SDL_max(fastest, value);
		if (k == core)
		{
			sample->cpu_main_megahertz = (float)(value / 1000.0);
			sample->cpu_frequency_known = true;
		}
	}
	sample->cpu_fastest_megahertz = (float)(fastest / 1000.0);
	if (sample->cpu_frequency_known)
	{
		SDL_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", core);
		sample->cpu_main_most_megahertz = read_number(path, &value) ? (float)(value / 1000.0) : 0.0f;
	}
}

// /proc/self/stat after the name (in parentheses, may hold spaces): the
// user and system times are the 12th and 13th fields
static bool process_cpu_seconds(double *seconds)
{
	char text[1024];
	FILE *file = fopen("/proc/self/stat", "r");
	size_t length;
	const char *after_name;
	unsigned long long user_ticks;
	unsigned long long system_ticks;
	long ticks_per_second = sysconf(_SC_CLK_TCK);

	if (file == NULL)
	{
		return false;
	}
	length = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[length] = '\0';
	after_name = strrchr(text, ')');
	if (after_name == NULL || ticks_per_second <= 0)
	{
		return false;
	}
	if (sscanf(after_name + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &user_ticks, &system_ticks) != 2)
	{
		return false;
	}
	*seconds = (double)(user_ticks + system_ticks) / (double)ticks_per_second;
	return true;
}

// The whole system's processor ticks so far, busy and all (from /proc/stat)
static bool system_cpu_ticks(unsigned long long *busy, unsigned long long *total)
{
	unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
	char line[256];

	if (!read_line("/proc/stat", line, sizeof(line)))
	{
		return false;
	}
	if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4)
	{
		return false;
	}
	*total = user + nice + system + idle + iowait + irq + softirq + steal;
	*busy = *total - idle - iowait;
	return true;
}

static int monitor_thread(void *data)
{
	monitor_paths_t paths;
	nvml_t nvml;
	bool nvml_tried = false;
	double previous_process_seconds = 0.0;
	bool previous_process_known = false;
	unsigned long long previous_busy = 0;
	unsigned long long previous_total = 0;
	bool previous_system_known = false;
	Uint64 previous_time = 0;

	(void)data;
	SDL_zero(paths);
	SDL_zero(nvml);
	find_cpu_temperature(&paths);
	while (SDL_GetAtomicInt(&monitor.quit) == 0)
	{
		Uint64 now = SDL_GetTicksNS();
		Uint32 asked = SDL_GetAtomicU32(&monitor.asked_seconds);

		// Nobody looking: nothing read (and the counters start over, the
		// time since the last sample too long to mean anything)
		if (asked == 0 || now / 1000000000ull + 1u > (Uint64)asked + ASKED_LATELY_SECONDS)
		{
			previous_process_known = false;
			previous_system_known = false;
			SDL_Delay(100);
			continue;
		}
		// The card's source found the first time someone looks
		if (!nvml_tried)
		{
			nvml_tried = true;
			if (!nvml_open(&nvml))
			{
				find_amd_card(&paths);
			}
		}
		{
			system_sample_t sample;
			double process_seconds;
			unsigned long long busy;
			unsigned long long total;
			double value;
			double elapsed = previous_time > 0 ? (double)(now - previous_time) / 1e9 : 0.0;

			SDL_zero(sample);
			sample.cpu_threads = SDL_GetNumLogicalCPUCores();
			if (process_cpu_seconds(&process_seconds))
			{
				if (previous_process_known && elapsed > 0.0)
				{
					sample.process_cpu_known = true;
					sample.process_cpu_percent = (float)(100.0 * (process_seconds - previous_process_seconds) / elapsed);
				}
				previous_process_seconds = process_seconds;
				previous_process_known = true;
			}
			if (system_cpu_ticks(&busy, &total))
			{
				if (previous_system_known && total > previous_total)
				{
					sample.system_cpu_known = true;
					sample.system_cpu_percent = (float)(100.0 * (double)(busy - previous_busy) / (double)(total - previous_total));
				}
				previous_busy = busy;
				previous_total = total;
				previous_system_known = true;
			}
			if (read_number(paths.cpu_temperature, &value))
			{
				sample.cpu_temperature_known = true;
				sample.cpu_celsius = (float)(value / 1000.0);
			}
			sample_cpu_frequency(&sample, sample.cpu_threads);
			if (nvml.library != NULL)
			{
				sample_nvml(&nvml, &sample);
			}
			else
			{
				sample_amd(&paths, &sample);
			}
			previous_time = now;
			SDL_LockMutex(monitor.mutex);
			monitor.sample = sample;
			monitor.sample_ready = true;
			SDL_UnlockMutex(monitor.mutex);
		}
		// Till the next one (in short sleeps: quitting is not kept waiting)
		while (SDL_GetAtomicInt(&monitor.quit) == 0 && SDL_GetTicksNS() - now < SAMPLE_INTERVAL_NS)
		{
			SDL_Delay(50);
		}
	}
	nvml_close(&nvml);
	return 0;
}

bool system_monitor_get(system_sample_t *sample)
{
	bool ready;

	if (monitor.thread == NULL)
	{
		if (monitor.start_failed)
		{
			return false;
		}
		monitor.mutex = SDL_CreateMutex();
		SDL_SetAtomicInt(&monitor.quit, 0);
		monitor.thread = monitor.mutex != NULL ? SDL_CreateThread(monitor_thread, "system monitor", NULL) : NULL;
		if (monitor.thread == NULL)
		{
			SDL_Log("System monitor: no thread (%s)", SDL_GetError());
			monitor.start_failed = true;
			return false;
		}
	}
	SDL_SetAtomicU32(&monitor.asked_seconds, (Uint32)(SDL_GetTicks() / 1000u) + 1u);
	SDL_LockMutex(monitor.mutex);
	ready = monitor.sample_ready;
	*sample = monitor.sample;
	SDL_UnlockMutex(monitor.mutex);
	return ready;
}

void system_monitor_stop(void)
{
	if (monitor.thread == NULL)
	{
		return;
	}
	SDL_SetAtomicInt(&monitor.quit, 1);
	SDL_WaitThread(monitor.thread, NULL);
	monitor.thread = NULL;
	SDL_DestroyMutex(monitor.mutex);
	monitor.mutex = NULL;
	monitor.sample_ready = false;
}

#else

bool system_monitor_get(system_sample_t *sample)
{
	SDL_zerop(sample);
	return false;
}

void system_monitor_stop(void)
{
}

#endif
