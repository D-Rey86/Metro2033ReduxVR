# Roadmap

## Highest priority

1. Profile the stereo render path on representative NVIDIA and AMD systems.
2. Design and validate a correct stereo optimization, ideally single-pass where the engine and shaders permit it, without reintroducing shader flicker or eye divergence.
3. Reproduce and fix headset/runtime-specific background flicker and menu double vision using captures from affected systems.
4. Validate additional storefront executable builds and make unsupported hashes fail clearly.

## Compatibility and polish

- establish an AMD test matrix and diagnostic workflow
- improve automatic compatibility reporting while keeping logs privacy-conscious
- verify resolution behavior across common headset aspect ratios
- investigate the localized sewer-water lower-view clipping
- investigate gun-range-only projectile/aim disagreement if it reproduces outside that area
- expand clean-install and uninstall tests

## Release readiness

- repeatable CI build and focused unit/static tests
- documented release packaging from a clean checkout
- signed or reproducibly hashed public binaries
- broader checkpoint regression pass
- a versioned first public test release after compatibility blockers are understood

