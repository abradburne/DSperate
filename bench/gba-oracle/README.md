# gba-oracle

`mgba-oracle`: mGBA (Homebrew's 0.10.5, `brew install mgba`) as a **reference only**, for
comparing DSperate's GBA mode frame by frame. It runs a ROM for N frames with scripted keys and
writes frames in the layout of DSperate's `--dump-frames`, so the same scripts read both. Mac only,
our machines only: not shipped, not linked into DSperate (mGBA takes no AI-written code, and none
goes its way).

    make                      # needs brew's mgba
    ./mgba-oracle ROM --frames 600 --dump out.raw [--dump-from 500] [--bios cultbios.bin] [--press 300:start:5]

`--press F:BUTTONS[:N]` matches `dsperate-headless --press`. Give both the same BIOS (the
Cult-of-GBA build DSperate embeds) and both boot past it, so frame numbers line up.
Comparison scripts used for phase 2 (`vs.sh`, `cmpraw.py`, `raw2png.py`, `shots.py`) are in
`../../../gba-work/` (scratch; not part of either repo).
