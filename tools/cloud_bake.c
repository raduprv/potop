// cloud_bake: grows clouds in a small simulated atmosphere and bakes them
// into shapes for the volumetric clouds' atlas (see engine/cloud_weather.h).
//
// The simulation is the classic one for clouds (Harris 2003, after
// Miyazaki and others): air as an incompressible fluid on a grid, carrying
// heat (potential temperature), water vapor and cloud water.
//   - A patch of warm, humid ground feeds a thermal: warm air rises
//     (buoyancy), cools as it rises and expands, and where it cools below
//     its dew point the vapor condenses into cloud. Condensing releases
//     latent heat, which lifts the cloud further: that is how cumulus heap
//     up. The base is flat where the rising air first saturates.
//   - The atmosphere's temperature profile decides how far: a stable
//     layer (an inversion) caps the clouds, a steep one lets them tower.
//   - Turbulence (kept alive by vorticity confinement) makes the
//     cauliflower billows.
// Numerics: a staggered (MAC) grid; heat, vapor and cloud water carried by
// MacCormack advection (sharp), the flow itself semi-Lagrangian; vorticity
// confinement, its push capped so it cannot run away; pressure projection
// by red-black successive over-relaxation; all on every core.
//
// Out: the cloud water, thresholded into a signed distance field (the
// shader's atlas format: 0.5 at the surface, more inside), cropped so the
// cloud's base is at the bottom, CLOUD_SHAPE_TEXELS a side. And, with
// --preview, a picture of the cloud seen from the side and from above
// (PGM), to look at without the game.
//
//   cloud_bake [--type humilis|mediocris|congestus|storm] [--grid N]
//              [--seed N] [--count N] [--out-dir directory] [--name type]
//              [--vorticity strength] [--out file.cloud]
//              [--preview file.pgm] [--save-water file] [--water file]
//              [--show] [--no-view] [knobs]
// A window shows the cloud growing (unless --no-view, or there is no
// display): drag to turn it, the wheel zooms, Space pauses, Enter keeps the
// cloud as it is now, N drops it for the next seed, Esc stops. When all
// are done the window stays, with the last cloud, until Esc.
// The knobs change the type's atmosphere (cloud_bake --help lists them,
// --show prints the values a run would use and stops); --name writes the
// clouds as another type (<name>_<seed>.cloud), e.g. a storm grown from
// the congestus' atmosphere with a higher cap.
// --count grows that many clouds, seeds N, N + 1 ..., each written to
// <out-dir>/<type>_<seed>.cloud (out-dir: data/clouds unless given); the
// game takes up to CLOUD_SHAPES_PER_TYPE of each type, from seed 1 up.
// --save-water keeps the simulation's cloud water; --water bakes such a
// file again without simulating.

#include <SDL3/SDL.h>

#include <math.h>
#include <omp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// PNGs (stb_image_write, in this file only)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#include "vdb_read.h"
#include "miniz.h"
#pragma GCC diagnostic pop

// The atlas's shape format (engine/cloud_weather.h)
#define SHAPE_TEXELS 64
#define SHAPE_TEXELS_UP 48
#define SHAPE_DISTANCE_RANGE 0.1f

#define GRAVITY 9.81f
#define GAS_CONSTANT 287.0f       // dry air, J / (kg K)
#define VAPOR_GAS_CONSTANT 461.5f
#define HEAT_CAPACITY 1004.0f     // dry air at constant pressure, J / (kg K)
#define LATENT_HEAT 2.5e6f        // of condensation, J / kg
#define SURFACE_PRESSURE 100000.0f
// Cloud water over this (kg / kg) is cloud
#define CLOUD_THRESHOLD 1e-4f
// The vorticity confinement's push at most (m/s^2): a 1.5 K warm thermal
// lifts with about 0.05
#define MAXIMUM_CONFINEMENT 0.15f

typedef struct
{
	const char *name;
	float cell_meters;            // at the 128 cell grid (scaled for others)
	float surface_temperature;    // K
	float lapse_low;              // K / m, below the condensation level
	float lapse_high;             // K / m, above it
	float inversion_height;       // meters: from here up the air is stable...
	float inversion_lapse;        // ...cooling this much (negative: warming)
	float surface_humidity;       // relative, of the ambient air
	float top_humidity;           // relative, at the top
	float source_humidity;        // relative, of the thermal's air
	float source_heating;         // K: how much warmer than the air around its air is
	float source_radius;          // of the domain's width
	float source_seconds;
	float seconds;                // simulated
	float wind_shear;             // m / s per km of height, along x (anvils)
	float burst_period;           // seconds between changes of the warm patches
	float patch_meters;           // how big the warm patches are
	float height_ratio;           // the domain's height, of its width
	float sponge;                 // the top share of the domain where the flow calms down
} atmosphere_t;

static const atmosphere_t atmospheres[] =
{
	// Fair weather: a stable layer close above the base flattens them
	{ "humilis", 12.0f, 293.0f, 0.0095f, 0.0065f, 700.0f, -0.002f, 0.7f, 0.4f, 0.92f, 1.0f, 0.18f, 500.0f, 700.0f, 0.0f, 60.0f, 300.0f, 0.75f, 0.125f },
	// Heaps: a higher cap
	{ "mediocris", 15.0f, 293.0f, 0.0095f, 0.007f, 1000.0f, -0.002f, 0.75f, 0.6f, 0.92f, 2.0f, 0.3f, 900.0f, 1100.0f, 0.0f, 60.0f, 300.0f, 0.75f, 0.125f },
	// Towers: no cap within the domain, steeper lapse aloft
	{ "congestus", 15.0f, 296.0f, 0.0095f, 0.0078f, 5000.0f, 0.0f, 0.75f, 0.45f, 0.93f, 2.0f, 0.14f, 900.0f, 1100.0f, 0.0f, 60.0f, 300.0f, 0.75f, 0.125f },
	// Storm: very unstable to a hard cap high up, wind aloft spreading the anvil
	{ "storm", 30.0f, 300.0f, 0.0095f, 0.0082f, 2400.0f, -0.003f, 0.8f, 0.5f, 0.95f, 2.5f, 0.14f, 1400.0f, 1800.0f, 4.0f, 60.0f, 300.0f, 0.75f, 0.125f },
};

typedef struct
{
	int nx;
	int ny;                       // up
	int nz;
	float h;                      // cell size (m)
	float *u;                     // (nx + 1) * ny * nz, x faces
	float *v;                     // nx * (ny + 1) * nz, y faces
	float *w;                     // nx * ny * (nz + 1), z faces
	float *u_next;
	float *v_next;
	float *w_next;
	float *theta;                 // potential temperature (K), cell centers
	float *vapor;                 // kg / kg
	float *water;                 // cloud water, kg / kg
	float *scratch;
	float *scratch_second;
	float *pressure;              // projection potential (kept between steps)
	float *divergence;
	// The ambient atmosphere per level
	float *ambient_theta;
	float *ambient_vapor;
	float *exner;                 // (p / p0)^(R / cp)
	float *level_pressure;
} grid_t;

static uint32_t random_state = 12345u;

static inline float random_unit(void)
{
	uint32_t value = random_state = random_state * 747796405u + 2891336453u;

	value = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
	return (float)(((value >> 22u) ^ value) >> 8) * (1.0f / 16777216.0f);
}

static inline size_t index_u(const grid_t *grid, int i, int j, int k)
{
	return ((size_t)k * (size_t)grid->ny + (size_t)j) * (size_t)(grid->nx + 1) + (size_t)i;
}

static inline size_t index_v(const grid_t *grid, int i, int j, int k)
{
	return ((size_t)k * (size_t)(grid->ny + 1) + (size_t)j) * (size_t)grid->nx + (size_t)i;
}

static inline size_t index_w(const grid_t *grid, int i, int j, int k)
{
	return ((size_t)k * (size_t)grid->ny + (size_t)j) * (size_t)grid->nx + (size_t)i;
}

static inline size_t index_c(const grid_t *grid, int i, int j, int k)
{
	return ((size_t)k * (size_t)grid->ny + (size_t)j) * (size_t)grid->nx + (size_t)i;
}

static float saturation_vapor(float temperature, float pressure)
{
	float vapor_pressure = 611.2f * expf(17.67f * (temperature - 273.15f) / (temperature - 29.65f));

	return 0.622f * vapor_pressure / fmaxf(pressure - vapor_pressure, 1.0f);
}

// ---------------------------------------------------------------------------
// Sampling a staggered field: value i of the field sits at (i + offset) * h

static float sample(const float *field, int size_x, int size_y, int size_z, float offset_x, float offset_y, float offset_z, float x, float y,
	float z, float h)
{
	float fx = x / h - offset_x;
	float fy = y / h - offset_y;
	float fz = z / h - offset_z;
	int i;
	int j;
	int k;
	float tx;
	float ty;
	float tz;
	float result = 0.0f;

	fx = fminf(fmaxf(fx, 0.0f), (float)(size_x - 1) - 1e-4f);
	fy = fminf(fmaxf(fy, 0.0f), (float)(size_y - 1) - 1e-4f);
	fz = fminf(fmaxf(fz, 0.0f), (float)(size_z - 1) - 1e-4f);
	i = (int)fx;
	j = (int)fy;
	k = (int)fz;
	tx = fx - (float)i;
	ty = fy - (float)j;
	tz = fz - (float)k;
	for (int c = 0; c < 8; c++)
	{
		int di = c & 1;
		int dj = (c >> 1) & 1;
		int dk = (c >> 2) & 1;
		float weight = (di ? tx : 1.0f - tx) * (dj ? ty : 1.0f - ty) * (dk ? tz : 1.0f - tz);

		result += weight * field[((size_t)(k + dk) * (size_t)size_y + (size_t)(j + dj)) * (size_t)size_x + (size_t)(i + di)];
	}
	return result;
}

static void velocity_at(const grid_t *grid, const float *u, const float *v, const float *w, float x, float y, float z, float *out)
{
	out[0] = sample(u, grid->nx + 1, grid->ny, grid->nz, 0.0f, 0.5f, 0.5f, x, y, z, grid->h);
	out[1] = sample(v, grid->nx, grid->ny + 1, grid->nz, 0.5f, 0.0f, 0.5f, x, y, z, grid->h);
	out[2] = sample(w, grid->nx, grid->ny, grid->nz + 1, 0.5f, 0.5f, 0.0f, x, y, z, grid->h);
}

// ---------------------------------------------------------------------------
// The ambient atmosphere

static float ambient_temperature(const atmosphere_t *atmosphere, float height, float condensation_height)
{
	float temperature = atmosphere->surface_temperature;
	float below = fminf(height, condensation_height);

	temperature -= atmosphere->lapse_low * below;
	if (height > condensation_height)
	{
		float above = fminf(height, atmosphere->inversion_height) - condensation_height;

		temperature -= atmosphere->lapse_high * fmaxf(above, 0.0f);
	}
	if (height > atmosphere->inversion_height)
	{
		temperature -= atmosphere->inversion_lapse * (height - fmaxf(atmosphere->inversion_height, condensation_height));
	}
	return temperature;
}

static void make_atmosphere(grid_t *grid, const atmosphere_t *atmosphere)
{
	// Where the thermal's air saturates (a rule of thumb: 125 m per degree
	// between temperature and dew point)
	float dew_point;
	float condensation_height;
	float pressure = SURFACE_PRESSURE;
	{
		float t = atmosphere->surface_temperature - 273.15f;
		float gamma = logf(atmosphere->source_humidity) + 17.67f * t / (t + 243.5f);

		dew_point = 243.5f * gamma / (17.67f - gamma) + 273.15f;
		condensation_height = 125.0f * (atmosphere->surface_temperature - dew_point);
	}
	for (int j = 0; j < grid->ny; j++)
	{
		float height = ((float)j + 0.5f) * grid->h;
		float temperature = ambient_temperature(atmosphere, height, condensation_height);
		float below = ambient_temperature(atmosphere, height - grid->h, condensation_height);
		float humidity = atmosphere->surface_humidity
			+ (atmosphere->top_humidity - atmosphere->surface_humidity) * height / ((float)grid->ny * grid->h);

		// Hydrostatic: pressure falls by the weight of the layer below
		pressure *= expf(-GRAVITY * (j == 0 ? grid->h * 0.5f : grid->h) / (GAS_CONSTANT * 0.5f * (temperature + below)));
		grid->level_pressure[j] = pressure;
		grid->exner[j] = powf(pressure / SURFACE_PRESSURE, GAS_CONSTANT / HEAT_CAPACITY);
		grid->ambient_theta[j] = temperature / grid->exner[j];
		grid->ambient_vapor[j] = humidity * saturation_vapor(temperature, pressure);
	}
	printf("Condensation level about %.0f m, cap at %.0f m, domain %.0f x %.0f m\n", (double)condensation_height,
		(double)atmosphere->inversion_height, (double)(grid->nx * grid->h), (double)(grid->ny * grid->h));
#pragma omp parallel for
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				size_t c = index_c(grid, i, j, k);

				grid->theta[c] = grid->ambient_theta[j];
				grid->vapor[c] = grid->ambient_vapor[j];
				grid->water[c] = 0.0f;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The steps

// The smallest and largest of the eight values around a point (the
// MacCormack correction is kept between them)
static void neighborhood_range(const float *field, const grid_t *grid, float x, float y, float z, float *low_out, float *high_out)
{
	float fx = fminf(fmaxf(x / grid->h - 0.5f, 0.0f), (float)(grid->nx - 1) - 1e-4f);
	float fy = fminf(fmaxf(y / grid->h - 0.5f, 0.0f), (float)(grid->ny - 1) - 1e-4f);
	float fz = fminf(fmaxf(z / grid->h - 0.5f, 0.0f), (float)(grid->nz - 1) - 1e-4f);
	int i = (int)fx;
	int j = (int)fy;
	int k = (int)fz;
	float low = 1e30f;
	float high = -1e30f;

	for (int c = 0; c < 8; c++)
	{
		float value = field[index_c(grid, i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1))];

		low = fminf(low, value);
		high = fmaxf(high, value);
	}
	*low_out = low;
	*high_out = high;
}

// Carried with the flow, by MacCormack's method: a step back along the
// flow (the plain semi-Lagrangian way, which smooths), a step forward
// from there, and half the difference back as a correction for the
// smoothing; kept within the values it came from, so it cannot ring.
// Much less smoothing than the plain way: the billows keep their edges
static void advect_scalar(grid_t *grid, float *field, float dt)
{
	float *backward = grid->scratch;
	float *forward = grid->scratch_second;
	size_t cells = (size_t)grid->nx * (size_t)grid->ny * (size_t)grid->nz;

#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				float x = ((float)i + 0.5f) * grid->h;
				float y = ((float)j + 0.5f) * grid->h;
				float z = ((float)k + 0.5f) * grid->h;
				float velocity[3];

				velocity_at(grid, grid->u, grid->v, grid->w, x, y, z, velocity);
				backward[index_c(grid, i, j, k)] = sample(field, grid->nx, grid->ny, grid->nz, 0.5f, 0.5f, 0.5f, x - velocity[0] * dt,
					y - velocity[1] * dt, z - velocity[2] * dt, grid->h);
			}
		}
	}
#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				float x = ((float)i + 0.5f) * grid->h;
				float y = ((float)j + 0.5f) * grid->h;
				float z = ((float)k + 0.5f) * grid->h;
				float velocity[3];

				velocity_at(grid, grid->u, grid->v, grid->w, x, y, z, velocity);
				forward[index_c(grid, i, j, k)] = sample(backward, grid->nx, grid->ny, grid->nz, 0.5f, 0.5f, 0.5f, x + velocity[0] * dt,
					y + velocity[1] * dt, z + velocity[2] * dt, grid->h);
			}
		}
	}
#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				size_t c = index_c(grid, i, j, k);
				float x = ((float)i + 0.5f) * grid->h;
				float y = ((float)j + 0.5f) * grid->h;
				float z = ((float)k + 0.5f) * grid->h;
				float velocity[3];
				float low;
				float high;
				float corrected;

				velocity_at(grid, grid->u, grid->v, grid->w, x, y, z, velocity);
				neighborhood_range(field, grid, x - velocity[0] * dt, y - velocity[1] * dt, z - velocity[2] * dt, &low, &high);
				corrected = backward[c] + 0.5f * (field[c] - forward[c]);
				// The forward pass's result goes in place of the field: it is
				// no longer needed as its own buffer
				forward[c] = fminf(fmaxf(corrected, low), high);
			}
		}
	}
	memcpy(field, forward, sizeof(float) * cells);
}

static void advect_velocity(grid_t *grid, float dt)
{
	float h = grid->h;

#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			for (int i = 0; i <= grid->nx; i++)
			{
				float velocity[3];
				float x = (float)i * h;
				float y = ((float)j + 0.5f) * h;
				float z = ((float)k + 0.5f) * h;

				velocity_at(grid, grid->u, grid->v, grid->w, x, y, z, velocity);
				grid->u_next[index_u(grid, i, j, k)] = sample(grid->u, grid->nx + 1, grid->ny, grid->nz, 0.0f, 0.5f, 0.5f, x - velocity[0] * dt,
					y - velocity[1] * dt, z - velocity[2] * dt, h);
			}
		}
		for (int j = 0; j <= grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				float velocity[3];
				float x = ((float)i + 0.5f) * h;
				float y = (float)j * h;
				float z = ((float)k + 0.5f) * h;

				velocity_at(grid, grid->u, grid->v, grid->w, x, y, z, velocity);
				grid->v_next[index_v(grid, i, j, k)] = sample(grid->v, grid->nx, grid->ny + 1, grid->nz, 0.5f, 0.0f, 0.5f, x - velocity[0] * dt,
					y - velocity[1] * dt, z - velocity[2] * dt, h);
			}
		}
	}
#pragma omp parallel for schedule(static)
	for (int k = 0; k <= grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				float velocity[3];
				float x = ((float)i + 0.5f) * h;
				float y = ((float)j + 0.5f) * h;
				float z = (float)k * h;

				velocity_at(grid, grid->u, grid->v, grid->w, x, y, z, velocity);
				grid->w_next[index_w(grid, i, j, k)] = sample(grid->w, grid->nx, grid->ny, grid->nz + 1, 0.5f, 0.5f, 0.0f, x - velocity[0] * dt,
					y - velocity[1] * dt, z - velocity[2] * dt, h);
			}
		}
	}
	{
		float *swap = grid->u;
		grid->u = grid->u_next;
		grid->u_next = swap;
		swap = grid->v;
		grid->v = grid->v_next;
		grid->v_next = swap;
		swap = grid->w;
		grid->w = grid->w_next;
		grid->w_next = swap;
	}
}

// Vapor to cloud water where the air is supersaturated (and back where it
// is not), all at once (saturation adjustment), with the latent heat
static void condense(grid_t *grid)
{
#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			float exner = grid->exner[j];
			float pressure = grid->level_pressure[j];

			for (int i = 0; i < grid->nx; i++)
			{
				size_t c = index_c(grid, i, j, k);
				float temperature = grid->theta[c] * exner;
				float saturated = saturation_vapor(temperature, pressure);
				float excess = grid->vapor[c] - saturated;
				// The latent heat warms the air, which then holds more:
				// only this share of the excess condenses
				float share = 1.0f / (1.0f + LATENT_HEAT * LATENT_HEAT * saturated / (HEAT_CAPACITY * VAPOR_GAS_CONSTANT * temperature * temperature));
				float change = excess * share;

				if (change < 0.0f)
				{
					change = fmaxf(change, -grid->water[c]);
				}
				grid->vapor[c] -= change;
				grid->water[c] += change;
				grid->theta[c] += LATENT_HEAT / (HEAT_CAPACITY * exner) * change;
			}
		}
	}
}

// Warm air rises, moist air too (it is lighter), cloud water weighs it down
static void buoyancy(grid_t *grid, float dt)
{
#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 1; j < grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				size_t below = index_c(grid, i, j - 1, k);
				size_t above = index_c(grid, i, j, k);
				float b_below = (grid->theta[below] - grid->ambient_theta[j - 1]) / grid->ambient_theta[j - 1]
					+ 0.61f * (grid->vapor[below] - grid->ambient_vapor[j - 1]) - grid->water[below];
				float b_above = (grid->theta[above] - grid->ambient_theta[j]) / grid->ambient_theta[j]
					+ 0.61f * (grid->vapor[above] - grid->ambient_vapor[j]) - grid->water[above];

				grid->v[index_v(grid, i, j, k)] += GRAVITY * 0.5f * (b_below + b_above) * dt;
			}
		}
	}
}

// Turbulence that the coarse grid and the smoothing advection would lose,
// pushed back (vorticity confinement)
static void confine_vorticity(grid_t *grid, float strength, float dt)
{
	int nx = grid->nx;
	int ny = grid->ny;
	int nz = grid->nz;
	float h = grid->h;
	size_t count = (size_t)nx * ny * nz;
	float *curl = calloc(count * 4, sizeof(float));
	float *force = malloc(sizeof(float) * count * 3);

	if (curl == NULL || force == NULL)
	{
		free(curl);
		free(force);
		return;
	}
#pragma omp parallel for schedule(static)
	for (int k = 1; k < nz - 1; k++)
	{
		for (int j = 1; j < ny - 1; j++)
		{
			for (int i = 1; i < nx - 1; i++)
			{
				// Center velocities of the neighbors (averaged faces)
#define CU(a, b, c) (0.5f * (grid->u[index_u(grid, (a), (b), (c))] + grid->u[index_u(grid, (a) + 1, (b), (c))]))
#define CV(a, b, c) (0.5f * (grid->v[index_v(grid, (a), (b), (c))] + grid->v[index_v(grid, (a), (b) + 1, (c))]))
#define CW(a, b, c) (0.5f * (grid->w[index_w(grid, (a), (b), (c))] + grid->w[index_w(grid, (a), (b), (c) + 1)]))
				float wx = (CW(i, j + 1, k) - CW(i, j - 1, k) - CV(i, j, k + 1) + CV(i, j, k - 1)) / (2.0f * h);
				float wy = (CU(i, j, k + 1) - CU(i, j, k - 1) - CW(i + 1, j, k) + CW(i - 1, j, k)) / (2.0f * h);
				float wz = (CV(i + 1, j, k) - CV(i - 1, j, k) - CU(i, j + 1, k) + CU(i, j - 1, k)) / (2.0f * h);
#undef CU
#undef CV
#undef CW
				size_t c = index_c(grid, i, j, k) * 4;

				curl[c + 0] = wx;
				curl[c + 1] = wy;
				curl[c + 2] = wz;
				curl[c + 3] = sqrtf(wx * wx + wy * wy + wz * wz);
			}
		}
	}
	// The force at each cell, then half of each neighbor cell's to each face
	// (two passes: a face is shared by two cells)
#pragma omp parallel for schedule(static)
	for (int k = 0; k < nz; k++)
	{
		for (int j = 0; j < ny; j++)
		{
			for (int i = 0; i < nx; i++)
			{
				size_t c = index_c(grid, i, j, k);
				float gx;
				float gy;
				float gz;
				float length;

				if (i < 2 || j < 2 || k < 2 || i >= nx - 2 || j >= ny - 2 || k >= nz - 2)
				{
					force[c * 3 + 0] = 0.0f;
					force[c * 3 + 1] = 0.0f;
					force[c * 3 + 2] = 0.0f;
					continue;
				}
				gx = curl[index_c(grid, i + 1, j, k) * 4 + 3] - curl[index_c(grid, i - 1, j, k) * 4 + 3];
				gy = curl[index_c(grid, i, j + 1, k) * 4 + 3] - curl[index_c(grid, i, j - 1, k) * 4 + 3];
				gz = curl[index_c(grid, i, j, k + 1) * 4 + 3] - curl[index_c(grid, i, j, k - 1) * 4 + 3];
				length = sqrtf(gx * gx + gy * gy + gz * gz) + 1e-12f;
				gx /= length;
				gy /= length;
				gz /= length;
				{
					float fx = strength * h * (gy * curl[c * 4 + 2] - gz * curl[c * 4 + 1]);
					float fy = strength * h * (gz * curl[c * 4 + 0] - gx * curl[c * 4 + 2]);
					float fz = strength * h * (gx * curl[c * 4 + 1] - gy * curl[c * 4 + 0]);
					float magnitude = sqrtf(fx * fx + fy * fy + fz * fz);
					// Never more than a few times what the warmth lifts with:
					// stronger, it feeds itself and the flow blows up
					float limit = magnitude > MAXIMUM_CONFINEMENT ? MAXIMUM_CONFINEMENT / magnitude : 1.0f;

					force[c * 3 + 0] = fx * limit;
					force[c * 3 + 1] = fy * limit;
					force[c * 3 + 2] = fz * limit;
				}
			}
		}
	}
#pragma omp parallel for schedule(static)
	for (int k = 1; k < nz - 1; k++)
	{
		for (int j = 1; j < ny - 1; j++)
		{
			for (int i = 1; i < nx - 1; i++)
			{
				size_t c = index_c(grid, i, j, k);

				grid->u[index_u(grid, i, j, k)] += 0.5f * (force[c * 3 + 0] + force[index_c(grid, i - 1, j, k) * 3 + 0]) * dt;
				grid->v[index_v(grid, i, j, k)] += 0.5f * (force[c * 3 + 1] + force[index_c(grid, i, j - 1, k) * 3 + 1]) * dt;
				grid->w[index_w(grid, i, j, k)] += 0.5f * (force[c * 3 + 2] + force[index_c(grid, i, j, k - 1) * 3 + 2]) * dt;
			}
		}
	}
	free(force);
	free(curl);
}

// Walls all around: no flow through them
static void enforce_walls(grid_t *grid)
{
#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			grid->u[index_u(grid, 0, j, k)] = 0.0f;
			grid->u[index_u(grid, grid->nx, j, k)] = 0.0f;
		}
		for (int i = 0; i < grid->nx; i++)
		{
			grid->v[index_v(grid, i, 0, k)] = 0.0f;
			grid->v[index_v(grid, i, grid->ny, k)] = 0.0f;
		}
	}
#pragma omp parallel for schedule(static)
	for (int j = 0; j < grid->ny; j++)
	{
		for (int i = 0; i < grid->nx; i++)
		{
			grid->w[index_w(grid, i, j, 0)] = 0.0f;
			grid->w[index_w(grid, i, j, grid->nz)] = 0.0f;
		}
	}
}

// The flow made divergence free: solve for the potential whose gradient
// takes the divergence away, then subtract that gradient
static void project(grid_t *grid, int iterations)
{
	int nx = grid->nx;
	int ny = grid->ny;
	int nz = grid->nz;
	float h = grid->h;
	const float over_relaxation = 1.8f;

	enforce_walls(grid);
#pragma omp parallel for schedule(static)
	for (int k = 0; k < nz; k++)
	{
		for (int j = 0; j < ny; j++)
		{
			for (int i = 0; i < nx; i++)
			{
				grid->divergence[index_c(grid, i, j, k)] = (grid->u[index_u(grid, i + 1, j, k)] - grid->u[index_u(grid, i, j, k)]
					+ grid->v[index_v(grid, i, j + 1, k)] - grid->v[index_v(grid, i, j, k)] + grid->w[index_w(grid, i, j, k + 1)]
					- grid->w[index_w(grid, i, j, k)]) * h;
			}
		}
	}
	for (int iteration = 0; iteration < iterations; iteration++)
	{
		for (int color = 0; color < 2; color++)
		{
#pragma omp parallel for schedule(static)
			for (int k = 0; k < nz; k++)
			{
				for (int j = 0; j < ny; j++)
				{
					for (int i = (j + k + color) & 1; i < nx; i += 2)
					{
						size_t c = index_c(grid, i, j, k);
						float sum = 0.0f;
						int neighbors = 0;

						// Walls: no neighbor (the potential's gradient is 0 through them)
						if (i > 0) { sum += grid->pressure[c - 1]; neighbors++; }
						if (i < nx - 1) { sum += grid->pressure[c + 1]; neighbors++; }
						if (j > 0) { sum += grid->pressure[c - (size_t)nx]; neighbors++; }
						if (j < ny - 1) { sum += grid->pressure[c + (size_t)nx]; neighbors++; }
						if (k > 0) { sum += grid->pressure[c - (size_t)nx * ny]; neighbors++; }
						if (k < nz - 1) { sum += grid->pressure[c + (size_t)nx * ny]; neighbors++; }
						grid->pressure[c] += over_relaxation * ((sum - grid->divergence[c]) / (float)neighbors - grid->pressure[c]);
					}
				}
			}
		}
	}
#pragma omp parallel for schedule(static)
	for (int k = 0; k < nz; k++)
	{
		for (int j = 0; j < ny; j++)
		{
			for (int i = 1; i < nx; i++)
			{
				grid->u[index_u(grid, i, j, k)] -= (grid->pressure[index_c(grid, i, j, k)] - grid->pressure[index_c(grid, i - 1, j, k)]) / h;
			}
		}
		for (int j = 1; j < ny; j++)
		{
			for (int i = 0; i < nx; i++)
			{
				grid->v[index_v(grid, i, j, k)] -= (grid->pressure[index_c(grid, i, j, k)] - grid->pressure[index_c(grid, i, j - 1, k)]) / h;
			}
		}
		if (k > 0)
		{
			for (int j = 0; j < ny; j++)
			{
				for (int i = 0; i < nx; i++)
				{
					grid->w[index_w(grid, i, j, k)] -= (grid->pressure[index_c(grid, i, j, k)] - grid->pressure[index_c(grid, i, j, k - 1)]) / h;
				}
			}
		}
	}
}

// The thermals: the ground heats the air over it unevenly (a field of warm
// patches some hundreds of meters across, strongest towards the middle,
// changing every burst_period seconds), so the air rises in many plumes
// that merge into one heap of cloud, fed bubble after bubble (a steady
// source makes a steady plume: a column of cloud). The air near the
// ground is humid as source_humidity says over the warm patches

static uint32_t burst_seed = 1u;

// Smooth random value noise over the ground (period: cells per feature)
static float ground_noise(int i, int k, float period, uint32_t seed)
{
	float u = (float)i / period;
	float v = (float)k / period;
	int iu = (int)floorf(u);
	int iv = (int)floorf(v);
	float fu = u - (float)iu;
	float fv = v - (float)iv;
	float corners[4];

	fu = fu * fu * (3.0f - 2.0f * fu);
	fv = fv * fv * (3.0f - 2.0f * fv);
	for (int c = 0; c < 4; c++)
	{
		uint32_t state = (uint32_t)(iu + (c & 1)) * 73856093u ^ (uint32_t)(iv + (c >> 1)) * 19349663u ^ seed * 83492791u;

		state = state * 747796405u + 2891336453u;
		state = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		corners[c] = (float)((state >> 22u ^ state) >> 8) / 16777216.0f;
	}
	return (corners[0] + (corners[1] - corners[0]) * fu) * (1.0f - fv) + (corners[2] + (corners[3] - corners[2]) * fu) * fv;
}

static void feed_thermal(grid_t *grid, const atmosphere_t *atmosphere, float dt, float time)
{
	int burst = (int)(time / atmosphere->burst_period);
	float radius = atmosphere->source_radius * (float)grid->nx;
	int depth = (int)fmaxf(2.0f, 60.0f / grid->h);
	float patch_cells = atmosphere->patch_meters / grid->h;

	if (time >= atmosphere->source_seconds)
	{
		return;
	}
#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int i = 0; i < grid->nx; i++)
		{
			float dx = ((float)i + 0.5f - (float)grid->nx * 0.5f) / radius;
			float dz = ((float)k + 0.5f - (float)grid->nz * 0.5f) / radius;
			// Strongest in the middle, fading out over a few radii
			float middle = expf(-(dx * dx + dz * dz));
			float patches = ground_noise(i, k, patch_cells, burst_seed * 31u + (uint32_t)burst) * 0.7f
				+ ground_noise(i, k, patch_cells * 0.4f, burst_seed * 57u + (uint32_t)burst * 3u) * 0.3f;
			float warmth = fmaxf(patches - 0.45f, 0.0f) * 2.0f * middle;

			for (int j = 0; j < depth; j++)
			{
				size_t c = index_c(grid, i, j, k);
				// Humid as the source says, at the ambient temperature
				float ambient = grid->ambient_theta[j] * grid->exner[j];
				float target_vapor = atmosphere->source_humidity * saturation_vapor(ambient, grid->level_pressure[j]);
				float target_theta = grid->ambient_theta[j] + atmosphere->source_heating * warmth;
				float rate = fminf(0.05f * dt, 1.0f);

				grid->theta[c] += (target_theta - grid->theta[c]) * rate;
				grid->vapor[c] += (target_vapor - grid->vapor[c]) * rate * fminf(warmth * 2.0f, 1.0f);
			}
		}
	}
}

// Near the top and the walls the flow and the heat calm down, so nothing
// bounces back off them (a sponge)
static void sponge(grid_t *grid, const atmosphere_t *atmosphere, float dt)
{
	int layer = (int)fmaxf((float)grid->ny * atmosphere->sponge, 1.0f);
#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = grid->ny - layer; j < grid->ny; j++)
		{
			float damping = fminf(0.05f * dt * (float)(j - (grid->ny - layer)) / (float)layer, 1.0f);

			for (int i = 0; i < grid->nx; i++)
			{
				size_t c = index_c(grid, i, j, k);

				grid->u[index_u(grid, i, j, k)] *= 1.0f - damping;
				grid->v[index_v(grid, i, j, k)] *= 1.0f - damping;
				grid->w[index_w(grid, i, j, k)] *= 1.0f - damping;
				grid->theta[c] += (grid->ambient_theta[j] - grid->theta[c]) * damping;
			}
		}
	}
}

static void add_shear(grid_t *grid, const atmosphere_t *atmosphere, float dt)
{
	if (atmosphere->wind_shear == 0.0f)
	{
		return;
	}
	// The wind aloft blows across the domain's top; the cloud's top is
	// dragged towards it (relaxing u to the wind's profile)
#pragma omp parallel for schedule(static)
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			float height = ((float)j + 0.5f) * grid->h;
			float wind = atmosphere->wind_shear * height * 0.001f;

			for (int i = 1; i < grid->nx; i++)
			{
				float *u = &grid->u[index_u(grid, i, j, k)];

				*u += (wind - *u) * fminf(0.01f * dt, 1.0f);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Out

// The cloud water down to the atlas's shape (cubic texels, the cloud's base
// at the bottom), as a signed distance field in its format; the top (of the
// width) and how many texels are cloud
static float bake(const grid_t *grid, uint8_t *out, int *cloud_texels_out)
{
	int sx = SHAPE_TEXELS;
	int sy = SHAPE_TEXELS_UP;
	int sz = SHAPE_TEXELS;
	float scale = (float)grid->nx / (float)sx;      // grid cells per texel
	float *density = calloc((size_t)sx * sy * sz, sizeof(float));
	float *distance = malloc(sizeof(float) * (size_t)sx * sy * sz);
	int base = grid->ny;
	int top_texel = 0;
	int cloud_texels = 0;
	const int search = 8;

	// The cloud's box in the grid, its base at the shape's bottom, its
	// larger side across 85% of the shape's width (and its top within the
	// shape's reach)
	int low_x = grid->nx;
	int high_x = -1;
	int low_z = grid->nz;
	int high_z = -1;
	int high_y = -1;
	float cells_per_texel;
	float origin_x;
	float origin_z;

	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				if (grid->water[index_c(grid, i, j, k)] > CLOUD_THRESHOLD)
				{
					low_x = i < low_x ? i : low_x;
					high_x = i > high_x ? i : high_x;
					low_z = k < low_z ? k : low_z;
					high_z = k > high_z ? k : high_z;
					base = j < base ? j : base;
					high_y = j > high_y ? j : high_y;
				}
			}
		}
	}
	if (high_x < 0)
	{
		low_x = 0;
		high_x = grid->nx - 1;
		low_z = 0;
		high_z = grid->nz - 1;
		base = 0;
		high_y = grid->ny - 1;
	}
	{
		float across = (float)(high_x - low_x > high_z - low_z ? high_x - low_x : high_z - low_z) + 1.0f;
		float up = (float)(high_y - base) + 1.0f;

		cells_per_texel = fmaxf(across / (0.85f * (float)sx), up / (0.95f * (float)sy));
		origin_x = 0.5f * (float)(low_x + high_x + 1) - cells_per_texel * (float)sx * 0.5f;
		origin_z = 0.5f * (float)(low_z + high_z + 1) - cells_per_texel * (float)sz * 0.5f;
	}
	(void)scale;
	// The cloud water at each texel's middle (trilinear)
#pragma omp parallel for schedule(static)
	for (int k = 0; k < sz; k++)
	{
		for (int j = 0; j < sy; j++)
		{
			for (int i = 0; i < sx; i++)
			{
				float x = (origin_x + ((float)i + 0.5f) * cells_per_texel) * grid->h;
				float y = ((float)base + ((float)j + 0.5f) * cells_per_texel) * grid->h;
				float z = (origin_z + ((float)k + 0.5f) * cells_per_texel) * grid->h;
				bool outside = x < 0.0f || z < 0.0f || x > (float)grid->nx * grid->h || z > (float)grid->nz * grid->h
					|| y > (float)grid->ny * grid->h;

				density[((size_t)k * sy + (size_t)j) * sx + (size_t)i] = outside ? 0.0f
					: sample(grid->water, grid->nx, grid->ny, grid->nz, 0.5f, 0.5f, 0.5f, x, y, z, grid->h);
			}
		}
	}
	printf("Cloud: %.0f x %.0f m, %.0f m tall\n", (double)((float)(high_x - low_x + 1) * grid->h), (double)((float)(high_z - low_z + 1) * grid->h),
		(double)((float)(high_y - base + 1) * grid->h));
	// Distance (texels) to the nearest texel on the other side of the
	// surface, within the range that matters, half a texel off the surface
#pragma omp parallel for schedule(dynamic, 1) reduction(+ : cloud_texels) reduction(max : top_texel)
	for (int k = 0; k < sz; k++)
	{
		for (int j = 0; j < sy; j++)
		{
			for (int i = 0; i < sx; i++)
			{
				bool inside = density[((size_t)k * sy + (size_t)j) * sx + (size_t)i] > CLOUD_THRESHOLD;
				float nearest = (float)search;

				if (inside)
				{
					cloud_texels++;
					top_texel = j > top_texel ? j : top_texel;
				}
				for (int c = -search; c <= search; c++)
				{
					for (int b = -search; b <= search; b++)
					{
						for (int a = -search; a <= search; a++)
						{
							int ni = i + a;
							int nj = j + b;
							int nk = k + c;
							bool other;
							float d;

							if (ni < 0 || nj < 0 || nk < 0 || ni >= sx || nj >= sy || nk >= sz)
							{
								// Outside the shape's box is clear air
								other = inside;
							}
							else
							{
								other = (density[((size_t)nk * sy + (size_t)nj) * sx + (size_t)ni] > CLOUD_THRESHOLD) != inside;
							}
							if (!other)
							{
								continue;
							}
							d = sqrtf((float)(a * a + b * b + c * c));
							nearest = d < nearest ? d : nearest;
						}
					}
				}
				nearest -= 0.5f;
				distance[((size_t)k * sy + (size_t)j) * sx + (size_t)i] = inside ? -nearest : nearest;
			}
		}
	}
	for (size_t t = 0; t < (size_t)sx * sy * sz; t++)
	{
		// Texels to shape widths, then the atlas's encoding
		float d = distance[t] / (float)sx;
		int value = (int)((0.5f - d / SHAPE_DISTANCE_RANGE * 0.5f) * 255.0f + 0.5f);

		out[t] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
	}
	free(density);
	free(distance);
	*cloud_texels_out = cloud_texels;
	return (float)(top_texel + 1) / (float)sx;
}

// The cloud seen from the side (x, y) and from above (x, z): how much
// cloud water along each line, as a picture
static void write_preview(const grid_t *grid, const char *path)
{
	int width = grid->nx * 2 + 4;
	int height = grid->ny > grid->nz ? grid->ny : grid->nz;
	uint8_t *pixels = calloc((size_t)width * height, 1);
	FILE *file;

	for (int j = 0; j < grid->ny; j++)
	{
		for (int i = 0; i < grid->nx; i++)
		{
			float sum = 0.0f;

			for (int k = 0; k < grid->nz; k++)
			{
				sum += grid->water[index_c(grid, i, j, k)];
			}
			pixels[(size_t)(height - 1 - j) * width + (size_t)i] = (uint8_t)fminf(255.0f, 255.0f * (1.0f - expf(-sum * 400.0f)));
		}
	}
	for (int k = 0; k < grid->nz; k++)
	{
		for (int i = 0; i < grid->nx; i++)
		{
			float sum = 0.0f;

			for (int j = 0; j < grid->ny; j++)
			{
				sum += grid->water[index_c(grid, i, j, k)];
			}
			pixels[(size_t)k * width + (size_t)(grid->nx + 4 + i)] = (uint8_t)fminf(255.0f, 255.0f * (1.0f - expf(-sum * 400.0f)));
		}
	}
	file = fopen(path, "wb");
	if (file != NULL)
	{
		fprintf(file, "P5\n%d %d\n255\n", width, height);
		fwrite(pixels, 1, (size_t)width * height, file);
		fclose(file);
	}
	free(pixels);
}

// ---------------------------------------------------------------------------
// The same simulation on the GPU (data/shaders/cloud_sim.comp): every field
// in one storage buffer, a compute pass for each piece of a step. Some ten
// times faster than the CPU's; not the same numbers (it need not be), the
// same physics

#define MODE_FEED 0
#define MODE_ADVECT_VELOCITY 1
#define MODE_ADVECT_BACKWARD 2
#define MODE_ADVECT_FORWARD 3
#define MODE_ADVECT_CORRECT 4
#define MODE_CONDENSE 5
#define MODE_BUOYANCY 6
#define MODE_SHEAR 7
#define MODE_CURL 8
#define MODE_CONFINE_FORCE 9
#define MODE_CONFINE_APPLY 10
#define MODE_SPONGE 11
#define MODE_WALLS 12
#define MODE_DIVERGENCE 13
#define MODE_PRESSURE 14
#define MODE_GRADIENT 15
#define MODE_LIGHT 16
#define MODE_RENDER 17
#define MODE_SUN_DEPTH 18
#define MODE_SKY_DEPTH 19
#define MODE_RENDER_LIT 20

#define PRESSURE_ITERATIONS 40
// Extinction per meter for each kg of cloud water per kg of air (in the
// pictures only)
#define VIEW_EXTINCTION 60.0f
// The picture the GPU draws: the viewer's, and each of a PNG's three views
#define PICTURE_WIDTH 400
#define PICTURE_HEIGHT 300

// The shader's uniform block (std140: all vec4 sized)
typedef struct
{
	int32_t grid[4];
	float cell[4];
	int32_t velocity[4];
	int32_t velocity_next[4];
	int32_t scalars[4];
	int32_t temporary[4];
	int32_t solver[4];
	int32_t ambient[4];
	float thermal[4];
	float thermal_more[4];
	float wind[4];
	float view_origin[4];
	float view_right[4];
	float view_up[4];
	float view_forward[4];
} simulation_uniforms_t;

// The phases of a step, for the profile
typedef enum
{
	PHASE_ADVECTION = 0,      // thermals and carrying everything with the flow
	PHASE_PHYSICS,            // condensing, buoyancy, shear, the sponge
	PHASE_CONFINEMENT,
	PHASE_PRESSURE,
	PHASE_COUNT
} phase_t;

static const char *const phase_names[PHASE_COUNT] = { "advection", "physics", "vortices", "pressure" };

typedef struct
{
	SDL_GPUDevice *device;
	SDL_GPUComputePipeline *pipeline;
	SDL_GPUBuffer *fields;
	SDL_GPUTexture *picture;
	SDL_GPUTransferBuffer *transfer;
	uint32_t transfer_size;
	SDL_GPUCommandBuffer *commands;   // being recorded (NULL: none)
	simulation_uniforms_t uniforms;
	uint32_t total_floats;
	// Offsets (floats) of the fields in the buffer
	int32_t u;
	int32_t v;
	int32_t w;
	int32_t u_next;
	int32_t v_next;
	int32_t w_next;
	int32_t theta;
	int32_t vapor;
	int32_t water;
	int32_t light;
	int32_t temporary[3];
	int32_t pressure;
	int32_t divergence;
	int32_t curl;
	int32_t force;
	int32_t ambient_theta;
	int32_t ambient_vapor;
	int32_t exner;
	int32_t level_pressure;
	// The profile: wall time over the steps since the last flush, and the
	// phases' times measured now and then
	int steps_since_flush;
	bool view_only;               // only the water and its light (a volume to look at)
	int picture_width;
	int picture_height;
	// The lit render's sun (view_only): where it is, and where it was when
	// the cloud's light was worked out
	float sun_azimuth;
	float sun_elevation;
	float lit_azimuth;
	float lit_elevation;
	bool lit;
	float sun_light;              // how bright the sun, the sky, the picture
	float sky_light;
	float exposure;
	float thickness;              // how much thicker than the simulation's water a read cloud looks
	uint64_t flushed_ticks;
	double milliseconds_per_step;
	double phase_milliseconds[PHASE_COUNT];
} gpu_simulation_t;

static void *load_file(const char *path, size_t *size_out)
{
	FILE *file = fopen(path, "rb");
	void *data;
	long size;

	if (file == NULL)
	{
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc((size_t)size);
	if (data != NULL && fread(data, 1, (size_t)size, file) != (size_t)size)
	{
		free(data);
		data = NULL;
	}
	fclose(file);
	*size_out = (size_t)size;
	return data;
}

// The shader, from the data directory next to where the tool is run, or
// next to the tool itself
static SDL_GPUComputePipeline *load_simulation_pipeline(SDL_GPUDevice *device)
{
	const char *relative = "data/shaders/spirv/cloud_sim.comp.spv";
	char path[1024];
	size_t size = 0;
	void *code = load_file(relative, &size);
	SDL_GPUComputePipelineCreateInfo info;
	SDL_GPUComputePipeline *pipeline;

	if (code == NULL)
	{
		snprintf(path, sizeof(path), "%s../%s", SDL_GetBasePath(), relative);
		code = load_file(path, &size);
	}
	if (code == NULL)
	{
		fprintf(stderr, "No %s (make shaders, and run from the game's directory)\n", relative);
		return NULL;
	}
	memset(&info, 0, sizeof(info));
	info.code = code;
	info.code_size = size;
	info.entrypoint = "main";
	info.format = SDL_GPU_SHADERFORMAT_SPIRV;
	info.num_readwrite_storage_textures = 1;
	info.num_readwrite_storage_buffers = 1;
	info.num_uniform_buffers = 1;
	info.threadcount_x = 64;
	info.threadcount_y = 1;
	info.threadcount_z = 1;
	pipeline = SDL_CreateGPUComputePipeline(device, &info);
	free(code);
	if (pipeline == NULL)
	{
		fprintf(stderr, "The simulation shader: %s\n", SDL_GetError());
	}
	return pipeline;
}

static void gpu_simulation_destroy(gpu_simulation_t *simulation)
{
	if (simulation->device == NULL)
	{
		return;
	}
	if (simulation->transfer != NULL)
	{
		SDL_ReleaseGPUTransferBuffer(simulation->device, simulation->transfer);
	}
	if (simulation->picture != NULL)
	{
		SDL_ReleaseGPUTexture(simulation->device, simulation->picture);
	}
	if (simulation->fields != NULL)
	{
		SDL_ReleaseGPUBuffer(simulation->device, simulation->fields);
	}
	if (simulation->pipeline != NULL)
	{
		SDL_ReleaseGPUComputePipeline(simulation->device, simulation->pipeline);
	}
	memset(simulation, 0, sizeof(*simulation));
}

// The shader and the buffer laid out for the grid, on the device. False
// (and a word on stderr) if they cannot be made
// view_only: room for the water and its light alone (a volume read from a
// file, to look at, not to simulate)
static bool gpu_simulation_create(gpu_simulation_t *simulation, SDL_GPUDevice *device, const grid_t *grid, bool view_only)
{
	int32_t cells = grid->nx * grid->ny * grid->nz;
	int32_t u_size = (grid->nx + 1) * grid->ny * grid->nz;
	int32_t v_size = grid->nx * (grid->ny + 1) * grid->nz;
	int32_t w_size = grid->nx * grid->ny * (grid->nz + 1);
	int32_t offset = 0;

	memset(simulation, 0, sizeof(*simulation));
	simulation->device = device;
	simulation->view_only = view_only;
#define PLACE(field, size) do { simulation->field = offset; offset += (size); } while (0)
	if (view_only)
	{
		PLACE(water, cells);
		PLACE(light, cells);
		PLACE(temporary[0], cells);
		u_size = v_size = w_size = cells = 0;
	}
	PLACE(u, u_size);
	PLACE(v, v_size);
	PLACE(w, w_size);
	PLACE(u_next, u_size);
	PLACE(v_next, v_size);
	PLACE(w_next, w_size);
	PLACE(theta, cells);
	PLACE(vapor, cells);
	if (!view_only)
	{
		PLACE(water, cells);
		PLACE(light, cells);
	}
	PLACE(temporary[0], cells);
	PLACE(temporary[1], cells);
	PLACE(temporary[2], cells);
	PLACE(pressure, cells);
	PLACE(divergence, cells);
	PLACE(curl, cells * 4);
	PLACE(force, cells * 3);
	PLACE(ambient_theta, view_only ? 0 : grid->ny);
	PLACE(ambient_vapor, view_only ? 0 : grid->ny);
	PLACE(exner, view_only ? 0 : grid->ny);
	PLACE(level_pressure, view_only ? 0 : grid->ny);
#undef PLACE
	simulation->total_floats = (uint32_t)offset;
	simulation->pipeline = load_simulation_pipeline(simulation->device);
	{
		SDL_GPUBufferCreateInfo info;

		memset(&info, 0, sizeof(info));
		info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
		info.size = simulation->total_floats * (uint32_t)sizeof(float);
		simulation->fields = SDL_CreateGPUBuffer(simulation->device, &info);
	}
	{
		SDL_GPUTextureCreateInfo info;

		memset(&info, 0, sizeof(info));
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		// Bigger for a cloud to look at closely
		simulation->picture_width = view_only ? PICTURE_WIDTH * 2 : PICTURE_WIDTH;
		simulation->picture_height = view_only ? PICTURE_HEIGHT * 2 : PICTURE_HEIGHT;
		simulation->sun_azimuth = 0.9f;
		simulation->sun_elevation = 0.6f;
		simulation->sun_light = 4.0f;
		simulation->sky_light = 0.35f;
		simulation->exposure = 1.0f;
		simulation->thickness = 4.0f;
		info.width = (uint32_t)simulation->picture_width;
		info.height = (uint32_t)simulation->picture_height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		simulation->picture = SDL_CreateGPUTexture(simulation->device, &info);
	}
	{
		SDL_GPUTransferBufferCreateInfo info;

		memset(&info, 0, sizeof(info));
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		simulation->transfer_size = simulation->total_floats * (uint32_t)sizeof(float);
		info.size = simulation->transfer_size;
		simulation->transfer = SDL_CreateGPUTransferBuffer(simulation->device, &info);
	}
	if (simulation->pipeline == NULL || simulation->fields == NULL || simulation->picture == NULL || simulation->transfer == NULL)
	{
		fprintf(stderr, "GPU: %s\n", SDL_GetError());
		gpu_simulation_destroy(simulation);
		return false;
	}
	printf("GPU (%s): %.0f MB for the simulation\n", SDL_GetGPUDeviceDriver(simulation->device),
		(double)simulation->total_floats * 4.0 / 1048576.0);
	return true;
}

static SDL_GPUCommandBuffer *commands(gpu_simulation_t *simulation)
{
	if (simulation->commands == NULL)
	{
		simulation->commands = SDL_AcquireGPUCommandBuffer(simulation->device);
	}
	return simulation->commands;
}

// What is recorded goes to the GPU, and this waits for it
static void gpu_flush(gpu_simulation_t *simulation)
{
	SDL_GPUFence *fence;

	if (simulation->commands == NULL)
	{
		return;
	}
	fence = SDL_SubmitGPUCommandBufferAndAcquireFence(simulation->commands);
	simulation->commands = NULL;
	if (fence != NULL)
	{
		SDL_WaitForGPUFences(simulation->device, true, &fence, 1);
		SDL_ReleaseGPUFence(simulation->device, fence);
	}
}

// One dispatch over count threads, in a pass of its own (so it sees the
// one before it done)
static void dispatch(gpu_simulation_t *simulation, int mode, uint32_t count)
{
	SDL_GPUCommandBuffer *buffer = commands(simulation);
	SDL_GPUStorageTextureReadWriteBinding picture;
	SDL_GPUStorageBufferReadWriteBinding fields;
	SDL_GPUComputePass *pass;

	memset(&picture, 0, sizeof(picture));
	memset(&fields, 0, sizeof(fields));
	picture.texture = simulation->picture;
	fields.buffer = simulation->fields;
	simulation->uniforms.grid[3] = mode;
	SDL_PushGPUComputeUniformData(buffer, 0, &simulation->uniforms, sizeof(simulation->uniforms));
	pass = SDL_BeginGPUComputePass(buffer, &picture, 1, &fields, 1);
	SDL_BindGPUComputePipeline(pass, simulation->pipeline);
	{
		// Rows of work groups, for counts past what one row may hold
		uint32_t groups = (count + 63) / 64;
		uint32_t across = groups < 4096 ? (groups > 0 ? groups : 1) : 4096;

		SDL_DispatchGPUCompute(pass, across, (groups + across - 1) / across, 1);
	}
	SDL_EndGPUComputePass(pass);
}

// The uniforms' offsets from where the fields are now (they move as the
// ping-pong swaps them)
static void set_offsets(gpu_simulation_t *simulation)
{
	simulation_uniforms_t *uniforms = &simulation->uniforms;

	uniforms->velocity[0] = simulation->u;
	uniforms->velocity[1] = simulation->v;
	uniforms->velocity[2] = simulation->w;
	uniforms->velocity_next[0] = simulation->u_next;
	uniforms->velocity_next[1] = simulation->v_next;
	uniforms->velocity_next[2] = simulation->w_next;
	uniforms->scalars[0] = simulation->theta;
	uniforms->scalars[1] = simulation->vapor;
	uniforms->scalars[2] = simulation->water;
	uniforms->scalars[3] = simulation->light;
	uniforms->temporary[0] = simulation->temporary[0];
	uniforms->temporary[1] = simulation->temporary[1];
	uniforms->temporary[2] = simulation->temporary[2];
	uniforms->solver[0] = simulation->pressure;
	uniforms->solver[1] = simulation->divergence;
	uniforms->solver[2] = simulation->curl;
	uniforms->solver[3] = simulation->force;
	uniforms->ambient[0] = simulation->ambient_theta;
	uniforms->ambient[1] = simulation->ambient_vapor;
	uniforms->ambient[2] = simulation->exner;
	uniforms->ambient[3] = simulation->level_pressure;
}

static void copy_into(float *all, int32_t offset, const float *field, size_t count)
{
	memcpy(all + offset, field, sizeof(float) * count);
}

// The grid's state (fresh from make_atmosphere) up to the GPU
static void gpu_upload(gpu_simulation_t *simulation, const grid_t *grid)
{
	size_t cells = (size_t)grid->nx * grid->ny * grid->nz;
	float *all = SDL_MapGPUTransferBuffer(simulation->device, simulation->transfer, false);
	SDL_GPUCopyPass *pass;
	SDL_GPUTransferBufferLocation source;
	SDL_GPUBufferRegion destination;

	if (all == NULL)
	{
		return;
	}
	memset(all, 0, simulation->transfer_size);
	if (simulation->view_only)
	{
		copy_into(all, simulation->water, grid->water, cells);
	}
	else
	{
	copy_into(all, simulation->u, grid->u, (size_t)(grid->nx + 1) * grid->ny * grid->nz);
	copy_into(all, simulation->v, grid->v, (size_t)grid->nx * (grid->ny + 1) * grid->nz);
	copy_into(all, simulation->w, grid->w, (size_t)grid->nx * grid->ny * (grid->nz + 1));
	copy_into(all, simulation->theta, grid->theta, cells);
	copy_into(all, simulation->vapor, grid->vapor, cells);
	copy_into(all, simulation->water, grid->water, cells);
	copy_into(all, simulation->ambient_theta, grid->ambient_theta, (size_t)grid->ny);
	copy_into(all, simulation->ambient_vapor, grid->ambient_vapor, (size_t)grid->ny);
	copy_into(all, simulation->exner, grid->exner, (size_t)grid->ny);
	copy_into(all, simulation->level_pressure, grid->level_pressure, (size_t)grid->ny);
	}
	SDL_UnmapGPUTransferBuffer(simulation->device, simulation->transfer);
	memset(&source, 0, sizeof(source));
	memset(&destination, 0, sizeof(destination));
	source.transfer_buffer = simulation->transfer;
	destination.buffer = simulation->fields;
	destination.size = simulation->total_floats * (uint32_t)sizeof(float);
	pass = SDL_BeginGPUCopyPass(commands(simulation));
	SDL_UploadToGPUBuffer(pass, &source, &destination, false);
	SDL_EndGPUCopyPass(pass);
	gpu_flush(simulation);
	simulation->flushed_ticks = SDL_GetTicksNS();
	simulation->steps_since_flush = 0;
}

// The fields the CPU looks at (stats, baking) down from the GPU
static void gpu_download(gpu_simulation_t *simulation, grid_t *grid)
{
	size_t cells = (size_t)grid->nx * grid->ny * grid->nz;
	SDL_GPUTransferBufferCreateInfo info;
	SDL_GPUTransferBuffer *download;
	SDL_GPUCopyPass *pass;
	const float *all;

	memset(&info, 0, sizeof(info));
	info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	info.size = simulation->total_floats * (uint32_t)sizeof(float);
	download = SDL_CreateGPUTransferBuffer(simulation->device, &info);
	if (download == NULL)
	{
		return;
	}
	{
		SDL_GPUBufferRegion source;
		SDL_GPUTransferBufferLocation destination;

		memset(&source, 0, sizeof(source));
		memset(&destination, 0, sizeof(destination));
		source.buffer = simulation->fields;
		source.size = info.size;
		destination.transfer_buffer = download;
		pass = SDL_BeginGPUCopyPass(commands(simulation));
		SDL_DownloadFromGPUBuffer(pass, &source, &destination);
		SDL_EndGPUCopyPass(pass);
	}
	gpu_flush(simulation);
	all = SDL_MapGPUTransferBuffer(simulation->device, download, false);
	if (all != NULL)
	{
		memcpy(grid->v, all + simulation->v, sizeof(float) * (size_t)grid->nx * (grid->ny + 1) * grid->nz);
		memcpy(grid->theta, all + simulation->theta, sizeof(float) * cells);
		memcpy(grid->vapor, all + simulation->vapor, sizeof(float) * cells);
		memcpy(grid->water, all + simulation->water, sizeof(float) * cells);
		SDL_UnmapGPUTransferBuffer(simulation->device, download);
	}
	SDL_ReleaseGPUTransferBuffer(simulation->device, download);
}

static void swap_offsets(int32_t *a, int32_t *b)
{
	int32_t swap = *a;

	*a = *b;
	*b = swap;
}

static uint32_t face_space(const grid_t *grid)
{
	return (uint32_t)((grid->nx + 1) * (grid->ny + 1) * (grid->nz + 1));
}

// One step (see main's CPU loop for the same order). profile: each phase
// waited for and timed (slower: only now and then)
static void gpu_step(gpu_simulation_t *simulation, const grid_t *grid, const atmosphere_t *atmosphere, float vorticity, float dt, float time,
	bool profile)
{
	simulation_uniforms_t *uniforms = &simulation->uniforms;
	uint32_t cells = (uint32_t)(grid->nx * grid->ny * grid->nz);
	uint32_t columns = (uint32_t)(grid->nx * grid->nz);
	uint64_t phase_start = SDL_GetTicksNS();

#define END_PHASE(phase) \
	do \
	{ \
		if (profile) \
		{ \
			gpu_flush(simulation); \
			simulation->phase_milliseconds[phase] = (double)(SDL_GetTicksNS() - phase_start) / 1e6; \
			phase_start = SDL_GetTicksNS(); \
		} \
	} while (0)

	if (profile)
	{
		gpu_flush(simulation);
		phase_start = SDL_GetTicksNS();
	}
	uniforms->grid[0] = grid->nx;
	uniforms->grid[1] = grid->ny;
	uniforms->grid[2] = grid->nz;
	uniforms->cell[0] = grid->h;
	uniforms->cell[1] = dt;
	uniforms->cell[2] = time;
	uniforms->cell[3] = vorticity;
	uniforms->thermal[0] = atmosphere->source_heating;
	uniforms->thermal[1] = atmosphere->source_humidity;
	uniforms->thermal[2] = atmosphere->source_radius * (float)grid->nx;
	uniforms->thermal[3] = atmosphere->patch_meters / grid->h;
	uniforms->thermal_more[0] = (float)(int)(time / atmosphere->burst_period);
	uniforms->thermal_more[1] = (float)burst_seed;
	uniforms->thermal_more[2] = time < atmosphere->source_seconds ? 1.0f : 0.0f;
	uniforms->thermal_more[3] = fmaxf((float)grid->ny * atmosphere->sponge, 1.0f);
	uniforms->wind[0] = atmosphere->wind_shear;
	uniforms->wind[1] = MAXIMUM_CONFINEMENT;
	set_offsets(simulation);

	dispatch(simulation, MODE_FEED, columns);
	dispatch(simulation, MODE_ADVECT_VELOCITY, face_space(grid));
	swap_offsets(&simulation->u, &simulation->u_next);
	swap_offsets(&simulation->v, &simulation->v_next);
	swap_offsets(&simulation->w, &simulation->w_next);
	set_offsets(simulation);
	{
		int32_t *scalars[3] = { &simulation->theta, &simulation->vapor, &simulation->water };

		for (int s = 0; s < 3; s++)
		{
			uniforms->velocity[3] = *scalars[s];
			dispatch(simulation, MODE_ADVECT_BACKWARD, cells);
			dispatch(simulation, MODE_ADVECT_FORWARD, cells);
			dispatch(simulation, MODE_ADVECT_CORRECT, cells);
			// The corrected field is the field now
			swap_offsets(scalars[s], &simulation->temporary[2]);
			set_offsets(simulation);
		}
	}
	END_PHASE(PHASE_ADVECTION);
	dispatch(simulation, MODE_CONDENSE, cells);
	dispatch(simulation, MODE_BUOYANCY, face_space(grid));
	if (atmosphere->wind_shear != 0.0f)
	{
		dispatch(simulation, MODE_SHEAR, face_space(grid));
	}
	END_PHASE(PHASE_PHYSICS);
	if (vorticity > 0.0f)
	{
		dispatch(simulation, MODE_CURL, cells);
		dispatch(simulation, MODE_CONFINE_FORCE, cells);
		dispatch(simulation, MODE_CONFINE_APPLY, face_space(grid));
	}
	END_PHASE(PHASE_CONFINEMENT);
	dispatch(simulation, MODE_SPONGE, cells);
	dispatch(simulation, MODE_WALLS, face_space(grid));
	dispatch(simulation, MODE_DIVERGENCE, cells);
	for (int iteration = 0; iteration < PRESSURE_ITERATIONS; iteration++)
	{
		for (int color = 0; color < 2; color++)
		{
			uniforms->velocity_next[3] = color;
			dispatch(simulation, MODE_PRESSURE, cells);
		}
	}
	dispatch(simulation, MODE_GRADIENT, face_space(grid));
	END_PHASE(PHASE_PRESSURE);
#undef END_PHASE
	simulation->steps_since_flush++;
}

// Submitting what the steps recorded, and how long they took a step
static void gpu_finish_steps(gpu_simulation_t *simulation)
{
	uint64_t now;

	if (simulation->steps_since_flush == 0)
	{
		return;
	}
	gpu_flush(simulation);
	now = SDL_GetTicksNS();
	simulation->milliseconds_per_step = (double)(now - simulation->flushed_ticks) / 1e6 / (double)simulation->steps_since_flush;
	simulation->flushed_ticks = now;
	simulation->steps_since_flush = 0;
}

// The cloud into the picture, seen from (yaw, pitch), zoom of the whole
// domain across
// A cloud to look at, lit by a sun anywhere and by the sky: its light is
// worked out again only when the sun moves (in pieces, each waited for: at
// full resolution it is a lot of work for one go)
static void gpu_render_lit(gpu_simulation_t *simulation, const grid_t *grid)
{
	simulation_uniforms_t *uniforms = &simulation->uniforms;
	uint32_t cells = (uint32_t)(grid->nx * grid->ny * grid->nz);

	uniforms->thermal[0] = sinf(simulation->sun_azimuth) * cosf(simulation->sun_elevation);
	uniforms->thermal[1] = sinf(simulation->sun_elevation);
	uniforms->thermal[2] = cosf(simulation->sun_azimuth) * cosf(simulation->sun_elevation);
	uniforms->thermal[3] = 1.0f;
	uniforms->thermal_more[0] = simulation->sky_light;
	uniforms->thermal_more[1] = simulation->sun_light;
	uniforms->thermal_more[2] = simulation->exposure;
	uniforms->wind[2] = VIEW_EXTINCTION * simulation->thickness;
	if (!simulation->lit || simulation->lit_azimuth != simulation->sun_azimuth || simulation->lit_elevation != simulation->sun_elevation)
	{
		const uint32_t piece = 1u << 21;

		for (uint32_t first = 0; first < cells; first += piece)
		{
			uniforms->velocity[3] = (int32_t)first;
			dispatch(simulation, MODE_SUN_DEPTH, cells - first < piece ? cells - first : piece);
			gpu_flush(simulation);
		}
		uniforms->velocity[3] = 0;
		dispatch(simulation, MODE_SKY_DEPTH, (uint32_t)(grid->nx * grid->nz));
		simulation->lit = true;
		simulation->lit_azimuth = simulation->sun_azimuth;
		simulation->lit_elevation = simulation->sun_elevation;
	}
	dispatch(simulation, MODE_RENDER_LIT, (uint32_t)(simulation->picture_width * simulation->picture_height));
}

static void gpu_render(gpu_simulation_t *simulation, const grid_t *grid, float yaw, float pitch, float zoom)
{
	simulation_uniforms_t *uniforms = &simulation->uniforms;
	float center[3] = { (float)grid->nx * grid->h * 0.5f, (float)grid->ny * grid->h * 0.5f, (float)grid->nz * grid->h * 0.5f };
	float radius = sqrtf(center[0] * center[0] + center[1] * center[1] + center[2] * center[2]);
	float forward[3] = { -sinf(yaw) * cosf(pitch), -sinf(pitch), -cosf(yaw) * cosf(pitch) };
	float right[3] = { cosf(yaw), 0.0f, -sinf(yaw) };
	float up[3] = { right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
		right[0] * forward[1] - right[1] * forward[0] };
	float half_width = radius * zoom;
	float half_height = half_width * (float)simulation->picture_height / (float)simulation->picture_width;

	if (up[1] < 0.0f)
	{
		for (int a = 0; a < 3; a++)
		{
			up[a] = -up[a];
		}
	}
	for (int a = 0; a < 3; a++)
	{
		uniforms->view_origin[a] = center[a] - forward[a] * radius * 1.1f;
		uniforms->view_right[a] = right[a] * half_width;
		uniforms->view_up[a] = up[a] * half_height;
		uniforms->view_forward[a] = forward[a];
	}
	uniforms->view_forward[3] = radius * 2.2f;
	uniforms->wind[2] = VIEW_EXTINCTION;
	uniforms->wind[3] = grid->h;
	uniforms->grid[0] = grid->nx;
	uniforms->grid[1] = grid->ny;
	uniforms->grid[2] = grid->nz;
	uniforms->cell[0] = grid->h;
	set_offsets(simulation);
	if (simulation->view_only)
	{
		gpu_render_lit(simulation, grid);
		return;
	}
	dispatch(simulation, MODE_LIGHT, (uint32_t)(grid->nx * grid->nz));
	dispatch(simulation, MODE_RENDER, (uint32_t)(simulation->picture_width * simulation->picture_height));
}

// The picture down to memory (RGBA, PICTURE_WIDTH x PICTURE_HEIGHT)
static bool gpu_read_picture(gpu_simulation_t *simulation, uint8_t *pixels_out)
{
	SDL_GPUTransferBufferCreateInfo info;
	SDL_GPUTransferBuffer *download;
	SDL_GPUCopyPass *pass;
	SDL_GPUTextureRegion source;
	SDL_GPUTextureTransferInfo destination;
	const uint8_t *mapped;

	memset(&info, 0, sizeof(info));
	info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	info.size = (uint32_t)(simulation->picture_width * simulation->picture_height * 4);
	download = SDL_CreateGPUTransferBuffer(simulation->device, &info);
	if (download == NULL)
	{
		return false;
	}
	memset(&source, 0, sizeof(source));
	memset(&destination, 0, sizeof(destination));
	source.texture = simulation->picture;
	source.w = (uint32_t)simulation->picture_width;
	source.h = (uint32_t)simulation->picture_height;
	source.d = 1;
	destination.transfer_buffer = download;
	pass = SDL_BeginGPUCopyPass(commands(simulation));
	SDL_DownloadFromGPUTexture(pass, &source, &destination);
	SDL_EndGPUCopyPass(pass);
	gpu_flush(simulation);
	mapped = SDL_MapGPUTransferBuffer(simulation->device, download, false);
	if (mapped != NULL)
	{
		memcpy(pixels_out, mapped, info.size);
		SDL_UnmapGPUTransferBuffer(simulation->device, download);
	}
	SDL_ReleaseGPUTransferBuffer(simulation->device, download);
	return mapped != NULL;
}

// The cloud seen from below, from the front and from above, side by side,
// as a PNG
static bool gpu_write_views(gpu_simulation_t *simulation, const grid_t *grid, const char *path)
{
	static const float pitches[3] = { -1.25f, 0.12f, 1.3f };
	int view_width = simulation->picture_width;
	int view_height = simulation->picture_height;
	int width = view_width * 3;
	uint8_t *image = malloc((size_t)width * view_height * 4);
	uint8_t *view = malloc((size_t)view_width * view_height * 4);
	bool ok = image != NULL && view != NULL;

	for (int v = 0; v < 3 && ok; v++)
	{
		gpu_render(simulation, grid, 0.6f, pitches[v], 0.75f);
		ok = gpu_read_picture(simulation, view);
		for (int y = 0; y < view_height && ok; y++)
		{
			memcpy(&image[((size_t)y * width + (size_t)v * view_width) * 4], &view[(size_t)y * view_width * 4], (size_t)view_width * 4);
		}
	}
	if (ok)
	{
		ok = stbi_write_png(path, width, view_height, 4, image, width * 4) != 0;
	}
	free(image);
	free(view);
	return ok;
}

// ---------------------------------------------------------------------------
// Watching it grow: a window with the cloud as it is, drawn every fraction
// of a second (lit from above, seen from any side), and keys to stop it

typedef enum
{
	VIEW_GO = 0,
	VIEW_SAVE_NOW,            // Enter: stop here, keep this cloud
	VIEW_SKIP,                // N: drop this one, on to the next seed
	VIEW_QUIT                 // Esc or closing the window: stop, keep nothing
} view_action_t;

typedef struct
{
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	uint32_t *pixels;
	int width;
	int height;
	float yaw;                // radians around the vertical
	float pitch;
	float zoom;
	bool dragging;
	bool paused;
	uint64_t last_draw;
	gpu_simulation_t *gpu;    // drawing on the GPU (NULL: the CPU draws)
} viewer_t;

#define VIEW_WIDTH 320
#define VIEW_HEIGHT 240
#define VIEW_SCALE 3

static bool viewer_open(viewer_t *viewer, gpu_simulation_t *gpu)
{
	memset(viewer, 0, sizeof(*viewer));
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "No window (%s): running without watching\n", SDL_GetError());
		return false;
	}
	viewer->yaw = 0.6f;
	viewer->pitch = 0.15f;
	viewer->zoom = 0.7f;
	if (gpu != NULL)
	{
		// The GPU draws the picture and stretches it over the window
		viewer->window = SDL_CreateWindow("cloud_bake", PICTURE_WIDTH * 2, PICTURE_HEIGHT * 2, SDL_WINDOW_RESIZABLE);
		if (viewer->window == NULL || !SDL_ClaimWindowForGPUDevice(gpu->device, viewer->window))
		{
			fprintf(stderr, "No window (%s): running without watching\n", SDL_GetError());
			if (viewer->window != NULL)
			{
				SDL_DestroyWindow(viewer->window);
				viewer->window = NULL;
			}
			return false;
		}
		viewer->gpu = gpu;
		viewer->width = PICTURE_WIDTH;
		viewer->height = PICTURE_HEIGHT;
		return true;
	}
	if (!SDL_CreateWindowAndRenderer("cloud_bake", VIEW_WIDTH * VIEW_SCALE, VIEW_HEIGHT * VIEW_SCALE, SDL_WINDOW_RESIZABLE, &viewer->window,
		&viewer->renderer))
	{
		fprintf(stderr, "No window (%s): running without watching\n", SDL_GetError());
		SDL_Quit();
		return false;
	}
	viewer->texture = SDL_CreateTexture(viewer->renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, VIEW_WIDTH, VIEW_HEIGHT);
	viewer->pixels = malloc(sizeof(uint32_t) * VIEW_WIDTH * VIEW_HEIGHT);
	viewer->width = VIEW_WIDTH;
	viewer->height = VIEW_HEIGHT;
	return viewer->texture != NULL && viewer->pixels != NULL;
}

static void viewer_close(viewer_t *viewer)
{
	if (viewer->window == NULL)
	{
		return;
	}
	if (viewer->gpu != NULL)
	{
		gpu_flush(viewer->gpu);
		SDL_ReleaseWindowFromGPUDevice(viewer->gpu->device, viewer->window);
		SDL_DestroyWindow(viewer->window);
		memset(viewer, 0, sizeof(*viewer));
		return;
	}
	free(viewer->pixels);
	SDL_DestroyTexture(viewer->texture);
	SDL_DestroyRenderer(viewer->renderer);
	SDL_DestroyWindow(viewer->window);
	memset(viewer, 0, sizeof(*viewer));
}

// The cloud seen from outside the domain, lit by a sun overhead (each
// cell's light: how much cloud water is above it)
// On the GPU: the picture drawn there, stretched over the window
static void viewer_draw_gpu(viewer_t *viewer, grid_t *grid, const char *title)
{
	gpu_simulation_t *gpu = viewer->gpu;
	SDL_GPUTexture *swapchain = NULL;
	uint32_t width = 0;
	uint32_t height = 0;

	gpu_render(gpu, grid, viewer->yaw, viewer->pitch, viewer->zoom);
	if (SDL_AcquireGPUSwapchainTexture(commands(gpu), viewer->window, &swapchain, &width, &height) && swapchain != NULL)
	{
		SDL_GPUBlitInfo blit;

		memset(&blit, 0, sizeof(blit));
		blit.source.texture = gpu->picture;
		blit.source.w = (uint32_t)viewer->gpu->picture_width;
		blit.source.h = (uint32_t)viewer->gpu->picture_height;
		blit.destination.texture = swapchain;
		blit.destination.w = width;
		blit.destination.h = height;
		blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter = SDL_GPU_FILTER_LINEAR;
		SDL_BlitGPUTexture(commands(gpu), &blit);
	}
	gpu_flush(gpu);
	SDL_SetWindowTitle(viewer->window, title);
	viewer->last_draw = SDL_GetTicks();
}

static void viewer_draw(viewer_t *viewer, grid_t *grid, const char *title)
{
	if (viewer->gpu != NULL)
	{
		viewer_draw_gpu(viewer, grid, title);
		return;
	}
	int nx = grid->nx;
	int ny = grid->ny;
	int nz = grid->nz;
	float h = grid->h;
	float *light = grid->scratch;
	float center[3] = { (float)nx * h * 0.5f, (float)ny * h * 0.5f, (float)nz * h * 0.5f };
	float radius = sqrtf(center[0] * center[0] + center[1] * center[1] + center[2] * center[2]);
	float forward[3] = { -sinf(viewer->yaw) * cosf(viewer->pitch), -sinf(viewer->pitch), -cosf(viewer->yaw) * cosf(viewer->pitch) };
	float right[3] = { cosf(viewer->yaw), 0.0f, -sinf(viewer->yaw) };
	float up[3] = { right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
		right[0] * forward[1] - right[1] * forward[0] };
	float half_width = radius * viewer->zoom;
	float half_height = half_width * (float)viewer->height / (float)viewer->width;
	float step = h * 1.5f;

	if (up[1] < 0.0f)
	{
		up[0] = -up[0];
		up[1] = -up[1];
		up[2] = -up[2];
	}
#pragma omp parallel for schedule(static)
	for (int k = 0; k < nz; k++)
	{
		for (int i = 0; i < nx; i++)
		{
			float above = 0.0f;

			for (int j = ny - 1; j >= 0; j--)
			{
				size_t c = index_c(grid, i, j, k);

				light[c] = expf(-above * VIEW_EXTINCTION * h);
				above += grid->water[c];
			}
		}
	}
#pragma omp parallel for schedule(dynamic, 4)
	for (int y = 0; y < viewer->height; y++)
	{
		for (int x = 0; x < viewer->width; x++)
		{
			float u = ((float)x + 0.5f) / (float)viewer->width * 2.0f - 1.0f;
			float v = 1.0f - ((float)y + 0.5f) / (float)viewer->height * 2.0f;
			float origin[3];
			float transmittance = 1.0f;
			float brightness = 0.0f;
			float sky = 0.55f + 0.25f * v;
			float red;
			float green;
			float blue;

			for (int a = 0; a < 3; a++)
			{
				origin[a] = center[a] - forward[a] * radius * 1.1f + right[a] * u * half_width + up[a] * v * half_height;
			}
			for (float t = 0.0f; t < radius * 2.2f && transmittance > 0.02f; t += step)
			{
				float px = origin[0] + forward[0] * t;
				float py = origin[1] + forward[1] * t;
				float pz = origin[2] + forward[2] * t;
				float water;
				float alpha;

				if (px < 0.0f || py < 0.0f || pz < 0.0f || px > (float)nx * h || py > (float)ny * h || pz > (float)nz * h)
				{
					continue;
				}
				water = sample(grid->water, nx, ny, nz, 0.5f, 0.5f, 0.5f, px, py, pz, h);
				if (water <= 1e-6f)
				{
					continue;
				}
				alpha = 1.0f - expf(-water * VIEW_EXTINCTION * step);
				brightness += transmittance * alpha
					* (0.35f + 0.65f * sample(light, nx, ny, nz, 0.5f, 0.5f, 0.5f, px, py, pz, h));
				transmittance *= 1.0f - alpha;
			}
			red = brightness + transmittance * sky * 0.55f;
			green = brightness + transmittance * sky * 0.7f;
			blue = brightness + transmittance * sky;
			viewer->pixels[y * viewer->width + x] = (uint32_t)(fminf(red, 1.0f) * 255.0f) << 16 | (uint32_t)(fminf(green, 1.0f) * 255.0f) << 8
				| (uint32_t)(fminf(blue, 1.0f) * 255.0f);
		}
	}
	SDL_UpdateTexture(viewer->texture, NULL, viewer->pixels, viewer->width * (int)sizeof(uint32_t));
	SDL_RenderClear(viewer->renderer);
	SDL_RenderTexture(viewer->renderer, viewer->texture, NULL, NULL);
	SDL_RenderPresent(viewer->renderer);
	SDL_SetWindowTitle(viewer->window, title);
	viewer->last_draw = SDL_GetTicks();
	{
		// For checking the picture without a screen: CLOUD_BAKE_VIEW_DUMP=file.ppm
		const char *dump = getenv("CLOUD_BAKE_VIEW_DUMP");
		FILE *file = dump != NULL ? fopen(dump, "wb") : NULL;

		if (file != NULL)
		{
			fprintf(file, "P6\n%d %d\n255\n", viewer->width, viewer->height);
			for (int p = 0; p < viewer->width * viewer->height; p++)
			{
				uint8_t rgb[3] = { (uint8_t)(viewer->pixels[p] >> 16), (uint8_t)(viewer->pixels[p] >> 8), (uint8_t)viewer->pixels[p] };

				fwrite(rgb, 1, 3, file);
			}
			fclose(file);
		}
	}
}

// The window's events (and a new picture, now and then or when asked);
// while paused it waits here
static view_action_t viewer_update(viewer_t *viewer, grid_t *grid, const char *title, bool force)
{
	view_action_t action = VIEW_GO;
	// The CPU's pictures are slow: a few a second; the GPU's often
	bool redraw = force || SDL_GetTicks() - viewer->last_draw > (viewer->gpu != NULL ? 40u : 300u);

	if (viewer->window == NULL)
	{
		return VIEW_GO;
	}
	do
	{
		SDL_Event event;

		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
			case SDL_EVENT_QUIT:
				return VIEW_QUIT;
			case SDL_EVENT_KEY_DOWN:
				if (event.key.key == SDLK_ESCAPE)
				{
					return VIEW_QUIT;
				}
				if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
				{
					return VIEW_SAVE_NOW;
				}
				if (event.key.key == SDLK_N)
				{
					return VIEW_SKIP;
				}
				if (event.key.key == SDLK_SPACE)
				{
					viewer->paused = !viewer->paused;
				}
				if (viewer->gpu != NULL && viewer->gpu->view_only)
				{
					// The arrows move the sun around and up
					gpu_simulation_t *gpu = viewer->gpu;

					if (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT)
					{
						gpu->sun_azimuth += event.key.key == SDLK_LEFT ? -0.2f : 0.2f;
						redraw = true;
					}
					if (event.key.key == SDLK_UP || event.key.key == SDLK_DOWN)
					{
						gpu->sun_elevation = fminf(fmaxf(gpu->sun_elevation + (event.key.key == SDLK_UP ? 0.1f : -0.1f), -0.2f), 1.55f);
						redraw = true;
					}
				}
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
				viewer->dragging = true;
				break;
			case SDL_EVENT_MOUSE_BUTTON_UP:
				viewer->dragging = false;
				break;
			case SDL_EVENT_MOUSE_MOTION:
				if (viewer->dragging)
				{
					viewer->yaw -= event.motion.xrel * 0.01f;
					viewer->pitch = fminf(fmaxf(viewer->pitch + event.motion.yrel * 0.01f, -1.4f), 1.4f);
					redraw = true;
				}
				break;
			case SDL_EVENT_MOUSE_WHEEL:
				viewer->zoom = fminf(fmaxf(viewer->zoom * (event.wheel.y > 0.0f ? 0.9f : 1.1f), 0.2f), 2.0f);
				redraw = true;
				break;
			default:
				break;
			}
		}
		if (redraw)
		{
			char paused_title[600];

			snprintf(paused_title, sizeof(paused_title), "%s%s", viewer->paused ? "PAUSED  " : "", title);
			viewer_draw(viewer, grid, paused_title);
			redraw = false;
		}
		if (viewer->paused)
		{
			SDL_Delay(15);
		}
	}
	while (viewer->paused);
	return action;
}

static float *allocate(size_t count)
{
	float *memory = calloc(count, sizeof(float));

	if (memory == NULL)
	{
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}
	return memory;
}

// The atmosphere's knobs on the command line, in the units people think in
// (scale turns them into the struct's); see print_atmosphere
typedef struct
{
	const char *option;
	size_t offset;
	float scale;
	float shift;
	const char *meaning;
} knob_t;

#define KNOB(option, field, scale, shift, meaning) { option, offsetof(atmosphere_t, field), scale, shift, meaning }

static const knob_t knobs[] =
{
	KNOB("--cell-meters", cell_meters, 1.0f, 0.0f, "cell size at --grid 128 (m; the domain is 128 of them across)"),
	KNOB("--ground-temperature", surface_temperature, 1.0f, 273.15f, "air temperature at the ground (C)"),
	KNOB("--lapse-low", lapse_low, 0.001f, 0.0f, "cooling with height below the cloud base (C per km; 9.8 is neutral for dry air)"),
	KNOB("--lapse-high", lapse_high, 0.001f, 0.0f, "cooling with height above the base (C per km; over about 6 rising cloud keeps rising)"),
	KNOB("--cap-height", inversion_height, 1.0f, 0.0f, "height of the stable layer that caps the clouds (m)"),
	KNOB("--cap-lapse", inversion_lapse, 0.001f, 0.0f, "cooling with height above the cap (C per km; negative: warming, a hard cap)"),
	KNOB("--humidity-low", surface_humidity, 0.01f, 0.0f, "relative humidity of the air at the ground (%)"),
	KNOB("--humidity-high", top_humidity, 0.01f, 0.0f, "relative humidity at the domain's top (%; drier air eats clouds' edges)"),
	KNOB("--thermal-humidity", source_humidity, 0.01f, 0.0f, "relative humidity of the thermals' air (%; higher: lower cloud base)"),
	KNOB("--thermal-warmth", source_heating, 1.0f, 0.0f, "how much warmer than the air around the thermals are (C)"),
	KNOB("--thermal-radius", source_radius, 0.01f, 0.0f, "how far from the middle the ground heats (% of the domain's width)"),
	KNOB("--thermal-seconds", source_seconds, 1.0f, 0.0f, "how long the ground heats (s)"),
	KNOB("--seconds", seconds, 1.0f, 0.0f, "how long the simulation runs (s)"),
	KNOB("--shear", wind_shear, 1.0f, 0.0f, "wind growing with height, along x (m/s per km; spreads anvils)"),
	KNOB("--burst-seconds", burst_period, 1.0f, 0.0f, "how often the warm patches change (s)"),
	KNOB("--patch-meters", patch_meters, 1.0f, 0.0f, "how big the warm patches are (m)"),
	KNOB("--height-ratio", height_ratio, 1.0f, 0.0f, "the domain's height, of its width"),
	KNOB("--sponge", sponge, 0.01f, 0.0f, "top share of the domain where the flow is calmed (%)"),
};

static void print_atmosphere(const atmosphere_t *atmosphere)
{
	printf("%s:\n", atmosphere->name);
	for (size_t k = 0; k < sizeof(knobs) / sizeof(knobs[0]); k++)
	{
		float value = *(const float *)((const char *)atmosphere + knobs[k].offset);

		printf("  %-22s %8.2f   %s\n", knobs[k].option, (double)((value - knobs[k].shift) / knobs[k].scale), knobs[k].meaning);
	}
}

// ---------------------------------------------------------------------------
// Runs: the grid, the settings files, one simulation

static void grid_free(grid_t *grid)
{
	free(grid->u);
	free(grid->v);
	free(grid->w);
	free(grid->u_next);
	free(grid->v_next);
	free(grid->w_next);
	free(grid->theta);
	free(grid->vapor);
	free(grid->water);
	free(grid->scratch);
	free(grid->scratch_second);
	free(grid->pressure);
	free(grid->divergence);
	free(grid->ambient_theta);
	free(grid->ambient_vapor);
	free(grid->exner);
	free(grid->level_pressure);
	memset(grid, 0, sizeof(*grid));
}

// A clean grid for the atmosphere: size cells across, as many up as its
// height_ratio says, cells of its cell_meters (at size 128)
static void grid_allocate(grid_t *grid, int size, const atmosphere_t *atmosphere)
{
	size_t cells;

	memset(grid, 0, sizeof(*grid));
	grid->nx = size;
	grid->ny = (int)fmaxf((float)size * atmosphere->height_ratio + 0.5f, 8.0f);
	grid->nz = size;
	grid->h = atmosphere->cell_meters * 128.0f / (float)size;
	cells = (size_t)grid->nx * grid->ny * grid->nz;
	grid->u = allocate((size_t)(grid->nx + 1) * grid->ny * grid->nz);
	grid->u_next = allocate((size_t)(grid->nx + 1) * grid->ny * grid->nz);
	grid->v = allocate((size_t)grid->nx * (grid->ny + 1) * grid->nz);
	grid->v_next = allocate((size_t)grid->nx * (grid->ny + 1) * grid->nz);
	grid->w = allocate((size_t)grid->nx * grid->ny * (grid->nz + 1));
	grid->w_next = allocate((size_t)grid->nx * grid->ny * (grid->nz + 1));
	grid->theta = allocate(cells);
	grid->vapor = allocate(cells);
	grid->water = allocate(cells);
	grid->scratch = allocate(cells);
	grid->scratch_second = allocate(cells);
	grid->pressure = allocate(cells);
	grid->divergence = allocate(cells);
	grid->ambient_theta = allocate((size_t)grid->ny);
	grid->ambient_vapor = allocate((size_t)grid->ny);
	grid->exner = allocate((size_t)grid->ny);
	grid->level_pressure = allocate((size_t)grid->ny);
}

// A knob by its name (without the dashes), in its command line units;
// false if there is no such knob
static bool set_knob(atmosphere_t *atmosphere, const char *name, float value)
{
	for (size_t k = 0; k < sizeof(knobs) / sizeof(knobs[0]); k++)
	{
		if (strcmp(knobs[k].option + 2, name) == 0)
		{
			*(float *)((char *)atmosphere + knobs[k].offset) = value * knobs[k].scale + knobs[k].shift;
			return true;
		}
	}
	return false;
}

static bool select_type(atmosphere_t *atmosphere, const char *name)
{
	for (size_t t = 0; t < sizeof(atmospheres) / sizeof(atmospheres[0]); t++)
	{
		if (strcmp(name, atmospheres[t].name) == 0)
		{
			*atmosphere = atmospheres[t];
			return true;
		}
	}
	fprintf(stderr, "Unknown type %s (humilis, mediocris, congestus, storm)\n", name);
	return false;
}

// "  name = value  ; comment" into its name and value (false: no such line)
static bool split_line(char *line, char **name_out, char **value_out)
{
	char *equals = strchr(line, '=');
	char *end;

	for (char *cut = line; *cut != '\0'; cut++)
	{
		if (*cut == ';' || *cut == '#')
		{
			*cut = '\0';
			break;
		}
	}
	equals = strchr(line, '=');
	if (equals == NULL)
	{
		return false;
	}
	*equals = '\0';
	*name_out = line;
	*value_out = equals + 1;
	while (**name_out == ' ' || **name_out == '\t')
	{
		(*name_out)++;
	}
	end = *name_out + strlen(*name_out);
	while (end > *name_out && (end[-1] == ' ' || end[-1] == '\t'))
	{
		*--end = '\0';
	}
	while (**value_out == ' ' || **value_out == '\t')
	{
		(*value_out)++;
	}
	end = *value_out + strlen(*value_out);
	while (end > *value_out && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
	{
		*--end = '\0';
	}
	return **name_out != '\0';
}

// What a run used, written by write_params: the type first, then the grid,
// the seed, the vorticity and the knobs
static bool read_params(const char *path, atmosphere_t *atmosphere, int *size, uint32_t *seed, float *vorticity)
{
	FILE *file = fopen(path, "r");
	char line[512];

	if (file == NULL)
	{
		fprintf(stderr, "Cannot read %s\n", path);
		return false;
	}
	while (fgets(line, sizeof(line), file) != NULL)
	{
		char *name;
		char *value;

		if (!split_line(line, &name, &value))
		{
			continue;
		}
		if (strcmp(name, "type") == 0)
		{
			if (!select_type(atmosphere, value))
			{
				fclose(file);
				return false;
			}
		}
		else if (strcmp(name, "grid") == 0)
		{
			*size = atoi(value);
		}
		else if (strcmp(name, "seed") == 0)
		{
			*seed = (uint32_t)strtoul(value, NULL, 10);
		}
		else if (strcmp(name, "vorticity") == 0)
		{
			*vorticity = (float)atof(value);
		}
		else if (!set_knob(atmosphere, name, (float)atof(value)))
		{
			fprintf(stderr, "%s: no setting %s\n", path, name);
		}
	}
	fclose(file);
	return true;
}

static void write_params(const char *path, const char *picture_name, const atmosphere_t *atmosphere, int size, uint32_t seed, float vorticity,
	const char *note)
{
	FILE *file = fopen(path, "w");

	if (file == NULL)
	{
		fprintf(stderr, "Cannot write %s\n", path);
		return;
	}
	fprintf(file, "# %s: grown with these (cloud_bake --params %s), %s\n", picture_name, path, note);
	fprintf(file, "# for the game: cloud_bake --params %s --grid 128 --out data/clouds/%s_1.cloud\n", path, atmosphere->name);
	fprintf(file, "type = %s\n", atmosphere->name);
	fprintf(file, "grid = %d\n", size);
	fprintf(file, "seed = %u\n", seed);
	fprintf(file, "vorticity = %.4f\n", (double)vorticity);
	for (size_t k = 0; k < sizeof(knobs) / sizeof(knobs[0]); k++)
	{
		float value = *(const float *)((const char *)atmosphere + knobs[k].offset);

		fprintf(file, "%s = %.6g\n", knobs[k].option + 2, (double)((value - knobs[k].shift) / knobs[k].scale));
	}
	fclose(file);
}

// The fuzzer's settings: [run] (type, grid, runs) and [ranges] (a knob or
// vorticity = low, high: uniform between them; one number: fixed)
typedef struct
{
	char name[64];
	float low;
	float high;
} range_t;

#define MAXIMUM_RANGES 64

typedef struct
{
	char type[32];
	int size;
	int runs;
	range_t ranges[MAXIMUM_RANGES];
	int range_count;
} fuzz_t;

static bool read_fuzz(const char *path, fuzz_t *fuzz)
{
	FILE *file = fopen(path, "r");
	char line[512];
	bool in_ranges = false;

	memset(fuzz, 0, sizeof(*fuzz));
	fuzz->size = 64;
	fuzz->runs = 100;
	if (file == NULL)
	{
		fprintf(stderr, "Cannot read %s\n", path);
		return false;
	}
	while (fgets(line, sizeof(line), file) != NULL)
	{
		char *name;
		char *value;
		char *start = line;

		while (*start == ' ' || *start == '\t')
		{
			start++;
		}
		if (*start == '[')
		{
			in_ranges = strncmp(start, "[ranges]", 8) == 0;
			continue;
		}
		if (!split_line(start, &name, &value))
		{
			continue;
		}
		if (!in_ranges)
		{
			if (strcmp(name, "type") == 0)
			{
				snprintf(fuzz->type, sizeof(fuzz->type), "%s", value);
			}
			else if (strcmp(name, "grid") == 0)
			{
				fuzz->size = atoi(value);
			}
			else if (strcmp(name, "runs") == 0)
			{
				fuzz->runs = atoi(value);
			}
			continue;
		}
		if (fuzz->range_count < MAXIMUM_RANGES)
		{
			range_t *range = &fuzz->ranges[fuzz->range_count++];
			char *comma = strchr(value, ',');

			snprintf(range->name, sizeof(range->name), "%s", name);
			range->low = (float)atof(value);
			range->high = comma != NULL ? (float)atof(comma + 1) : range->low;
		}
	}
	fclose(file);
	return true;
}

// The first free number in the directory (its N.txt not there yet)
static int next_free_number(const char *directory)
{
	char path[1024];

	for (int n = 1; n < 1000000; n++)
	{
		FILE *file;

		snprintf(path, sizeof(path), "%s/%d.txt", directory, n);
		file = fopen(path, "r");
		if (file == NULL)
		{
			return n;
		}
		fclose(file);
	}
	return 1000000;
}

// The numbers a run is watched by: cloud water, the fastest updraft, the
// warmest and the most humid air (the GPU's fields fetched first)
static void print_progress(grid_t *grid, gpu_simulation_t *gpu, float time, double start)
{
	double water = 0.0;
	float fastest = 0.0f;
	float warmest = 0.0f;
	float wettest = 0.0f;
	size_t cells = (size_t)grid->nx * grid->ny * grid->nz;

	if (gpu != NULL)
	{
		gpu_finish_steps(gpu);
		gpu_download(gpu, grid);
	}
	for (size_t c = 0; c < cells; c++)
	{
		water += grid->water[c];
	}
	for (size_t f = 0; f < (size_t)grid->nx * (grid->ny + 1) * grid->nz; f++)
	{
		fastest = fmaxf(fastest, fabsf(grid->v[f]));
	}
	for (int k = 0; k < grid->nz; k++)
	{
		for (int j = 0; j < grid->ny; j++)
		{
			for (int i = 0; i < grid->nx; i++)
			{
				size_t c = index_c(grid, i, j, k);
				float temperature = grid->theta[c] * grid->exner[j];

				warmest = fmaxf(warmest, grid->theta[c] - grid->ambient_theta[j]);
				wettest = fmaxf(wettest, grid->vapor[c] / saturation_vapor(temperature, grid->level_pressure[j]));
			}
		}
	}
	printf("  %4.0f s: cloud water %.3g, fastest updraft %.1f m/s, warmest +%.2f K, most humid %.0f%% (%.0f s so far)\n", (double)time, water,
		(double)fastest, (double)warmest, (double)wettest * 100.0, omp_get_wtime() - start);
	if (gpu != NULL)
	{
		printf("        GPU: %.2f ms a step (%s %.2f, %s %.2f, %s %.2f, %s %.2f ms, measured on their own)\n", gpu->milliseconds_per_step,
			phase_names[0], gpu->phase_milliseconds[0], phase_names[1], gpu->phase_milliseconds[1], phase_names[2], gpu->phase_milliseconds[2],
			phase_names[3], gpu->phase_milliseconds[3]);
	}
	fflush(stdout);
}

// One cloud grown: VIEW_GO when it ran its course, VIEW_SAVE_NOW when told
// to stop and keep it, VIEW_SKIP, VIEW_QUIT. On the GPU (gpu not NULL) the
// grid's water, heat and vapor are fetched back at the end
static view_action_t simulate(grid_t *grid, const atmosphere_t *atmosphere, float vorticity, gpu_simulation_t *gpu, viewer_t *viewer,
	const char *label, double start)
{
	// About a cell per step for the fastest updrafts
	float dt = grid->h / 8.0f;
	int steps = (int)(atmosphere->seconds / dt);
	view_action_t result = VIEW_GO;

	for (int step = 0; step < steps; step++)
	{
		float time = (float)step * dt;

		if (gpu != NULL)
		{
			// Each phase timed on its own now and then (it waits for each)
			gpu_step(gpu, grid, atmosphere, vorticity, dt, time, step % 200 == 1);
			if (gpu->steps_since_flush >= 4)
			{
				gpu_finish_steps(gpu);
			}
		}
		else
		{
			feed_thermal(grid, atmosphere, dt, time);
			advect_velocity(grid, dt);
			advect_scalar(grid, grid->theta, dt);
			advect_scalar(grid, grid->vapor, dt);
			advect_scalar(grid, grid->water, dt);
			condense(grid);
			buoyancy(grid, dt);
			add_shear(grid, atmosphere, dt);
			confine_vorticity(grid, vorticity, dt);
			sponge(grid, atmosphere, dt);
			project(grid, PRESSURE_ITERATIONS);
		}
		if (viewer->window != NULL)
		{
			char title[512];
			view_action_t action;

			snprintf(title, sizeof(title), "%s: %.0f of %.0f s  |  drag: turn, wheel: zoom, Space: pause, Enter: keep it now, N: next, Esc: stop",
				label, (double)time, (double)atmosphere->seconds);
			action = viewer_update(viewer, grid, title, step == steps - 1);
			if (action != VIEW_GO)
			{
				result = action;
				break;
			}
		}
		if (step % 50 == 0 || step == steps - 1)
		{
			print_progress(grid, gpu, time, start);
		}
	}
	if (gpu != NULL)
	{
		gpu_finish_steps(gpu);
		gpu_download(gpu, grid);
	}
	return result;
}

static void write_cloud(grid_t *grid, const char *path)
{
	uint8_t *shape = malloc((size_t)SHAPE_TEXELS * SHAPE_TEXELS_UP * SHAPE_TEXELS);
	int cloud_texels = 0;
	float top = bake(grid, shape, &cloud_texels);
	FILE *file = fopen(path, "wb");

	if (file == NULL)
	{
		fprintf(stderr, "Cannot write %s\n", path);
		free(shape);
		return;
	}
	{
		// Header: magic, the shape's texels across, up, deep, its top (of its width)
		uint32_t header[4] = { 0x444c4350u, SHAPE_TEXELS, SHAPE_TEXELS_UP, SHAPE_TEXELS };

		fwrite(header, sizeof(header), 1, file);
		fwrite(&top, sizeof(top), 1, file);
		fwrite(shape, 1, (size_t)SHAPE_TEXELS * SHAPE_TEXELS_UP * SHAPE_TEXELS, file);
	}
	fclose(file);
	free(shape);
	printf("Wrote %s: %d texels of cloud, top at %.2f of its width\n", path, cloud_texels, (double)top);
}

// The volume made smaller, its longest side size voxels (each the average
// of the ones it covers); its voxels' size grows to match
static void shrink_volume(vdb_volume_t *volume, int size, float *meters)
{
	int longest = volume->nx > volume->ny ? volume->nx : volume->ny;
	float factor;
	int nx;
	int ny;
	int nz;
	float *values;

	longest = volume->nz > longest ? volume->nz : longest;
	if (size <= 0 || longest <= size)
	{
		return;
	}
	factor = (float)longest / (float)size;
	nx = (int)ceilf((float)volume->nx / factor);
	ny = (int)ceilf((float)volume->ny / factor);
	nz = (int)ceilf((float)volume->nz / factor);
	values = calloc((size_t)nx * ny * nz, sizeof(float));
#pragma omp parallel for schedule(static)
	for (int k = 0; k < nz; k++)
	{
		int z0 = (int)((float)k * factor);
		int z1 = (int)fminf((float)(k + 1) * factor, (float)volume->nz);

		for (int j = 0; j < ny; j++)
		{
			int y0 = (int)((float)j * factor);
			int y1 = (int)fminf((float)(j + 1) * factor, (float)volume->ny);

			for (int i = 0; i < nx; i++)
			{
				int x0 = (int)((float)i * factor);
				int x1 = (int)fminf((float)(i + 1) * factor, (float)volume->nx);
				double sum = 0.0;
				int count = 0;

				for (int z = z0; z < z1; z++)
				{
					for (int y = y0; y < y1; y++)
					{
						for (int x = x0; x < x1; x++)
						{
							sum += volume->values[((size_t)z * volume->ny + (size_t)y) * volume->nx + (size_t)x];
							count++;
						}
					}
				}
				values[((size_t)k * ny + (size_t)j) * nx + (size_t)i] = count > 0 ? (float)(sum / count) : 0.0f;
			}
		}
	}
	free(volume->values);
	volume->values = values;
	volume->nx = nx;
	volume->ny = ny;
	volume->nz = nz;
	volume->largest = 0.0f;
	for (size_t c = 0; c < (size_t)nx * ny * nz; c++)
	{
		volume->largest = fmaxf(volume->largest, values[c]);
	}
	*meters *= factor;
}

// A cloud's density from an OpenVDB file into the grid's water (the rest of
// the grid left out: it is only looked at and baked): voxels of meters each
// (bigger if it is made smaller: size voxels at most along its longest side),
// the densest part as dense as a simulated cloud's water (--vdb-density
// thickens it when it is drawn)
// The volume turned so the file's up (up: "y", "z", "x", or with a minus,
// "-z" for down) is the grid's y; false if up is none of those
static bool turn_volume(vdb_volume_t *volume, const char *up)
{
	bool down = up[0] == '-';
	char axis = down ? up[1] : up[0];
	int size[3] = { volume->nx, volume->ny, volume->nz };
	int turned[3];
	float *values;
	// Which of the file's axes each of the grid's is (swapped, so mirrored:
	// a cloud does not mind)
	int from[3];

	if (axis == 'y' && !down)
	{
		return true;
	}
	if (axis == 'x')
	{
		from[0] = 1;
		from[1] = 0;
		from[2] = 2;
	}
	else if (axis == 'y')
	{
		from[0] = 0;
		from[1] = 1;
		from[2] = 2;
	}
	else if (axis == 'z')
	{
		from[0] = 0;
		from[1] = 2;
		from[2] = 1;
	}
	else
	{
		return false;
	}
	for (int a = 0; a < 3; a++)
	{
		turned[a] = size[from[a]];
	}
	values = malloc((size_t)turned[0] * turned[1] * turned[2] * sizeof(float));
	for (int k = 0; k < turned[2]; k++)
	{
		for (int j = 0; j < turned[1]; j++)
		{
			for (int i = 0; i < turned[0]; i++)
			{
				int at[3];
				int grid_at[3] = { i, down ? turned[1] - 1 - j : j, k };

				for (int a = 0; a < 3; a++)
				{
					at[from[a]] = grid_at[a];
				}
				values[((size_t)k * turned[1] + (size_t)j) * turned[0] + (size_t)i]
					= volume->values[((size_t)at[2] * size[1] + (size_t)at[1]) * size[0] + (size_t)at[0]];
			}
		}
	}
	free(volume->values);
	volume->values = values;
	volume->nx = turned[0];
	volume->ny = turned[1];
	volume->nz = turned[2];
	return true;
}

static bool load_vdb(grid_t *grid, const char *path, const char *grid_name, float meters, int size, const char *up)
{
	vdb_volume_t volume;
	char error[512];
	size_t cells;
	float scale;

	if (!vdb_read(path, grid_name, &volume, error, sizeof(error)))
	{
		fprintf(stderr, "%s: %s\n", path, error);
		return false;
	}
	if (!turn_volume(&volume, up))
	{
		fprintf(stderr, "--vdb-up %s: x, y, z, -x, -y or -z\n", up);
		vdb_volume_free(&volume);
		return false;
	}
	// The scale is the full volume's, so a smaller one looks as dense
	{
		float largest = volume.largest;

		shrink_volume(&volume, size, &meters);
		volume.largest = largest;
	}
	memset(grid, 0, sizeof(*grid));
	grid->nx = volume.nx;
	grid->ny = volume.ny;
	grid->nz = volume.nz;
	grid->h = meters;
	cells = (size_t)grid->nx * grid->ny * grid->nz;
	grid->water = allocate(cells);
	grid->scratch = allocate(cells);
	// The densest voxel as 2 g of cloud water a kilogram of air
	scale = 0.002f / fmaxf(volume.largest, 1e-6f);
	for (size_t c = 0; c < cells; c++)
	{
		grid->water[c] = volume.values[c] * scale;
	}
	printf("%s: %d x %d x %d voxels (%.0f x %.0f x %.0f m at %.1f m), densest %.3g\n", path, volume.nx, volume.ny, volume.nz,
		(double)(volume.nx * meters), (double)(volume.ny * meters), (double)(volume.nz * meters), (double)meters, (double)volume.largest);
	vdb_volume_free(&volume);
	return true;
}

// ---------------------------------------------------------------------------
// Density clouds for the game (read from VDB files): the format
// engine/cloud_weather.c loads as a layer's shapes

// "PCL2": the cloud's density, longest side across DENSITY_CLOUD_TEXELS
#define DENSITY_CLOUD_MAGIC 0x324c4350u
#define DENSITY_CLOUD_TEXELS 192
// Pieces of a cloud field smaller than this (voxels, at the file's
// resolution) are dropped as crumbs
#define SPLIT_MINIMUM_VOXELS 3000

// The water (any scale) of a box of the grid, sized so its longest side
// across (x or z) is DENSITY_CLOUD_TEXELS at most (up as tall as it is, of that),
// each side a multiple of 4 (BC4's blocks), its density 0..255 with the
// 95th percentile of the cloud's voxels at 255; written as: magic, x, y, z
// texels, meters a texel, the zlib packed size, then zlib(x fastest, z
// last). False if it cannot be written
static bool write_density_cloud(const grid_t *grid, const int low[3], const int high[3], const char *path)
{
	int size[3] = { high[0] - low[0], high[1] - low[1], high[2] - low[2] };
	int across = size[0] > size[2] ? size[0] : size[2];
	// (smaller clouds keep their own resolution: no point in blowing them up)
	float factor = fmaxf((float)across / (float)DENSITY_CLOUD_TEXELS, 1.0f);
	int texels[3];
	size_t count;
	float *values;
	uint8_t *bytes;
	float percentile = 0.0f;
	mz_ulong packed_size;
	uint8_t *packed;
	FILE *file;
	uint32_t header[6];
	float meters;

	for (int a = 0; a < 3; a++)
	{
		texels[a] = ((int)ceilf((float)size[a] / factor) + 3) / 4 * 4;
		texels[a] = texels[a] < 4 ? 4 : texels[a];
	}
	count = (size_t)texels[0] * texels[1] * texels[2];
	values = calloc(count, sizeof(float));
	bytes = calloc(count, 1);
	// Each texel the average of the voxels it covers
#pragma omp parallel for schedule(static)
	for (int k = 0; k < texels[2]; k++)
	{
		int z0 = low[2] + (int)((float)k * factor);
		int z1 = low[2] + (int)fminf((float)(k + 1) * factor, (float)size[2]);

		for (int j = 0; j < texels[1]; j++)
		{
			int y0 = low[1] + (int)((float)j * factor);
			int y1 = low[1] + (int)fminf((float)(j + 1) * factor, (float)size[1]);

			for (int i = 0; i < texels[0]; i++)
			{
				int x0 = low[0] + (int)((float)i * factor);
				int x1 = low[0] + (int)fminf((float)(i + 1) * factor, (float)size[0]);
				double sum = 0.0;
				int covered = 0;

				for (int z = z0; z < z1 || (z == z0 && z0 < low[2] + size[2]); z++)
				{
					for (int y = y0; y < y1 || (y == y0 && y0 < low[1] + size[1]); y++)
					{
						for (int x = x0; x < x1 || (x == x0 && x0 < low[0] + size[0]); x++)
						{
							sum += grid->water[index_c(grid, x, y, z)];
							covered++;
						}
					}
				}
				values[((size_t)k * texels[1] + (size_t)j) * texels[0] + (size_t)i] = covered > 0 ? (float)(sum / covered) : 0.0f;
			}
		}
	}
	{
		// The 95th percentile of what is cloud
		size_t histogram[1024] = { 0 };
		size_t cloudy = 0;
		float largest = 0.0f;

		for (size_t c = 0; c < count; c++)
		{
			largest = fmaxf(largest, values[c]);
		}
		for (size_t c = 0; c < count && largest > 0.0f; c++)
		{
			if (values[c] > largest * 0.01f)
			{
				histogram[(int)fminf(values[c] / largest * 1023.0f, 1023.0f)]++;
				cloudy++;
			}
		}
		for (size_t b = 0, seen = 0; b < 1024; b++)
		{
			seen += histogram[b];
			if (seen >= cloudy * 95 / 100)
			{
				percentile = ((float)b + 1.0f) / 1024.0f * largest;
				break;
			}
		}
	}
	{
		// Faded out over the last few texels at the sides and the top:
		// a cloud cut off by its simulation's box (or a field's piece by the
		// field's edge) would otherwise end in a flat wall
		const float fade = 6.0f;

		for (int k = 0; k < texels[2]; k++)
		{
			for (int j = 0; j < texels[1]; j++)
			{
				for (int i = 0; i < texels[0]; i++)
				{
					float x_edge = fminf((float)i + 0.5f, (float)size[0] / factor - ((float)i + 0.5f));
					float z_edge = fminf((float)k + 0.5f, (float)size[2] / factor - ((float)k + 0.5f));
					float top_edge = (float)size[1] / factor - ((float)j + 0.5f);
					float edge = fminf(fminf(x_edge, z_edge), top_edge) / fade;
					size_t c = ((size_t)k * texels[1] + (size_t)j) * texels[0] + (size_t)i;
					float kept = edge >= 1.0f ? 1.0f : edge <= 0.0f ? 0.0f : edge * edge * (3.0f - 2.0f * edge);

					bytes[c] = percentile > 0.0f ? (uint8_t)fminf(values[c] * kept / percentile * 255.0f + 0.5f, 255.0f) : 0;
				}
			}
		}
	}
	free(values);
	packed_size = mz_compressBound((mz_ulong)count);
	packed = malloc(packed_size);
	if (packed == NULL || mz_compress2(packed, &packed_size, bytes, (mz_ulong)count, 9) != MZ_OK)
	{
		free(packed);
		free(bytes);
		return false;
	}
	free(bytes);
	meters = grid->h * factor;
	header[0] = DENSITY_CLOUD_MAGIC;
	header[1] = (uint32_t)texels[0];
	header[2] = (uint32_t)texels[1];
	header[3] = (uint32_t)texels[2];
	memcpy(&header[4], &meters, sizeof(float));
	header[5] = (uint32_t)packed_size;
	file = fopen(path, "wb");
	if (file == NULL)
	{
		free(packed);
		return false;
	}
	fwrite(header, sizeof(header), 1, file);
	fwrite(packed, 1, packed_size, file);
	fclose(file);
	free(packed);
	printf("Wrote %s: %d x %d x %d texels (%.0f x %.0f x %.0f m), %.0f KB\n", path, texels[0], texels[1], texels[2],
		(double)(texels[0] * meters), (double)(texels[1] * meters), (double)(texels[2] * meters), (double)(sizeof(header) + packed_size) / 1024.0);
	return true;
}

// The first free <directory>/<name>_<n>.cloud
static void next_cloud_path(const char *directory, const char *name, char *path, size_t path_size)
{
	for (int n = 1; n < 100000; n++)
	{
		FILE *file;

		snprintf(path, path_size, "%s/%s_%d.cloud", directory, name, n);
		file = fopen(path, "rb");
		if (file == NULL)
		{
			return;
		}
		fclose(file);
	}
}

// A field of clouds cut into its clouds: each piece of cloud not touching
// another (faces, edges and corners count) on its own, the crumbs dropped;
// each written as a density cloud (the others' voxels cleared from its
// box). Returns how many were written
static int write_split_clouds(grid_t *grid, const char *directory, const char *name)
{
	size_t cells = (size_t)grid->nx * grid->ny * grid->nz;
	float threshold = 0.0f;
	int32_t *labels = calloc(cells, sizeof(int32_t));
	size_t *stack = malloc(cells * sizeof(size_t));
	float *kept = malloc(cells * sizeof(float));
	int written = 0;
	int32_t label = 0;

	for (size_t c = 0; c < cells; c++)
	{
		threshold = fmaxf(threshold, grid->water[c]);
	}
	// What counts as cloud for the cutting: a little of the densest
	threshold *= 0.02f;
	memcpy(kept, grid->water, cells * sizeof(float));
	for (size_t seed = 0; seed < cells; seed++)
	{
		size_t top = 0;
		size_t voxels = 0;
		int low[3] = { grid->nx, grid->ny, grid->nz };
		int high[3] = { 0, 0, 0 };

		if (labels[seed] != 0 || kept[seed] <= threshold)
		{
			continue;
		}
		label++;
		labels[seed] = label;
		stack[top++] = seed;
		while (top > 0)
		{
			size_t c = stack[--top];
			int i = (int)(c % (size_t)grid->nx);
			int j = (int)((c / (size_t)grid->nx) % (size_t)grid->ny);
			int k = (int)(c / ((size_t)grid->nx * grid->ny));
			int at[3] = { i, j, k };

			voxels++;
			for (int a = 0; a < 3; a++)
			{
				low[a] = at[a] < low[a] ? at[a] : low[a];
				high[a] = at[a] + 1 > high[a] ? at[a] + 1 : high[a];
			}
			for (int dk = -1; dk <= 1; dk++)
			{
				for (int dj = -1; dj <= 1; dj++)
				{
					for (int di = -1; di <= 1; di++)
					{
						int ni = i + di;
						int nj = j + dj;
						int nk = k + dk;
						size_t n;

						if (ni < 0 || nj < 0 || nk < 0 || ni >= grid->nx || nj >= grid->ny || nk >= grid->nz)
						{
							continue;
						}
						n = index_c(grid, ni, nj, nk);
						if (labels[n] == 0 && kept[n] > threshold)
						{
							labels[n] = label;
							stack[top++] = n;
						}
					}
				}
			}
		}
		if (voxels < SPLIT_MINIMUM_VOXELS)
		{
			continue;
		}
		{
			// This piece alone in its box (the thin fringe under the
			// threshold kept with it), its base at the box's bottom
			char path[1024];

			for (int k = low[2]; k < high[2]; k++)
			{
				for (int j = low[1]; j < high[1]; j++)
				{
					for (int i = low[0]; i < high[0]; i++)
					{
						size_t c = index_c(grid, i, j, k);

						grid->water[c] = labels[c] == label || (labels[c] == 0 && kept[c] > 0.0f) ? kept[c] : 0.0f;
					}
				}
			}
			next_cloud_path(directory, name, path, sizeof(path));
			if (write_density_cloud(grid, low, high, path))
			{
				written++;
			}
		}
	}
	memcpy(grid->water, kept, cells * sizeof(float));
	free(kept);
	free(stack);
	free(labels);
	printf("%d clouds cut out of the field (%d pieces in all)\n", written, (int)label);
	return written;
}

static void usage(void)
{
	fprintf(stderr, "usage: cloud_bake [--type humilis|mediocris|congestus|storm] [--params file.txt] [--grid N] [--seed N] [--count N] "
		"[--out-dir directory] [--name type] [--vorticity strength] [--out file.cloud] [--preview file.pgm] [--save-water file] "
		"[--water file] [--picture file.png] [--show] [--no-view] [--cpu] [--fuzz file.ini] [--vdb file.vdb [--vdb-grid name] [--vdb-meters m] [--vdb-density x] [--vdb-size n] [--vdb-up y|z|-z|...] [--split] [--sun x] [--sky x] [--exposure x]] [knobs]\nknobs (the type's values unless given):\n");
	for (size_t k = 0; k < sizeof(knobs) / sizeof(knobs[0]); k++)
	{
		fprintf(stderr, "  %-22s %s\n", knobs[k].option, knobs[k].meaning);
	}
}

int main(int argc, char **argv)
{
	atmosphere_t chosen = atmospheres[1];
	const char *name = NULL;
	const char *out_path = NULL;
	const char *preview_path = NULL;
	const char *raw_in_path = NULL;
	const char *vdb_path = NULL;
	const char *picture_path = NULL;
	bool split = false;
	const char *vdb_grid = NULL;
	float vdb_meters = 5.0f;
	float vdb_density = 1.0f;
	int vdb_size = 0;
	const char *vdb_up = "z";
	float view_light[4] = { 4.0f, 0.35f, 1.0f, 4.0f };   // sun, sky, exposure, thickness
	const char *raw_out_path = NULL;
	const char *out_directory = NULL;
	const char *fuzz_path = NULL;
	bool show = false;
	bool watch = true;
	bool use_cpu = false;
	bool quit = false;
	int count = 1;
	int size = 128;
	uint32_t first_seed = 1u;
	float vorticity = 0.0f;
	fuzz_t fuzz;
	viewer_t viewer;
	grid_t grid;
	gpu_simulation_t gpu;
	SDL_GPUDevice *device = NULL;
	uint32_t fuzz_random = (uint32_t)time(NULL) * 2654435761u;
	double start = omp_get_wtime();

	memset(&grid, 0, sizeof(grid));
	memset(&gpu, 0, sizeof(gpu));
	memset(&viewer, 0, sizeof(viewer));
	// The type first, then the fuzzer's ini, then a params file: each
	// changes what came before, and the rest of the command line all of them
	for (int a = 1; a < argc - 1; a++)
	{
		if (strcmp(argv[a], "--type") == 0 && !select_type(&chosen, argv[a + 1]))
		{
			return 1;
		}
	}
	for (int a = 1; a < argc - 1; a++)
	{
		if (strcmp(argv[a], "--fuzz") == 0)
		{
			fuzz_path = argv[a + 1];
			if (!read_fuzz(fuzz_path, &fuzz) || (fuzz.type[0] != '\0' && !select_type(&chosen, fuzz.type)))
			{
				return 1;
			}
			size = fuzz.size;
			count = fuzz.runs;
		}
	}
	for (int a = 1; a < argc - 1; a++)
	{
		if (strcmp(argv[a], "--params") == 0 && !read_params(argv[a + 1], &chosen, &size, &first_seed, &vorticity))
		{
			return 1;
		}
	}
	for (int a = 1; a < argc; a++)
	{
		bool knob = false;

		for (size_t k = 0; k < sizeof(knobs) / sizeof(knobs[0]) && a + 1 < argc; k++)
		{
			if (strcmp(argv[a], knobs[k].option) == 0)
			{
				*(float *)((char *)&chosen + knobs[k].offset) = (float)atof(argv[a + 1]) * knobs[k].scale + knobs[k].shift;
				knob = true;
			}
		}
		if (knob || ((strcmp(argv[a], "--type") == 0 || strcmp(argv[a], "--params") == 0 || strcmp(argv[a], "--fuzz") == 0) && a + 1 < argc))
		{
			a++;
		}
		else if (strcmp(argv[a], "--name") == 0 && a + 1 < argc)
		{
			name = argv[++a];
		}
		else if (strcmp(argv[a], "--show") == 0)
		{
			show = true;
		}
		else if (strcmp(argv[a], "--no-view") == 0)
		{
			watch = false;
		}
		else if (strcmp(argv[a], "--cpu") == 0)
		{
			use_cpu = true;
		}
		else if (strcmp(argv[a], "--grid") == 0 && a + 1 < argc)
		{
			size = atoi(argv[++a]);
		}
		else if (strcmp(argv[a], "--seed") == 0 && a + 1 < argc)
		{
			first_seed = (uint32_t)strtoul(argv[++a], NULL, 10);
		}
		else if (strcmp(argv[a], "--count") == 0 && a + 1 < argc)
		{
			count = atoi(argv[++a]);
			count = count < 1 ? 1 : count;
		}
		else if (strcmp(argv[a], "--out-dir") == 0 && a + 1 < argc)
		{
			out_directory = argv[++a];
		}
		else if (strcmp(argv[a], "--vorticity") == 0 && a + 1 < argc)
		{
			vorticity = (float)atof(argv[++a]);
		}
		else if (strcmp(argv[a], "--out") == 0 && a + 1 < argc)
		{
			out_path = argv[++a];
		}
		else if (strcmp(argv[a], "--save-water") == 0 && a + 1 < argc)
		{
			raw_out_path = argv[++a];
		}
		else if (strcmp(argv[a], "--water") == 0 && a + 1 < argc)
		{
			raw_in_path = argv[++a];
		}
		else if (strcmp(argv[a], "--split") == 0)
		{
			split = true;
		}
		else if (strcmp(argv[a], "--picture") == 0 && a + 1 < argc)
		{
			picture_path = argv[++a];
		}
		else if (strcmp(argv[a], "--vdb") == 0 && a + 1 < argc)
		{
			vdb_path = argv[++a];
		}
		else if (strcmp(argv[a], "--vdb-grid") == 0 && a + 1 < argc)
		{
			vdb_grid = argv[++a];
		}
		else if (strcmp(argv[a], "--vdb-meters") == 0 && a + 1 < argc)
		{
			vdb_meters = (float)atof(argv[++a]);
		}
		else if (strcmp(argv[a], "--sun") == 0 && a + 1 < argc)
		{
			view_light[0] = (float)atof(argv[++a]);
		}
		else if (strcmp(argv[a], "--sky") == 0 && a + 1 < argc)
		{
			view_light[1] = (float)atof(argv[++a]);
		}
		else if (strcmp(argv[a], "--exposure") == 0 && a + 1 < argc)
		{
			view_light[2] = (float)atof(argv[++a]);
		}
		else if (strcmp(argv[a], "--vdb-up") == 0 && a + 1 < argc)
		{
			vdb_up = argv[++a];
		}
		else if (strcmp(argv[a], "--vdb-size") == 0 && a + 1 < argc)
		{
			vdb_size = atoi(argv[++a]);
		}
		else if (strcmp(argv[a], "--vdb-density") == 0 && a + 1 < argc)
		{
			vdb_density = (float)atof(argv[++a]);
		}
		else if (strcmp(argv[a], "--preview") == 0 && a + 1 < argc)
		{
			preview_path = argv[++a];
		}
		else
		{
			usage();
			return 1;
		}
	}
	if (name != NULL)
	{
		chosen.name = name;
	}
	if (vdb_path != NULL)
	{
		// One cloud, read: nothing to simulate
		count = 1;
		fuzz_path = NULL;
	}
	print_atmosphere(&chosen);
	if (show)
	{
		return 0;
	}
	size = size < 32 ? 32 : size > 256 ? 256 : size;
	size &= ~1;
	if (fuzz_path != NULL && out_directory == NULL)
	{
		out_directory = "fuzz";
	}
	if (fuzz_path == NULL && count > 1 && out_directory == NULL)
	{
		out_directory = "data/clouds";
	}
	if (out_directory != NULL)
	{
		SDL_CreateDirectory(out_directory);
	}

	// The GPU, unless told otherwise or there is none
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
	}
	if (!use_cpu)
	{
		device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
		if (device == NULL)
		{
			fprintf(stderr, "No GPU (%s): simulating on the CPU\n", SDL_GetError());
		}
	}
	if (device == NULL && fuzz_path != NULL)
	{
		fprintf(stderr, "Fuzzing draws its pictures on the GPU: there is none\n");
		return 1;
	}
	// The window is the GPU's from the start (the simulation's buffers come
	// with each run)
	gpu.device = device;
	if (watch)
	{
		viewer_open(&viewer, device != NULL ? &gpu : NULL);
		if (viewer.window != NULL)
		{
			printf("Watching: drag to turn, wheel to zoom, Space pauses, Enter keeps the cloud as it is now, N drops it for the next one, "
				"Esc stops\n");
		}
	}

	for (int cloud = 0; cloud < count && !quit; cloud++)
	{
		atmosphere_t atmosphere = chosen;
		float run_vorticity = vorticity;
		view_action_t action = VIEW_GO;
		gpu_simulation_t *simulation = NULL;
		char label[256];
		int number = 0;

		if (fuzz_path != NULL)
		{
			// A seed of its own (unless the ranges fix it) and the ranges'
			// values for this run
			fuzz_random = fuzz_random * 747796405u + 2891336453u;
			burst_seed = (fuzz_random >> 8) % 1000000u + 1u;
			for (int r = 0; r < fuzz.range_count; r++)
			{
				const range_t *range = &fuzz.ranges[r];
				float t = (float)((fuzz_random = fuzz_random * 747796405u + 2891336453u) >> 8) / 16777216.0f;
				float value = range->low + (range->high - range->low) * t;

				if (strcmp(range->name, "vorticity") == 0)
				{
					run_vorticity = value;
				}
				else if (strcmp(range->name, "seed") == 0)
				{
					burst_seed = (uint32_t)(value + 0.5f);
				}
				else if (!set_knob(&atmosphere, range->name, value))
				{
					fprintf(stderr, "%s: no setting %s\n", fuzz_path, range->name);
				}
			}
			number = next_free_number(out_directory);
			snprintf(label, sizeof(label), "%s run %d of %d (%s/%d), seed %u", atmosphere.name, cloud + 1, count, out_directory, number,
				burst_seed);
		}
		else
		{
			burst_seed = first_seed + (uint32_t)cloud;
			snprintf(label, sizeof(label), "%s, seed %u", atmosphere.name, burst_seed);
		}
		random_state = burst_seed * 2654435761u + 1u;
		grid_free(&grid);
		if (vdb_path != NULL)
		{
			if (!load_vdb(&grid, vdb_path, vdb_grid, vdb_meters, vdb_size, vdb_up))
			{
				return 1;
			}
		}
		else
		{
			grid_allocate(&grid, size, &atmosphere);
			printf("%s: %d x %d x %d cells of %.1f m\n", label, grid.nx, grid.ny, grid.nz, (double)grid.h);
			make_atmosphere(&grid, &atmosphere);
		}
		if (device != NULL)
		{
			gpu_simulation_destroy(&gpu);
			if (!gpu_simulation_create(&gpu, device, &grid, vdb_path != NULL))
			{
				return 1;
			}
			gpu.sun_light = view_light[0];
			gpu.sky_light = view_light[1];
			gpu.exposure = view_light[2];
			gpu.thickness = vdb_density * view_light[3];
			simulation = &gpu;
		}
		if (vdb_path != NULL)
		{
			if (simulation != NULL)
			{
				gpu_upload(simulation, &grid);
			}
			if (viewer.window != NULL)
			{
				char title[512];

				snprintf(title, sizeof(title), "%s  |  drag: turn, wheel: zoom, arrows: the sun, Enter: bake it, Esc: stop", vdb_path);
				action = viewer_update(&viewer, &grid, title, true);
				while (action == VIEW_GO)
				{
					SDL_Delay(15);
					action = viewer_update(&viewer, &grid, title, false);
				}
			}
		}
		else if (raw_in_path != NULL)
		{
			// A simulation saved before (--save-water): bake it again only
			FILE *file = fopen(raw_in_path, "rb");
			size_t cells = (size_t)grid.nx * grid.ny * grid.nz;

			if (file == NULL || fread(grid.water, sizeof(float), cells, file) != cells)
			{
				fprintf(stderr, "Cannot read %s (made with the same --type and --grid?)\n", raw_in_path);
				return 1;
			}
			fclose(file);
			if (simulation != NULL)
			{
				gpu_upload(simulation, &grid);
			}
			if (viewer.window != NULL)
			{
				const char *title = "saved simulation  |  drag: turn, wheel: zoom, Enter: bake it, Esc: stop";

				action = viewer_update(&viewer, &grid, title, true);
				while (action == VIEW_GO)
				{
					SDL_Delay(15);
					action = viewer_update(&viewer, &grid, title, false);
				}
			}
		}
		else
		{
			if (simulation != NULL)
			{
				gpu_upload(simulation, &grid);
			}
			action = simulate(&grid, &atmosphere, run_vorticity, simulation, &viewer, label, start);
		}
		if (action == VIEW_QUIT)
		{
			quit = true;
			printf("Stopped: nothing more kept\n");
			break;
		}
		if (fuzz_path != NULL)
		{
			// Every run kept, good or not: its picture and what grew it
			char picture_path[1024];
			char params_path[1024];
			char picture_name[64];

			snprintf(picture_name, sizeof(picture_name), "%d.png", number);
			snprintf(picture_path, sizeof(picture_path), "%s/%d.png", out_directory, number);
			snprintf(params_path, sizeof(params_path), "%s/%d.txt", out_directory, number);
			if (!gpu_write_views(&gpu, &grid, picture_path))
			{
				fprintf(stderr, "Cannot write %s\n", picture_path);
			}
			write_params(params_path, picture_name, &atmosphere, size, burst_seed, run_vorticity,
				action == VIEW_GO ? "ran its course" : "stopped early");
			printf("Wrote %s and %s\n", picture_path, params_path);
			continue;
		}
		if (action == VIEW_SKIP)
		{
			printf("Dropped seed %u\n", burst_seed);
			continue;
		}
		if (raw_out_path != NULL)
		{
			FILE *file = fopen(raw_out_path, "wb");

			if (file != NULL)
			{
				fwrite(grid.water, sizeof(float), (size_t)grid.nx * grid.ny * grid.nz, file);
				fclose(file);
			}
		}
		if (preview_path != NULL)
		{
			write_preview(&grid, preview_path);
		}
		if (picture_path != NULL)
		{
			// From below, the side and above, as the fuzzer's
			if (simulation == NULL || !gpu_write_views(simulation, &grid, picture_path))
			{
				fprintf(stderr, "Cannot write %s (it is drawn on the GPU)\n", picture_path);
			}
			else
			{
				printf("Wrote %s\n", picture_path);
			}
		}
		if (vdb_path != NULL && (out_directory != NULL || out_path != NULL))
		{
			// For the game: density, the whole box or each cloud of a field
			int low[3] = { 0, 0, 0 };
			int high[3] = { grid.nx, grid.ny, grid.nz };
			const char *cloud_name = name != NULL ? name : "cumulus";

			if (split)
			{
				write_split_clouds(&grid, out_directory != NULL ? out_directory : "data/clouds", cloud_name);
			}
			else if (out_path != NULL)
			{
				if (!write_density_cloud(&grid, low, high, out_path))
				{
					fprintf(stderr, "Cannot write %s\n", out_path);
				}
			}
			else
			{
				char path[1024];

				next_cloud_path(out_directory, cloud_name, path, sizeof(path));
				if (!write_density_cloud(&grid, low, high, path))
				{
					fprintf(stderr, "Cannot write %s\n", path);
				}
			}
		}
		else if (out_directory != NULL)
		{
			char named_out[1024];

			snprintf(named_out, sizeof(named_out), "%s/%s_%u.cloud", out_directory, atmosphere.name, burst_seed);
			write_cloud(&grid, named_out);
		}
		else if (out_path != NULL)
		{
			write_cloud(&grid, out_path);
		}
	}
	printf("Done in %.1f s\n", omp_get_wtime() - start);
	if (viewer.window != NULL && !quit)
	{
		// The last cloud stays up until it has been seen: Esc (or closing
		// the window) to go
		const char *title = "Done  |  drag: turn, wheel: zoom, Esc: close";
		view_action_t action = viewer_update(&viewer, &grid, title, true);

		printf("Done: Esc in the window closes it\n");
		fflush(stdout);
		while (action != VIEW_QUIT)
		{
			SDL_Delay(15);
			action = viewer_update(&viewer, &grid, title, false);
		}
	}
	viewer_close(&viewer);
	gpu_simulation_destroy(&gpu);
	if (device != NULL)
	{
		SDL_DestroyGPUDevice(device);
	}
	grid_free(&grid);
	SDL_Quit();
	return 0;
}
