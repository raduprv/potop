// glTF 2.0 (.gltf with its files, or .glb) into the game's own files, for
// potop_convert (see its usage):
//
//   .pact  an animated actor (see engine/actor_format.h): the first skin's
//          joints are its bones, every animation a clip, every material's
//          base color one texture (several are packed side by side). A
//          model without a skin becomes an actor of one bone that stands
//          still.
//   .pobj  an object (see engine/object_format.h): the meshes in their rest
//          pose, a material per glTF material, with its base color, normal
//          map, occlusion-roughness-metal and emissive.
//   .e3d   an Eternal Lands object (see engine/object_model.h): the same, but
//          the base color alone, each written next to it as a PNG.
//
// Actors carry the same maps (see engine/actor_format.h).
//
// glTF has Y up, meters and +Z forward, as the game does: nothing turns
// unless asked (--up z, --turn).

#ifndef CONVERT_GLTF_H
#define CONVERT_GLTF_H

#include <stdbool.h>

// arguments: what follows "potop_convert" (the options, then the model and
// the output). Returns the process' exit code
int convert_gltf(int argument_count, char **arguments);

// True if the path names a glTF file (by its extension)
bool is_gltf_path(const char *path);

#endif
