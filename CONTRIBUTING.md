# Contributing

Thanks for helping. This mod touches a closed-source renderer, so small-looking changes can regress unrelated scenes or only one eye.

## Before changing code

1. Reproduce the issue from a known-good build.
2. Record the exact game executable hash, checkpoint, headset/runtime, resolution, refresh rate, and GPU.
3. Identify the render path or state transition responsible. Avoid fixes based only on visual coincidence.
4. Preserve the verified baseline so a failed experiment can be rolled back immediately.

## Pull requests

A pull request should contain one coherent fix and explain:

- the root cause and evidence supporting it
- why the affected code owns the behavior
- all assumptions that remain unproven
- before/after reproduction steps
- paths checked for regressions, especially gameplay, menus, scripted scenes, both eyes, UI, hands, weapons, decals, particles, and lighting
- performance measurements when render work changes

Do not commit game assets, executable dumps, captured shaders other than the small replacement sources already represented here, videos, logs containing private information, crash dumps, test packages, or proprietary SDK files.

## Compatibility reports

Use the issue templates. A claim such as “flickers” or “slow” is rarely actionable without the runtime, headset, GPU/driver, render resolution, refresh rate, exact checkpoint, logs, and a video that identifies which eye or head motion triggers it.

## Code style

Keep changes narrow and consistent with the surrounding 3Dmigoto-derived C++ code. Prefer named state and documented invariants over unexplained constants. Tests that encode a discovered contract are strongly encouraged.

