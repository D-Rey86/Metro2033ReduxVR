# Metro shader replacements

This directory contains only the six replacement pixel-shader sources used by the current mod package. Captured shaders and FrameAnalysis data are intentionally excluded.

The filenames preserve 3Dmigoto's shader-hash convention. Build/package tooling expects matching compiled files ending in `-ps_replace.bin` inside the runtime `ShaderFixes` directory.

Changes to these shaders require in-headset verification in both eyes. In particular, recheck decals, particles, lighting occlusion, distant geometry, menus, and scripted scenes.

