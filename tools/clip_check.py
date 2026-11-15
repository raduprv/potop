#!/usr/bin/env python3
# Checks an animated glTF's clips by their bones (no rendering):
#
#   jumps     per clip, each joint's rotation from one frame to the next
#             (30 frames a second) and the change in that (a snap: a high
#             speed or a high acceleration for one frame)
#   seams     the loops: the pose at the end against the start, and the
#             joints' speed across the seam against inside the clip
#   matches   between the walks (walk, walk_left, walk_right): for each
#             frame of one, the frame of the other nearest it by the legs'
#             joints (where they are, from the body's root, and how they
#             go); how far the best is, and how far the same frame is
#   slip      the walks moved as the engine moves them (the walk's speed;
#             walk_left / walk_right turned TURN degrees a cycle as well):
#             how far a foot on the ground slides
#
# Usage: clip_check.py model.glb [--cycle-meters M] [--turn DEGREES]

import argparse
import json
import math
import struct
import sys

import numpy

FRAMES_PER_SECOND = 30.0
LEG_WORDS = ("thigh", "lower_leg", "foot", "toe", "upper_arm", "forearm", "shin", "calf", "leg")
FOOT_WORDS = ("toe", "foot")


def read_glb(path):
	data = open(path, "rb").read()
	magic, version, length = struct.unpack_from("<III", data, 0)
	if magic != 0x46546C67:
		sys.exit("not a glb")
	offset = 12
	document = None
	binary = b""
	while offset < length:
		chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
		chunk = data[offset + 8:offset + 8 + chunk_length]
		if chunk_type == 0x4E4F534A:
			document = json.loads(chunk)
		elif chunk_type == 0x004E4942:
			binary = chunk
		offset += 8 + chunk_length
	return document, binary


COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
TYPES = {5126: ("<f", 4, None), 5123: ("<H", 2, 65535.0), 5121: ("<B", 1, 255.0), 5122: ("<h", 2, 32767.0), 5120: ("<b", 1, 127.0)}


def accessor(document, binary, index):
	item = document["accessors"][index]
	view = document["bufferViews"][item["bufferView"]]
	form, size, normal = TYPES[item["componentType"]]
	count = COMPONENTS[item["type"]]
	stride = view.get("byteStride", size * count)
	start = view.get("byteOffset", 0) + item.get("byteOffset", 0)
	values = numpy.zeros((item["count"], count), dtype=numpy.float64)
	for i in range(item["count"]):
		values[i] = struct.unpack_from("<" + form[1] * count, binary, start + i * stride)
	if normal is not None and item.get("normalized", False):
		values = numpy.maximum(values / normal, -1.0)
	return values


def quaternion_matrix(q):
	x, y, z, w = q
	return numpy.array([
		[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
		[2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
		[2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def slerp(a, b, s):
	dot = numpy.dot(a, b)
	if dot < 0.0:
		b = -b
		dot = -dot
	if dot > 0.9995:
		q = a + (b - a) * s
		return q / numpy.linalg.norm(q)
	angle = math.acos(min(dot, 1.0))
	return (math.sin((1 - s) * angle) * a + math.sin(s * angle) * b) / math.sin(angle)


class clip_t:
	def __init__(self, name):
		self.name = name
		self.channels = []            # (node, path, times, values, interpolation)
		self.duration = 0.0


def load(path):
	document, binary = read_glb(path)
	nodes = document["nodes"]
	parents = [-1] * len(nodes)
	for i, node in enumerate(nodes):
		for child in node.get("children", []):
			parents[child] = i
	skin = document["skins"][0]
	joints = skin["joints"]
	clips = []
	for animation in document.get("animations", []):
		clip = clip_t(animation.get("name", "?"))
		for channel in animation["channels"]:
			sampler = animation["samplers"][channel["sampler"]]
			times = accessor(document, binary, sampler["input"])[:, 0]
			values = accessor(document, binary, sampler["output"])
			clip.channels.append((channel["target"]["node"], channel["target"]["path"], times, values, sampler.get("interpolation", "LINEAR")))
			clip.duration = max(clip.duration, float(times[-1]))
		clips.append(clip)
	return nodes, parents, joints, clips


def sample_channel(times, values, interpolation, t):
	if interpolation == "CUBICSPLINE":
		values = values[1::3]
	if t <= times[0]:
		return values[0]
	if t >= times[-1]:
		return values[-1]
	k = int(numpy.searchsorted(times, t)) - 1
	if interpolation == "STEP":
		return values[k]
	s = (t - times[k]) / (times[k + 1] - times[k])
	if values.shape[1] == 4:
		return slerp(values[k], values[k + 1], s)
	return values[k] + (values[k + 1] - values[k]) * s


def pose(nodes, parents, clip, t):
	# local rotations (quaternions) and global positions of every node
	locals_t = [numpy.array(n.get("translation", [0, 0, 0]), dtype=float) for n in nodes]
	locals_r = [numpy.array(n.get("rotation", [0, 0, 0, 1]), dtype=float) for n in nodes]
	locals_s = [numpy.array(n.get("scale", [1, 1, 1]), dtype=float) for n in nodes]
	for node, path, times, values, interpolation in clip.channels:
		value = sample_channel(times, values, interpolation, t)
		if path == "translation":
			locals_t[node] = value
		elif path == "rotation":
			locals_r[node] = value / numpy.linalg.norm(value)
		elif path == "scale":
			locals_s[node] = value
	world = [None] * len(nodes)

	def get(i):
		if world[i] is None:
			m = numpy.eye(4)
			m[:3, :3] = quaternion_matrix(locals_r[i]) * locals_s[i]
			m[:3, 3] = locals_t[i]
			world[i] = m if parents[i] < 0 else get(parents[i]) @ m
		return world[i]

	for i in range(len(nodes)):
		get(i)
	positions = numpy.array([w[:3, 3] for w in world])
	return locals_r, positions


def frames(nodes, parents, clip):
	count = max(2, int(round(clip.duration * FRAMES_PER_SECOND)) + 1)
	rotations = []
	positions = []
	for k in range(count):
		r, p = pose(nodes, parents, clip, min(k / FRAMES_PER_SECOND, clip.duration))
		rotations.append(r)
		positions.append(p)
	return numpy.array(rotations), numpy.array(positions)


def angle_between(a, b):
	dot = abs(float(numpy.dot(a, b)))
	return math.degrees(2.0 * math.acos(min(dot, 1.0)))


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("model")
	parser.add_argument("--cycle-meters", type=float, default=0.0, help="the walk's step a cycle (slip check)")
	parser.add_argument("--turn", type=float, default=15.0, help="walk_left / walk_right: degrees a cycle")
	parser.add_argument("--speed-limit", type=float, default=360.0, help="a joint faster than this (degrees a second) for one frame: a jump")
	arguments = parser.parse_args()
	nodes, parents, joints, clips = load(arguments.model)
	names = [n.get("name", str(i)) for i, n in enumerate(nodes)]
	sampled = {}
	print("%s: %d joints, %d clips" % (arguments.model, len(joints), len(clips)))

	# Jumps
	print("\njumps (a joint's rotation speed frame to frame; a spike: over 2.5x the frames either side):")
	for clip in clips:
		rotations, positions = frames(nodes, parents, clip)
		sampled[clip.name] = (rotations, positions)
		worst_speed = (0.0, "", 0)
		worst_spike = (0.0, "", 0, 0.0)
		for j in joints:
			speeds = [angle_between(rotations[k][j], rotations[k + 1][j]) * FRAMES_PER_SECOND for k in range(len(rotations) - 1)]
			for k, speed in enumerate(speeds):
				if speed > worst_speed[0]:
					worst_speed = (speed, names[j], k)
				# (a spike: much faster than the frames either side of it)
				around = 0.5 * (speeds[k - 1] + speeds[(k + 1) % len(speeds)])
				if speed > 120.0 and speed / max(around, 1.0) > worst_spike[0]:
					worst_spike = (speed / max(around, 1.0), names[j], k, speed)
		flag = "  <-- SPIKE" if worst_spike[0] > 2.5 else ""
		print("  %-20s fastest %6.0f deg/s (%s, frame %d); worst spike %4.1fx its neighbours (%s, frame %d, %.0f deg/s)%s" % (clip.name, worst_speed[0],
			worst_speed[1], worst_speed[2], worst_spike[0], worst_spike[1], worst_spike[2], worst_spike[3], flag))

	# Seams (the clips looped by name: walk*, run, idle*, turn_*, sleep_idle)
	print("\nseams (loops: end pose against start; a joint's speed change across the seam against its biggest inside):")
	for clip in clips:
		if not (clip.name.startswith(("walk", "run", "idle", "turn_", "sleep_idle", "carry_walk", "carry_idle"))):
			continue
		rotations, positions = sampled[clip.name]
		gap = max(angle_between(rotations[0][j], rotations[-1][j]) for j in joints)
		worst = (0.0, "", 0.0)
		for j in joints:
			speeds = [angle_between(rotations[k][j], rotations[k + 1][j]) for k in range(len(rotations) - 1)]
			# (the last frame is the first: the step before it against the one after)
			seam = abs(speeds[-1] - speeds[0])
			inside = max([abs(speeds[k + 1] - speeds[k]) for k in range(len(speeds) - 1)] or [0.0])
			if seam - inside > worst[0] - worst[2] or worst[1] == "":
				worst = (seam, names[j], inside)
		print("  %-20s end vs start %5.2f deg; worst speed change across the seam %5.2f deg/frame (%s; inside that joint at most %5.2f)%s" % (clip.name,
			gap, worst[0], worst[1], worst[2], "  <-- SEAM" if gap > 1.0 or worst[0] > 1.5 * worst[2] + 0.5 else ""))

	# Matches between the walks
	legs = [j for j in joints if any(w in names[j].lower() for w in LEG_WORDS)]
	root = joints[0]
	walks = [c for c in ("walk", "walk_left", "walk_right") if c in sampled]

	def features(name):
		rotations, positions = sampled[name]
		relative = positions[:, legs, :] - positions[:, root:root + 1, :]
		count = len(relative) - 1        # the last frame is the first again
		relative = relative[:count]
		velocity = numpy.roll(relative, -1, axis=0) - numpy.roll(relative, 1, axis=0)
		return relative, velocity * 0.5 * FRAMES_PER_SECOND

	if len(walks) > 1 and legs:
		print("\nmatches (legs: %s); cm = mean joint distance, the velocities weighed in at 0.1 s:" % ", ".join(names[j] for j in legs))
		for a in walks:
			for b in walks:
				if a == b:
					continue
				pa, va = features(a)
				pb, vb = features(b)
				best = []
				same = []
				offsets = []
				for k in range(len(pa)):
					costs = [numpy.mean(numpy.linalg.norm(pa[k] - pb[m], axis=1)) + 0.1 * numpy.mean(numpy.linalg.norm(va[k] - vb[m], axis=1))
						for m in range(len(pb))]
					m = int(numpy.argmin(costs))
					best.append(costs[m])
					offsets.append((m * len(pa) - k * len(pb)) / max(len(pb), 1))
					same_m = int(round(k * len(pb) / len(pa))) % len(pb)
					same.append(costs[same_m])
				print("  %-10s -> %-10s best %5.1f cm mean, %5.1f worst; same frame %5.1f mean, %5.1f worst; frame offset %+.1f..%+.1f" % (a, b,
					100 * numpy.mean(best), 100 * max(best), 100 * numpy.mean(same), 100 * max(same), min(offsets), max(offsets)))

	# Slip
	if arguments.cycle_meters > 0.0:
		feet = [j for j in joints if any(w in names[j].lower() for w in FOOT_WORDS)]
		# (the lowest of each leg's: its toe if it has one)
		toes = [j for j in feet if "toe" in names[j].lower()] or feet
		print("\nslip (a foot within 5 mm of its lowest, the engine moving the model; worst over the clip, mm):")
		for name in walks:
			rotations, positions = sampled[name]
			count = len(positions)
			duration = (count - 1) / FRAMES_PER_SECOND
			results = []
			for sign in ((0.0,) if name == "walk" else (1.0, -1.0)):
				worst = 0.0
				turn_cycle = math.radians(arguments.turn) * sign
				for j in toes:
					heights = positions[:, j, 1]
					low = heights.min() + 0.005
					world = []
					for k in range(count):
						s = k / (count - 1)
						yaw = turn_cycle * s
						if abs(turn_cycle) > 1e-6:
							radius = arguments.cycle_meters / turn_cycle
							origin = numpy.array([radius * (1 - math.cos(yaw)), 0.0, radius * math.sin(yaw)])
						else:
							origin = numpy.array([0.0, 0.0, arguments.cycle_meters * s])
						c = math.cos(yaw)
						n = math.sin(yaw)
						x, y, z = positions[k, j]
						world.append(origin + numpy.array([x * c + z * n, y, -x * n + z * c]))
					world = numpy.array(world)
					k = 0
					while k < count:
						if heights[k] <= low:
							start = k
							while k < count and heights[k] <= low:
								k += 1
							drift = numpy.linalg.norm((world[start:k] - world[start])[:, [0, 2]], axis=1).max()
							worst = max(worst, drift)
						else:
							k += 1
				results.append((worst, sign))
			best = min(results)
			side = "" if name == "walk" else (" (turning %s)" % ("toward +X" if best[1] > 0 else "toward -X"))
			print("  %-10s %5.1f mm%s" % (name, best[0] * 1000.0, side))


main()
