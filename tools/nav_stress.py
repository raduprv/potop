#!/usr/bin/env python3
# The navigation's stress test: a course of terraces, stairs, balustrades,
# doors and a low beam, each in a few sizes round what the agents fit
# (data/objects/nav_stress.pobj, made from boxes here), and a walk test of
# each kind of agent (a hen, a man, a bear, a cow) to every station of it,
# against what each should do.
#
#   tools/nav_stress.py build             the course's model (glTF, then
#                                         tools/potop_convert)
#   tools/nav_stress.py run [TYPE...]     the walks (all four by default),
#                                         a table of what went right
#   tools/nav_stress.py list              the stations and the expectations
#
# The game runs headless (xvfb-run, lavapipe when VK_ICD_FILENAMES is not
# set), --houses nav_stress: the course alone on flat ground. The model
# stands turned a half turn (the gallery's), so a station's local (x, z)
# is (origin x - x, origin z - z) in the world; the origin comes from the
# game's log.
#
# Expectations: "yes" there and up on it, "no" not up on it, "?" either
# (a size on the edge of a quarter meter: where the course falls on the
# grid decides). Openings narrower than an agent's width plus a quarter
# meter are on the edge: the grid sees a wall in any quarter it touches.

import base64
import json
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL = os.path.join(ROOT, "data", "objects", "nav_stress.pobj")

# The agents, as data/actors.ini has them (width 2 x radius, height, step)
AGENTS = ["hen", "farmer", "bear", "cow"]

# Stations along local x, STATION_SPACING apart; their fronts (stairs,
# doors) toward +z (the agents come from there, HOME_Z out)
STATION_SPACING = 7.0
HOME_Z = 6.0

boxes = []


def box(x0, x1, y0, y1, z0, z1):
	boxes.append((min(x0, x1), max(x0, x1), min(y0, y1), max(y0, y1), min(z0, z1), max(z0, z1)))


# A terrace: 4 x 3 m, its top at height, its front edge at z = 0; stairs
# in front of it (treads of rise each, 0.3 deep, 1.4 wide) unless no
# stairs; round its edge a balustrade (solid: balusters close together are
# a wall to the grid) of rail_height with an opening of opening wide over
# the stairs, or a single bar (posts at the corners) at rail_height
def terrace(cx, height, treads, opening, rail_height, rail_kind):
	box(cx - 2.0, cx + 2.0, 0.0, height, -3.0, 0.0)
	if treads > 0:
		rise = height / treads
		for t in range(treads - 1):
			# (tread t from the bottom: its top at rise * (t + 1))
			z_front = 0.3 * (treads - 1 - t)
			box(cx - 0.7, cx + 0.7, 0.0, rise * (t + 1), 0.0, z_front)
	if rail_kind == "solid":
		top = height + rail_height
		box(cx - 2.0, cx + 2.0, height, top, -3.0, -2.9)
		box(cx - 2.0, cx - 1.9, height, top, -3.0, 0.0)
		box(cx + 1.9, cx + 2.0, height, top, -3.0, 0.0)
		box(cx - 2.0, cx - opening * 0.5, height, top, -0.1, 0.0)
		box(cx + opening * 0.5, cx + 2.0, height, top, -0.1, 0.0)
	elif rail_kind == "bar":
		bar_bottom = height + rail_height - 0.05
		for px in (cx - 2.0, cx + 1.95):
			for pz in (-3.0, -0.05):
				box(px, px + 0.05, height, height + rail_height, pz, pz + 0.05)
		box(cx - 2.0, cx + 2.0, bar_bottom, height + rail_height, -3.0, -2.95)
		box(cx - 2.0, cx + 2.0, bar_bottom, height + rail_height, -0.05, 0.0)
		box(cx - 2.0, cx - 1.95, bar_bottom, height + rail_height, -3.0, 0.0)
		box(cx + 1.95, cx + 2.0, bar_bottom, height + rail_height, -3.0, 0.0)


# A hut: 4 x 4 m on a floor 0.25 high (a step up onto a 0.6 m porch), walls
# 0.3 thick and 2.4 high, a roof; a door in the front wall door_width wide
# and door_height high
def hut(cx, door_width, door_height):
	floor = 0.25
	wall_top = floor + 2.4
	box(cx - 2.0, cx + 2.0, 0.0, floor, -4.0, 0.6)
	box(cx - 2.0, cx + 2.0, wall_top, wall_top + 0.2, -4.0, 0.0)
	box(cx - 2.0, cx + 2.0, floor, wall_top, -4.0, -3.7)
	box(cx - 2.0, cx - 1.7, floor, wall_top, -4.0, 0.0)
	box(cx + 1.7, cx + 2.0, floor, wall_top, -4.0, 0.0)
	box(cx - 2.0, cx - door_width * 0.5, floor, wall_top, -0.3, 0.0)
	box(cx + door_width * 0.5, cx + 2.0, floor, wall_top, -0.3, 0.0)
	box(cx - door_width * 0.5, cx + door_width * 0.5, floor + door_height, wall_top, -0.3, 0.0)


# A yard on the ground: 5 x 5 m walled round (2 m high), its way in from
# the front a gap 2.5 m wide with a beam across it, its underside at
# beam_height
def yard(cx, beam_height):
	box(cx - 2.5, cx + 2.5, 0.0, 2.0, -5.0, -4.7)
	box(cx - 2.5, cx - 2.2, 0.0, 2.0, -5.0, 0.0)
	box(cx + 2.2, cx + 2.5, 0.0, 2.0, -5.0, 0.0)
	box(cx - 2.5, cx - 1.25, 0.0, 2.0, -0.3, 0.0)
	box(cx + 1.25, cx + 2.5, 0.0, 2.0, -0.3, 0.0)
	box(cx - 1.25, cx + 1.25, beam_height, beam_height + 0.3, -0.3, 0.0)


# The agents as data/actors.ini and their models have them: width in
# quarter meters (2 x radius, the nearest), height, step
AGENT_SIZES = {
	"hen": {"quarters": 1, "height": 0.5, "step": 0.2},
	"farmer": {"quarters": 2, "height": 1.85, "step": 0.4},
	"bear": {"quarters": 3, "height": 1.0, "step": 0.4},
	"cow": {"quarters": 3, "height": 1.59, "step": 0.3},
}

# Objects' tops this near the ground are the ground to the navigation
# (engine/navigation.c's MERGE_METERS)
MERGE_METERS = 0.2

OPEN = 1e9

# The stations: name, builder, the goal's local (x offset, z), the height
# up there, and what it asks of an agent: the highest step on the way, the
# narrowest opening, the least room above
STATIONS = [
	("stairs 0.15, opening 1.2", lambda cx: terrace(cx, 0.45, 3, 1.2, 0.9, "solid"), (0.0, -1.8), 0.45, 0.15, 1.2, OPEN),
	("stairs 0.15, opening 0.5", lambda cx: terrace(cx, 0.45, 3, 0.5, 0.9, "solid"), (0.0, -1.8), 0.45, 0.15, 0.5, OPEN),
	("stairs 0.15, opening 0.8", lambda cx: terrace(cx, 0.45, 3, 0.8, 0.9, "solid"), (0.0, -1.8), 0.45, 0.15, 0.8, OPEN),
	("stairs 0.225 up 0.9", lambda cx: terrace(cx, 0.9, 4, 1.2, 0.9, "solid"), (0.0, -1.8), 0.9, 0.225, 1.2, OPEN),
	("stairs 0.35 up 0.7", lambda cx: terrace(cx, 0.7, 2, 1.2, 0.9, "solid"), (0.0, -1.8), 0.7, 0.35, 1.2, OPEN),
	("stairs 0.45 up 0.9", lambda cx: terrace(cx, 0.9, 2, 1.2, 0.9, "solid"), (0.0, -1.8), 0.9, 0.45, 1.2, OPEN),
	("step 0.25, bar rail 0.9", lambda cx: terrace(cx, 0.25, 0, 0.0, 0.9, "bar"), (0.0, -1.5), 0.25, 0.25, OPEN, 0.85),
	("step 0.25, bar rail 1.3", lambda cx: terrace(cx, 0.25, 0, 0.0, 1.3, "bar"), (0.0, -1.5), 0.25, 0.25, OPEN, 1.25),
	("hut, door 1.0 x 2.0", lambda cx: hut(cx, 1.0, 2.0), (0.0, -2.0), 0.25, 0.25, 1.0, 2.0),
	("hut, door 0.5 x 2.0", lambda cx: hut(cx, 0.5, 2.0), (0.0, -2.0), 0.25, 0.25, 0.5, 2.0),
	("hut, door 1.0 x 1.4", lambda cx: hut(cx, 1.0, 1.4), (0.0, -2.0), 0.25, 0.25, 1.0, 1.4),
	("yard, beam at 1.3", lambda cx: yard(cx, 1.3), (0.0, -2.5), 0.0, 0.0, 2.5, 1.3),
	("yard, beam at 2.1", lambda cx: yard(cx, 2.1), (0.0, -2.5), 0.0, 0.0, 2.5, 2.1),
]


# What an agent should do at a station: "yes", "no", "?" (an opening on
# the edge of the grid: an agent of w quarters fits through w + 1 of them
# for sure, never through fewer than w), "limit" (a known one: a first
# tread within MERGE_METERS of the ground is ground, so the second looks a
# step of two treads)
def expected(station, agent):
	size = AGENT_SIZES[agent]
	step, opening, room = station[4], station[5], station[6]
	if room < size["height"] or step > size["step"] + 1e-6:
		return "no"
	if step > 0.0 and step <= MERGE_METERS and 2.0 * step > size["step"] + 1e-6 and station[0].startswith("stairs"):
		return "limit"
	if opening < size["quarters"] * 0.25:
		return "no"
	if opening < (size["quarters"] + 1) * 0.25:
		return "?"
	return "yes"


def station_x(index):
	return (index - (len(STATIONS) - 1) * 0.5) * STATION_SPACING


def build():
	boxes.clear()
	for index, station in enumerate(STATIONS):
		station[1](station_x(index))
	positions = []
	normals = []
	uvs = []
	indices = []
	faces = [
		((1, 0, 0), [(1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)]),
		((-1, 0, 0), [(0, 0, 1), (0, 1, 1), (0, 1, 0), (0, 0, 0)]),
		((0, 1, 0), [(0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)]),
		((0, -1, 0), [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)]),
		((0, 0, 1), [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]),
		((0, 0, -1), [(1, 0, 0), (0, 0, 0), (0, 1, 0), (1, 1, 0)]),
	]
	for x0, x1, y0, y1, z0, z1 in boxes:
		for normal, corners in faces:
			base = len(positions)
			for cx, cy, cz in corners:
				positions.append((x1 if cx else x0, y1 if cy else y0, z1 if cz else z0))
				normals.append(normal)
				uvs.append((cx if normal[0] == 0 else cz, cy if normal[1] == 0 else cz))
			# (counterclockwise seen from outside)
			a, b, c, d = base, base + 1, base + 2, base + 3
			e1 = [positions[b][i] - positions[a][i] for i in range(3)]
			e2 = [positions[c][i] - positions[a][i] for i in range(3)]
			cross = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
			if sum(cross[i] * normal[i] for i in range(3)) >= 0:
				indices += [a, b, c, a, c, d]
			else:
				indices += [a, c, b, a, d, c]
	blob = bytearray()
	views = []

	def add(data, target):
		while len(blob) % 4:
			blob.append(0)
		views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
		blob.extend(data)
		return len(views) - 1

	position_view = add(b"".join(struct.pack("<3f", *p) for p in positions), 34962)
	normal_view = add(b"".join(struct.pack("<3f", *n) for n in normals), 34962)
	uv_view = add(b"".join(struct.pack("<2f", *u) for u in uvs), 34962)
	index_view = add(b"".join(struct.pack("<I", i) for i in indices), 34963)
	low = [min(p[i] for p in positions) for i in range(3)]
	high = [max(p[i] for p in positions) for i in range(3)]
	gltf = {
		"asset": {"version": "2.0", "generator": "tools/nav_stress.py"},
		"scene": 0,
		"scenes": [{"nodes": [0]}],
		"nodes": [{"mesh": 0, "name": "nav_stress"}],
		"materials": [{"name": "stone", "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.52, 0.48, 1.0], "metallicFactor": 0.0,
			"roughnessFactor": 0.9}}],
		"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
		"accessors": [
			{"bufferView": position_view, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": low, "max": high},
			{"bufferView": normal_view, "componentType": 5126, "count": len(normals), "type": "VEC3"},
			{"bufferView": uv_view, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
			{"bufferView": index_view, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
		],
		"bufferViews": views,
		"buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()}],
	}
	gltf_path = os.path.join(ROOT, "build", "nav_stress.gltf")
	os.makedirs(os.path.dirname(gltf_path), exist_ok=True)
	with open(gltf_path, "w") as out:
		json.dump(gltf, out)
	subprocess.run([os.path.join(ROOT, "tools", "potop_convert"), gltf_path, MODEL], check=True)
	print("course: %d stations, %d boxes -> %s" % (len(STATIONS), len(boxes), MODEL))


def game_command(extra):
	environment = dict(os.environ)
	environment.setdefault("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/lvp_icd.json")
	return ["timeout", "900", "xvfb-run", "-a", os.path.join(ROOT, "potop"), "--houses", "nav_stress"] + extra, environment


def course_origin():
	command, environment = game_command(["--sim", "0.5"])
	log = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True).stdout
	found = re.search(r"Houses: 'nav_stress' at \(([-0-9.]+), ([-0-9.]+)\)", log)
	if found is None:
		sys.exit("nav_stress: the course is not in the game's log (tools/nav_stress.py build, and objects.ini's [nav_stress])")
	return float(found.group(1)), float(found.group(2))


def world(origin, x, z):
	# (the gallery's half turn)
	return origin[0] - x, origin[1] - z


def run(types):
	if not os.path.exists(MODEL):
		build()
	origin = course_origin()
	home = world(origin, station_x(0), HOME_Z)
	results = {}
	inside = {}
	boxes.clear()
	for index, station in enumerate(STATIONS):
		station[1](station_x(index))
	# (the flat ground's height: the gallery's)
	ground = 10.0
	for agent in types:
		goals = []
		for index, station in enumerate(STATIONS):
			offset = station[2]
			goals.append(world(origin, station_x(index) + offset[0], offset[1]))
			# (back out in front of it between stations)
			goals.append(world(origin, station_x(index) + offset[0], HOME_Z))
		text = ";".join("%.2f,%.2f" % goal for goal in goals)
		trace = os.path.join(ROOT, "build", "nav_stress_%s.trace" % agent)
		command, environment = game_command(["--sim", "3000", "--trace", trace, "--walk_from", "%.2f,%.2f" % home, "--walk_test", agent, text])
		log = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True).stdout
		with open(os.path.join(ROOT, "build", "nav_stress_%s.log" % agent), "w") as out:
			out.write(log)
		ends = {}
		for found in re.finditer(r"Walk test: goal (\d+) \(([-0-9.]+) ([-0-9.]+)\): (walk_test_\w+) ([-0-9.]+)\S*, at ([-0-9.]+) ([-0-9.]+), ([-0-9.]+) m up",
			log):
			ends[int(found.group(1))] = (found.group(4), float(found.group(5)), float(found.group(8)))
		results[agent] = ends
		# Its trail through the solid: a center inside a box (a wall, a
		# terrace, a step) between its bottom and top (less a step: on
		# its way up a stair its height lags the treads), how deep in
		inside[agent] = []
		step = AGENT_SIZES[agent]["step"]
		with open(trace) as trail:
			for line in trail:
				parts = line.split()
				if len(parts) < 11 or parts[0] != "a":
					continue
				x, z, y = float(parts[5]), float(parts[6]), float(parts[10])
				lx, lz = origin[0] - x, origin[1] - z
				for x0, x1, y0, y1, z0, z1 in boxes:
					depth = min(lx - x0, x1 - lx, lz - z0, z1 - lz)
					if depth > 0.05 and ground + y0 + 0.05 < y < ground + y1 - max(step, 0.05):
						inside[agent].append((depth, float(parts[1]), x, z, y))
						break
	failures = 0
	print("%-28s" % "station" + "".join("%-18s" % agent for agent in types))
	for index, station in enumerate(STATIONS):
		line = "%-28s" % station[0]
		for agent in types:
			ends = results[agent]
			expectation = expected(station, agent)
			end = ends.get(index * 2)
			home_end = ends.get(index * 2 + 1) or ends.get(1)
			if end is None or home_end is None:
				line += "%-16s" % "(no result)"
				failures += 1
				continue
			ground_height = home_end[2]
			up = end[1] < 1.0 and abs(end[2] - (ground_height + station[3])) < 0.2
			if expectation == "?":
				verdict = "ok?"
			elif expectation == "limit":
				verdict = "LIMIT"
			elif (expectation == "yes") == up:
				verdict = "ok"
			else:
				verdict = "WRONG"
				failures += 1
			line += "%-18s" % ("%s %s %s" % (verdict, "up" if up else "not", "(%s)" % expectation))
		print(line)
	# (deeper than the grid's quarter meter: through it; less: brushing it,
	# a placement on a step's edge, told but not wrong)
	for agent in types:
		if inside[agent]:
			deepest = max(inside[agent])
			if deepest[0] > 0.25:
				failures += 1
			print("%s%s inside the solid %d times (a quarter of a second each), the deepest %.2f m in at %.1f s at (%.2f, %.2f), %.2f m up" % (
				"WRONG: " if deepest[0] > 0.25 else "", agent,
				len(inside[agent]), deepest[0], deepest[1], deepest[2], deepest[3], deepest[4]))
	print("%d wrong" % failures)
	return failures


def main():
	if len(sys.argv) < 2 or sys.argv[1] not in ("build", "run", "list"):
		sys.exit("tools/nav_stress.py build | run [TYPE...] | list")
	if sys.argv[1] == "build":
		build()
	elif sys.argv[1] == "list":
		for index, station in enumerate(STATIONS):
			print("%-28s x %6.1f  %s" % (station[0], station_x(index), " ".join("%s:%s" % (agent, expected(station, agent)) for agent in AGENTS)))
	else:
		sys.exit(1 if run(sys.argv[2:] or AGENTS) > 0 else 0)


if __name__ == "__main__":
	main()
