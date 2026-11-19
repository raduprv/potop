#!/usr/bin/env python3
# The small things' test: a grove of trunks (some leaning), boulders and
# buckets on flat ground, and a row of trunks too close to pass between;
# each kind of agent walked through and round them. Small things are not
# in the navigation's rectangles (engine/navigation.h: object_kind): the
# ways are led round them. What is asked: every goal reached, and the
# body never inside a trunk or a boulder on the way (a bucket is soft:
# only counted).
#
#   tests/navigation/small.py build              the models (boxes, as nav_stress.py's)
#   tests/navigation/small.py run [TYPE...] [--walls]   the walks, a table
#                                         (--walls: all of them walls, the
#                                         way it was, to compare)
#
# The game runs headless, --houses nav_fence (a fence's segment: longer
# than [simulation] navigation_wall_meters, a wall) and --put for the rest.

import math
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nav_stress
import trace_analysis

ROOT = nav_stress.ROOT
AGENTS = ["hen", "farmer", "bear", "cow", "bear+run"]
# The models: boxes (x0, x1, y0, y1, z0, z1); what is in a body's way of
# each is its box up to a head's height
MODELS = {
	"nav_fence": [(-1.0, 1.0, 0.0, 1.1, -0.06, 0.06)],
	"nav_trunk": [(-0.2, 0.2, 0.0, 5.0, -0.2, 0.2)],
	# (leans 20 cm for every meter up: over a hen's head by its root, over
	# a man's 40 cm from it. Much more and its reach is a wall's)
	"nav_trunk_bent": [(-0.2 + 0.1 * k, 0.2 + 0.1 * k, 0.5 * k, 0.5 * (k + 1), -0.2, 0.2) for k in range(10)],
	"nav_boulder": [(-0.4, 0.4, 0.0, 0.7, -0.35, 0.35)],
	"nav_bucket": [(-0.15, 0.15, 0.0, 0.35, -0.15, 0.15)],
}
SOFT = {"nav_bucket"}
# The grove: this box of the flat ground, so many of each, no two nearer
# than this (their middles)
GROVE = (70.0, 80.0, 90.0, 100.0)
GROVE_COUNTS = [("nav_trunk", 60), ("nav_trunk_bent", 12), ("nav_boulder", 12), ("nav_bucket", 12)]
GROVE_SPACING = 1.3
# The row: trunks this far apart (nothing but a hen fits between), along z
ROW_X = 80.0
ROW_Z = (106.0, 114.0)
ROW_SPACING = 0.75
START = (64.0, 90.0)
# Through the grove and back, across it corner to corner, into its
# middle; then to either side of the row and back
GOALS = [(96.0, 90.0), (66.0, 98.0), (92.0, 82.0), (80.0, 90.0), (66.0, 84.0), (76.0, 110.0), (84.0, 110.0), (76.0, 110.0)]
# (a hen is slow: nearer ones)
HEN_START = (69.0, 90.0)
HEN_GOALS = [(80.0, 90.0), (74.0, 84.0), (78.5, 110.0), (81.5, 110.0)]
# A body this far inside a trunk's box: through it (the trail is its
# middle's; a quarter meter is the grid's)
INSIDE_METERS = 0.15


def build():
	for name, boxes in MODELS.items():
		nav_stress.write_model(boxes, name)
	print("small things: %s" % ", ".join(MODELS))


def placements():
	generator = random.Random(7)
	placed = []
	for name, count in GROVE_COUNTS:
		tries = 0
		while count > 0 and tries < 100000:
			tries += 1
			x = generator.uniform(GROVE[0], GROVE[2])
			z = generator.uniform(GROVE[1], GROVE[3])
			if all((x - px) ** 2 + (z - pz) ** 2 >= GROVE_SPACING ** 2 for _, px, pz in placed):
				placed.append((name, x, z))
				count -= 1
	z = ROW_Z[0]
	while z <= ROW_Z[1] + 1e-6:
		placed.append(("nav_trunk", ROW_X, z))
		z += ROW_SPACING
	return placed


# How far inside an object's box (as high as the agent) a body's circle
# is: over 0 they overlap. The objects stand as their models lie (--put:
# not turned)
def inside(x, z, radius, height, name, px, pz):
	worst = -1e9
	for x0, x1, y0, y1, z0, z1 in MODELS[name]:
		if y0 >= height:
			continue
		dx = max(px + x0 - x, 0.0, x - (px + x1))
		dz = max(pz + z0 - z, 0.0, z - (pz + z1))
		worst = max(worst, radius - math.hypot(dx, dz))
	return worst


def walk(agent, placed):
	running = agent.endswith("+run")
	goals_of = HEN_GOALS if agent == "hen" else GOALS
	kind = agent.split("+")[0]
	name = "small_%s" % agent.replace("+", "_")
	trace = os.path.join(ROOT, "build", name + ".trace")
	environment = dict(os.environ)
	environment.setdefault("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/lvp_icd.json")
	command = ["timeout", "900", "xvfb-run", "-a", os.path.join(ROOT, "potop"), "--houses", "nav_fence"]
	for model in MODELS:
		points = ["%.2f,%.2f" % (x, z) for placed_name, x, z in placed if placed_name == model]
		if points and model != "nav_fence":
			command += ["--put", model, ";".join(points)]
	# (--walls: everything a wall, as before the small things' rule, to compare)
	if "--walls" in sys.argv:
		command += ["--set", "simulation.navigation_wall_meters=0"]
	command += ["--sim", "1500", "--trace", trace, "--walk_from", "%.2f,%.2f" % (HEN_START if kind == "hen" else START)] + (["--walk_run"] if running else [])
	command += ["--walk_test", kind, ";".join("%.2f,%.2f" % goal for goal in goals_of)]
	os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
	log = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True).stdout
	with open(os.path.join(ROOT, "build", name + ".log"), "w") as out:
		out.write(log)
	if "Houses: 1 of" not in log or log.count("Put: ") < len(MODELS) - 1:
		sys.exit("small: the models did not load (tests/navigation/small.py build; see build/%s.log)" % name)
	ends = {}
	for line in log.splitlines():
		if line.startswith("Walk test: goal ") and "walk_test_goal" not in line:
			parts = line.split()
			ends[int(parts[3])] = parts[6].replace("walk_test_", "")
	wrong = sum(1 for index in range(len(goals_of)) if ends.get(index, "none") != "there")
	samples, rects, goals = trace_analysis.read(trace)
	size = nav_stress.AGENT_SIZES[kind]
	radius = 0.125 * size["quarters"]
	through = 0
	deepest = 0.0
	soft = 0
	for sample in samples:
		hard_here = 0.0
		soft_here = 0.0
		for placed_name, px, pz in placed:
			if abs(px - sample["x"]) > 3.0 or abs(pz - sample["z"]) > 3.0:
				continue
			depth = inside(sample["x"], sample["z"], radius, size["height"], placed_name, px, pz)
			if placed_name in SOFT:
				soft_here = max(soft_here, depth)
			else:
				hard_here = max(hard_here, depth)
		through += 1 if hard_here > INSIDE_METERS else 0
		soft += 1 if soft_here > INSIDE_METERS else 0
		deepest = max(deepest, hard_here)
	walked = sum(math.hypot(b["x"] - a["x"], b["z"] - a["z"]) for a, b in zip(samples, samples[1:]))
	seconds = samples[-1]["time"] - samples[0]["time"] if samples else 0.0
	analysis = trace_analysis.summary(trace)
	grove = sum(1 for rect in rects if rect[0] < GROVE[2] and rect[0] + rect[2] > GROVE[0] and rect[1] < GROVE[3] and rect[1] + rect[3] > GROVE[1])
	print("%-9s %s" % (agent, " ".join("%d:%s%s" % (index, ends.get(index, "none"), "" if ends.get(index) == "there" else "!")
		for index in range(len(goals_of)))))
	print("          %.0f m in %.0f s, through a trunk or boulder %d samples%s (deepest %.2f m), in a bucket %d; on the spot %.0f s, the most %.0f degrees; %d rectangles in the grove"
		% (walked, seconds, through, "!" if through else "", deepest, soft, analysis["turn_seconds"], analysis["most_degrees"], grove))
	return wrong + (1 if through else 0)


def main():
	if len(sys.argv) < 2 or sys.argv[1] not in ("build", "run"):
		sys.exit(__doc__ or "small.py build | run [TYPE...]")
	if sys.argv[1] == "build":
		build()
		return
	if not all(os.path.exists(os.path.join(ROOT, "data", "objects", name + ".pobj")) for name in MODELS):
		build()
	placed = placements()
	wrong = 0
	for agent in [argument for argument in sys.argv[2:] if not argument.startswith("--")] or AGENTS:
		wrong += walk(agent, placed)
	print("%d wrong (! above)" % wrong)
	sys.exit(1 if wrong else 0)


if __name__ == "__main__":
	main()
