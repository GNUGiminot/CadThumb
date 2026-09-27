#pragma once
#include "render/Mesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ct {

// Render-ready model for the interactive viewer: a triangle soup (per-vertex position, normal, color)
// plus feature edges. Always Z-up.
struct ViewModel {
    std::vector<float> pos;      // 3 per vertex, 3 vertices per triangle
    std::vector<float> nrm;      // 3 per vertex
    std::vector<uint8_t> col;    // RGBA per vertex
    std::vector<float> edges;    // x0,y0,z0,x1,y1,z1 per segment
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
    size_t triangles = 0;
    bool edgesSkipped = false;   // too many triangles for edge extraction
    size_t openEdges = 0;        // boundary edges: > 0 means the surface is not closed (volume is approximate)
    double volume = 0;           // model units^3 (mm^3), from the divergence theorem

    size_t VertexCount() const { return pos.size() / 3; }
};

// Converts a loaded mesh (upAxis 'Y' is rotated to Z-up). Smooth normals are kept when the mesh has them
// (STEP), otherwise faces are flat-shaded. Feature edges: boundaries and creases sharper than creaseDeg.
void BuildViewModel(Mesh&& mesh, char upAxis, float creaseDeg, ViewModel& out);

} // namespace ct
