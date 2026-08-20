#include "Fbank.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace dvs {

namespace {

constexpr double kPi = 3.14159265358979323846;

double melScale(double freqHz) { return 1127.0 * std::log(1.0 + freqHz / 700.0); }

int nextPowerOfTwo(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// In-place radix-2 FFT. `a` must be a power-of-two length; 512 for the
// standard 25 ms @ 16 kHz frame.
void fftRadix2(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<float> wlen(static_cast<float>(std::cos(ang)),
                                       static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

} // namespace

FbankComputer::FbankComputer(const FbankOptions& opts) : opts_(opts) {
    frameLength_ = static_cast<int>(opts_.sampleRate * opts_.frameLengthMs / 1000.0);
    frameShift_ = static_cast<int>(opts_.sampleRate * opts_.frameShiftMs / 1000.0);
    fftSize_ = nextPowerOfTwo(frameLength_);

    // Povey window: hann^0.85, as used by Kaldi and torchaudio's default.
    window_.resize(static_cast<size_t>(frameLength_));
    for (int i = 0; i < frameLength_; ++i) {
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * kPi * i / (frameLength_ - 1.0));
        window_[static_cast<size_t>(i)] = static_cast<float>(std::pow(hann, 0.85));
    }

    // Triangular mel filters over the power spectrum bins 0..fftSize/2.
    const double nyquist = opts_.sampleRate / 2.0;
    const double highFreq = opts_.highFreq > 0.0 ? opts_.highFreq : nyquist + opts_.highFreq;
    const double melLow = melScale(opts_.lowFreq);
    const double melHigh = melScale(highFreq);
    const double melDelta = (melHigh - melLow) / (opts_.numMelBins + 1);
    const int numFftBins = fftSize_ / 2 + 1;
    const double binWidth = static_cast<double>(opts_.sampleRate) / fftSize_;

    melOffset_.assign(static_cast<size_t>(opts_.numMelBins), 0);
    melWeights_.assign(static_cast<size_t>(opts_.numMelBins), {});

    for (int bin = 0; bin < opts_.numMelBins; ++bin) {
        const double leftMel = melLow + bin * melDelta;
        const double centerMel = leftMel + melDelta;
        const double rightMel = leftMel + 2 * melDelta;

        int firstIndex = -1;
        std::vector<float> weights;
        for (int k = 0; k < numFftBins; ++k) {
            const double mel = melScale(k * binWidth);
            if (mel <= leftMel || mel >= rightMel) {
                if (firstIndex >= 0) break; // filters are contiguous
                continue;
            }
            const double w = mel <= centerMel ? (mel - leftMel) / melDelta
                                              : (rightMel - mel) / melDelta;
            if (firstIndex < 0) firstIndex = k;
            weights.push_back(static_cast<float>(w));
        }
        melOffset_[static_cast<size_t>(bin)] = firstIndex < 0 ? 0 : firstIndex;
        melWeights_[static_cast<size_t>(bin)] = std::move(weights);
    }
}

FbankResult FbankComputer::compute(const float* samples, size_t count) const {
    FbankResult out;
    out.dim = opts_.numMelBins;

    if (count < static_cast<size_t>(frameLength_)) return out;

    // snip_edges=True: only frames fully inside the signal.
    out.frames = static_cast<int>((count - frameLength_) / frameShift_) + 1;
    out.data.assign(static_cast<size_t>(out.frames) * out.dim, 0.0f);

    std::vector<float> frame(static_cast<size_t>(frameLength_));
    std::vector<std::complex<float>> spectrum(static_cast<size_t>(fftSize_));
    std::vector<float> power(static_cast<size_t>(fftSize_ / 2 + 1));
    const float logFloor = std::numeric_limits<float>::epsilon();

    for (int f = 0; f < out.frames; ++f) {
        const size_t start = static_cast<size_t>(f) * frameShift_;

        // Kaldi works in the int16 domain; our input is normalised float.
        for (int i = 0; i < frameLength_; ++i) {
            frame[static_cast<size_t>(i)] = samples[start + i] * 32768.0f;
        }

        if (opts_.removeDcOffset) {
            double sum = 0.0;
            for (float v : frame) sum += v;
            const float mean = static_cast<float>(sum / frameLength_);
            for (float& v : frame) v -= mean;
        }

        if (opts_.preemphasis > 0.0) {
            const float c = static_cast<float>(opts_.preemphasis);
            for (int i = frameLength_ - 1; i > 0; --i) {
                frame[static_cast<size_t>(i)] -= c * frame[static_cast<size_t>(i - 1)];
            }
            frame[0] -= c * frame[0];
        }

        for (int i = 0; i < frameLength_; ++i) {
            frame[static_cast<size_t>(i)] *= window_[static_cast<size_t>(i)];
        }

        std::fill(spectrum.begin(), spectrum.end(), std::complex<float>(0.0f, 0.0f));
        for (int i = 0; i < frameLength_; ++i) {
            spectrum[static_cast<size_t>(i)] = std::complex<float>(frame[static_cast<size_t>(i)], 0.0f);
        }
        fftRadix2(spectrum);

        for (size_t k = 0; k < power.size(); ++k) {
            const float re = spectrum[k].real();
            const float im = spectrum[k].imag();
            power[k] = re * re + im * im;
        }

        float* row = out.data.data() + static_cast<size_t>(f) * out.dim;
        for (int bin = 0; bin < opts_.numMelBins; ++bin) {
            const int offset = melOffset_[static_cast<size_t>(bin)];
            const std::vector<float>& w = melWeights_[static_cast<size_t>(bin)];
            float energy = 0.0f;
            for (size_t j = 0; j < w.size(); ++j) {
                const size_t k = static_cast<size_t>(offset) + j;
                if (k >= power.size()) break;
                energy += w[j] * power[k];
            }
            row[bin] = std::log(std::max(energy, logFloor));
        }
    }

    if (opts_.subtractMean && out.frames > 0) {
        std::vector<double> mean(static_cast<size_t>(out.dim), 0.0);
        for (int f = 0; f < out.frames; ++f) {
            const float* row = out.row(f);
            for (int d = 0; d < out.dim; ++d) mean[static_cast<size_t>(d)] += row[d];
        }
        for (double& m : mean) m /= out.frames;
        for (int f = 0; f < out.frames; ++f) {
            float* row = out.data.data() + static_cast<size_t>(f) * out.dim;
            for (int d = 0; d < out.dim; ++d) {
                row[d] -= static_cast<float>(mean[static_cast<size_t>(d)]);
            }
        }
    }

    return out;
}

} // namespace dvs
