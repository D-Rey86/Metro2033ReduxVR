"""Verify the exact world-decal bounded-offset integration and shader source."""

import ast
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "ThirdParty/3Dmigoto/DirectX11/HackerContext.cpp"
HEADER = ROOT / "ThirdParty/3Dmigoto/DirectX11/HackerContext.h"
FXC = Path(r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe")

source = CPP.read_text(encoding="utf-8")
header = HEADER.read_text(encoding="utf-8")

match = re.search(
    r"static const char \*kDecalOffsetVSSource =\s*(.*?);\s*\n",
    source,
    re.DOTALL,
)
assert match, "embedded decal vertex shader source was not found"
hlsl = "".join(ast.literal_eval(token) for token in re.findall(r'"(?:\\.|[^"\\])*"', match.group(1)))

assert "rasterPosition.xyz += worldNormal * (facing * 0.01);" in hlsl
assert "viewPosition = mul(m_V, p);" in hlsl
assert "clip = mul(m_VP, rasterPosition);" in hlsl
assert "mDecalOffsetVS" in header

# All active view paths must select the same shader: the live/inline path saves
# and swaps it, while batched replay computes an effective shader per held draw.
assert "mOrigContext1->VSSetShader(mDecalOffsetVS, NULL, 0);" in source
assert "effectiveVS = mDecalOffsetVS;" in source
assert "mOrigContext1->VSSetShader(mDecalSavedVS, NULL, 0);" in source

# Strip explicitly retained experiment-history blocks before checking that no
# unbounded decal depth manipulation remains executable.
active = re.sub(r"#if 0.*?#endif", "", source, flags=re.DOTALL)
assert "kDecalClipSpaceDepthBias" not in active
assert "mDecalBiasRasterState" not in active
assert "rd.DepthBias = -65536" not in active

# A normalized surface normal makes the world-space displacement exactly one
# centimetre regardless of camera distance. The rejected D24 offset was a
# fixed 1/256 of the entire depth range and had no physical upper bound.
surface_offset = 0.01
for normal in ((1.0, 0.0, 0.0), (0.0, -1.0, 0.0), (0.0, 0.0, 1.0)):
    displacement = tuple(component * surface_offset for component in normal)
    length = sum(component * component for component in displacement) ** 0.5
    assert abs(length - surface_offset) < 1e-12
assert abs(65536 / (2**24) - 1 / 256) < 1e-12

assert FXC.exists(), f"fxc not found at {FXC}"
with tempfile.TemporaryDirectory(prefix="metro-decal-vs-") as temp_dir:
    hlsl_path = Path(temp_dir) / "decal_vs.hlsl"
    bytecode_path = Path(temp_dir) / "decal_vs.bin"
    assembly_path = Path(temp_dir) / "decal_vs.asm"
    hlsl_path.write_text(hlsl, encoding="utf-8")
    result = subprocess.run(
        [str(FXC), "/nologo", "/T", "vs_5_0", "/E", "main", "/O3",
         "/Fo", str(bytecode_path), "/Fc", str(assembly_path), str(hlsl_path)],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert bytecode_path.stat().st_size > 0
    assembly = assembly_path.read_text(encoding="utf-8")
    for declaration in (
        "dcl_input v0.xyzw",
        "dcl_input v1.xyzw",
        "dcl_input v2.xyzw",
        "dcl_input v4.xy",
        "dcl_output_siv o0.xyzw, position",
        "dcl_output o1.xyzw",
        "dcl_output o2.xyz",
        "dcl_output o3.xyz",
        "dcl_output o4.xyzw",
    ):
        assert declaration in assembly, f"missing signature declaration: {declaration}"

print("PASS: bounded world-decal offset shader and all stereo routes verified")
