# Dialogs

The engine shows a conversation, the game holds it (`engine/dialog.h`):
`dialog_open` (who: an actor and a name), `dialog_say` (their line),
`dialog_set_choices`, `dialog_add_choice`, `dialog_remove_choice` (the
player's answers, each with the game's id, unique in the conversation, so
they can change while it goes on; none: a line to read and Continue, id
0), `dialog_close`. What the player picks comes back as an event
(`dialog_poll_event`: the answer's id, or that they closed it), and the
game says the next line: its trees, conditions and flags are its own C
code. The window sits low in the middle, in a bigger font (`dialog_text_scale`
times the interface's, 1.6; Settings > Interface: 2 or more for a
television across the room) on a see-through ground (80%): the name in gold, the line,
the answers (a click, or 1 to 9). While it is open the pointer is free and the
player stays put; Escape or walking off (6 m) closes it.

Actors can have a name the game gives them (`actors_set_name`; enemies'
in red): it is shown over their heads within 30 m. The game knows its own
actors by its numbers for them (`actors_set_game_id`, 0 none;
`actors_find_by_game_id`): a quest's wolf is that wolf, not any wolf. Their
ids are only for while they live in memory; the numbers are kept in the
map's file with the actors (`actors.bin`: type, place, name, number, enemy
or not, dead or not; a map from before them leaves the actors there are).
The hits only report the damage (`projectiles_take_hits`: which actor,
where on it, how much, who shot); whether it flinches or dies is the
game's (`actors_hurt`, `actors_kill` only play it).

The World window has what K gives the actors it puts: a name, the game's
number, enemy or not. `actors_look_at` turns
one standing to look at a place (a talker at the player),
`actors_stop_looking` lets it be.

The game's talker is Moș Martin, an old bear put down a few meters ahead
the first time one is on foot. **E** on him talks: who he is, the other
bears (he knows if you have shot any), and his honey, which he trades for
three tomatoes: a small quest in the journal (the place is Moș Martin, the
count from the pack), done when you bring them (a jar of honey). Shooting
him only makes him cross. `--map-test` checks the dialog's events.
