// Decals (see engine/decals.h): bullet holes, scorches, mud, damp stains,
// laid over what a pixel shows before it is lit. They come in the lights'
// cluster buffer (lights.glsl, include it first): a cluster's words 2 and 3
// are its decals' first index and count; the decals themselves and their
// atlas sit at fixed places after the indices (lights.c's
// DECAL_RECORDS_OFFSET and DECAL_ATLAS_OFFSET: keep these the same).

#define DECAL_CLUSTERS (16u * 9u * 24u)
#define DECAL_INDICES_MAX 65536u
#define DECAL_RECORD_WORDS 20u
#define DECALS_MAX 512u
#define DECAL_ATLAS_SIZE 512
#define DECAL_RECORDS (4u * DECAL_CLUSTERS + DECAL_INDICES_MAX)
#define DECAL_ATLAS (DECAL_RECORDS + DECALS_MAX * DECAL_RECORD_WORDS)

vec4 decal_vec4(uint word)
{
	return vec4(uintBitsToFloat(cluster_data[word]), uintBitsToFloat(cluster_data[word + 1u]), uintBitsToFloat(cluster_data[word + 2u]),
		uintBitsToFloat(cluster_data[word + 3u]));
}

// The atlas at a texel (layer 0 the color and coverage, 1 the relief and
// roughness)
vec4 decal_texel(ivec2 texel, uint layer)
{
	uint at = DECAL_ATLAS + layer * uint(DECAL_ATLAS_SIZE * DECAL_ATLAS_SIZE) + uint(texel.y * DECAL_ATLAS_SIZE + texel.x);

	return unpackUnorm4x8(cluster_data[at]);
}

// Filtered by hand (there is no sampler for it): the four texels round uv,
// kept inside the cell (rectangle: its corner and size, 0..1)
vec4 decal_sample(vec2 uv, vec4 rectangle, uint layer)
{
	vec2 lowest = rectangle.xy * float(DECAL_ATLAS_SIZE) + 0.5;
	vec2 highest = (rectangle.xy + rectangle.zw) * float(DECAL_ATLAS_SIZE) - 1.5;
	vec2 position = clamp(uv * float(DECAL_ATLAS_SIZE) - 0.5, lowest, highest);
	ivec2 corner = ivec2(floor(position));
	vec2 blend = position - vec2(corner);
	vec4 a = decal_texel(corner, layer);
	vec4 b = decal_texel(corner + ivec2(1, 0), layer);
	vec4 c = decal_texel(corner + ivec2(0, 1), layer);
	vec4 d = decal_texel(corner + ivec2(1, 1), layer);

	return mix(mix(a, b, blend.x), mix(c, d, blend.x), blend.y);
}

// The decals over a point: albedo (linear), normal (the shading normal,
// world space, normalized) and roughness changed in place. surface_normal:
// the face's own (a decal does not run round onto faces turned away from it)
void apply_decals(vec3 point, vec3 surface_normal, inout vec3 albedo, inout vec3 normal, inout float roughness)
{
	uint cluster;

	if (!cluster_of(point, cluster))
	{
		return;
	}
	uint first = cluster_data[4u * cluster + 2u];
	uint count = cluster_data[4u * cluster + 3u];

	for (uint i = 0u; i < count; i++)
	{
		uint record = DECAL_RECORDS + cluster_data[first + i] * DECAL_RECORD_WORDS;
		vec4 plane_along = decal_vec4(record);
		vec4 plane_across = decal_vec4(record + 4u);
		vec4 plane_out = decal_vec4(record + 8u);
		vec3 local = vec3(dot(plane_along.xyz, point) + plane_along.w, dot(plane_across.xyz, point) + plane_across.w,
			dot(plane_out.xyz, point) + plane_out.w);

		if (any(greaterThan(abs(local), vec3(1.0))))
		{
			continue;
		}
		vec3 decal_normal = normalize(plane_out.xyz);
		float facing = smoothstep(0.25, 0.6, dot(surface_normal, decal_normal));

		if (facing <= 0.0)
		{
			continue;
		}
		vec4 rectangle = decal_vec4(record + 12u);
		vec4 look = decal_vec4(record + 16u);
		// (the picture's v down the atlas, its across up the surface)
		vec2 uv = rectangle.xy + vec2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5) * rectangle.zw;
		vec4 color = decal_sample(uv, rectangle, 0u);
		// (thinning out toward the box's front and back)
		float amount = color.a * look.x * facing * (1.0 - local.z * local.z);

		if (amount <= 0.002)
		{
			continue;
		}
		vec4 relief = decal_sample(uv, rectangle, 1u);
		vec3 along = normalize(plane_along.xyz);
		vec3 across = normalize(plane_across.xyz);
		vec2 tilt = (relief.xy * 2.0 - 1.0) * look.y;

		albedo = mix(albedo, color.rgb * color.rgb, amount);
		roughness = mix(roughness, relief.b, amount * look.z);
		normal = normalize(normal + (along * tilt.x - across * tilt.y) * amount);
	}
}
