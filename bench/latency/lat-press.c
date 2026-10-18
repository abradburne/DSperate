// lat-press: one button press on the DS+, timed where it enters and where pocket-input passes it on.
//
//   sudo lat-press KEY [HOLD_MS]        KEY: a, b, x, y, l, r, start, select, up, down, left, right
//
// Writes the key down (and up HOLD_MS later, default 100) into gpio-keys-control, the kernel's
// device for the buttons. pocket-input has that device grabbed, but a write to an evdev node goes in
// through the device's own handle, which is the grab's, so pocket-input reads it as it reads a real
// press (only the GPIO interrupt and gpio-keys' debounce are left out). Meanwhile it reads the pad
// pocket-input makes ("Anbernic RG DS+ Gamepad", uinput) and takes the kernel's timestamp of the
// first event there after each write. Prints, CLOCK_MONOTONIC ns:
//
//   down <written> <on the pad>
//   up <written> <on the pad>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int open_named(const char *want, int flags) {
    char path[64], name[256];
    for (int i = 0; i < 64; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, flags);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && strcmp(name, want) == 0) return fd;
        close(fd);
    }
    fprintf(stderr, "lat-press: no input device named %s\n", want);
    exit(1);
}

static void put(int fd, int type, int code, int value) {
    struct input_event e = {.type = type, .code = code, .value = value};
    if (write(fd, &e, sizeof e) != sizeof e) { perror("lat-press: write"); exit(1); }
}

// The pad's first event at or after `since`, by the kernel's stamp; 0 after 200 ms of nothing.
static long long pad_event(int pad, long long since) {
    struct input_event e;
    for (;;) {
        struct pollfd p = {.fd = pad, .events = POLLIN};
        if (poll(&p, 1, 200) <= 0) return 0;
        while (read(pad, &e, sizeof e) == sizeof e) {
            long long t = e.time.tv_sec * 1000000000LL + e.time.tv_usec * 1000LL;
            if (e.type != EV_SYN && t >= since - 1000000) return t;
        }
    }
}

int main(int argc, char **argv) {
    static const struct { const char *name; int code; } keys[] = {
        {"a", BTN_EAST}, {"b", BTN_SOUTH}, {"x", BTN_NORTH}, {"y", BTN_WEST}, {"l", BTN_TL}, {"r", BTN_TR},
        {"start", BTN_START}, {"select", BTN_SELECT}, {"up", BTN_DPAD_UP}, {"down", BTN_DPAD_DOWN},
        {"left", BTN_DPAD_LEFT}, {"right", BTN_DPAD_RIGHT},
    };
    if (argc < 2) { fprintf(stderr, "usage: lat-press KEY [HOLD_MS]\n"); return 2; }
    int code = -1;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) if (!strcmp(argv[1], keys[i].name)) code = keys[i].code;
    if (code < 0) { fprintf(stderr, "lat-press: unknown key %s\n", argv[1]); return 2; }
    int hold_ms = argc > 2 ? atoi(argv[2]) : 100;

    int pad = open_named("Anbernic RG DS+ Gamepad", O_RDONLY | O_NONBLOCK);
    int clk = CLOCK_MONOTONIC;
    if (ioctl(pad, EVIOCSCLOCKID, &clk) < 0) { perror("lat-press: EVIOCSCLOCKID"); return 1; }
    int keys_fd = open_named("gpio-keys-control", O_WRONLY);
    struct input_event drain;
    while (read(pad, &drain, sizeof drain) == sizeof drain) {}

    for (int value = 1; value >= 0; value--) {
        long long t = now_ns();
        put(keys_fd, EV_KEY, code, value);
        put(keys_fd, EV_SYN, SYN_REPORT, 0);
        long long p = pad_event(pad, t);
        printf("%s %lld %lld\n", value ? "down" : "up", t, p);
        fflush(stdout);
        if (value) usleep(hold_ms * 1000);
    }
    return 0;
}
