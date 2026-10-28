# RetroAchievements in DSperate

How the RetroAchievements pieces fit the frontend. The client itself (`src/cheevos/`,
`rc_client` over the vendored rcheevos, libcurl through `dlopen`) is described in the source;
this page covers what the player sees and what runs where.

## Unlock toast

When `rc_client` fires `RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED`, the client posts a
`Message` of kind `Unlock`: the title, the description, the points and the **badge name**
(`rc_client_achievement_t::badge_name`). The frontend's `cheevos_show()` turns it into a
`Toast` (*Achievement unlocked*, four seconds, bottom right of the panel) and asks the client
for the badge image with `Client::fetch_badge(name)`.

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
while its badge is still on its way, then shows it with or without.

## How every toast is drawn

Every toast (an unlock, the sign-in and the set loaded, a network note, the SD card and DSi DSP
warnings, the NAND shortcuts) is drawn in **DSlot's style** where DSperate is built with nibkit
(`DSPERATE_NIBKIT_MENU`, as nibOS's package is): `nib_toast.cpp` asks nibkit-menu-c's
`nib_toast_render` for a picture of it, with DSlot's face (Roboto Condensed), panel colour and
rounded corners, the heading gold for an unlock and red for a problem, the points in gold at the
title's right and the badge at the left. The picture is RGBA with alpha, sized to the toast and
drawn for the canvas's scale (its width over the screen's 1024 units). It is drawn **once** per
toast and canvas size, as an ordinary task (`OffRealtime`, so the text rasterising never counts
against `RLIMIT_RTTIME`), and kept. Where the output tier has a panel-resolution canvas each
frame lays it over the canvas, bottom right, and tells the display what it covered. Where the
screens are textures the GPU scales (the GLES present tier, which nibOS's RG DS+ uses) there is
no such buffer: the picture goes to the presenter as an overlay (`Display::set_overlay`,
`GlPresent::set_overlay`), uploaded once and blended over the screens at every present until the
toast ends. `DS_VERBOSE` logs how long each one took to draw: about 3 ms on the RG DS+.

Without nibkit, or on a canvas below half the screen's size (the 256x192 scratch some output
tiers draw into), `menu.cpp`'s `draw_toast` draws it in DSperate's own 5x7 face instead, which
folds the words to capitals: the badge then sits at the left edge, nearest-neighbour scaled. The
toasts' words are written in sentence case for the first and read the same in the second.

Nothing here is configurable beyond the existing `cheevos.toasts` switch: with toasts off the
badge is not fetched either.
