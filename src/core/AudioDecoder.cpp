#include "AudioDecoder.h"

#include <QByteArray>
#include <QFileInfo>
#include <QProcess>

#include <algorithm>
#include <cstring>

#include "Paths.h"

namespace dvs {

AudioDecodeResult decodeToMono(const QString& path, int sampleRate) {
    AudioDecodeResult result;
    result.audio.sampleRate = sampleRate;

    if (!QFileInfo::exists(path)) {
        result.error = QStringLiteral("Audio file not found: %1").arg(path);
        return result;
    }

    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty()) {
        result.error = QStringLiteral(
            "ffmpeg was not found. Put ffmpeg.exe in the app's tools/ folder, set the "
            "DVS_FFMPEG environment variable, or choose it in Settings.");
        return result;
    }

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(ffmpeg, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-i"), path,
        QStringLiteral("-vn"),
        QStringLiteral("-ac"), QStringLiteral("1"),
        QStringLiteral("-ar"), QString::number(sampleRate),
        QStringLiteral("-f"), QStringLiteral("f32le"),
        QStringLiteral("-"),
    });

    if (!proc.waitForStarted(10000)) {
        result.error = QStringLiteral("Could not start ffmpeg (%1): %2")
                           .arg(ffmpeg, proc.errorString());
        return result;
    }

    QByteArray pcm;
    while (proc.state() != QProcess::NotRunning) {
        proc.waitForReadyRead(200);
        pcm.append(proc.readAllStandardOutput());
    }
    pcm.append(proc.readAllStandardOutput());

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        const QString stderrText = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
        result.error = QStringLiteral("ffmpeg failed to decode %1%2")
                           .arg(QFileInfo(path).fileName(),
                                stderrText.isEmpty() ? QString()
                                                     : QStringLiteral(": ") + stderrText);
        return result;
    }

    const int frameCount = static_cast<int>(pcm.size() / sizeof(float));
    if (frameCount == 0) {
        result.error = QStringLiteral("%1 contains no decodable audio").arg(QFileInfo(path).fileName());
        return result;
    }

    result.audio.samples.resize(static_cast<size_t>(frameCount));
    std::memcpy(result.audio.samples.data(), pcm.constData(), static_cast<size_t>(frameCount) * sizeof(float));
    return result;
}

QList<PeakBucket> computePeaks(const AudioBuffer& audio, int buckets) {
    QList<PeakBucket> peaks;
    if (audio.isEmpty() || buckets <= 0) return peaks;

    peaks.reserve(buckets);
    const size_t total = audio.samples.size();
    for (int i = 0; i < buckets; ++i) {
        const size_t begin = total * static_cast<size_t>(i) / static_cast<size_t>(buckets);
        size_t end = total * static_cast<size_t>(i + 1) / static_cast<size_t>(buckets);
        if (end <= begin) end = std::min(begin + 1, total);

        PeakBucket b;
        b.min = 0.0f;
        b.max = 0.0f;
        if (begin < total) {
            const auto first = audio.samples.begin() + static_cast<ptrdiff_t>(begin);
            const auto last = audio.samples.begin() + static_cast<ptrdiff_t>(end);
            const auto mm = std::minmax_element(first, last);
            b.min = *mm.first;
            b.max = *mm.second;
        }
        peaks.append(b);
    }
    return peaks;
}

} // namespace dvs
