# Writes /home/claude/potop_fast/data/settings.ini from the project's,
# then the overrides given (section.key=value), then the arguments' own.
import re, sys
BASE = {
	'terrain.size': '512',
	'render.sky_occlusion': 'false',
	# (worked out at startup even when off: as few directions as it takes)
	'render.sky_occlusion_directions': '8',
	'render.sky_occlusion_texel_meters': '2.0',
	'render.volumetric_clouds': 'false',
	'render.water_reflections': 'false',
	'render.taa': 'false',
	'render.bloom': 'false',
	'render.auto_exposure': 'false',
	'render.day_cycle': 'false',
	'render.fog_mode': '0',
	'render.local_lights': 'false',
	'render.decals': 'false',
	'render.vsync': 'false',
	# (lavapipe is bound by pixels: a frame at 800x450 about 3 s, at half scale about 1.3 s)
	'render.render_scale': '0.5',
	'render.start_fullscreen': 'false',
	'render.start_maximized': 'false',
	'objects.grass': 'false',
	'objects.impostors': 'false',
	'simulation.pause_in_background': 'false',
}

def apply(text, key, value):
	section, name = key.split('.', 1)
	pattern = r'(?m)^' + re.escape(name) + r'\s*=.*$'
	block = re.search(r'(?ms)^\[' + re.escape(section) + r'\]\n(.*?)(?=^\[|\Z)', text)
	if block is None:
		return text + '\n[%s]\n%s = %s\n' % (section, name, value)
	body = block.group(1)
	if re.search(pattern, body):
		body = re.sub(pattern, name + ' = ' + value, body, count=1)
	else:
		body = name + ' = ' + value + '\n' + body
	return text[:block.start(1)] + body + text[block.end(1):]

text = open('/home/claude/potop/data/settings.ini').read()
items = dict(BASE)
for item in sys.argv[1:]:
	key, value = item.split('=', 1)
	items[key] = value
for key, value in items.items():
	text = apply(text, key, value)
open('/home/claude/potop_fast/data/settings.ini', 'w').write(text)
