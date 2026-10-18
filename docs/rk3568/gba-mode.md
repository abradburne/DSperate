# GBA mode: plan and progress

Goal: Game Boy Advance games on the RG DS+, from a **GBA page in DSlot**, as smooth as the DS
side: push a cart in, play, Home saves and comes back to the shelf, starting it again resumes.

## Which emulator

Wanted: modern, actively maintained, fully open source, not hostile to AI-assisted work, and
able to fit DSlot's launch/resume model.

| Emulator | Licence | State (Oct 2026) | AI contributions | Fit |
|---|---|---|---|---|
| mGBA | MPL-2.0 | Reference-grade accuracy; 0.10.5 (Mar 2025), active | **Refused**: "AI-generated code ... will be summarily closed" | Excellent library (libmgba), but we could never send fixes upstream |
| SkyEmu | MIT | Very accurate (passes mGBA's timing suite) | **Refused**: "we do not accept AI generated code" | Good, same problem |
| Mesen 2 | GPL-3.0 | Multi-system, GBA since 2024 | Reported to refuse (unconfirmed) | Heavy |
| NanoBoyAdvance | GPL-3.0 | Accurate, **archived June 2026** | — | Not maintained |
| VBA-M | GPL-2.0 | Maintained, old code base | No policy found | Average accuracy |
| gpSP (libretro) | GPL-2.0 | Fast ARM dynarec, made for weak handhelds | No policy found | Fast, least accurate |
| **DSperate, GBA mode** | DSperate's | Ours, agent-built and openly so | Welcome | Already DSlot's emulator |

**Recommendation: give DSperate a GBA mode.** A real DS runs GBA games by switching its ARM7
into GBA mode, and DSperate already has most of what that needs:

- the **ARM7TDMI** (ARMv4T, ARM and Thumb) interpreter **and** its ARM → AArch64 recompiler,
  verified instruction by instruction;
- the 2D engine (the DS's engine A is a superset of the GBA's PPU: BG modes 0–2, sprites,
  windows, mosaic, blending), DMA, timers, the event scheduler and IRQ plumbing;
- save states, the pause menu, Home-saves-and-quit, dual-window SDL/GL output, fast boot,
  frame dumps and the verification workflow;
- it is already packaged, launched and resumed by DSlot on nibOS (formerly pocketOS).

So the GBA mode becomes a new machine inside DSperate (`gba.cpp` beside `nds.cpp`) that reuses
those parts, not a second emulator to integrate. **mGBA is still useful, locally only, as the
reference** (frame dumps, save files, timings), the way melonDS is for the DS side. Using it as
a test oracle on our own machines doesn't involve contributing to it.

## What DSperate needs for GBA

| Piece | Have | To do |
|---|---|---|
| CPU | ARM7TDMI interp + JIT | GBA bus timing: wait states (WAITCNT), the 8-halfword **prefetch buffer**, open bus |
| Memory map | DS ARM7 map | GBA map: BIOS 16 KB, EWRAM 256 KB, IWRAM 32 KB, I/O, palette, VRAM 96 KB, OAM, cart ROM (3 wait-state mirrors), SRAM |
| Video | DS 2D engine | GBA register layout and VRAM addressing, **bitmap modes 3–5**, GBA scanline/H-blank timing (1232 cycles per line, 228 lines), affine reference latching |
| Sound | DS SPU (16 channels) | GBA sound: 4 PSG channels (square ×2, wave, noise) + 2 Direct Sound DMA FIFOs, SOUNDBIAS resampling to 32 kHz+ |
| BIOS | DS freebios (open) | A **free GBA BIOS** (e.g. the open-source Cult-of-GBA BIOS, MIT) by default; the user's own dump if present. HLE of SWI calls as a later speed-up |
| Saves | DS save chips | SRAM 32 KB, Flash 64/128 KB (with ID/bank switching), EEPROM 512 B/8 KB (DMA bit-serial), save-type detection from the ROM's strings plus a database |
| Extras | — | RTC (Pokémon Ruby/Sapphire/Emerald), rumble (Drill Dozer) on the DS+'s vibrator, solar/tilt only if wanted |
| Timing | DS cycle tables | GBA cycle costs; idle-loop detection carries over |
| Frontend | DS dual windows | One 240×160 screen; layouts below; same menu, states, Home |

Verification, as with the DS side: the open GBA test suites (jsmolka's gba-tests, mGBA's own
suite, AGS-style timing tests where free), plus frame-dump and save-file comparisons against
mGBA on a set of commercial games, in CI headless like DSperate's existing tests.

## The GBA page in DSlot

- **Switching pages**: L/R (or a DS | GBA tab on the touch screen) flips the shelf between
  `Roms/NDS` and `Roms/GBA`. Same shelf, same play-time and ratings, GBA carts drawn in the
  GBA's wider, shorter shell. Pushing a cart goes into the **slot-2** opening at the bottom of
  the DS+'s top half, the way a DS Lite takes GBA carts. The cart art, cart shell and slot
  graphics are new assets; the shelf code is shared.
- **Titles and art**: the header has a 12-character title and a 4-character game code; box art
  from the same thumbnail source DSlot uses for DS (No-Intro names), cached on ROMS.
- **Launch**: `dslot` already starts `/usr/lib/dslot/launch ROM`. The launcher picks DSperate's
  GBA mode for `.gba` (and `.zip` containing one). Saves go to `Saves/GBA/<ROM name>/`, states
  beside them; Menu = save state and back to the shelf, Back = DSperate's pause menu, as now.
- **Screens** (1024×768 each):
  - top: the game at 4× integer scale, 960×640, centred (a 32 px border left and right, 64 px
    above and below), with an optional LCD-grid/colour-correction shader;
  - bottom: the cart's art and the controls, a touch Home/state/fast-forward strip, or off to
    save power. A "big" option fills the top screen at non-integer 4.27× for those who prefer it.
- **Controls**: A/B/L/R/Start/Select/D-pad map directly; the stick mirrors the D-pad; X/Y are
  free for fast-forward and quick save/load; L2/R2 rewind (if added later).

## Phases

1. **GBA page in DSlot** (independent of the emulator): `Roms/GBA` scanning, page switch, GBA
   cart shell and slot-2 visuals, launcher routing by extension. Until DSperate's GBA mode
   exists, the launcher can point at a locally built mGBA SDL frontend so the page can be used
   and timed (not shipped).
2. **DSperate GBA core, first light**: memory map, ARM7TDMI in GBA mode (interpreter), video
   modes 0–5, DMA, timers, IRQs, keypad, free BIOS. Milestone: jsmolka's ARM/Thumb/memory tests
   pass; a homebrew demo and two commercial games reach gameplay with matching frames.
3. **Sound, saves, timing**: PSG + FIFOs, SRAM/Flash/EEPROM with detection, prefetch and wait
   states, RTC. Milestone: mGBA's suite at parity on the parts we target; a 30-game compatibility
   list (top sellers plus known timing-sensitive titles) plays with saves.
4. **Speed and polish on the DS+**: the JIT in GBA mode, idle loops, frame pacing at 59.73 Hz on
   the 59.94 Hz panels (audio-driven pacing or dynamic rate control), shader, rumble, Home/resume.
   Target: full speed with the CPU at its lowest clock that holds it, and battery life better than
   the DS side.
5. **Upstream** into `abradburne/DSperate`, packaged by `packaging/arch`, documented in
   DSlot's README.
6. **Game Boy and Game Boy Color**, a third machine in DSperate. A GBA plays GB/GBC carts on a
   separate built-in CPU (the Sharp SM83), which the DS dropped, so this is new: an SM83
   interpreter, the GB/GBC PPU (one background, a window, 40 sprites, GBC palettes and VRAM
   banks), cartridge mappers (MBC1/2/3 with RTC/5) and their saves, GBC double speed. Shared with
   GBA: the **PSG sound** (the GBA's four PSG channels are the Game Boy's sound chip, so phase 3
   builds it as a standalone component both machines use), the scheduler, states, menu, Home
   and frontend. Screen: 160x144 at 5x = 800x720 on the top panel. DSlot gets GB and GBC pages
   with their own cart shells (grey DMG, clear GBC). Verification: Blargg's and Mooneye's test
   ROMs, dmg-acid2 and cgb-acid2, frame dumps against a locally built SameBoy or mGBA (reference
   only). Smaller than the GBA core: an 8-bit CPU, and an interpreter is plenty fast here.

## Progress

### Phase 2, first light: done (2026-10-17)

In DSperate, branch `gba` (commits 3f9233d, f275318, 415d721 and
on, on top of `pocketos`), since merged into `pocketos` and packaged (below).

**Design.** `src/core/gba/` is a GBA machine of its own that `NDS` hosts (`NDS::load_gba()`), the
way a DS's ARM7 switches into GBA mode. Once loaded, `run_frame()`, the framebuffers, save states
and `reset()` run it; the DS path gains only an `if (gba)` per frame and nothing per instruction.

- **CPU**: DSperate's `CpuContext` and ARM7TDMI interpreter, compiled a second time against the
  GBA bus (`gba_interp_*.cpp` include the DS sources with `DS_INTERP_GBA`; accessors in
  `gba_cpu_mem.h` replace `cpu_mem.h`'s), so the DS build is byte-for-byte the same code. The GBA
  run loop keeps the ARM7TDMI's two prefetched instructions (self-modifying code, BIOS open bus).
  Cycle costs come from a per-32 KB table rebuilt from WAITCNT (N/S, 16/32-bit), DSperate's exact
  model. Two GBA-only interpreter fixes: ARMv3 `CMPP`-style tests with Rd = 15.
- **Memory map**: BIOS (read-protected, open-bus latch), EWRAM/IWRAM mirrors, I/O, palette, VRAM
  (96 KB with the OBJ mirror), OAM, cartridge ROM in its three wait-state mirrors with open bus past
  the end, plain SRAM. Direct pages through DSperate's page table; byte-store quirks on the slow path.
- **Video**: a line PPU of its own (`gba_ppu.cpp`): modes 0-5, text/affine BGs, bitmaps, 128
  sprites (affine, double size, semi-transparent, OBJ window), windows, mosaic, blending,
  brightness; 1232-cycle lines, 228 lines, HBlank at 1006. Not the DS's Engine2D: that is built on
  the DS's VRAM banking, write journal, lazy batches and 256-px lines, and an adapter would be
  larger than the GBA PPU and put GBA branches on the DS's hottest path.
- **System**: DMA 0-3 (immediate/VBlank/HBlank, repeat, reload, IRQs), timers with cascade, IE/IF/
  IME, HALTCNT, keypad and KEYCNT IRQ, an event loop in GBA cycles.
- **BIOS**: the Cult-of-GBA BIOS (MIT, built in, with a local fix: Sqrt(0) returned 1;
  `cultbios-sqrt0.patch`, `tools/gen_gba_bios.py`); `--gba-bios`/`paths.gba_bios` takes a dump.
  BIOS reads outside the BIOS see the opcodes Nintendo's BIOS leaves on the bus.
- **Frontend**: `dsperate --gba ROM` (implied by `.gba`; a `.zip` holding a `.gba` loads too, and a
  zip with no DS ROM is tried as GBA). The 240x160 picture sits centred on the top screen, so the
  DS+'s 4x top panel shows it at exactly 960x640 with a 32/64 px border; without dual windows the
  layout defaults to the single screen. Pause menu, save states, the auto slot (Home saves and
  quits, the next start resumes), per-game config, screenshots and plain SRAM saves all work,
  keyed by the GBA game code. Headless: `--gba`, `--gba-bios`, `--gba-boot-bios`, `--gba-trace`,
  and `--press F:BUTTONS[:N]` for scripted input.

**Verification.**
- `ctest` (`tests/gba_test.cpp`): jsmolka's gba-tests **arm, thumb, memory, bios and nes pass**
  (the ROMs are vendored, MIT); his hello/shades/stripes PPU demos hash-match mGBA's frames; bus
  quirks, timers, DMA, IRQs and a state round trip. The 30 DS tests still pass.
- mGBA 0.10.5 as a local oracle only: `bench/gba-oracle/mgba-oracle` (Homebrew's libmgba, never
  linked into DSperate) writes frames in DSperate's `--dump-frames` layout for comparison. (mGBA
  itself fails jsmolka arm #235, which DSperate passes.)
- **Homebrew**: Downburn (the user's own GBA port) reaches gameplay; its frames match mGBA except
  single twinkling stars of a per-frame random starfield.
- **Commercial** (the user's carts from the DS+'s ROMS card, copied to the Mac for testing only):
  Kirby - Nightmare in Dream Land and Castlevania - Circle of the Moon reach gameplay; Kirby's
  level frames are pixel-identical to mGBA's, Castlevania's differ only where a fade lands a frame
  apart. F-Zero Maximum Velocity and Mario Kart Super Circuit run their menus identically to mGBA.
- **Speed on the DS+ (interpreter only, Release -mcpu=cortex-a55, schedutil, headless):** Kirby
  in its first level 10.1 ms a frame median (p99 10.6), Castlevania in its first room 7.0 ms (p99
  8.1), against 16.7 ms: no frame over budget. Full speed before the JIT; the JIT (phase 4) is for
  battery, not for reaching 60.

### Shipped on nibOS: dsperate 0.r7c1fb67.47.b36cd8f (2026-10-17)

`gba` merged into the fork's `pocketos` branch (b36cd8f, on origin = abradburne/DSperate only) and
packaged as `dsperate 0.r7c1fb67.47.b36cd8f-1` (`packaging/arch/PKGBUILD`; the Cult-of-GBA BIOS's
licence goes in as `/usr/share/licenses/dsperate/LICENSE.cult-bios`). DSlot's launcher has
`gba="/usr/bin/dsperate --gba"` (`rootfs/dslot/launch` in nibOS); the mGBA stand-in now runs only GB and
GBC. Checked on the DS+ from DSlot's GBA page with Advance Wars (USA).zip from `/mnt/ds/Roms/GBA`:
the top panel shows 960x640 at (32, 64), so exactly 4x with the 32/64 px border, the bottom panel
black; Back opens the pause menu; Menu writes `Saves/GBA/<ROM name>/AWRE.auto.dss` (627 KB) and is
back on the shelf ~0.55 s after the press; the next start resumes from it. Still silent, and Flash/EEPROM games
keep no battery save until phase 3. (The log's "bios: ... FreeBIOS" line is the DS's and does not
apply in GBA mode.)

### Phase 3, sound: done (2026-10-17)

DSperate 4e3c5e6 (fork `pocketos`); on the DS+ since package `0.r7c1fb67.49.3c877f0` (which the
Golden Sun audio work built on top of it).

- **`src/core/psg/gb_psg`**: the Game Boy's sound chip as a standalone component, as the GB/GBC
  plan (phase 6) asks: two squares (one with sweep), wave, noise, length counters, envelopes and the
  512 Hz frame sequencer, addressed by Game Boy register numbers (NR10-NR52, wave RAM) and clocked
  at 4.19 MHz, with no GBA in it. Models DMG/CGB/AGB; the AGB one adds the two wave banks, forced
  75% wave volume and write-only bits reading 0. Channel levels are averaged per output sample.
- **`src/core/gba/gba_audio`**: the GBA's mixer around it. Direct Sound A/B: 32-byte FIFOs popped
  by timer 0/1 overflows and refilled four words at a time by DMA 1/2 in sound timing; SOUNDCNT_H
  volumes and routing, SOUNDBIAS, the 10-bit DAC clamp, and a 20 Hz high-pass standing in for the
  output's AC coupling. 32768 Hz stereo into the SPU's output ring, so the frontend's playback,
  `--dump-audio` and pacing are unchanged.
- **Checked**: `ctest` has the PSG's pitch, length and envelope and a FIFO/DMA/timer square wave.
  Against mGBA (`gba-oracle --audio`): Castlevania (PSG) matches in pitch and spectrum (0.93
  spectrogram correlation). Kirby's Direct Sound output is exactly the sample stream the game writes
  to FIFO A at its timer's 15768 Hz; the oracle's own capture of mGBA's Direct Sound disagrees with
  that stream, so for Direct Sound the game's FIFO data is the reference, not the oracle. No
  measurable speed cost (1.10 ms a frame on the Mac, as before).
- **On the DS+** (installed package, headless): Kirby in its first level 11.5 ms a frame median
  with sound against 10.1 ms without, p99 15.8 ms, 1 frame in 500 just over the 16.7 ms budget;
  its intro 10.0 ms median, 1 in 1300 over. The cost is the sample event every 512 cycles cutting
  the CPU's slices: phase 4 should mix lazily (catch up at frame end and on register/FIFO events)
  instead, before or with the JIT.
- Not yet: SOUNDBIAS's higher sampling rates (the output is always 32768 Hz), FIFO timing finer
  than the sample event.

### Phase 3, saves, timing, RTC: done (2026-10-18)

DSperate 974f7ee (fork `pocketos`, pushed to origin only), package `0.r7c1fb67.58.974f7ee`,
installed on the DS+.

- **Saves** (`src/core/gba/gba_save`): SRAM 32 KB, Flash 64 KB (Panasonic ID) and 128 KB
  (Macronix ID, banked) with ID mode, chip/sector erase and programming, EEPROM 512 B / 8 KB
  bit-serial over DMA 3 at 0x0D000000, sized by the first command. The type comes from the SDK
  string in the ROM (mGBA's method); a ROM with none that writes 0x0E gets SRAM. Files are the
  chip's bytes in mGBA's order, so the earlier stand-in's `<ROM name>.sav` files carry over.
  jsmolka's save tests pass (sram, flash64, flash128, none).
- **RTC** (`gba_rtc`): the GPIO port and the S-3511 (date/time/status, BCD, host clock plus the
  game's offset), present when the ROM has `SIIRTC_V` (Pokemon R/S/E carry it).
- **Timing** (`gba_cpu_cycles.h`, GBA build only): the prefetch buffer, timers counting 2 cycles
  after the enable, IRQs reaching the CPU 7 cycles after they are raised (and a halt sleeping
  through that), a branch's own fetch, internal cycles followed by N fetches, long
  multiply-accumulate m+2, bursts going non-sequential across regions, the I/O read map (write-only
  and unused registers read open bus), DMA alignment and its open-bus latch, the serial port with
  nothing plugged in, and the I/O the BIOS leaves (forced blank).
- **mGBA's test suite** (built with devkitARM in Docker, run scripted in both through mGBA's debug
  port; same Cult-of-GBA BIOS in both):

  | Suite | DSperate | mGBA 0.10.5 |
  |---|---|---|
  | Memory | 1498/1552 | 1546/1552 |
  | I/O read | 130/130 | 128/130 |
  | Timing | 1276/2020 | 1768/2020 |
  | Timer count-up | 504/936 | 639/936 |
  | Timer IRQ | 71/90 | 70/90 |
  | Shifter / Carry / Multiply long | 140 / 93 / 52 | 140 / 93 / 52 |
  | BIOS math | 603/615 | 591/615 |
  | DMA | 1244/1244 | 1228/1244 |
  | SIO register R/W | 25/90 | 26/90 |
  | Misc. edge cases | 4/12 | 2/12 |

  At or above mGBA on all but memory, timing and timer count-up. What is left there: the prefetch
  buffer's exact cycle in some sequences, DMA's 2-cycle start interleaving with the next
  instruction (the interpreter runs whole instructions), and Cult BIOS behaviours (CpuSet
  refusing BIOS sources, Div/Sqrt cycle counts). Fixing the BIOS ones moves its IntrWait, which
  would void every existing state, so they wait for a reason; a user's own BIOS dump
  (`paths.gba_bios`) has them right anyway.
- **Compatibility, 30 games on the DS+** (headless, the installed build's code, 3600 frames each
  with Start/A pressed through menus): all 30 run, reaching gameplay or the menus a script cannot
  get past (name entry, file select). Pokemon Emerald/FireRed/Ruby, Mario Kart Super Circuit, Super
  Mario Advance 2 and 4, Metroid Fusion and Zero Mission, Zelda Minish Cap and Link to the Past,
  Golden Sun, Mario & Luigi, Kirby Amazing Mirror, Castlevania Aria of Sorrow, FF VI Advance, Fire
  Emblem, Advance Wars, Wario Land 4, WarioWare, Sonic Advance 2, Mega Man Zero, MMBN 3, DKC 2,
  Mother 3 (fan translation), Rhythm Tengoku, Gunstar Super Heroes, Drill Dozer, Tony Hawk 2,
  Harvest Moon FoMT, Kingdom Hearts CoM. Sonic Advance 2 and Drill Dozer were the two failures of
  the first run (both: the forced-blank bit the BIOS leaves). Frame times: median 7-15 ms; the
  heaviest (Castlevania AoS, DKC 2) have 3-5% of frames over 16.7 ms in their busiest scenes, the
  rest under 1%. Save writes were seen in 20 of them within the run.
- Save states made by `.47`/`.49` (chunk GBAM, SRAM inside) still load.
- Not yet: PPU details (vertical mosaic on affine BGs, OBJ render-cycle limit, DMA 3 video
  capture), rumble (Drill Dozer), solar/tilt sensors.

### Then phase 4 (speed, polish)

- The JIT in GBA mode: `CpuContext` is the same; the recompiler needs the GBA bus in its slow-path
  helpers and the prefetch behaviour at block ends.
- Pacing at 59.73 Hz (the frontend paces at the DS's 59.83 Hz today, 0.17% fast), the bottom
  panel (cart art/controls or off), the "big" non-integer scale, an LCD shader.
- Machine boundary: today NDS hosts the GBA (`NDS::gba`, dispatch in `run_frame`, states,
  `Gpu::show_gba`). Before a third machine (GB/GBC, SM83 CPU) lands, lift that into a small
  interface (run a frame, framebuffer + size, keys, save data, states with a machine tag in HEAD,
  reset) that the frontend and NDS dispatch through, so menu/Home/states stay machine-agnostic.
- Speed first where the compatibility run found it: Castlevania AoS and DKC 2 run 3-5% of frames
  over budget on the A55 with the interpreter; the JIT and lazy audio mixing are for those.
  **Done another way (2026-10-18, core-review.md):** the PPU and WAITCNT changes plus PGO took AoS
  9.9 -> 6.7 ms and DKC 2 12.5 -> 9.6 ms median with no frame over budget (package .78). Lazy audio
  mixing was built (exact) and measured no gain, so it is not in; the GBA JIT is not needed for 60.

## Risks and open questions

- **BIOS**: a free replacement BIOS covers nearly all games; a few rely on exact BIOS
  behaviour (the startup animation, some checksum games). Users can drop in their own dump.
- **Accuracy tail**: the last few percent of GBA games are timing-sensitive (prefetch, DMA
  priorities, PPU mid-line changes). mGBA as the oracle keeps this measurable.
- **Scope**: DSperate's core is DS-shaped. Keeping the GBA machine separate (sharing CPU,
  scheduler, 2D pieces through interfaces) avoids slowing the DS path.
- **Naming**: does the GBA mode stay DSperate, or become a sibling front name? It doesn't change
  the work.

Sources: [mGBA's AI policy (Held Games, 2026-09)](https://heldgames.com/guides/emulator-projects-ban-ai-code),
[SkyEmu README](https://github.com/skylersaleh/SkyEmu/blob/dev/README.md),
[GBA emulator overview](https://emulation.gametechwiki.com/index.php/Game_Boy_Advance_emulators),
[2026 comparison](https://tech-insider.org/mgba-vs-vba-m-vs-skyemu-2026/).
