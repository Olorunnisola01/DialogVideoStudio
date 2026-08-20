#pragma once

#include <QList>
#include <QString>

#include <vector>

namespace dvs {

struct AudioBuffer {
    std::vector<float> samples; // mono, [-1, 1]
    int sampleRate = 16000;

    bool isEmpty() const { return samples.empty(); }
    qint64 durationMs() const {
        return sampleRate > 0
                   ? static_cast<qint64>(samples.size()) * 1000 / sampleRate
                   : 0;
    }
};

struct AudioDecodeResult {
    AudioBuffer audio;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Decodes any ffmpeg-readable audio file to mono float32 at `sampleRate`.
// Uses the bundled ffmpeg (see Paths::ffmpegPath) piped through stdout.
AudioDecodeResult decodeToMono(const QString& path, int sampleRate = 16000);

struct PeakBucket {
    float min = 0.0f;
    float max = 0.0f;
};

// Downsamples to `buckets` min/max pairs for waveform drawing.
QList<PeakBucket> computePeaks(const AudioBuffer& audio, int buckets);

} // namespace dvs
