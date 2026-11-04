# QCS6490 Camera Pipeline: DSP vs CPU Real-Time Image Processing

A camera-based embedded vision project demonstrating streaming image processing on the Qualcomm QCS6490 platform, with alternative CPU and Hexagon DSP execution paths, interactive visualisation, and pixel-level output verification.

**Platform:** Radxa Dragon Q6A · Qualcomm QCS6490  
**Camera:** Raspberry Pi Camera Module with Sony IMX219 sensor  
**Processing:** Embedded Linux · V4L2 · C++20 · Qualcomm Hexagon CDSP · HVX · FastRPC  
**Display:** GStreamer · Wayland

---

## 1. Overview

This project explores how a camera-driven image-processing pipeline can be implemented on a heterogeneous embedded computing platform, using the CPU and the Qualcomm Hexagon Digital Signal Processor (DSP) as alternative execution engines.

The application captures image frames from an IMX219 camera, processes image data using a configurable pipeline, and displays the resulting video through GStreamer. The same processing operations can be executed using either a CPU implementation or a DSP implementation, allowing their output to be compared directly.

The primary objective is to demonstrate practical DSP integration in an embedded Linux application: processing streamed image data, invoking DSP code through the platform's remote execution interface, and validating the correctness of the resulting image.

Video provides a useful engineering workload for this purpose. It produces a continuous stream of structured data, exercises multiple image-processing operations, and makes the effects of individual algorithms immediately visible. Incorrect pixel transformations, unexpected output differences, and changes caused by filter parameters can be inspected directly in the displayed image.

Rather than presenting the project primarily as a numerical performance benchmark, this repository focuses on the implementation and visual verification of the processing pipeline.

## 2. Engineering Objectives

The project addresses several practical aspects of heterogeneous embedded software development.

### 2.1 DSP integration

The application integrates a Hexagon DSP processing path into a Linux-hosted application. The host application manages the camera pipeline, user interaction, and display, while DSP processing is accessed through Qualcomm's FastRPC mechanism.

The DSP implementation provides a practical example of moving computational work away from the application CPU and into a dedicated processing engine.

### 2.2 Equivalent CPU and DSP implementations

The project includes separate CPU and DSP processing paths intended to implement equivalent image-processing operations.

This makes it possible to inspect the same input frame under both execution modes and verify that the two implementations produce matching output for the supported operations and parameter settings.

The CPU implementation serves as a reference for functional comparison. The purpose is to validate algorithmic equivalence, not to assume that either implementation is correct without testing.

### 2.3 Streaming image processing

The application operates on camera frames as part of a live video pipeline rather than treating image processing exclusively as an offline task.

This provides a practical environment for exploring frame acquisition, processing, buffer handling, and display integration in an embedded Linux application.

### 2.4 Visual verification

The interactive interface makes the output of the processing pipeline observable in real time.

Users can enable and disable filters, change processing parameters, switch between CPU and DSP execution, and inspect the resulting images without relying exclusively on console logs or numerical reports.

### 2.5 A foundation for further performance analysis

The current project establishes a functional and visually verifiable processing pipeline. More extensive performance characterisation—including throughput under sustained load, frame latency distributions, deadline compliance, and resource contention—is a separate measurement task and is not claimed on the basis of visual operation alone.

---

## 3. System Architecture

The application connects camera acquisition, image processing, and video presentation into a single pipeline.

```text
                    Embedded Linux
                 Radxa Dragon Q6A
                  Qualcomm QCS6490
                          |
                          v
                  IMX219 Camera
                          |
                          v
                    CSI / V4L2
                          |
                          v
                  Frame Acquisition
                          |
                          v
                  Image Processing
                     /       \
                    /         \
                   v           v
             CPU Engine    Hexagon CDSP
             C++ Reference  HVX Processing
                   \           /
                    \         /
                     v       v
                  Output Frame
                          |
                          v
                  GStreamer Pipeline
                          |
                          v
                    Wayland Sink
                          |
                          v
                       Display
```

The diagram represents the logical processing flow. The precise memory ownership, buffer sharing, and transfer behaviour depend on the implementation of the relevant pipeline components.

### 3.1 Camera acquisition

The application uses the Linux Video4Linux2 (V4L2) interface to acquire frames from the camera device.

The camera supplies image data in the selected sensor mode. The application configures the capture path and obtains the frames required by the processing pipeline.

Supported resolutions and pixel formats depend on the camera sensor mode and the corresponding driver configuration.

### 3.2 Processing engine selection

The processing stage can use one of two implementations:

- **CPU engine:** performs the image-processing arithmetic in C++ on the application processor.
- **DSP engine:** invokes the corresponding DSP implementation on the Qualcomm Hexagon compute DSP through FastRPC.

The engine selection allows the same application and input data to be used for functional comparison without requiring two separate applications.

### 3.3 Video presentation

Processed frames are presented using GStreamer and a Wayland-compatible video sink.

The display path provides immediate visual feedback while the application continues to acquire and process frames.

### 3.4 Interactive control

The terminal-based interface provides controls for selecting filters, adjusting their strength, changing camera settings, and switching processing engines.

This makes the application suitable for live demonstrations and interactive investigation of the processing pipeline.

---

## 4. Image-Processing Pipeline

The application includes image preparation and processing stages intended to support camera input and configurable visual effects.

The following operations are described by their intended role in the pipeline. Their exact ordering, supported input formats, and execution location should be interpreted according to the implementation.

| Operation | Purpose |
|---|---|
| Black-level correction | Adjusts the image's baseline pixel level. |
| White balance | Adjusts colour-channel balance. |
| Demosaicing | Reconstructs colour pixels from Bayer-pattern sensor data. |
| Downscaling | Reduces image dimensions for subsequent processing or display. |
| Gamma adjustment | Applies a nonlinear intensity transformation. |
| Image filters | Applies configurable effects such as edge detection, sharpening, and colour transformations. |

Not every operation necessarily runs on both execution engines or supports every camera format. The supported combinations should be verified against the actual implementation.

### 4.1 Interactive filters

The application exposes the following filter selections:

| Key | Filter |
|---|---|
| `1` | Edge detection |
| `2` | Emboss |
| `3` | Posterization |
| `4` | Inversion |
| `5` | Pixelation |
| `6` | Sketch effect |
| `7` | Sepia |
| `8` | Sharpening |
| `0` | Disable all filters |

The `+` and `-` keys adjust the filter strength within the supported range of 1–6.

These operations provide a convenient collection of visual workloads for checking processing behaviour across the CPU and DSP paths.

---

## 5. CPU vs DSP: Functional Equivalence

A central feature of the project is the ability to compare the outputs of the two processing implementations.

Both engines are intended to implement the same arithmetic for the supported processing operations. Consequently, differences in their output can reveal implementation errors, inconsistencies in parameter handling, or differences in pixel-format interpretation.

The application includes a parity-check mode:

```bash
./build/pipeline \
    --replay frame.raw \
    --width 640 \
    --height 480 \
    --raw8 \
    --parity
```

The check compares the output produced by the CPU and DSP implementations for the selected input and supported filter configurations.

A reported result of zero differing pixels indicates that the compared output buffers matched for that test.

For example, the following result was reported for the supplied synthetic test frame:

```text
[parity] worst 0, bad 0 over 60 cases
```

This result documents the reported test outcome; it should not be interpreted as proof that all camera modes, pixel formats, filters, and parameter combinations have been exhaustively verified.

### 5.1 Reproducible input

A synthetic frame can be generated without a connected camera:

```bash
./build/make_frame frame.raw 640 480
```

The resulting file can then be replayed through the application.

This separates algorithmic verification from camera availability and makes it easier to reproduce a particular test input.

### 5.2 Captured-frame verification

The application also supports frame dumping and replay. A captured frame can be saved and processed again, allowing the same input to be examined under different processing configurations.

This is useful for investigating discrepancies that might otherwise be difficult to reproduce during live camera operation.

---

## 6. Hardware and Software Requirements

### 6.1 Reference hardware

The documented target configuration is:

- Radxa Dragon Q6A development board.
- Qualcomm QCS6490 system-on-chip.
- Raspberry Pi Camera Module using the Sony IMX219 sensor.
- Embedded Linux image with the required camera, DSP, and display support.

The DSP path depends on the availability of the appropriate Qualcomm runtime components and the prebuilt DSP skeleton.

### 6.2 Software dependencies

The host application requires the following development and runtime components:

- CMake 3.16 or later.
- A C++20-compatible compiler.
- GStreamer 1.20 or later, including the components required by the application and `waylandsink`.
- Qualcomm FastRPC runtime libraries, including `libcdsprpc`.
- A compatible Hexagon DSP runtime and firmware environment.
- A running Wayland session for graphical output.

Building the DSP skeleton from source additionally requires the appropriate Qualcomm Hexagon SDK and toolchain.

See [`docs/dsp-build.md`](docs/dsp-build.md) for the DSP build instructions.

The exact package names and installation procedures depend on the operating-system image used on the board.

---

## 7. Build Instructions

Build the host-side application from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The build produces the following executables:

```text
build/
├── pipeline
├── make_frame
└── dsp/
    └── libpipe_hvx_skel.so
```

The DSP skeleton is a prebuilt component in the documented configuration. The host build does not, by itself, replace the separate Hexagon SDK build process.

Before running the DSP engine, verify that the skeleton and the required runtime dependencies are available to the application.

---

## 8. Running the Application

### 8.1 Live camera mode

Start the application using the IMX219 camera:

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0

./build/pipeline \
    --raw8 \
    --width 640 \
    --height 480 \
    --sink wayland \
    --tui
```

The environment variables above are examples for a system using the specified Wayland runtime directory and display name. Adjust them to match the active graphical session.

The `--tui` option enables terminal-based controls while presenting the video in a separate window.

### 8.2 Selecting the CPU engine

To run the processing path on the CPU:

```bash
./build/pipeline \
    --engine cpu \
    --raw8 \
    --width 640 \
    --height 480 \
    --sink wayland \
    --tui
```

### 8.3 Synthetic-frame replay

Generate a test frame:

```bash
./build/make_frame frame.raw 640 480
```

Replay it through the pipeline:

```bash
./build/pipeline \
    --replay frame.raw \
    --width 640 \
    --height 480 \
    --raw8 \
    --sink wayland \
    --tui
```

This mode is useful when testing filter behaviour without a live camera.

### 8.4 Additional command-line options

| Option | Description |
|---|---|
| `--engine cpu` | Selects the CPU processing implementation. |
| `--no-agc` | Disables automatic gain control. |
| `--dump frame.rgba` | Saves an output frame for inspection. |
| `--seconds N` | Stops the application after the specified duration. |
| `--parity` | Compares CPU and DSP output for the supported test configurations. |
| `--replay frame.raw` | Uses a previously generated or captured frame as input. |

The supported combinations of command-line options depend on the input mode and the selected processing path.

---

## 9. Interactive Controls

The application provides a terminal interface for controlling the processing pipeline during operation.

| Key | Action |
|---|---|
| `1`–`8` | Select an image filter. |
| `0` | Disable all filters. |
| `+` / `-` | Adjust filter strength, from 1 to 6. |
| `a` | Enable or disable automatic gain control. |
| `r` | Select the next supported sensor resolution. |
| `s` | Select the next window scale, from 1:1 to 1:6. |
| `e` | Switch between DSP and CPU execution. |
| `h` | Display help. |
| `q` | Exit the application. |

When automatic gain control is disabled, the application freezes the gain at its current setting, according to the documented behaviour.

The scale control changes the display-processing scale using the DSP path as described by the application.

The engine-switching and configuration controls are subject to the lifetime restrictions of the DSP runtime described in the limitations section.

---

## 10. Performance Information

The application exposes processing information through its runtime panel, including frame rate and processing-call timing.

The currently documented reference configuration is:

| Parameter | Value |
|---|---|
| Image resolution | 640 × 480 |
| Input format | RAW8 |
| Advertised frame rate | 60 fps |
| Frames in flight | 1 |
| Reported DSP processing time | Approximately 7 ms per frame |

These values describe the reported configuration and should not be treated as independently verified end-to-end performance measurements.

In particular, an advertised or configured frame rate is not equivalent to a measured sustained frame rate. Likewise, the execution time of a processing call does not necessarily include sensor acquisition, buffer handling, scheduling, display synchronisation, or presentation latency.

The runtime panel distinguishes the reported processing-call timing from the frame-rate information. Its `lat` field represents the wall-clock duration of one `process()` call, while `latmax` records the maximum observed value during the current second.

A complete performance characterisation would require additional measurements, including:

- Sustained throughput under a defined workload.
- Processing latency distributions, including median, P95, and P99.
- End-to-end frame latency from acquisition to presentation.
- Frame drops and missed processing deadlines.
- CPU and DSP utilisation.
- Memory bandwidth and buffer-transfer overhead.
- Behaviour under thermal and resource contention.

Those measurements are outside the primary scope of this visualisation-oriented project.

---

## 11. Memory and Data-Flow Considerations

The application integrates V4L2 capture, processing, and GStreamer presentation. The implementation includes DMA-BUF-related buffer handling intended to support efficient data exchange between pipeline components.

DMA-BUF can allow compatible devices and subsystems to share references to the same underlying buffer without requiring a CPU copy of the image data at every stage.

However, DMA-BUF usage alone does not establish that an entire pipeline is zero-copy. The actual result depends on buffer import and export support, format compatibility, memory layout, alignment, and the requirements of individual processing and display components.

The same distinction applies to DSP processing: the memory used by the CPU, the DSP runtime, and the display subsystem must be compatible with the relevant interfaces and ownership rules.

For this reason, zero-copy operation should be considered an implementation property to verify rather than a universal guarantee inferred from the use of DMA-BUF.

---

## 12. Source Code Organisation

The principal components are organised as follows:

| Path | Responsibility |
|---|---|
| `src/v4l2_capture.*` | V4L2 frame acquisition and capture-buffer handling. |
| `src/route.*` | Media-graph discovery and configuration. |
| `src/sensor.*` | Sensor exposure and gain controls. |
| `src/dsp_engine.*` | FastRPC session management and DSP invocation. |
| `src/cpu_engine.*` | CPU-side image-processing implementation. |
| `src/sink.*` | Video-output integration with GStreamer and the display path. |
| `src/panel.*` | Terminal runtime panel and user interaction. |
| `dsp/` | Hexagon DSP kernels and related DSP-side components. |
| `idl/` | Interface definitions and generated host-side stubs. |
| `docs/dsp-build.md` | Instructions for building the DSP component. |

The host-side application coordinates the pipeline, while the DSP-side component provides the corresponding processing functionality exposed through the remote-call interface.

---

## 13. Known Limitations

The following limitations apply to the documented implementation.

### DSP session lifetime

The application intentionally uses a single DSP session. The current implementation has encountered stability limitations when repeatedly creating short-lived sessions on the target platform.

As a result, certain configuration changes—including engine switching, resolution changes, and display-scale changes—may require the application to restart rather than creating an additional DSP session.

### RAW8 and RAW10 modes

The application must use the correct pixel format for the selected camera mode.

In particular, the `--raw8` option is not appropriate for modes that supply packed 10-bit sensor data. The documented 1640 × 1232 and larger modes use RAW10 and must be configured accordingly.

RAW8 and RAW10 are not interchangeable memory layouts. Selecting the wrong format can lead to incorrect interpretation of the captured data.

### Frame dumps

The `--dump` option writes the output buffer in its in-memory RGBA representation. The resulting file is intended for debugging and pixel-level inspection rather than as a colour-managed photographic image.

### Platform dependencies

The DSP execution path depends on the target platform's Qualcomm runtime, firmware, and FastRPC integration. Reproducing the complete setup on another Linux system or SoC may require different runtime components and build procedures.

### Scope of performance validation

The project provides processing-call timing and runtime feedback, but it does not claim a complete real-time performance analysis or hard-deadline guarantee.

The reported parity test validates the tested outputs, not the correctness of every possible input, camera configuration, or execution condition.

---

## 14. Future Work

Potential extensions include:

- A dedicated benchmark suite for sustained CPU and DSP throughput.
- End-to-end frame-latency measurement and percentile reporting.
- Frame-drop detection and deadline-miss accounting.
- Instrumentation of buffer ownership, memory transfers, and DMA-BUF interoperability.
- Extended testing across supported sensor modes and pixel formats.
- Stress testing under CPU load, memory contention, and thermal constraints.
- Automated regression tests for CPU/DSP output equivalence.

These extensions would complement the current application by adding quantitative system-level evidence to the existing functional and visual demonstration.

---

## 15. Project Relevance

This project demonstrates practical work at the intersection of embedded Linux, camera-based image processing, and heterogeneous computing.

The main engineering areas are:

- C++ application development on embedded Linux.
- V4L2-based camera integration.
- Image-processing pipeline design.
- Qualcomm Hexagon DSP integration through FastRPC.
- CPU/DSP functional comparison.
- DMA-BUF-aware pipeline design.
- GStreamer-based video presentation.
- Interactive debugging and reproducible input testing.

For embedded camera platforms, the ability to connect data acquisition, specialised compute engines, and a working software pipeline is an important part of system integration.

The project provides a concrete demonstration of that work and a foundation for more detailed performance and system-level investigations.

---

## License

See [`LICENSE`](LICENSE) for licensing information.
