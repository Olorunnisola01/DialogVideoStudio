#pragma once

#include <QList>
#include <QObject>
#include <QSize>
#include <QString>

#include <atomic>

#include "core/Project.h"

namespace dvs {

// One piece of the finished video: either a project to render, or a video file
// that is already made.
struct StitchItem {
    QString path;
    QString title; // shown in the list; defaults to the file name

    bool isProject() const { return path.endsWith(QStringLiteral(".dvsproj"), Qt::CaseInsensitive); }
};

struct StitchSettings {
    QString outputPath;
    // Every part is brought to this shape. Projects are rendered at it directly;
    // existing videos are scaled to fit and padded, never cropped or stretched.
    QSize canvas{1080, 1920};
    int fps = 30;
    QString videoCodec = QStringLiteral("libx264");
    QString preset = QStringLiteral("medium");
    int crf = 18;
    int audioBitrateKbps = 192;
};

struct StitchReport {
    int parts = 0;
    bool streamCopied = false; // true when the join needed no re-encode
    QString warning;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

// Renders each project, brings every part to a common format, and joins them
// into one file. Blocking - run it on a worker thread.
class VideoStitcher : public QObject {
    Q_OBJECT

public:
    explicit VideoStitcher(QObject* parent = nullptr);

    StitchReport run(const QList<StitchItem>& items, const StitchSettings& settings);

    void cancel() { cancelled_.store(true); }
    void reset() { cancelled_.store(false); }

signals:
    void progress(int percent);
    void status(const QString& message);
    void log(const QString& line);

private:
    // Re-encodes an existing video to the target shape, letterboxing rather than
    // cropping so nothing of the original is lost.
    QString normaliseVideo(const QString& input, const QString& output,
                           const StitchSettings& settings);
    QString concatenate(const QStringList& parts, const StitchSettings& settings,
                        bool* streamCopied);
    QString runFfmpeg(const QStringList& args, int timeoutMs);

    std::atomic<bool> cancelled_{false};
};

} // namespace dvs
