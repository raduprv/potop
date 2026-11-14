#!/bin/bash
# Finds the faces of a house that show their back from the street: draws
# eight views from eye height round it with and without back face culling
# (house_view --no-cull) and counts the pixels that differ. A face seen
# only without culling is one the game would leave a hole at (it draws
# nothing two sided). Run from the repository's root:
#   tools/house_leaks.sh PRESET [potop_house arguments...]
# Pictures land in /tmp/house_leaks/, a PRESET_N_diff.png for each view
# with more than 200 differing pixels (culled, unculled, the difference).
PRESET=$1; shift
OUT=/tmp/house_leaks
mkdir -p $OUT
tools/potop_house --preset $PRESET "$@" $OUT/house.pobj >/dev/null || exit 1
python3 - "$PRESET" "$@" <<'PY'
import math, struct, subprocess, sys
from PIL import Image, ImageChops
preset = sys.argv[1]
extra = sys.argv[2:]
out = '/tmp/house_leaks'
header = struct.unpack_from('<4sIIIIII6f', open(out + '/house.pobj', 'rb').read(), 0)
low = header[7:10]
high = header[10:13]
center_x = (low[0] + high[0]) / 2
center_z = (low[2] + high[2]) / 2
radius_x = (high[0] - low[0]) / 2
radius_z = (high[2] - low[2]) / 2
top = high[1]
total = 0
for k in range(8):
    angle = k * math.pi / 4
    eye = '%f,1.6,%f' % (center_x + math.cos(angle) * (radius_x + 3.0), center_z + math.sin(angle) * (radius_z + 3.0))
    look = '%f,%f,%f' % (center_x + math.cos(angle) * radius_x * 0.8, top * 0.75, center_z + math.sin(angle) * radius_z * 0.8)
    pictures = []
    for mode in ('cull', 'nocull'):
        prefix = '%s/%s_%d_%s' % (out, preset, k, mode)
        command = ['tools/house_view', '--preset', preset] + extra + ['--views', 'front', '--eye', eye, '--look', look, '--sun', '50,35',
            '--size', '600x400', '--out', prefix]
        if mode == 'nocull':
            command.append('--no-cull')
        subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pictures.append(Image.open(prefix + '_front.png').convert('RGB'))
    difference = ImageChops.difference(pictures[0], pictures[1]).convert('L').point(lambda value: 255 if value > 40 else 0)
    count = sum(1 for value in difference.getdata() if value)
    total += count
    print('%s view %d: %d differing pixels' % (preset, k, count))
    if count > 200:
        sheet = Image.new('RGB', (1800, 400))
        sheet.paste(pictures[0], (0, 0))
        sheet.paste(pictures[1], (600, 0))
        sheet.paste(difference.convert('RGB'), (1200, 0))
        sheet.save('%s/%s_%d_diff.png' % (out, preset, k))
print('total %d' % total)
PY
