#pragma once

namespace ct {

// Appends one line to %LOCALAPPDATA%\CadThumb\cadthumb.log (never throws, silently ignores errors).
void Log(const wchar_t* fmt, ...);
// Same, but only when VerboseLog=1.
void LogVerbose(const wchar_t* fmt, ...);
void SetLogTag(const wchar_t* tag); // "dll", "host", "render"...

} // namespace ct
