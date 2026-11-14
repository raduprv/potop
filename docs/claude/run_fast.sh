#!/bin/sh
# My fast headless runs (never Radu's settings, never his textures):
#   run_fast.sh script.txt [WxH, 800x450] [section.key=value ...]
# Runs from /home/claude/potop_fast: textures shrunk to 256 px, its own
# settings.ini (512 map, sky occlusion, clouds, fog, grass, impostors,
# reflections, TAA, bloom, lights, decals off), its own cache. Turn back on
# what the test needs with section.key=value. Rebuild the root after new
# textures: python3 /home/claude/tools_night/make_fast_root.py
# POTOP_ARGS: extra game arguments; POTOP_WRAP: e.g. "gdb -batch -ex run -ex bt --args".
script=$(readlink -f "$1")
size=${2:-800x450}
shift 2 2>/dev/null
python3 /home/claude/tools_night/fast_settings.py "$@" || exit 1
# (lavapipe maps every allocation of its own: the houses' gallery passes
# 65530 mappings, the default limit, and mmap fails inside lavapipe: a
# segfault in its queue thread. The container is reset now and then)
[ "$(cat /proc/sys/vm/max_map_count)" -lt 262144 ] && sysctl -w vm.max_map_count=262144 >/dev/null 2>&1
cd /home/claude/potop_fast || exit 1
POTOP_SCRIPT=$script VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
	timeout 1700 xvfb-run -a -s "-screen 0 1600x900x24" $POTOP_WRAP ./potop $POTOP_ARGS --width ${size%x*} --height ${size#*x}
echo "exit=$?"
