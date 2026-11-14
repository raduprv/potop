#ifndef POTOP_SEASON_H
#define POTOP_SEASON_H

// The year in the plants (render.month): 1 the start of January .. 13 the
// end of December, round. What each month looks like is in the plants'
// pictures (the atlas' layers, engine/plant_atlas_file.h: a month shows its
// own, or the latest before it); the autumn is the shaders' (plant.glsl):
// through a plant's autumn months its foliage turns and falls, and it
// stays bare until its leaves' month; the winter's snow on the bark, and
// the branches sagging under it, through the snow's months.

// The month as 1 .. 13 (13 itself the end of December)
static inline float season_month_wrapped(float month)
{
	float wrapped = month;

	while (wrapped < 1.0f)
	{
		wrapped += 12.0f;
	}
	while (wrapped > 13.0f)
	{
		wrapped -= 12.0f;
	}
	return wrapped;
}

// How far into a span of months it is (from the start of the first to the
// end of the last, round the year): 0..1, or -1 outside it
static inline float season_span_progress(float month, float first, float last)
{
	float length = (float)(((int)last - (int)first + 12) % 12 + 1);
	float into = season_month_wrapped(month) - first;

	while (into < 0.0f)
	{
		into += 12.0f;
	}
	while (into >= 12.0f)
	{
		into -= 12.0f;
	}
	return into < length ? into / length : -1.0f;
}

// The autumn as plant.glsl takes it (0 green .. 1 bare): through its
// months, then bare until the leaves' month starts
static inline float season_autumn(float month, float first, float last, float leaves_month)
{
	float progress = season_span_progress(month, first, last);
	int bare_first = (int)last % 12 + 1;
	int bare_last = ((int)leaves_month + 10) % 12 + 1;

	if (progress >= 0.0f)
	{
		return progress;
	}
	if ((int)leaves_month == bare_first)
	{
		return 0.0f;
	}
	return season_span_progress(month, (float)bare_first, (float)bare_last) >= 0.0f ? 1.0f : 0.0f;
}

// The winter's snow on the bark, and the branches' sag (0 none .. 1 all of
// it): from the start of the first month, all of it a month on, going in
// the last half month
static inline float season_snow(float month, float first, float last)
{
	float length = (float)(((int)last - (int)first + 12) % 12 + 1);
	float progress = season_span_progress(month, first, last);
	float into;
	float coming;
	float going;

	if (progress < 0.0f)
	{
		return 0.0f;
	}
	into = progress * length;
	coming = into < 1.0f ? into : 1.0f;
	going = (length - into) / 0.5f;
	going = going < 1.0f ? going : 1.0f;
	return coming < going ? coming : going;
}

// The foliage's new growth (its lighter tips): through its months, as the
// snow comes and goes (0 none .. 1 all of it)
static inline float season_new_growth(float month, float first, float last)
{
	return season_snow(month, first, last);
}

static inline int season_month_index(float month)
{
	int index = (int)season_month_wrapped(month) - 1;

	return index < 0 ? 0 : (index > 11 ? 11 : index);
}

static inline const char *season_month_name(float month)
{
	static const char *const names[12] =
	{
		"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"
	};

	return names[season_month_index(month)];
}

#endif
