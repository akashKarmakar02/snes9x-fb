# Snes9x — Twilight OS framebuffer port.
#
# Builds the Snes9x C++ core + the Twilight /dev/fb0 frontend into a single
# dynamically-linked executable. The core is C++; it must be compiled with a
# musl-native g++ so the result links against the musl libc + libstdc++ that
# ship in the Twilight rootfs (Alpine-built). The Fedora host's g++ targets
# glibc, so this Makefile is meant to be driven from inside the Alpine
# distrobox, where `g++` is musl-native and its C++ headers match the runtime.
#
# Build (from the repo root):
#   distrobox enter alpine -- make -C twilight
# or, from inside the alpine box:
#   make -C twilight
#
# No static linking — the binary uses /lib/ld-musl-x86_64.so.1 + libstdc++.so.

# --- Toolchain -------------------------------------------------------------
CXX      ?= g++
CC       ?= gcc

# --- Directories -----------------------------------------------------------
CORE_DIR := ..

# --- Flags -----------------------------------------------------------------
DEFINES  := -D__LIBRETRO__ -DHAVE_STDINT_H -DHAVE_STRINGS_H -DPIXEL_FORMAT=RGB565 \
            -DLSB_FIRST -DFAST_LSB_WORD_ACCESS

INCFLAGS := -I$(CORE_DIR) -I$(CORE_DIR)/apu -I$(CORE_DIR)/apu/bapu

CFLAGS   ?= -O2 -Wall -fno-exceptions -fno-rtti $(DEFINES) $(INCFLAGS)
CXXFLAGS ?= -O2 -Wall -std=c++17 -fno-exceptions -fno-rtti $(DEFINES) $(INCFLAGS)

LDLIBS   ?= -lm

# --- Sources ---------------------------------------------------------------
# Same core sources as the libretro backend (Makefile.common), minus
# libretro/libretro.cpp, plus the Twilight frontend.
CORE_SRCS := \
	$(CORE_DIR)/apu/apu.cpp \
	$(CORE_DIR)/apu/bapu/dsp/sdsp.cpp \
	$(CORE_DIR)/apu/bapu/smp/smp.cpp \
	$(CORE_DIR)/apu/bapu/smp/smp_state.cpp \
	$(CORE_DIR)/bsx.cpp \
	$(CORE_DIR)/c4.cpp \
	$(CORE_DIR)/c4emu.cpp \
	$(CORE_DIR)/cheats.cpp \
	$(CORE_DIR)/cheats2.cpp \
	$(CORE_DIR)/clip.cpp \
	$(CORE_DIR)/conffile.cpp \
	$(CORE_DIR)/controls.cpp \
	$(CORE_DIR)/cpu.cpp \
	$(CORE_DIR)/cpuexec.cpp \
	$(CORE_DIR)/cpuops.cpp \
	$(CORE_DIR)/crosshairs.cpp \
	$(CORE_DIR)/dma.cpp \
	$(CORE_DIR)/dsp.cpp \
	$(CORE_DIR)/dsp1.cpp \
	$(CORE_DIR)/dsp2.cpp \
	$(CORE_DIR)/dsp3.cpp \
	$(CORE_DIR)/dsp4.cpp \
	$(CORE_DIR)/fxinst.cpp \
	$(CORE_DIR)/fxemu.cpp \
	$(CORE_DIR)/gfx.cpp \
	$(CORE_DIR)/globals.cpp \
	$(CORE_DIR)/memmap.cpp \
	$(CORE_DIR)/obc1.cpp \
	$(CORE_DIR)/msu1.cpp \
	$(CORE_DIR)/ppu.cpp \
	$(CORE_DIR)/stream.cpp \
	$(CORE_DIR)/sa1.cpp \
	$(CORE_DIR)/sa1cpu.cpp \
	$(CORE_DIR)/screenshot.cpp \
	$(CORE_DIR)/sdd1.cpp \
	$(CORE_DIR)/sdd1emu.cpp \
	$(CORE_DIR)/seta.cpp \
	$(CORE_DIR)/seta010.cpp \
	$(CORE_DIR)/seta011.cpp \
	$(CORE_DIR)/seta018.cpp \
	$(CORE_DIR)/snapshot.cpp \
	$(CORE_DIR)/snes9x.cpp \
	$(CORE_DIR)/spc7110.cpp \
	$(CORE_DIR)/srtc.cpp \
	$(CORE_DIR)/tile.cpp \
	$(CORE_DIR)/tileimpl-n1x1.cpp \
	$(CORE_DIR)/tileimpl-n2x1.cpp \
	$(CORE_DIR)/tileimpl-h2x1.cpp \
	$(CORE_DIR)/sha256.cpp \
	$(CORE_DIR)/bml.cpp \
	$(CORE_DIR)/movie.cpp \
	$(CORE_DIR)/fscompat.cpp

C_SRCS    := $(CORE_DIR)/filter/snes_ntsc.c
TWI_SRCS  := twilight.cpp

SRCS      := $(TWI_SRCS) $(CORE_SRCS)
OBJS      := $(SRCS:.cpp=.o)
C_OBJS    := $(C_SRCS:.c=.o)

BIN       := snes9x

# --- Rules -----------------------------------------------------------------
.PHONY: all clean

all: $(BIN)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS) $(C_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(C_OBJS) $(LDLIBS)

clean:
	rm -f $(BIN) $(OBJS) $(C_OBJS)
