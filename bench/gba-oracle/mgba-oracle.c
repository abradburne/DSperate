// mgba-oracle: the reference side of DSperate's GBA frame comparisons. Runs a
// ROM in libmgba (Homebrew's mGBA 0.10.5) for N frames with scripted keys and
// writes frames in the layout of DSperate's --dump-frames (two 256x192 BGRA
// screens per frame, the GBA picture at 8,16 on the first), so the same
// tools read both. Our machines only: mGBA is a test oracle here, never
// shipped or linked into DSperate (and mGBA takes no AI-written code).
//
//   mgba-oracle ROM --frames N --dump OUT.raw [--dump-from F] [--bios FILE]
//               [--boot-bios] [--press F:BUTTONS[:N]]... [--audio OUT.s16]
// --audio: s16 stereo at 32768 Hz, the rate DSperate's --dump-audio writes in GBA mode.
// BUTTONS: a b select start right left up down r l, joined with '+'.
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/core/interface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Quiet, except ORACLE_LOG=1 prints everything (the game's own debug prints among it).
static void quiet(struct mLogger* l, int cat, enum mLogLevel lv, const char* fmt, va_list a) {
  (void)l; (void)lv;
  if (!getenv("ORACLE_LOG")) return;
  fprintf(stderr, "[%s] ", mLogCategoryName(cat));
  vfprintf(stderr, fmt, a);
  fputc('\n', stderr);
}

struct Press { int frame, n; unsigned keys; };

// Every sample mGBA's mixer produces, at the GBA's own rate (32768 Hz at the default SOUNDBIAS).
static void post_audio(struct mAVStream* s, int16_t l, int16_t r) {
  FILE* f = *(FILE**)((char*)s + sizeof(struct mAVStream));
  int16_t lr[2] = {l, r};
  fwrite(lr, 2, 2, f);
}

static unsigned parse_buttons(const char* s) {
  static const char* names[] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l"};
  unsigned m = 0;
  char buf[128]; snprintf(buf, sizeof buf, "%s", s);
  for (char* t = strtok(buf, "+"); t; t = strtok(NULL, "+"))
    for (unsigned i = 0; i < 10; ++i) if (!strcmp(t, names[i])) m |= 1u << i;
  return m;
}

int main(int argc, char** argv) {
  const char *rom = NULL, *dump = NULL, *bios = NULL, *audio = NULL;
  int frames = 60, from = 0, boot_bios = 0, np = 0, npeek = 0;
  unsigned peek[64];
  struct Press press[256];
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) from = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
    else if (!strcmp(argv[i], "--boot-bios")) boot_bios = 1;
    else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio = argv[++i];
    else if (!strcmp(argv[i], "--peek") && i + 1 < argc && npeek < 64) peek[npeek++] = (unsigned)strtoul(argv[++i], NULL, 16);
    else if (!strcmp(argv[i], "--press") && i + 1 < argc && np < 256) {
      char b[96] = ""; struct Press p = {0, 1, 0};
      sscanf(argv[++i], "%d:%95[^:]:%d", &p.frame, b, &p.n);
      p.keys = parse_buttons(b); press[np++] = p;
    } else rom = argv[i];
  }
  if (!rom || !dump) { fprintf(stderr, "usage: mgba-oracle ROM --frames N --dump OUT.raw [--dump-from F] [--bios FILE] [--boot-bios] [--press F:KEYS[:N]]\n"); return 2; }
  static struct mLogger logger = { .log = quiet };
  mLogSetDefaultLogger(&logger);
  struct mCore* core = mCoreFind(rom);
  if (!core || !core->init(core)) { fprintf(stderr, "no core for %s\n", rom); return 1; }
  mCoreInitConfig(core, NULL);
  if (bios) { mCoreConfigSetValue(&core->config, "useBios", "1"); mCoreConfigSetValue(&core->config, "bios", bios); }
  else mCoreConfigSetValue(&core->config, "useBios", "0");
  mCoreConfigSetValue(&core->config, "skipBios", boot_bios ? "0" : "1");
  mCoreLoadConfig(core);
  core->opts.volume = 0x100;   // no defaults are loaded: it would be 0, silence
  core->loadConfig(core, &core->config);
  static color_t px[240 * 160];
  core->setVideoBuffer(core, px, 240);
  if (!mCoreLoadFile(core, rom)) { fprintf(stderr, "cannot load %s\n", rom); return 1; }
  core->reset(core);
  if (getenv("ORACLE_DEBUG")) fprintf(stderr, "opts volume %d mute %d\n", core->opts.volume, core->opts.mute);
  FILE* aout = audio ? fopen(audio, "wb") : NULL;
  struct Stream { struct mAVStream s; FILE* f; } stream = {{0}, aout};
  stream.s.postAudioFrame = post_audio;
  if (aout) core->setAVStream(core, &stream.s);
  FILE* out = fopen(dump, "wb");
  if (!out) { fprintf(stderr, "cannot write %s\n", dump); return 1; }
  static unsigned char fb[2][256 * 192 * 4];
  for (int f = 0; f < frames; ++f) {
    unsigned keys = 0;
    for (int k = 0; k < np; ++k) if (f >= press[k].frame && f < press[k].frame + press[k].n) keys |= press[k].keys;
    core->setKeys(core, keys);
    core->runFrame(core);
    if (f < from) continue;
    memset(fb, 0, sizeof fb);
    for (int i = 0; i < 256 * 192; ++i) { fb[0][i * 4 + 3] = 0xFF; fb[1][i * 4 + 3] = 0xFF; }
    for (int y = 0; y < 160; ++y)
      for (int x = 0; x < 240; ++x) {
        const uint32_t c = (uint32_t)px[y * 240 + x];   // XBGR8: R in the low byte
        unsigned char* d = &fb[0][((16 + y) * 256 + 8 + x) * 4];
        d[0] = (c >> 16) & 0xFF; d[1] = (c >> 8) & 0xFF; d[2] = c & 0xFF; d[3] = 0xFF;
      }
    fwrite(fb, 1, sizeof fb, out);
  }
  fclose(out);
  if (aout) fclose(aout);
  for (int k = 0; k < npeek; ++k) printf("peek %08x = %04x\n", peek[k], (unsigned)core->busRead16(core, peek[k]));
  core->deinit(core);
  return 0;
}
