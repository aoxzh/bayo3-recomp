#version 450

// Reconstructed from the previously embedded presentation SPIR-V.
// See docs/LICENSING.md for provenance and validation.
layout(push_constant) uniform PresentMap {
    vec4 matrix_columns;
    vec2 offset;
} map;

layout(location = 0) out vec2 texcoord;

void main() {
    vec2 vertex_uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(vertex_uv * 2.0 - 1.0, 0.0, 1.0);
    mat2 linear_map = mat2(map.matrix_columns.xy, map.matrix_columns.zw);
    texcoord = linear_map * vertex_uv + map.offset;
}
