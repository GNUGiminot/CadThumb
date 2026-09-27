#include "render/Loaders.h"

#include "common/ThreeMfPackage.h"
#include "common/Zip.h"

#include <pugixml.hpp>
#include <charconv>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

namespace ct {

namespace {

// 3MF affine transform "m00 m01 m02 m10 m11 m12 m20 m21 m22 m30 m31 m32" (row-vector convention).
struct Mat {
    float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    void Apply(float x, float y, float z, float& ox, float& oy, float& oz) const {
        ox = x * m[0] + y * m[3] + z * m[6] + m[9];
        oy = x * m[1] + y * m[4] + z * m[7] + m[10];
        oz = x * m[2] + y * m[5] + z * m[8] + m[11];
    }
};

// Transform `a` applied first, then `b`.
Mat Then(const Mat& a, const Mat& b) {
    Mat c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0;
            for (int k = 0; k < 3; ++k) s += a.m[i * 3 + k] * b.m[k * 3 + j];
            c.m[i * 3 + j] = s;
        }
    for (int j = 0; j < 3; ++j) {
        float s = b.m[9 + j];
        for (int k = 0; k < 3; ++k) s += a.m[9 + k] * b.m[k * 3 + j];
        c.m[9 + j] = s;
    }
    return c;
}

const char* SkipWs(const char* p, const char* e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

Mat ParseMat(const char* s) {
    Mat r;
    if (!s || !*s) return r;
    const char* e = s + strlen(s);
    float v[12];
    for (int i = 0; i < 12; ++i) {
        s = SkipWs(s, e);
        auto res = std::from_chars(s, e, v[i]);
        if (res.ec != std::errc()) return Mat{};
        s = res.ptr;
    }
    memcpy(r.m, v, sizeof(v));
    return r;
}

inline float ParseFloat(const char* s) {
    float v = 0;
    const char* e = s + strlen(s);
    s = SkipWs(s, e);
    std::from_chars(s, e, v);
    return v;
}

inline const char* LocalName(const char* n) {
    const char* c = strrchr(n, ':');
    return c ? c + 1 : n;
}

inline bool Is(const pugi::xml_node& n, const char* local) { return strcmp(LocalName(n.name()), local) == 0; }

const char* Attr(const pugi::xml_node& n, const char* local) {
    for (auto a = n.first_attribute(); a; a = a.next_attribute())
        if (strcmp(LocalName(a.name()), local) == 0) return a.value();
    return nullptr;
}

int AttrInt(const pugi::xml_node& n, const char* local, int def) {
    const char* v = Attr(n, local);
    if (!v || !*v) return def;
    return atoi(v);
}

bool ParseColor(const char* s, uint32_t& rgb) {
    if (!s || s[0] != '#') return false;
    size_t len = strlen(s + 1);
    if (len != 6 && len != 8) return false;
    unsigned long v = strtoul(std::string(s + 1, 6).c_str(), nullptr, 16);
    rgb = LiftColor(uint32_t(v) & 0xFFFFFF, 0.3f);
    return true;
}

struct ObjectMesh {
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    std::vector<uint32_t> color; // per triangle, 0xFF000000 = "no color"
};

constexpr uint32_t kNoColor = 0xFF000000u;

struct Part {
    std::vector<char> buf;
    pugi::xml_document doc;
    pugi::xml_node build;
    std::unordered_map<int, pugi::xml_node> objects;
    std::unordered_map<int, std::vector<uint32_t>> props;
    std::unordered_map<int, std::unique_ptr<ObjectMesh>> meshes;
};

class Loader {
public:
    Loader(ZipArchive& zip, Mesh& out) : zip_(zip), out_(out) {}

    Part* GetPart(const std::string& path) {
        std::string key = NormalizeZipPath(path);
        auto it = parts_.find(key);
        if (it != parts_.end()) return it->second.get();
        auto part = std::make_unique<Part>();
        Part* raw = part.get();
        parts_[key] = std::move(part); // cache even on failure
        int idx = zip_.Find(key);
        if (idx < 0 || !zip_.Extract(idx, raw->buf, 4ull << 30)) return nullptr;
        auto res = raw->doc.load_buffer_inplace(raw->buf.data(), raw->buf.size(), pugi::parse_minimal);
        if (!res) return nullptr;
        pugi::xml_node model;
        for (auto n : raw->doc.children())
            if (n.type() == pugi::node_element && Is(n, "model")) model = n;
        if (!model) return nullptr;
        for (auto sec : model.children()) {
            if (Is(sec, "resources")) {
                for (auto r : sec.children()) {
                    int id = AttrInt(r, "id", -1);
                    if (id < 0) continue;
                    if (Is(r, "object")) {
                        raw->objects[id] = r;
                    } else if (Is(r, "basematerials")) {
                        auto& v = raw->props[id];
                        for (auto b : r.children()) {
                            uint32_t c = kNoColor;
                            ParseColor(Attr(b, "displaycolor"), c);
                            v.push_back(c);
                        }
                    } else if (Is(r, "colorgroup")) {
                        auto& v = raw->props[id];
                        for (auto b : r.children()) {
                            uint32_t c = kNoColor;
                            ParseColor(Attr(b, "color"), c);
                            v.push_back(c);
                        }
                    }
                }
            } else if (Is(sec, "build")) {
                raw->build = sec;
            }
        }
        return raw;
    }

    uint32_t Lookup(Part* part, int pid, int index) {
        if (pid < 0) return kNoColor;
        auto it = part->props.find(pid);
        if (it == part->props.end() || index < 0 || index >= (int)it->second.size()) return kNoColor;
        return it->second[index];
    }

    ObjectMesh* GetMesh(Part* part, int id, const pugi::xml_node& obj) {
        auto it = part->meshes.find(id);
        if (it != part->meshes.end()) return it->second.get();
        auto om = std::make_unique<ObjectMesh>();
        pugi::xml_node mesh;
        for (auto c : obj.children())
            if (Is(c, "mesh")) mesh = c;
        if (mesh) {
            const int objPid = AttrInt(obj, "pid", -1);
            const int objIdx = AttrInt(obj, "pindex", 0);
            const uint32_t objColor = Lookup(part, objPid, objIdx);
            for (auto sec : mesh.children()) {
                if (Is(sec, "vertices")) {
                    for (auto v : sec.children()) {
                        float x = 0, y = 0, z = 0;
                        for (auto a = v.first_attribute(); a; a = a.next_attribute()) {
                            const char* n = a.name();
                            if (n[1] != 0) continue;
                            if (n[0] == 'x') x = ParseFloat(a.value());
                            else if (n[0] == 'y') y = ParseFloat(a.value());
                            else if (n[0] == 'z') z = ParseFloat(a.value());
                        }
                        om->pos.push_back(x);
                        om->pos.push_back(y);
                        om->pos.push_back(z);
                    }
                } else if (Is(sec, "triangles")) {
                    for (auto t : sec.children()) {
                        uint32_t v1 = 0, v2 = 0, v3 = 0;
                        int pid = objPid, p1 = -1;
                        for (auto a = t.first_attribute(); a; a = a.next_attribute()) {
                            const char* n = a.name();
                            if (strcmp(n, "v1") == 0) v1 = (uint32_t)atoi(a.value());
                            else if (strcmp(n, "v2") == 0) v2 = (uint32_t)atoi(a.value());
                            else if (strcmp(n, "v3") == 0) v3 = (uint32_t)atoi(a.value());
                            else if (strcmp(n, "pid") == 0) pid = atoi(a.value());
                            else if (strcmp(n, "p1") == 0) p1 = atoi(a.value());
                        }
                        om->idx.push_back(v1);
                        om->idx.push_back(v2);
                        om->idx.push_back(v3);
                        uint32_t c = objColor;
                        if (p1 >= 0 || pid != objPid) c = Lookup(part, pid, p1 >= 0 ? p1 : objIdx);
                        om->color.push_back(c);
                    }
                }
            }
        }
        ObjectMesh* raw = om.get();
        part->meshes[id] = std::move(om);
        return raw;
    }

    void Instantiate(Part* part, int id, const Mat& xf, int depth) {
        if (!part || depth > 16 || out_.TriangleCount() > kMaxTriangles) return;
        auto it = part->objects.find(id);
        if (it == part->objects.end()) return;
        const pugi::xml_node obj = it->second;
        const char* type = Attr(obj, "type");
        if (type && (strcmp(type, "support") == 0 || strcmp(type, "other") == 0)) return;

        ObjectMesh* om = GetMesh(part, id, obj);
        if (om && !om->idx.empty()) {
            const uint32_t base = (uint32_t)out_.VertexCount();
            const uint32_t nv = uint32_t(om->pos.size() / 3);
            for (size_t i = 0; i < om->pos.size(); i += 3) {
                float x, y, z;
                xf.Apply(om->pos[i], om->pos[i + 1], om->pos[i + 2], x, y, z);
                out_.AddVertex(x, y, z);
            }
            for (size_t t = 0; t < om->idx.size(); t += 3) {
                uint32_t a = om->idx[t], b = om->idx[t + 1], c = om->idx[t + 2];
                if (a >= nv || b >= nv || c >= nv) continue;
                out_.AddTriangle(base + a, base + b, base + c);
                uint32_t col = om->color[t / 3];
                out_.triColor.push_back(col);
                if (col != kNoColor) hasColor_ = true;
            }
        }
        for (auto c : obj.children()) {
            if (!Is(c, "components")) continue;
            for (auto comp : c.children()) {
                if (!Is(comp, "component")) continue;
                int oid = AttrInt(comp, "objectid", -1);
                Mat cxf = ParseMat(Attr(comp, "transform"));
                const char* path = Attr(comp, "path");
                Part* target = (path && *path) ? GetPart(path) : part;
                Instantiate(target, oid, Then(cxf, xf), depth + 1);
            }
        }
    }

    bool Load(std::string& error) {
        std::string rootPath = Find3mfRootModel(zip_);
        if (rootPath.empty()) {
            error = "3MF: no 3D model part";
            return false;
        }
        Part* root = GetPart(rootPath);
        if (!root) {
            error = "3MF: cannot parse " + rootPath;
            return false;
        }
        bool anyItem = false;
        if (root->build) {
            for (auto item : root->build.children()) {
                if (!Is(item, "item")) continue;
                const char* printable = Attr(item, "printable");
                if (printable && strcmp(printable, "0") == 0) continue;
                anyItem = true;
                const char* path = Attr(item, "path");
                Part* target = (path && *path) ? GetPart(path) : root;
                Instantiate(target, AttrInt(item, "objectid", -1), ParseMat(Attr(item, "transform")), 0);
            }
        }
        if (!anyItem) {
            for (auto& [id, node] : root->objects) Instantiate(root, id, Mat{}, 0);
        }
        if (out_.idx.empty()) {
            error = "3MF: no triangles";
            return false;
        }
        if (!hasColor_) {
            out_.triColor.clear();
        } else {
            for (auto& c : out_.triColor)
                if (c == kNoColor) c = out_.defaultColor;
        }
        return true;
    }

private:
    static constexpr size_t kMaxTriangles = 40'000'000;
    ZipArchive& zip_;
    Mesh& out_;
    std::map<std::string, std::unique_ptr<Part>> parts_;
    bool hasColor_ = false;
};

} // namespace

bool Load3mf(const std::wstring& path, Mesh& mesh, std::string& error) {
    ZipArchive zip;
    if (!zip.OpenFile(path)) {
        error = "3MF: not a ZIP package";
        return false;
    }
    try {
        Loader loader(zip, mesh);
        return loader.Load(error);
    } catch (const std::bad_alloc&) {
        error = "3MF: out of memory";
        return false;
    }
}

} // namespace ct
