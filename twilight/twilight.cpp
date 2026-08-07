// Snes9x — Twilight OS framebuffer port.
//
// This is the Snes9x core (the same C++ sources used by the libretro backend)
// with a minimal /dev/fb0 + /dev/input/event0 frontend, matching the pattern
// used by the smolnes, fbdoom and chip8 ports in this OS.
//
// The libretro port (libretro/libretro.cpp) is the reference for the minimal
// set of frontend callbacks Snes9x requires. We implement those callbacks
// here, driving the core through:
//
//   Memory.Init() / S9xInitAPU()   — one-time setup
//   S9xGraphicsInit()             — allocate the 16bpp render buffer
//   S9xInitInputDevices()         — no-op (we push state via S9xReportButton)
//   S9xSetController(0, CTL_JOYPAD,...)
//   S9xUnmapAllControls() + S9xMapButton()  — wire keys to joypad buttons
//   Memory.LoadROM(path)           — load a .sfc/.smc ROM
//   S9xMainLoop()                  — run one frame; calls S9xDeinitUpdate()
//                                    with the rendered image in GFX.Screen
//
// Rendering: Snes9x renders into GFX.Screen as RGB565 (256x224 or 512x448).
// S9xDeinitUpdate() converts that to 32bpp 0x00RRGGBB, integer-scales and
// centers it on the Twilight framebuffer, then flushes with FBIOPAN_DISPLAY.
//
// Input: /dev/input/event0 is polled each frame; Linux keycodes are mapped to
// SNES joypad buttons and reported via S9xReportButton().
//
// Build: see twilight/Makefile. Compiled with musl-gcc (C) + g++ (C++) using
// the musl specs; links libstdc++ dynamically (present in the Twilight rootfs).

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "snes9x.h"
#include "memmap.h"
#include "gfx.h"
#include "ppu.h"
#include "controls.h"
#include "display.h"
#include "fscompat.h"
#include "apu/apu.h"
#include "conffile.h"

// ---------------------------------------------------------------------------
// Framebuffer + input device definitions (Linux-compatible ioctls, same as
// the fbdoom / smolnes / chip8 ports in this OS).
// ---------------------------------------------------------------------------

#define FB_PATH       "/dev/fb0"
#define KEYBOARD_PATH  "/dev/input/event0"
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY      0x4606
#define EV_KEY 1

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t bits_per_pixel;
    uint32_t red_offset;
    uint32_t green_offset;
    uint32_t blue_offset;
};

struct fb_fix_screeninfo {
    uint32_t line_length;
    uint32_t smem_len;
};

struct input_event {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

// ---------------------------------------------------------------------------
// Backend state
// ---------------------------------------------------------------------------

static int       fb_fd = -1;
static uint32_t *fb_ptr = NULL;
static uint32_t  screen_w = 0;
static uint32_t  screen_h = 0;
static size_t    fb_size = 0;
static int       kbd_fd = -1;

// RGB565 -> 0x00RRGGBB (32bpp) lookup, built once at startup. Snes9x renders
// into a 16-bit RGB565 buffer; the Twilight framebuffer is 32bpp 0x00RRGGBB.
static uint32_t rgb565_to_xrgb[65536];

static void build_rgb565_table(void) {
    for (int i = 0; i < 65536; i++) {
        int r5 = (i >> 11) & 0x1f;
        int g6 = (i >> 5)  & 0x3f;
        int b5 =  i        & 0x1f;
        uint8_t r8 = (r5 << 3) | (r5 >> 2);
        uint8_t g8 = (g6 << 2) | (g6 >> 4);
        uint8_t b8 = (b5 << 3) | (b5 >> 2);
        rgb565_to_xrgb[i] = ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
    }
}

static void fb_init(void) {
    fb_fd = open(FB_PATH, O_RDWR);
    if (fb_fd < 0) {
        perror("snes9x: open /dev/fb0");
        exit(1);
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    memset(&vinfo, 0, sizeof(vinfo));
    memset(&finfo, 0, sizeof(finfo));

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("snes9x: ioctl VSCREENINFO");
        exit(1);
    }
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("snes9x: ioctl FSCREENINFO");
        exit(1);
    }

    screen_w = vinfo.xres;
    screen_h = vinfo.yres;
    fb_size  = finfo.smem_len;

    fb_ptr = (uint32_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fb_fd, 0);
    if (fb_ptr == MAP_FAILED) {
        perror("snes9x: mmap framebuffer");
        exit(1);
    }

    memset(fb_ptr, 0, fb_size);
    ioctl(fb_fd, FBIOPAN_DISPLAY, 0);

    printf("snes9x: framebuffer %ux%u, %u bpp, %zu bytes\n",
           screen_w, screen_h, vinfo.bits_per_pixel, fb_size);
}

// Blit the rendered SNES frame from GFX.Screen (RGB565) into the 32bpp
// framebuffer, integer-scaled and centered, then flush to the display.
// `width`/`height` are the dimensions Snes9x rendered this frame (256x224
// for normal resolution, 512x448 for interlaced/hires).
static void fb_present(int width, int height) {
    const uint16 *src = GFX.Screen;
    uint32 src_pitch_pixels = GFX.PPL;   // pixels per row in GFX.Screen

    int scale = (int)screen_w / width;
    int sy    = (int)screen_h / height;
    if (sy < scale) scale = sy;
    if (scale < 1) scale = 1;

    int blit_w = width  * scale;
    int blit_h = height * scale;
    int off_x  = ((int)screen_w - blit_w) / 2;
    int off_y  = ((int)screen_h - blit_h) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    for (int y = 0; y < height; y++) {
        const uint16 *srow = src + y * src_pitch_pixels;
        for (int syy = 0; syy < scale; syy++) {
            int dst_y = off_y + y * scale + syy;
            if (dst_y < 0 || (uint32_t)dst_y >= screen_h)
                continue;
            uint32_t *drow = fb_ptr + dst_y * screen_w;
            for (int x = 0; x < width; x++) {
                uint32_t pixel = rgb565_to_xrgb[srow[x]];
                for (int sxx = 0; sxx < scale; sxx++) {
                    int dst_x = off_x + x * scale + sxx;
                    if (dst_x >= 0 && (uint32_t)dst_x < screen_w)
                        drow[dst_x] = pixel;
                }
            }
        }
    }

    ioctl(fb_fd, FBIOPAN_DISPLAY, 0);
}

// ---------------------------------------------------------------------------
// Input: map Linux input keycodes -> SNES joypad button IDs.
//
// Button IDs here are the IDs we pass to S9xMapButton()/S9xReportButton().
// We use one byte for the pad index (pad 0 = bits 0..7 of the high nibble)
// and the low nibble for the button, matching MAKE_BUTTON(pad, btn).
// ---------------------------------------------------------------------------

// Pad numbering for S9xMapButton: pad 1 is the first joypad.
#define TWI_PAD_1 1

// SNES button indices (match the libretro RETRO_DEVICE_ID_JOYPAD_* order used
// in libretro.cpp's map_buttons).
enum {
    TWI_B      = 0,
    TWI_Y      = 1,
    TWI_SELECT = 2,
    TWI_START  = 3,
    TWI_UP     = 4,
    TWI_DOWN   = 5,
    TWI_LEFT   = 6,
    TWI_RIGHT  = 7,
    TWI_A      = 8,
    TWI_X      = 9,
    TWI_L      = 10,
    TWI_R      = 11,
};

#define MAKE_BUTTON(pad, btn) (((pad) << 4) | (btn))

// Map a Linux input keycode to a (pad, button) pair. Returns -1 if unmapped.
struct keymap { uint16_t code; int pad; int btn; };
static const keymap kmap[] = {
    // D-pad
    { 103, TWI_PAD_1, TWI_UP },      // KEY_UP
    { 108, TWI_PAD_1, TWI_DOWN },    // KEY_DOWN
    { 105, TWI_PAD_1, TWI_LEFT },    // KEY_LEFT
    { 106, TWI_PAD_1, TWI_RIGHT },   // KEY_RIGHT
    // Face buttons: A/B/X/Y
    { 30,  TWI_PAD_1, TWI_B },       // KEY_Z  -> B (Nintendo B)
    { 44,  TWI_PAD_1, TWI_A },       // KEY_X  -> A (Nintendo A)
    { 31,  TWI_PAD_1, TWI_Y },       // KEY_S  -> Y
    { 45,  TWI_PAD_1, TWI_X },       // KEY_D  -> X
    // Shoulder buttons
    { 42,  TWI_PAD_1, TWI_L },       // KEY_LEFTSHIFT  -> L
    { 54,  TWI_PAD_1, TWI_R },       // KEY_RIGHTSHIFT -> R
    // Select / Start
    { 15,  TWI_PAD_1, TWI_SELECT },  // KEY_TAB   -> Select
    { 28,  TWI_PAD_1, TWI_START },   // KEY_ENTER -> Start
};

static void kbd_poll(void) {
    struct input_event ev;
    for (;;) {
        ssize_t n = read(kbd_fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev))
            return;
        if (ev.type != EV_KEY)
            continue;
        // value: 1 = press, 0 = release, 2 = repeat (treat as press)
        bool pressed = ev.value != 0;
        for (const auto &m : kmap) {
            if (m.code == ev.code) {
                S9xReportButton(MAKE_BUTTON(m.pad, m.btn), pressed);
                break;
            }
        }
    }
}

static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

// ---------------------------------------------------------------------------
// Frontend callbacks required by the Snes9x core.
//
// These mirror the stubs in libretro/libretro.cpp but drive the Twilight
// framebuffer instead of the retro_video_refresh_t callback.
// ---------------------------------------------------------------------------

// Called by the core after a frame has been rendered into GFX.Screen.
// `width`/`height` are the rendered dimensions this frame.
bool8 S9xDeinitUpdate(int width, int height) {
    fb_present(width, height);
    return TRUE;
}

bool8 S9xContinueUpdate(int width, int height) {
    return S9xDeinitUpdate(width, height);
}

bool8 S9xInitUpdate(void) {
    return TRUE;
}

// Called once per frame by the core (from S9xMainLoop, after
// S9xEndScreenRefresh). We use it to pace to ~60 FPS.
void S9xSyncSpeed(void) {
    static uint64_t last = 0;
    const uint64_t frame_ms = Settings.PAL ? (1000 / 50) : (1000 / 60);

    uint64_t now = ms_now();
    if (last) {
        uint64_t elapsed = now - last;
        if (elapsed < frame_ms) {
            struct timespec ts;
            ts.tv_sec  = (frame_ms - elapsed) / 1000;
            ts.tv_nsec = ((frame_ms - elapsed) % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
    }
    last = ms_now();
    IPPU.RenderThisFrame = TRUE;
}

// --- Stubs / minimal implementations ---------------------------------------

void S9xInitInputDevices(void) {}
void S9xHandlePortCommand(s9xcommand_t, short, short) {}
bool S9xPollButton(unsigned int, bool *) { return false; }
bool S9xPollAxis(unsigned int, short *)  { return FALSE; }
bool S9xPollPointer(unsigned int, short *, short *) { return false; }
void S9xToggleSoundChannel(int) {}
void S9xExtraUsage(void) {}
void S9xParseArg(char **, int &, int) {}
void S9xParsePortConfig(ConfigFile &, int) {}
const char *S9xStringInput(const char *in) { return in; }
const char *S9xBasename(const char *in) { return in; }
bool8 S9xOpenSoundDevice(void) { return TRUE; }
void S9xExit(void) {}

void S9xMessage(int type, int, const char *s) {
    switch (type) {
    case S9X_DEBUG:   fprintf(stderr, "[snes9x] %s\n", s); break;
    case S9X_WARNING: fprintf(stderr, "[snes9x] WARN: %s\n", s); break;
    case S9X_INFO:    fprintf(stderr, "[snes9x] %s\n", s); break;
    case S9X_ERROR:   fprintf(stderr, "[snes9x] ERROR: %s\n", s); break;
    default:          fprintf(stderr, "[snes9x] %s\n", s); break;
    }
}

std::string S9xGetDirectory(s9x_getdirtype type) {
    (void)type;
    return std::string(".");
}

std::string S9xGetFilenameInc(std::string in, s9x_getdirtype) {
    return std::string("snes9x") + in;
}

bool8 S9xOpenSnapshotFile(const char *filepath, bool8 read_only, STREAM *file) {
    if (read_only) {
        if ((*file = OPEN_STREAM(filepath, "rb")) != 0)
            return TRUE;
    } else {
        if ((*file = OPEN_STREAM(filepath, "wb")) != 0)
            return TRUE;
    }
    return FALSE;
}

void S9xCloseSnapshotFile(STREAM file) {
    CLOSE_STREAM(file);
}

void S9xAutoSaveSRAM(void) {}

// ---------------------------------------------------------------------------
// Map keyboard buttons to SNES joypad 1 commands.
// ---------------------------------------------------------------------------

static void map_buttons(void) {
    static const struct { int btn; const char *name; } pad1[] = {
        { TWI_A,      "Joypad1 A" },
        { TWI_B,      "Joypad1 B" },
        { TWI_X,      "Joypad1 X" },
        { TWI_Y,      "Joypad1 Y" },
        { TWI_SELECT, "Joypad1 Select" },
        { TWI_START,  "Joypad1 Start" },
        { TWI_L,      "Joypad1 L" },
        { TWI_R,      "Joypad1 R" },
        { TWI_LEFT,   "Joypad1 Left" },
        { TWI_RIGHT,  "Joypad1 Right" },
        { TWI_UP,     "Joypad1 Up" },
        { TWI_DOWN,   "Joypad1 Down" },
    };
    for (const auto &b : pad1)
        S9xMapButton(MAKE_BUTTON(TWI_PAD_1, b.btn),
                     S9xGetCommandT(b.name), false);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom.sfc> [rom2]\n", argv[0]);
        return 1;
    }

    // --- Default settings (mirrors libretro's retro_init) -----------------
    memset(&Settings, 0, sizeof(Settings));
    Settings.SuperFX = TRUE;
    Settings.DSP = 1;
    Settings.SA1 = TRUE;
    Settings.C4 = TRUE;
    Settings.SDD1 = TRUE;
    Settings.SPC7110 = TRUE;
    Settings.SPC7110RTC = TRUE;
    Settings.OBC1 = TRUE;
    Settings.SETA = TRUE;
    Settings.SRTC = TRUE;
    Settings.BS = TRUE;
    Settings.BSXItself = TRUE;
    Settings.MSU1 = TRUE;
    Settings.MouseMaster = TRUE;
    Settings.JustifierMaster = TRUE;
    Settings.MultiPlayer5Master = TRUE;
    Settings.MacsRifleMaster = TRUE;

    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.SixteenBitSound = TRUE;
    Settings.Stereo = TRUE;
    Settings.SoundPlaybackRate = 32040;
    Settings.SoundInputRate = 32040;
    Settings.Transparency = TRUE;
    Settings.AutoDisplayMessages = TRUE;
    Settings.InitialInfoStringTimeout = 120;
    Settings.HDMATimingHack = 100;
    Settings.BlockInvalidVRAMAccessMaster = TRUE;
    Settings.SeparateEchoBuffer = FALSE;
    Settings.CartAName[0] = 0;
    Settings.CartBName[0] = 0;
    Settings.AutoSaveDelay = 1;
    Settings.DontSaveOopsSnapshot = TRUE;

    // Fields that the config-file path (snes9x.cpp) and libretro's
    // update_variables() set to non-zero defaults. We memset Settings to 0,
    // so restore them here. MaxSpriteTilesPerLine is the critical one: at 0
    // every sprite tile is flagged over-budget and skipped (gfx.cpp DrawOBJS),
    // leaving only backgrounds visible.
    Settings.MaxSpriteTilesPerLine = 34;   // SNES hardware sprite budget
    Settings.SuperFXClockMultiplier = 100; // SuperFX runs at 1x
    Settings.OverclockMode = 0;            // no CPU overclock
    Settings.OneClockCycle     = 6;
    Settings.OneSlowClockCycle = 8;
    Settings.TwoClockCycles    = 12;
    Settings.InterpolationMethod = DSP_INTERPOLATION_GAUSSIAN;
    Settings.SoundPlaybackRate = 32040;

    CPU.Flags = 0;

    if (!Memory.Init() || !S9xInitAPU()) {
        Memory.Deinit();
        S9xDeinitAPU();
        fprintf(stderr, "snes9x: failed to init Memory or APU.\n");
        return 1;
    }

    S9xInitSound(32);
    S9xSetSoundMute(TRUE);   // no audio device in this port yet
    S9xSetSamplesAvailableCallback(NULL, NULL);

    S9xGraphicsInit();

    S9xInitInputDevices();
    S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);

    S9xUnmapAllControls();
    map_buttons();

    // --- Load the ROM -----------------------------------------------------
    if (!Memory.LoadROM(argv[1])) {
        fprintf(stderr, "snes9x: failed to load ROM '%s'\n", argv[1]);
        return 1;
    }
    Memory.ClearSRAM();

    printf("snes9x: loaded '%s' (%s)\n", argv[1],
           Settings.PAL ? "PAL" : "NTSC");

    // --- Init the Twilight OS framebuffer + keyboard ----------------------
    build_rgb565_table();
    fb_init();
    kbd_fd = open(KEYBOARD_PATH, O_RDONLY | O_NONBLOCK);
    if (kbd_fd < 0) {
        perror("snes9x: open /dev/input/event0");
        return 1;
    }

    // --- Main loop: run one frame, poll input, present, repeat ------------
    for (;;) {
        kbd_poll();
        IPPU.RenderThisFrame = TRUE;
        S9xMainLoop();
    }

    // Unreachable in this port.
    return 0;
}
