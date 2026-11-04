
# QCS6490 Camera Pipeline: CPU vs Hexagon DSP Real-Time Image Processing

A real-time embedded camera application demonstrating image acquisition, processing, and visualisation on the Qualcomm QCS6490 heterogeneous computing platform. The project integrates a complete camera pipeline with two alternative processing engines: the application CPU and the Qualcomm Hexagon Compute DSP (CDSP/HVX).

The application supports multiple camera resolutions and Bayer input formats, interactive image filters, configurable processing parameters, display scaling, automatic gain control, and runtime switching between CPU and DSP processing.

**Platform:** Radxa Dragon Q6A · Qualcomm QCS6490  
**Camera:** Sony IMX219 MIPI CSI-2 Camera Module  
**Operating system:** Embedded Linux  
**Camera interface:** MIPI CSI-2 · D-PHY · V4L2 · Media Controller API  
**Programming language:** C++20  
**Processing:** CPU · Qualcomm Hexagon CDSP · HVX · FastRPC  
**Memory and buffers:** DMA-BUF · RPC Memory  
**Video output:** GStreamer · Wayland

---

## 1. Project Overview

This project implements a complete real-time camera processing application for the Qualcomm QCS6490 platform.

It combines camera acquisition, sensor configuration, image preparation, digital image processing, hardware-accelerated execution, and live video presentation in one integrated application.

A central feature is the availability of two alternative processing engines:

- **CPU:** image processing implemented in C++ and executed on the application processor.
- **Hexagon DSP:** corresponding image processing executed on the Qualcomm Hexagon CDSP using HVX vector instructions and FastRPC.

Both processing paths provide the same application functionality. The user can switch between CPU and DSP execution while working with the camera, selecting resolutions, changing image filters, adjusting filter strength, and modifying display scaling.

The application is not limited to a single camera mode or a fixed processing configuration. Its controls allow the camera and image-processing pipeline to be explored interactively across the supported operating modes.

The project focuses on practical heterogeneous embedded software engineering: integrating a camera subsystem, managing image buffers, implementing processing algorithms for different compute architectures, and verifying the resulting images.

## 2. Main Features

### 2.1 Camera integration

- Sony IMX219 camera sensor integration.
- MIPI CSI-2 camera acquisition over D-PHY.
- Linux V4L2 capture interface.
- Linux Media Controller API integration.
- Automatic camera media-graph discovery and configuration.
- Four supported camera resolutions, from VGA to the full 8-megapixel sensor resolution.
- Bayer RGGB RAW8 and packed RAW10 capture formats.
- Runtime camera resolution selection.
- Sensor exposure and analogue gain controls.
- Automatic Gain Control (AGC), with runtime enable/disable control.

### 2.2 Image processing

- Complete Bayer-to-RGBA image-processing pipeline.
- CPU and Hexagon DSP implementations.
- RAW8 and packed RAW10 input processing.
- Bayer unpacking and preparation.
- Black-level correction.
- Software-controlled digital gain.
- Bilinear demosaicing.
- White balance.
- Gamma correction.
- Box-filter downscaling.
- Eight interactive image filters.
- Adjustable filter strength.
- Multiple filters can be enabled simultaneously.
- Consistent processing functionality across all supported camera resolutions and scaling configurations.

### 2.3 Runtime configuration

- Runtime CPU/DSP processing-engine selection.
- Runtime camera-resolution selection.
- Runtime display-scaling selection.
- Interactive filter selection and combination.
- Adjustable filter strength.
- AGC enable/disable.
- Sensor exposure and analogue gain configuration.
- Live terminal control and status information.

### 2.4 Output and verification

- Live video output through GStreamer and Wayland.
- CPU/DSP output comparison.
- Pixel-level parity checking.
- Automated filter and strength test matrix.
- Synthetic-frame generation and replay.
- Captured-frame dumping for offline inspection.
- Runtime frame-rate and processing-time information.
- Input and output memory-bandwidth statistics.

## 3. Supported Camera Modes

### Camera Modes and Processing Compatibility

The application provides full support for all four tested IMX219 camera resolutions:

- 640 × 480 (VGA)
- 1640 × 1232
- 1920 × 1080 (Full HD)
- 3280 × 2464 (Full sensor resolution, 8 MP)

All supported camera modes and pixel formats, including Bayer formats, are fully integrated into the processing pipeline.

The complete functionality is available in both CPU and Hexagon DSP processing modes:

- All implemented image filters
- All available display scaling options
- Camera mode and resolution selection
- Supported Bayer and other camera pixel formats
- Automatic camera gain and light-level adjustment controls

All these features work with both CPU and DSP processing paths across the supported camera resolutions. No filter or scaling option is restricted to a particular resolution or processing mode.

The complete set of implemented image filters and all available display-scaling options work across these camera resolutions. No implemented filter or scaling option is restricted to a particular resolution or processing engine.

### 3.1 Camera pixel formats

The application supports two Bayer input configurations:

- **RAW8:** 8-bit RGGB Bayer.
- **RAW10:** packed 10-bit RGGB Bayer.

Packed RAW10 data is unpacked into the 8-bit processing representation used by the image-processing pipeline.

Both input formats are handled by the CPU and DSP implementations.

The supported capture configuration is selected together with the camera mode so that the processing pipeline receives image data in the expected format.

The application does not claim support for every pixel format exposed by the Linux V4L2 driver. Its processing path is specifically implemented for the two RGGB Bayer configurations described above.

## 4. System Architecture

The application connects the camera subsystem, image-processing engines, and video output into one continuous processing pipeline.

```text
                   Radxa Dragon Q6A
                   Qualcomm QCS6490
                           |
                           |
                    Sony IMX219
                           |
                     MIPI D-PHY
                           |
                     MIPI CSI-2
                           |
                           v
                Linux Media Controller
                           |
                           v
                     Linux V4L2
                           |
                           v
                   Frame Acquisition
                           |
                           v
                  Bayer Data Preparation
                           |
                           v
                  Image Processing
                     /         \
                    /           \
                   v             v
             CPU Processing   Hexagon CDSP
               C++ Engine      HVX Engine
                    \           /
                     \         /
                      v       v
                    RGBA Output
                           |
                           v
                    GStreamer
                           |
                           v
                    Wayland Sink
                           |
                           v
                        Display
```

The CPU and DSP paths are alternative execution engines within the same application. They process the camera image using corresponding processing operations and provide the same user-facing functionality.

### 4.1 Camera acquisition and media-graph configuration

Camera frames are acquired using the Linux Video4Linux2 (V4L2) interface.

The application configures the capture device and obtains image buffers from the camera pipeline.

Camera routing is handled through the Linux Media Controller API. The application discovers the available media topology, identifies the path between the sensor and the selected capture node, configures the required media links, and applies the corresponding formats.

The implementation also uses the V4L2 Sub-device API to configure the sensor and other relevant parts of the camera pipeline.

This makes camera routing and configuration part of the application rather than requiring a separately executed `media-ctl` command for normal startup.

### 4.2 CPU processing engine

The CPU engine implements the image-processing operations in C++.

It provides an independent scalar implementation of the processing algorithms and serves as a reference for functional comparison with the DSP engine.

It supports the same application-level processing configuration, including the supported Bayer input formats, camera resolutions, filters, filter strengths, AGC, and display scaling.

### 4.3 Hexagon DSP processing engine

The DSP engine executes the corresponding processing operations on the Qualcomm Hexagon CDSP.

The host application communicates with the DSP through Qualcomm FastRPC. The DSP-side processing code uses HVX vector instructions to perform image operations on the Hexagon architecture.

The DSP implementation is integrated into the same camera application and uses the same processing controls and filter configuration as the CPU implementation.

### 4.4 Video presentation

Processed images are passed to a GStreamer-based output pipeline.

The application uses GStreamer `appsrc` and a Wayland video sink to display the processed camera stream.

The output surface is passed to GStreamer using DMA-BUF-backed memory. The display configuration can be adjusted independently of the selected camera resolution.

## 5. Image-Processing Pipeline

The application implements a configurable image-processing pipeline that prepares raw camera data and applies image transformations before presentation.

The main processing stages are:

| Processing stage | Purpose |
|---|---|
| RAW unpacking | Converts RAW8 or packed RAW10 input into the 8-bit processing representation. |
| Black-level correction | Compensates for the sensor's black-level offset. |
| Digital gain / AGC | Adjusts image intensity using the software gain-control loop. |
| Bayer demosaicing | Reconstructs RGB colour information from the RGGB Bayer pattern using bilinear interpolation. |
| White balance | Applies fixed-point red and blue channel gain coefficients. |
| Box downscaling | Reduces image dimensions using block averaging. |
| Gamma correction | Applies the gamma 2.2 display transformation. |
| Image filters | Applies the selected visual processing effects. |

The pipeline is implemented for both CPU and DSP execution.

The same processing functionality is available across the supported camera resolutions, filters, filter strengths, and display-scaling configurations.

### 5.1 Interactive image filters

The application provides eight selectable image filters:

| Key | Filter | Description |
|---|---|---|
| `1` | Edge Detection | Highlights image edges and transitions using a Sobel-based operation. |
| `2` | Emboss | Produces a relief-like visual effect. |
| `3` | Posterization | Reduces the number of tonal levels. |
| `4` | Inversion | Inverts image intensity and colour values. |
| `5` | Pixelation | Produces a pixelated image effect using block averaging. |
| `6` | Sketch | Creates a sketch-like visual transformation. |
| `7` | Sepia | Applies a sepia colour transformation. |
| `8` | Sharpening | Enhances image detail and local contrast. |
| `0` | No Filter | Disables all selected image filters. |

Individual filters can be toggled, allowing multiple filters to be combined in one processing configuration.

Filter strength can be adjusted interactively using the `+` and `-` keys.

The supported strength range is 1–6.

All implemented filters and their combinations are available in both CPU and DSP processing modes and work across the supported camera resolutions and display-scaling configurations.

## 6. CPU and Hexagon DSP Processing

One of the main engineering objectives is to provide equivalent image-processing functionality on two different compute architectures.

The application contains separate CPU and DSP implementations of the processing operations.

The CPU implementation executes on the application processor, while the DSP implementation executes on the Hexagon CDSP using HVX.

Both implementations support the same camera input configurations, processing stages, filter selection, filter strength, AGC behaviour, and output scaling.

### 6.1 Functional equivalence

Both implementations perform equivalent image transformations using the same processing parameters.

The CPU implementation reproduces the DSP arithmetic, including integer rounding and saturation behaviour, to make output comparison meaningful.

This enables direct comparison of their output using the same input image and configuration.

The comparison covers:

- Pixel arithmetic.
- Processing parameters.
- Bayer input interpretation.
- Demosaicing.
- Digital gain and AGC.
- Intermediate image transformations.
- Filter operations and their execution order.
- Final output buffers.

### 6.2 Runtime engine switching

The application provides an interactive control for switching between CPU and DSP processing.

The selected engine does not remove or restrict the application's camera, filter, or display configuration.

Both engines support the implemented camera-processing functionality, including all available filters and scaling options.

When the user changes the processing engine, the application performs an orderly internal process relaunch. It closes the current camera capture, processing engine, DSP session where applicable, and video sink, then starts itself again with the selected configuration.

The application preserves the relevant runtime settings during this transition. The user does not need to stop and manually restart the program from the shell.

### 6.3 Pixel-level parity verification

The application includes a parity-check mode that compares CPU and DSP output using the same input frame and processing configuration.

Example:

```bash
./build/pipeline \
    --replay frame.raw \
    --width 640 \
    --height 480 \
    --raw8 \
    --parity
```

By default, parity mode executes a matrix of 60 test cases:

- No filter.
- Each of the eight individual filters.
- All filters enabled together.
- Six filter-strength levels, from 1 to 6.

The test compares the resulting RGBA output buffers.

For each case, it reports:

- Maximum absolute byte difference between the CPU and DSP outputs.
- Number of output bytes whose difference exceeds three intensity levels.

The overall pass criterion is based on the maximum difference and the configured `--tolerance` value.

One documented test result:

```text
[parity] worst 0, bad 0 over 60 cases
```

A `worst` value of zero means that the compared output buffers matched exactly in all tested cases.

Parity testing complements live visual inspection and helps validate the consistency of the two processing implementations.

### 6.4 Reproducible test input

A synthetic image can be generated without a connected camera:

```bash
./build/make_frame frame.raw 640 480
```

The generated image can then be replayed through the processing pipeline:

```bash
./build/pipeline \
    --replay frame.raw \
    --width 640 \
    --height 480 \
    --raw8 \
    --sink wayland \
    --tui
```

The replay functionality allows the same input image to be processed repeatedly using different engines, filters, strengths, and scaling configurations.

It is also used by the parity test to provide identical input data to both processing implementations.

## 7. Camera Controls and Automatic Gain Control

The application integrates camera sensor controls and software gain adjustment into the processing environment.

### 7.1 Software AGC

Automatic Gain Control (AGC) is implemented as a software feedback loop in the image-processing engines.

The loop evaluates image brightness using the green-channel luminance measurement and adjusts the digital gain applied to subsequent frames.

The AGC implementation is available in both CPU and DSP processing paths. The CPU implementation mirrors the DSP algorithm so that the two engines apply equivalent gain adjustments.

AGC is enabled by default and can be enabled or disabled during operation using the `a` key.

When AGC is disabled, the current digital gain is held rather than reset to unity. Re-enabling AGC resumes automatic adjustment from the retained gain value.

### 7.2 Sensor exposure and analogue gain

In addition to software AGC, the application provides access to the camera sensor's exposure and analogue gain controls through the V4L2 Sub-device API.

The application discovers the available sensor controls, reads their current values and ranges, and writes requested values through the Linux driver.

The driver may clamp requested values to the supported range. The application reads back the resulting values so that the reported configuration reflects what the sensor actually accepted.

The sensor controls and software AGC are separate mechanisms:

- Sensor exposure controls the sensor integration time.
- Sensor analogue gain controls the sensor's analogue amplification.
- Software AGC adjusts digital gain in the image-processing pipeline.

These controls are independent of whether image processing is executed on the CPU or the Hexagon DSP.

## 8. Display Scaling

The application provides six display-scaling configurations.

The available scale range is 1:1 through 1:6.

The terminal interface allows the user to select the next available display scale using the `s` key.

Downscaling is implemented using box averaging. The CPU and DSP engines implement corresponding scaling operations.

All six scaling options are available with both CPU and DSP processing modes and across all four supported camera resolutions.

Changing the display scale does not limit the available image filters or require a different processing engine.

This makes it possible to inspect the same camera mode and image-processing configuration at different display sizes.

Changing the display scale triggers the application's automatic internal relaunch so that the output surface and video sink can be recreated with the new geometry.

## 9. Memory Management and Data Flow

The application integrates V4L2 capture, image processing, and GStreamer presentation using Linux buffer-management mechanisms.

The design uses DMA-BUF for camera-buffer sharing and RPC memory for processing surfaces.

### 9.1 V4L2 capture buffers and DMA-BUF

The camera capture path requests V4L2 buffers using `V4L2_MEMORY_MMAP`.

The application maps the capture buffers into its address space and exports them as DMA-BUF file descriptors using `VIDIOC_EXPBUF`.

These DMA-BUF descriptors allow compatible subsystems to access the same underlying image buffers without copying the complete image at every interface.

The CPU engine reads the captured image through the mapped buffer.

The DSP engine registers the camera-buffer mapping with the Qualcomm FastRPC subsystem using `remote_register_buf`. The same captured image is then passed to the DSP processing function without an intermediate image staging buffer.

### 9.2 Processing surfaces and RPC memory

Both processing engines allocate their output surfaces using Qualcomm RPC memory allocation functions.

The allocated memory is mapped into the host application and associated with a file descriptor through `rpcmem_to_fd`.

The CPU engine writes its output directly into these surfaces.

The DSP engine writes the processed image into the corresponding RPC-memory output surface through its FastRPC processing call.

This provides a common output-buffer interface for both processing engines.

### 9.3 GStreamer and Wayland integration

The processed output surface is wrapped in a GStreamer buffer using the DMA-BUF allocator.

The application supplies the required video metadata, including the RGBA format, output dimensions, and stride.

The GStreamer pipeline uses `appsrc` and `waylandsink`.

The Wayland compositor can import the DMA-BUF-backed output surface for display. A buffer lease keeps the processing surface occupied until GStreamer releases the corresponding buffer, after which the application returns the surface to the processing engine.

This approach avoids an explicit intermediate pixel-data copy when transferring the output surface into the GStreamer pipeline.

The exact memory-sharing behaviour depends on the capabilities of the participating driver, allocator, GStreamer element, and compositor.

## 10. Interactive Controls

The application provides a terminal-based interface for controlling the camera and processing pipeline while the video is running.

| Key | Action |
|---|---|
| `1`–`8` | Toggle individual image filters. |
| `0` | Disable all selected filters. |
| `+` / `-` | Increase or decrease filter strength. |
| `a` | Enable or disable software AGC. |
| `r` | Select the next supported camera resolution. |
| `s` | Select the next display scale, from 1:1 to 1:6. |
| `e` | Switch between CPU and DSP processing. |
| `h` | Display help. |
| `q` | Exit the application. |

The controls allow the user to change the camera configuration, processing engine, filter combination, filter strength, gain-control behaviour, and display scaling without using separate applications.

Filter selection and strength changes are applied directly to the running processing engine.

Changing the camera resolution, display scale, or processing engine triggers an automatic internal process relaunch with the updated configuration. The application handles resource shutdown and reinitialisation itself.

The supported camera modes, filters, and scaling configurations remain available in both processing modes.

## 11. Hardware and Software Requirements

### 11.1 Reference hardware

The documented hardware configuration consists of:

- Radxa Dragon Q6A development board.
- Qualcomm QCS6490 system-on-chip.
- Sony IMX219 MIPI CSI-2 camera module.
- MIPI CSI-2 camera interface over D-PHY.
- Compatible Embedded Linux image.
- Qualcomm Hexagon DSP runtime and firmware support.
- Display environment with Wayland support.

### 11.2 Software dependencies

The host application uses:

- CMake 3.16 or later.
- C++20-compatible compiler.
- GStreamer 1.20 or later.
- GStreamer App, Video, and DMA-BUF allocator components.
- Wayland-compatible video sink.
- Qualcomm FastRPC runtime libraries, including `libcdsprpc`.
- Compatible Qualcomm DSP runtime and firmware.
- Linux V4L2 and Media Controller interfaces.

The repository includes the prebuilt Hexagon DSP skeleton used by the application.

Rebuilding the DSP-side component requires the appropriate Qualcomm Hexagon SDK and toolchain.

The exact package names and installation procedures depend on the Linux image and Qualcomm software environment installed on the target board.

## 12. Build Instructions

Build the host application from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The build produces the host-side application and supporting utilities.

The expected build layout includes:

```text
build/
├── pipeline
├── make_frame
└── dsp/
    └── libpipe_hvx_skel.so
```

The CMake build copies the included DSP skeleton into the build directory so that the application can locate it next to the executable.

The DSP skeleton is a separate Hexagon component. The host-side CMake build does not rebuild the DSP binary from source.

Before using DSP processing, make sure that the DSP skeleton and the required Qualcomm runtime dependencies are available on the target.

## 13. Running the Application

### 13.1 Live camera mode

Example configuration for a 640 × 480 RAW8 camera mode:

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0

./build/pipeline \
    --device /dev/video9 \
    --media /dev/media0 \
    --raw8 \
    --width 640 \
    --height 480 \
    --sink wayland \
    --tui
```

The device paths and environment variables are examples and should match the active camera and Wayland configuration.

The application defaults to `/dev/video9` for camera capture and `/dev/media0` for media routing. Use `--device` and `--media` to select different nodes when required.

The `--tui` option enables the terminal-based controls while the video is displayed in a separate window.

For other camera resolutions, select the corresponding supported sensor mode and matching Bayer input configuration.

The default input format is packed RAW10. The `--raw8` option selects 8-bit Bayer capture.

### 13.2 CPU processing

To explicitly select the CPU processing engine:

```bash
./build/pipeline \
    --engine cpu \
    --raw8 \
    --width 640 \
    --height 480 \
    --sink wayland \
    --tui
```

The DSP engine is selected by default. The `--engine cpu` option selects the CPU implementation.

### 13.3 Synthetic-frame replay

Generate a synthetic input frame:

```bash
./build/make_frame frame.raw 640 480
```

Replay the frame:

```bash
./build/pipeline \
    --replay frame.raw \
    --width 640 \
    --height 480 \
    --raw8 \
    --sink wayland \
    --tui
```

This mode is useful for repeatable image-processing tests without a live camera.

The replay mode uses the same processing engine and image-processing pipeline as the live camera mode.

### 13.4 Command-line options

| Option | Description |
|---|---|
| `--device PATH` | Selects the V4L2 capture node. Default: `/dev/video9`. |
| `--media PATH` | Selects the media device used for camera routing. Default: `/dev/media0`. |
| `--sink NAME` | Selects the video output: `wayland` or `none`. Default: `none`. |
| `--width N` | Sets the requested camera width. Default: 640. |
| `--height N` | Sets the requested camera height. Default: 480. |
| `--fps N` | Sets the frame rate advertised to the video sink. Default: 30. |
| `--scale N` | Sets display scaling from 1 to 6. Default: 1. |
| `--raw8` | Selects 8-bit Bayer capture. Default input format: packed RAW10. |
| `--filters LIST` | Selects filters: `none`, `all`, or a comma-separated list of filter names. |
| `--strength N` | Sets filter strength from 1 to 6. Default: 2. |
| `--engine NAME` | Selects `dsp` or `cpu`. Default: `dsp`. |
| `--agc` | Enables software AGC. This is the default. |
| `--no-agc` | Disables automatic gain adjustment and holds the current digital gain. |
| `--exposure N` | Sets sensor exposure in lines. The resulting value is read back from the driver. |
| `--gain N` | Sets sensor analogue gain using the driver's control value. |
| `--dump PATH` | Saves a processed output frame as tightly packed RGBA data. |
| `--dump-after N` | Selects which captured frame is saved. Default: 1. |
| `--replay PATH` | Replays one saved RAW frame and exits. |
| `--parity` | Compares CPU and DSP output for the replayed frame. Requires `--replay`. |
| `--tolerance N` | Sets the maximum byte difference accepted by parity mode. Default: 2. |
| `--stride N` | Sets the row stride of a replayed RAW frame. By default, the stride is inferred from the file size. |
| `--tui` | Enables the terminal user interface. |
| `--seconds N` | Stops the application after the specified duration. Zero means run until interrupted. |
| `--no-route` | Disables automatic media-link configuration before capture starts. |
| `--help`, `-h` | Displays command-line help. |

The `--filters` option accepts the following names:

```text
edge,emboss,posterize,invert,pixelate,sketch,sepia,sharpen
```

The special values `none` and `all` disable or enable the complete filter set.

The selected input dimensions and pixel format must match the input data and camera configuration.

## 14. Runtime Information and Performance

The application provides runtime information about image processing, including measured frame rate, peak frame rate, processing-call timing, and input/output memory bandwidth.

The documented reference configuration includes:

| Parameter | Reported value |
|---|---|
| Camera resolution | 640 × 480 |
| Input format | RAW8 |
| GStreamer advertised frame rate | 60 fps |
| Frames in flight | 1 |
| DSP processing time | Approximately 7 ms per frame |

The 60 fps value is the configured frame rate advertised to the video sink. It should not be interpreted as a guaranteed measured capture or processing rate.

The actual frame rate is measured by counting processed frames over a one-second time window.

The runtime panel distinguishes processing-call timing from frame-rate information:

- `fps` represents the measured number of processed frames per second.
- `peak` represents the highest measured one-second frame rate.
- `lat` represents the average duration of the processing-engine call during the current one-second measurement window.
- `latmax` represents the maximum processing-engine call duration observed during that same window.

The processing-call duration is not the same as end-to-end camera latency. Camera acquisition, buffer handling, scheduling, display synchronisation, and presentation can contribute additional time.

The application also reports input and output memory bandwidth estimates based on the frame sizes and measured frame rate.

The approximately 7 ms DSP processing time is a reported value for the reference configuration, not a guaranteed value for every resolution, filter combination, or operating condition.

The application provides a practical environment for observing CPU and DSP processing behaviour under live camera input.

A more extensive quantitative performance evaluation can additionally include sustained throughput, latency distributions, frame-drop accounting, CPU/DSP utilisation, memory bandwidth, and thermal behaviour.

## 15. Source Code Organisation

The repository separates the Linux host application, DSP-side implementation, interface definitions, and supporting tools.

| Path | Responsibility |
|---|---|
| `src/main.cpp` | Application entry point, command-line parsing, runtime orchestration, event loop, and engine selection. |
| `src/v4l2_capture.*` | V4L2 camera acquisition, MMAP buffers, DMA-BUF export, and frame handling. |
| `src/route.*` | Camera media-graph discovery, link configuration, and sub-device format setup. |
| `src/sensor.*` | Camera sensor control discovery, exposure, analogue gain, and read-back. |
| `src/dsp_engine.*` | FastRPC session management, remote buffer registration, and DSP invocation. |
| `src/cpu_engine.*` | CPU-side image-processing implementation and reference calculations. |
| `src/engine.h` | Common processing-engine interface, filter definitions, and output geometry. |
| `src/frame.h` | Frame and surface data structures. |
| `src/metrics.h` | Frame-rate and processing-time measurements. |
| `src/sink.*` | GStreamer video-output integration and DMA-BUF buffer handling. |
| `src/panel.*` | Terminal interface, keyboard controls, and runtime status display. |
| `dsp/` | Hexagon DSP processing kernels, HVX implementation, AGC, and DSP-side components. |
| `idl/` | FastRPC interface definitions and generated host-side stubs. |
| `tools/` | Supporting utilities, including synthetic-frame generation. |

The host application manages camera acquisition, runtime configuration, processing-engine selection, buffer handling, and video presentation.

The DSP-side component provides the corresponding remote processing functionality through the FastRPC interface.

## 16. Implementation Scope and Technical Considerations

The project demonstrates an integrated camera-processing application rather than an isolated image-processing kernel.

Its implementation covers several layers of embedded software development:

**Camera and sensor integration**

- Sony IMX219 sensor integration.
- MIPI CSI-2 over D-PHY.
- Linux V4L2 capture.
- Linux Media Controller API.
- V4L2 Sub-device API.
- Automatic camera media-graph discovery and configuration.
- Sensor resolution and control configuration.
- RGGB Bayer RAW8 and packed RAW10 input.
- Exposure and analogue gain control.

**Image-processing implementation**

- RAW8 and packed RAW10 input preparation.
- Black-level correction.
- Software AGC and digital gain.
- Bilinear Bayer demosaicing.
- White balance.
- Box downscaling.
- Gamma correction.
- Eight configurable image filters.
- Adjustable filter strength and filter combinations.
- CPU-side processing in C++.
- Hexagon DSP processing using HVX.
- Equivalent processing functionality across both execution engines.
- Multiple camera resolutions and display scales.

**Heterogeneous system integration**

- Qualcomm FastRPC communication.
- Hexagon CDSP session management.
- DSP runtime and memory integration.
- Remote registration of DMA-BUF camera buffers.
- RPC-memory output surfaces.
- GStreamer DMA-BUF integration.
- Wayland display integration.

**Runtime operation and verification**

- Interactive camera and processing controls.
- CPU/DSP switching.
- Camera-resolution selection.
- Display-scale selection.
- Image-filter selection and adjustment.
- AGC control.
- Sensor exposure and analogue gain configuration.
- Pixel-level output comparison.
- Automated parity test matrix.
- Synthetic-frame generation and replay.
- Runtime frame-rate and processing-time measurements.

The application is designed to make these components work together as one operational system.

## 17. Future Development

Possible extensions to the project include:

- Extended CPU/DSP performance benchmarking.
- Sustained-throughput measurements at all supported camera resolutions.
- End-to-end frame-latency measurement.
- Frame-drop and processing-deadline accounting.
- CPU and DSP utilisation analysis.
- Memory-bandwidth and buffer-transfer profiling.
- Additional image-processing algorithms.
- Automated regression testing of CPU/DSP output equivalence.

These extensions would build on the existing camera pipeline and processing architecture.

## 18. Project Relevance

This project demonstrates practical engineering work in embedded Linux, camera integration, image processing, and heterogeneous computing.

The implementation combines:

- Embedded Linux application development in C++20.
- Sony IMX219 camera integration.
- MIPI CSI-2 and D-PHY.
- V4L2 camera integration.
- Linux Media Controller API.
- Bayer image-data processing.
- Camera sensor controls and software AGC.
- Real-time image-processing pipeline design.
- Qualcomm Hexagon CDSP integration.
- HVX vector processing.
- Qualcomm FastRPC.
- DMA-BUF memory handling.
- GStreamer and Wayland video presentation.
- CPU/DSP functional comparison.
- Interactive runtime configuration and debugging.

The main result is a working camera application in which the CPU and Hexagon DSP can both execute the implemented image-processing pipeline, with the same supported camera modes, Bayer input configurations, filters, filter strengths, AGC behaviour, and display-scaling functionality.

It provides a practical demonstration of integrating camera hardware, Linux multimedia interfaces, DSP acceleration, memory management, and application-level controls on a modern heterogeneous SoC.

## License

See [LICENSE](LICENSE) for licensing information.

