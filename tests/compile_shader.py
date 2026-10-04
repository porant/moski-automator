"""Compile data/face-points.effect with fxc so HLSL errors are caught in CI.

verify_shader.py only greps the source, so a shader that fails to compile (and therefore silently
leaves the OBS filter with no parameters and no effect) slips through. This test mirrors what OBS
does: it adapts the OBS-specific .effect syntax (texture2d, sampler_state, : TARGET, technique) to
plain HLSL and compiles the pixel shader with the Windows SDK fxc (the same D3DCompiler OBS uses).

Skips with a notice when fxc is unavailable (non-Windows or no Windows SDK installed).
"""

import glob
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fx = root / "data" / "face-points.effect"
src = fx.read_text(encoding="utf-8")

# --- Adapt OBS .effect syntax to plain HLSL that fxc understands -------------------------------
src = re.sub(r"uniform\s+texture2d\s+(\w+)\s*(?::\s*\w+)?\s*;", r"Texture2D \1;", src)
src = re.sub(r"sampler_state\s+\w+\s*\{[^{}]*\}\s*;", "SamplerState textureSampler;", src)
src = src.replace(": TARGET", ": SV_Target")
# The technique/pass wrapper is OBS glue; fxc gets the entry point via /E instead.
src = re.sub(r"technique\s+\w+\s*\{[^{}]*\{[^{}]*\}[^{}]*\}\s*$", "", src, flags=re.S)

for leftover in ("texture2d", "sampler_state", "technique"):
    assert leftover not in src, f"could not adapt OBS syntax away: {leftover}"
assert ": SV_Target" in src, "pixel shader entry point not found"


def find_fxc():
    patterns = [
        r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe",
        r"C:\Program Files\Windows Kits\10\bin\*\x64\fxc.exe",
    ]
    for pat in patterns:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]  # newest SDK version sorts last
    return None


fxc = find_fxc()
if fxc is None:
    print("SKIP: fxc.exe not found; install the Windows SDK to validate the shader")
    sys.exit(0)

work = Path(tempfile.mkdtemp(prefix="opa-shader-"))
hlsl = work / "face-points.hlsl"
hlsl.write_text(src, encoding="utf-8")

result = subprocess.run(
    [fxc, "/nologo", "/T", "ps_5_0", "/E", "mainImage", "/Fo", str(work / "out.fxo"), str(hlsl)],
    capture_output=True,
    text=True,
)
if result.returncode != 0:
    sys.stderr.write(result.stdout + result.stderr)
    sys.exit(f"fxc failed to compile {fx.name} (the filter would have no parameters and no effect)")

print(f"PASS: {fx.name} compiles as ps_5_0 with {fxc}")
