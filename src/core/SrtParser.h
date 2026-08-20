#pragma once

#include <QList>
#include <QString>

namespace dvs {

struct SrtCue {
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString text;
};

struct SrtParseResult {
    QList<SrtCue> cues;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Parses SubRip content. Tolerant of the variations Whisper-family tools emit:
// CRLF or LF, missing index lines, `HH:MM:SS,mmm` / `MM:SS.mmm` / `SS.mmm`
// timestamps, BOM, and inline `<i>`/`{\an8}` style tags (stripped).
SrtParseResult parseSrt(const QString& content);
SrtParseResult parseSrtFile(const QString& path);

QString formatTimestamp(qint64 ms);

} // namespace dvs
