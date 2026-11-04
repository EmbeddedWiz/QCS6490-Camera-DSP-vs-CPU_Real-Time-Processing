# QCS6490 Camera: DSP vs CPU, Real-Time Processing

An IMX219 frame is captured with V4L2, developed and filtered **either on the
Hexagon CDSP (HVX) or on the CPU**, and presented through GStreamer without a
single copy on the way.  Both engines implement the same arithmetic, pixel for
pixel, so the difference between them is never the picture — only the time it
takes.

That parity is the point of the project.  The CPU path is the reference: if the
two renders of the same frame differ by one level, the DSP kernel is wrong, and
the check says so.

```
  camera (CSI)          dma-buf            dma-buf            dma-buf
  /dev/video9  ──►  DSP engine  or  ──►  waylandsink  ──►  screen
                    CPU engine
```

No `memcpy`, no conversion, no intermediate buffer: the pages the sensor wrote
are the pages the engine reads and the compositor shows.

## What it does

- Captures RAW8/RAW10 from the IMX219 at up to 3280×2464 and runs the whole
  image pipeline — black level, white balance, demosaic, box downscale, gamma,
  then the filters — on one of the two engines.
- Measures what the panel shows: per-frame engine time, fps, and the exposure
  state of the automatic gain loop.
- Verifies itself: `--parity` renders the same frame on both engines and
  compares every byte.

## Hardware and software

Tested on a Radxa Dragon Q6A (Qualcomm QCS6490) with the Raspberry Pi camera
module (IMX219).  The DSP half is a prebuilt Hexagon skeleton in
`dsp/libpipe_hvx_skel.so`; building it needs the Hexagon SDK, see
[docs/dsp-build.md](docs/dsp-build.md).

The host needs:

- CMake ≥ 3.16 and a C++20 compiler
- GStreamer 1.20+ with `waylandsink` (the app, video and allocators libraries)
- the FastRPC libraries (`libcdsprpc`), which come with the board image
- a Wayland session (`XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` set)

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

This produces `build/pipeline`, `build/make_frame`, and puts the Hexagon
skeleton where the binary looks for it (`build/dsp/`).

## Run

```sh
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
./build/pipeline --raw8 --width 640 --height 480 --sink wayland --tui
```

`--tui` puts the controls in the terminal and the picture in its own window.

| key | |
|---|---|
| `1`–`8` | edge, emboss, posterize, invert, pixelate, sketch, sepia, sharpen |
| `0` | all filters off |
| `+` / `-` | filter strength, 1–6 |
| `a` | AGC on/off (off freezes the gain it settled on) |
| `r` | next sensor resolution |
| `s` | next window scale (1:1 … 1:6, done on the DSP) |
| `e` | switch engine, DSP ↔ CPU |
| `h` | help |
| `q` | quit |

Useful options: `--engine cpu` renders on the CPU, `--no-agc` fixes the
exposure, `--dump frame.rgba` saves one output frame, `--seconds N` stops on
its own.

## The parity check

`--parity` needs a frame.  `make_frame` writes a synthetic one, so the numbers
can be reproduced without the camera:

```sh
./build/make_frame frame.raw 640 480
./build/pipeline --replay frame.raw --width 640 --height 480 --raw8 --parity
```

On the reference frame above, every filter at every strength:

```
[parity] worst 0, bad 0 over 60 cases
```

A real capture works the same way: `--dump` writes the raw frame next to the
RGBA output, and that `.raw` file can be fed back through `--replay`.

## Numbers

640×480, RAW8, one frame in flight, measured on the board over one second:

| | DSP | CPU |
|---|---|---|
| engine time per frame | 6.3 ms | 10.7 ms |
| fps | 61 | 59 |

The panel's `lat` is the wall time of one `process()` call, `latmax` the worst
in the current second.

## Layout

| | |
|---|---|
| `src/v4l2_capture.*` | V4L2 buffers, exported as dma-buf, woken by the CSI interrupt |
| `src/route.*` | the media graph, walked and set up in C++ |
| `src/sensor.*` | exposure and gain, the two knobs that decide how much light a frame carries |
| `src/dsp_engine.*` | the FastRPC session and the DSP call |
| `src/cpu_engine.*` | the same arithmetic, in scalar C++ |
| `src/sink.*` | dma-buf → `waylandsink` |
| `src/panel.*` | the terminal panel |
| `dsp/` | the Hexagon kernels and the IDL |
| `idl/` | generated host stubs for the IDL |

## Style

The sources follow the `.clang-format` file in this directory: 4 spaces, 80
columns, braces on their own line for functions, attached everywhere else.
To reformat everything after a change:

```sh
clang-format -i src/*.cpp src/*.h dsp/*.c dsp/*.h tools/*.cpp
```

## Limitations

- One process, one DSP session on purpose: the CDSP on this board does not
  survive hundreds of short-lived sessions, so `e`/`r`/`s` restart the process
  rather than open a second session.
- `--raw8` on a 10-bit sensor mode reads the wrong bytes; the 1640×1232 and
  larger modes are RAW10.
- `--dump` writes the RGBA surface as it sits in memory and is meant for
  debugging, not for colour-accurate stills.

## License

See [LICENSE](LICENSE).
