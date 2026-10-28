# RetroAchievements in DSperate

How the RetroAchievements pieces fit the frontend. The client itself (`src/cheevos/`,
`rc_client` over the vendored rcheevos, libcurl through `dlopen`) is described in the source;
this page covers what the player sees and what runs where.

## Unlock toast

When `rc_client` fires `RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED`, the client posts a
`Message` of kind `Unlock`: the title, the description, the points and the **badge name**
(`rc_client_achievement_t::badge_name`). The frontend's `cheevos_show()` turns it into a
`Toast` (`ACHIEVEMENT UNLOCKED`, four seconds, bottom right of the panel) as before, and now
asks the client for the badge image with `Client::fetch_badge(name)`.

The badge is fetched and decoded **on the cheevos worker thread**, the one that already
carries every HTTP request (SCHED_OTHER, nice 10): nothing of it touches the emulation thread,
the real-time budget (`rt_budget.h`) or a frame. The worker:

1. looks for `<cache>/retroachievements/badges/<name>.png`, where `<cache>` is
   `$XDG_CACHE_HOME` or `~/.cache`. This folder is **shared with DSlot**, which writes the same
   files for its shelf, so a badge is downloaded once per device whichever program asks first;
2. failing that, GETs `https://media.retroachievements.org/Badge/<name>.png` through the
   libcurl backend and writes it to the cache (temporary file, then rename) on a 200;
3. decodes the PNG (`cheevos_png.cpp`, our own decoder over the vendored miniz inflate:
   8-bit greyscale, RGB, palette, grey+alpha and RGBA, palette and grey at 1/2/4 bits too,
   filters 0–4, no interlace) into `0xAARRGGBB` pixels and queues a `Badge` for the frontend.

`cheevos_show()` drains `Client::take_badges()` into a map by name each frame (a move of a
vector, nothing more). `toast_step()` holds an unlock toast back for up to 90 frames (1.5 s)
while its badge is still on its way, then shows it with or without. `draw_toast` blits the
badge at the toast's left edge, a square the height of the text block, nearest-neighbour
scaled and composited over the panel colour; the text moves right by that much. A toast with
no badge (a network note, an SD card note) draws as it always did.

Nothing here is configurable beyond the existing `cheevos.toasts` switch: with toasts off the
badge is not fetched either.
