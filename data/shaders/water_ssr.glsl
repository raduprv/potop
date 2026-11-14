// Screen space reflections of the water (water.frag, water_reflection.comp):
// a reflected ray marched over the copy of the scene drawn before the
// water. Wants the scene block (view_projection), scene_color_texture,
// scene_depth_texture and depth_to_distance (x, y: the copy's depth to the
// distance along the view, y / (depth - x)).

// The march: steps that grow as the ray goes (near things in detail, the
// far shore still in reach), then halving the last step to the surface
#define REFLECTION_STEPS 32
#define REFLECTION_GROWTH 1.16
#define REFLECTION_REFINE 4
// How much of the ripples' bend the march follows (see main)
#define SSR_RIPPLE_SHARE 0.35

// A point to the copy of the scene: its place there (0..1), and its
// distance along the view (negative: behind the eye)
vec3 screen_place(vec3 point)
{
	vec4 clip = world_to_clip(point);
	vec2 ndc = clip.xy / max(clip.w, 1e-4);

	return vec3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, clip.w);
}

// How far behind what the copy shows at that place the point is (meters,
// along the view; negative: in front of it, or nothing there)
float behind_scene(vec3 place)
{
	float depth = textureLod(scene_depth_texture, place.xy, 0.0).r;

	if (depth >= 1.0)
	{
		return -1e9;
	}
	return place.z - depth_to_distance.y / (depth - depth_to_distance.x);
}

// What the ray from the surface meets on screen: its color (linear) and
// how sure (0: nothing found: off the screen, behind the eye, the sky or
// out of reach). The trust fades by the screen's edges and towards the end
// of the reach, so the sky takes over without a seam.
// How far to trust what is found near the screen's edges: a mirror's rays
// run mostly up the screen, so only a thin margin at the sides (a wide one
// let the sky through down both sides of every lake), more at the top and
// bottom
float screen_edge_trust(vec2 edge)
{
	return smoothstep(0.0, 0.015, edge.x) * smoothstep(0.0, 0.08, edge.y);
}

vec4 screen_reflection(vec3 origin, vec3 direction, float jitter)
{
	float step_length = 0.25 + 0.01 * length(origin - eye_position);
	float travelled = step_length * jitter;
	float previous = travelled;
	float previous_distance = screen_place(origin + direction * travelled).z;
	// Where the ray first went behind something right by the water (the
	// foot of a rock, a post, a trunk: the ray starts inside its reflection
	// and the first step lands too far behind to count as a hit): what it
	// shows if nothing else is met. Without it those rays went on to the
	// sky, and a row of bright dots lined every object's foot
	vec4 blocked = vec4(0.0);

	for (int i = 0; i < REFLECTION_STEPS; i++)
	{
		travelled += step_length;
		vec3 place = screen_place(origin + direction * travelled);

		if (place.z <= 0.05 || place.x < 0.0 || place.x > 1.0 || place.y < 0.0 || place.y > 1.0)
		{
			return blocked;
		}
		float behind = behind_scene(place);

		if (behind > 0.0 && blocked.a == 0.0 && travelled < 1.5 + 0.02 * place.z)
		{
			vec2 edge = min(place.xy, 1.0 - place.xy);

			blocked = vec4(textureLod(scene_color_texture, place.xy, 0.0).rgb, screen_edge_trust(edge));
		}

		// The ray went through a surface in this step: now behind it, but
		// by no more than it went away from the eye in the step. Further
		// behind, what the copy shows there is something in front of the
		// ray (a blade of grass, a branch) that it passed behind, not
		// through: taking it smears the grass down the water in streaks
		if (behind > 0.0 && behind < abs(place.z - previous_distance) + 0.1 + 0.002 * place.z)
		{
			float near_end = previous;
			float far_end = travelled;

			for (int k = 0; k < REFLECTION_REFINE; k++)
			{
				float middle = 0.5 * (near_end + far_end);

				if (behind_scene(screen_place(origin + direction * middle)) > 0.0)
				{
					far_end = middle;
				}
				else
				{
					near_end = middle;
				}
			}
			place = screen_place(origin + direction * far_end);
			vec2 edge = min(place.xy, 1.0 - place.xy);
			float trust = screen_edge_trust(edge) * (1.0 - smoothstep(0.7, 1.0, float(i) / float(REFLECTION_STEPS)));

			return vec4(textureLod(scene_color_texture, place.xy, 0.0).rgb, trust);
		}
		previous = travelled;
		previous_distance = place.z;
		step_length *= REFLECTION_GROWTH;
	}
	return blocked;
}
