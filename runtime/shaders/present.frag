#version 450

// Reconstructed from the previously embedded presentation SPIR-V.
// See docs/LICENSING.md for provenance and validation.
layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

void main() {
    color = texture(source_image, texcoord);
}
