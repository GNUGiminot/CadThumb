#pragma once
#include "common/ImageIO.h"
#include "render/Mesh.h"

#include <string>

namespace ct {

struct RenderParams {
    int size = 256;          // output image is size x size
    int ssaa = 3;            // supersampling factor
    double yawDeg = 45.0;    // camera rotation around the up axis
    double pitchDeg = 30.0;  // camera elevation
    char upAxis = 'Z';       // 'Z' or 'Y'
    bool outline = true;     // silhouette + crease lines
    uint32_t background = 0; // 0xAARRGGBB, alpha 0 = transparent
    double margin = 0.04;    // empty border, fraction of size
};

// CPU rasterizer: z-buffer, two-sided lighting, supersampling, screen-space outlines.
// No GPU/D3D is used on purpose: it runs reliably in any session/service context.
bool RenderMesh(const Mesh& mesh, const RenderParams& params, Image& out, std::string& error);

} // namespace ct
