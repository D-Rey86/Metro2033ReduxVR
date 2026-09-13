"""Verify clean-install weapon and supporting calibration data is compiled in."""

import hashlib
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "ThirdParty/3Dmigoto/DirectX11/VRPose.cpp").read_text()
DRAW_SOURCE = (ROOT / "ThirdParty/3Dmigoto/DirectX11/HackerContext.cpp").read_text()

start = SOURCE.index("static const char *kEmbeddedWeaponCalibrationLines[]")
end = SOURCE.index("\n\t};", start)
lines = re.findall(r'^\s*"(v4 [^"]+)",?$', SOURCE[start:end], re.MULTILINE)

assert len(lines) == 11
assert [int(line.split()[1]) for line in lines] == [
    100011229, 100009540, 6273, 16251, 100021948, 100010875,
    100019221, 100015222, 100014562, 100040740, 100012060,
]

for line in lines:
    fields = line.split()
    mesh_count = int(fields[9])
    assert len(fields[10:]) == mesh_count

snapshot = ("\n".join(lines) + "\n").encode()
assert hashlib.sha256(snapshot).hexdigest().upper() == (
    "2D5E801A3E7C0C9B89A0E55AE667A6661190821C10EE99472C9265FB3ECA00EC"
)

embedded_load = SOURCE.index(
    "for (const char *line : kEmbeddedWeaponCalibrationLines)", end
)
external_load = SOURCE.index(
    'fopen_s(&f, "vr_weapon_calibrations.txt", "r")', embedded_load
)
compaction = SOURCE.index("sSavedWeaponAdjusts.swap(compacted)", external_load)
assert embedded_load < external_load < compaction

for expected in (
    "static float sLeftHandAdjust[3] = { 0.004000f, 0.172000f, -0.178000f };",
    "1.149999f, -0.440000f, 0.650000f",
    "-0.907504f, 0.204000f, -0.456001f",
    "-0.110000f, -0.020000f, 0.000000f",
    "-0.073615f, 0.141142f, -0.058959f",
    "0.119332f, 0.114026f, 0.000000f",
    "static float sJournalAdjustScale = 0.605206f;",
):
    assert expected in SOURCE

mesh_start = DRAW_SOURCE.index("static const UINT kEmbeddedViewmodelIndexCounts[]")
mesh_end = DRAW_SOURCE.index("\n};", mesh_start)
embedded_meshes = [
    int(value)
    for value in re.findall(r"\b\d+\b", DRAW_SOURCE[mesh_start:mesh_end])
]
expected_meshes = [
    6, 18, 24, 36, 48, 60, 66, 69, 120, 162, 180, 228, 252, 288, 324,
    420, 570, 696, 750, 786, 810, 852, 888, 912, 942, 966, 1005, 1056,
    1188, 1224, 1296, 1380, 1410, 1479, 1500, 1692, 1782, 1866, 2160,
    2184, 2304, 2394, 2454, 2499, 2538, 2589, 2601, 2628, 2796, 2919,
    3000, 3072, 3075, 3330, 3924, 4008, 4176, 4320, 4716, 5292, 5508,
    5808, 5832, 6108, 6273, 6576, 6840, 7560, 8112, 9462, 9690, 9720,
    10875, 11229, 12132, 14544, 14562, 16251, 19221, 21252, 21948, 27618,
]
assert embedded_meshes == expected_meshes
embedded_mesh_load = DRAW_SOURCE.index(
    "for (UINT count : kEmbeddedViewmodelIndexCounts)", mesh_end
)
external_mesh_load = DRAW_SOURCE.index(
    'fopen_s(&f, kViewmodelMeshFile, "r")', embedded_mesh_load
)
assert embedded_mesh_load < external_mesh_load

print(
    "embedded release calibrations verified: 11 exact weapon profiles, "
    "82 viewmodel meshes, left hand, watch, and journal; external files "
    "remain optional overrides"
)
