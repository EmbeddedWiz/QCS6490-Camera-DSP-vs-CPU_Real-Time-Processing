# Building the DSP half

The board runs `dsp/libpipe_hvx_skel.so`, a Hexagon shared object.  It is
checked in prebuilt because building it needs the Hexagon SDK and an x86 host;
this page is the exact recipe, so the file can be reproduced.

Measured with **Hexagon SDK 6.4.0.2**.  Two programs from the SDK do the work:
`qaic` (the IDL compiler) and `hexagon-clang`.

## 1. Generate the IDL bindings

```sh
SDK=/path/to/hexagon-sdk/6.4.0.2

"$SDK/ipc/fastrpc/qaic/bin/qaic" -m dll -mdll -o idl \
    -I "$SDK/incs" -I "$SDK/incs/stddef" \
    dsp/idl/pipe_hvx.idl
```

This writes three files into `idl/`:

- `pipe_hvx.h` — the interface, for the host and the DSP
- `pipe_hvx_stub.c` — the host side, linked into `build/pipeline`
- `pipe_hvx_skel.c` — the DSP side, compiled below

Only the first two are needed on the board; `pipe_hvx_skel.c` is an input to
the next step.

## 2. Build the skeleton

```sh
# The tools version folder depends on the SDK install; 6.4.0.2 ships 19.0.04.
CLANG="$SDK/tools/HEXAGON_Tools/19.0.04/Tools/bin/hexagon-clang"
QURT="$SDK/rtos/qurt/computev68/include/qurt"

"$CLANG" -mv68 -mhvx -mhvx-length=128B -G0 -O2 -Wall -Wextra \
    -fno-strict-aliasing -fPIC -shared \
    -Wl,-Bsymbolic -Wl,-soname,libpipe_hvx_skel.so \
    -I idl -I dsp -I "$SDK/incs" -I "$SDK/incs/stddef" -I "$QURT" \
    idl/pipe_hvx_skel.c dsp/agc.c dsp/demosaic.c dsp/filters.c \
    dsp/pipe_hvx_impl.c \
    -lc -o dsp/libpipe_hvx_skel.so
```

The toolchain directory inside `HEXAGON_Tools` changes between SDK releases;
pick the one that is there.

## 3. Put it in place

`build/pipeline` looks for the skeleton next to itself, in `build/dsp/`.  A
normal host build copies it there from `dsp/`, so nothing else is needed:

```sh
cmake --build build -j4
```

## Notes

- `-mv68` is the CDSP of the QCS6490; `-mhvx-length=128B` is what the kernels
  assume (128-byte vectors, 32 lanes of 32 bits).
- The kernels are compiled with `-fno-strict-aliasing` because the IDL stubs
  copy structures through `char*`.
- One session per process is deliberate: the CDSP on this board stops answering
  after a few dozen short-lived sessions, and the failure is an SMMU fault that
  does not heal by itself.
