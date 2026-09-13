# ARKTIKA.1 module-diff capture result

Date: 2026-08-17

## Capture

An automatic monitor watched the loaded ARKTIKA.1 main-module writable data across a restart, intro, gun range, and pistol use. It required no focus changes or in-game diagnostic input.

## Result

The module data changed heavily during initialization and continued to show allocator, animation, timing, and scene-state churn. No clean shot-only field could be identified from the module `.data` section. This is expected for an object-oriented engine where active weapon and ray data live in heap objects.

## Consequence

Do not repeat module-wide data diffs. The next capture must hook or observe the live weapon/fire call and its heap arguments, then record the pre-trace origin, direction, spread, range, and hit result directly.

The result does not invalidate ARKTIKA as a reference; it only rules out this particular passive capture method.

## Additional static check

The executable contains a CodeView PDB reference (`D:\bin_x64\arktika1.pdb`), but the matching PDB is not present in the supplied game folder or the local project folders. The firing-related strings also have no direct RIP-relative code references or embedded pointer references in the executable. Symbol-based or string-xref-based hooking is therefore unavailable; the next probe must discover the live path at runtime.
