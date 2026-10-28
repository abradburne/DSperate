# Changelog

This fork's changes (abradburne/DSperate, the `pocketos` branch, packaged for nibOS as
`dsperate 0.r7c1fb67.<n>`). Upstream's are in beebono/DSperate's history.

## Unreleased

### RetroAchievements

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
