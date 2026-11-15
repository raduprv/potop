#!/usr/bin/env python3
# The poses as the engine drew them (POTOP_ANIMATION_LOG: each frame, each
# actor's action, its time, the action fading out and its time, the
# crossfade left), rebuilt from the model's glTF clips: how often it
# switches between actions, and the joints' jumps from frame to frame
# (the drawn pose, crossfade and all) against the fastest the clips
# themselves go.
#
# Usage: animation_check.py model.glb animation.log [--blend 0.25] [--actor ID]
# (--blend: the type's blend_seconds in actors.ini)

import argparse
import collections
import math
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
specification = importlib.util.spec_from_file_location("clip_check", os.path.join(os.path.dirname(os.path.abspath(__file__)), "clip_check.py"))
source = open(specification.origin).read().replace("\nmain()\n", "\n")
clip_check = {}
exec(compile(source, specification.origin, "exec"), clip_check)

import numpy


def smoothstep(x):
	x = min(max(x, 0.0), 1.0)
	return x * x * (3.0 - 2.0 * x)


def blend(a, b, amount):
	out = []
	for qa, qb in zip(a, b):
		if numpy.dot(qa, qb) < 0.0:
			qb = -qb
		q = qa + (qb - qa) * amount
		out.append(q / numpy.linalg.norm(q))
	return out


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("model")
	parser.add_argument("log")
	parser.add_argument("--blend", type=float, default=0.25)
	parser.add_argument("--actor", type=int, default=0)
	parser.add_argument("--limit", type=float, default=0.0, help="a jump: a joint faster than this (deg/s); 0: 1.3 x the clips' fastest")
	arguments = parser.parse_args()
	nodes, parents, joints, clips = clip_check["load"](arguments.model)
	names = [n.get("name", str(i)) for i, n in enumerate(nodes)]
	by_name = {c.name: c for c in clips}
	cache = {}

	def rotations(action, seconds):
		clip = by_name.get(action)
		if clip is None:
			return None
		if action.startswith(("walk", "run", "idle", "turn_", "sleep_idle")):
			seconds = math.fmod(seconds, clip.duration)
			if seconds < 0.0:
				seconds += clip.duration
		seconds = min(max(seconds, 0.0), clip.duration)
		key = (action, round(seconds * 600.0))
		if key not in cache:
			r, p = clip_check["pose"](nodes, parents, clip, seconds)
			cache[key] = [r[j] for j in joints]
		return cache[key]

	def bent(action, seconds, curve):
		pose = rotations(action, seconds)
		if pose is None or action != "walk" or abs(curve) < 0.01:
			return pose
		side = "walk_left" if curve > 0.0 else "walk_right"
		if side not in by_name:
			return pose
		phase = math.fmod(seconds, by_name["walk"].duration) / by_name["walk"].duration
		phase += 1.0 if phase < 0.0 else 0.0
		return blend(pose, rotations(side, phase * by_name[side].duration), abs(curve))

	lines = collections.defaultdict(list)
	for line in open(arguments.log):
		p = line.split()
		if len(p) < 9:
			continue
		lines[int(p[1])].append((float(p[0]), p[2], float(p[3]), float(p[4]), p[5], float(p[6]), float(p[7]), float(p[8]),
			float(p[9]) if len(p) > 9 else 0.0))
	fastest_clip = 0.0
	for clip in clips:
		if clip.name.startswith(("walk", "turn_", "idle")):
			r, p = clip_check["frames"](nodes, parents, clip)
			for j in joints:
				for k in range(len(r) - 1):
					fastest_clip = max(fastest_clip, clip_check["angle_between"](r[k][j], r[k + 1][j]) * 30.0)
	limit = arguments.limit if arguments.limit > 0.0 else 1.3 * fastest_clip
	switches = collections.Counter()
	during_blend = collections.Counter()
	jumps = collections.Counter()
	examples = collections.defaultdict(list)
	frames_total = 0
	for actor, rows in sorted(lines.items()):
		if arguments.actor and actor != arguments.actor:
			continue
		last = None
		last_row = None
		for row in rows:
			t, action, seconds, rate, previous, previous_seconds, blend_left, speed, curve = row
			pose = bent(action, seconds, curve)
			if pose is None:
				last = None
				continue
			if previous != "-" and blend_left > 0.0:
				old = bent(previous, previous_seconds, curve)
				if old is not None:
					pose = blend(old, pose, smoothstep(1.0 - blend_left / arguments.blend))
			cause = None
			if last_row is not None and last_row[1] != action:
				switches[(last_row[1], action)] += 1
				if last_row[4] != "-" and last_row[6] > 0.0:
					during_blend[(last_row[4], last_row[1], action)] += 1
					cause = "switch during a crossfade %s>%s>%s" % (last_row[4], last_row[1], action)
				else:
					cause = "switch %s>%s" % (last_row[1], action)
			if last is not None:
				dt = t - last_row[0]
				worst = (0.0, "")
				for k, j in enumerate(joints):
					speed_joint = clip_check["angle_between"](last[k], pose[k]) / max(dt, 1e-4)
					if speed_joint > worst[0]:
						worst = (speed_joint, names[j])
				frames_total += 1
				if worst[0] > limit:
					label = cause or ("in %s (blend from %s)" % (action, previous) if blend_left > 0.0 else "in " + action)
					jumps[label.split(" (")[0] if cause is None else label] += 1
					if len(examples[label]) < 3:
						examples[label].append("actor %d t %.2f %s %.0f deg/s" % (actor, t, worst[1], worst[0]))
			last = pose
			last_row = row
	print("%d frames; the clips' fastest joint %.0f deg/s, a jump: over %.0f" % (frames_total, fastest_clip, limit))
	print("\nswitches:")
	for (a, b), n in switches.most_common(20):
		print("  %-12s -> %-12s %5d" % (a, b, n))
	print("\nswitches during a crossfade (the one fading out dropped):")
	for (a, b, c), n in during_blend.most_common(12):
		print("  %s > %s > %s  %d" % (a, b, c, n))
	print("\njumps (the drawn pose):")
	for label, n in jumps.most_common(20):
		print("  %4d  %s   e.g. %s" % (n, label, "; ".join(examples[label][:2])))


main()
