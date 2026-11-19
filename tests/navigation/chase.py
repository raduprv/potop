#!/usr/bin/env python3
# Chases round the real houses (--houses NAME: the house alone on flat
# ground at about (96, 110)): a runner (the player, about: a farmer's way,
# the walk test's --walk_chase) goes round the house close to its walls
# and onto each of its porches and terraces, standing a while at each
# point, and once into the middle of the house (straight through the
# wall and back: somewhere the animal cannot follow); an animal after it
# as a bear after the player (sent again whenever the runner has moved
# on). The route is made from the house
# itself: a first short run only writes the navigation mesh round it
# (its low tops: the porches), the second is the chase.
#
#   tests/navigation/chase.py [HOUSE...] [--types bear,cow] [--speeds 3,5] [--pause 3]
#
# For each: how far from the runner the animal ended, how long it stood
# with nowhere to go, and how it turned (trace_analysis.py): on the spot,
# the most one way within a few meters (over LOOP_DEGREES: a loop, "!"),
# and all its turning by the meter walked (an animal that turns about for
# nothing turns a lot for its way). Logs and traces in build/chase_*.

import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nav_stress
import trace_analysis

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOUSES = ["inn", "inn_1", "church", "cottage", "gallery_house", "tall_house", "town_house", "craftsman", "stone_house", "log_cabin",
	"thatched_cottage", "hip_house", "smithy", "watermill"]
# A porch, a terrace: an object's top this far over the ground at most
PLATFORM_MOST_METERS = 2.0
PLATFORM_LEAST_METERS = 0.15
# The runner round the house this far off its tops' box
RING_METERS = 0.75
# Turned one way more than this within a few meters (trace_analysis.py's
# most_one_way): a loop, marked "!"
LOOP_DEGREES = 300.0


def run(arguments, log_path):
	environment = dict(os.environ)
	environment.setdefault("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/lvp_icd.json")
	log = subprocess.run(["timeout", "900", "xvfb-run", "-a", os.path.join(ROOT, "potop")] + nav_stress.BAKE + arguments, cwd=ROOT, env=environment, capture_output=True,
		text=True)
	text = log.stdout + log.stderr
	with open(log_path, "w") as out:
		out.write(text)
	return text


# The runner's route round a house: from the mesh a farmer has round it
def route(house):
	trace = os.path.join(ROOT, "build", "chase_%s_probe.trace" % house)
	text = run(["--houses", house, "--sim", "3", "--trace", trace, "--walk_from", "76.00,92.00", "--walk_test", "farmer", "116.00,132.00"],
		os.path.join(ROOT, "build", "chase_%s_probe.log" % house))
	if "Houses: 1 of" not in text:
		return None
	samples, rects, goals = trace_analysis.read(trace)
	ground = sorted(height for x, z, w, d, kind, height in rects if kind == 0)
	tops = [rect for rect in rects if rect[4] == 1]
	if not ground or not tops:
		return None
	ground_height = ground[len(ground) // 2]
	minimum_x = min(x for x, z, w, d, kind, height in tops)
	minimum_z = min(z for x, z, w, d, kind, height in tops)
	maximum_x = max(x + w for x, z, w, d, kind, height in tops)
	maximum_z = max(z + d for x, z, w, d, kind, height in tops)
	center = ((minimum_x + maximum_x) * 0.5, (minimum_z + maximum_z) * 0.5)
	points = []
	for x, z, w, d, kind, height in tops:
		if PLATFORM_LEAST_METERS < height - ground_height < PLATFORM_MOST_METERS:
			points.append((x + w * 0.5, z + d * 0.5))
	west = minimum_x - RING_METERS
	east = maximum_x + RING_METERS
	north = minimum_z - RING_METERS
	south = maximum_z + RING_METERS
	points += [(west, north), (center[0], north), (east, north), (east, center[1]), (east, south), (center[0], south), (west, south),
		(west, center[1])]
	# (round the house, the porches between the corners they are nearest)
	points.sort(key=lambda point: math.atan2(point[0] - center[0], point[1] - center[1]))
	start = (center[0], maximum_z + 10.0)
	# (and, half way round, into the middle of the house and back: behind a
	# shut door, on a roof, where the animal cannot follow)
	points.insert(len(points) // 2, center + ("!",))
	return [start] + points + [start]


def chase(house, points, agent, speed, pause):
	name = "chase_%s_%s_%g" % (house, agent, speed)
	trace = os.path.join(ROOT, "build", name + ".trace")
	text = run(["--houses", house, "--sim", "3000", "--trace", trace, "--walk_from", "%.2f,%.2f" % (points[0][0], points[0][1] + 4.0), "--walk_chase",
		"%g,%g" % (speed, pause), "--walk_test", agent, ";".join("%.2f,%.2f%s" % (point[0], point[1], point[2] if len(point) > 2 else "")
		for point in points)], os.path.join(ROOT, "build", name + ".log"))
	over = [line for line in text.splitlines() if "walk_chase_over" in line]
	samples, rects, goals = trace_analysis.read(trace)
	analysis = trace_analysis.summary(trace)
	meters = sum(math.hypot(b["x"] - a["x"], b["z"] - a["z"]) for a, b in zip(samples, samples[1:]))
	degrees = sum(abs(math.degrees(trace_analysis.wrap(b["yaw"] - a["yaw"]))) for a, b in zip(samples, samples[1:]))
	fields = over[0].split() if over else []
	ended = fields[fields.index("walk_chase_over") + 1] if over else "never"
	idle = fields[fields.index("idle") + 1] if over else "?"
	late = fields[fields.index("late") + 1] if over else "?"
	aside = text.count(" aside")
	nowhere = text.count("nowhere to stand")
	refused = text.count("steps refused")
	loop = analysis["loop_degrees"] > LOOP_DEGREES
	print("%-17s %-5s %g m/s  ended %-7s idle %-6s late %-6s aside %d | on the spot %3.0f s, most %3.0f, one way %3.0f%s, %2.0f degrees/m of %3.0f m, refused %d" % (
		house, agent, speed, ended, idle, late.rstrip(","), aside + nowhere, analysis["turn_seconds"], analysis["most_degrees"], analysis["loop_degrees"],
		"!" if loop else " ", degrees / max(meters, 1.0), meters, refused))
	return 1 if loop or analysis["spins"] else 0


def main():
	houses = [argument for argument in sys.argv[1:] if not argument.startswith("--") and argument in HOUSES]
	types = ["bear"]
	speeds = [3.0, 5.0]
	pause = 3.0
	for k, argument in enumerate(sys.argv):
		if argument == "--types" and k + 1 < len(sys.argv):
			types = sys.argv[k + 1].split(",")
		if argument == "--speeds" and k + 1 < len(sys.argv):
			speeds = [float(speed) for speed in sys.argv[k + 1].split(",")]
		if argument == "--pause" and k + 1 < len(sys.argv):
			pause = float(sys.argv[k + 1])
	os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
	spins = 0
	for house in houses or HOUSES:
		points = route(house)
		if points is None:
			print("%-17s did not load, or has no tops" % house)
			spins += 1
			continue
		for agent in types:
			for speed in speeds:
				spins += chase(house, points, agent, speed, pause)
	print("%d loops (! above)" % spins)
	sys.exit(1 if spins else 0)


if __name__ == "__main__":
	main()
