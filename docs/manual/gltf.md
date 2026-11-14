# glTF (.gltf, .glb)

glTF 2.0 files (Blender, Sketchfab, AI model generators, most asset sites)
convert into actors or objects; what is made follows the output's
extension:

```sh
tools/potop_convert --list model.glb                          # what it holds
tools/potop_convert --height 1 --ground fox.glb data/actors/fox.pact
tools/potop_convert --ground helmet.glb data/objects/helmet.pobj
tools/potop_convert --ground helmet.glb elsewhere/helmet.e3d  # for Eternal Lands
```

* `.pact`, an actor: the first skin's joints are its bones (64 at most),
  every animation a clip (sampled at 30 a second; linear, step and cubic
  spline keys), every clip an action named after it in lower case
  (Blender's "Armature|Walk" becomes `walk`), looping if its name says so
  (walk, run, idle, swim, fly...). Where the clips have no `walk` but one
  has walk in its name, that one is also `walk`; without an `idle`, the
  first clip is also `idle`. `--action NAME=CLIP[:loop|:once][@SECONDS]`
  (once or more) sets the actions instead; CLIP is a clip's name or
  number. A model without a skin becomes an actor of one still bone.
* `.pobj`, an object, the game's own (see `engine/object_format.h`): the
  meshes as they stand at rest, with tangents, one material per glTF
  material (16 at most), each with its base color, normal map,
  occlusion-roughness-metal and emissive, all in the one file. Give it a
  section in `objects.ini` as any object: `model = helmet` finds
  `objects/helmet.pobj` before `objects/helmet.e3d`.
* `.e3d`, an Eternal Lands object: the same meshes, the base color alone,
  each texture written next to it as a PNG (`helmet.png`, or
  `helmet_0.png`, `helmet_1.png`...). The game reads these too.

Each material's maps come out the size of its base color texture, its
factors already in (textures PNG or JPEG, inside the file, next to it, or
as a data URI): the base color (a material without a texture becomes a
small square of its color), the normal map (its scale in), occlusion,
roughness and metal in one texture (R, G, B, as glTF packs them; the
occlusion strength in) and the emissive color, where there is one. Actors
(`.pact` version 2) carry the same maps packed like their base color, and
their tangents.

Objects and actors are shaded physically (Graphics → PBR shading,
`pbr_shading`, on unless set): the normal map bends the normal in each
vertex's tangent frame, roughness and metal shape the light (Lambert for
the diffuse part, GGX with Smith's shadowing and Schlick's Fresnel for the
sun's reflection, the sky and the ground around reflected through Karis'
analytic fit of the split sum), occlusion darkens what the surroundings
light, and the emissive map adds the surface's own light. Models without
those maps (every `.e3d`, `.pact` version 1) get neutral ones: a flat
normal, not metal, no light of their own and no reflection at all (the
neutral map's alpha says so: their textures were painted for diffuse light,
and a sheen of the sky turns dark needles gray blue), and tangents made
from their texture coordinates; they look exactly as they did. Such
materials skip the physically based shading altogether (the maps are not
read, the light is the plain diffuse one), so they cost nothing more with
PBR shading on; only materials with maps of their own (`.pobj`, `.pact`
version 2 with maps) pay for it.
Off, everything is plain diffuse, as before. Opaque materials drop their
alpha; cut out (mask) ones keep it at their cutoff, and blended ones at a
half (the game draws nothing half transparent). An actor has one texture:
several are packed side by side, each with its edges repeated around it
(texture coordinates outside 0..1 that repeat a texture are clamped then,
with a warning). Normals the file lacks are made smooth from the
triangles; more than four influences per vertex keep the four heaviest.

glTF has Y up, meters and +Z forward, as the game, so nothing turns unless
asked. Joints scaled in the file (an armature at 0.01, as Mixamo's come)
are fine: each joint's world matrix is split into a rotation and a
translation for the game's bones, and its scale goes into the inverse bind
pose.

| Option | What it does |
|---|---|
| `--scale S` | sizes times S |
| `--height M` | scaled to stand M meters tall at rest |
| `--up z` | the model has Z up |
| `--turn DEGREES` | turned around the vertical (actors face +Z) |
| `--ground` | its bottom at 0, its middle over the origin |
| `--in-place` | the root bones do not travel forward or sideways (walks that move the model) |
| `--max-texture N` | textures halved until no side is above N (2048) |
| `--no-collide A,B` | `.pobj`: these materials (names as `--list` shows them) are passable: drawn, not in the way (branches); cut out ones never are |

Checked on Khronos' samples: Fox (skinned, three clips), CesiumMan,
RiggedFigure, DamagedHelmet and BoxTextured.

Actors are skinned on the GPU: each bone's matrix (pose times inverse bind
pose) goes to the vertex shader as three rows of a 3x4 matrix, up to 64
bones, and each vertex blends four of them (`actor_skinning.glsl`). They
use the objects' fragment shader, so the light, shadows, fog and the view
under water are the same.
