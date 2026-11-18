#!/usr/bin/env python3
# Walk tests round the real houses (--houses NAME: the house alone on flat
# ground at about (96, 110)), each a list of goals: up their stairs onto
# their terraces and porches, along them, round the houses, in (where the
# door is shut: not), out again. For each: the goals reached, the samples
# off the navigation mesh, the turns on the spot (trace_analysis.py).
#
#   tests/navigation/houses.py [CASE...] [--run] [--types bear,cow,farmer]
#
# CASE: church, inn, inn_1, inn_2 (all by default). --run: also running
# (the walk test's --walk_run). Logs and traces in build/houses_*.

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trace_analysis

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (house, where the walker starts, its goals; the goals each type should
# not reach ("*": none should): inside behind a shut door, under the
# church porch's beam (1.65 m: a farmer is 1.85), up the inns' stairs
# (a tread of 0.32: a cow steps 0.3))
CASES = {
	"church": ("church", (80.0, 100.0), [(92.0, 108.2), (104.0, 108.0), (91.0, 107.3), (84.0, 104.0), (103.0, 107.4), (112.0, 100.0), (97.0, 108.5),
		(80.0, 112.0), (96.0, 104.0), (96.0, 112.0)], {"*": {9}, "farmer": {0, 1, 2, 4, 6, 9}}),
	"inn": ("inn", (96.0, 124.0), [(90.5, 108.5), (105.5, 108.6), (84.0, 112.0), (92.0, 107.3), (108.0, 112.0), (100.0, 107.3), (96.0, 124.0)],
		{"cow": {0, 3, 5}}),
	"inn_1": ("inn_1", (96.0, 124.0), [(90.5, 108.5), (105.5, 108.6), (84.0, 112.0), (92.0, 107.3), (108.0, 112.0), (100.0, 107.3), (96.0, 124.0)],
		{"cow": {0, 3, 5}}),
	"inn_2": ("inn_2", (96.0, 124.0), [(90.5, 108.5), (105.5, 108.6), (84.0, 112.0), (92.0, 107.3), (108.0, 112.0), (100.0, 107.3), (96.0, 124.0)],
		{"cow": {0, 3, 5}}),
}


def walk(case, agent, running):
	house, start, goals, unreachable_by_type = CASES[case]
	unreachable = unreachable_by_type.get(agent, unreachable_by_type.get("*", set()))
	name = "houses_%s_%s%s" % (case, agent, "_run" if running else "")
	trace = os.path.join(ROOT, "build", name + ".trace")
	environment = dict(os.environ)
	environment.setdefault("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/lvp_icd.json")
	command = ["timeout", "900", "xvfb-run", "-a", os.path.join(ROOT, "potop"), "--houses", house, "--sim", "1500", "--trace", trace, "--walk_from",
		"%.2f,%.2f" % start] + (["--walk_run"] if running else []) + ["--walk_test", agent, ";".join("%.2f,%.2f" % goal for goal in goals)]
	os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
	log = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True).stdout
	with open(os.path.join(ROOT, "build", name + ".log"), "w") as out:
		out.write(log)
	ends = {}
	for line in log.splitlines():
		if line.startswith("Walk test: goal ") and "walk_test_goal" not in line:
			parts = line.split()
			ends[int(parts[3])] = parts[6]
	wrong = 0
	marks = []
	for index in range(len(goals)):
		end = ends.get(index, "none")
		there = end == "walk_test_there"
		if there == (index in unreachable):
			wrong += 1
			marks.append("%d:%s!" % (index, end.replace("walk_test_", "")))
		else:
			marks.append("%d:%s" % (index, end.replace("walk_test_", "")))
	analysis = trace_analysis.summary(trace)
	print("%-8s %-10s %s" % (case, agent + (" run" if running else ""), " ".join(marks)))
	print("         off the mesh %d, on the spot %.0f s, the most %.0f degrees, spins %d" % (analysis["off"], analysis["turn_seconds"],
		analysis["most_degrees"], analysis["spins"]))
	return wrong


def main():
	cases = [argument for argument in sys.argv[1:] if not argument.startswith("--") and argument in CASES]
	types = ["bear", "cow", "farmer"]
	for k, argument in enumerate(sys.argv):
		if argument == "--types" and k + 1 < len(sys.argv):
			types = sys.argv[k + 1].split(",")
	running = [False, True] if "--run" in sys.argv else [False]
	wrong = 0
	for case in cases or list(CASES):
		for agent in types:
			for run in running:
				wrong += walk(case, agent, run)
	print("%d goals wrong (! above)" % wrong)
	sys.exit(1 if wrong else 0)


if __name__ == "__main__":
	main()
