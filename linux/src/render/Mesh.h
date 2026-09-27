#pragma once
#include <cstdint>
#include <vector>

namespace ct {

// Raises the luminance of a dark 0xRRGGBB color to at least minLum (0..1), keeping the hue.
// CAD exports often carry dark "steel" appearances that turn into black blobs at thumbnail size.
inline uint32_t LiftColor(uint32_t rgb, float minLum) {
    float r = ((rgb >> 16) & 0xFF) / 255.0f, g = ((rgb >> 8) & 0xFF) / 255.0f, b = (rgb & 0xFF) / 255.0f;
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum >= minLum || lum >= 0.999f) return rgb;
    float t = (minLum - lum) / (1.0f - lum);
    auto c = [&](float v) { return uint32_t((v + (1.0f - v) * t) * 255.0f + 0.5f); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

// Indexed triangle mesh prepared for thumbnail rendering.
struct Mesh {
    std::vector<float> pos;          // x,y,z per vertex
    std::vector<float> nrm;          // optional smooth normals (same count as pos) — empty = flat shading
    std::vector<uint32_t> idx;       // 3 indices per triangle
    std::vector<uint32_t> triColor;  // optional 0xRRGGBB per triangle — empty = defaultColor
    std::vector<float> lines;        // optional wireframe: x0,y0,z0,x1,y1,z1 per segment
    uint32_t defaultColor = 0xB9C3CE;

    size_t VertexCount() const { return pos.size() / 3; }
    size_t TriangleCount() const { return idx.size() / 3; }
    bool Empty() const { return idx.empty() && lines.empty(); }
    bool HasColors() const { return !triColor.empty(); }

    uint32_t AddVertex(float x, float y, float z) {
        pos.push_back(x);
        pos.push_back(y);
        pos.push_back(z);
        return uint32_t(pos.size() / 3 - 1);
    }
    void AddTriangle(uint32_t a, uint32_t b, uint32_t c) {
        idx.push_back(a);
        idx.push_back(b);
        idx.push_back(c);
    }
};

} // namespace ct
