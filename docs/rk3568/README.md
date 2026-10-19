# Measurements on the RG DS+ (RK3568)

The write-ups behind the RK3568 port (`../rk3568-port.md`), measured on the Anbernic RG DS+
(four Cortex-A55 at 1.99 GHz, Mali-G52 on Panfrost, two 1024x768 panels) under nibOS
(formerly pocketOS), with the tools in `../../bench`. They are dated working reports: each
says what was measured, how, and what changed because of it.

| | |
|---|---|
| `measurements.md` | the emulator selection (October 2026, history), real time on the panels, the panels' refresh, the launcher integration, hi-res 3D, button-to-screen latency and frame pacing |
| `core-review.md` | the emulation core on the A55: where the time goes, the speed-ups taken and rejected, PGO, exactness |
| `render-review.md` | the render and present path: 1x and hi-res cost, the GPU per stage, Golden Sun at 2x, the fallback, battery |
| `gba-mode.md` | GBA mode: why in DSperate, the phases, mGBA's test suite, 30-game compatibility |
