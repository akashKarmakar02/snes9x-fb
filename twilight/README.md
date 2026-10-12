# Twilight OS framebuffer frontend

This fork contains the Snes9x core and the Twilight `/dev/fb0` frontend in
`twilight.cpp`. The frontend reads keyboard input from `/dev/input/event0` directly.
Upstream licensing remains in `LICENSE` and the original source headers.
ROMs and generated executables are not included.

Twilight OS pins this repository as `userspace/snes9x-fb`. Its
`userspace/apps/snes9x/Makefile` delegates here through an Alpine distrobox,
which supplies musl-native C++ tools matching the guest runtime.

Build from this repository's root:

```sh
distrobox enter alpine -- env CC=gcc CXX=g++ make -C twilight
```

The guest requires the musl loader and libstdc++ shared runtime. The output
is `twilight/snes9x`; run it with a legally obtained SNES ROM.

Commit and push frontend changes before updating the Twilight OS gitlink.
The initial submodule integration preserves the frontend from commit
`44a8fb8d3710c13e80a12d2005abb920be33cc5e` without source changes.

The Twilight branch omits upstream's nested shader, audio-backend, and
Windows library submodules (SPIRV-Cross, glslang, Vulkan-Headers, cubeb,
libpng, zlib). The standalone framebuffer Makefile uses none of them;
recursive OS checkouts only need the Snes9x core and this frontend.
