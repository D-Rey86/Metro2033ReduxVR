# Performance investigation

## What is known

The game's age does not make VR rendering cheap. The established path duplicates significant rendering work for the second eye, while visibility changes needed for comfortable VR can increase the amount of geometry and effects submitted. High headset render resolutions and supersampling compound that cost.

Performance can also be shaped by SteamVR's motion smoothing/reprojection, headset refresh rate, compositor behavior, connection/encoding overhead, driver state, and Metro's own thread limits. An exact 60/72/90-style plateau is not by itself proof of an in-mod frame limiter.

## Current baseline

The reliable path is multi-pass stereo. Prior single-pass experiments did not provide a verified large gain and caused shader flicker, so they are not the production baseline. A replacement must be based on GPU/CPU timing and draw-path ownership rather than another global toggle.

## Useful measurements

For comparisons, hold these constant:

- exact checkpoint and camera position
- headset refresh rate and motion-smoothing mode
- SteamVR per-application resolution
- mod resolution scale
- Metro quality preset and relevant mod toggles
- GPU driver and VR connection method

Capture CPU frame time, GPU frame time, compositor misses/reprojection, submitted eye resolution, and a short stable interval. Report minima alone only with the full time series; transient lows are easy to misread.

## Candidate work

1. Attribute frame time between Metro CPU work, duplicated draw submission, GPU shading, and compositor/encoding.
2. Establish captures on at least one NVIDIA and one AMD system.
3. Classify passes that can safely share work between eyes and those that are inherently eye-dependent.
4. Implement a narrow candidate behind a disabled-by-default development setting.
5. Verify both eyes and regression-sensitive paths before comparing performance.

Expanded visibility should be measured separately. It is intended to reduce VR-visible pop-in; its performance cost and restart semantics should not be conflated with stereo architecture.

