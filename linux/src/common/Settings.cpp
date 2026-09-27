#include "common/Settings.h"
#include "common/Paths.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace ct {

namespace {

std::unordered_map<std::string, std::string> ReadIni(const std::string& path) {
    std::unordered_map<std::string, std::string> kv;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t c = line.find_first_of(";#");
        if (c != std::string::npos) line.resize(c);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r");
            size_t b = s.find_last_not_of(" \t\r");
            s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val);
        kv[ToLower(key)] = val;
    }
    return kv;
}

int GetInt(const std::unordered_map<std::string, std::string>& kv, const char* key, int def) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return def;
    return atoi(it->second.c_str());
}

bool GetBool(const std::unordered_map<std::string, std::string>& kv, const char* key, bool def) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return def;
    return it->second != "0";
}

double GetDouble(const std::unordered_map<std::string, std::string>& kv, const char* key, double def) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return def;
    return atof(it->second.c_str());
}

uint32_t GetColor(const std::unordered_map<std::string, std::string>& kv, const char* key, uint32_t def,
                  bool allowTransparent) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return def;
    std::string s = it->second;
    if (allowTransparent && (ToLower(s) == "transparent" || s == "none")) return 0;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 || !std::all_of(s.begin(), s.end(), [](char c) { return isxdigit((unsigned char)c); }))
        return def;
    uint32_t v = uint32_t(strtoul(s.c_str(), nullptr, 16));
    return allowTransparent ? (0xFF000000u | v) : v;
}

} // namespace

Settings Settings::Load() {
    Settings s;
    auto kv = ReadIni(SettingsPath());
    if (kv.empty()) return s;
    s.enableStep = GetBool(kv, "enablestep", s.enableStep);
    s.enable3mf = GetBool(kv, "enable3mf", s.enable3mf);
    s.enableStl = GetBool(kv, "enablestl", s.enableStl);
    s.maxFileSizeMB = std::max(1, GetInt(kv, "maxfilesizemb", s.maxFileSizeMB));
    s.renderTimeoutSec = std::max(5, GetInt(kv, "rendertimeoutsec", s.renderTimeoutSec));

    s.prefer3mfEmbedded = GetBool(kv, "prefer3mfembedded", s.prefer3mfEmbedded);
    s.quality = std::clamp(GetDouble(kv, "quality", s.quality), 0.1, 8.0);
    std::string up = ToLower(kv.count("upaxisstep") ? kv.at("upaxisstep") : "auto");
    s.upAxisStep = up == "y" ? 'Y' : up == "z" ? 'Z' : 'A';
    s.yawDeg = GetDouble(kv, "viewyaw", s.yawDeg);
    s.pitchDeg = std::clamp(GetDouble(kv, "viewpitch", s.pitchDeg), -89.0, 89.0);
    s.outline = GetBool(kv, "outline", s.outline);
    s.colorStep = GetColor(kv, "colorstep", s.colorStep, false);
    s.color3mf = GetColor(kv, "color3mf", s.color3mf, false);
    s.colorStl = GetColor(kv, "colorstl", s.colorStl, false);
    s.background = GetColor(kv, "background", 0, true);
    return s;
}

static const char kDefaultIni[] =
    "; CadThumb -- thumbnails for STEP/3MF/STL in a freedesktop.org-compatible file manager\n"
    "; (GNOME Files/Nautilus, Nemo, Caja, PCManFM, Thunar via Tumbler).\n"
    "; Edited values apply the next time a thumbnail is generated; changing them does not\n"
    "; retroactively invalidate thumbnails your file manager already cached.\n"
    "\n"
    "; Files larger than this are not rendered (embedded 3MF thumbnails still work)\n"
    "MaxFileSizeMB=300\n"
    "; Hard safety timeout for one render, in seconds\n"
    "RenderTimeoutSec=60\n"
    "EnableStep=1\n"
    "Enable3mf=1\n"
    "EnableStl=1\n"
    "\n"
    "; 1 = use the picture saved by the slicer inside the 3MF, 0 = always render the model\n"
    "Prefer3mfEmbedded=1\n"
    "; STEP tessellation quality: 0.5 coarser/faster, 2 finer/slower\n"
    "Quality=1.0\n"
    "; STEP vertical axis: Z, Y or auto (Y for SolidWorks/Inventor/Creo, Z otherwise)\n"
    "UpAxisStep=auto\n"
    "ViewYaw=45\n"
    "ViewPitch=30\n"
    "Outline=1\n"
    "ColorStep=#B9C3CE\n"
    "Color3mf=#F0A040\n"
    "ColorStl=#8FB3D9\n"
    "; transparent or #RRGGBB\n"
    "Background=transparent\n";

void Settings::WriteDefaultsIfMissing() {
    std::string path = SettingsPath();
    if (FileExists(path)) return;
    FILE* f = fopen(path.c_str(), "wx");
    if (!f) return;
    fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, f);
    fclose(f);
}

} // namespace ct
