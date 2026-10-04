"""Add only the standalone libobs host wrapper; preserve original body byte for byte."""
from pathlib import Path
import hashlib
root=Path(__file__).resolve().parents[1]
original=(root/'data/original-6-zone.effect').read_bytes()
prefix=b'''// Standalone libobs wrapper. Original shader follows verbatim.
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 uv_size;
uniform float elapsed_time;
sampler_state textureSampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};
struct VertData {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};
VertData VSDefault(VertData v) {
    v.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
    return v;
}
// BEGIN ORIGINAL SHADER\n'''
suffix=b'''\n// END ORIGINAL SHADER
technique Draw {
    pass {
        vertex_shader = VSDefault(v);
        pixel_shader = mainImage(v_in);
    }
}
'''
(root/'data/6-zone.effect').write_bytes(prefix+original+suffix)
assert (root/'data/6-zone.effect').read_bytes()[len(prefix):-len(suffix)]==original
(root/'data/original.sha256').write_text(hashlib.sha256(original).hexdigest()+'  original-6-zone.effect\n')
print('Shader preserved verbatim:',hashlib.sha256(original).hexdigest())
