#include "common/Settings.h"
#include "common/Hash.h"
#include "common/Paths.h"

#include <windows.h>
#include <cwchar>
#include <algorithm>
#include <mutex>
#include <cmath>
#include <cwctype>

namespace ct {

static constexpr int kRendererVersion = 5; // bump to invalidate every cached thumbnail

static std::wstring ReadStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def, const std::wstring& ini) {
    wchar_t buf[256]{};
    GetPrivateProfileStringW(sec, key, def, buf, 256, ini.c_str());
    // strip inline comments and whitespace
    std::wstring s = buf;
    size_t c = s.find_first_of(L";#", s.empty() || s[0] != L'#' ? 0 : 1);
    if (c != std::wstring::npos && c > 0) s.resize(c);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}

static int ReadInt(const wchar_t* sec, const wchar_t* key, int def, const std::wstring& ini) {
    std::wstring s = ReadStr(sec, key, L"", ini);
    if (s.empty()) return def;
    return _wtoi(s.c_str());
}

static double ReadDouble(const wchar_t* sec, const wchar_t* key, double def, const std::wstring& ini) {
    std::wstring s = ReadStr(sec, key, L"", ini);
    if (s.empty()) return def;
    for (auto& ch : s) if (ch == L',') ch = L'.';
    wchar_t* end = nullptr;
    double value = wcstod(s.c_str(), &end);
    return end != s.c_str() && *end == 0 && std::isfinite(value) ? value : def;
}

static uint32_t ReadColor(const wchar_t* sec, const wchar_t* key, uint32_t def, bool allowTransparent,
                          const std::wstring& ini) {
    std::wstring s = ReadStr(sec, key, L"", ini);
    if (s.empty()) return def;
    if (allowTransparent && (_wcsicmp(s.c_str(), L"transparent") == 0 || s == L"none")) return 0;
    if (s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6) return def;
    if (!std::all_of(s.begin(), s.end(), [](wchar_t c) { return iswxdigit(c) != 0; })) return def;
    uint32_t v = wcstoul(s.c_str(), nullptr, 16);
    return allowTransparent ? (0xFF000000u | v) : v;
}

static Settings LoadFromIni(const std::wstring& ini) {
    Settings s;
    const wchar_t* G = L"General";
    const wchar_t* R = L"Render";
    s.enableStep = ReadInt(G, L"EnableStep", 1, ini) != 0;
    s.enable3mf = ReadInt(G, L"Enable3mf", 1, ini) != 0;
    s.enableStl = ReadInt(G, L"EnableStl", 1, ini) != 0;
    s.maxFileSizeMB = std::max(1, ReadInt(G, L"MaxFileSizeMB", s.maxFileSizeMB, ini));
    s.handlerWaitSec = std::min(120, std::max(1, ReadInt(G, L"HandlerWaitSec", s.handlerWaitSec, ini)));
    s.renderTimeoutSec = std::clamp(ReadInt(G, L"RenderTimeoutSec", s.renderTimeoutSec, ini), 5, 86400);
    s.maxParallel = std::min(16, std::max(1, ReadInt(G, L"MaxParallelRenders", s.maxParallel, ini)));
    s.memoryLimitMB = std::max(256, ReadInt(G, L"RenderMemoryLimitMB", s.memoryLimitMB, ini));
    s.failRetryHours = std::max(0, ReadInt(G, L"FailRetryHours", s.failRetryHours, ini));
    s.cacheMaxMB = std::max(16, ReadInt(G, L"CacheMaxMB", s.cacheMaxMB, ini));
    s.verboseLog = ReadInt(G, L"VerboseLog", 0, ini) != 0;

    s.prefer3mfEmbedded = ReadInt(R, L"Prefer3mfEmbedded", 1, ini) != 0;
    s.quality = std::min(8.0, std::max(0.1, ReadDouble(R, L"Quality", s.quality, ini)));
    std::wstring up = ReadStr(R, L"UpAxisStep", L"auto", ini);
    s.upAxisStep = (up == L"Y" || up == L"y") ? L'Y' : (up == L"Z" || up == L"z") ? L'Z' : L'A';
    s.yawDeg = ReadDouble(R, L"ViewYaw", s.yawDeg, ini);
    s.pitchDeg = std::min(89.0, std::max(-89.0, ReadDouble(R, L"ViewPitch", s.pitchDeg, ini)));
    s.outline = ReadInt(R, L"Outline", 1, ini) != 0;
    s.colorStep = ReadColor(R, L"ColorStep", s.colorStep, false, ini);
    s.color3mf = ReadColor(R, L"Color3mf", s.color3mf, false, ini);
    s.colorStl = ReadColor(R, L"ColorStl", s.colorStl, false, ini);
    s.background = ReadColor(R, L"Background", 0, true, ini);

    Hasher h(0x5EED);
    h.Add(kRendererVersion);
    h.Add(s.prefer3mfEmbedded);
    h.Add(s.quality);
    h.Add(s.upAxisStep);
    h.Add(s.yawDeg);
    h.Add(s.pitchDeg);
    h.Add(s.outline);
    h.Add(s.colorStep);
    h.Add(s.color3mf);
    h.Add(s.colorStl);
    h.Add(s.background);
    s.renderSignature = h.Digest64();
    return s;
}

Settings Settings::Get() {
    static std::mutex mtx;
    static Settings cached = LoadFromIni(SettingsPath());
    static FILETIME cachedTime{};
    static ULONGLONG lastCheck = 0;

    std::lock_guard<std::mutex> lock(mtx);
    ULONGLONG now = GetTickCount64();
    if (now - lastCheck > 2000 || lastCheck == 0) {
        lastCheck = now;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        FILETIME ft{};
        if (GetFileAttributesExW(SettingsPath().c_str(), GetFileExInfoStandard, &fad)) ft = fad.ftLastWriteTime;
        if (CompareFileTime(&ft, &cachedTime) != 0) {
            cachedTime = ft;
            cached = LoadFromIni(SettingsPath());
        }
    }
    return cached;
}

static const wchar_t kDefaultIni[] =
    L"; CadThumb — настройки эскизов STEP / 3MF / STL для Проводника Windows.\r\n"
    L"; Изменения применяются автоматически (через пару секунд).\r\n"
    L"; Изменение параметров секции [Render] делает старые эскизы недействительными.\r\n"
    L"\r\n"
    L"[General]\r\n"
    L"; Включить эскизы для типов файлов (1/0). Регистрация в Проводнике: CadThumb.exe --register\r\n"
    L"EnableStep=1\r\n"
    L"Enable3mf=1\r\n"
    L"EnableStl=1\r\n"
    L"; Файлы больше этого размера не рендерятся (встроенные эскизы 3MF работают всегда)\r\n"
    L"MaxFileSizeMB=300\r\n"
    L"; Сколько секунд Проводник ждёт эскиз. Если не успели — рендер продолжается в фоне,\r\n"
    L"; эскиз появится при следующем открытии папки (или F5).\r\n"
    L"HandlerWaitSec=15\r\n"
    L"; Жёсткий предел на один фоновый рендер (сек). Процесс рендера после этого убивается.\r\n"
    L"RenderTimeoutSec=300\r\n"
    L"; Сколько файлов рендерится одновременно\r\n"
    L"MaxParallelRenders=2\r\n"
    L"; Лимит памяти на один процесс рендера (МБ)\r\n"
    L"RenderMemoryLimitMB=4096\r\n"
    L"; Через сколько часов повторять попытку для файла, который не удалось отрендерить\r\n"
    L"FailRetryHours=24\r\n"
    L"; Максимальный размер собственного кэша эскизов (МБ)\r\n"
    L"CacheMaxMB=1024\r\n"
    L"VerboseLog=0\r\n"
    L"\r\n"
    L"[Render]\r\n"
    L"; 1 = для 3MF брать эскиз, сохранённый слайсером (быстро), 0 = всегда рендерить модель\r\n"
    L"Prefer3mfEmbedded=1\r\n"
    L"; Точность тесселяции STEP: 0.5 — грубее/быстрее, 2 — точнее/медленнее\r\n"
    L"Quality=1.0\r\n"
    L"; Вертикальная ось STEP: Z, Y или auto (Y для SolidWorks/Inventor/Creo, иначе Z)\r\n"
    L"UpAxisStep=auto\r\n"
    L"; Направление взгляда (градусы): поворот вокруг вертикали и наклон\r\n"
    L"ViewYaw=45\r\n"
    L"ViewPitch=30\r\n"
    L"; Контурные линии (силуэт и острые рёбра)\r\n"
    L"Outline=1\r\n"
    L"; Цвета по умолчанию (если в файле нет своих цветов)\r\n"
    L"ColorStep=#B9C3CE\r\n"
    L"Color3mf=#F0A040\r\n"
    L"ColorStl=#8FB3D9\r\n"
    L"; Фон: transparent или #RRGGBB\r\n"
    L"Background=transparent\r\n";

void Settings::WriteDefaultsIfMissing() {
    std::wstring path = SettingsPath();
    if (FileExists(path)) return;
    // UTF-16 LE with BOM so that GetPrivateProfileStringW reads Cyrillic comments correctly.
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const wchar_t bom = 0xFEFF;
    DWORD w = 0;
    WriteFile(f, &bom, sizeof(bom), &w, nullptr);
    WriteFile(f, kDefaultIni, (DWORD)(wcslen(kDefaultIni) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
}

} // namespace ct
