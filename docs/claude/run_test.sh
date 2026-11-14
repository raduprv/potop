#!/bin/sh
# Runs the game headless on a small map with a console script, then puts
# settings.ini back. Usage:
#   run_test.sh script.txt [map cells, 512] [WxH, 800x450] [section.key=value ...]
# e.g. render.sky_occlusion=false render.volumetric_clouds=false
# Extra game arguments go in POTOP_ARGS (e.g. POTOP_ARGS=--houses).
script=$1
cells=${2:-512}
size=${3:-800x450}
shift 3 2>/dev/null
cd /home/claude/potop || exit 1
# (lavapipe: the houses' gallery passes the default 65530 mappings)
[ "$(cat /proc/sys/vm/max_map_count)" -lt 262144 ] && sysctl -w vm.max_map_count=262144 >/dev/null 2>&1
cp data/settings.ini /tmp/settings.ini.saved
trap 'cp /tmp/settings.ini.saved data/settings.ini' EXIT INT TERM
python3 - "$cells" "$@" <<'PY'
import sys, re
path = 'data/settings.ini'
text = open(path).read()
text = re.sub(r'(?m)^size = \d+', 'size = ' + sys.argv[1], text, count=1)
for item in sys.argv[2:]:
	key, value = item.split('=', 1)
	section, name = key.split('.', 1)
	pattern = r'(?m)^' + re.escape(name) + r' = .*$'
	block = re.search(r'(?ms)^\[' + re.escape(section) + r'\]\n(.*?)(?=^\[|\Z)', text)
	if block is None:
		text += '\n[%s]\n%s = %s\n' % (section, name, value)
		continue
	body = block.group(1)
	if re.search(pattern, body):
		body = re.sub(pattern, name + ' = ' + value, body, count=1)
	else:
		body = name + ' = ' + value + '\n' + body
	text = text[:block.start(1)] + body + text[block.end(1):]
open(path, 'w').write(text)
PY
POTOP_SCRIPT=$script VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
	timeout 1700 xvfb-run -a -s "-screen 0 1600x900x24" $POTOP_WRAP ./potop $POTOP_ARGS --width ${size%x*} --height ${size#*x}
echo "exit=$?"
