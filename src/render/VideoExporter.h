#pragma once

#include <QObject>
#include <QString>

#include <atomic>

#include "core/Project.h"

namespace dvs {

// Renders every frame with FrameRenderer and pipes it straight into ffmpeg as
// raw BGRA - no PNG temp files. run() blocks, so call it from a worker thread
// (QtConcurrent::run) and connect to the signals.
class VideoExporter : public QObject {
    Q_OBJECT

public:
    explicit VideoExporter(QObject* parent = nullptr);

    // Returns an empty string on success, or a message describing the failure.
    QString run(const Project& project);

    // Safe to call from another thread.
    void cancel() { cancelled_.store(true); }
    bool wasCancelled() const { return cancelled_.load(); }
    void reset() { cancelled_.store(false); }

signals:
    void progress(int percent);
    void log(const QString& line);

private:
    std::atomic<bool> cancelled_{false};
};

} // namespace dvs
