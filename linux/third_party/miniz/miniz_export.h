// Hand-written replacement for the file miniz's own CMake build generates via GenerateExportHeader.
// We compile miniz.c straight into cadthumb-thumbnailer (no shared-library boundary), so no symbol
// visibility annotation is needed.
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT

#endif
