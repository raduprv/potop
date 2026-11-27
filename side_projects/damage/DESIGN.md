# Damage: a destruction model for Potop's buildings

A side project, on its own for now: nothing here is built by the
project's Makefile and nothing of the engine is linked. What follows is
what Radu and Claude agreed on (Nov 27 2026) before any of the model was
written. Decisions, not code: check the sources for what exists.

## The tools

- `soft_render.c`: a crude software rasterizer (flat triangles, depth
  buffer, a fixed light, boxes, cylinders, prisms, spheres, lines, points,
  a 5 x 7 font). It exists so Claude can look at geometry and physics
  without lavapipe, and so Radu sees the very same pixels live.
- `damage_scene.h`: what a scene is (open, seek to a frame, draw, a line
  about the frame, a checksum, a ray cast). The scenes: `pile`
  `support` and `cabin` (model_scenes.c, the damage model: the three
  cases of the support pass; a log cabin and a mortared wall), `pile` (pile_scene.c, Box3D alone, with a film of its own),
  `sandbox` (the pile's wall and crib with nothing happening by itself)
  and `test` (test_scene.c, the renderer's own test).
- `damage_model.c`: the model itself (below), on Box3D; `damage.ini`: its
  numbers, read by the cabin scene at every start (R in the viewer).
  A saved replay gives the same checksum only with the same ini.
- `damage_events.h`: the balls thrown and blasts set off from the viewer
  (left mouse, X; Backspace takes the last one back), each noted with its
  frame. A scene sets them off again when it comes to that frame, so
  going back replays them; P saves them to a .txt beside the picture and
  `damage_shot --events` draws the very same wreck (checked: the same
  checksum).
- `damage_shot`: headless, a frame or a sheet of frames to a PNG;
  `--debug` adds contacts and impulses.
- `damage_view`: the same scenes in an SDL3 window: free camera, pause, a
  frame at a time forward and back, slow motion. P saves the picture and
  prints the `damage_shot` command that draws the same one, with the
  scene's checksum; C prints the camera.

Build: `make` here (SDL3 through pkg-config, as the project's Makefile;
Box3D is compiled from third_party/box3d into build/); `make damage_shot`
needs no SDL3.

## What step 2 found (Box3D commit of Oct 7 2026, one thread)

- **The same steps give the same world**: a frame's checksum is the same
  run after run and after going back and forward again (seek steps from
  the start). Going back is therefore free of any recording.
- **A log's shape**: Box3D has no cylinder (spheres, capsules, convex
  hulls, meshes). A log is a hull, a prism of 12 sides lying on a flat
  one; the renderer draws that very prism (soft_render_prism). Stacked
  crosswise with nothing holding them, such logs stand and fall asleep.
- **Hulls are copied**: the world keeps its own copy of a hull given to a
  shape (shared by hash), so the caller's can be destroyed at once.
- **A hit** is read two ways. Hit events (shape definition
  enableHitEvents, world hitEventThreshold in m/s) give the two shapes,
  the point, the normal and the approach speed, but no impulse. Contact
  data (b3Body_GetContactData) gives, a contact point, totalNormalImpulse
  in newton seconds (the steel ball on the brick wall: about 1900).
  A shape's userData holds its piece's number.
- **Many shapes on one body are cheap**: 240 logs as one body cost 0.012
  ms a step lying still, against 0.059 ms as 240 bodies; made in 6 ms.
- **Taking a body apart in mid motion** (split_body in pile_scene.c): a
  new body a piece, each moving as that part of the old one did (its
  speed plus what the turning adds that far from the center), the old
  body destroyed. The common center goes on along the same parabola to
  the last digit. The parts of a turning body then drift apart, as they
  should: nothing pulls them round any more. 240 pieces: 4 ms, and 1.2
  ms for the first step after.
- **b3World_Explode** exists: an impulse per area on what faces it,
  within a radius and a falloff. Enough for a first explosion.

## What step 3 made and found

damage_model.c has the graph, the groups and the first damage: one
strength a link (a threshold and hit points), hits read from Box3D's hit
events with the contact's impulse, blasts of its own (Box3D's explosion
does nothing to a static body), small loose groups as rubble.

- **The rule of a hit**: the hit piece shares the impulse evenly among
  its whole links; each link passes hop_falloff of what it got to the
  piece at its other end, which shares it again. A link is hurt once a
  hit. With six links a brick the damage stays close to the hit; the
  rest is done by what came loose hitting what still stands.
- **A hit is worked out by Box3D before the damage is known**: the ball
  bounces off a wall that was still whole. So a body whose hit broke
  links gets back the speed it had before the step, times
  breakthrough_speed_kept, and meets the loosened pieces in the next
  step (which then take their push from a real collision).
- **Pieces need play** (shape_gap, 3 mm): a brick freed among bricks
  still standing, with shapes that touch exactly, is held like a drawer
  gone askew whatever pushes it. Collision shapes are a little smaller
  than the pieces drawn.
- **Only hits hurt** (contacts faster than hit_speed): a slow push,
  however strong, breaks nothing. A ball coming to rest in a seam wedged
  two loose bricks apart with some 1500 N s a step against their standing
  neighbours and nothing gave. To be decided: whether a contact's impulse
  alone, over the threshold, should count (the weight of a heap on a
  beam would then break it, which was left out on purpose).
- **Nothing held anything up yet** in step 3, as planned: knock the
  bricks out from under a wall and the rest hung there.
- The cabin and wall (79 pieces, 242 links): 0.03 ms a step through a
  scene of balls and a blast; the same checksum straight, after going
  back, and replayed from an events file.

## What step 4 made and found

The support pass (work_out_support in damage_model.c), run when the
graph changed, on what still stands. It was written twice.

**The first one went piece by piece** and is gone: each piece sent its
weight down the links towards the ground, shared by leverage rules (one
link: a cantilever; links on a line: a seesaw; links round the load:
balanced both ways), with a made up "distance to the ground" deciding
which links those were. It was right for walls, stacks and beams on
pillars, where the weight has one way down, and wrong for frames: a
roof on a wall and ONE post (its other post knocked out) it took for a
lintel balancing on one post, broke that link, then the wall's. Nothing
fell, because Box3D found the loose pieces stable where they lay, as
they are; but nothing was nailed any more. Box3D had it right because
it balances everything at once, which the pass did not.

**The one there now solves the building at once**: the pieces stay
rigid; every link is a short stiff spring against its two pieces moving
apart or turning apart where it is; gravity pulls on every piece; where
it all settles (six unknowns a piece, conjugate gradients, started from
where it settled last time) gives the force and the moment through
every link. The weight finds all the ways down there are, each as far
as it is stiff. No rules about who rests on whom are left.

- **A link's limits**: compression and pull along its direction (worked
  out from where it is when it is added: up when it is under one piece
  and over the other, else level), pull also for what it bears
  sideways, until sliding has limits of its own; cantilever for its
  moment. **A material's**: crush, against half of all that presses on
  a piece. A crushed piece lets go of everything and is gone from the
  world (left as a solid box it went on propping the wall up).
- **joint_arm** (damage.ini) is the one new number: how stiff a link is
  against turning, as a length. It decides how much goes through joints
  as cantilevers and how much round by other ways.
- **Giving way takes time**: an overloaded link loses hit points as fast
  as it is overloaded (collapse_seconds at just its limit), so what hits
  have hurt goes sooner and a building stands a moment first. A link
  breaking makes the pass due again at the next step: a collapse goes
  on frame by frame.
- **The cases** (the `support` scene; support_show.txt). The wall on a
  pane: untouched the pane has about half of the wall (it is the
  stiffest way down); with a pier gone the wall balances on it, it has
  all 13 kN, is crushed, and the wall falls. The roof: with one post
  gone it stands and every link is whole; with both gone the beams
  hold it from the wall alone, 17 links are overloaded, and it comes
  down, wall and all. The beam on three pillars with the pillar under
  the crate gone: the far pillar's link is pulled up (1.3 times its
  limit) and goes, then the middle one's (4 times) and the beam tips.
  The cabin and its wall carry themselves with no link over a quarter
  of its limit.
- **Cost**: the cabin from nothing 0.34 ms (41 rounds); through its
  replay 0.06 ms a step, the slowest step 0.8 ms. It grows with pieces
  times rounds.
- Open, as before: only hits hurt links, slow pushes do not; and what
  lies on a building weighs nothing in the pass (only linked pieces
  are in it).

## What step 5 made and found

- A piece has hit points of its own (per material, in the ini, with a
  threshold below which a hit does nothing); out of them it is removed.
- A piece is long when its longest side is `long_ratio` times the next.
  Only long pieces bend; their long axis is found once.
- Standing still: after the support pass, the bending moment along the
  piece is worked out from the forces of its links and its own weight,
  and set against what its section bears (`bend` of the material times
  thickness squared times width). Over that, the piece is cut straight
  across where the moment is largest. The inspector shows it as a
  percentage ("bent").
- Hit: the impulse of a hit bends the piece between the nearest links on
  either side that hold it up (a link by which something lies on it or
  hangs from it does not count); over the material's `snap`, it is cut
  at the hit. A hit past the last such link is a plank struck on its
  free end.
- A cut makes two pieces out of one (no stub shorter than
  `cut_min_length`; and `cut_clearance` past the edge of any piece
  linked to it, or the loose part is left lying on a sliver of its
  support), each link goes to the half it is on, and the group
  is remade. The cut is flat, as by a laser, as agreed.
- The bend scene: a plank under a heavy crate with a prop in the middle
  (take the prop out and the plank breaks under the crate by itself), a
  plank with a light crate (a ball breaks it), a log (a ball knocks it
  off whole, a fast one breaks it).
- The brick pillars are long pieces too, so a ball snaps them. Seems
  right; `long_ratio` and brick's `snap` decide.

Known and left so:

- A piece is rigid in the support pass, so on three or more supports in
  a row it leans on all alike wherever its load is. The propped plank is
  reckoned 84% bent untouched, more than a real one would be, and a prop
  right under a load takes a third of it, not most.
- The two halves of a cut piece no longer hold each other in any way: a
  snapped prop still stands (Box3D stacks it), but the support pass no
  longer counts it as holding anything, so what leaned on it may break.

## What step 6 made and found

- A link has a limit against sliding (`slide`) apart from the one
  against being pulled apart (`pull`); for the old link types they are
  the same number.
- A new link type, `notch`: wood lying in wood with no nails (a log in
  the notch of the one it crosses, or in the groove of the one under).
  It does not hold against being lifted out, only against sliding, and
  what presses on it (from the last support pass) holds it more:
  `grip` newtons more from the side for each newton pressed (and that,
  at `joint_arm`, more turning), and `preload` newton seconds more that
  it shrugs off of a hit.
- The cabin's walls are all notches now (the roof beams are still
  nailed, the first course is on the foundation). Untouched, its worst
  link is at a third of its limit. The same ball that knocks a log out
  near the top only hurts links near the bottom.
- A blast now also breaks long pieces across (pushed where they are
  nearest the blast), and pushes the parts of what it cut. The viewer's
  two strengths are in the ini (`[blast]`), five times what they were:
  the old ones did nothing to the cabin or the wall.

Known and left so:

- A blast gives each piece its push by itself, with nothing shielding
  anything, and hurts links only as a hit on each piece does. So a wall
  of small bricks shrugs off a blast that takes the top off the cabin
  (a brick faces it with little area), then goes all at once at twice
  that: a real wall would give way to the push on the whole of it.
- A notch is as strong against sliding out along the log as across it.

## The house scene, and what it changed

- A nailed timber frame house (scene `house`, the default now): sills
  on the foundation, posts, plates, boards across the posts with a door
  and a window, five trusses (tie beam, two rafters, a collar) and
  boards across the rafters. 120 pieces, 369 links. `house_show.txt`
  throws balls at it and sets off a blast inside.
- How much bending a piece bears now depends on which way it is pushed:
  how deep it is that way, squared, times how wide. A board bears far
  more on its edge than on its face (before, its thin side was always
  taken, and the house cut itself to bits standing still).
- Wood is ten times stronger in bending than it was (`bend` in the
  ini): at the old number a 10 cm square timber bore 300 Nm, about a
  twentieth of a real one, and the frame's sills and plates were near
  their limit from the weight of the house alone. The bend scene's
  plank is 5 cm thick now instead of 16, so it breaks under the same
  crates as before.
- The support pass spreads load over every link alike, boards and all,
  so in a frame like this small loads show up in odd places (a sill
  between two foundation points was "bent" by the posts on it until it
  was held under each post). Real strengths leave room for that.

## Blasts on what stands, as one push

- A blast's pushes on the standing pieces are put on them all at once
  and the same springs solved as in the support pass: what goes through
  each link is what hurts it. Neighbors pushed alike do not strain the
  link between them; the links round the pushed part, and those that
  hold it to the ground, bear it. Long pieces are cut by the bending
  the same solve gives them, against `snap`.
- `blast_inertia`: each piece's own mass takes up part of the push on
  the way (a spring from each piece to where it stands, as stiff as its
  mass times this). At 0 everything goes down to the foundation and the
  smallest blast shears a whole building off its base; too much and
  nothing but the piece hit feels it. 0.005 is a first guess.
- `blast_shielding`: a ray from the blast to each piece's middle; if it
  meets another piece or the ground first, the piece loses that part
  of its push (0.8).
- What is already loose is hurt piece by piece, as before.
- Seen with these numbers: 3000 inside the cabin only shakes bits off,
  6000 leaves its first course; the brick wall loses an end to 6000
  and stands; 3000 inside the house blows it apart, the same outside
  the front wall snaps its boards, which stay hanging by their nails.
  All of it turns sharply on `blast_inertia` and the links' thresholds:
  to be tuned by eye.

## The model

Not a CAD grade simulation: if it shows in the game it is in, if it only
shows in a chart it is out.

- **Box3D does all that moves** (collisions, stacking, impulses, sleep).
  A group of pieces still linked together is one body with many shapes;
  the links are never Box3D joints.
- **One graph a building**: the pieces are its nodes, the links its edges,
  made once when the building is. A broken link is marked dead; nothing is
  rebuilt. A group is a connected island of that graph (a number a
  piece). The graph outlives the building: logs flying 50 m away are still
  looked up in it when they land. It is freed when no live link is left.
- **Small groups become rubble**: under a few pieces (an ini number) a
  group stays glued for good as a plain body, with no contact reports.
- **A link** has a threshold (a light wind must not wear it down), hit
  points (several events can break it) and limits by direction: pressed
  together, pulled apart, slid sideways, and as a cantilever (weight times
  arm). A log cabin's notched corner is a link type, not geometry: almost
  nothing against pulling apart, a lot against sliding and pressing.
- **A link remembers how hard it is pressed** (the static load from the
  last support pass): a pull must beat that first. The bottom log, with
  the house on it, does not jump at a slap; the top one does.
- **A piece** has a crushing limit and its own hit points (glass breaks
  itself, not the nails round it). Long pieces (told by their size, not
  by a flag) have a bending limit worked out on the piece itself: the
  moment summed along its axis through where it is held and loaded; over
  the limit it is cut there in two pieces, straight as by a laser (a
  noise cap later if it looks wrong). So the graph also gains nodes.
  Plates (floors, doors, panes) do not bend: hit points and crushing only.
- **The support pass** runs only after something broke, on the island
  still anchored to the ground: each piece's weight goes down to its
  supports by leverage (not split evenly: a beam on two pillars with a
  free end pulls one of them up), each link and piece is checked against
  its limits, and it repeats until nothing more breaks. An overloaded
  link may lose hit points over a second or two instead of at once (the
  house stands a moment, then goes).
  Its test cases: a whole house left standing on a ground floor window;
  a roof hanging from one wall; a beam on three pillars with an end
  pillar gone, pushed on its free end.
- **What lies on a building** (a fallen tree, the neighbour's rubble)
  counts only as the hit, not as weight afterwards.

Order of an event: the hit breaks what it breaks (against the old
loads); the islands are relabeled; the support pass on the anchored one;
again until quiet.

## Where damage comes from

- **Blunt force and explosions first** (the same code, two sources of
  impulse): if these do not work there is no point in the rest. A blunt
  hit comes from Box3D's contact impulses; an explosion is a radial
  impulse falling off with distance. A hit spreads through the graph,
  weaker each link away (how much weaker: an ini number).
- **Water, later**: no continuous push on the links. The first impulse
  over a threshold either breaks links or not; after that the water only
  carries what came loose (the engine's water already carries logs), and
  a house swept into something takes a blunt hit.
- **Fire, last**: lowers the links' hit points (and the pieces' mass) as
  they burn; gravity does the rest.

## Buildings

- Special buildings made physically from predefined modules (closed
  meshes, convex collision shapes, a material that gives the limits and
  the density). The existing generated houses are not used: they are not
  physical (no back faces, no anchors).
- A module is one piece or several linked ones (not decided); it is a
  convenience of building only: the graph gets separate pieces.
- Links are made procedurally by rules, neither from mere touching nor
  all by hand. How exactly: not decided.

## Steps

1. The soft renderer and its test scene. **Done.**
2. Box3D alone: a pile of boxes and logs falling, seen through it.
   **Done** (what it found is above).
3. The graph: pieces, links, islands, groups splitting, with the
   simplest damage (a ball, a blast). **Done** (what it found is above).
4. The support pass, with its three test cases. **Done** (above).
5. Bending and cutting of long pieces, piece hit points. **Done** (above).
6. The notched corner link and explosions. **Done** (above).

Radu tests each step himself before the next is started.
