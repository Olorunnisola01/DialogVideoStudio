#include "VideoExporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>

#include <algorithm>

#include "core/Paths.h"
#include "render/FrameRenderer.h"

namespace dvs {

namespace {

// Everything that can change what a frame looks like. Frames whose key matches
// the previous one are re-sent from the buffer instead of being re-rendered -
// which is most of them, since the picture is static between caption changes.
// Alphas are quantised to 8 bits so a fade re-renders once per visible step
// rather than once per frame.
QByteArray frameStateKey(const Project& project, qint64 ms) {
    QByteArray key = QByteArray::number(project.sceneAt(ms));
    for (const Project::ActiveSegment& active : project.activeSegmentsAt(ms)) {
        key += '|';
        key += QByteArray::number(active.index);
        key += ':';
        key += QByteArray::number(static_cast<int>(active.alpha * 255.0 + 0.5));
    }
    for (const Overlay& overlay : project.overlays) {
        key += '#';
        key += QByteArray::number(
            static_cast<int>(overlay.alphaAt(ms, project.transitionMs) * 255.0 + 0.5));
    }
    return key;
}

} // namespace

VideoExporter::VideoExporter(QObject* parent) : QObject(parent) {}

QString VideoExporter::run(const Project& project) {
    cancelled_.store(false);

    if (project.exportSettings.outputPath.isEmpty()) {
        return QStringLiteral("No output file was chosen.");
    }
    if (project.audioPath.isEmpty() || !QFileInfo::exists(project.audioPath)) {
        return QStringLiteral("The audio file is missing: %1").arg(project.audioPath);
    }
    if (project.fps <= 0) {
        return QStringLiteral("Frame rate must be greater than zero.");
    }

    // H.264 requires even dimensions.
    const QSize canvas(project.canvas.width() & ~1, project.canvas.height() & ~1);
    if (canvas.width() < 2 || canvas.height() < 2) {
        return QStringLiteral("The canvas size is too small.");
    }

    qint64 durationMs = project.durationMs;
    for (const Segment& s : project.segments) durationMs = std::max(durationMs, s.endMs);
    for (const Scene& s : project.scenes) durationMs = std::max(durationMs, s.endMs);
    if (durationMs <= 0) {
        return QStringLiteral("Nothing to render - load audio and subtitles first.");
    }

    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty()) {
        return QStringLiteral(
            "ffmpeg was not found. Put ffmpeg.exe in the app's tools/ folder, set the "
            "DVS_FFMPEG environment variable, or choose it in Settings.");
    }

    QDir().mkpath(QFileInfo(project.exportSettings.outputPath).absolutePath());

    Project job = project;
    job.canvas = canvas;

    const qint64 totalFrames =
        (durationMs * job.fps + 999) / 1000; // ceil

    QStringList args{
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-nostdin"),
        // video: raw frames on stdin
        QStringLiteral("-f"), QStringLiteral("rawvideo"),
        QStringLiteral("-pixel_format"), QStringLiteral("bgra"),
        QStringLiteral("-video_size"), QStringLiteral("%1x%2").arg(canvas.width()).arg(canvas.height()),
        QStringLiteral("-framerate"), QString::number(job.fps),
        QStringLiteral("-i"), QStringLiteral("-"),
        // audio
        QStringLiteral("-i"), job.audioPath,
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("1:a:0"),
        QStringLiteral("-c:v"), job.exportSettings.videoCodec,
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
    };

    if (job.exportSettings.videoCodec.startsWith(QStringLiteral("libx26"))) {
        args << QStringLiteral("-preset") << job.exportSettings.preset
             << QStringLiteral("-crf") << QString::number(job.exportSettings.crf);
    } else {
        // NVENC/AMF/QSV take -cq rather than -crf.
        args << QStringLiteral("-cq") << QString::number(job.exportSettings.crf);
    }

    // Fixed 48 kHz stereo AAC. Nothing needs it for a single export, but it is
    // what lets several exports be joined later with a stream copy instead of a
    // re-encode - parts whose audio parameters differ cannot simply be
    // concatenated.
    args << QStringLiteral("-c:a") << QStringLiteral("aac")
         << QStringLiteral("-ar") << QStringLiteral("48000")
         << QStringLiteral("-ac") << QStringLiteral("2")
         << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(job.exportSettings.audioBitrateKbps)
         << QStringLiteral("-movflags") << QStringLiteral("+faststart")
         << QStringLiteral("-shortest")
         << job.exportSettings.outputPath;

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(ffmpeg, args);
    if (!proc.waitForStarted(10000)) {
        return QStringLiteral("Could not start ffmpeg: %1").arg(proc.errorString());
    }

    FrameRenderer renderer;
    QImage frame;
    QByteArray frameBytes;
    QByteArray lastKey;
    int lastPercent = -1;

    for (qint64 f = 0; f < totalFrames; ++f) {
        if (cancelled_.load()) {
            proc.closeWriteChannel();
            proc.waitForFinished(3000);
            if (proc.state() != QProcess::NotRunning) proc.kill();
            QFile::remove(job.exportSettings.outputPath);
            return QStringLiteral("Export cancelled.");
        }
        if (proc.state() == QProcess::NotRunning) {
            const QString err = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
            return QStringLiteral("ffmpeg stopped early%1")
                .arg(err.isEmpty() ? QString() : QStringLiteral(": ") + err);
        }

        const qint64 timeMs = f * 1000 / job.fps;

        const QByteArray key = frameStateKey(job, timeMs);
        if (key != lastKey || frameBytes.isEmpty()) {
            frame = renderer.renderFrame(job, timeMs).convertToFormat(QImage::Format_ARGB32);
            frameBytes = QByteArray(reinterpret_cast<const char*>(frame.constBits()),
                                    static_cast<int>(frame.sizeInBytes()));
            lastKey = key;
        }

        qint64 written = 0;
        while (written < frameBytes.size()) {
            const qint64 n = proc.write(frameBytes.constData() + written,
                                        frameBytes.size() - written);
            if (n < 0) {
                return QStringLiteral("Lost the connection to ffmpeg while writing frames.");
            }
            written += n;
            if (!proc.waitForBytesWritten(30000)) {
                if (proc.state() == QProcess::NotRunning) break;
                return QStringLiteral("ffmpeg stopped accepting frames (timed out).");
            }
        }

        const int percent = static_cast<int>(95 * (f + 1) / totalFrames);
        if (percent != lastPercent) {
            lastPercent = percent;
            emit progress(percent);
        }

        const QByteArray err = proc.readAllStandardError();
        if (!err.isEmpty()) emit log(QString::fromLocal8Bit(err).trimmed());
    }

    proc.closeWriteChannel();
    if (!proc.waitForFinished(120000)) {
        proc.kill();
        return QStringLiteral("ffmpeg did not finish writing the file.");
    }

    const QString stderrText = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
    if (!stderrText.isEmpty()) emit log(stderrText);

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        return QStringLiteral("ffmpeg failed%1")
            .arg(stderrText.isEmpty() ? QString() : QStringLiteral(": ") + stderrText);
    }

    emit progress(100);
    return {};
}

} // namespace dvs
