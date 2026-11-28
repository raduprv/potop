#version 450
// A solid of one color: the sun (with its shadow map, 3 x 3 samples), the
// sky above and the ground below as ambient. The ground is
// drawn with a grid of 1 and 5 meters.

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_world_normal;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec4 in_light_position;

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2DShadow shadow_map;

layout(set = 3, binding = 0) uniform light_block
{
	vec4 sun_direction;
	vec4 sun_color;
	vec4 sky_color;
	vec4 ground_color;
	vec4 fog_color;                    // (not used now)
	vec4 eye;
	vec4 shadow_texel;
};

float sun_visible()
{
	vec3 position = in_light_position.xyz / in_light_position.w;
	vec2 coordinates = vec2(0.5 + 0.5 * position.x, 0.5 - 0.5 * position.y);
	float lit = 0.0;

	if (any(lessThan(coordinates, vec2(0.0))) || any(greaterThan(coordinates, vec2(1.0))) || position.z > 1.0)
	{
		return 1.0;
	}
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			lit += texture(shadow_map, vec3(coordinates + vec2(x, y) * shadow_texel.x * 1.2, position.z));
		}
	}
	return lit / 9.0;
}

float grid_line(float coordinate, float spacing, float width)
{
	float distance_to_line = abs(fract(coordinate / spacing + 0.5) - 0.5) * spacing;
	float blur = fwidth(coordinate) * 1.5;
	return 1.0 - smoothstep(width, width + blur, distance_to_line);
}

void main()
{
	vec3 normal = normalize(in_world_normal);
	vec3 albedo = in_color.rgb;
	float facing_sun = max(dot(normal, sun_direction.xyz), 0.0);
	float shadow = facing_sun > 0.0 ? sun_visible() : 0.0;
	vec3 ambient = mix(ground_color.rgb, sky_color.rgb, 0.5 + 0.5 * normal.y);
	vec3 color;

	if (in_color.a < 0.5)
	{
		float minor = max(grid_line(in_world_position.x, 1.0, 0.01), grid_line(in_world_position.z, 1.0, 0.01));
		float major = max(grid_line(in_world_position.x, 5.0, 0.02), grid_line(in_world_position.z, 5.0, 0.02));
		albedo *= 1.0 - 0.07 * minor - 0.14 * major;
	}
	color = albedo * (ambient * 0.85 + sun_color.rgb * facing_sun * shadow);
	// A gentle curve, then to sRGB by hand (the target is not sRGB)
	color = color / (1.0 + color * 0.25);
	out_color = vec4(pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
