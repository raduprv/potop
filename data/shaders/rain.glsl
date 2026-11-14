// What the rain's drops and splashes share (rain.vert, rain_splash.vert;
// see src/rain.h): their block, the ground and the water under them, and
// a hash that turns a drop's number into where it is.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D height_texture;   // R32F: the ground (meters)
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D water_texture;    // RG32F: the water on it (meters), last step first

layout(std140, set = UNIFORM_SET, binding = 1) uniform rain_block
{
	vec4 area;                      // the box's center (the eye) x, y, z; its half width
	vec4 fall;                      // the wind x, z (m/s), the fall speed (m/s), the clock (seconds)
	vec4 look;                      // streak length (m), intensity (0..1), the box's height, meters per pixel at 1 m
	vec4 map;                       // cells per side, cell size (m), the splashes' half width, a splash's life (s)
};

// A well mixed hash (PCG)
uint rain_hash(uint value)
{
	uint state = value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;

	return (word >> 22u) ^ word;
}

// Three numbers in 0..1 from a drop's number and a seed
vec3 rain_random3(uint number, uint seed)
{
	uint a = rain_hash(number * 3u + seed * 0x9e3779b9u);
	uint b = rain_hash(a ^ 0x68bc21ebu);
	uint c = rain_hash(b ^ 0x02e5be93u);

	return vec3(uvec3(a, b, c) >> 8u) * (1.0 / 16777216.0);
}

// The top of what a drop lands on: the ground, or the water on it
float rain_surface(vec2 world_xz)
{
	int size = int(map.x);
	ivec2 cell = clamp(ivec2(floor(world_xz / map.y + 0.5)), ivec2(0), ivec2(size - 1));
	float depth = texelFetch(water_texture, cell, 0).x;

	return texelFetch(height_texture, cell, 0).r + (depth > 0.002 ? depth : 0.0);
}

// Whether a point is over the map at all
bool rain_on_map(vec2 world_xz)
{
	float extent = (map.x - 1.0) * map.y;

	return world_xz.x >= 0.0 && world_xz.y >= 0.0 && world_xz.x <= extent && world_xz.y <= extent;
}
