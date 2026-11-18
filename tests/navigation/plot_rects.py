#!/usr/bin/env python3
# The navigation mesh's rectangles in a trace (--trace) as a picture, with
# the actor's trail and the goals: objects' tops filled (colored by height
# over the ground, labeled), the ground's outlined.
#
#   tests/navigation/plot_rects.py TRACE OUT.png X0 Z0 X1 Z1 [PIXELS_PER_METER]
#
# (Needs Pillow. tools/animal_trace.py draws trails over the obstacles.)

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trace_analysis

from PIL import Image, ImageDraw


def main():
	if len(sys.argv) < 7:
		sys.exit("tests/navigation/plot_rects.py TRACE OUT.png X0 Z0 X1 Z1 [PIXELS_PER_METER]")
	path, out = sys.argv[1], sys.argv[2]
	x0, z0, x1, z1 = map(float, sys.argv[3:7])
	scale = float(sys.argv[7]) if len(sys.argv) > 7 else 24.0
	samples, rects, goals = trace_analysis.read(path)
	width = int((x1 - x0) * scale)
	height = int((z1 - z0) * scale)
	image = Image.new("RGB", (width, height), (255, 255, 255))
	draw = ImageDraw.Draw(image)

	def at(x, z):
		return ((x - x0) * scale, height - (z - z0) * scale)

	ground = min([rect[5] for rect in rects if rect[4] == 0] or [0.0])
	for x, z, w, d, kind, top in rects:
		a = at(x, z + d)
		b = at(x + w, z)
		if kind == 1:
			t = max(0.0, min(1.0, (top - ground) / 3.0))
			draw.rectangle([a[0], a[1], b[0], b[1]], fill=(int(255 * t), int(200 * (1 - t)) + 40, 255 - int(200 * t)), outline=(0, 0, 0))
			if w * d >= 1.0:
				draw.text((a[0] + 2, a[1] + 2), "%.2f" % (top - ground), fill=(0, 0, 0))
		else:
			draw.rectangle([a[0], a[1], b[0], b[1]], outline=(180, 180, 180))
	for previous, sample in zip(samples, samples[1:]):
		draw.line([at(previous["x"], previous["z"]), at(sample["x"], sample["z"])], fill=(200, 0, 0), width=2)
	for x, z in goals:
		a = at(x, z)
		draw.ellipse([a[0] - 4, a[1] - 4, a[0] + 4, a[1] + 4], outline=(0, 120, 0), width=2)
	image.save(out)


if __name__ == "__main__":
	main()
