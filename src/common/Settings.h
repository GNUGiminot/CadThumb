#pragma once
#include <cstdint>
#include <string>

namespace ct {

struct Settings {
    // [General]
    bool enableStep = true;
    bool enable3mf = true;
    bool enableStl = true;
    int maxFileSizeMB = 300;     // larger files are not rendered (3MF embedded thumbnails still work)
    int handlerWaitSec = 15;     // how long the Explorer handler waits for a render
    int renderTimeoutSec = 300;  // hard limit for one background render
    int maxParallel = 2;         // concurrent render processes
    int memoryLimitMB = 4096;    // per render process
    int failRetryHours = 24;     // how long a failed render is not retried
    int cacheMaxMB = 1024;
    bool verboseLog = false;

    // [Render]
    bool prefer3mfEmbedded = true;
    double quality = 1.0;        // tessellation quality multiplier for STEP
    wchar_t upAxisStep = L'A';   // 'Z', 'Y' or 'A' (auto by originating CAD system)
    double yawDeg = 45.0;
    double pitchDeg = 30.0;
    bool outline = true;
    uint32_t colorStep = 0xB9C3CE;
    uint32_t color3mf = 0xF0A040;
    uint32_t colorStl = 0x8FB3D9;
    uint32_t background = 0;     // 0xAARRGGBB, alpha 0 = transparent

    // Hash of everything above that affects the rendered image (part of the cache key).
    uint64_t renderSignature = 0;

    // Cached load; re-reads the INI when its timestamp changes.
    static Settings Get();
    static void WriteDefaultsIfMissing();
};

} // namespace ct
