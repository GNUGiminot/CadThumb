// cadthumb-thumbnailer -- generates a thumbnail for a STEP/3MF/STL file.
//
// Implements the freedesktop.org "Thumbnailer Entry" contract used by GNOME Files/Nautilus, Nemo,
// Caja, PCManFM and Thunar (via Tumbler): the file manager itself provides caching, concurrency and
// a timeout, so this program is a plain, synchronous, one-shot CLI with no service of its own --
// see packaging/cadthumb.thumbnailer for the registration.
//
//   cadthumb-thumbnailer <file-or-uri> <output.png> <size>
#include "common/FileType.h"
#include "common/Paths.h"
#include "common/Settings.h"
#include "render/RenderFile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

using namespace ct;

namespace {

std::atomic<bool> g_done{false};

// Safety net: a malformed file could in principle make OpenCASCADE spin far longer than expected.
// Most file managers already enforce their own timeout on the whole process, but this guarantees we
// exit on our own rather than becoming a stuck background process that outlives the file manager.
void ArmWatchdog(int seconds) {
    std::thread([seconds] {
        for (int waited = 0; waited < seconds; ++waited) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_done.load()) return;
        }
        if (!g_done.load()) _exit(2);
    }).detach();
}

} // namespace

int main(int argc, char** argv) {
    Settings::WriteDefaultsIfMissing(); // also on --help/--version: `cadthumb settings` can rely on it existing

    if (argc == 2 && (strcmp(argv[1], "--version") == 0)) {
        printf("cadthumb-thumbnailer " CADTHUMB_VERSION "\n");
        return 0;
    }
    if (argc != 4 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr,
                "usage: cadthumb-thumbnailer <file-or-uri> <output.png> <size>\n"
                "  Renders a thumbnail for a .step/.stp/.3mf (or .stl) file.\n"
                "  Settings: %s (created with defaults on first run)\n"
                "  Management: run 'cadthumb' for status/settings/install/uninstall.\n",
                SettingsPath().c_str());
        return 3;
    }

    const Settings s = Settings::Load();
    ArmWatchdog(s.renderTimeoutSec);

    const std::string input = UriToPath(argv[1]);
    const std::string output = argv[2];
    const int size = atoi(argv[3]) > 0 ? atoi(argv[3]) : 256;

    if (!FileExists(input)) {
        fprintf(stderr, "cadthumb-thumbnailer: file not found: %s\n", input.c_str());
        g_done = true;
        return 1;
    }

    FileType type = FileTypeFromExtension(ExtOf(input));
    if (type == FileType::Unknown) {
        uint8_t head[512] = {};
        size_t n = 0;
        if (FILE* f = fopen(input.c_str(), "rb")) {
            n = fread(head, 1, sizeof(head), f);
            fclose(f);
        }
        type = FileTypeSniff(head, n);
    }
    if (type == FileType::Unknown || !FileTypeEnabled(type, s)) {
        fprintf(stderr, "cadthumb-thumbnailer: unsupported or disabled file type: %s\n", input.c_str());
        g_done = true;
        return 1;
    }

    long fileSizeMB = 0;
    if (FILE* f = fopen(input.c_str(), "rb")) {
        fseeko(f, 0, SEEK_END);
        fileSizeMB = (long)(ftello(f) / (1024 * 1024));
        fclose(f);
    }
    if (fileSizeMB > s.maxFileSizeMB) {
        fprintf(stderr, "cadthumb-thumbnailer: %s is %ld MB, over MaxFileSizeMB=%d\n", input.c_str(), fileSizeMB,
                s.maxFileSizeMB);
        g_done = true;
        return 1;
    }

    Image img;
    std::string error, details;
    bool ok = RenderFileToImage(input, type, size, s, img, error, &details);
    if (ok) ok = SavePng(output, img);
    g_done = true;

    if (!ok) {
        fprintf(stderr, "cadthumb-thumbnailer: %s: %s\n", input.c_str(), error.c_str());
        return 1;
    }
    fprintf(stderr, "cadthumb-thumbnailer: %s: %s\n", input.c_str(), details.c_str());
    return 0;
}
