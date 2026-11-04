# Building the Hexagon DSP Component

## 1. Overview

The QCS6490 Camera DSP vs CPU project uses a prebuilt Hexagon DSP shared object:

`dsp/libpipe_hvx_skel.so`

The host application invokes this component through Qualcomm FastRPC. The DSP skeleton contains the DSP-side implementation and the generated interface code required to expose the processing functions to the host.

The binary is included in the repository so that the host application can be built on the target development board without requiring the complete Hexagon SDK toolchain.

Rebuilding the DSP component is a separate process that requires:

- A compatible Qualcomm Hexagon SDK installation.
- An x86 development host supported by that SDK release.
- The `qaic` IDL compiler.
- The `hexagon-clang` compiler and associated linker tools.
- The DSP source files and FastRPC interface definition included in this repository.

The commands below document the build procedure used with **Qualcomm Hexagon SDK 6.4.0.2**. Toolchain paths may differ between SDK installations.

## 2. Build Process

The build consists of three stages:

1. Generate the FastRPC interface bindings from the IDL definition.
2. Compile the DSP skeleton and processing kernels into a shared object.
3. Place the resulting library where the host application expects to find it.

The host-side application is built separately using CMake.

## 3. Generate the FastRPC Interface

The interface definition is located at:

`dsp/idl/pipe_hvx.idl`

The Qualcomm `qaic` compiler generates the C interface and the host and DSP glue code required for the remote-call interface.

Set the SDK path and run the compiler:

```bash
SDK=/path/to/hexagon-sdk/6.4.0.2

"$SDK/ipc/fastrpc/qaic/bin/qaic" \
    -m dll \
    -mdll \
    -o idl \
    -I "$SDK/incs" \
    -I "$SDK/incs/stddef" \
    dsp/idl/pipe_hvx.idl
```

The expected generated files are:

| File | Purpose |
|---|---|
| `idl/pipe_hvx.h` | Interface declarations. |
| `idl/pipe_hvx_stub.c` | Host-side FastRPC stub, linked into `pipeline`. |
| `idl/pipe_hvx_skel.c` | DSP-side skeleton, compiled into the shared object. |

The host application requires the interface header and host stub. The DSP build uses the generated skeleton together with the DSP implementation sources.

If the SDK version changes, verify the `qaic` command-line options and generated output layout before reusing this procedure.

## 4. Compile the DSP Skeleton

The DSP shared object is compiled using the Hexagon compiler provided by the SDK.

For the documented SDK installation, the compiler path is:

```bash
SDK=/path/to/hexagon-sdk/6.4.0.2

CLANG="$SDK/tools/HEXAGON_Tools/8.8.06/Tools/bin/hexagon-clang"
QURT="$SDK/rtos/qurt/computev68/include/qurt"
```

The `HEXAGON_Tools` version directory may differ between installations. Check the actual SDK contents and select a compatible compiler.

Build the shared object:

```bash
"$CLANG" \
    -mv68 \
    -mhvx \
    -mhvx-length=128B \
    -G0 \
    -O2 \
    -Wall \
    -Wextra \
    -fno-strict-aliasing \
    -fPIC \
    -shared \
    -Wl,-Bsymbolic \
    -Wl,-soname,libpipe_hvx_skel.so \
    -I idl \
    -I dsp \
    -I "$SDK/incs" \
    -I "$SDK/incs/stddef" \
    -I "$QURT" \
    idl/pipe_hvx_skel.c \
    dsp/agc.c \
    dsp/demosaic.c \
    dsp/filters.c \
    dsp/pipe_hvx_impl.c \
    -lc \
    -o dsp/libpipe_hvx_skel.so
```

### Compiler options

| Option | Purpose |
|---|---|
| `-mv68` | Selects the documented Hexagon v68 target. |
| `-mhvx` | Enables Hexagon Vector eXtensions. |
| `-mhvx-length=128B` | Configures the 128-byte HVX vector length expected by the kernels. |
| `-G0` | Uses the specified small-data configuration. |
| `-O2` | Enables compiler optimisation. |
| `-fno-strict-aliasing` | Disables strict-aliasing assumptions for code that relies on compatible low-level memory access patterns. |
| `-fPIC` | Generates position-independent code. |
| `-shared` | Produces a shared library. |
| `-Wl,-Bsymbolic` | Requests local binding of references to symbols defined within the shared object. |
| `-Wl,-soname,libpipe_hvx_skel.so` | Sets the shared object's soname. |

The 128-byte HVX vector configuration is part of the assumptions made by the DSP kernels. If the vector configuration or target architecture changes, the implementation must be reviewed and rebuilt accordingly.

The `-fno-strict-aliasing` option is retained from the documented build because the generated interface code and low-level data handling require compatibility with the SDK's generated implementation.

## 5. Install the Build Output

The host application looks for the DSP skeleton in a directory relative to the executable:

`build/dsp/libpipe_hvx_skel.so`

The normal CMake build copies the prebuilt library from the repository into this location:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

After rebuilding the DSP skeleton, the resulting file should be present at:

`dsp/libpipe_hvx_skel.so`

The normal CMake build can then copy it into the build directory.

If the host application is already running, restart it after replacing the shared object.

## 6. Host-Side Build Requirements

Building the Linux host application requires a separate set of dependencies:

- A C++20-compatible compiler.
- CMake 3.16 or later.
- GStreamer development packages.
- FastRPC development headers and the `cdsprpc` library.
- A compatible Linux environment and Qualcomm runtime on the target board.

The generated host stub, `idl/pipe_hvx_stub.c`, is compiled as part of the `pipeline` executable.

The host application build does not invoke `hexagon-clang` automatically. The DSP skeleton is a separately built artifact.

## 7. Troubleshooting

### `qaic` is not found

Verify the SDK installation and confirm that the executable exists at the expected path:

```bash
ls "$SDK/ipc/fastrpc/qaic/bin/qaic"
```

### `hexagon-clang` is not found

Check the installed toolchain directories:

```bash
find "$SDK/tools/HEXAGON_Tools" \
    -type f \
    -name hexagon-clang
```

Use the compiler supplied by the installed SDK and verify that it supports the target and HVX options used by the project.

### Generated IDL files are missing

Check that the `qaic` command completed successfully and that the `-o idl` output directory is writable.

Confirm that the interface source exists:

```bash
ls dsp/idl/pipe_hvx.idl
```

### The host application cannot locate the skeleton

Verify the expected file:

```bash
ls -l build/dsp/libpipe_hvx_skel.so
```

If necessary, rerun the CMake build to execute the post-build copy step.

### DSP session failures

The current application uses one DSP session per process because repeated creation of short-lived sessions has been observed to cause failures on the reference platform.

Rebuilding the skeleton does not, by itself, resolve runtime or firmware-level session-lifetime issues.

## 8. Reproducibility Notes

The documented build was measured with Hexagon SDK 6.4.0.2 and the corresponding Hexagon toolchain.

For reproducible builds, record the following information when changing the toolchain:

- Qualcomm Hexagon SDK version.
- Hexagon compiler version and target architecture.
- HVX vector configuration.
- `qaic` version.
- Host-side compiler and CMake versions.
- Exact source revision used to build the skeleton.

Changes in the SDK, compiler, generated bindings, or target configuration may require adjustments to the build procedure.

## 9. Summary

The DSP component is built independently from the Linux host application. The `qaic` compiler generates the FastRPC interface bindings, and `hexagon-clang` compiles the DSP skeleton and processing kernels into `libpipe_hvx_skel.so`.

Keeping these stages separate makes the target-side application build simpler while preserving a documented path for rebuilding the Hexagon component from source.
