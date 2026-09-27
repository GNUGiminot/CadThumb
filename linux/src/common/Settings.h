#pragma once
#include <cstdint>

namespace ct {

// Rendering options only: the Linux build has no background service, so the queue/cache/autostart
// knobs from the Windows Settings struct do not apply here — the desktop environment's own
// thumbnailing daemon (Tumbler, gnome-desktop, ...) already provides caching, concurrency limits and
// a per-call timeout. Field names match the Windows [Render] section for a consistent settings.ini.
struct Settings {
    bool enableStep = true;
    bool enable3mf = true;
    bool enableStl = true;
    int maxFileSizeMB = 300;
    int renderTimeoutSec = 60; // safety watchdog; most desktop thumbnailers time out well before this anyway

    bool prefer3mfEmbedded = true;
    double quality = 1.0;
    char upAxisStep = 'A'; // 'Z', 'Y' or 'A' (auto, by originating CAD system)
    double yawDeg = 45.0;
    double pitchDeg = 30.0;
    bool outline = true;
    uint32_t colorStep = 0xB9C3CE;
    uint32_t color3mf = 0xF0A040;
    uint32_t colorStl = 0x8FB3D9;
    uint32_t background = 0; // 0xAARRGGBB, alpha 0 = transparent

    static Settings Load(); // reads ~/.config/cadthumb/settings.ini if present, else defaults
    static void WriteDefaultsIfMissing();
};

} // namespace ct
