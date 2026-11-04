# Engineering Findings, Measurements and Platform Constraints

## 1. Overview

This document records the main engineering findings obtained while developing the camera acquisition and real-time image-processing pipeline for the Qualcomm QCS6490 platform.

The work covers several connected development stages:

- Camera sensor and CSI/CAMSS integration.
- V4L2-based frame acquisition.
- DMA-BUF buffer handling and ownership.
- CPU and Qualcomm Hexagon DSP processing paths.
- HVX vector processing.
- CPU/DSP functional parity.
- GStreamer and Wayland video presentation.
- Real-time pipeline behavior, profiling and failure analysis.

The findings distinguish measured results from implementation constraints, observed failure patterns and unresolved questions.

The project is an engineering reference and demonstration platform. It is not presented as a production camera product, a complete ISP implementation or a general-purpose Qualcomm BSP.

## 2. Target Platform and Software Environment

| Component | Configuration |
|---|---|
| SoC | Qualcomm QCS6490 |
| Development board | Radxa Dragon Q6A |
| CPU architecture | ARM64 |
| CPU cores | 4 × Cortex-A55 + 4 × Cortex-A78 |
| DSP | Qualcomm Hexagon v68 |
| Vector engine | HVX, 128-byte vector width |
| DSP domain | CDSP, domain 3 |
| Camera sensor | Sony IMX219 |
| Camera interface | CSI / CAMSS |
| Capture interface | V4L2 |
| Kernel | Linux 6.18 |
| Hexagon SDK | 6.4.0.2 |
| Hexagon Tools | 19.0.04 |
| GStreamer | 1.24.2 |
| CMake | 3.28.3 |
| Compiler | GCC 13.3, AArch64 |

The platform configuration above describes the development environment used for the recorded experiments. It should not be interpreted as a claim that every software component is built into the final demonstration package.

## 3. Processing Architecture

The development established two alternative processing paths:

- CPU processing.
- Hexagon DSP/HVX processing.

The intended processing model is frame-oriented: a frame is processed by one selected engine rather than being split unpredictably between CPU and DSP stages.

The pipeline includes camera acquisition, frame-buffer handling, image processing and presentation. The development also investigated direct camera-buffer access from the DSP through FastRPC and the interaction between DMA-BUF-backed surfaces and the display pipeline.

The final demonstration is a deliberately reduced and independently buildable subset of the broader development work.

## 4. Measured Performance

### 4.1 CPU and DSP processing

The following measurements were recorded during development:

| Workload | Resolution | CPU | DSP |
|---|---:|---:|---:|
| New DSP processing engine | 640 × 480 | 10.6 ms | 2.7 ms |
| New DSP processing engine | 1920 × 1080 | 19.3 ms | 11.1 ms |
| Earlier `fast-dsp` implementation | 640 × 480 | 1.3 ms | 4.1 ms |

The results demonstrate that DSP acceleration is workload- and implementation-dependent.

The earlier `fast-dsp` implementation was slower on the DSP than on the CPU. The later engine produced better results because of differences in processing organization and implementation. These measurements must not be interpreted as a universal performance advantage of the DSP.

### 4.2 HVX microbenchmark

A separate compute microbenchmark recorded:

| Implementation | Cycles |
|---|---:|
| Scalar | 2,335,756 |
| HVX | 35,060 |

Measured speedup: approximately **66.62×**.

This is a result for the specific microbenchmark, not for the complete camera pipeline or video-processing workload.

### 4.3 Camera pipeline throughput

The CPU pipeline at 640 × 480 produced the following measurements:

| Output mode | Measured FPS | Processing time |
|---|---:|---:|
| Null sink | 60.1 | 10.45 ms |
| Wayland sink | 60.0 | 10.47 ms |

A separate DSP demonstration recorded approximately 60 FPS at 640 × 480, with processing time around 7 ms.

These figures belong to different test configurations and should not be combined into a single benchmark claim.

A measured 60 FPS output rate does not, by itself, prove that every possible camera mode, filter configuration or display condition can sustain the same rate.

## 5. CPU/DSP Functional Parity

One of the central engineering objectives was to make the CPU reference implementation reproduce the DSP processing behavior exactly.

This required matching actual implementation semantics rather than assuming that mathematically ideal or rounded arithmetic was correct.

### 5.1 HVX averaging semantics

The HVX instruction `Q6_Vub_vavg_VubVub` uses truncating unsigned-byte averaging.

For example:

- Average of 0 and 1 produces 0.
- A CPU implementation using rounded arithmetic would produce a different result.

The CPU reference was adjusted to reproduce the DSP's truncation behavior and the two-level 2 × 2 averaging sequence.

An initial mismatch affected 1,200 bytes, primarily around the first and last columns, with differences up to 18 intensity levels.

After correction, the recorded parity run contained 488 comparisons with a worst-case difference of zero.

### 5.2 Gamma LUT and alpha-channel corruption

A 32-bit RGBA word was initially handled without correctly masking the individual byte values.

The alpha byte (`0xFF`) consequently contaminated the LUT index or output calculation, producing an all-white image.

The issue was corrected by explicitly masking and handling the relevant byte fields.

### 5.3 Identity processing as a mandatory baseline

A filter-only parity matrix can conceal errors in the underlying tone or color pipeline.

The validation sequence was therefore changed to begin with a no-filter identity case.

This provides a baseline against which subsequent filter combinations can be compared.

### 5.4 Geometry and stride

The output geometry uses:

`out_w = (width / scale) & ~31`

The output stride is:

`stride = out_w * 4`

The width is quantized down to a multiple of 32 pixels to satisfy the processing geometry requirements.

The recorded geometry test covered 32 combinations, all of which passed.

The corresponding trade-off is that up to 31 columns may be discarded on the right-hand side.

### 5.5 Parity acceptance criteria

The parity framework is intended to validate the actual processing implementation, not merely visual similarity.

The recorded results include:

- 488 parity comparisons.
- Worst-case difference: 0.
- 32/32 geometry combinations passed.

The scope of these results is limited to the tested configurations. Full parity coverage across every camera mode, scale and filter combination has not been established.

## 6. Camera Capture and Buffer Handling

### 6.1 Camera acquisition

The camera path was investigated through the Linux media subsystem, CSI/CAMSS, V4L2 and GStreamer.

The IMX219 camera was operated through the VFE RDI capture path.

The development covered multiple sensor modes, including:

- 640 × 480
- 1640 × 1232
- 1920 × 1080
- 3280 × 2464

Not every mode was validated through every processing and display configuration.

### 6.2 DMA-BUF buffer rotation

The camera capture path uses a pool of four buffers.

An important failure occurred when only the first buffer file descriptor was registered for DSP access.

The camera continued rotating through the buffer pool, while the DSP could access a buffer that had already been reused or overwritten.

This caused CPU/DSP digest mismatches.

The implementation was corrected to account for the active buffer descriptor rather than assuming that the first descriptor remained valid for every frame.

The general engineering lesson is that buffer identity and ownership must be tracked per frame, not inferred from a persistent file descriptor.

### 6.3 Surface lifetime and display ownership

Two output surfaces were insufficient for the tested display pipeline.

The sink and compositor could retain surfaces beyond the point at which the application had submitted a frame.

The solution introduced explicit surface ownership:

- The sink declares the number of surfaces it requires.
- The processing engine does not reuse a surface until the sink returns it.
- `enable-last-sample=false` prevents an additional retained sample from extending the surface lifetime.
- The sink is destroyed before the engine so that retained surfaces are released before engine teardown.

The tested sink requirements were:

| Sink | Required surfaces |
|---|---:|
| Null sink | 2 |
| Wayland sink | 4 |

This is an explicit ownership model rather than an assumption that a buffer becomes reusable immediately after submission.

## 7. Hexagon DSP and FastRPC Findings

### 7.1 DSP session stability

Repeated short-lived FastRPC sessions produced a recurring failure pattern.

The CDSP became unavailable after approximately 55–77 sessions in the observed tests.

The failure was associated with SMMU faults. The DSP did not reliably recover by itself.

The observed value of 55–77 sessions is an empirical failure range for the tested configuration, not a documented platform limit.

### 7.2 DSP liveness checks

The state:

`/sys/class/remoteproc/.../state = attached`

does not prove that the DSP is operational.

A real RPC operation is required to establish that the DSP is responding.

This distinction matters because the remoteproc state may remain attached after the DSP has stopped responding to requests.

### 7.3 Threading restrictions in the tested execution environment

The tested unsigned protection domain did not export the following functions:

- `qurt_thread_create`
- `qurt_thread_join`
- `qurt_sched_yield`
- `qurt_thread_get_id`
- `qurt_thread_get_priority`

A threaded skeleton failed to load with:

`AEE_EUNABLETOLOAD (0x80000406)`

Consequently, the tested DSP implementation uses a single processing thread.

This is a restriction observed in the tested unsigned protection-domain configuration. It is not a claim that the Hexagon architecture or all Qualcomm DSP environments are inherently single-threaded.

### 7.4 FastRPC memory registration

The experiments investigated the relationship between RPC memory allocation, buffer registration and the timing of session creation.

One important observation was that `RPCMEM_TRY_MAP_STATIC` only has the intended effect when the relevant allocation occurs before the FastRPC session is opened.

In the tested implementation, the buffers were allocated after `frame_hvx_open()`, so the expected static-mapping behavior could not be assumed.

Compilation of a memory-management variant was not considered proof that the corresponding memory contract worked at runtime.

## 8. Pixel Formats and Diagnostic Pitfalls

### 8.1 RAW10 versus RAW8

The IMX219 modes at 1640 × 1232 and above use RAW10.

Using a RAW8 interpretation for these modes produces invalid image data.

The format must be established from the actual V4L2 format configuration rather than inferred from the number of bytes read.

### 8.2 Dump padding and apparent image corruption

The diagnostic dump path initially included padded surface data for some resolutions.

This produced diagonal displacement or apparent image corruption in the dumped files.

The problem was in the dump interpretation, not necessarily in the live processing pipeline.

Diagnostic output must therefore account for the actual stride and active image width.

### 8.3 Bayer validation

A green-channel spread metric was found to be invalid for the tested Bayer data because the CFA pattern alternates color components spatially.

A synthetic horizontal ramp and a shift-tolerant SAD comparison were identified as a more appropriate validation method.

## 9. Build and Deployment Findings

### 9.1 Generated IDL artifacts

The FastRPC interface has generated components on both sides of the interface.

Regenerating or rebuilding only one side can produce an ABI mismatch.

Both host and DSP generated artifacts must be kept consistent.

### 9.2 Header dependency tracking

A simple build procedure based only on file modification times may fail to rebuild an object when a generated header changes.

A reliable build must track header dependencies explicitly.

### 9.3 Skeleton replacement

In the tested configuration, replacing the DSP skeleton was picked up by the next DSP session without requiring a remoteproc restart.

This was observed twice.

This behavior should be treated as a tested property of the particular deployment setup, not a universal guarantee for every Qualcomm platform.

## 10. Unresolved Issues and Known Limitations

The following items remain outside the scope of fully validated functionality:

- RAW8 support for higher-resolution camera modes.
- Full live testing across all sensor modes and processing scales.
- Complete parity coverage at 1640 × 1232 and 1920 × 1080.
- CPU utilization below 10% at 60 FPS has not been established.
- No demonstrated automatic recovery procedure for the observed CDSP/SMMU failure.
- DSP multithreading is unavailable in the tested unsigned protection-domain configuration.
- The complete absence of copies throughout the camera-to-display path has not been proven.
- The measured performance results do not establish production-level thermal or long-duration reliability across all operating conditions.

## 11. Engineering Conclusions

The work demonstrates practical integration across several layers of an embedded multimedia system:

- Linux camera acquisition through V4L2 and CSI/CAMSS.
- Frame-buffer handling and DMA-BUF integration.
- Qualcomm FastRPC communication with the Hexagon DSP.
- HVX-oriented image-processing implementation.
- CPU reference implementation and bit-exact parity validation.
- GStreamer and Wayland presentation.
- Buffer lifetime, surface ownership and pipeline synchronization.
- Investigation of DSP failures, memory registration and platform constraints.

The most important outcome is not a single acceleration number. It is the establishment of a reproducible processing and validation framework in which camera acquisition, CPU/DSP execution, buffer ownership and output behavior can be investigated independently and measured under controlled conditions.
