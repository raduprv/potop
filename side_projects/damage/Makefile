# The damage side project: a destruction model for Potop's buildings, seen
# through a crude software renderer. On its own for now: nothing here is
# built by the project's Makefile and nothing of the engine is linked;
# only Box3D (third_party/box3d, the engine's own copy) is compiled in.
#
#   make             both tools
#   make damage_shot only the headless one (needs no SDL3)
#
# SDL3 is found through pkg-config, as in the project's own Makefile.

CC ?= gcc
PKG_CONFIG ?= pkg-config

THIRD_PARTY = ../../third_party
# -ffp-contract=off: no fused multiply-adds, so the same steps give the
# same world whatever the compiler feels like
CFLAGS += -std=c11 -Wall -Wextra -O2 -g -ffp-contract=off -I$(THIRD_PARTY) -I$(THIRD_PARTY)/box3d/include
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)
SDL_LIBRARY_DIRECTORY := $(shell $(PKG_CONFIG) --variable=libdir sdl3 2>/dev/null)

COMMON_SOURCES = soft_render.c soft_render_png.c command_line.c damage_events.c damage_scene.c test_scene.c pile_scene.c
COMMON_OBJECTS = $(COMMON_SOURCES:%.c=build/%.o)
HEADERS = $(wildcard *.h)
BOX3D_SOURCES = $(wildcard $(THIRD_PARTY)/box3d/src/*.c)
BOX3D_OBJECTS = $(BOX3D_SOURCES:$(THIRD_PARTY)/box3d/src/%.c=build/box3d/%.o)

all: damage_shot damage_view

damage_shot: build/damage_shot.o $(COMMON_OBJECTS) $(BOX3D_OBJECTS)
	$(CC) -o $@ $^ -lm -lpthread

ifeq ($(SDL_LIBS),)
damage_view:
	@echo "damage_view: SDL3 not found by pkg-config (see the project's README.md, Building); damage_shot needs none"
	@false
else
damage_view: build/damage_view.o $(COMMON_OBJECTS) $(BOX3D_OBJECTS)
	$(CC) -Wl,-rpath,$(SDL_LIBRARY_DIRECTORY) -o $@ $^ $(SDL_LIBS) -lm -lpthread
endif

build/damage_view.o: damage_view.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

build/%.o: %.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# Box3D wants C17; its warnings are not ours to fix
build/box3d/%.o: $(THIRD_PARTY)/box3d/src/%.c
	@mkdir -p build/box3d
	$(CC) -std=c17 -w -O2 -g -ffp-contract=off -I$(THIRD_PARTY)/box3d/include -I$(THIRD_PARTY)/box3d/src -c -o $@ $<

clean:
	rm -rf build damage_shot damage_view

.PHONY: all clean
