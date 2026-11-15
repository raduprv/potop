#!/usr/bin/env python3
# The animals' trace (potop --trace FILE, game/trace.c) drawn from above
# and counted: what their minds did, where they got stuck.
#
#     tools/animal_trace.py TRACE [--out PICTURE.png] [--from SECONDS] [--to SECONDS]
#                                 [--kind KIND] [--pixels-per-meter N]
#
# The picture: the obstacles the animals' casts see (at their feet brown,
# at their middle blue grey, both grey), the homes'
# spots (areas outlined, points crossed and named), each animal's trail
# coloured by its mind's state, and what went wrong marked: a walk stopped
# short (orange x), a detour on the way in (red ring), in late (magenta
# square), no way in (big red X), walking in place (black dot). The
# numbers go to the standard output.
#
# --walk: the trails coloured by how they walk instead (the boredom walk,
# game/wander.c): green free, yellow its little turn aside (wander_drift),
# orange blocked and turned (wander_turn), blue a corner (stopped, turned
# on the spot with its turn clips), black a step back, red forced (a
# turn on the spot of more than SPIN_DEGREES within a second, walking in
# place), violet jitter (not walking, yet moved: the physics pushing it,
# at a feeder mostly), grey given up (wander_gave_up); not walking, pale.

import argparse
import math
import sys
from collections import defaultdict

from PIL import Image, ImageDraw

STATE_COLOURS = {
	"roam": (40, 160, 60),
	"busy": (220, 180, 30),
	"going_in": (40, 90, 220),
	"going_down": (130, 60, 190),
	"asleep": (90, 40, 130),
	"getting_up": (30, 180, 200),
	"going_out": (30, 180, 200),
	"dead": (200, 30, 30),
	# the bears'
	"wander": (40, 160, 60),
	"investigate": (220, 140, 30),
	"attack": (200, 30, 30),
}

# The obstacles: at the feet only (a sill, a trough's foot: brown), at the
# middle only (a rail, a trough: blue grey), both (grey)
OBSTACLE_COLOURS = { "1": (190, 140, 90), "2": (150, 165, 195), "3": (150, 150, 150) }

# --walk: a turn on the spot, this much within SPIN_SECONDS while it got
# less than SPIN_METERS on; an event colours its trail this long after it
SPIN_DEGREES = 40.0
SPIN_SECONDS = 1.0
SPIN_METERS = 0.3
JITTER_METERS = 0.015             # not walking, yet moved this much from one sample to the next
WALK_EVENT_SECONDS = 0.6
WALK_COLOURS = {
	"free": (40, 170, 60),
	"wander_drift": (230, 200, 0),
	"wander_turn": (245, 130, 0),
	"forced": (220, 20, 20),
	"wander_back": (0, 0, 0),
	"wander_corner": (30, 110, 255),
	"jitter": (170, 0, 220),
	"wander_gave_up": (120, 120, 120),
	"still": (215, 225, 215),
}

# Walking in place: "moving" this long while it got less than this far
IN_PLACE_SECONDS = 3.0
IN_PLACE_METERS = 0.15


def read_trace(path):
	samples = defaultdict(list)       # id -> [(t, kind, state, x, z, moving, hour)]
	yaws = defaultdict(dict)          # id -> {t: yaw} (newer traces)
	events = []                       # (t, id, kind, x, z, text)
	spots = []                        # (home, kind, name, x, z, yaw, half_w, half_d)
	obstacles = None
	with open(path) as stream:
		lines = iter(stream)
		for line in lines:
			if line.startswith("a "):
				p = line.split()
				samples[int(p[2])].append((float(p[1]), p[3], p[4], float(p[5]), float(p[6]), p[7] == "1", float(p[8])))
				if len(p) > 9:
					yaws[int(p[2])][float(p[1])] = float(p[9])
			elif line.startswith("e "):
				p = line.split(None, 6)
				events.append((float(p[1]), int(p[2]), p[3], float(p[4]), float(p[5]), p[6].strip()))
			elif line.startswith("s "):
				p = line.split()
				spots.append((int(p[1]), p[2], p[3], float(p[4]), float(p[5]), float(p[6]), float(p[7]), float(p[8])))
			elif line.startswith("o "):
				p = line.split()
				x0, z0, cell, cells = float(p[1]), float(p[2]), float(p[3]), int(p[4])
				rows = [next(lines).strip() for _ in range(cells)]
				obstacles = (x0, z0, cell, cells, rows)
	return samples, events, spots, obstacles, yaws


# Whether a body stands at its feet there (under a raised hen house, a
# trough's top, a hen is not in anything)
def obstacle_at(obstacles, x, z):
	if obstacles is None:
		return False
	x0, z0, cell, cells, rows = obstacles
	column = int((x - x0) / cell)
	row = int((z - z0) / cell)
	return 0 <= row < cells and 0 <= column < cells and rows[row][column] in "13"


def main():
	parser = argparse.ArgumentParser(description="The animals' trace, drawn and counted")
	parser.add_argument("trace")
	parser.add_argument("--out", default=None)
	parser.add_argument("--from", dest="time_from", type=float, default=0.0)
	parser.add_argument("--to", dest="time_to", type=float, default=1e30)
	parser.add_argument("--kind", default=None)
	parser.add_argument("--pixels-per-meter", type=float, default=0.0)
	parser.add_argument("--walk", action="store_true", help="colour the trails by how they walk (game/wander.c)")
	arguments = parser.parse_args()

	samples, events, spots, obstacles, yaws = read_trace(arguments.trace)
	for animal in list(samples):
		samples[animal] = [s for s in samples[animal] if arguments.time_from <= s[0] <= arguments.time_to
			and (arguments.kind is None or s[1] == arguments.kind)]
		if not samples[animal]:
			del samples[animal]
	events = [e for e in events if arguments.time_from <= e[0] <= arguments.time_to and (arguments.kind is None or e[2] == arguments.kind)]

	# What went wrong, found in the samples
	in_place = []                     # (t, id, kind, x, z, seconds)
	inside = defaultdict(int)         # kind -> samples inside an obstacle cell
	inside_points = []
	state_time = defaultdict(lambda: defaultdict(float))
	night_in = {}                     # id -> seconds from going_in to asleep
	for animal, trail in samples.items():
		start = None
		going_in_at = None
		for k, (t, kind, state, x, z, moving, hour) in enumerate(trail):
			if k + 1 < len(trail):
				state_time[kind][state] += trail[k + 1][0] - t
			if moving:
				if start is None:
					start = k
				elif t - trail[start][0] >= IN_PLACE_SECONDS:
					if math.hypot(x - trail[start][3], z - trail[start][4]) < IN_PLACE_METERS:
						in_place.append((t, animal, kind, x, z, t - trail[start][0]))
					start = k
			else:
				start = None
			if obstacle_at(obstacles, x, z) and state not in ("asleep", "going_down", "dead"):
				inside[kind] += 1
				inside_points.append((x, z))
			if state == "going_in" and going_in_at is None:
				going_in_at = t
			if state in ("going_down", "asleep") and going_in_at is not None:
				night_in[animal] = (kind, t - going_in_at)
				going_in_at = None

	# The numbers
	kinds = sorted({s[1] for trail in samples.values() for s in trail})
	print("trace %s: %d animals, %.0f s" % (arguments.trace, len(samples),
		max((trail[-1][0] for trail in samples.values()), default=0.0) - min((trail[0][0] for trail in samples.values()), default=0.0)))
	for kind in kinds:
		animals = [a for a, trail in samples.items() if trail[0][1] == kind]
		total = sum(state_time[kind].values()) or 1.0
		minutes = total / 60.0
		shares = ", ".join("%s %.0f%%" % (state, 100.0 * seconds / total) for state, seconds in sorted(state_time[kind].items(), key=lambda item: -item[1]))
		print("\n%s: %d animals, %.1f animal-minutes" % (kind, len(animals), minutes))
		print("  time: " + shares)
		kind_events = defaultdict(list)
		for e in events:
			if e[2] == kind:
				kind_events[e[5].split()[0]].append(e)
		for name, found in sorted(kind_events.items()):
			line = "  %-14s %4d (%.1f a minute)" % (name, len(found), len(found) / max(minutes, 1e-6))
			if name == "walk_short":
				distances = sorted(float(e[5].split()[1]) for e in found)
				line += ", short by %.1f m median, %.1f m most" % (distances[len(distances) // 2], distances[-1])
			print(line)
		kind_in_place = [p for p in in_place if p[2] == kind]
		if kind_in_place:
			print("  walking in place: %d times (%.0f s or more each), e.g. at %s" % (len(kind_in_place), IN_PLACE_SECONDS,
				", ".join("(%.1f, %.1f) t %.0f" % (p[3], p[4], p[0]) for p in kind_in_place[:4])))
		if inside[kind]:
			print("  inside an obstacle (at the feet): %d samples (%.1f%% of its awake ones)" % (inside[kind], 100.0 * inside[kind] / max(1, sum(len(samples[a]) for a in animals))))
		times = sorted(seconds for k, seconds in night_in.values() if k == kind)
		if times:
			print("  in for the night: %d, %.0f s median, %.0f s the slowest" % (len(times), times[len(times) // 2], times[-1]))

	# Turns on the spot (newer traces, with the yaw): this much within a
	# second while it hardly got on
	spins = []                        # (t, id, kind, x, z, degrees)
	spin_times = defaultdict(list)    # id -> [t]
	for animal, trail in samples.items():
		turns = yaws.get(animal, {})
		if not turns:
			continue
		j = 0
		last_spin = -1e9
		for k in range(len(trail)):
			t, kind, state, x, z = trail[k][:5]
			while trail[j][0] < t - SPIN_SECONDS:
				j += 1
			if j == k or trail[j][0] not in turns or t not in turns or state in ("asleep", "dead"):
				continue
			turned = abs((turns[t] - turns[trail[j][0]] + math.pi) % (2.0 * math.pi) - math.pi)
			moved = math.hypot(x - trail[j][3], z - trail[j][4])
			if math.degrees(turned) > SPIN_DEGREES and moved < SPIN_METERS:
				spin_times[animal].append(t)
				if t - last_spin > SPIN_SECONDS:
					spins.append((t, animal, kind, x, z, math.degrees(turned)))
				last_spin = t
	# Jitter: not walking, yet moved (the physics pushing it about)
	jitter_times = defaultdict(set)   # id -> {t}
	jitter_count = defaultdict(int)   # kind -> samples
	for animal, trail in samples.items():
		for k in range(1, len(trail)):
			a = trail[k - 1]
			b = trail[k]
			if not a[5] and not b[5] and b[2] not in ("asleep", "dead", "going_in", "going_out") \
					and math.hypot(b[3] - a[3], b[4] - a[4]) > JITTER_METERS and b[0] - a[0] < 1.0:
				jitter_times[animal].add(b[0])
				jitter_count[b[1]] += 1
	if jitter_count:
		print("\njitter (not walking, moved over %.0f cm a sample): %s" % (JITTER_METERS * 100.0,
			", ".join("%s %d" % (kind, n) for kind, n in sorted(jitter_count.items()))))
	if spins:
		print("\nturns on the spot (more than %.0f degrees in %.0f s, under %.1f m on):" % (SPIN_DEGREES, SPIN_SECONDS, SPIN_METERS))
		for kind in sorted(set(s[2] for s in spins)):
			found = [s for s in spins if s[2] == kind]
			print("  %s: %d, e.g. %s" % (kind, len(found), ", ".join("(%.1f, %.1f) t %.0f" % (s[3], s[4], s[0]) for s in found[:4])))

	if arguments.out is None:
		return
	# The picture: the obstacles' square, or round the trails
	if obstacles is not None:
		x0, z0, cell, cells, rows = obstacles
		side = cell * cells
	else:
		xs = [s[3] for trail in samples.values() for s in trail]
		zs = [s[4] for trail in samples.values() for s in trail]
		x0, z0 = min(xs) - 5.0, min(zs) - 5.0
		side = max(max(xs) - x0, max(zs) - z0) + 5.0
	scale = arguments.pixels_per_meter if arguments.pixels_per_meter > 0.0 else min(16.0, 1800.0 / side)
	size = int(side * scale) + 1
	picture = Image.new("RGB", (size, size), (250, 250, 245))
	draw = ImageDraw.Draw(picture)

	# (z up the picture: north up, as the map)
	def at(x, z):
		return ((x - x0) * scale, size - 1 - (z - z0) * scale)

	if obstacles is not None:
		for row in range(cells):
			line = rows[row]
			for column in range(cells):
				if line[column] != "0":
					a = at(x0 + column * cell, z0 + (row + 1) * cell)
					draw.rectangle([a[0], a[1], a[0] + cell * scale, a[1] + cell * scale], fill=OBSTACLE_COLOURS[line[column]])
	for home, kind, name, x, z, yaw, half_w, half_d in spots:
		if half_w > 0.0 or half_d > 0.0:
			c, s = math.cos(yaw), math.sin(yaw)
			corners = []
			for u, v in ((-half_w, -half_d), (half_w, -half_d), (half_w, half_d), (-half_w, half_d)):
				corners.append(at(x + u * c + v * s, z - u * s + v * c))
			draw.polygon(corners, outline=(120, 120, 220))
			draw.text(at(x - half_w * 0.9, z + half_d * 0.9), name, fill=(90, 90, 200))
		else:
			p = at(x, z)
			draw.line([p[0] - 4, p[1], p[0] + 4, p[1]], fill=(60, 60, 200))
			draw.line([p[0], p[1] - 4, p[0], p[1] + 4], fill=(60, 60, 200))
			draw.text((p[0] + 5, p[1] - 5), name, fill=(60, 60, 200))
	walk_events = defaultdict(list)   # id -> [(t, name)]
	for t, animal, kind, x, z, text in events:
		name = text.split()[0]
		if name in ("wander_drift", "wander_turn", "wander_back", "wander_gave_up", "wander_corner"):
			walk_events[animal].append((t, name))
	in_place_times = defaultdict(list)
	for t, animal, kind, x, z, seconds in in_place:
		in_place_times[animal].append((t, seconds))
	for animal, trail in samples.items():
		marks = walk_events.get(animal, [])
		m = 0
		for k in range(1, len(trail)):
			a = trail[k - 1]
			b = trail[k]
			if not arguments.walk:
				colour = STATE_COLOURS.get(b[2], (0, 0, 0))
			else:
				t = b[0]
				while m < len(marks) and marks[m][0] < t - WALK_EVENT_SECONDS:
					m += 1
				recent = [name for when, name in marks[m:] if when <= t]
				moved = math.hypot(b[3] - a[3], b[4] - a[4]) > 0.01
				if "wander_back" in recent:
					colour = WALK_COLOURS["wander_back"]
				elif "wander_corner" in recent or any(when <= t <= when + 4.0 for when, name in marks if name == "wander_corner"):
					colour = WALK_COLOURS["wander_corner"]
				elif t in spin_times.get(animal, ()) or any(when <= t <= when + seconds for when, seconds in in_place_times.get(animal, ())):
					colour = WALK_COLOURS["forced"]
				elif t in jitter_times.get(animal, ()):
					colour = WALK_COLOURS["jitter"]
				elif recent:
					order = ("wander_gave_up", "wander_turn", "wander_drift")
					colour = WALK_COLOURS[min(recent, key=order.index)]
				elif b[2] == "roam" and moved:
					colour = WALK_COLOURS["free"]
				else:
					colour = WALK_COLOURS["still"]
			draw.line([at(a[3], a[4]), at(b[3], b[4])], fill=colour, width=2)
	for t, animal, kind, x, z, seconds in in_place:
		p = at(x, z)
		draw.ellipse([p[0] - 3, p[1] - 3, p[0] + 3, p[1] + 3], fill=WALK_COLOURS["forced"] if arguments.walk else (0, 0, 0))
	for t, animal, kind, x, z, text in events:
		p = at(x, z)
		name = text.split()[0]
		if name == "walk_short":
			draw.line([p[0] - 3, p[1] - 3, p[0] + 3, p[1] + 3], fill=(240, 140, 0), width=2)
			draw.line([p[0] - 3, p[1] + 3, p[0] + 3, p[1] - 3], fill=(240, 140, 0), width=2)
		elif name == "detour":
			draw.ellipse([p[0] - 5, p[1] - 5, p[0] + 5, p[1] + 5], outline=(220, 0, 0), width=2)
		elif name == "in_late":
			draw.rectangle([p[0] - 5, p[1] - 5, p[0] + 5, p[1] + 5], outline=(220, 0, 220), width=2)
		elif name == "no_way_in":
			draw.line([p[0] - 8, p[1] - 8, p[0] + 8, p[1] + 8], fill=(220, 0, 0), width=3)
			draw.line([p[0] - 8, p[1] + 8, p[0] + 8, p[1] - 8], fill=(220, 0, 0), width=3)
	# (the key, top left)
	y = 4
	if arguments.walk:
		for name, label in (("free", "free"), ("wander_drift", "drift aside"), ("wander_turn", "blocked, turned"), ("wander_back", "step back"), ("wander_corner", "corner: turn on the spot"),
				("forced", "spin / in place (dot)"), ("jitter", "jitter (physics)"), ("wander_gave_up", "gave up"), ("still", "not walking")):
			draw.rectangle([4, y + 2, 14, y + 10], fill=WALK_COLOURS[name])
			draw.text((18, y), label, fill=(0, 0, 0))
			y += 13
	else:
		for state in ("roam", "busy", "going_in", "going_down", "asleep", "going_out", "dead"):
			draw.rectangle([4, y + 2, 14, y + 10], fill=STATE_COLOURS[state])
			draw.text((18, y), state, fill=(0, 0, 0))
			y += 13
	draw.text((4, y + 2), "x walk short  o detour  [] in late  X no way in  . in place   %.1f px/m" % scale, fill=(0, 0, 0))
	picture.save(arguments.out)
	print("\npicture: %s (%d x %d, %.1f px a meter)" % (arguments.out, size, size, scale))


if __name__ == "__main__":
	sys.exit(main())
