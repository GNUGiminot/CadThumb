// Lightweight interactive 3D viewer: `CadThumb.exe --view <file>`.
// OpenGL 1.1 fixed pipeline + display lists (works with any driver, even the GDI software renderer),
// MSAA when the driver offers it. The model is loaded on a background thread with the same loaders
// as the thumbnails (STEP via OpenCASCADE, 3MF, STL).
#include "app/App.h"
#include "common/FileType.h"
#include "common/ImageIO.h"
#include "common/Paths.h"
#include "common/Settings.h"
#include "render/RenderFile.h"
#include "viewer/ViewModel.h"

#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>

namespace ct {

namespace {

// ---- WGL_ARB_pixel_format (multisampling)
constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001, WGL_ACCELERATION_ARB = 0x2003, WGL_SUPPORT_OPENGL_ARB = 0x2010,
              WGL_DOUBLE_BUFFER_ARB = 0x2011, WGL_PIXEL_TYPE_ARB = 0x2013, WGL_COLOR_BITS_ARB = 0x2014,
              WGL_DEPTH_BITS_ARB = 0x2022, WGL_STENCIL_BITS_ARB = 0x2023, WGL_FULL_ACCELERATION_ARB = 0x2027,
              WGL_TYPE_RGBA_ARB = 0x202B, WGL_SAMPLE_BUFFERS_ARB = 0x2041, WGL_SAMPLES_ARB = 0x2042;
constexpr GLenum GL_MULTISAMPLE_ARB_ = 0x809D;
constexpr GLenum GL_BGRA_EXT_ = 0x80E1;
using ChoosePixelFormatARB = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using SwapIntervalEXT = BOOL(WINAPI*)(int);

constexpr UINT WM_APP_LOADED = WM_APP + 10;
constexpr UINT_PTR TIMER_LOADING = 1, TIMER_TOAST = 2;
constexpr float kFovDeg = 30.0f;
constexpr wchar_t kWindowClass[] = L"CadThumbViewer";

struct V3 {
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Norm(V3 a) {
    float l = std::sqrt(Dot(a, a));
    return l > 1e-20f ? a * (1.0f / l) : V3{0, 0, 1};
}

struct LoadResult {
    bool ok = false;
    std::string error;
    ViewModel model;
    double seconds = 0;
};

struct TextTex {
    GLuint tex = 0;
    int w = 0, h = 0, tw = 1, th = 1;
};

struct Viewer {
    HWND hwnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    int w = 1, h = 1;
    UINT dpi = 96;
    HFONT font = nullptr, fontSmall = nullptr, fontBig = nullptr;

    std::wstring path;
    bool loading = false;
    ULONGLONG loadStart = 0;
    std::wstring error;

    bool hasModel = false;
    GLuint facesList = 0, edgesList = 0;
    size_t triangles = 0, edgeCount = 0;
    bool edgesSkipped = false;
    float bmin[3]{}, bmax[3]{};
    std::vector<float> fitPts; // subsample of vertices for "show all"
    V3 center{0, 0, 0};
    float radius = 1;
    double loadSeconds = 0;

    V3 target{0, 0, 0};
    float dist = 3, yaw = 45, pitch = 30;
    bool perspective = true, showEdges = true, darkBg = false, showHelp = true;

    int drag = 0; // 1 rotate, 2 pan
    POINT last{};

    TextTex info, help, big;
    bool textDirty = true;

    double volume = 0;
    size_t openEdges = 0;

    // ruler: click two points on the model; snaps to edge vertices near the cursor
    struct Measure {
        V3 a{}, b{};
        bool done = false;
        TextTex label;
    };
    bool ruler = false;
    std::vector<Measure> measures;
    std::vector<float> snapPts;  // feature-edge endpoints
    std::vector<float> depth;    // depth buffer of the last frame (ruler mode only)
    bool hover = false, hoverSnapped = false;
    V3 hoverPt{};
    POINT downPt{};
    double mv[16]{}, pj[16]{};
    GLint vp[4]{};

    std::wstring toast;
    ULONGLONG toastUntil = 0;
    TextTex toastTex;
    bool capture = false; // rendering a picture for the clipboard: no help text or cursor markers

    std::wstring snapshot; // test mode: save first frame and exit
    int testStage = -1;    // ruler self-test in snapshot mode
    Settings settings;
} V;

// ------------------------------------------------------------------ helpers

std::wstring Group(size_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(size_t(i), L" ");
    return s;
}

std::wstring Num(float v) {
    wchar_t b[32];
    swprintf_s(b, v >= 100 ? L"%.0f" : v >= 10 ? L"%.1f" : L"%.2f", v);
    for (wchar_t* p = b; *p; ++p)
        if (*p == L'.') *p = L',';
    return b;
}

// Measurement precision: 0,01 mm below 1 m.
std::wstring Mm(float v) {
    wchar_t b[32];
    swprintf_s(b, v >= 1000 ? L"%.1f" : L"%.2f", v);
    for (wchar_t* p = b; *p; ++p)
        if (*p == L'.') *p = L',';
    return b;
}

int NextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

void MakeText(TextTex& t, const std::wstring& text, HFONT font) {
    if (t.tex) glDeleteTextures(1, &t.tex);
    t = TextTex{};
    if (text.empty()) return;
    HDC mdc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldFont = SelectObject(mdc, font);
    RECT rc{0, 0, 4096, 0};
    DrawTextW(mdc, text.c_str(), -1, &rc, DT_CALCRECT | DT_NOPREFIX | DT_LEFT);
    t.w = rc.right + 4;
    t.h = rc.bottom + 4;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = t.w;
    bi.bmiHeader.biHeight = -t.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(mdc, bmp);
    memset(bits, 0, size_t(t.w) * t.h * 4);
    SetTextColor(mdc, RGB(255, 255, 255));
    SetBkMode(mdc, TRANSPARENT);
    RECT r{2, 2, t.w, t.h};
    DrawTextW(mdc, text.c_str(), -1, &r, DT_NOPREFIX | DT_LEFT);
    GdiFlush();

    t.tw = NextPow2(t.w);
    t.th = NextPow2(t.h);
    std::vector<uint8_t> alpha(size_t(t.tw) * t.th, 0);
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < t.h; ++y)
        for (int x = 0; x < t.w; ++x) {
            const uint8_t* p = src + (size_t(y) * t.w + x) * 4;
            alpha[size_t(y) * t.tw + x] = std::max({p[0], p[1], p[2]});
        }
    SelectObject(mdc, oldBmp);
    SelectObject(mdc, oldFont);
    DeleteObject(bmp);
    DeleteDC(mdc);

    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, t.tw, t.th, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alpha.data());
}

// Draws the text texture at pixel position (x, y) of the top-left corner; a contrasting halo keeps it
// readable over the model.
void DrawText(const TextTex& t, float x, float y, float r, float g, float b) {
    if (!t.tex) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    float u = float(t.w) / t.tw, v = float(t.h) / t.th;
    auto quad = [&](float ox, float oy) {
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0);
        glVertex2f(x + ox, y + oy);
        glTexCoord2f(u, 0);
        glVertex2f(x + ox + t.w, y + oy);
        glTexCoord2f(u, v);
        glVertex2f(x + ox + t.w, y + oy + t.h);
        glTexCoord2f(0, v);
        glVertex2f(x + ox, y + oy + t.h);
        glEnd();
    };
    float halo = (r + g + b) > 1.5f ? 0.0f : 1.0f;
    glColor4f(halo, halo, halo, 0.55f);
    quad(1, 1);
    quad(-1, 0);
    glColor4f(r, g, b, 1.0f);
    quad(0, 0);
    glDisable(GL_TEXTURE_2D);
}

// ------------------------------------------------------------------ camera

V3 ViewDir() { // from target towards the eye
    float y = V.yaw * 3.14159265f / 180.0f, p = V.pitch * 3.14159265f / 180.0f;
    return {std::cos(p) * std::sin(y), -std::cos(p) * std::cos(y), std::sin(p)};
}

void Basis(V3& s, V3& u, V3& f) {
    V3 d = ViewDir();
    f = d * -1.0f;
    s = Cross(f, {0, 0, 1});
    if (Dot(s, s) < 1e-8f) {
        float y = V.yaw * 3.14159265f / 180.0f;
        s = {std::cos(y), std::sin(y), 0};
    }
    s = Norm(s);
    u = Cross(s, f);
}

float HalfHeight() { return V.dist * std::tan(kFovDeg * 0.5f * 3.14159265f / 180.0f); }

void Fit() {
    // Project a subsample of the model vertices onto the view plane and fit that rectangle
    // (the bounding box is far too loose for L-shaped parts).
    V3 s, u, f;
    Basis(s, u, f);
    float minS = FLT_MAX, maxS = -FLT_MAX, minU = FLT_MAX, maxU = -FLT_MAX, minF = FLT_MAX, maxF = -FLT_MAX;
    for (size_t i = 0; i + 2 < V.fitPts.size(); i += 3) {
        V3 c{V.fitPts[i], V.fitPts[i + 1], V.fitPts[i + 2]};
        minS = std::min(minS, Dot(c, s)), maxS = std::max(maxS, Dot(c, s));
        minU = std::min(minU, Dot(c, u)), maxU = std::max(maxU, Dot(c, u));
        minF = std::min(minF, Dot(c, f)), maxF = std::max(maxF, Dot(c, f));
    }
    const float aspect = float(V.w) / float(std::max(V.h, 1));
    const float hh = std::max({(maxU - minU) * 0.5f, (maxS - minS) * 0.5f / aspect, V.radius * 0.01f}) * 1.12f;
    V.target = s * ((minS + maxS) * 0.5f) + u * ((minU + maxU) * 0.5f) + f * ((minF + maxF) * 0.5f);
    // perspective: keep the nearest face of the box inside the frustum as well
    V.dist = hh / std::tan(kFovDeg * 0.5f * 3.14159265f / 180.0f) + (maxF - minF) * 0.5f * (V.perspective ? 1.0f : 0.0f);
    InvalidateRect(V.hwnd, nullptr, FALSE);
}

void SetView(float yaw, float pitch) {
    V.yaw = yaw;
    V.pitch = pitch;
    Fit();
}

// ------------------------------------------------------------------ text

void UpdateTexts() {
    V.textDirty = false;
    std::wstring name = FileNameOf(V.path);
    std::wstring infoText, bigText;
    if (V.loading) {
        unsigned sec = unsigned((GetTickCount64() - V.loadStart) / 1000);
        bigText = L"Загрузка «" + name + L"»…";
        if (sec > 0) bigText += L"  " + std::to_wstring(sec) + L" с";
    } else if (!V.error.empty()) {
        bigText = L"Не удалось открыть «" + name + L"»\n" + V.error;
    } else if (V.hasModel) {
        infoText = name + L"\nТреугольников: " + Group(V.triangles);
        wchar_t t[32];
        swprintf_s(t, L"%.1f", V.loadSeconds);
        for (wchar_t* p = t; *p; ++p)
            if (*p == L'.') *p = L',';
        infoText += L"   ·   загрузка " + std::wstring(t) + L" с";
        infoText += L"\nГабариты: " + Num(V.bmax[0] - V.bmin[0]) + L" × " + Num(V.bmax[1] - V.bmin[1]) + L" × " +
                    Num(V.bmax[2] - V.bmin[2]) + L" мм";
        if (V.volume > 0 && V.triangles) {
            const double cm3 = V.volume / 1000.0;
            // PLA 1.24 g/cm³ at 100 % infill: an upper bound for the printed part
            infoText += L"\nОбъём: " + Num(float(cm3)) + L" см³   ·   PLA при 100 % заполнении ≈ " +
                        Num(float(cm3 * 1.24)) + L" г";
            if (V.openEdges) infoText += L"  (поверхность не замкнута, объём приблизительный)";
        }
        if (V.edgesSkipped) infoText += L"\nРёбра не показаны: слишком большая модель";
        if (V.ruler) {
            infoText += L"\n\nЛинейка: щёлкните две точки на модели";
            const V3* a = nullptr;
            V3 b{};
            if (!V.measures.empty() && !V.measures.back().done && V.hover) {
                a = &V.measures.back().a;
                b = V.hoverPt;
            } else if (!V.measures.empty() && V.measures.back().done) {
                a = &V.measures.back().a;
                b = V.measures.back().b;
            }
            if (a) {
                V3 d{b.x - a->x, b.y - a->y, b.z - a->z};
                infoText += L"\nРасстояние: " + Mm(std::sqrt(Dot(d, d))) + L" мм   (ΔX " + Mm(std::fabs(d.x)) +
                            L"   ΔY " + Mm(std::fabs(d.y)) + L"   ΔZ " + Mm(std::fabs(d.z)) + L")";
            }
        }
    } else {
        bigText = L"Перетащите сюда файл STEP, 3MF или STL\nили нажмите Ctrl+O";
    }
    MakeText(V.info, infoText, V.font);
    MakeText(V.big, bigText, V.fontBig);
    MakeText(V.help,
             V.showHelp ? L"ЛКМ — вращать   ПКМ / СКМ / Shift+ЛКМ — сдвиг   колесо — масштаб   двойной щелчок, F — "
                          L"показать всё\n"
                          L"Виды: 1 спереди  2 сзади  3 слева  4 справа  5 сверху  6 снизу  0 изометрия\n"
                          L"R — линейка (Backspace — убрать последний замер, Esc — очистить)   "
                          L"Ctrl+C — картинка в буфер обмена\n"
                          L"E — рёбра   O — перспектива   B — фон   Enter — открыть в своей программе   "
                          L"Ctrl+O — другой файл   H — скрыть подсказку"
                        : L"H — подсказка",
             V.fontSmall);
}

// ------------------------------------------------------------------ loading

void StartLoad(const std::wstring& path) {
    if (V.loading) return;
    V.path = path;
    V.error.clear();
    V.loading = true;
    V.loadStart = GetTickCount64();
    V.textDirty = true;
    SetWindowTextW(V.hwnd, (FileNameOf(path) + L" — CadThumb").c_str());
    SetTimer(V.hwnd, TIMER_LOADING, 500, nullptr);
    InvalidateRect(V.hwnd, nullptr, FALSE);

    HWND hwnd = V.hwnd;
    Settings s = V.settings;
    std::thread([hwnd, path, s] {
        auto res = std::make_unique<LoadResult>();
        ULONGLONG t0 = GetTickCount64();
        FileType type = FileTypeFromExtension(ExtOf(path));
        if (type == FileType::Unknown) {
            uint8_t head[512] = {};
            if (FILE* f = _wfopen(path.c_str(), L"rb")) {
                size_t n = fread(head, 1, sizeof(head), f);
                fclose(f);
                type = FileTypeSniff(head, n);
            }
        }
        try {
            Mesh mesh;
            char up = 'Z';
            if (!FileExists(path)) throw std::runtime_error("файл не найден");
            // finer tessellation than thumbnails: the model can be zoomed in
            if (LoadModelMesh(path, type, int(1600 * std::max(0.25, s.quality)), s, mesh, up, res->error)) {
                BuildViewModel(std::move(mesh), up, 30.0f, res->model);
                res->ok = res->model.triangles > 0 || !res->model.edges.empty();
                if (!res->ok) res->error = "в файле нет геометрии";
            }
        } catch (const std::bad_alloc&) {
            res->error = "недостаточно памяти";
        } catch (const std::runtime_error& e) {
            res->error = e.what();
        } catch (...) {
            res->error = "ошибка при чтении файла";
        }
        res->seconds = (GetTickCount64() - t0) / 1000.0;
        if (!PostMessageW(hwnd, WM_APP_LOADED, 0, (LPARAM)res.get())) return;
        res.release();
    }).detach();
}

void CompileModel(ViewModel& m) {
    if (V.facesList) glDeleteLists(V.facesList, 1);
    if (V.edgesList) glDeleteLists(V.edgesList, 1);
    V.facesList = glGenLists(1);
    glNewList(V.facesList, GL_COMPILE);
    if (!m.pos.empty()) {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, m.pos.data());
        glNormalPointer(GL_FLOAT, 0, m.nrm.data());
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, m.col.data());
        const GLsizei total = GLsizei(m.VertexCount()), chunk = 3 * 1'000'000;
        for (GLsizei first = 0; first < total; first += chunk)
            glDrawArrays(GL_TRIANGLES, first, std::min(chunk, total - first));
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
    }
    glEndList();

    V.edgesList = glGenLists(1);
    glNewList(V.edgesList, GL_COMPILE);
    if (!m.edges.empty()) {
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, m.edges.data());
        glDrawArrays(GL_LINES, 0, GLsizei(m.edges.size() / 3));
        glDisableClientState(GL_VERTEX_ARRAY);
    }
    glEndList();

    V.fitPts.clear();
    for (const std::vector<float>* src : {&m.pos, &m.edges}) {
        const size_t n = src->size() / 3, step = std::max<size_t>(1, n / 200000);
        for (size_t i = 0; i < n; i += step) V.fitPts.insert(V.fitPts.end(), src->begin() + i * 3, src->begin() + i * 3 + 3);
    }
    V.snapPts = m.edges; // endpoints of feature edges: corners, hole rims, silhouettes of curved faces
    for (auto& ms : V.measures)
        if (ms.label.tex) glDeleteTextures(1, &ms.label.tex);
    V.measures.clear();
    V.hover = false;
    V.volume = m.volume;
    V.openEdges = m.openEdges;
    V.triangles = m.triangles;
    V.edgeCount = m.edges.size() / 6;
    V.edgesSkipped = m.edgesSkipped;
    for (int k = 0; k < 3; ++k) V.bmin[k] = m.bmin[k], V.bmax[k] = m.bmax[k];
    V.center = {(m.bmin[0] + m.bmax[0]) / 2, (m.bmin[1] + m.bmax[1]) / 2, (m.bmin[2] + m.bmax[2]) / 2};
    float dx = m.bmax[0] - m.bmin[0], dy = m.bmax[1] - m.bmin[1], dz = m.bmax[2] - m.bmin[2];
    V.radius = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
    V.hasModel = true;
}

void OnLoaded(LoadResult* raw) {
    std::unique_ptr<LoadResult> res(raw);
    V.loading = false;
    KillTimer(V.hwnd, TIMER_LOADING);
    V.loadSeconds = res->seconds;
    if (res->ok) {
        wglMakeCurrent(V.dc, V.rc);
        CompileModel(res->model);
        SetView(float(V.settings.yawDeg), float(V.settings.pitchDeg));
    } else {
        V.error = Wide(res->error);
        if (!V.snapshot.empty()) {
            Out(L"ERROR: " + V.error + L"\r\n");
            PostQuitMessage(2);
        }
    }
    V.textDirty = true;
    InvalidateRect(V.hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------ drawing

void DrawBackground() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glBegin(GL_QUADS);
    if (V.darkBg) glColor3f(0.10f, 0.11f, 0.13f);
    else glColor3f(0.80f, 0.84f, 0.89f);
    glVertex2f(0, 0);
    glVertex2f(1, 0);
    if (V.darkBg) glColor3f(0.22f, 0.24f, 0.28f);
    else glColor3f(0.96f, 0.97f, 0.98f);
    glVertex2f(1, 1);
    glVertex2f(0, 1);
    glEnd();
}

void LoadViewMatrix(bool rotationOnly) {
    V3 s, u, f;
    Basis(s, u, f);
    V3 eye = rotationOnly ? ViewDir() * 3.0f : V.target + ViewDir() * V.dist;
    const float m[16] = {s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0,
                         -Dot(s, eye), -Dot(u, eye), Dot(f, eye), 1};
    glMultMatrixf(m);
}

void DrawScene() {
    const float aspect = float(V.w) / float(std::max(V.h, 1));
    const float hh = HalfHeight();
    const float zn = std::max(V.dist - V.radius * 2.0f, V.radius * 0.005f);
    const float zf = V.dist + V.radius * 2.0f;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (V.perspective) {
        float k = zn / V.dist;
        glFrustum(-hh * aspect * k, hh * aspect * k, -hh * k, hh * k, zn, zf);
    } else {
        glOrtho(-hh * aspect, hh * aspect, -hh, hh, zn, zf);
    }
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // lights fixed to the camera (eye space): key from upper-left, fill from the right
    const GLfloat l0[] = {-0.45f, 0.65f, 0.6f, 0.0f}, l1[] = {0.6f, -0.25f, 0.45f, 0.0f};
    const GLfloat d0[] = {0.62f, 0.62f, 0.62f, 1}, d1[] = {0.26f, 0.26f, 0.28f, 1}, a0[] = {0.32f, 0.32f, 0.33f, 1};
    const GLfloat spec[] = {0.25f, 0.25f, 0.25f, 1}, none[] = {0, 0, 0, 1};
    glLightfv(GL_LIGHT0, GL_POSITION, l0);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, d0);
    glLightfv(GL_LIGHT0, GL_SPECULAR, spec);
    glLightfv(GL_LIGHT1, GL_POSITION, l1);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, d1);
    glLightfv(GL_LIGHT1, GL_SPECULAR, none);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, a0);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 40.0f);
    LoadViewMatrix(false);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);
    glCallList(V.facesList);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_LIGHTING);

    if (V.showEdges && V.edgeCount) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glLineWidth(std::max(1.0f, V.dpi / 96.0f));
        if (V.darkBg) glColor4f(0.02f, 0.02f, 0.03f, 0.7f);
        else glColor4f(0.08f, 0.10f, 0.13f, V.triangles ? 0.55f : 0.9f);
        glCallList(V.edgesList);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // for picking and label placement
    glGetDoublev(GL_MODELVIEW_MATRIX, V.mv);
    glGetDoublev(GL_PROJECTION_MATRIX, V.pj);
    glGetIntegerv(GL_VIEWPORT, V.vp);
    if (V.ruler && !V.capture) {
        V.depth.resize(size_t(V.w) * V.h);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, V.w, V.h, GL_DEPTH_COMPONENT, GL_FLOAT, V.depth.data());
    }
    glDisable(GL_DEPTH_TEST);

    // measurements: always on top of the model
    const bool pending = !V.measures.empty() && !V.measures.back().done;
    if (V.measures.empty() && !(V.ruler && V.hover)) return;
    const float px = std::max(1.0f, V.dpi / 96.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glEnable(GL_LINE_SMOOTH);
    glLineWidth(2.0f * px);
    glColor4f(0.95f, 0.42f, 0.05f, 1.0f);
    glBegin(GL_LINES);
    for (const auto& m : V.measures) {
        if (!m.done) continue;
        glVertex3f(m.a.x, m.a.y, m.a.z);
        glVertex3f(m.b.x, m.b.y, m.b.z);
    }
    if (pending && V.hover && !V.capture) { // rubber band to the cursor
        const V3& a = V.measures.back().a;
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(V.hoverPt.x, V.hoverPt.y, V.hoverPt.z);
    }
    glEnd();
    glPointSize(7.0f * px);
    glBegin(GL_POINTS);
    for (const auto& m : V.measures) {
        glVertex3f(m.a.x, m.a.y, m.a.z);
        if (m.done) glVertex3f(m.b.x, m.b.y, m.b.z);
    }
    glEnd();
    if (V.ruler && V.hover && !V.capture) {
        glPointSize(10.0f * px);
        if (V.hoverSnapped) glColor4f(0.1f, 0.7f, 0.25f, 0.95f); // snapped to a vertex
        else glColor4f(0.95f, 0.42f, 0.05f, 0.6f);
        glBegin(GL_POINTS);
        glVertex3f(V.hoverPt.x, V.hoverPt.y, V.hoverPt.z);
        glEnd();
    }
    glDisable(GL_POINT_SMOOTH);
    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_BLEND);
}

// ------------------------------------------------------------------ ruler picking

bool ToScreen(V3 p, double& sx, double& sy, double& sz) {
    if (!gluProject(p.x, p.y, p.z, V.mv, V.pj, V.vp, &sx, &sy, &sz)) return false;
    sy = V.h - sy; // window coordinates, y down
    return sz > 0 && sz < 1;
}

float DepthAt(int x, int y) {
    if (V.depth.size() != size_t(V.w) * V.h || x < 0 || y < 0 || x >= V.w || y >= V.h) return 1.0f;
    return V.depth[size_t(V.h - 1 - y) * V.w + x];
}

// Point under the cursor: a visible edge vertex within a few pixels, otherwise the surface itself.
bool PickPoint(int x, int y, V3& out, bool& snapped) {
    const double radius = 10.0 * V.dpi / 96.0;
    double best = radius * radius;
    snapped = false;
    for (size_t i = 0; i + 2 < V.snapPts.size(); i += 3) {
        V3 p{V.snapPts[i], V.snapPts[i + 1], V.snapPts[i + 2]};
        double sx, sy, sz;
        if (!ToScreen(p, sx, sy, sz)) continue;
        double dx = sx - x, dy = sy - y, d2 = dx * dx + dy * dy;
        if (d2 >= best) continue;
        if (sz > DepthAt(int(sx), int(sy)) + 2e-4) continue; // hidden behind the model
        best = d2;
        out = p;
        snapped = true;
    }
    if (snapped) return true;
    float d = DepthAt(x, y);
    if (d >= 1.0f) return false;
    double ox, oy, oz;
    if (!gluUnProject(x + 0.5, V.h - 1 - y + 0.5, d, V.mv, V.pj, V.vp, &ox, &oy, &oz)) return false;
    out = {float(ox), float(oy), float(oz)};
    return true;
}

void UpdateHover(int x, int y) {
    V.hover = PickPoint(x, y, V.hoverPt, V.hoverSnapped);
    V.textDirty = true;
    InvalidateRect(V.hwnd, nullptr, FALSE);
}

void AddRulerPoint(int x, int y) {
    V3 p;
    bool snapped;
    if (!PickPoint(x, y, p, snapped)) return;
    if (V.measures.empty() || V.measures.back().done) {
        V.measures.push_back({});
        V.measures.back().a = p;
    } else {
        auto& m = V.measures.back();
        m.b = p;
        m.done = true;
        V3 d{m.b.x - m.a.x, m.b.y - m.a.y, m.b.z - m.a.z};
        MakeText(m.label, Mm(std::sqrt(Dot(d, d))) + L" мм", V.font);
    }
    V.textDirty = true;
    InvalidateRect(V.hwnd, nullptr, FALSE);
}

void ClearMeasures(bool lastOnly) {
    if (V.measures.empty()) return;
    size_t keep = lastOnly ? V.measures.size() - 1 : 0;
    for (size_t i = keep; i < V.measures.size(); ++i)
        if (V.measures[i].label.tex) glDeleteTextures(1, &V.measures[i].label.tex);
    V.measures.resize(keep);
    V.textDirty = true;
    InvalidateRect(V.hwnd, nullptr, FALSE);
}

void DrawTriad() {
    const int size = MulDiv(64, V.dpi, 96), margin = MulDiv(12, V.dpi, 96);
    glViewport(margin, margin, size, size);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.3, 1.3, -1.3, 1.3, -10, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    LoadViewMatrix(true);
    glLineWidth(std::max(2.0f, 2.0f * V.dpi / 96.0f));
    glBegin(GL_LINES);
    glColor3f(0.85f, 0.2f, 0.2f);
    glVertex3f(0, 0, 0);
    glVertex3f(1, 0, 0);
    glColor3f(0.2f, 0.65f, 0.25f);
    glVertex3f(0, 0, 0);
    glVertex3f(0, 1, 0);
    glColor3f(0.2f, 0.4f, 0.9f);
    glVertex3f(0, 0, 0);
    glVertex3f(0, 0, 1);
    glEnd();
    glViewport(0, 0, V.w, V.h);
}

void DrawOverlay() {
    if (V.textDirty) UpdateTexts();
    glViewport(0, 0, V.w, V.h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, V.w, V.h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const float c = V.darkBg ? 0.92f : 0.12f;
    const float pad = float(MulDiv(12, V.dpi, 96));

    // distance labels next to the middle of each measurement
    if (V.hasModel && !V.loading) {
        for (const auto& m : V.measures) {
            if (!m.done || !m.label.tex) continue;
            double sx, sy, sz;
            V3 mid{(m.a.x + m.b.x) / 2, (m.a.y + m.b.y) / 2, (m.a.z + m.b.z) / 2};
            if (!ToScreen(mid, sx, sy, sz)) continue;
            float x = float(sx) - m.label.w / 2.0f, y = float(sy) - m.label.h - 4.0f;
            glColor4f(1.0f, 1.0f, 1.0f, 0.85f);
            glBegin(GL_QUADS);
            glVertex2f(x - 3, y);
            glVertex2f(x + m.label.w + 3, y);
            glVertex2f(x + m.label.w + 3, y + m.label.h);
            glVertex2f(x - 3, y + m.label.h);
            glEnd();
            DrawText(m.label, x, y, 0.75f, 0.28f, 0.0f);
        }
    }

    DrawText(V.info, pad, pad, c, c, c + 0.02f);
    if (V.big.tex) DrawText(V.big, (V.w - V.big.w) / 2.0f, (V.h - V.big.h) / 2.0f, c, c, c + 0.02f);
    if (!V.capture) DrawText(V.help, float(MulDiv(88, V.dpi, 96)), V.h - pad - V.help.h, c, c, c + 0.02f);
    if (!V.capture && GetTickCount64() < V.toastUntil && V.toastTex.tex) {
        float x = (V.w - V.toastTex.w) / 2.0f, y = V.h * 0.12f;
        glColor4f(0.1f, 0.12f, 0.15f, 0.8f);
        glBegin(GL_QUADS);
        glVertex2f(x - 10, y - 6);
        glVertex2f(x + V.toastTex.w + 10, y - 6);
        glVertex2f(x + V.toastTex.w + 10, y + V.toastTex.h + 6);
        glVertex2f(x - 10, y + V.toastTex.h + 6);
        glEnd();
        DrawText(V.toastTex, x, y, 1.0f, 1.0f, 1.0f);
    }
    glDisable(GL_BLEND);
}

void Render() {
    wglMakeCurrent(V.dc, V.rc);
    glViewport(0, 0, V.w, V.h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    DrawBackground();
    if (V.hasModel && !V.loading) {
        DrawScene();
        DrawTriad();
    }
    DrawOverlay();
    glFinish();

    // test hook (CADTHUMB_TEST_RULER=1 with --snapshot): measure across the middle of the view
    if (!V.snapshot.empty() && V.hasModel && !V.loading && V.testStage >= 0 && V.testStage < 2) {
        if (V.testStage++ == 0) {
            V.ruler = true;
        } else {
            AddRulerPoint(V.w * 35 / 100, V.h / 2);
            AddRulerPoint(V.w * 65 / 100, V.h / 2);
            UpdateHover(V.w * 60 / 100, V.h * 40 / 100);
        }
        SwapBuffers(V.dc);
        InvalidateRect(V.hwnd, nullptr, FALSE);
        return;
    }
    if (!V.snapshot.empty() && V.hasModel && !V.loading) {
        Image img;
        img.resize(V.w, V.h);
        std::vector<uint8_t> raw(size_t(V.w) * V.h * 4);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, V.w, V.h, GL_BGRA_EXT_, GL_UNSIGNED_BYTE, raw.data());
        for (int y = 0; y < V.h; ++y) memcpy(&img.px[size_t(y) * V.w * 4], &raw[size_t(V.h - 1 - y) * V.w * 4], size_t(V.w) * 4);
        for (size_t i = 3; i < img.px.size(); i += 4) img.px[i] = 255;
        SavePng(V.snapshot, img);
        Out(L"OK snapshot " + std::to_wstring(V.w) + L"x" + std::to_wstring(V.h) + L", " + Group(V.triangles) +
            L" triangles, " + std::to_wstring(V.edgeCount) + L" edges\r\n");
        PostQuitMessage(0);
    }
    SwapBuffers(V.dc);
}

void ShowToast(const std::wstring& text) {
    wglMakeCurrent(V.dc, V.rc);
    MakeText(V.toastTex, text, V.font);
    V.toastUntil = GetTickCount64() + 1800;
    SetTimer(V.hwnd, TIMER_TOAST, 1900, nullptr);
    InvalidateRect(V.hwnd, nullptr, FALSE);
}

// Ctrl+C: the current view (with measurements, without help text) as a bitmap in the clipboard.
void CopyToClipboard() {
    if (!V.hasModel || V.loading) return;
    wglMakeCurrent(V.dc, V.rc);
    V.capture = true;
    glViewport(0, 0, V.w, V.h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    DrawBackground();
    DrawScene();
    DrawTriad();
    DrawOverlay();
    glFinish();
    V.capture = false;

    const size_t bytes = size_t(V.w) * V.h * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (!mem) return;
    auto* bih = static_cast<BITMAPINFOHEADER*>(GlobalLock(mem));
    *bih = BITMAPINFOHEADER{sizeof(BITMAPINFOHEADER)};
    bih->biWidth = V.w;
    bih->biHeight = V.h; // bottom-up, same row order as glReadPixels
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, V.w, V.h, GL_BGRA_EXT_, GL_UNSIGNED_BYTE, bih + 1);
    GlobalUnlock(mem);
    bool ok = false;
    if (OpenClipboard(V.hwnd)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_DIB, mem) != nullptr;
        CloseClipboard();
    }
    if (!ok) GlobalFree(mem);
    ShowToast(ok ? L"Картинка скопирована в буфер обмена" : L"Не удалось скопировать картинку");
}

// ------------------------------------------------------------------ OpenGL context

int ChooseMultisampleFormat(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"CadThumbGLProbe";
    RegisterClassW(&wc);
    HWND tmp = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, inst, nullptr);
    HDC dc = GetDC(tmp);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);
    int result = 0;
    auto choose = (ChoosePixelFormatARB)wglGetProcAddress("wglChoosePixelFormatARB");
    if (choose) {
        for (int samples : {8, 4, 2}) {
            const int attrs[] = {WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
                                 WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
                                 WGL_COLOR_BITS_ARB, 32, WGL_DEPTH_BITS_ARB, 24, WGL_STENCIL_BITS_ARB, 8,
                                 WGL_SAMPLE_BUFFERS_ARB, 1, WGL_SAMPLES_ARB, samples, 0};
            int fmt = 0;
            UINT n = 0;
            if (choose(dc, attrs, nullptr, 1, &fmt, &n) && n > 0) {
                result = fmt;
                break;
            }
        }
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(tmp, dc);
    DestroyWindow(tmp);
    return result;
}

bool CreateContext(HINSTANCE inst) {
    int fmt = ChooseMultisampleFormat(inst);
    V.dc = GetDC(V.hwnd);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    if (fmt) DescribePixelFormat(V.dc, fmt, sizeof(pfd), &pfd);
    else fmt = ChoosePixelFormat(V.dc, &pfd);
    if (!SetPixelFormat(V.dc, fmt, &pfd)) return false;
    V.rc = wglCreateContext(V.dc);
    if (!V.rc || !wglMakeCurrent(V.dc, V.rc)) return false;
    glEnable(GL_MULTISAMPLE_ARB_);
    glGetError(); // GL 1.1 renderers do not know GL_MULTISAMPLE
    if (auto swap = (SwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT")) swap(1);
    return true;
}

void CreateFonts() {
    for (HFONT* f : {&V.font, &V.fontSmall, &V.fontBig})
        if (*f) DeleteObject(*f);
    auto mk = [](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, V.dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    V.font = mk(10, FW_NORMAL);
    V.fontSmall = mk(9, FW_NORMAL);
    V.fontBig = mk(13, FW_SEMIBOLD);
    V.textDirty = true;
}

// ------------------------------------------------------------------ input

void OpenDialog() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC filters[] = {{L"3D-модели (STEP, 3MF, STL)", L"*.step;*.stp;*.3mf;*.stl"},
                                         {L"Все файлы", L"*.*"}};
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetTitle(L"Открыть 3D-модель");
    if (SUCCEEDED(dlg->Show(V.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                StartLoad(p);
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
}

void OnKey(WPARAM key) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const float iy = float(V.settings.yawDeg), ip = float(V.settings.pitchDeg);
    if (ctrl) {
        if (key == 'O') OpenDialog();
        if (key == 'C') CopyToClipboard();
        return;
    }
    switch (key) {
    case 'F':
    case VK_HOME: Fit(); break;
    case '1': case VK_NUMPAD1: SetView(0, 0); break;
    case '2': case VK_NUMPAD2: SetView(180, 0); break;
    case '3': case VK_NUMPAD3: SetView(-90, 0); break;
    case '4': case VK_NUMPAD4: SetView(90, 0); break;
    case '5': case VK_NUMPAD5: SetView(0, 89.9f); break;
    case '6': case VK_NUMPAD6: SetView(0, -89.9f); break;
    case '0': case '7': case VK_NUMPAD0: case VK_NUMPAD7: SetView(iy, ip); break;
    case 'E': V.showEdges = !V.showEdges; break;
    case 'O':
    case 'P': V.perspective = !V.perspective; break;
    case 'B': V.darkBg = !V.darkBg; break;
    case 'H':
    case VK_F1:
        V.showHelp = !V.showHelp;
        V.textDirty = true;
        break;
    case VK_RETURN:
        if (!V.path.empty()) ShellExecuteW(V.hwnd, nullptr, V.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case 'R':
    case 'M':
        V.ruler = !V.ruler;
        V.hover = false;
        if (!V.ruler && !V.measures.empty() && !V.measures.back().done) ClearMeasures(true); // unfinished one
        V.textDirty = true;
        ShowToast(V.ruler ? L"Линейка: щёлкните две точки на модели" : L"Линейка выключена");
        break;
    case VK_BACK: ClearMeasures(true); break;
    case VK_DELETE: ClearMeasures(false); break;
    case VK_ESCAPE:
        // step by step: measurements -> ruler mode -> window
        if (!V.measures.empty()) ClearMeasures(false);
        else if (V.ruler) OnKey('R');
        else DestroyWindow(V.hwnd);
        return;
    default: return;
    }
    InvalidateRect(V.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (V.rc) Render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        V.w = std::max(1, (int)LOWORD(lp));
        V.h = std::max(1, (int)HIWORD(lp));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        V.dpi = HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        CreateFonts();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_LOADING) V.textDirty = true;
        if (wp == TIMER_TOAST) KillTimer(hwnd, TIMER_TOAST);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP_LOADED: OnLoaded(reinterpret_cast<LoadResult*>(lp)); return 0;
    case WM_SETCURSOR:
        if (V.ruler && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (!V.ruler) {
            Fit();
            return 0;
        }
        [[fallthrough]]; // in ruler mode a double click is just two clicks
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        V.drag = ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && !(wp & MK_SHIFT)) ? 1 : 2;
        V.last = V.downPt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        const bool click = std::abs(x - V.downPt.x) <= 3 && std::abs(y - V.downPt.y) <= 3;
        V.drag = 0;
        ReleaseCapture();
        // a click (not a drag) with the ruler adds a point; dragging still rotates the view
        if (msg == WM_LBUTTONUP && click && V.ruler && V.hasModel && !V.loading) AddRulerPoint(x, y);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!V.drag) {
            if (V.ruler && V.hasModel && !V.loading) UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        if (!V.hasModel) return 0;
        V.hover = false;
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        float dx = float(p.x - V.last.x), dy = float(p.y - V.last.y);
        V.last = p;
        if (V.drag == 1) {
            V.yaw -= dx * 0.4f;
            V.pitch = std::clamp(V.pitch + dy * 0.4f, -89.9f, 89.9f);
        } else {
            V3 s, u, f;
            Basis(s, u, f);
            float k = 2.0f * HalfHeight() / float(V.h);
            V.target = V.target + s * (-dx * k) + u * (dy * k);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        float steps = GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA);
        V.dist = std::clamp(V.dist * std::pow(0.87f, steps), V.radius * 0.02f, V.radius * 60.0f);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: OnKey(wp); return 0;
    case WM_DROPFILES: {
        wchar_t file[MAX_PATH * 4];
        if (DragQueryFileW(reinterpret_cast<HDROP>(wp), 0, file, ARRAYSIZE(file))) StartLoad(file);
        DragFinish(reinterpret_cast<HDROP>(wp));
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void LaunchViewer(const std::wstring& path) {
    std::wstring exe = SelfExePath();
    std::wstring cmd = L"\"" + exe + L"\" --view";
    if (!path.empty()) cmd += L" \"" + path + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, DirOf(exe).c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int RunViewer(HINSTANCE inst, const std::wstring& path, const std::wstring& snapshot) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    V.settings = Settings::Get();
    V.snapshot = snapshot;
    if (!snapshot.empty() && _wgetenv(L"CADTHUMB_TEST_RULER")) V.testStage = 0;
    V.yaw = float(V.settings.yawDeg);
    V.pitch = float(V.settings.pitchDeg);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // ~70% of the work area of the monitor under the cursor
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int ww = snapshot.empty() ? (wa.right - wa.left) * 7 / 10 : 1000;
    int wh = snapshot.empty() ? (wa.bottom - wa.top) * 7 / 10 : 700;
    V.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, kWindowClass, L"CadThumb", WS_OVERLAPPEDWINDOW,
                             wa.left + ((wa.right - wa.left) - ww) / 2, wa.top + ((wa.bottom - wa.top) - wh) / 2, ww,
                             wh, nullptr, nullptr, inst, nullptr);
    if (!V.hwnd) return 1;
    V.dpi = GetDpiForWindow(V.hwnd);
    if (!CreateContext(inst)) {
        MessageBoxW(V.hwnd, L"Не удалось инициализировать OpenGL.", L"CadThumb", MB_ICONERROR);
        return 1;
    }
    CreateFonts();
    ShowWindow(V.hwnd, snapshot.empty() ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    UpdateWindow(V.hwnd);
    if (!path.empty()) StartLoad(path);
    else if (snapshot.empty()) OpenDialog();

    MSG msg;
    int code = 0;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    code = (int)msg.wParam;
    // A STEP file may still be loading on a worker thread: do not wait for it.
    ExitProcess(code);
}

} // namespace ct
