# Builds /home/claude/potop_fast: a run root for my own headless tests.
# Its data/ mirrors the project's: directories real, files linked, but
# every picture shrunk to 256 px (atlases 512), its own settings.ini and an
# empty cache of its own. Nothing here is ever zipped or sent to Radu.
import os, sys
from multiprocessing import Pool
from PIL import Image

SOURCE = '/home/claude/potop/data'
ROOT = '/home/claude/potop_fast'
TARGET = ROOT + '/data'
PICTURES = ('.png', '.jpg', '.jpeg', '.tga', '.bmp')

def shrink(job):
	source, target = job
	if os.path.exists(target) and os.path.getmtime(target) >= os.path.getmtime(source):
		return 0
	try:
		image = Image.open(source)
		image.load()
	except Exception as error:
		os.symlink(source, target)
		return 0
	name = os.path.basename(source)
	largest = 512 if ('details' in name or 'atlas' in name or 'decals' in name) else 256
	width, height = image.size
	if max(width, height) > largest:
		scale = largest / max(width, height)
		size = (max(1, round(width * scale)), max(1, round(height * scale)))
		if image.mode in ('I;16', 'I', 'F'):
			image = image.resize(size, Image.BILINEAR)
		else:
			image = image.resize(size, Image.LANCZOS)
	if os.path.lexists(target):
		os.remove(target)
	image.save(target)
	return 1

def main():
	jobs = []
	for directory, folders, files in os.walk(SOURCE):
		relative = os.path.relpath(directory, SOURCE)
		if relative == 'cache' or relative.startswith('cache' + os.sep):
			continue
		os.makedirs(os.path.join(TARGET, relative), exist_ok=True)
		for name in files:
			source = os.path.join(directory, name)
			target = os.path.join(TARGET, relative, name)
			if relative == '.' and name == 'settings.ini':
				continue
			if name.lower().endswith(PICTURES):
				jobs.append((source, target))
			elif not os.path.lexists(target):
				os.symlink(source, target)
	os.makedirs(TARGET + '/cache', exist_ok=True)
	with Pool(os.cpu_count()) as pool:
		done = sum(pool.map(shrink, jobs, chunksize=4))
	print('pictures', len(jobs), 'shrunk now', done)
	for name in ('potop', 'potop_editor'):
		link = os.path.join(ROOT, name)
		if not os.path.lexists(link):
			os.symlink('/home/claude/potop/' + name, link)

main()
