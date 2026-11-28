# WreckingBall: build things out of pieces and knock them down, on the
# engine's destruction model (engine/wreck.c) with a renderer of its own
# on SDL_GPU. Built on its own: only engine/wreck.c and Box3D come from
# the rest of the repository.
#
#   make             the program and its shaders
#   ./wrecking_ball  (see wb_main.c for its options)
#
# SDL3 is found through pkg-config, glslangValidator for the shaders.

CC ?= gcc
PKG_CONFIG ?= pkg-config
GLSLANG ?= glslangValidator

THIRD_PARTY = ../../third_party
ENGINE = ../../engine
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)
SDL_LIBRARY_DIRECTORY := $(shell $(PKG_CONFIG) --variable=libdir sdl3 2>/dev/null)
CFLAGS += -std=c11 -Wall -Wextra -O2 -g -I$(THIRD_PARTY) -I$(THIRD_PARTY)/box3d/include -I$(ENGINE) $(SDL_CFLAGS)

SOURCES = wb_main.c wb_world.c wb_render.c wb_math.c wb_ini.c
OBJECTS = $(SOURCES:%.c=build/%.o) build/wb_stb.o build/wreck.o
HEADERS = $(wildcard *.h) $(ENGINE)/wreck.h
BOX3D_SOURCES = $(wildcard $(THIRD_PARTY)/box3d/src/*.c)
BOX3D_OBJECTS = $(BOX3D_SOURCES:$(THIRD_PARTY)/box3d/src/%.c=build/box3d/%.o)
SHADERS = solid.vert solid.frag shadow.vert shadow.frag
SHADER_BINARIES = $(SHADERS:%=build/shaders/%.spv)

all: wrecking_ball $(SHADER_BINARIES)

wrecking_ball: $(OBJECTS) $(BOX3D_OBJECTS)
	$(CC) -Wl,-rpath,$(SDL_LIBRARY_DIRECTORY) -o $@ $^ $(SDL_LIBS) -lm -lpthread

build/%.o: %.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/wb_stb.o: wb_stb.c
	@mkdir -p build
	$(CC) $(CFLAGS) -w -c -o $@ $<

build/wreck.o: $(ENGINE)/wreck.c $(ENGINE)/wreck.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# Box3D wants C17; its warnings are not ours to fix
build/box3d/%.o: $(THIRD_PARTY)/box3d/src/%.c
	@mkdir -p build/box3d
	$(CC) -std=c17 -w -O2 -g -I$(THIRD_PARTY)/box3d/include -I$(THIRD_PARTY)/box3d/src -c -o $@ $<

build/shaders/%.spv: shaders/% $(wildcard shaders/*.glsl)
	@mkdir -p build/shaders
	$(GLSLANG) -V --quiet -Ishaders -o $@ $<

clean:
	rm -rf build wrecking_ball

.PHONY: all clean
