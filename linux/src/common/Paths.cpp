#include "common/Paths.h"

#include <sys/stat.h>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace ct {

std::string ConfigDir() {
    static const std::string dir = [] {
        std::string base;
        if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg) base = xdg;
        else if (const char* home = getenv("HOME"); home && *home) base = std::string(home) + "/.config";
        else base = "/tmp";
        std::string d = base + "/cadthumb";
        EnsureDir(d);
        return d;
    }();
    return dir;
}

std::string SettingsPath() { return ConfigDir() + "/settings.ini"; }

std::string DirOf(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string(".") : (p == 0 ? std::string("/") : path.substr(0, p));
}

std::string FileNameOf(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = char(tolower((unsigned char)c));
    return s;
}

std::string ExtOf(const std::string& path) {
    std::string name = FileNameOf(path);
    size_t p = name.find_last_of('.');
    return p == std::string::npos ? std::string() : ToLower(name.substr(p));
}

bool FileExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool EnsureDir(const std::string& path) {
    if (path.empty()) return false;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    // create parents first (mkdir -p)
    size_t pos = path.find('/', 1);
    while (pos != std::string::npos) {
        std::string parent = path.substr(0, pos);
        if (!parent.empty()) mkdir(parent.c_str(), 0755);
        pos = path.find('/', pos + 1);
    }
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UriToPath(const std::string& in) {
    const std::string prefix = "file://";
    if (in.compare(0, prefix.size(), prefix) != 0) return in;
    std::string rest = in.substr(prefix.size());
    // strip an optional "hostname" component (file://hostname/path); empty host is the common case
    size_t slash = rest.find('/');
    if (slash != std::string::npos && slash != 0) rest = rest.substr(slash);
    std::string out;
    out.reserve(rest.size());
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            int hi = HexVal(rest[i + 1]), lo = HexVal(rest[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(char((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(rest[i]);
    }
    return out;
}

} // namespace ct
