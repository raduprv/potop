#!/usr/bin/env python3
# What a walk test's trace (--trace) says about how an actor walked:
# where it left the navigation mesh, and how it turned on the spot.
#
#   tests/navigation/trace_analysis.py TRACE      a summary of the trace
#
# Trace lines (game/trace.c): "a TIME ID TYPE STATE X Z MOVING HOUR YAW Y"
# an actor every quarter of a second; "r X Z W D KIND HEIGHT" the
# navigation mesh's rectangles round the trail (its class's; KIND 1 an
# object's top); "g X Z" the walk test's goals in turn.

import math
import os
import sys

# Moved less than this in a quarter of a second (0.4 m/s): on the spot,
# or as good as (a bear walks 0.77 m/s); or less than half its walking
# pace, for one that walks slower than that (a hen's 0.25 m/s: all of its
# walk was "on the spot", 20 minutes of it, and its turns walking "spins")
SPOT_METERS = 0.1
SPOT_PACE_SHARE = 0.5
ACTORS_INI = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "data", "actors.ini")
# A rectangle's height this near the actor's: the one it stands on
HEIGHT_SLACK = 0.5
# A loop: turned one way within this much walking (most_one_way)
LOOP_METERS = 8.0


# Each type's walking pace (data/actors.ini: walk_meters_per_second, 1.5
# where it is not given)
def walking_paces():
	paces = {}
	section = None
	try:
		with open(ACTORS_INI) as ini:
			for line in ini:
				line = line.split(";")[0].strip()
				if line.startswith("[") and line.endswith("]"):
					section = line[1:-1]
					paces[section] = 1.5
				elif section is not None and line.replace(" ", "").startswith("walk_meters_per_second="):
					paces[section] = float(line.split("=")[1])
	except OSError:
		pass
	return paces


def read(path):
	paces = walking_paces()
	samples = []
	rects = []
	goals = []
	with open(path) as trace:
		for line in trace:
			parts = line.split()
			if not parts:
				continue
			if parts[0] == "a" and len(parts) >= 11:
				samples.append({"time": float(parts[1]), "x": float(parts[5]), "z": float(parts[6]), "moving": parts[7] == "1",
					"yaw": float(parts[9]), "y": float(parts[10]), "pace": paces.get(parts[3], 1.5)})
			elif parts[0] == "r" and len(parts) >= 7:
				rects.append((float(parts[1]), float(parts[2]), float(parts[3]), float(parts[4]), int(parts[5]), float(parts[6])))
			elif parts[0] == "g" and len(parts) >= 3:
				goals.append((float(parts[1]), float(parts[2])))
	return samples, rects, goals


# The samples off the mesh: no rectangle under it at about its height
def off_mesh(samples, rects):
	off = []
	for sample in samples:
		on = False
		for x, z, w, d, kind, height in rects:
			if x <= sample["x"] <= x + w and z <= sample["z"] <= z + d and abs(height - sample["y"]) < HEIGHT_SLACK:
				on = True
				break
		if not on:
			off.append(sample)
	return off


def wrap(angle):
	while angle > math.pi:
		angle -= 2.0 * math.pi
	while angle < -math.pi:
		angle += 2.0 * math.pi
	return angle


# Its turns on the spot: runs of samples, moving, where it went (almost)
# nowhere; each run's degrees turned (all of them, either way) and
# seconds. A spin is a run of more than 360 degrees.
def spot_turns(samples):
	runs = []
	run = None
	for previous, sample in zip(samples, samples[1:]):
		moved = math.hypot(sample["x"] - previous["x"], sample["z"] - previous["z"])
		if sample["moving"] and moved < min(SPOT_METERS, SPOT_PACE_SHARE * sample["pace"] * (sample["time"] - previous["time"])):
			if run is None:
				run = {"start": previous["time"], "x": previous["x"], "z": previous["z"], "degrees": 0.0, "seconds": 0.0}
			run["degrees"] += abs(math.degrees(wrap(sample["yaw"] - previous["yaw"])))
			run["seconds"] = sample["time"] - run["start"]
		elif run is not None:
			runs.append(run)
			run = None
	if run is not None:
		runs.append(run)
	return [run for run in runs if run["degrees"] > 1.0]


# The most it turned one way (left or right, the other way taken off)
# within LOOP_METERS of walking: a house's corner is a quarter turn, a
# turn about on a terrace half a one; a whole turn and more is a loop, a
# spin on the spot or one walked
def most_one_way(samples):
	meters = [0.0]
	turned = [0.0]
	for previous, sample in zip(samples, samples[1:]):
		meters.append(meters[-1] + math.hypot(sample["x"] - previous["x"], sample["z"] - previous["z"]))
		turned.append(turned[-1] + math.degrees(wrap(sample["yaw"] - previous["yaw"])))
	most = {"degrees": 0.0, "start": 0.0, "end": 0.0, "x": 0.0, "z": 0.0}
	first = 0
	for last in range(len(samples)):
		while meters[last] - meters[first] > LOOP_METERS:
			first += 1
		for k in range(first, last):
			if abs(turned[last] - turned[k]) > most["degrees"]:
				most = {"degrees": abs(turned[last] - turned[k]), "start": samples[k]["time"], "end": samples[last]["time"], "x": samples[last]["x"],
					"z": samples[last]["z"]}
	return most


def summary(path):
	samples, rects, goals = read(path)
	off = off_mesh(samples, rects)
	turns = spot_turns(samples)
	loop = most_one_way(samples)
	spins = [turn for turn in turns if turn["degrees"] > 360.0]
	lines = ["%d samples, %d off the mesh" % (len(samples), len(off))]
	for sample in off[:10]:
		lines.append("  off at %.2f s: (%.2f, %.2f) %.2f m up" % (sample["time"], sample["x"], sample["z"], sample["y"]))
	lines.append("turns on the spot: %d, %.0f s in all, the most %.0f degrees; spins (over 360): %d" % (len(turns),
		sum(turn["seconds"] for turn in turns), max([turn["degrees"] for turn in turns] or [0.0]), len(spins)))
	for turn in sorted(turns, key=lambda turn: -turn["degrees"])[:5]:
		lines.append("  %.0f degrees in %.1f s from %.2f s at (%.2f, %.2f)" % (turn["degrees"], turn["seconds"], turn["start"], turn["x"], turn["z"]))
	lines.append("the most one way within %.0f m: %.0f degrees, from %.1f to %.1f s, by (%.2f, %.2f)" % (LOOP_METERS, loop["degrees"], loop["start"],
		loop["end"], loop["x"], loop["z"]))
	return {"off": len(off), "turn_seconds": sum(turn["seconds"] for turn in turns), "most_degrees": max([turn["degrees"] for turn in turns] or [0.0]),
		"spins": len(spins), "loop_degrees": loop["degrees"], "text": "\n".join(lines)}


if __name__ == "__main__":
	if len(sys.argv) != 2:
		sys.exit("tests/navigation/trace_analysis.py TRACE")
	print(summary(sys.argv[1])["text"])
