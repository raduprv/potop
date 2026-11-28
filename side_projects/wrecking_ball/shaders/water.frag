#version 450
// The water's surface, see through: clearer where shallow, the sky in it
// at a slant (Schlick's Fresnel), the sun's glint where it is not in
// shadow

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_world_normal;
layout(location = 2) in float in_depth;
layout(location = 3) in vec4 in_light_position;

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2DShadow shadow_map;

layout(set = 3, binding = 0) uniform light_block
{
	vec4 sun_direction;
	vec4 sun_color;
	vec4 sky_color;
	vec4 ground_color;
	vec4 fog_color;
	vec4 eye;
	vec4 shadow_texel;
};

float sun_visible()
{
	vec3 position = in_light_position.xyz / in_light_position.w;
	vec2 coordinates = vec2(0.5 + 0.5 * position.x, 0.5 - 0.5 * position.y);

	if (any(lessThan(coordinates, vec2(0.0))) || any(greaterThan(coordinates, vec2(1.0))) || position.z > 1.0)
	{
		return 1.0;
	}
	return texture(shadow_map, vec3(coordinates, position.z - 0.002));
}

void main()
{
	vec3 normal = normalize(in_world_normal);
	vec3 to_eye = normalize(eye.xyz - in_world_position);
	float shadow = sun_visible();
	vec3 half_vector = normalize(to_eye + sun_direction.xyz);
	float glint = pow(max(dot(normal, half_vector), 0.0), 180.0) * 6.0 * shadow;
	float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(normal, to_eye), 0.0), 5.0);
	vec3 shallow = vec3(0.10, 0.32, 0.36);
	vec3 deep = vec3(0.02, 0.10, 0.18);
	float deepness = clamp(in_depth / 1.5, 0.0, 1.0);
	vec3 body = mix(shallow, deep, deepness) * (0.35 + 0.65 * shadow * max(sun_direction.y, 0.0));
	float alpha = clamp(0.3 + in_depth * 2.0, 0.3, 0.88);
	// (fading out at the shore, where the grid's steps would show)
	float edge = smoothstep(0.0, 0.04, in_depth);

	alpha = max(alpha, fresnel) * edge;
	glint *= edge * pow(normal.y, 16.0);
	vec3 color = mix(body, sky_color.rgb, fresnel) + sun_color.rgb * glint;


	color = color / (1.0 + color * 0.25);
	out_color = vec4(pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2)), alpha);
}
