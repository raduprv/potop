# Potop - build for desktop Linux (and other Unix-likes with pkg-config).
#
#   make              optimized build with debug info
#   make DEBUG=1      no optimization, address/undefined sanitizers
#   make OPENMP=0     without OpenMP (terrain generation runs single threaded)
#   make PROFILE=1    the optimized build, with frame pointers kept: perf's
#                     call chains whole without --call-graph dwarf. After a
#                     make clean (the objects are the same files)
#   make profile      the running game recorded with perf for
#                     PROFILE_SECONDS (30), then where its time went, the
#                     first PROFILE_LINES (120) lines; the record is
#                     build/perf.data (perf report -i build/perf.data to
#                     walk it). Start the game first, load the map, then
#                     run this from another terminal. perf may need
#                     sudo sysctl kernel.perf_event_paranoid=1
#   make tools        the desktop tools (tools/potop_convert: actors to .pact, glTF to .pact or .e3d)
#
# SDL3 is found through pkg-config. For a locally installed SDL3, set the
# variable in the environment (not as a make argument, which $(shell) would
# not see):
#   PKG_CONFIG_PATH=/path/to/sdl3/lib/pkgconfig make
# The SDL library directory is baked into the executable (rpath), so it runs
# without LD_LIBRARY_PATH.

CC ?= cc
PKG_CONFIG ?= pkg-config
OPENMP ?= 1
DEBUG ?= 0
PROFILE ?= 0
PROFILE_SECONDS ?= 30
PROFILE_LINES ?= 120

# The engine is a library (build/libpotop.a, engine/); the game (game/:
# potop) and the editor (editor/: potop_editor) link it
TARGET = potop
EDITOR = potop_editor
ENGINE_LIBRARY = build/libpotop.a
ENGINE_SOURCES = $(wildcard engine/*.c)
EDITOR_SOURCES = $(wildcard editor/*.c)
# The game (through potop.h alone)
GAME_SOURCES = $(wildcard game/*.c)
# Box3D (physics), third party C17, built with its own flags below
BOX3D_SOURCES = $(wildcard third_party/box3d/src/*.c)
ENGINE_OBJECTS = $(ENGINE_SOURCES:engine/%.c=build/%.o) $(BOX3D_SOURCES:third_party/box3d/src/%.c=build/box3d/%.o) build/bc7enc.o
EDITOR_OBJECTS = $(EDITOR_SOURCES:editor/%.c=build/editor/%.o)
GAME_OBJECTS = $(GAME_SOURCES:game/%.c=build/game/%.o)
DEPENDENCIES = $(ENGINE_OBJECTS:.o=.d) $(EDITOR_OBJECTS:.o=.d) $(GAME_OBJECTS:.o=.d)

SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)
SDL_LIBRARY_DIRECTORY := $(shell $(PKG_CONFIG) --variable=libdir sdl3 2>/dev/null)

ifeq ($(filter clean profile,$(MAKECMDGOALS)),)
ifeq ($(SDL_LIBS),)
$(error SDL3 not found by pkg-config; see README.md, "Building")
endif
# SDL 3.2.24 to 3.2.28 break on NVIDIA once the window changes size (their
# Vulkan memory defragmentation marks moved buffers as index buffers, and
# compute passes read stale data from then on): 3.2.30 at least
ifneq ($(shell $(PKG_CONFIG) --atleast-version=3.2.30 sdl3 && echo ok),ok)
$(error SDL3 $(shell $(PKG_CONFIG) --modversion sdl3) is too old: 3.2.30 or newer is needed (3.4 recommended); see README.md, "Building")
endif
endif

CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -g -MMD -MP
CFLAGS += -Iengine -Ithird_party -Ithird_party/box3d/include $(SDL_CFLAGS)
LDLIBS += $(SDL_LIBS) -lm -lpthread
LDFLAGS += -Wl,-rpath,$(SDL_LIBRARY_DIRECTORY)

# x86-64 CPUs since 2012 convert 16 bit floats in hardware (F16C); with it
# the water fluxes are stored at half size. Other targets fall back to 32 bit.
ifneq ($(findstring x86_64,$(shell $(CC) -dumpmachine)),)
	CFLAGS += -mf16c
endif

ifeq ($(DEBUG),1)
	CFLAGS += -O0 -fsanitize=address,undefined
	LDFLAGS += -fsanitize=address,undefined
else
	# -fno-trapping-math changes no result: it only lets the compiler compute
	# both sides of a choice, which the water's vector loops need
	CFLAGS += -O3 -fno-trapping-math
endif

# For perf: every function keeps its frame pointer (the call chain is read
# from them, cheaply and whole), and a call at a function's end stays a
# call (its caller stays in the chain). A percent or two slower
ifeq ($(PROFILE),1)
	CFLAGS += -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls
endif

ifeq ($(OPENMP),1)
	CFLAGS += -fopenmp
	LDFLAGS += -fopenmp
else
	CFLAGS += -Wno-unknown-pragmas
endif

# Shaders: GLSL in data/shaders, compiled to SPIR-V in data/shaders/spirv
# (kept in the repository, so building the game needs no shader compiler;
# changing a shader needs glslangValidator, from glslang)
GLSLANG ?= glslangValidator
SHADER_SOURCES = $(wildcard data/shaders/*.vert data/shaders/*.frag data/shaders/*.comp)
SHADER_INCLUDES = $(wildcard data/shaders/*.glsl)
SHADER_BINARIES = $(SHADER_SOURCES:data/shaders/%=data/shaders/spirv/%.spv)
HAVE_GLSLANG := $(shell command -v $(GLSLANG) 2>/dev/null)

ifneq ($(HAVE_GLSLANG),)
all: shaders
endif
all: $(TARGET) $(EDITOR)

# The compute shaders with subgroup variants (Vulkan 1.1: SPIR-V 1.3;
# loaded only where the device has it, gpu.h)
SUBGROUP_SHADERS = grass.comp grass_map.comp objects_cull.comp
SUBGROUP_BINARIES = $(SUBGROUP_SHADERS:%=data/shaders/spirv/%.subgroups.spv)

shaders: $(SHADER_BINARIES) $(SUBGROUP_BINARIES)

data/shaders/spirv/%.vert.spv: data/shaders/%.vert $(SHADER_INCLUDES) | data/shaders/spirv
	$(GLSLANG) -V --quiet -DVERTEX_SHADER -Idata/shaders -o $@ $<

data/shaders/spirv/%.frag.spv: data/shaders/%.frag $(SHADER_INCLUDES) | data/shaders/spirv
	$(GLSLANG) -V --quiet -DFRAGMENT_SHADER -Idata/shaders -o $@ $<

data/shaders/spirv/%.comp.spv: data/shaders/%.comp $(SHADER_INCLUDES) | data/shaders/spirv
	$(GLSLANG) -V --quiet -DCOMPUTE_SHADER -Idata/shaders -o $@ $<

data/shaders/spirv/%.comp.subgroups.spv: data/shaders/%.comp $(SHADER_INCLUDES) | data/shaders/spirv
	$(GLSLANG) -V --quiet --target-env vulkan1.1 -DCOMPUTE_SHADER -DSUBGROUPS -Idata/shaders -o $@ $<

data/shaders/spirv:
	mkdir -p data/shaders/spirv

# (the BC7 encoder: the texture cache, engine/texture_cache.c)
$(ENGINE_LIBRARY): $(ENGINE_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

$(TARGET): $(GAME_OBJECTS) $(ENGINE_LIBRARY)
	$(CC) $(LDFLAGS) -o $@ $(GAME_OBJECTS) $(ENGINE_LIBRARY) $(LDLIBS)

$(EDITOR): $(EDITOR_OBJECTS) $(ENGINE_LIBRARY)
	$(CC) $(LDFLAGS) -o $@ $(EDITOR_OBJECTS) $(ENGINE_LIBRARY) $(LDLIBS)

build/%.o: engine/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/editor/%.o: editor/%.c | build
	@mkdir -p build/editor
	$(CC) $(CFLAGS) -c -o $@ $<

build/game/%.o: game/%.c | build
	@mkdir -p build/game
	$(CC) $(CFLAGS) -c -o $@ $<

# Box3D wants C17; its warnings are not ours to fix
build/box3d/%.o: third_party/box3d/src/%.c | build
	@mkdir -p build/box3d
	$(CC) $(filter-out -std=c11 -fopenmp,$(CFLAGS)) -std=c17 -w -Ithird_party/box3d/src -c -o $@ $<

# Third party code is not ours to clean up warnings in
build/stb_implementation.o: CFLAGS += -w
build/nuklear_implementation.o: CFLAGS += -w
build/miniz_implementation.o: CFLAGS += -w

# Desktop tools: not part of the game, never shipped with it
TOOLS = tools/potop_convert tools/potop_plant tools/plant_editor tools/potop_prop tools/prop_editor tools/house_editor tools/cloud_bake tools/potop_texture \
	tools/potop_house tools/house_view tools/potop_module

tools: $(TOOLS)

tools/potop_convert: tools/potop_convert.c tools/convert_gltf.c tools/convert_gltf.h engine/dds.c engine/actor_format.h build/miniz_implementation.o \
		build/stb_implementation.o build/cgltf_implementation.o
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -Wno-format-truncation -Itools -o $@ tools/potop_convert.c tools/convert_gltf.c engine/dds.c \
		build/miniz_implementation.o build/stb_implementation.o build/cgltf_implementation.o $(filter-out -fopenmp,$(LDFLAGS)) $(SDL_LIBS) -lm

# The plant grower: straight into .pobj, with the shared atlases
tools/potop_plant: tools/potop_plant.c tools/plant_grow.c tools/plant_grow.h engine/object_format.h engine/plant_atlas_file.h \
		build/stb_implementation.o build/bc7enc.o
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -Wno-format-truncation -Itools -o $@ tools/potop_plant.c tools/plant_grow.c \
		build/stb_implementation.o build/bc7enc.o -lm -lpthread

# The prop maker (rocks, walls, fences): straight into .pobj
tools/potop_prop: tools/potop_prop.c tools/prop_grow.c tools/prop_grow.h engine/object_format.h build/stb_implementation.o
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -Wno-format-truncation -Itools -o $@ tools/potop_prop.c tools/prop_grow.c \
		build/stb_implementation.o -lm

# The house maker (timber framed and stone houses): straight into .pobj
HOUSE_SOURCES = tools/house_grow.c tools/house_textures.c tools/house_atlas.c tools/house_grow.h engine/object_format.h engine/module_file.c \
	engine/module_file.h engine/module_format.h
tools/potop_house: tools/potop_house.c $(HOUSE_SOURCES) build/stb_implementation.o
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -O2 -Wno-format-truncation -Itools -o $@ tools/potop_house.c tools/house_grow.c \
		tools/house_textures.c tools/house_atlas.c engine/module_file.c build/stb_implementation.o -lm

# What is in a module (.pmod)
tools/potop_module: tools/potop_module.c engine/module_file.c engine/module_file.h engine/module_format.h
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -Itools -o $@ tools/potop_module.c engine/module_file.c

# The ground's textures made from nothing (tools/potop_texture.c)
tools/potop_texture: tools/potop_texture.c build/stb_implementation.o
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -O2 -fopenmp -Wno-format-truncation -Itools -o $@ tools/potop_texture.c build/stb_implementation.o -lm

# The BC7 encoder (tools/potop_plant's atlases), third party C
build/bc7enc.o: third_party/bc7enc/bc7enc.c third_party/bc7enc/bc7enc.h | build
	$(CC) $(filter-out -fopenmp -MMD -MP -std=c11,$(CFLAGS)) -std=gnu11 -w -c -o $@ $<

# The plant editor: potop_plant in a window, the plant drawn with the
# game's shaders (so it takes some of the game's own code)
PLANT_EDITOR_GAME_OBJECTS = build/scene_ground.o build/gpu.o build/profile.o build/file_io.o build/object_model.o build/dds.o build/settings.o \
	build/ini_parser.o build/post.o build/render_target.o build/screenshot.o build/ui_backend.o build/nuklear_implementation.o \
	build/miniz_implementation.o build/stb_implementation.o build/texture_cache.o build/bc7enc.o

tools/plant_editor: tools/plant_editor.c tools/plant_preview.c tools/plant_preview.h tools/plant_grow.c tools/plant_grow.h engine/season.h \
		engine/plant_atlas_file.h $(PLANT_EDITOR_GAME_OBJECTS)
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -Wno-format-truncation -Itools -o $@ tools/plant_editor.c tools/plant_preview.c \
		tools/plant_grow.c $(PLANT_EDITOR_GAME_OBJECTS) $(filter-out -fopenmp,$(LDFLAGS)) $(SDL_LIBS) -lm -lpthread -lgomp

# The prop editor: potop_prop in a window, drawn with the game's prop
# shaders
tools/prop_editor: tools/prop_editor.c tools/prop_preview.c tools/prop_preview.h tools/prop_grow.c tools/prop_grow.h engine/season.h \
		$(PLANT_EDITOR_GAME_OBJECTS)
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -Wno-format-truncation -Itools -o $@ tools/prop_editor.c tools/prop_preview.c \
		tools/prop_grow.c $(PLANT_EDITOR_GAME_OBJECTS) $(filter-out -fopenmp,$(LDFLAGS)) $(SDL_LIBS) -lm -lpthread -lgomp

# The house viewer: potop_house's house drawn with the game's shaders into
# PNG files, headless (for a machine without a screen)
tools/house_view: tools/house_view.c tools/house_preview.c tools/house_preview.h $(HOUSE_SOURCES) engine/season.h $(PLANT_EDITOR_GAME_OBJECTS)
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -O2 -Wno-format-truncation -Itools -o $@ tools/house_view.c tools/house_preview.c tools/house_grow.c \
		tools/house_textures.c tools/house_atlas.c engine/module_file.c $(PLANT_EDITOR_GAME_OBJECTS) $(filter-out -fopenmp,$(LDFLAGS)) $(SDL_LIBS) -lm -lpthread -lgomp

# The house editor: potop_house in a window, drawn with the game's
# building shaders
tools/house_editor: tools/house_editor.c tools/house_preview.c tools/house_preview.h $(HOUSE_SOURCES) engine/season.h $(PLANT_EDITOR_GAME_OBJECTS)
	$(CC) $(filter-out -fopenmp -MMD -MP,$(CFLAGS)) -O2 -Wno-format-truncation -Itools -o $@ tools/house_editor.c tools/house_preview.c tools/house_grow.c \
		tools/house_textures.c tools/house_atlas.c engine/module_file.c $(PLANT_EDITOR_GAME_OBJECTS) $(filter-out -fopenmp,$(LDFLAGS)) $(SDL_LIBS) -lm -lpthread -lgomp

# cgltf (third party, MIT): for the tools only
build/cgltf_implementation.o: tools/cgltf_implementation.c third_party/cgltf.h | build
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -w -c -o $@ $<

tools/cloud_bake: tools/cloud_bake.c tools/vdb_read.c tools/vdb_read.h data/shaders/spirv/cloud_sim.comp.spv build/miniz_implementation.o
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -Itools -o $@ tools/cloud_bake.c tools/vdb_read.c build/miniz_implementation.o -fopenmp $(SDL_LIBS) -lm

# The running game's time, by perf (see the top of this file)
.PHONY: profile
profile:
	@mkdir -p build
	perf record -g -o build/perf.data -p $$(pidof $(TARGET)) -- sleep $(PROFILE_SECONDS)
	perf report -i build/perf.data --no-children --stdio 2>/dev/null | grep -v '^$$' | head -$(PROFILE_LINES)

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET) $(EDITOR) $(TOOLS)

-include $(DEPENDENCIES)

.PHONY: all clean tools shaders
