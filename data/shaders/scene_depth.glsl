// The multisampled scene (render_target.h): each sample's depth written
// beside its color, as a multisampled depth can neither be read nor
// resolved; the resolve averages them into the plain depth. Every shader
// drawn into the scene's first part writes it first thing
// (scene_depth_write); without multisampling there is no target for it and
// the write goes nowhere. Needs scene.glsl.

layout(location = 1) out float fragment_depth;

void scene_depth_write()
{
	fragment_depth = gl_FragCoord.z;
}

// A cut out edge (leaves): with multisampling, its alpha sharpened to a
// pixel wide ramp round the threshold and written as the alpha, the
// pipeline's alpha to coverage making it the share of the samples drawn
// (scene's temporal_jitter.z 1: set by world_render with multisampling);
// else drawn or discarded at the threshold as before. Returns the alpha
// to write (1 without multisampling). Every pipeline drawing with it has
// alpha to coverage on (gpu_pipeline_info_t)
float alpha_cutout(float alpha, float threshold)
{
	if (temporal_jitter.z > 0.5)
	{
		float coverage = clamp((alpha - threshold) / max(fwidth(alpha), 1e-4) + 0.5, 0.0, 1.0);

		if (coverage <= 0.0)
		{
			discard;
		}
		return coverage;
	}
	if (alpha < threshold)
	{
		discard;
	}
	return 1.0;
}

// The alpha to write: with alpha to coverage the cut out edge's coverage
// (alpha_cutout's; 1 for what is not cut out), else the pixel's mark (the
// buildings' 0.25, for the temporal anti-aliasing: taa.frag)
float scene_alpha(float cut_alpha, float mark)
{
	return temporal_jitter.z > 0.5 ? cut_alpha : mark;
}
