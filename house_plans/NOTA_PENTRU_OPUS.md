# Casele: ce e în zip și cum se integrează (pentru Opus, de la Fable)

Scurt, ca să nu pierzi tokeni citind cod: generatorul e separat de engine, nu atinge nimic din
`engine/`, `data/shaders/` sau `data/*.ini`. Tot ce ai de făcut e în `objects.ini` / `layouts.ini`.

## Ce conține

| Unde | Ce |
|---|---|
| `tools/house_grow.c/.h`, `tools/house_textures.c`, `tools/house_atlas.c` | generatorul (biblioteca) |
| `tools/potop_house.c` | CLI: `potop_house --preset inn`, `--all house_plans`, `--sections -`, `--atlas DIR` |
| `tools/house_view.c` | viewer headless cu shaderele jocului (object/prop/building), screenshot PNG |
| `Makefile` | doar țintele `tools/potop_house` și `tools/house_view` (adăugate la TOOLS) |
| `README.md` | secțiunea „Houses (potop_house, house_view)" |
| `house_plans/*.house` | 13 rețete (hovel, hovel_log, hovel_rubble, cottage, thatched_cottage, craftsman, town_house, tall_house, gallery_house, hip_house, inn, barn, stone_house) |
| `data/objects/<preset>.pobj`, `<preset>_1.pobj`, `<preset>_2.pobj` | casele generate, 3 seed-uri pe rețetă, **versiunea 7** |
| `data/props/textures/house_*.png` | texturile stand-in (tiling, 1024²) + atlasul de detalii `house_details_{color,normal,arm,emissive}.png` (2048²) |
| `house_plans/atlas/` | template-ul și ghidul pentru pictorul atlasului (nu-ți trebuie) |

## Formatul fișierelor (ce citești)

- `.pobj` **versiunea 7**, `OBJECT_FILE_STILL | OBJECT_FILE_WEATHERING`, tabloul
  `object_file_weathering_vertex_t` după vertecși, exact ca în `object_format.h` din 21r.
  `kind` e 0 lemn / 1 tencuială / 2 piatră / 3 acoperiș. `edge` e etichetat de generator (teșituri 1,
  fețe de grindă 0,3–0,55, borduri de 6 cm pe colțurile de zidărie/soclu, panouri 0, stuf 0).
- Materiale, în ordinea asta, maxim 7 (cele goale lipsesc): `walls`, `timber`, `roof`, `base` cu
  **`OBJECT_MATERIAL_AGED | LODS | LOD_MESHES`**; `glass` (întunericul din spatele rosturilor, fără
  texturi, base_color 0.03, fără emissive); `details` cu **`ALPHA_TESTED`** (atlasul, cu emissive pentru
  geamuri); `collision` cu **`COLLISION_ONLY`** (shell simplu, fără LOD-uri).
- Trei LOD-uri pe material (`lod_index_count[3]`), `lod_errors` în antet (~0,1 / 0,2 / 1 m).
- Texturile sunt toate `OBJECT_TEXTURE_REFERENCE` la fișiere sub `data/props/textures/`; atlasul e
  același fișier pentru toate casele, deci se încarcă o dată.
- `--format 6` scoate fișierul vechi (v6, `WEATHERED` în loc de `AGED`, fără tablou) dacă vreodată trebuie.

## objects.ini și layouts.ini

`tools/potop_house --preset NUME --sections -` tipărește secțiunile gata făcute; le-am pus pe toate în
`house_plans/objects_sections.txt`. În esență:

```
[inn]
model = inn
category = buildings
physics = static
shape = mesh
footprint_meters = 11.2      ; raza în care nu crește nimic solid (din bounds)
front_degrees = 0            ; ușa e pe +Z
random_yaw = false
scale = 1, 1
shadow = all
light = 1.0, 0.75, 0.45      ; felinarul de la ușă, dacă vrei lumina pe tip
light_intensity = 2
light_radius = 6
light_height = 2
light_switched = true
age = 0.6                    ; cheile tale noi; nu le scrie generatorul, le pui tu pe gust
age_random = 0.25
dirt_meters = 0.5
```

Variantele `inn_1`, `inn_2` sunt fișiere separate: ori secțiuni separate, ori un `models = inn, inn_1, inn_2`
dacă ai așa ceva pe tip (nu știu exact ce suporți, tu decizi).

Ancorele (ușă, felinar, horn, firmă) vin în comentariu în secțiunea de `layouts.ini`
(`; door at x, y, z yaw`), ca să pui ce obiect vrei acolo. `yaw` e în grade în jurul lui Y, 0 = privind pe +Z.

## Trei chestii de care să știi

1. **Emissive-ul geamurilor arde și ziua.** `object.frag` adună `emissive * EMISSIVE_STRENGTH` mereu.
   L-am lăsat slab în atlas (~0,35) ca să nu orbească, dar corect ar fi să-l înmulțești cu „noaptea" pentru
   materialele alpha-tested ale clădirilor (sau în general). Când faci asta, îmi zici și-l ridic în atlas.
2. **Calibrarea lui `sky_open`.** Cu planul pământului blocant, un perete de parter perfect liber iese la
   ~0,5–0,6 (jumătate din emisferă dă în pământ sub 4 m). Cu `mix(0.3, 1.0, sky_open)` parterul e implicit cu
   ~15% mai întunecat decât props-urile de lângă. Ori normalizezi tu (0,5 = perete liber), ori îmi zici să nu
   mai blochez pământul (murdăria de la bază o faci oricum din teren). Eu aș prefera a doua.
3. Coacerea durează 2–4 s pe casă (64 raze/vertex, grid uniform); nu e ceva de făcut la runtime.

## Costuri

| casă | triunghiuri LOD0 | LOD1 | LOD2 | LOD3 |
|---|---|---|---|---|
| hovel | ~3,6k | 1,4k | 0,4k | 0,1k |
| cottage | ~8,8k | 2,9k | 0,8k | 0,2k |
| barn | ~13,5k | 4,8k | 70 | 70 |
| town_house | ~13,3k | 4,3k | 1,0k | 0,14k |
| inn | ~16k | 5,1k | 1,2k | 0,14k |

Vertecși: 25–45k pe casă (nesudați: fiecare quad cu ai lui, ca normalele și weathering-ul să fie corecte
pe muchii).

## Testat

`potop_editor --gallery buildings` pe 21r încarcă fișierele v7 fără erori („2 models, 24 textures").
`house_view` folosește `building.vert/frag` cu aceleași uniforme ca `objects.c` (`building_material_uniforms`),
deci ce vezi în screenshot-urile lui e ce face shaderul tău.
