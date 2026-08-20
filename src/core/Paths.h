#pragma once

#include <QString>

namespace dvs {

// Locates ffmpeg.exe: the DVS_FFMPEG environment variable, then a user
// override stored in QSettings, then tools/ffmpeg.exe next to the executable
// (the bundled copy), then PATH. Returns an empty string if none resolve.
QString ffmpegPath();

// Sets (or clears, when empty) the persisted ffmpeg override.
void setFfmpegOverride(const QString& path);

// Absolute path to the speaker-embedding model, or an empty string if it has
// not been fetched yet (scripts/fetch_deps.ps1).
QString speakerModelPath();

// Directory the app writes caches to (%LOCALAPPDATA%/DialogVideoStudio).
QString cacheDir();

} // namespace dvs
