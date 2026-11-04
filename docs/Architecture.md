# Architecture: Camera-to-Display Processing on Qualcomm QCS6490

## 1. Purpose and Scope

This document describes the architecture of the QCS6490 Camera DSP vs CPU project, a camera-driven image-processing application for the Radxa Dragon Q6A platform.

The application implements two alternative processing paths for image data acquired through Linux V4L2:

- A CPU implementation written in C++.
- A Hexagon CDSP implementation using HVX vector instructions and invoked through Qualcomm FastRPC.

Both paths are integrated into the same application and produce output for the same display pipeline. This makes it possible to inspect their results under equivalent input conditions and verify their functional consistency.

The primary engineering objective is to demonstrate DSP integration, streaming image processing, and CPU/DSP functional equivalence. Runtime timing information is available for interactive investigation, but comprehensive performance characterisation is outside the scope of this document.

## 2. System Overview

The application is divided into four functional areas:

1. **Acquisition:** configures the camera and receives image frames through V4L2.
2. **Processing:** applies image preparation, intensity transformations, and configurable filters using either the CPU or DSP implementation.
3. **Presentation:** passes the processed output into the GStreamer and Wayland display path.
4. **Control and verification:** manages user interaction, reports runtime information, and compares CPU and DSP output.

### Logical data flow

```text
                    IMX219 Camera
                          |
                          v
                     CSI Interface
                          |
                          v
                      V4L2
                          |
                          v
                   Frame Acquisition
                          |
                          v
                   Input Image Buffer
                          |
                          v
                  Processing Selection
                     /           \
                    /             \
                   v               v
              CPU Engine       Hexagon CDSP
              Scalar C++       HVX Kernels
                   \               /
                    \             /
                     v           v
                    Output Image Buffer
                          |
                          v
                     GStreamer
                          |
                          v
                      Wayland
                          |
                          v
                        Display
```

This diagram represents the logical architecture rather than every physical memory transfer. Buffer allocation, registration, mapping, and ownership are handled by the relevant implementation components.

## 3. Camera Acquisition

Camera acquisition is implemented in `src/v4l2_capture.cpp`.

The application configures the selected video device, queues capture buffers, and waits for completed frames using the Linux event notification mechanism.

In the documented configuration, the capture device is `/dev/video9`.

The capture path exports buffers as DMA-BUF file descriptors where supported. This provides a mechanism for sharing buffer references between compatible Linux subsystems without necessarily copying the underlying image data.

DMA-BUF export alone does not guarantee that all subsequent operations are copy-free. That depends on the buffer import mechanisms, memory layout, pixel format, and requirements of the processing and display components.

The camera's selected operating mode determines the input dimensions and pixel format. In particular, RAW8 and RAW10 require different handling and must not be treated as interchangeable formats.

## 4. Processing Engines

The project separates processing logic from the application-level control and presentation components.

### 4.1 CPU engine

The CPU implementation is located in `src/cpu_engine.cpp`.

It provides a scalar C++ implementation of the image-processing operations supported by the project. Its arithmetic is designed to reproduce the relevant integer behaviour of the DSP kernels rather than merely approximate their visual output.

This is important because mathematically similar expressions do not necessarily produce identical integer results. Rounding, truncation, intermediate precision, and saturation can all affect individual pixels.

The CPU implementation serves as the functional reference for the parity tests.

### 4.2 Hexagon CDSP engine

The DSP integration is implemented in `src/dsp_engine.cpp`.

The host application establishes a FastRPC session and invokes the DSP-side processing implementation through generated interface bindings.

The DSP skeleton is supplied as a prebuilt shared object:

`dsp/libpipe_hvx_skel.so`

The corresponding DSP source code includes the processing implementation and the HVX-oriented kernels.

The DSP path uses the Qualcomm Hexagon compute DSP available on the QCS6490 platform. HVX instructions provide vector-processing capabilities that can be used to process multiple data elements within a vector operation.

The host-side application and the DSP implementation have separate build requirements. The host application is built with the normal CMake toolchain, while rebuilding the DSP skeleton requires the appropriate Hexagon SDK and compiler.

### 4.3 Engine selection

The application can select the CPU or DSP path at runtime through its control interface.

This permits visual inspection of both implementations using the same application and comparable input conditions.

Switching engines does not, by itself, constitute a controlled performance benchmark. Such a benchmark would also need to control input data, processing parameters, warm-up, execution conditions, and measurement methodology.

## 5. Image-Processing Stages

The documented processing sequence is:

```text
Input samples
     |
     v
Black-level correction
     |
     v
Automatic gain control
     |
     v
White balance
     |
     v
Demosaicing
     |
     v
Box downscaling
     |
     v
Gamma transformation
     |
     v
Optional image filters
     |
     v
Output image
```

The precise execution location of each stage depends on the selected implementation and the actual kernel path.

### 5.1 Black-level correction

The sensor's black level represents a baseline offset in the raw pixel values.

The documented implementation uses a black-level value of 16. Removing the baseline before applying gain prevents the sensor pedestal from being amplified along with the image signal.

The exact correction and clipping behaviour is determined by the kernel implementation.

### 5.2 Automatic gain control

The application includes an automatic gain-control mechanism that adjusts image intensity according to the observed output.

The implementation uses power-of-two gain steps. Doubling the signal corresponds to one exposure stop before clipping and other nonlinear transformations are considered.

After gamma correction with an exponent of approximately 1/2.2, doubling the input intensity produces an output factor of approximately 2^(1/2.2), or 1.37, in the unclipped region.

This relationship helps explain the documented display-intensity target band of approximately 100–140.

The application also supports disabling AGC. In this mode, the gain remains at its current setting according to the application's control logic.

### 5.3 White balance

White balance adjusts the relative contribution of colour channels to compensate for the colour characteristics of the illumination.

The exact channel gains and their application are defined by the implementation.

### 5.4 Demosaicing

Demosaicing reconstructs colour information from the sensor's Bayer-pattern samples.

The CPU and DSP implementations must use compatible integer arithmetic and sampling rules if their outputs are expected to match exactly.

### 5.5 Box downscaling

Box downscaling reduces the image dimensions by averaging groups of source samples.

The order of operations matters. Averaging linear samples before gamma correction produces a different result from averaging gamma-transformed values.

The current pipeline performs the downscaling stage before gamma correction.

### 5.6 Gamma correction

Gamma correction applies a nonlinear transformation to the processed intensity values to produce a display-oriented output.

Because gamma correction changes the relationship between input intensity and output intensity, it should be treated as a distinct stage rather than folded into a linear averaging operation.

### 5.7 Optional filters

The application provides interactive image effects, including edge detection, embossing, posterization, inversion, pixelation, sketch, sepia, and sharpening.

The filters are useful for visual verification because their effects are easy to recognise in the displayed image.

The supported filter parameters and arithmetic are shared between the relevant CPU and DSP implementations.

## 6. CPU/DSP Arithmetic Equivalence

A central architectural requirement is that the two processing implementations reproduce the same integer behaviour for supported operations.

The CPU path is deliberately not a floating-point approximation of the DSP path. It reproduces the relevant semantics of the DSP arithmetic.

Examples include:

### 6.1 Averaging and truncation

For an integer averaging operation that truncates the result:

```text
avg(0, 1) = 0
```

This differs from rounding to the nearest integer.

Using a different rounding convention can produce small pixel-level differences that accumulate across multiple processing stages.

### 6.2 Demosaic averaging

The documented 2×2 estimate is calculated as:

```text
avg(avg(a, b), avg(c, d))
```

It is not necessarily equivalent to:

```text
(a + b + c + d + 2) >> 2
```

The intermediate truncation steps can change the final result.

### 6.3 Saturating gain

The AGC implementation uses saturating doubling in 8-bit arithmetic. Values exceeding the representable range are saturated rather than allowed to wrap around.

The CPU implementation must preserve this behaviour to match the DSP kernel.

These examples illustrate why functional equivalence requires matching the actual instruction semantics, not just the high-level mathematical intention.

## 7. Parity Verification

The `--parity` option compares the CPU and DSP outputs for the same input frame and supported processing configurations.

The comparison operates at the output-buffer level. A zero-difference result indicates that the compared bytes match for the tested cases.

The reported synthetic-frame result is:

```text
[parity] worst 0, bad 0 over 60 cases
```

Here, `worst 0` indicates that the maximum observed per-byte difference was zero, and `bad 0` indicates that no differing bytes were reported across the tested cases.

This is evidence of functional equivalence for that test set, not a proof of equivalence for every possible image, sensor mode, filter parameter, or runtime condition.

### 7.1 Reproducible testing

A synthetic frame can be generated without camera hardware:

```bash
./build/make_frame frame.raw 640 480
```

The frame can then be replayed:

```bash
./build/pipeline \
    --replay frame.raw \
    --width 640 \
    --height 480 \
    --raw8 \
    --parity
```

Replay-based verification makes it easier to isolate processing behaviour from camera acquisition and sensor configuration.

### 7.2 Testing real captures

A captured frame can also be saved and replayed to investigate differences under a particular input condition.

For reproducible results, the input frame, format, dimensions, filter selection, and strength should be kept consistent between runs.

## 8. Buffer Handling and Presentation

The application integrates camera capture, processing, and display using Linux buffer interfaces and GStreamer.

### 8.1 FastRPC buffer registration

The DSP processing path registers or otherwise exposes the required buffers through the FastRPC integration and invokes the DSP implementation.

The intended data path avoids unnecessary host-side copies by using shared buffer resources where supported.

However, registering a file descriptor with a runtime is not equivalent to proving that no data movement occurs within the runtime or hardware. Such a claim requires evidence for the specific memory and DSP interface implementation.

### 8.2 GStreamer output

The display component is implemented in `src/sink.cpp`.

The documented implementation wraps output buffers in GStreamer buffers and associates video metadata with the relevant memory layout. DMA-BUF import can allow compatible display components to consume shared buffers directly.

Whether `waylandsink` imports a buffer without copying depends on the actual negotiated format, allocator behaviour, and sink implementation.

Consequently, end-to-end zero-copy operation should be documented only when verified across the complete path.

## 9. DSP Session Lifetime

The current application is designed around one DSP session per process.

The session is established through the generated FastRPC interface. During development, repeated creation of short-lived DSP sessions on the target board was observed to lead to failures, including SMMU faults.

To avoid repeatedly creating sessions during interactive operation, certain configuration changes restart the application instead of opening an additional session.

The affected controls include:

- `e` — switching processing engine.
- `r` — changing sensor resolution.
- `s` — changing processing or display scale.

The parity test also executes its test cases within a single session.

This behaviour is a platform-specific stability constraint observed in the current implementation. It should not be generalised to every QCS6490 firmware, runtime version, or FastRPC application without further investigation.

## 10. Runtime Diagnostics and Performance Scope

The application reports processing-call timing and runtime state through its control panel.

These values are useful for observing the behaviour of the processing pipeline, but they represent only selected aspects of system performance.

For example, the duration of a processing call does not necessarily include sensor exposure, frame acquisition, queueing, scheduling delays, display synchronisation, or final presentation.

A separate performance study would need to define and measure:

- Sustained frame throughput.
- Processing latency distributions.
- End-to-end acquisition-to-display latency.
- Frame drops and deadline misses.
- CPU and DSP utilisation.
- Buffer-transfer overhead and memory bandwidth.
- Thermal behaviour under sustained load.

The current project focuses on functional processing and visual verification rather than claiming hard real-time guarantees.

## 11. Related Source Files

| File or directory | Responsibility |
|---|---|
| `src/v4l2_capture.cpp` | Camera capture and V4L2 buffer handling. |
| `src/route.cpp` | Media-graph configuration. |
| `src/sensor.cpp` | Sensor exposure and gain controls. |
| `src/cpu_engine.cpp` | CPU-side processing implementation. |
| `src/dsp_engine.cpp` | FastRPC session management and DSP invocation. |
| `src/sink.cpp` | GStreamer output integration. |
| `src/panel.cpp` | Runtime controls and diagnostics. |
| `dsp/` | DSP kernels and implementation. |
| `idl/` | FastRPC interface definitions and generated host stubs. |
| `docs/dsp-build.md` | DSP skeleton build instructions. |

## 12. Summary

The project combines V4L2 camera acquisition, CPU and Hexagon DSP processing, FastRPC integration, and GStreamer-based presentation in one embedded Linux application.

Its primary contribution is a practical and visually verifiable demonstration of heterogeneous image processing, supported by reproducible input generation and CPU/DSP output comparison.

The architecture also provides a foundation for subsequent work on quantitative performance characterisation, memory-path validation, and sustained real-time behaviour.
