from pathlib import Path
import hashlib
root=Path(__file__).resolve().parents[1]
src=(root/'data/original-6-zone.effect').read_bytes()
wrapped=(root/'data/6-zone.effect').read_bytes()
body=wrapped.split(b'// BEGIN ORIGINAL SHADER\n',1)[1].split(b'\n// END ORIGINAL SHADER',1)[0]
assert body==src, 'Original shader body changed'
expected=(root/'data/original.sha256').read_text().split()[0]
assert hashlib.sha256(src).hexdigest()==expected, 'Original file changed'
assert body.count(b'correctedUV = distortZone(')==6
assert body.count(b'image.Sample(')==1
assert b'correctedUV.x /= ar;' in body and b'correctedUV.x *= ar;' in body
# The dynamic face-point shader keeps the same distortion math and aspect correction but drives a
# zone array instead of six hard-coded zones.
fp=(root/'data/face-points.effect').read_text()
assert 'uniform float4 zone_data[' in fp, 'face-points.effect must declare the zone_data array'
assert 'zone_count' in fp, 'face-points.effect must use zone_count'
assert 'distortZone(' in fp and 'smoothstep' in fp and 'pow(percent' in fp, 'distortion math changed'
assert fp.count('image.Sample(')>=1
assert 'correctedUV.x /= ar;' in fp, 'aspect correction missing'
assert ('correctedUV.x *= ar;' in fp) or ('correctedUV.x * ar' in fp), 'aspect un-correction missing'
assert 'blur_faces' in fp and 'debug_points' in fp, 'blur/debug options missing'
assert 'uniform float4 face_box[' in fp and 'uniform float4 marker_data[' in fp, 'blur/debug arrays missing'
assert 'float3 markerColor(' in fp, 'debug marker colour map missing'
assert 'uniform float4 marker_data[64]' in fp, 'marker array must fit anchors + landmarks per face'
# The effect compiles as HLSL for D3D11, not GLSL: GLSL-only intrinsics fail at runtime with
# "undeclared identifier". Keep them out (use lerp instead of mix, etc.).
for glsl_only in ('mix(', 'fract(', 'texture('):
    assert glsl_only not in fp, f'GLSL-only intrinsic not valid in HLSL: {glsl_only}'
print('PASS: original shader bytes, 6 calls, texture samples, aspect correction; face-points shader (morph + independent blur/debug)')
