#include "viewer/ViewModel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <execution>

namespace ct {

namespace {

constexpr size_t kMaxEdgeTriangles = 6'000'000;

struct V3 {
    float x, y, z;
};
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Norm(V3 a) {
    float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return l > 1e-30f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1};
}

inline V3 At(const std::vector<float>& v, uint32_t i) { return {v[i * 3], v[i * 3 + 1], v[i * 3 + 2]}; }

// Welds coincident vertices (quantized positions) -> index of the first vertex at that position.
std::vector<uint32_t> Weld(const std::vector<float>& pos, float quantum) {
    const uint32_t nv = uint32_t(pos.size() / 3);
    std::vector<uint32_t> canon(nv);
    size_t cap = 1;
    while (cap < size_t(nv) * 2) cap <<= 1;
    std::vector<uint32_t> table(cap, UINT32_MAX);
    const size_t mask = cap - 1;
    const float inv = 1.0f / quantum;
    auto q = [&](uint32_t i, int64_t& a, int64_t& b, int64_t& c) {
        a = (int64_t)std::llround(pos[i * 3] * inv);
        b = (int64_t)std::llround(pos[i * 3 + 1] * inv);
        c = (int64_t)std::llround(pos[i * 3 + 2] * inv);
    };
    for (uint32_t i = 0; i < nv; ++i) {
        int64_t a, b, c;
        q(i, a, b, c);
        uint64_t h = (uint64_t)a * 0x9E3779B97F4A7C15ull ^ (uint64_t)b * 0xC2B2AE3D27D4EB4Full ^
                     (uint64_t)c * 0x165667B19E3779F9ull;
        h ^= h >> 29;
        size_t slot = h & mask;
        for (;;) {
            uint32_t j = table[slot];
            if (j == UINT32_MAX) {
                table[slot] = i;
                canon[i] = i;
                break;
            }
            int64_t a2, b2, c2;
            q(j, a2, b2, c2);
            if (a == a2 && b == b2 && c == c2) {
                canon[i] = j;
                break;
            }
            slot = (slot + 1) & mask;
        }
    }
    return canon;
}

size_t FeatureEdges(const Mesh& m, float creaseDeg, float quantum, std::vector<float>& out) {
    size_t open = 0;
    const size_t nt = m.TriangleCount();
    std::vector<uint32_t> canon = Weld(m.pos, quantum);
    std::vector<V3> fn(nt);
    for (size_t t = 0; t < nt; ++t) {
        V3 a = At(m.pos, canon[m.idx[t * 3]]), b = At(m.pos, canon[m.idx[t * 3 + 1]]), c = At(m.pos, canon[m.idx[t * 3 + 2]]);
        fn[t] = Norm(Cross(Sub(b, a), Sub(c, a)));
    }
    struct E {
        uint64_t key;
        uint32_t tri;
    };
    std::vector<E> edges;
    edges.reserve(nt * 3);
    for (size_t t = 0; t < nt; ++t) {
        uint32_t v[3] = {canon[m.idx[t * 3]], canon[m.idx[t * 3 + 1]], canon[m.idx[t * 3 + 2]]};
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue; // degenerate
        for (int k = 0; k < 3; ++k) {
            uint32_t a = v[k], b = v[(k + 1) % 3];
            if (a > b) std::swap(a, b);
            edges.push_back({(uint64_t(a) << 32) | b, uint32_t(t)});
        }
    }
    std::sort(std::execution::par_unseq, edges.begin(), edges.end(),
              [](const E& x, const E& y) { return x.key < y.key; });
    const float cosT = std::cos(creaseDeg * 3.14159265f / 180.0f);
    auto emit = [&](uint64_t key) {
        uint32_t a = uint32_t(key >> 32), b = uint32_t(key);
        out.insert(out.end(), {m.pos[a * 3], m.pos[a * 3 + 1], m.pos[a * 3 + 2], m.pos[b * 3], m.pos[b * 3 + 1],
                               m.pos[b * 3 + 2]});
    };
    for (size_t i = 0; i < edges.size();) {
        size_t j = i + 1;
        while (j < edges.size() && edges[j].key == edges[i].key) ++j;
        size_t n = j - i;
        if (n == 1) {
            emit(edges[i].key); // open boundary
            ++open;
        } else if (n == 2) {
            V3 a = fn[edges[i].tri], b = fn[edges[i + 1].tri];
            // abs(): tolerate inconsistently oriented STL triangles
            if (std::fabs(a.x * b.x + a.y * b.y + a.z * b.z) < cosT) emit(edges[i].key);
        } else {
            emit(edges[i].key); // non-manifold
        }
        i = j;
    }
    return open;
}

} // namespace

void BuildViewModel(Mesh&& m, char upAxis, float creaseDeg, ViewModel& out) {
    out = ViewModel{};
    const bool smooth = m.nrm.size() == m.pos.size();

    // Y-up -> Z-up: (x, y, z) -> (x, -z, y)
    if (upAxis == 'Y' || upAxis == 'y') {
        auto rot = [](std::vector<float>& v) {
            for (size_t i = 0; i + 2 < v.size(); i += 3) {
                float y = v[i + 1], z = v[i + 2];
                v[i + 1] = -z;
                v[i + 2] = y;
            }
        };
        rot(m.pos);
        rot(m.nrm);
        rot(m.lines);
    }

    for (int k = 0; k < 3; ++k) out.bmin[k] = FLT_MAX, out.bmax[k] = -FLT_MAX;
    auto grow = [&](const std::vector<float>& v) {
        for (size_t i = 0; i + 2 < v.size(); i += 3)
            for (int k = 0; k < 3; ++k) {
                out.bmin[k] = std::min(out.bmin[k], v[i + k]);
                out.bmax[k] = std::max(out.bmax[k], v[i + k]);
            }
    };
    if (!m.idx.empty()) grow(m.pos);
    grow(m.lines);
    if (out.bmin[0] > out.bmax[0])
        for (int k = 0; k < 3; ++k) out.bmin[k] = out.bmax[k] = 0;

    const size_t nt = m.TriangleCount();
    const uint32_t nv = uint32_t(m.VertexCount());
    const bool colored = m.triColor.size() == nt;
    out.pos.reserve(nt * 9);
    out.nrm.reserve(nt * 9);
    out.col.reserve(nt * 12);
    for (size_t t = 0; t < nt; ++t) {
        uint32_t i[3] = {m.idx[t * 3], m.idx[t * 3 + 1], m.idx[t * 3 + 2]};
        if (i[0] >= nv || i[1] >= nv || i[2] >= nv) continue;
        V3 p[3] = {At(m.pos, i[0]), At(m.pos, i[1]), At(m.pos, i[2])};
        V3 f = Norm(Cross(Sub(p[1], p[0]), Sub(p[2], p[0])));
        uint32_t rgb = colored ? m.triColor[t] : m.defaultColor;
        for (int k = 0; k < 3; ++k) {
            V3 n = f;
            if (smooth) {
                V3 s = At(m.nrm, i[k]);
                if (s.x * s.x + s.y * s.y + s.z * s.z > 0.25f) n = s;
            }
            out.pos.insert(out.pos.end(), {p[k].x, p[k].y, p[k].z});
            out.nrm.insert(out.nrm.end(), {n.x, n.y, n.z});
            out.col.insert(out.col.end(), {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), 255});
        }
    }
    out.triangles = out.pos.size() / 9;

    // Signed volume: sum of tetrahedra (origin, triangle). Exact for closed, consistently oriented surfaces;
    // the vertices do not have to be shared, so STEP faces with separate nodes work too.
    double vol = 0;
    const double cx = (out.bmin[0] + out.bmax[0]) * 0.5, cy = (out.bmin[1] + out.bmax[1]) * 0.5,
                 cz = (out.bmin[2] + out.bmax[2]) * 0.5; // centering improves precision
    for (size_t t = 0; t < out.triangles; ++t) {
        const float* p = &out.pos[t * 9];
        double ax = p[0] - cx, ay = p[1] - cy, az = p[2] - cz;
        double bx = p[3] - cx, by = p[4] - cy, bz = p[5] - cz;
        double qx = p[6] - cx, qy = p[7] - cy, qz = p[8] - cz;
        vol += ax * (by * qz - bz * qy) - ay * (bx * qz - bz * qx) + az * (bx * qy - by * qx);
    }
    out.volume = std::fabs(vol) / 6.0;

    out.edges = std::move(m.lines); // STEP files with curves only
    if (nt > 0 && nt <= kMaxEdgeTriangles) {
        float diag = std::sqrt((out.bmax[0] - out.bmin[0]) * (out.bmax[0] - out.bmin[0]) +
                               (out.bmax[1] - out.bmin[1]) * (out.bmax[1] - out.bmin[1]) +
                               (out.bmax[2] - out.bmin[2]) * (out.bmax[2] - out.bmin[2]));
        out.openEdges = FeatureEdges(m, creaseDeg, std::max(diag * 1e-6f, 1e-9f), out.edges);
    } else if (nt > kMaxEdgeTriangles) {
        out.edgesSkipped = true;
    }
}

} // namespace ct
