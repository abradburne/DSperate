# Changelog

This fork's changes (abradburne/DSperate, the `pocketos` branch, packaged for nibOS as
`dsperate 0.r7c1fb67.<n>`). Upstream's are in beebono/DSperate's history.

## Unreleased

### LCD shaders

- **GBA DS's display shaders** (`video.lcd_shader`, `--lcd-shader`, OPTIONS > VISUAL FX > LCD
  SHADER): off, the LCD grid, scanlines, soft cells, and subpixels (red, green and blue stripes
  with a gap, light, normal and strong, the picture brightened so it doesn't go dim), over DS
  games a cell per DS pixel and over GBA games a cell per GBA pixel. The LCD GRID row now sets
  how dark the grid, scanlines and soft cells are. nibOS's renderer sink (the GLES present) draws
  every look; the Vulkan present draws them at whole-number scales; the CPU scaler, the RGA and
  the display engine draw the LCD grid in their place. A config with only `lcd_grid` keeps its
  grid (`grid` is the default), and `off` turns the grid off too. Ported from GBA DS by kyannon
  (GPL-3.0, https://github.com/kyannon/gba-ds, `src/ui.c` at 7189176 "Release v1.5").
- **The GBA's colours** (`video.colour = gba`, COLOUR > GBA): mGBA's GBA Color shader (Pokefan531
  and hunterk), as GBA DS runs it. Colour correction now reaches GBA games on every route (it
  used to on the Vulkan present only).

### Low battery

- **The low battery over a game** (nibkit-menu-c 0.4.0's `nib_battery_*`): when nibOS warns
  (10%, 5%, empty) a toast says so, and from 5% a small red battery with the percent left stays
  at the top right of the top screen till the charger goes in. nibOS then powers off near empty,
  and DSperate writes its resume state on the way (SIGTERM, with `autosave`). Built against an
  older nibkit-menu-c, or run with one, DSperate shows none.

### Volume and brightness

- **The volume and brightness indicator** (nibkit-menu-c 0.2.3's `nib_levels_*`): when the volume
  keys or Menu + volume change a level, nibkit's panel shows at the bottom right of the top
  screen, in DS and GBA mode, and fades 1.5 s after the last press; a toast in that corner moves
  up above it. Built against an older nibkit-menu-c, or run with one, DSperate shows none.

### RetroAchievements

- **GBA games earn achievements** in GBA mode, with everything DS games have: the Casual
  placard, unlock toasts with badges, progress and challenge indicators, the offline outbox,
  progress in save states and the resume point. The game is identified by rcheevos' GBA rule (the
  MD5 of the whole ROM file, a zipped one's entry unzipped) and evaluated against rcheevos' GBA
  map: internal work RAM, on-board work RAM and the cartridge's SRAM or Flash (an EEPROM's
  achievements are unsupported). The User-Agent's core segment says `gba` in GBA mode.
- **A name of its own.** The User-Agent is `DSperate-nibOS/0.<n> nds/0.<n> rcheevos/12.4`:
  RetroAchievements refuses a client name that is not unique, and the fork had been sending
  upstream's (`DSperate/3.0.5 rcheevos/12.4`). `<n>` is the package's pkgver count, so the
  version rises with every commit. The DSi TMD download sends `DSperate-nibOS/0.<n>` instead of
  `DSperate`. See docs/retroachievements.md for the earlier identity to disclose.
- **The mode as a game starts**: *RetroAchievements: Casual*, the game, how many of its
  achievements are unlocked and how many are unsupported. Shown even with toasts off, as
  RetroAchievements requires.
- **Progress and challenge indicators** from rcheevos' indicator events, kept on screen bottom
  left while they last, nibkit's panels (the same as inkdot's): the measured progress of one
  achievement, updated in place, above a row of icons for the challenges under way, each its
  badge alone, 60% opaque, in a slowly pulsing gold ring; a challenge for the achievement in
  progress is shown in its panel.
- **Unlocks earned offline survive a quit**: each award is written to `<config>/cheevos.outbox`
  (0600, no token in it) before it is sent and struck off when the server settles it; the next
  sign-in sends what is left.
- **Quitting no longer waits on the network**: a request in flight is abandoned within about a
  second (it could hold the quit up to 40 s).
- A request that never reached the server goes back to rcheevos as a retryable client error with
  curl's reason, rather than status 0 and an empty body.
- rcheevos' warnings and errors are always logged; its verbose log with `DS_CHEEVOS_VERBOSE=1`.
- **Signing in and out from the pause menu**: run on its own, DSperate now keeps a password
  sign-in's token in `<config>/cheevos.token` and removes it on sign-out (neither was ever done).
  Started by DSlot, the sign-in is DSlot's: the menu's sign-in and sign-out last for that run,
  write nothing, and say to sign in or out in DSlot.
- The firmware's power-off reboot and the extra frame an autosave runs for its thumbnail now
  keep rcheevos in step (it missed the reset, and the frame).
- Badge downloads wait behind unlocks, pings and sign-ins.
- The pause menu's achievement switches read `yes`/`no` like every other setting, and the list is
  built once per refresh, not twice.
- **Toasts through nibkit's shared queue** (nibkit-menu-c 0.1.2's `nib_toasts_*`): the order,
  the times, the wait for a badge and the pictures every nibOS app has. Built without nibkit,
  DSperate keeps its own queue and pixel toast.
- Inside: `cheevos::Session` holds the run's RetroAchievements (out of `main.cpp`), the client
  keeps `rc_client` behind a pimpl and reports typed events, and the credentials, badges and
  outbox are modules of their own, the credentials' precedence and the outbox unit-tested.
