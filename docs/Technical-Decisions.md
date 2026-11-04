# Technical Decisions and Architectural Rationale

## 1. Purpose

This document records the major architectural and implementation decisions made during the development of the QCS6490 camera and DSP processing pipeline.

Each decision describes:

- The selected approach.
- The engineering rationale.
- The evidence or observation supporting it.
- The associated trade-offs and limitations.

The decisions apply to different stages of the development. They should not all be interpreted as simultaneous requirements of every experimental branch or of the final standalone demonstration.

## 2. Processing Architecture

### TD-01 — Keep CPU and DSP Processing Paths Explicitly Separate

**Decision**

A frame is processed by either the CPU engine or the DSP engine. The processing path is selected explicitly rather than distributing individual operations between the two engines.

**Rationale**

The main objective was to make CPU and DSP behavior independently measurable and comparable.

Splitting a frame-processing sequence across multiple execution domains would introduce additional synchronization, data-transfer and intermediate-buffer requirements. It would also make performance attribution and numerical validation more difficult.

**Evidence**

The development established separate CPU and DSP engines and introduced a common parity-validation approach.

**Trade-offs**

- Some operations may be more efficient on a different execution unit.
- The design does not exploit arbitrary CPU/DSP task partitioning.
- The architecture favors predictable ownership and validation over maximum scheduling flexibility.

**Scope**

This decision describes the selected processing model. Earlier camera experiments included conventional libcamera-based paths and other exploratory implementations.

### TD-02 — Use the DSP Arithmetic as the CPU Reference Contract

**Decision**

The CPU implementation reproduces the arithmetic behavior of the DSP implementation, including truncation and byte-level semantics.

**Rationale**

A CPU reference is useful only if it validates the actual implementation being deployed.

Using mathematically ideal rounding where the DSP performs truncating arithmetic would produce false parity failures.

**Evidence**

The HVX unsigned-byte average instruction `Q6_Vub_vavg_VubVub` truncates.

The CPU reference was modified accordingly.

The recorded parity results included 488 comparisons with a maximum absolute difference of zero.

**Trade-offs**

The CPU implementation may not always represent the mathematically preferred image-processing formula. Its purpose is to reproduce the DSP result exactly.

### TD-03 — Treat Functional Parity as an Acceptance Criterion

**Decision**

CPU/DSP parity is part of the validation process, not an optional visual check.

The parity matrix begins with a no-filter identity case.

**Rationale**

Testing only complex filter combinations can hide defects in the underlying color pipeline or tone transformation.

A baseline identity case isolates fundamental processing behavior before additional operations are introduced.

**Evidence**

An earlier test matrix exposed failures associated with the tone curve. Introducing the identity baseline made the underlying regression visible.

The recorded validation included:

- 488 parity comparisons.
- Maximum absolute difference: zero.
- 32/32 geometry combinations passed.

**Trade-offs**

- Validation requires controlled input frames and reproducible configurations.
- Full sweeps can be expensive.
- The CDSP session failure pattern requires careful test planning.

**Scope**

The results apply to the recorded test configurations. They do not establish exhaustive parity for every possible sensor mode, scale and filter combination.

## 3. Color and Image-Processing Pipeline

### TD-04 — Use a Fixed Processing Order

**Decision**

The selected processing sequence is:

1. Black-level correction.
2. Gain.
3. White balance.
4. Downscaling.
5. Gamma correction.
6. Image filters.

**Rationale**

The sequence separates sensor-level correction and linear-domain operations from subsequent tone transformation and image filtering.

Downscaling is performed before gamma correction so that the averaging operation works on the earlier processing representation rather than on gamma-transformed values.

**Trade-offs**

The selected order is an engineering choice for this processing pipeline. It is not intended to reproduce every possible commercial ISP architecture.

### TD-05 — Use Fixed Color Parameters for Reproducibility

**Decision**

The reference configuration uses fixed values:

- Black-level pedestal: 16.
- White-balance gains: 319 and 556.
- Gamma: 2.2.

Automatic white balance is not included in the reference configuration.

**Rationale**

Fixed parameters reduce variation between test runs and simplify CPU/DSP comparison.

**Trade-offs**

The resulting color reproduction is not adaptive to changing illumination or different sensor conditions.

The values are reference parameters for the project, not calibrated universal values for the IMX219.

### TD-06 — Make AGC Behavior Explicit and Reproducible

**Decision**

Automatic gain control is enabled by default.

The `--no-agc` option freezes the current gain.

The gain adjustment uses power-of-two steps and a target average-green range of approximately 100–140 after gamma processing.

**Rationale**

A reproducible gain policy is required for meaningful comparisons between processing engines and test runs.

The same control semantics are implemented in the CPU and DSP paths.

**Trade-offs**

- AGC behavior must remain synchronized between the two implementations.
- Fixed gain may be more appropriate for controlled experiments.
- The target brightness range is a project-specific operating parameter, not a general camera exposure standard.

## 4. Output Geometry and Memory Layout

### TD-07 — Quantize Output Width to the Required Processing Geometry

**Decision**

The output width is calculated as:

`out_w = (width / scale) & ~31`

The output stride is:

`stride = out_w * 4`

**Rationale**

The processing implementation requires output geometry aligned to a multiple of 32 pixels.

Explicit stride calculation prevents the CPU, DSP and presentation paths from interpreting the output surface differently.

**Evidence**

All 32 recorded geometry combinations passed validation.

**Trade-offs**

The output may discard up to 31 columns on the right-hand side.

The design favors predictable processing geometry over preserving every input column.

### TD-08 — Use DMA-BUF-Based Buffer Exchange

**Decision**

The camera and presentation pipeline uses DMA-BUF-backed buffers where supported.

The capture path exports buffers through `VIDIOC_EXPBUF`. The presentation path wraps DMA-BUF-backed surfaces in GStreamer buffers and associates video metadata with them.

**Rationale**

DMA-BUF provides a mechanism for sharing buffer memory between compatible Linux subsystems without requiring every component to allocate and copy its own image data.

It is particularly useful for camera-to-processing-to-display integration.

**Evidence**

The CPU pipeline recorded 301 frames over five seconds at approximately 60 FPS in the tested configuration.

The implementation used DMA-BUF export and GStreamer DMA-BUF allocation/import mechanisms.

**Trade-offs and limitations**

DMA-BUF use does not, by itself, prove that the complete camera-to-display path is copy-free.

A driver, hardware block, GStreamer element or compositor may still perform internal copies or transformations.

The accurate claim is that the pipeline uses DMA-BUF-backed buffer sharing and is designed to avoid unnecessary application-level copies. Full end-to-end zero-copy behavior has not been independently proven.

## 5. Event Handling and Concurrency

### TD-09 — Use a Single Application-Level Event Loop for Capture and Control

**Decision**

The application uses an `epoll`-based event loop to handle camera events and terminal input.

The camera capture path is driven by DMA completion events.

**Rationale**

An event-driven application structure simplifies coordination between capture, processing and control operations.

It avoids introducing unnecessary application-level synchronization into the main frame-handling path.

**Trade-offs**

- The application does not gain additional processing parallelism from multiple application threads.
- Throughput depends on the execution time of the selected processing path.
- GStreamer, the kernel and the compositor may use their own internal threads.

**Scope**

This decision describes the application's event-handling model. It is not a claim that the complete Linux multimedia stack is single-threaded.

### TD-10 — Control DSP Session Lifetime Explicitly

**Decision**

The original pipeline design used one DSP session per process.

The DSP session was opened through the processing engine and kept alive for the lifetime of that processing instance.

Some control operations restarted the application through `execv` to establish a clean processing configuration.

**Rationale**

Repeated short-lived FastRPC sessions were associated with CDSP/SMMU failures after approximately 55–77 sessions in the observed tests.

Keeping the session lifetime explicit reduced unnecessary session churn.

**Trade-offs**

- Restarting the process resets application state and statistics unless explicitly preserved.
- Parameters must be propagated correctly to the new process.
- A DSP session cannot be treated as an unlimited, failure-free resource.

**Scope**

This describes the original pipeline control model. Later experimental window-control work introduced additional in-process behavior. The final extracted demonstration does not include the custom window and protocol implementation.

## 6. Project Separation and Code Organization

### TD-11 — Keep the New Pipeline Independent from the Earlier `fast-dsp` Implementation

**Decision**

The new `pipeline/` implementation is maintained independently from the earlier `fast-dsp/` experimental code.

The earlier implementation remains a reference rather than a runtime dependency.

**Rationale**

The new pipeline was intended to provide a clearer architecture and a controlled basis for CPU/DSP comparisons.

Reusing experimental code would have preserved earlier assumptions and made it harder to isolate new design decisions.

**Trade-offs**

Some processing logic is duplicated.

The CPU and DSP implementations require continued synchronization and parity testing.

### TD-12 — Treat a CDSP SMMU Failure as a Stop Condition

**Decision**

The observed CDSP/SMMU failure is treated as a reason to stop the current test sequence.

The application does not assume that the DSP will automatically recover.

**Rationale**

The experiments showed that remoteproc state alone cannot establish DSP liveness.

Continuing a test sequence against an unresponsive DSP can produce misleading results and secondary connection errors.

**Trade-offs**

A failure may interrupt a batch of tests.

Automatic recovery and safe-state handling remain unresolved.

**Scope**

This is a conservative test and demonstration policy, not a complete production fault-recovery design.

## 7. Camera Configuration

### TD-13 — Configure the Media Graph Through Linux Media Subsystem Interfaces

**Decision**

The selected pipeline configures the media graph through C++ ioctl operations.

The implementation uses interfaces including:

- `MEDIA_IOC_G_TOPOLOGY`
- `MEDIA_IOC_SETUP_LINK`
- Subdevice format configuration through `VIDIOC_SUBDEV_S_FMT`

The camera and CSI-related topology is inspected and configured through the Linux media subsystem.

**Rationale**

Keeping configuration within the application makes the required setup explicit and reproducible.

It also reduces reliance on external command-line utilities during runtime.

**Trade-offs**

The implementation is tied to the media topology and driver behavior of the target platform.

It is not a portable camera configuration layer for arbitrary SoCs.

**Scope**

External utilities such as `media-ctl` and `v4l2-ctl` remain useful for diagnostics. Their use during development does not imply that the final runtime depends on them.

## 8. Display Architecture

### TD-14 — Use an Application-Owned Window with a Wayland Video Subsurface

**Decision**

In the later custom-window branch, the application owns the Wayland toplevel window and the video sink renders into a subsurface.

Resizing recreates the video subsurface rather than transferring ownership of the toplevel to the video sink.

**Rationale**

The earlier approach exposed a window-management problem: when the video sink owned the toplevel, caps changes could interfere with keyboard focus.

Separating window ownership from video presentation provided more predictable control.

**Evidence**

The custom window mechanism was recorded as working on November 1, 2026.

**Trade-offs**

- Requires custom Wayland protocol and surface-management code.
- Adds complexity to window lifecycle management.
- The custom window implementation is not included in the extracted standalone demonstration.

**Scope**

This decision belongs to the broader development tree, not the reduced final demo.

## 9. Demonstration Scope

### TD-15 — Position the Project as an Engineering Reference, Not a Production Product

**Decision**

The project is presented as a technical reference and demonstration of embedded camera processing, CPU/DSP integration and multimedia pipeline engineering.

It is not presented as a production-ready camera product.

**Rationale**

The project establishes useful engineering results but does not include all the elements required for a commercial product.

Examples include exhaustive camera-mode validation, production ISP calibration, complete DSP recovery, thermal qualification and product-level reliability testing.

**Trade-offs**

Some potential product features remain outside the project scope.

The narrower scope makes the demonstrated results easier to reproduce and evaluate.

### TD-16 — Extract a Standalone Camera/DSP Demonstration

**Decision**

The final demonstration is extracted from the independent `pipeline/` implementation.

The extraction removes components not required by the selected demonstration, including:

- The custom window implementation.
- Its protocol code.
- The original build orchestration code.

A CMake-based build structure is provided.

The DSP skeleton is included as a prebuilt component in the demonstration build tree.

**Rationale**

The goal is to make the demonstration easier to build and evaluate without requiring the complete development environment on the target board.

Synthetic frame generation is retained for CPU/DSP parity testing.

**Trade-offs**

- The demonstration contains a reduced feature set.
- The extracted tree and the broader development tree must be kept consistent when shared processing logic changes.
- The prebuilt DSP component is tied to the relevant platform and toolchain configuration.

### TD-17 — Avoid Runtime Package-Installation Dependencies

**Decision**

The target-board demonstration is designed to use the software environment already available on the board.

The runtime does not depend on installing additional packages as part of its normal execution.

**Rationale**

A controlled target environment makes the demonstration easier to reproduce and reduces deployment uncertainty.

Where practical, functionality is implemented directly in C++ or omitted from the reduced demonstration.

**Trade-offs**

- The implementation may need to provide functionality that could otherwise be delegated to an external utility.
- The available software environment constrains the selected implementation.

**Scope clarification**

This is a target-board deployment policy. It does not mean that the development host or Hexagon build environment is prohibited from using the SDK, compiler or development dependencies required to produce the DSP component.

## 10. Buffer Ownership and Resource Lifecycle

### TD-18 — Make Surface Ownership Explicit Between the Engine and Sink

**Decision**

The processing engine transfers a surface to the sink through an explicit ownership interface:

`Sink::push(surface, done)`

The surface becomes reusable only after the sink invokes the completion callback.

The sink declares its required surface depth through `surfaces_needed()`.

The tested values were:

- Null sink: 2 surfaces.
- Wayland sink: 4 surfaces.

The implementation also disables GStreamer's last-sample retention and destroys the sink before destroying the processing engine.

**Rationale**

Submitting a frame does not mean that the display system has finished using its memory.

The sink or compositor may retain a reference after the application has submitted the frame.

Explicit ownership prevents the processing engine from overwriting a surface that is still in use.

**Evidence**

The initial two-surface configuration could deadlock. The revised ownership and surface-depth model resolved the observed problem.

**Trade-offs**

- The sink must correctly invoke the completion callback.
- The engine depends on accurate surface-lifetime reporting.
- Different sinks may require different surface-pool sizes.

## 11. Summary of Architectural Priorities

The decisions above reflect five recurring priorities:

1. **Measurability** — CPU and DSP behavior must be independently observable.
2. **Correctness** — numerical parity and buffer ownership must be validated, not assumed.
3. **Predictability** — session lifetime, event handling and surface reuse must be explicit.
4. **Reproducibility** — fixed test parameters and controlled build/runtime environments reduce ambiguity.
5. **Scope discipline** — the demonstration must distinguish implemented functionality from experimental work and future product requirements.

The resulting architecture is intended to be a technically inspectable reference for embedded camera acquisition, image processing, DSP acceleration and Linux multimedia integration on Qualcomm QCS6490.