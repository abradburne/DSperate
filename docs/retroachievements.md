# RetroAchievements in DSperate

How the RetroAchievements pieces fit together, what the player sees and what runs where.
DSperate plays **Casual** (softcore) only: hardcore stays off until RetroAchievements has
approved the emulator.

## The pieces

`src/cheevos/` is a library of its own (`dsperate_cheevos`), with no screen in it:

| File | What it is |
| --- | --- |
| `cheevos_identity` | the name DSperate gives the server (below) |
| `cheevos_client` | `rc_client` over the vendored rcheevos 12.4, behind a pimpl: sign-in, the set, per-frame evaluation, typed `Event`s out |
| `cheevos_session` | one run as a frontend drives it: the sign-in at start and who keeps it, the game's identity, the set asked for once signed in, a save state's progress held until the set is there, the account page's list |
| `cheevos_credentials` | the sign-in files and which wins (unit-tested) |
| `cheevos_outbox` | unlocks the server has not confirmed, on disk (unit-tested) |
| `cheevos_badges` | the badge cache shared with DSlot, and the media host behind it |
| `cheevos_http` | libcurl through `dlopen`, abortable |

The credentials, the outbox and the badges sit behind small interfaces of their own so they
can move into a library DSlot and the other emulators share.

The SDL frontend holds one `cheevos::Session` for the run (declared before any state can load,
so a state's progress is never lost), calls `frame(nds)` once per emulated frame (the extra frame
an autosave runs for its thumbnail included) and `idle(nds)` while paused, `after_reset()` on
every reset (the menu's and the firmware's power-off reboot), and maps each `Event` to a toast in
one place, `cheevos_toast` in `main.cpp`. The pause menu's pages read the session through
`cheevos_menu.cpp`.

## The name on the wire

RetroAchievements tells clients apart by the User-Agent's product token, refuses one that is
not unique, and wants a numeric version that rises. DSperate sends

    DSperate-nibOS/0.<n> nds/0.<n> rcheevos/12.4

`DSperate-nibOS` is `kClientName` in `cheevos_identity.h`, the one place it is spelled. `<n>` is
the fork's commit count since upstream 7c1fb67, the same number as the package's pkgver
(`0.r7c1fb67.<n>.<commit>`), so it rises with every commit and names the build; CMake takes it at
configure time (`-DDSPERATE_CHEEVOS_VERSION=` overrides it). The `nds` segment is the core, as
RetroAchievements advises for an emulator of more than one system (`gba` when GBA plays with
achievements). The last clause is `rc_client_get_user_agent_clause`'s. The DSi TMD download sends
the product token alone.

**Earlier identity, to disclose when applying:** until 0.r7c1fb67.225 this fork sent upstream
beebono/DSperate's User-Agent unchanged, `DSperate/<upstream's latest tag> rcheevos/12.4`
(`DSperate/3.0.5 rcheevos/12.4` in the builds that went out, `DSperate/1.13.1` without tags).

## What the player is told

Every `Event` is logged (`cheevos: ...`) and, with `cheevos.toasts` on, shown:

- **The placard**, as a game's set loads: *RetroAchievements: Casual*, the game, "12 of 40
  unlocked" and how many are unsupported (a condition reads memory DSperate does not map).
  RetroAchievements requires the mode to be shown at game start in every mode, so this one is
  shown even with toasts off.
- **An unlock**: *Achievement unlocked*, title, description, points, badge (below).
- Sign-in, sign-out, a ROM RetroAchievements does not know, server errors, going offline and
  back, all achievements earned.

Those are toasts, one at a time in turn (nibkit's queue, below). Two things are not, because
they last: rcheevos' **indicators** (`cheevos_indicators.cpp`), nibkit's panels
(`nib_indicators_*`, the same as inkdot's), kept on screen bottom left, opposite the toasts,
while rcheevos says they are up:

- **Progress** (`RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_*`): a panel in the toasts'
  style headed *Progress*, the achievement, its measured progress ("5/10") and badge. One at a
  time, updated in place, taken down when rcheevos says. At the top of the corner.
- **Challenges** under way (`..._CHALLENGE_INDICATOR_SHOW` / `_HIDE`): each one just its badge,
  an icon (56 units, rounded corners) in a gold ring that pulses slowly (every 1.8 s) so it
  reads as live, the whole icon 60% opaque so the game shows through; in a row under the
  progress, the newest at the right. A challenge for the achievement whose progress is shown is
  no icon: the progress panel shows it, headed *Challenge*, its badge ringed. Gone when the
  challenge ends, the game changes or the machine resets. When more are up than fit across the
  screen, the oldest are left out.

A panel or icon whose badge is on its way waits for it up to 1.5 s, as an unlock does, then
shows without it (an icon as a dark square). The panel and icons are drawn once each time what they say changes (as an ordinary task, off
the real-time class); while a ring pulses only the ring is painted again, 16 times a second, and
handed to the GLES presenter's overlay slot then, not every frame. The clock is the frames
played, so a pulse stops while the game is paused.

They are drawn only where DSperate has nibkit; without it they are in the log alone.

Leaderboards are submitted by `rc_client` as it finds them; nothing is drawn for them yet.

## Sign-in: who keeps it

Tried in order (`resolve_credentials`): a token file the player named (`cheevos.token_file`,
`--cheevos-token`), the launcher's (`RA_TOKEN_FILE`, `RA_USERNAME`), DSperate's own
`<config>/cheevos.token`, the CFW front end's (`cheevos.use_system_login`). The first that gives
a username and a token is used; the others that failed are logged.

**Started by DSlot** (`RA_TOKEN_FILE` set): DSlot owns `~/.config/dslot/retroachievements.token`.
Signing in or out from DSperate's pause menu lasts for that run and writes nothing; a toast says
to sign in or out in DSlot to keep it. **Run on its own**: a password sign-in from the menu keeps
the token in `<config>/cheevos.token` (0600; the password is never stored), and signing out
removes that file. A token file the player named, or the CFW's, is never written or removed.

`RA_HARDCORE=1` is noted in the log; Casual is what is played.

## Unlocks earned offline

When `rc_client` asks to award an achievement, the request is parsed (`parse_award_request`) and
the unlock written to `<config>/cheevos.outbox` (0600, renamed into place) before it is sent: the
user, the achievement, the game's hash and when it was earned, never the token. The server's answer
strikes it off once it settles the award (any answer but none at all, a 5xx, 401, 403, 408 or 429).
While the run lasts, `rc_client` retries on its own (*RetroAchievements offline* ... *reconnected*).
If DSperate quits first, the next sign-in as the same user sends what is left, signed with that
day's token and saying how long ago each was earned (`seconds_since_unlock`); a reconnect sends
them again if they failed.

A request that never reached the server (DNS, connect, TLS, a timeout) goes back to rcheevos as
`RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR` with curl's error as the body, which rcheevos
retries and logs.

## Threads and quitting

`rc_client` and every callback run on the emulation thread. One worker thread (SCHED_OTHER,
nice 10) carries the HTTP: rcheevos' requests (sign-in, the set, unlocks, pings) always first,
badge downloads in a lane of their own behind them. Quitting sets a stop flag that libcurl's
progress callback (`CURLOPT_XFERINFOFUNCTION`) checks about once a second, so a request in flight
is abandoned instead of waited out (up to 40 s before); whatever was unconfirmed stays in the
outbox.

## Logging

rcheevos' warnings and errors are always logged (`cheevos: rc: ...`); with `DS_CHEEVOS_VERBOSE=1`
its verbose log too, and every request and response.

## GBA

GBA games get no achievements yet. It would take `Memory::attach` for rcheevos' GBA map (32 KB
IWRAM, 256 KB EWRAM, the cart's save RAM), the ROM's hash for `RC_CONSOLE_GAMEBOY_ADVANCE`,
`Session::identify` and `frame` in the GBA mode's loop, and `Core::Gba` at load.

## Unlock toast

When `rc_client` fires `RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED`, the client posts an `Event` of
type `Unlock`: the id, title, description, points, unlock time and the **badge name**
(`rc_client_achievement_t::badge_name`). `cheevos_toast()` turns it into a `Toast`
(*Achievement unlocked*, four seconds, bottom right of the panel) and asks the client for the
badge image with `Client::fetch_badge(name)`; the indicators ask for theirs too.

The badge is fetched and decoded **on the cheevos worker thread**, the one that already
carries every HTTP request (SCHED_OTHER, nice 10), when it has no rcheevos request waiting:
nothing of it touches the emulation thread, the real-time budget (`rt_budget.h`) or a frame.
The worker (`load_badge` in `cheevos_badges.cpp`):

1. looks for `<cache>/retroachievements/badges/<name>.png`, where `<cache>` is
   `$XDG_CACHE_HOME` or `~/.cache`. This folder is **shared with DSlot**, which writes the same
   files for its shelf, so a badge is downloaded once per device whichever program asks first;
2. failing that, GETs `https://media.retroachievements.org/Badge/<name>.png` through the
   libcurl backend and writes it to the cache (temporary file, then rename) on a 200;
3. decodes the PNG (`cheevos_png.cpp`, our own decoder over the vendored miniz inflate:
   8-bit greyscale, RGB, palette, grey+alpha and RGBA, palette and grey at 1/2/4 bits too,
   filters 0–4, no interlace) into `0xAARRGGBB` pixels and queues a `Badge` for the frontend.

`cheevos_show()` hands each badge from `take_badges()` to the toast queue (`ToastQueue::badge`),
which gives it to nibkit's queue: an unlock waiting for its badge waits up to 1.5 s at the head
of the queue, then shows without it, taking it if it comes later.

## How every toast is drawn

Every toast (an unlock, the sign-in and the placard, a network note, the SD card and DSi DSP
warnings, the NAND shortcuts) goes through `ToastQueue` (`toast_queue.cpp`). Where DSperate is
built with nibkit (`DSPERATE_NIBKIT_MENU`, as nibOS's package is) that is **nibkit's queue**,
`nib_toasts_*` from nibkit-menu-c, the one DSlot and every nibOS app use: its order, its times
(an unlock 4 s, anything else 2.5 s, or the toast's own), its wait for a badge, and its
pictures in DSlot's style: DSlot's face (Roboto Condensed), panel colour and rounded corners,
the heading gold for an unlock and red for a problem, the points in gold at the title's right
and the badge at the left, placed in the corner every app's toast sits in (bottom right, a
margin in). Its clock is the frames played (`tick()` once per emulated frame, x 1000 / 60), so a
toast waits while the game is paused.

A picture is RGBA with alpha, sized to the toast and drawn for the target's scale
(`nib_menu_scale_for`). It is drawn **once** per toast and target size, as an ordinary task
(`OffRealtime`, so the text rasterising never counts against `RLIMIT_RTTIME`), and kept. Where
the output tier has a panel-resolution canvas each frame lays it over the canvas and tells the
display what it covered. Where the screens are textures the GPU scales (the GLES present tier,
which nibOS's RG DS+ uses) there is no such buffer: the picture goes to the presenter as an
overlay (`Display::set_overlay`, `GlPresent::set_overlay`), uploaded once and blended over the
screens at every present until it changes. The presenter has two overlay slots: the toast, and
the indicators above. `DS_VERBOSE` logs how long each toast took to draw: about 3 ms on the RG DS+.

On a target below half the screen's scale (the 256x192 scratch some output tiers draw into),
`menu.cpp`'s `draw_toast` draws the toast on show in DSperate's own 5x7 face instead, which folds
the words to capitals; the badge then sits at the left edge, nearest-neighbour scaled. Built
without nibkit (upstream's builds), `ToastQueue` is DSperate's own queue with the same rules
counted in frames, and every toast is the pixel toast. The toasts' words are written in sentence
case for the first and read the same in the second.

Nothing here is configurable beyond the existing `cheevos.toasts` switch: with toasts off, the
indicators are off too and no badge is fetched; the placard still shows.
