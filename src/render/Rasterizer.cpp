#include "render/Rasterizer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace ct {

namespace {

struct V3 {
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(V3 a) {
    float l = Len(a);
    return l > 1e-20f ? a * (1.0f / l) : V3{0, 0, 0};
}

constexpr float kEmpty = -std::numeric_limits<float>::infinity();
constexpr uint32_t kNormalValid = 0x01000000u;
constexpr uint32_t kLinePixel = 0x02000000u;

inline uint32_t PackNormal(V3 n) {
    auto q = [](float v) { return uint32_t(std::clamp(int(std::lround(v * 127.0f)) + 128, 0, 255)); };
    return kNormalValid | q(n.x) | (q(n.y) << 8) | (q(n.z) << 16);
}
inline V3 UnpackNormal(uint32_t p) {
    return {((p & 0xFF) - 128) / 127.0f, (((p >> 8) & 0xFF) - 128) / 127.0f, (((p >> 16) & 0xFF) - 128) / 127.0f};
}

inline uint32_t Scale(uint32_t rgb, float k) {
    int r = int(((rgb >> 16) & 0xFF) * k), g = int(((rgb >> 8) & 0xFF) * k), b = int((rgb & 0xFF) * k);
    return (uint32_t(std::clamp(r, 0, 255)) << 16) | (uint32_t(std::clamp(g, 0, 255)) << 8) |
           uint32_t(std::clamp(b, 0, 255));
}

struct Camera {
    V3 d, r, u;       // to-camera, screen right, screen up
    V3 l1, l2, h1;    // lights and half vector
    bool yUp = false;
    V3 Model(const float* p) const { return yUp ? V3{p[0], -p[2], p[1]} : V3{p[0], p[1], p[2]}; }
};

} // namespace

bool RenderMesh(const Mesh& mesh, const RenderParams& prm, Image& out, std::string& error) {
    if (mesh.Empty()) {
        error = "empty mesh";
        return false;
    }
    const int ss = std::clamp(prm.ssaa, 1, 4);
    const int W = std::clamp(prm.size, 16, 2048) * ss;
    const int H = W;

    Camera cam;
    cam.yUp = prm.upAxis == 'Y' || prm.upAxis == 'y';
    const float yaw = float(prm.yawDeg * 3.14159265358979 / 180.0);
    const float pitch = float(prm.pitchDeg * 3.14159265358979 / 180.0);
    cam.d = Norm({std::cos(pitch) * std::sin(yaw), -std::cos(pitch) * std::cos(yaw), std::sin(pitch)});
    V3 f = -cam.d;
    cam.r = Norm(Cross(f, {0, 0, 1}));
    if (Len(cam.r) < 0.5f) cam.r = {1, 0, 0};
    cam.u = Norm(Cross(cam.r, f));
    cam.l1 = Norm(cam.r * -0.45f + cam.u * 0.65f + cam.d * 0.6f);
    cam.l2 = Norm(cam.r * 0.7f + cam.u * -0.25f + cam.d * 0.45f);
    cam.h1 = Norm(cam.l1 + cam.d);

    // ---- fit to view
    float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
    auto extend = [&](const float* p) {
        V3 q = cam.Model(p);
        float sx = Dot(q, cam.r), sy = Dot(q, cam.u), sz = Dot(q, cam.d);
        if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz)) return;
        minX = std::min(minX, sx), maxX = std::max(maxX, sx);
        minY = std::min(minY, sy), maxY = std::max(maxY, sy);
        minZ = std::min(minZ, sz), maxZ = std::max(maxZ, sz);
    };
    if (!mesh.idx.empty()) {
        for (size_t i = 0; i < mesh.pos.size(); i += 3) extend(&mesh.pos[i]);
    }
    for (size_t i = 0; i < mesh.lines.size(); i += 3) extend(&mesh.lines[i]);
    if (minX > maxX) {
        error = "no finite geometry";
        return false;
    }
    float span = std::max(maxX - minX, maxY - minY);
    if (!(span > 0)) span = 1.0f;
    const float scale = float(W * (1.0 - 2.0 * prm.margin)) / span;
    const float ox = W * 0.5f - (minX + maxX) * 0.5f * scale;
    const float oy = H * 0.5f + (minY + maxY) * 0.5f * scale;
    const float depthRange = std::max(maxZ - minZ, 1e-6f);

    std::vector<float> depth(size_t(W) * H, kEmpty);
    std::vector<uint32_t> color(size_t(W) * H, 0);
    std::vector<uint32_t> nbuf(size_t(W) * H, 0);

    auto shade = [&](V3 n, uint32_t base) -> uint32_t {
        float ndl1 = std::max(0.0f, Dot(n, cam.l1));
        float ndl2 = std::max(0.0f, Dot(n, cam.l2));
        float ndv = std::max(0.0f, Dot(n, cam.d));
        float spec = std::pow(std::max(0.0f, Dot(n, cam.h1)), 48.0f) * 0.22f;
        float k = 0.30f + 0.52f * ndl1 + 0.16f * ndl2 + 0.14f * ndv;
        float r = ((base >> 16) & 0xFF) / 255.0f * k + spec;
        float g = ((base >> 8) & 0xFF) / 255.0f * k + spec;
        float b = (base & 0xFF) / 255.0f * k + spec;
        auto c = [](float v) { return uint32_t(std::clamp(int(v * 255.0f + 0.5f), 0, 255)); };
        return (c(r) << 16) | (c(g) << 8) | c(b);
    };

    // ---- triangles (parallel over horizontal bands; every band walks all triangles)
    const size_t triCount = mesh.TriangleCount();
    const bool smooth = mesh.nrm.size() == mesh.pos.size();
    const bool colored = mesh.triColor.size() == triCount;
    const uint32_t nv = uint32_t(mesh.VertexCount());

    auto rasterBand = [&](int bandY0, int bandY1) {
        for (size_t t = 0; t < triCount; ++t) {
            uint32_t i0 = mesh.idx[t * 3], i1 = mesh.idx[t * 3 + 1], i2 = mesh.idx[t * 3 + 2];
            if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
            V3 q0 = cam.Model(&mesh.pos[i0 * 3]), q1 = cam.Model(&mesh.pos[i1 * 3]), q2 = cam.Model(&mesh.pos[i2 * 3]);
            float y0s = oy - Dot(q0, cam.u) * scale, y1s = oy - Dot(q1, cam.u) * scale, y2s = oy - Dot(q2, cam.u) * scale;
            int pyMin = std::max(bandY0, int(std::floor(std::min({y0s, y1s, y2s}))));
            int pyMax = std::min(bandY1 - 1, int(std::ceil(std::max({y0s, y1s, y2s}))));
            if (pyMin > pyMax) continue;
            float x0s = ox + Dot(q0, cam.r) * scale, x1s = ox + Dot(q1, cam.r) * scale, x2s = ox + Dot(q2, cam.r) * scale;
            int pxMin = std::max(0, int(std::floor(std::min({x0s, x1s, x2s}))));
            int pxMax = std::min(W - 1, int(std::ceil(std::max({x0s, x1s, x2s}))));
            if (pxMin > pxMax) continue;
            float area = (x1s - x0s) * (y2s - y0s) - (x2s - x0s) * (y1s - y0s);
            if (!(std::fabs(area) > 1e-12f)) continue;
            float invA = 1.0f / area;
            float z0 = Dot(q0, cam.d), z1 = Dot(q1, cam.d), z2 = Dot(q2, cam.d);

            V3 fn = Norm(Cross(q1 - q0, q2 - q0));
            if (Len(fn) < 0.5f) fn = cam.d;
            V3 n0{}, n1{}, n2{};
            if (smooth) {
                n0 = cam.Model(&mesh.nrm[i0 * 3]);
                n1 = cam.Model(&mesh.nrm[i1 * 3]);
                n2 = cam.Model(&mesh.nrm[i2 * 3]);
            }
            const uint32_t base = colored ? mesh.triColor[t] : mesh.defaultColor;

            for (int py = pyMin; py <= pyMax; ++py) {
                float cy = py + 0.5f;
                for (int px = pxMin; px <= pxMax; ++px) {
                    float cx = px + 0.5f;
                    float b0 = ((x2s - x1s) * (cy - y1s) - (y2s - y1s) * (cx - x1s)) * invA;
                    float b1 = ((x0s - x2s) * (cy - y2s) - (y0s - y2s) * (cx - x2s)) * invA;
                    float b2 = 1.0f - b0 - b1;
                    if (b0 < 0 || b1 < 0 || b2 < 0) continue;
                    float z = b0 * z0 + b1 * z1 + b2 * z2;
                    size_t id = size_t(py) * W + px;
                    if (z <= depth[id]) continue;
                    depth[id] = z;
                    V3 n = fn;
                    if (smooth) {
                        V3 s = Norm(n0 * b0 + n1 * b1 + n2 * b2);
                        if (Len(s) > 0.5f) n = s;
                    }
                    if (Dot(n, cam.d) < 0) n = -n;
                    color[id] = shade(n, base);
                    nbuf[id] = PackNormal(n);
                }
            }
        }
    };

    int threads = int(std::clamp<unsigned>(std::thread::hardware_concurrency(), 1u, 8u));
    if (triCount < 5000) threads = 1;
    if (threads == 1) {
        rasterBand(0, H);
    } else {
        std::vector<std::thread> pool;
        int band = (H + threads - 1) / threads;
        for (int i = 0; i < threads; ++i) {
            int a = i * band, b = std::min(H, a + band);
            if (a < b) pool.emplace_back(rasterBand, a, b);
        }
        for (auto& th : pool) th.join();
    }

    // ---- wireframe (used when a STEP file has only curves)
    if (!mesh.lines.empty()) {
        const uint32_t lineColor = triCount ? 0x2E3640 : 0x3A4A5C;
        const int half = std::max(0, (ss - 1) / 2);
        const float bias = depthRange * 0.003f;
        for (size_t i = 0; i + 5 < mesh.lines.size(); i += 6) {
            V3 a = cam.Model(&mesh.lines[i]), b = cam.Model(&mesh.lines[i + 3]);
            float ax = ox + Dot(a, cam.r) * scale, ay = oy - Dot(a, cam.u) * scale, az = Dot(a, cam.d);
            float bx = ox + Dot(b, cam.r) * scale, by = oy - Dot(b, cam.u) * scale, bz = Dot(b, cam.d);
            int steps = int(std::ceil(std::max(std::fabs(bx - ax), std::fabs(by - ay)))) + 1;
            if (steps > 4 * W) steps = 4 * W;
            for (int s = 0; s <= steps; ++s) {
                float t = float(s) / steps;
                int x = int(ax + (bx - ax) * t), y = int(ay + (by - ay) * t);
                float z = az + (bz - az) * t + bias;
                for (int dy = -half; dy <= half; ++dy)
                    for (int dx = -half; dx <= half; ++dx) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                        size_t id = size_t(yy) * W + xx;
                        if (z < depth[id]) continue;
                        depth[id] = std::max(depth[id], z - bias);
                        color[id] = lineColor;
                        nbuf[id] = kLinePixel;
                    }
            }
        }
    }

    // ---- screen-space outlines: silhouette / depth discontinuity / crease
    std::vector<uint32_t> finalColor;
    const std::vector<uint32_t>* src = &color;
    if (prm.outline && triCount) {
        finalColor = color;
        const float depthThr = std::max(depthRange * 0.02f, 3.0f / scale);
        const float creaseCos = 0.79f; // ~38 degrees
        const int ks = std::max(1, (ss + 1) / 2);
        static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        auto outlineRows = [&](int r0, int r1) {
            for (int y = r0; y < r1; ++y) {
                for (int x = 0; x < W; ++x) {
                    size_t id = size_t(y) * W + x;
                    if (depth[id] == kEmpty || (nbuf[id] & kLinePixel)) continue;
                    bool sil = false, crease = false;
                    V3 n = UnpackNormal(nbuf[id]);
                    for (auto& dv : dirs) {
                        for (int k = 1; k <= ks && !sil; ++k) {
                            int xx = x + dv[0] * k, yy = y + dv[1] * k;
                            if (xx < 0 || yy < 0 || xx >= W || yy >= H) {
                                sil = true;
                                break;
                            }
                            size_t q = size_t(yy) * W + xx;
                            if (depth[q] == kEmpty || depth[q] - depth[id] > depthThr) sil = true;
                            else if (k == 1 && (nbuf[q] & kNormalValid) && Dot(n, UnpackNormal(nbuf[q])) < creaseCos)
                                crease = true;
                        }
                        if (sil) break;
                    }
                    if (sil) finalColor[id] = Scale(color[id], 0.32f);
                    else if (crease) finalColor[id] = Scale(color[id], 0.58f);
                }
            }
        };
        if (threads == 1) {
            outlineRows(0, H);
        } else {
            std::vector<std::thread> pool;
            int band = (H + threads - 1) / threads;
            for (int i = 0; i < threads; ++i) {
                int a = i * band, b = std::min(H, a + band);
                if (a < b) pool.emplace_back(outlineRows, a, b);
            }
            for (auto& th : pool) th.join();
        }
        src = &finalColor;
    }

    // ---- downsample with coverage -> alpha
    const int S = W / ss;
    out.resize(S, S);
    const bool opaqueBg = (prm.background >> 24) != 0;
    const float bgR = float((prm.background >> 16) & 0xFF), bgG = float((prm.background >> 8) & 0xFF), bgB = float(prm.background & 0xFF);
    bool any = false;
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            int cov = 0;
            float r = 0, g = 0, b = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    size_t id = size_t(y * ss + sy) * W + (x * ss + sx);
                    if (depth[id] == kEmpty) continue;
                    uint32_t c = (*src)[id];
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                    ++cov;
                }
            uint8_t* p = &out.px[(size_t(y) * S + x) * 4];
            float a = float(cov) / float(ss * ss);
            if (cov) {
                any = true;
                r /= cov, g /= cov, b /= cov;
            }
            if (opaqueBg) {
                r = r * a + bgR * (1 - a);
                g = g * a + bgG * (1 - a);
                b = b * a + bgB * (1 - a);
                a = 1.0f;
            }
            p[0] = uint8_t(std::clamp(b + 0.5f, 0.0f, 255.0f));
            p[1] = uint8_t(std::clamp(g + 0.5f, 0.0f, 255.0f));
            p[2] = uint8_t(std::clamp(r + 0.5f, 0.0f, 255.0f));
            p[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    if (!any) {
        error = "nothing visible";
        return false;
    }
    return true;
}

} // namespace ct
