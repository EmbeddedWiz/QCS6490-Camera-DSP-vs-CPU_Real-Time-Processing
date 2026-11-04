# How the two paths work

The project exists to answer one question with numbers: **what does the same
image cost on the CDSP and on the CPU?**

## One frame, start to finish

```
 V4L2 capture ──► dma-buf ──► engine ──► dma-buf ──► waylandsink
```

1. **Capture.** `v4l2_capture.cpp` queues buffers on `/dev/video9`, exports each
   one as a dma-buf, and waits in `epoll` for the CSI DMA-done interrupt.  No
   polling, no copy.
2. **Engine.** `dsp_engine.cpp` hands the buffer to FastRPC once (registered by
   file descriptor, not copied) and asks the DSP to render into an output
   surface.  `cpu_engine.cpp` does the same work in scalar C++ on the same
   pixels.
3. **Present.** `sink.cpp` wraps the output dma-buf in a `GstBuffer` with a
   `GstVideoMeta` that points at the descriptor, so `waylandsink` imports it
   instead of copying it.

Both engines write into dma-bufs from the same heap (`rpcmem`), so the sink
does not care which one produced the surface.

## The stages, in order

```
black level 16 → AGC gain → white balance → demosaic → box downscale → gamma
                                     └── filters (edge, emboss, …)
```

The order is not arbitrary:

- **Black first, gain second.** The pedestal is a property of the sensor and
  must not be amplified with the light.
- **Gamma last.** Downscaling sums *linear* samples and divides.  Gamma before
  the average would brighten the result, and the AGC — which measures the
  displayed green — would then chase its own tail.
- **AGC is a power of two**, so one step is a full stop: on the display curve
  that is a factor of 2^(1/2.2) ≈ 1.37, which is why the target band is
  100…140 rather than an octave wide.  The gain is applied while the row is
  still packed, where one saturating add covers 128 pixels.

## Why the CPU is bit-exact

The CPU engine is not an approximation.  It reproduces the hardware's integer
behaviour, including the parts that are easy to get wrong:

- the CDSP's averaging instruction truncates: `avg(0, 1)` is 0, not 1;
- a 2×2 demosaic estimate is `avg(avg(a, b), avg(c, d))`, not
  `(a + b + c + d + 2) >> 2`;
- the AGC gain is saturating doubling in 8 bits, exactly as the HVX kernel
  does it.

`--parity` renders the same frame through both engines and compares every
byte.  On the synthetic frame and on real captures it reports `worst 0`.

## One process, one DSP session

`pipe_hvx_open()` creates the FastRPC session, and the session is what the CDSP
does not survive in large numbers: a few dozen short-lived ones and the DSP
stops answering with an SMMU fault.  So the panel's `e`/`r`/`s` restart the
whole process instead of opening a second session, and `--parity` runs its
sixty cases inside one session on purpose.
