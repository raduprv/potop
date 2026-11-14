# Journal and notices

The engine shows the quest log; the game keeps its quests (their flags,
why an objective is done) and tells the journal (`engine/journal.h`) what to
show: `journal_set_quest` (an id, the title, the description, the state:
active, inactive, completed, failed), `journal_set_objective` (a line
each: open, done or failed, how far along, "3/10"), `journal_set_place`
(where it is on the map), `journal_remove_quest`, `journal_remove_objective`.
The player tracks quests (their open objectives with a place are shown on
the compass and the map) and puts one objective in focus (the active
marker, with how far it is); how many may be tracked is the game's
(`journal_set_track_limit`, 3). What the player chose comes back as events
(`journal_poll_event`), if the game cares.

**J** on foot opens it: the quests by state, the newest first, a gold dot
on the tracked ones; the chosen one's title, state, story and objectives
(a ticked box, a crossed one, an empty one; the focused line gold), Focus
and Map on each open one with a place (Map opens the map on it), Track or
Untrack.

`hud_notify(hud, text, seconds, style)` shows what the game says for a few
seconds under the compass (info, quest in gold, warning in red), newest
last, fading out.

The game plays a quest, "The Hunt", in C: shoot a bear (the
place follows the nearest living bear), gather five falcon feathers (the
nearest falcon; how many from the pack), then an objective not shown
before: bring the bear pelt to the camp (a place a little way off); there
it takes the pelt, gives 150 coins and is completed. `--map-test` checks
the track limit, the markers, the focus, and the markers going with done
objectives and untracked quests. The journal is not saved yet (the game's
saves will keep its quests).
