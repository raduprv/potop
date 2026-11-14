// Constants the shaders share with the C side (materials.h, terrain.h);
// terrain_renderer.c checks at compile time that they still agree.

// The material map's ground byte (material_map.h: MATERIAL_GROUND_*)
#define MATERIAL_GROUND_NO_GRASS 1
#define MATERIAL_GROUND_HIDDEN 2
#define MATERIAL_GRASS 0
#define MATERIAL_DIRT 1
#define MATERIAL_ROCK 2
#define MATERIAL_SAND 3
#define MATERIAL_CLAY 4
#define MATERIAL_SNOW 5
#define MATERIAL_SILT 6
#define MATERIAL_NEEDLES 7
#define MATERIAL_LEAVES 8
#define MATERIAL_TRAMPLED 9
#define MATERIAL_PETALS 10
#define MATERIAL_TRAMPLED_DIRT 11
#define MATERIAL_COUNT 12

#define SOIL_SAND 0
#define SOIL_EARTH 1
#define SOIL_CLAY 2
#define SOIL_ROCK 3
