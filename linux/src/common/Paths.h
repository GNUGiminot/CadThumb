#pragma once
#include <string>

namespace ct {

// Paths use plain UTF-8 std::string (the native encoding for Linux filesystem paths and argv),
// unlike the Windows build which uses std::wstring/UTF-16 throughout.

std::string ConfigDir();  // $XDG_CONFIG_HOME/cadthumb or ~/.config/cadthumb
std::string SettingsPath(); // .../settings.ini

std::string DirOf(const std::string& path);
std::string FileNameOf(const std::string& path);
std::string ExtOf(const std::string& path); // lower-case, with dot: "/a/b.STEP" -> ".step"
std::string ToLower(std::string s);

bool FileExists(const std::string& path);
bool EnsureDir(const std::string& path);

// Decodes a "file://" URI (as passed by %u in a .thumbnailer Exec= line) into a plain path.
// Returns the input unchanged if it is not a file:// URI.
std::string UriToPath(const std::string& uriOrPath);

} // namespace ct
