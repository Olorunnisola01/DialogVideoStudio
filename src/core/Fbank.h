#pragma once

#include <cstddef>
#include <vector>

namespace dvs {

struct FbankOptions {
    int sampleRate = 16000;
    double frameLengthMs = 25.0;
    double frameShiftMs = 10.0;
    int numMelBins = 80;
    double lowFreq = 20.0;
    double highFreq = 0.0;   // <= 0 means Nyquist + highFreq
    double preemphasis = 0.97;
    bool removeDcOffset = true;
    // Per-utterance mean subtraction over the time axis, which is what the
    // WeSpeaker / 3D-Speaker exports expect before the encoder.
    bool subtractMean = true;
};

// Kaldi-compatible log mel filterbank (povey window, power spectrum, snip
// edges). Returns `frames` rows of `numMelBins` values, row-major.
struct FbankResult {
    std::vector<float> data;
    int frames = 0;
    int dim = 0;

    const float* row(int i) const { return data.data() + static_cast<size_t>(i) * dim; }
};

class FbankComputer {
public:
    explicit FbankComputer(const FbankOptions& opts = {});

    // `samples` is mono float in [-1, 1]. It is scaled by 32768 internally to
    // match Kaldi's int16-domain conventions.
    FbankResult compute(const float* samples, size_t count) const;

    int frameLength() const { return frameLength_; }
    int frameShift() const { return frameShift_; }

private:
    FbankOptions opts_;
    int frameLength_ = 400;
    int frameShift_ = 160;
    int fftSize_ = 512;
    std::vector<float> window_;
    // One (offset, weights) triangular filter per mel bin.
    std::vector<int> melOffset_;
    std::vector<std::vector<float>> melWeights_;
};

} // namespace dvs
