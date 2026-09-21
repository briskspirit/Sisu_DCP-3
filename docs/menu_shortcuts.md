# Numeric Menu Shortcuts

Opening Menu arms numeric navigation. The shortcut controller selects existing
menu rows and invokes their ordinary Navi handlers; it has no separate save,
network-request, security, or confirmation logic.
Tone pickers also reuse their ordinary selection-preview handler.

## Original Behavior

Nokia 3210 v6.00 implements shortcuts through its shared numeric editor, not
through literal digit comparisons in each menu handler. The relevant ROM paths
are the editor descriptor at `0x002ded94`, menu dispatcher at `0x002638e4`,
static menu opener at `0x00263d30`, and dynamic opener at `0x00263154`.

- The input window is `0x181` eight-millisecond ticks: 3080 ms.
- An unambiguous choice normally activates immediately.
- Item flag `0x04` instead highlights the choice for `0xc0` ticks (1536 ms)
  before activation. Another digit activates that parent first, then selects its
  child. This includes editor handoffs and the dynamic game-option actions at
  `0x002dbd84` through `0x002dbe48`.
- The editor keeps two digits, dropping the oldest on overflow. A possible
  two-digit prefix waits for another digit or the input timeout. For example,
  root `1` remains ambiguous when menu 10 or 11 is visible. Leading zeroes do
  not remove that ambiguity: `01` also waits for the input timeout.
- Hidden static entries retain their original numbers. The highest visible
  ordinal controls prefix ambiguity, but hidden choices cannot be activated.
- Invalid choices use display record 52 and translated SID `0x2aa`, then return
  to standby. Normal scrolling ends shortcut mode.

Games' root descriptor and dynamic options both inherit shortcut mode. Their
level ladders and gameplay do not: those keep their ordinary digit controls.
The appended Pac Man entry follows the same rules as the stock games.

## Ownership

Each app exposes its current selection, count, visibility, context, and deferred
activation flags through `menu_shortcut_view_t`. Dynamic catalogues use their
existing list builders. A changed route, submenu identity, visibility, keyguard,
or selected row prevents a deferred action from operating on stale state.

Returning from a dialog or editor does not re-arm shortcuts. A fresh Menu entry
does. Held shortcut keys remain consumed until release so they cannot insert
characters or control a game after the shortcut changes screens. A subsequent
fresh key belongs to the destination editor or game normally.

## Verification

`test_menu_shortcuts` covers deadlines, wraparound, prefixes, hidden choices,
rapid sequences, held keys, star/hash passthrough, and route interruption. `test_menu_shortcut_views`
checks the real apps' list descriptors, including changing game menus and custom
ringtone availability. `test_menu_results` exercises shortcuts through the real
Settings handlers, including unsupported operations and failed persistence.

The timing and flags above come from ROM analysis. They are not a claim of
side-by-side timing measurements on an original handset.
