# Development History

## 1. Project Evolution

The project evolved from an initial CPU-based image-processing prototype into a multi-stage camera and DSP integration effort on the Qualcomm QCS6490 platform.

The development can be divided into six major phases:

1. CPU image-processing prototype.
2. Camera sensor and Linux media-stack integration.
3. Hexagon DSP and HVX feasibility work.
4. Live camera-to-DSP processing and memory integration.
5. Real-time CPU/DSP pipeline and functional parity.
6. Extraction of a standalone QCS6490 camera/DSP demonstration.

The history below describes the development line that led to the final demonstration. Earlier experiments and intermediate implementations are retained where they explain important architectural decisions or technical findings.

## 2. Phase A — CPU Image-Processing Prototype

**October 10, 2026**

The initial stage established a CPU-based image-processing prototype.

The implementation covered several basic image-processing operations, including:

- Thresholding.
- Motion-related processing.
- Edge detection.
- Preprocessing.
- Combined processing modes.

AddressSanitizer was used during debugging.

The initial synthetic workload achieved approximately 27–28 FPS at 640 × 360.

Several software defects were identified and corrected:

- A double-free condition.
- An AArch64 segmentation fault associated with the use of 32-bit unsigned counters in nested loops in the edge-processing path. The affected counters were changed to signed integer types.
- An approximately 13 MB memory leak caused by the producer not draining the ring buffer.

This phase established the first working CPU reference and exposed basic memory-management and data-flow problems before hardware acceleration was introduced.

## 3. Phase B — Camera Sensor and Linux Media Integration

**October 13, 2026**

The next stage moved from synthetic frames to live camera input.

The work involved investigation of:

- Linux media-controller topology.
- CSI and CAMSS configuration.
- VFE RDI capture.
- V4L2 format negotiation.
- Camera sensor modes.
- GStreamer preview.

The IMX219 sensor was successfully accessed through the board's camera pipeline.

A Wayland preview smoke test recorded approximately 590 frames over 10 seconds.

This demonstrated that the camera and display path were operational. It was not treated as proof of sustained 60 FPS or of final image quality.

The sensor exposure and gain were adjusted to improve the initial image brightness. The recorded mean brightness increased from approximately 16 to 126.

A separate investigation explored the libcamera Simple IPA path because the initial direct RAW workflow did not yet provide the desired camera experience.

The development therefore examined two distinct directions:

- A conventional camera path using libcamera.
- A direct V4L2/RAW-oriented path intended for lower-level control of captured image data.

These were exploratory stages, not simultaneous requirements of the final demonstration.

The final processing architecture focused on direct V4L2 acquisition and explicit control of the frame-processing path.

## 4. Phase C — Hexagon DSP and HVX Feasibility

**October 13–14, 2026**

The development moved to the Qualcomm Hexagon DSP.

The environment was established using:

- Hexagon SDK 6.4.0.2.
- Hexagon Tools 19.0.04.
- CDSP domain 3.
- Hexagon v68.
- HVX with a 128-byte vector width.

Initial DSP tests verified that the host could communicate with the DSP and execute basic workloads.

An HVX vector-add smoke test successfully processed 64 values.

A separate scalar-versus-HVX microbenchmark recorded approximately 66.62× speedup.

This result established that HVX acceleration was available and useful for suitable compute workloads. It was deliberately kept separate from camera-pipeline performance claims.

## 5. Phase D — Live Camera Data and FastRPC Memory Integration

**October 14–21, 2026**

The next major challenge was to move from isolated DSP workloads to processing actual camera data.

The investigation covered:

- FastRPC interface generation.
- Host and DSP skeleton integration.
- RPC memory allocation.
- DMA-BUF access.
- Buffer registration.
- DSP-side frame processing.
- CDSP failure diagnosis.

Several failure modes were encountered.

An early camera/DSP experiment failed with a FastRPC error and CDSP termination. Subsequent attempts showed that different error codes could represent different stages of the failure sequence.

The investigation distinguished an initial RPC failure from later connection-refused errors caused by the already-unavailable DSP.

The memory-registration experiments also showed that compiling a new RPC memory-management path was not sufficient to prove that the runtime buffer contract was correct.

### Live DSP processing

By October 21, the implementation had reached a live camera-processing stage.

The S1–S5 test sequence was completed, and the first working `frame_hvx` path was established.

A significant defect was found in the camera-buffer registration logic: only the first file descriptor from a four-buffer capture pool had been registered.

The camera continued rotating through the pool, so the DSP could access a buffer that no longer represented the current frame.

The implementation was changed to account for the active buffer descriptor.

This was an important distinction: the observed CPU/DSP digest mismatch was caused by buffer lifecycle and ownership, not by an incorrect DSP processing algorithm.

The work also established that the display path and the DSP processing path had to be validated independently. A successful display test did not prove that the DSP could safely access the same memory.

## 6. Phase E — Live DSP Processing and Image-Processing Features

**October 22–24, 2026**

The DSP processing path was extended with image-processing functionality.

The development included:

- Denoising.
- Sharpening.
- Edge and contour processing.
- Gain adjustment.
- Bayer-related processing experiments.
- In-place filter execution.

An X-ray-style contour path replaced the earlier edge-processing experiment.

The implementation was also extended with an automatic gain stage.

A later experiment demonstrated that redundant memory copies could be removed from the in-place filter path without breaking the tested live processing workflow.

The DSP implementation was no longer treated merely as a standalone compute test. It had become part of a live camera-processing system.

The logs also contain a long-duration live demonstration entry for October 22. The original duration notation should be checked against the underlying test record before being used as a formal reliability claim.

## 7. Phase F — Pipeline Refactoring and Reproducible Validation

**October 25–29, 2026**

The project structure was reorganized to separate experimental DSP code from the new camera-processing pipeline.

A new `pipeline/` implementation was developed as an independent C++ application.

The architecture included:

- V4L2 multi-planar capture.
- DMA-BUF export through `VIDIOC_EXPBUF`.
- An `epoll`-based event loop for camera and input events.
- CPU processing engine.
- DSP processing engine.
- Null and Wayland sinks.
- GStreamer-based presentation.
- Terminal-based control and diagnostics.

The new pipeline was intentionally kept independent of the earlier `fast-dsp/` implementation.

### CPU pipeline baseline

At 640 × 480, the CPU path recorded:

- Approximately 60.1 FPS with the null sink.
- Approximately 60.0 FPS with the Wayland sink.
- Approximately 10.45–10.47 ms processing time.

The test recorded 301 frames over five seconds.

The buffer path used DMA-BUF-backed surfaces and GStreamer DMA-BUF allocation/import mechanisms. The implementation was designed to avoid unnecessary application-level copies. A complete end-to-end proof of zero-copy operation was not established.

### Surface lifetime problem

The initial two-surface configuration could deadlock because the sink or compositor retained surfaces longer than expected.

The problem was addressed by making surface ownership explicit and allowing each sink to declare its required queue depth.

The Wayland sink used a deeper surface pool than the null sink.

The implementation also disabled GStreamer's last-sample retention and corrected destruction order so that the sink released its surfaces before the processing engine was destroyed.

### DSP engine integration

The DSP engine was added to the new pipeline alongside the CPU engine.

The two engines were then subjected to common validation rather than being tested only through separate standalone applications.

## 8. Phase G — Color Pipeline and Bit-Exact CPU/DSP Parity

**October 29–31, 2026**

The processing pipeline was extended with a more structured color and filter sequence.

The recorded order became:

1. Black-level correction.
2. Gain.
3. White balance.
4. Downscaling.
5. Gamma correction.
6. Image filters.

The implementation used fixed parameters for reproducible tests:

- Black-level pedestal: 16.
- White-balance gains: 319 and 556.
- Gamma: 2.2.

Automatic gain control was introduced with explicit freeze semantics.

The CPU and DSP implementations were progressively aligned.

Several subtle differences were identified:

- HVX unsigned-byte averaging truncates rather than rounds.
- Gamma LUT processing required explicit byte masking.
- CPU and DSP geometry had to use the same width quantization.
- Stride had to match the actual output surface layout.

A no-filter identity case was added as the baseline for the parity matrix.

The recorded validation results included:

- 488 parity comparisons.
- Maximum absolute difference: zero.
- 32 out of 32 geometry combinations passed.

The result established exact parity for the tested configurations. It did not establish exhaustive parity across every possible sensor mode and processing combination.

## 9. Phase H — Standalone Demonstration Extraction

**November 2–3, 2026**

The final stage extracted a smaller demonstration from the broader development tree.

The demonstration was derived from the independent `pipeline/` implementation.

The extraction removed components that were not required for the selected demonstration scope, including:

- The custom window implementation.
- The associated protocol code.
- The original build orchestration code.

A standalone CMake build structure and project documentation were added.

The DSP skeleton is provided as a prebuilt component in the demonstration build tree, allowing the demonstration to be built without requiring the complete Hexagon SDK toolchain on the target board.

Synthetic frame generation is retained for parity testing.

The extraction deliberately reduces the scope of the original development system. It is intended to demonstrate the camera-processing and CPU/DSP integration concepts without reproducing every experimental branch.

The extraction itself should not be presented as a new chronological engineering breakthrough. It is a packaging and reproducibility step based on the earlier work.

## 10. Development Scope and Exclusions

The development line includes several experimental branches that are not part of the final demonstration.

The following repositories are separate projects and should not be described as components of this camera/DSP development line:

- `Q6A-Hexagon-DSP-Benchmark`
- `qcs6490-resnet50-inference-benchmark`
- `hexagon-v68-test`

The final demonstration also does not claim to provide:

- A production ISP.
- A complete camera HAL.
- A general-purpose Qualcomm BSP.
- A kernel driver implementation.
- A production-grade thermal-management system.
- Full DSP fault recovery.
- Exhaustive validation of every sensor mode.

## 11. Development Outcome

The project progressed from a CPU-based image-processing prototype to a live camera pipeline with alternative CPU and Hexagon DSP processing engines.

The most significant engineering work involved the integration boundaries:

- Camera capture and frame ownership.
- Host-to-DSP memory access.
- FastRPC session lifecycle.
- HVX processing semantics.
- CPU/DSP numerical parity.
- GStreamer and Wayland surface lifetime.
- Reproducible performance and correctness testing.

The resulting codebase provides a practical engineering reference for camera and multimedia processing on the Qualcomm QCS6490 platform.
