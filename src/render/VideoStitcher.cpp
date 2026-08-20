#include "VideoStitcher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include "core/Paths.h"
#include "render/VideoExporter.h"

namespace dvs {

VideoStitcher::VideoStitcher(QObject* parent) : QObject(parent) {}

QString VideoStitcher::runFfmpeg(const QStringList& args, int timeoutMs) {
    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty()) return QStringLiteral("ffmpeg was not found.");

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(ffmpeg, args);
    if (!proc.waitForStarted(10000)) {
        return QStringLiteral("Could not start ffmpeg: %1").arg(proc.errorString());
    }

    QByteArray output;
    while (proc.state() != QProcess::NotRunning) {
        if (cancelled_.load()) {
            proc.kill();
            proc.waitForFinished(3000);
            return QStringLiteral("Cancelled.");
        }
        proc.waitForFinished(200);
        output.append(proc.readAll());
        if (timeoutMs > 0 && output.size() > (1 << 22)) break; // runaway guard
    }
    output.append(proc.readAll());

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        QString detail = QString::fromLocal8Bit(output).trimmed();
        if (detail.size() > 600) detail = detail.right(600);
        emit log(detail);
        return QStringLiteral("ffmpeg failed: %1").arg(detail);
    }
    return {};
}

namespace {

// ffprobe is not bundled, so ask ffmpeg itself: with no output file it prints
// the stream table and exits non-zero. A part with no audio track has to be
// given silence, or the join produces a file whose audio stops partway.
bool hasAudioStream(const QString& path) {
    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty()) return true;

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-i"), path});
    if (!proc.waitForStarted(10000)) return true;
    proc.waitForFinished(20000);
    return QString::fromLocal8Bit(proc.readAll()).contains(QLatin1String("Audio:"));
}

} // namespace

QString VideoStitcher::normaliseVideo(const QString& input, const QString& output,
                                      const StitchSettings& settings) {
    const int w = settings.canvas.width() & ~1;
    const int h = settings.canvas.height() & ~1;

    // Fit inside the frame and pad the remainder, so a part shot at a different
    // aspect ratio is letterboxed rather than cropped or squashed.
    const QString filter =
        QStringLiteral("scale=%1:%2:force_original_aspect_ratio=decrease,"
                       "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,fps=%3")
            .arg(w)
            .arg(h)
            .arg(settings.fps);

    const bool hasAudio = hasAudioStream(input);

    QStringList args{
        QStringLiteral("-y"), QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-i"), input,
    };
    // Every input has to be declared before any output option.
    if (!hasAudio) {
        args << QStringLiteral("-f") << QStringLiteral("lavfi")
             << QStringLiteral("-i")
             << QStringLiteral("anullsrc=channel_layout=stereo:sample_rate=48000");
    }

    args << QStringLiteral("-vf") << filter
         << QStringLiteral("-map") << QStringLiteral("0:v:0")
         << QStringLiteral("-map") << (hasAudio ? QStringLiteral("0:a:0")
                                                : QStringLiteral("1:a:0"))
         << QStringLiteral("-c:v") << settings.videoCodec
         << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");

    if (settings.videoCodec.startsWith(QStringLiteral("libx26"))) {
        args << QStringLiteral("-preset") << settings.preset
             << QStringLiteral("-crf") << QString::number(settings.crf);
    } else {
        args << QStringLiteral("-cq") << QString::number(settings.crf);
    }

    // Match what VideoExporter writes, so the join can be a stream copy.
    args << QStringLiteral("-c:a") << QStringLiteral("aac")
         << QStringLiteral("-ar") << QStringLiteral("48000")
         << QStringLiteral("-ac") << QStringLiteral("2")
         << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(settings.audioBitrateKbps);
    // Silence is infinite, so it has to be cut to the video's length.
    if (!hasAudio) args << QStringLiteral("-shortest");
    args << output;

    return runFfmpeg(args, 0);
}

QString VideoStitcher::concatenate(const QStringList& parts, const StitchSettings& settings,
                                   bool* streamCopied) {
    const QString listPath = QFileInfo(parts.first()).absolutePath() + QStringLiteral("/parts.txt");
    QFile list(listPath);
    if (!list.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return QStringLiteral("Could not write the join list.");
    }
    for (const QString& part : parts) {
        // The concat demuxer needs single quotes doubled inside a quoted path.
        QString escaped = QDir::toNativeSeparators(part);
        escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
        list.write(QStringLiteral("file '%1'\n").arg(escaped).toUtf8());
    }
    list.close();

    const QStringList common{
        QStringLiteral("-y"), QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-f"), QStringLiteral("concat"),
        QStringLiteral("-safe"), QStringLiteral("0"),
        QStringLiteral("-i"), listPath,
    };

    // Every part was written with the same codec and audio parameters, so the
    // join is normally a copy - instant, and it does not re-compress anything.
    emit status(QStringLiteral("Joining %1 parts...").arg(parts.size()));
    QStringList copyArgs = common;
    copyArgs << QStringLiteral("-c") << QStringLiteral("copy")
             << QStringLiteral("-movflags") << QStringLiteral("+faststart")
             << settings.outputPath;
    const QString copyError = runFfmpeg(copyArgs, 0);
    if (copyError.isEmpty()) {
        *streamCopied = true;
        return {};
    }
    if (cancelled_.load()) return copyError;

    // Something about the parts still did not line up; fall back to re-encoding
    // the join rather than failing.
    emit status(QStringLiteral("Re-encoding the join..."));
    QStringList encodeArgs = common;
    encodeArgs << QStringLiteral("-c:v") << settings.videoCodec
               << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    if (settings.videoCodec.startsWith(QStringLiteral("libx26"))) {
        encodeArgs << QStringLiteral("-preset") << settings.preset
                   << QStringLiteral("-crf") << QString::number(settings.crf);
    }
    encodeArgs << QStringLiteral("-c:a") << QStringLiteral("aac")
               << QStringLiteral("-ar") << QStringLiteral("48000")
               << QStringLiteral("-ac") << QStringLiteral("2")
               << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(settings.audioBitrateKbps)
               << QStringLiteral("-movflags") << QStringLiteral("+faststart")
               << settings.outputPath;

    *streamCopied = false;
    return runFfmpeg(encodeArgs, 0);
}

StitchReport VideoStitcher::run(const QList<StitchItem>& items, const StitchSettings& settings) {
    StitchReport report;
    cancelled_.store(false);

    if (items.isEmpty()) {
        report.error = QStringLiteral("Add at least one part first.");
        return report;
    }
    if (settings.outputPath.isEmpty()) {
        report.error = QStringLiteral("No output file was chosen.");
        return report;
    }
    if (ffmpegPath().isEmpty()) {
        report.error = QStringLiteral("ffmpeg was not found.");
        return report;
    }
    for (const StitchItem& item : items) {
        if (!QFileInfo::exists(item.path)) {
            report.error = QStringLiteral("Missing: %1").arg(item.path);
            return report;
        }
    }

    QTemporaryDir temp;
    if (!temp.isValid()) {
        report.error = QStringLiteral("Could not create a working folder.");
        return report;
    }

    // Each part gets an equal slice of the bar, with the last tenth for the join.
    const int count = static_cast<int>(items.size());
    const auto partProgress = [&](int index, int within) {
        emit progress(static_cast<int>((90.0 * index + 0.9 * within) / count));
    };

    QStringList parts;
    for (int i = 0; i < count; ++i) {
        if (cancelled_.load()) {
            report.error = QStringLiteral("Cancelled.");
            return report;
        }

        const StitchItem& item = items.at(i);
        const QString partPath =
            temp.filePath(QStringLiteral("part_%1.mp4").arg(i, 3, 10, QLatin1Char('0')));
        emit status(QStringLiteral("Part %1 of %2: %3")
                        .arg(i + 1)
                        .arg(count)
                        .arg(QFileInfo(item.path).fileName()));

        if (item.isProject()) {
            QString loadError;
            Project part = Project::load(item.path, &loadError);
            if (!loadError.isEmpty()) {
                report.error = loadError;
                return report;
            }
            // Force every part to the finished video's shape so the join works
            // and nothing jumps size mid-video.
            part.canvas = settings.canvas;
            part.fps = settings.fps;
            part.exportSettings.videoCodec = settings.videoCodec;
            part.exportSettings.preset = settings.preset;
            part.exportSettings.crf = settings.crf;
            part.exportSettings.audioBitrateKbps = settings.audioBitrateKbps;
            part.exportSettings.outputPath = partPath;

            VideoExporter exporter;
            connect(&exporter, &VideoExporter::progress, this,
                    [&](int pct) { partProgress(i, pct); });
            connect(&exporter, &VideoExporter::log, this, &VideoStitcher::log);
            if (cancelled_.load()) exporter.cancel();

            const QString error = exporter.run(part);
            if (!error.isEmpty()) {
                report.error = QStringLiteral("%1: %2").arg(QFileInfo(item.path).fileName(), error);
                return report;
            }
        } else {
            const QString error = normaliseVideo(item.path, partPath, settings);
            if (!error.isEmpty()) {
                report.error = QStringLiteral("%1: %2").arg(QFileInfo(item.path).fileName(), error);
                return report;
            }
            partProgress(i, 100);
        }

        parts << partPath;
    }

    QDir().mkpath(QFileInfo(settings.outputPath).absolutePath());
    const QString error = concatenate(parts, settings, &report.streamCopied);
    if (!error.isEmpty()) {
        report.error = error;
        return report;
    }

    report.parts = count;
    emit progress(100);
    return report;
}

} // namespace dvs
