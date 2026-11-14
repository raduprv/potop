# Inventory

What can be carried is `data/items.ini` (it documents its keys at its
top): one section per item, its name, category (a small sign before it in
the lists: weapon, ammo, food, material, clothing, tool, quest, money,
misc), weight, value, how many go in one row (`stack`), the body's slot it
is worn or held in, and what it is to the weapons (`weapon = ak47`: this
item is that weapon; `ammo = 7.62x39`: one of it is a round). The body's
slots are its `[slots]` section (hands, head, body, legs, feet, back).

A container (`engine/inventory.h`) is a list of rows (an item and how many)
with what is worn in the slots and a weight limit the game sets
(`inventory_set_max_weight`; 0 none). The player's pack, a dead animal's
loot, what lies on the ground: all containers. `inventory_add`,
`inventory_remove`, `inventory_count`, `inventory_move`,
`inventory_move_all`, `inventory_equip` and `inventory_unequip`, and
events for what goes in and out (`inventory_poll_event`: a quest watches
those; the game writes them to the log).

The weapons load from the pack: a reload takes the rounds there are
(`weapons_set_reserve`, `weapons_take_loaded`), the status line shows them
after the magazine (`AK-47 23/30 +120 auto`); 1 and 2 bring a weapon up
only if it is carried, and it goes into the hands.

On foot, **I** opens the inventory: the body's slots on the left (a click
takes a thing off), the rows on the right (a click chooses one: Use,
Equip, Drop, Drop all; a double click uses it), the weight over them. An
item with `usable = true` can be used: that is an event
(`INVENTORY_EVENT_USED`, `inventory_use`), what it does is the game's (the
game eats food). **E** acts on what the eye is on
within 2.5 m: a dead animal is looted (its rows next to yours; a click
moves a row across, Take all), a thing on the ground is picked up (what
fits). The pointer is free while a window is open; Escape or walking off
closes the loot.

What is dropped lies on the ground as an object (`engine/ground_items.h`):
the item's `model` names an objects.ini type (else a sack or a crate),
made as big as its `size`; a loose body like the rest (it falls, floats,
sinks, is pushed by bullets). The map's file keeps what lies about and the
player's pack (`items.bin`, by the items' names).

The game plays its part: a 40 kg pack with both rifles, their
rounds, bread, apples, coins, a knife and boots (the AK and the boots
worn); bears leave a pelt, meat and claws, falcons feathers and meat.
`--map-test` checks the weight limit, moving, wearing, and a thing laid
down coming back with the map and picked up.

## Picking things off the map

An object can be picked by hand (`engine/harvest.h`): **E** on foot on it
gives its item into the pack. objects.ini, per type or in a category's
`[defaults]`:

| Key | What |
|---|---|
| `harvest` | the item it gives (items.ini's) |
| `harvest_count` | how many a time: `2`, or `1, 3` (at random between) |
| `harvest_removes` | `true`: it goes once picked (a weed, a flower) |
| `harvest_times` | else how many times it can be picked (a willow: 2 twigs), then "Nothing left to pick" |
| `harvest_regrow_hours` | game hours after which it can be picked again (0: never; it needs the day cycle on) |
| `harvest_change_object` | the type it turns into while picked out (a bush without its berries), back once grown |

The game can set or change it per type (`harvest_set`). Each object keeps
how many times it was picked and how long until it grows again, and so
does the map's file (`harvest.bin`, each by its type and place). The
game, where objects.ini says nothing: tomatoes (3 times, 1 to 3 a time,
again after a day), willows (twigs, twice, again after two days), flowers
(they go). `--map-test` picks one out, turns it and lets it grow back,
keeps it as a file does, and picks a weed.
