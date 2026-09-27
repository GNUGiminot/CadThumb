// STEP (AP203/AP214/AP242) -> triangle mesh via OpenCASCADE. Same logic as the Windows build's
// src/render/StepLoader.cpp, adapted to std::string paths and a portable millisecond clock.
#include "render/Loaders.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Poly_Triangle.hxx>
#include <Quantity_Color.hxx>
#include <RWMesh_FaceIterator.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_DocumentExplorer.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace ct {

namespace {
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

std::string ReadStepOriginatingSystem(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string head(64 * 1024, '\0');
    head.resize(fread(head.data(), 1, head.size(), f));
    fclose(f);
    size_t p = head.find("FILE_NAME");
    if (p == std::string::npos) return {};
    size_t e = head.find(");", p);
    if (e == std::string::npos) e = std::min(head.size(), p + 2048);
    std::string s = head.substr(p, e - p);
    for (auto& c : s) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : (c == '\r' || c == '\n' ? ' ' : c);
    return s;
}

namespace {

uint32_t ToRgb(const Quantity_ColorRGBA& c) {
    double r, g, b;
    c.GetRGB().Values(r, g, b, Quantity_TOC_sRGB);
    auto q = [](double v) { return uint32_t(std::clamp(int(v * 255.0 + 0.5), 0, 255)); };
    return (q(r) << 16) | (q(g) << 8) | q(b);
}

struct Collector {
    Mesh& mesh;
    uint32_t defaultColor;

    void AddFaces(RWMesh_FaceIterator& fi) {
        for (; fi.More(); fi.Next()) {
            if (fi.IsEmptyMesh()) continue;
            const uint32_t color = fi.HasFaceColor() ? LiftColor(ToRgb(fi.FaceColor()), 0.58f) : defaultColor;
            const int lower = fi.NodeLower(), upper = fi.NodeUpper();
            const uint32_t base = (uint32_t)mesh.VertexCount();
            const bool haveNormals = fi.HasNormals();
            for (int i = lower; i <= upper; ++i) {
                gp_Pnt p = fi.NodeTransformed(i);
                mesh.AddVertex((float)p.X(), (float)p.Y(), (float)p.Z());
                if (haveNormals) {
                    gp_Dir n = fi.NormalTransformed(i);
                    mesh.nrm.push_back((float)n.X());
                    mesh.nrm.push_back((float)n.Y());
                    mesh.nrm.push_back((float)n.Z());
                } else {
                    mesh.nrm.insert(mesh.nrm.end(), {0.f, 0.f, 0.f});
                }
            }
            const size_t firstTri = mesh.TriangleCount();
            for (int e = fi.ElemLower(); e <= fi.ElemUpper(); ++e) {
                Poly_Triangle t = fi.TriangleOriented(e);
                int a, b, c;
                t.Get(a, b, c);
                if (a < lower || b < lower || c < lower || a > upper || b > upper || c > upper) continue;
                mesh.AddTriangle(base + uint32_t(a - lower), base + uint32_t(b - lower), base + uint32_t(c - lower));
                mesh.triColor.push_back(color);
            }
            if (!haveNormals) SmoothFaceNormals(base, firstTri);
        }
    }

    // Area-weighted vertex normals for a face whose surface cannot provide them.
    void SmoothFaceNormals(uint32_t firstVertex, size_t firstTri) {
        for (size_t t = firstTri; t < mesh.TriangleCount(); ++t) {
            uint32_t i[3] = {mesh.idx[t * 3], mesh.idx[t * 3 + 1], mesh.idx[t * 3 + 2]};
            const float* p0 = &mesh.pos[i[0] * 3];
            const float* p1 = &mesh.pos[i[1] * 3];
            const float* p2 = &mesh.pos[i[2] * 3];
            float ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
            float vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
            float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            for (uint32_t k : i) {
                if (k < firstVertex) continue;
                mesh.nrm[k * 3] += nx;
                mesh.nrm[k * 3 + 1] += ny;
                mesh.nrm[k * 3 + 2] += nz;
            }
        }
    }
};

void CollectEdges(const TopoDS_Shape& shape, double deflection, Mesh& mesh) {
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) {
        try {
            BRepAdaptor_Curve curve(TopoDS::Edge(ex.Current()));
            GCPnts_TangentialDeflection pts(curve, 0.3, deflection, 2);
            for (int i = 1; i < pts.NbPoints(); ++i) {
                gp_Pnt a = pts.Value(i), b = pts.Value(i + 1);
                mesh.lines.insert(mesh.lines.end(), {(float)a.X(), (float)a.Y(), (float)a.Z(), (float)b.X(),
                                                     (float)b.Y(), (float)b.Z()});
            }
        } catch (const Standard_Failure&) {
        }
    }
}

double ComputeDeflection(const TopoDS_Shape& shape, int size, double quality, double& diag) {
    Bnd_Box box;
    BRepBndLib::Add(shape, box, false);
    if (box.IsVoid()) return -1;
    diag = std::sqrt(box.SquareExtent());
    if (!(diag > 0)) return -1;
    // ~0.4 px chord error at the target thumbnail size
    double defl = diag * 0.4 / std::max(size, 64) / std::max(quality, 0.1);
    return std::max(defl, diag * 1e-4);
}

} // namespace

bool LoadStep(const std::string& path, int size, double quality, Mesh& mesh, StepInfo& info, std::string& error) {
    info.originatingSystem = ReadStepOriginatingSystem(path);
    Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_Printer));

    const double angular = std::clamp(0.45 / std::sqrt(std::max(quality, 0.1)), 0.12, 0.8);

    try {
        Handle(TDocStd_Document) doc;
        XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);

        STEPCAFControl_Reader reader;
        reader.SetColorMode(true);
        reader.SetNameMode(false);
        reader.SetLayerMode(false);
        reader.SetPropsMode(false);
        reader.SetGDTMode(false);
        reader.SetMatMode(false);
        reader.SetViewMode(false);
#if OCC_VERSION_HEX >= 0x070700
        reader.SetMetaMode(false); // added in OCCT 7.7; older distro packages (e.g. Ubuntu's 7.6) lack it
#endif

        long long t = NowMs();
        bool xcafOk = reader.ReadFile(path.c_str()) == IFSelect_RetDone;
        info.readMs = NowMs() - t;
        t = NowMs();
        xcafOk = xcafOk && reader.Transfer(doc);
        info.transferMs = NowMs() - t;
        if (xcafOk) {
            Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
            NCollection_Sequence<TDF_Label> roots;
            shapeTool->GetFreeShapes(roots);
            TopoDS_Compound all;
            BRep_Builder builder;
            builder.MakeCompound(all);
            for (int i = 1; i <= roots.Length(); ++i) {
                TopoDS_Shape s = XCAFDoc_ShapeTool::GetShape(roots.Value(i));
                if (!s.IsNull()) builder.Add(all, s);
            }
            double diag = 0;
            double defl = ComputeDeflection(all, size, quality, diag);
            if (defl <= 0) {
                error = "STEP: empty geometry";
                return false;
            }
            t = NowMs();
            BRepMesh_IncrementalMesh mesher(all, defl, false, angular, true);
            info.meshMs = NowMs() - t;
            t = NowMs();

            Collector col{mesh, mesh.defaultColor};
            for (XCAFPrs_DocumentExplorer ex(doc, XCAFPrs_DocumentExplorerFlags_OnlyLeafNodes); ex.More(); ex.Next()) {
                const XCAFPrs_DocumentNode& node = ex.Current();
                RWMesh_FaceIterator fi(node.RefLabel, node.Location, true, node.Style);
                col.AddFaces(fi);
            }
            if (mesh.idx.empty()) CollectEdges(all, defl, mesh);
            info.collectMs = NowMs() - t;
        } else {
            // Fallback without XCAF (no colors), sometimes succeeds on slightly broken files.
            STEPControl_Reader plain;
            if (plain.ReadFile(path.c_str()) != IFSelect_RetDone) {
                error = "STEP: cannot read file";
                return false;
            }
            plain.TransferRoots();
            TopoDS_Shape shape = plain.OneShape();
            if (shape.IsNull()) {
                error = "STEP: no shapes";
                return false;
            }
            double diag = 0;
            double defl = ComputeDeflection(shape, size, quality, diag);
            if (defl <= 0) {
                error = "STEP: empty geometry";
                return false;
            }
            BRepMesh_IncrementalMesh mesher(shape, defl, false, angular, true);
            Collector col{mesh, mesh.defaultColor};
            RWMesh_FaceIterator fi(shape);
            col.AddFaces(fi);
            if (mesh.idx.empty()) CollectEdges(shape, defl, mesh);
        }
    } catch (const Standard_Failure& e) {
        error = std::string("STEP: OCCT exception: ") + (e.GetMessageString() ? e.GetMessageString() : "?");
        return false;
    } catch (const std::bad_alloc&) {
        error = "STEP: out of memory";
        return false;
    } catch (const std::exception& e) {
        error = std::string("STEP: ") + e.what();
        return false;
    }

    for (size_t i = 0; i + 2 < mesh.nrm.size(); i += 3) {
        float* n = &mesh.nrm[i];
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-20f) n[0] /= l, n[1] /= l, n[2] /= l;
    }
    if (!mesh.triColor.empty() &&
        std::all_of(mesh.triColor.begin(), mesh.triColor.end(), [&](uint32_t c) { return c == mesh.defaultColor; }))
        mesh.triColor.clear();

    if (mesh.Empty()) {
        error = "STEP: no faces or edges";
        return false;
    }
    return true;
}

} // namespace ct
