#include "Diarizer.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>

#include <onnxruntime_cxx_api.h>

#include "Fbank.h"
#include "GpuDevice.h"
#include "Paths.h"

namespace dvs {

namespace {

// --- language cross-check -------------------------------------------------

const QSet<QString>& germanWords() {
    static const QSet<QString> s = {
        QStringLiteral("ich"), QStringLiteral("du"), QStringLiteral("er"),
        QStringLiteral("sie"), QStringLiteral("wir"), QStringLiteral("ihr"),
        QStringLiteral("der"), QStringLiteral("die"), QStringLiteral("das"),
        QStringLiteral("den"), QStringLiteral("dem"), QStringLiteral("ein"),
        QStringLiteral("eine"), QStringLiteral("einen"), QStringLiteral("einem"),
        QStringLiteral("nicht"), QStringLiteral("und"), QStringLiteral("oder"),
        QStringLiteral("aber"), QStringLiteral("zu"), QStringLiteral("zum"),
        QStringLiteral("zur"), QStringLiteral("um"), QStringLiteral("wenn"),
        QStringLiteral("weil"), QStringLiteral("mit"), QStringLiteral("von"),
        QStringLiteral("für"), QStringLiteral("auf"), QStringLiteral("aus"),
        QStringLiteral("ist"), QStringLiteral("sind"), QStringLiteral("war"),
        QStringLiteral("haben"), QStringLiteral("hat"), QStringLiteral("wird"),
        QStringLiteral("werden"), QStringLiteral("kann"), QStringLiteral("muss"),
        QStringLiteral("auch"), QStringLiteral("sehr"), QStringLiteral("schon"),
        QStringLiteral("noch"), QStringLiteral("immer"), QStringLiteral("oft"),
        QStringLiteral("jeden"), QStringLiteral("jede"), QStringLiteral("mein"),
        QStringLiteral("meine"), QStringLiteral("meinen"), QStringLiteral("dein"),
        QStringLiteral("unser"), QStringLiteral("bitte"), QStringLiteral("danke"),
        QStringLiteral("ja"), QStringLiteral("nein"), QStringLiteral("gut"),
        QStringLiteral("hier"), QStringLiteral("dort"), QStringLiteral("nach"),
        QStringLiteral("bei"), QStringLiteral("im"), QStringLiteral("am"),
        QStringLiteral("man"), QStringLiteral("es"), QStringLiteral("dass"),
        QStringLiteral("wie"), QStringLiteral("was"), QStringLiteral("wo"),
    };
    return s;
}

const QSet<QString>& englishWords() {
    static const QSet<QString> s = {
        QStringLiteral("i"), QStringLiteral("you"), QStringLiteral("he"),
        QStringLiteral("she"), QStringLiteral("we"), QStringLiteral("they"),
        QStringLiteral("the"), QStringLiteral("a"), QStringLiteral("an"),
        QStringLiteral("is"), QStringLiteral("are"), QStringLiteral("was"),
        QStringLiteral("were"), QStringLiteral("do"), QStringLiteral("does"),
        QStringLiteral("did"), QStringLiteral("have"), QStringLiteral("has"),
        QStringLiteral("not"), QStringLiteral("and"), QStringLiteral("or"),
        QStringLiteral("but"), QStringLiteral("to"), QStringLiteral("of"),
        QStringLiteral("for"), QStringLiteral("with"), QStringLiteral("from"),
        QStringLiteral("on"), QStringLiteral("in"), QStringLiteral("at"),
        QStringLiteral("my"), QStringLiteral("your"), QStringLiteral("our"),
        QStringLiteral("this"), QStringLiteral("that"), QStringLiteral("these"),
        QStringLiteral("when"), QStringLiteral("where"), QStringLiteral("what"),
        QStringLiteral("please"), QStringLiteral("thank"), QStringLiteral("yes"),
        QStringLiteral("no"), QStringLiteral("very"), QStringLiteral("often"),
        QStringLiteral("every"), QStringLiteral("use"), QStringLiteral("go"),
        QStringLiteral("it"), QStringLiteral("its"), QStringLiteral("new"),
        QStringLiteral("good"), QStringLiteral("here"), QStringLiteral("there"),
        QStringLiteral("learn"), QStringLiteral("work"), QStringLiteral("open"),
    };
    return s;
}

} // namespace

double germanEnglishScore(const QString& text) {
    const QStringList tokens = text.toLower().split(
        QRegularExpression(QStringLiteral(R"([^\p{L}]+)")), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) return 0.0;

    double de = 0.0;
    double en = 0.0;
    for (const QString& t : tokens) {
        if (germanWords().contains(t)) de += 1.0;
        if (englishWords().contains(t)) en += 1.0;
        // Umlauts and eszett are decisive on their own.
        if (t.contains(QChar(0x00E4)) || t.contains(QChar(0x00F6)) ||
            t.contains(QChar(0x00FC)) || t.contains(QChar(0x00DF))) {
            de += 2.0;
        }
        // German nouns are capitalised mid-sentence; approximated by common
        // German-only suffixes to avoid fighting with sentence-initial caps.
        if (t.endsWith(QStringLiteral("ung")) || t.endsWith(QStringLiteral("keit")) ||
            t.endsWith(QStringLiteral("heit")) || t.endsWith(QStringLiteral("chen"))) {
            de += 0.5;
        }
        if (t.endsWith(QStringLiteral("ing")) || t.endsWith(QStringLiteral("tion"))) {
            en += 0.5;
        }
    }

    const double total = de + en;
    if (total < 1.0) return 0.0;
    return (de - en) / total;
}

namespace {

// --- speaker embedding ----------------------------------------------------

class SpeakerEmbedder {
public:
    SpeakerEmbedder(const QString& modelPath, QString* backend, QString* error)
        : modelPath_(modelPath) {
        // ORT logs every failed kernel; the DML EP can produce hundreds of
        // those before we decide to fall back, so keep it quiet.
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_FATAL, "dvs-diarizer");
        load(true, backend, error);
    }

    bool valid() const { return session_ != nullptr; }
    bool usingGpu() const { return usingGpu_; }

    // Some GPUs/drivers reject individual kernels in this model (DirectML
    // returns E_INVALIDARG for AveragePool on certain adapters). Reloading on
    // the CPU provider keeps voice-based splitting working instead of silently
    // degrading to the text heuristic.
    bool fallbackToCpu(QString* backend) {
        if (!usingGpu_) return false;
        session_.reset();
        QString error;
        load(false, backend, &error);
        return session_ != nullptr;
    }

    // Returns an L2-normalised embedding, or an empty vector on failure.
    std::vector<float> embed(const FbankResult& feats) {
        if (!session_ || feats.frames == 0) return {};

        std::vector<int64_t> shape;
        std::vector<float> input;
        if (featureMajor_) {
            shape = {1, feats.dim, feats.frames};
            input.resize(static_cast<size_t>(feats.dim) * feats.frames);
            for (int t = 0; t < feats.frames; ++t) {
                for (int d = 0; d < feats.dim; ++d) {
                    input[static_cast<size_t>(d) * feats.frames + t] = feats.row(t)[d];
                }
            }
        } else {
            shape = {1, feats.frames, feats.dim};
            input = feats.data;
        }

        try {
            // The DirectML EP crashes if Run() is re-entered on one session.
            std::lock_guard<std::mutex> lock(runMutex_);

            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value tensor = Ort::Value::CreateTensor<float>(
                memInfo, input.data(), input.size(), shape.data(), shape.size());

            const char* inNames[] = {inputName_.c_str()};
            const char* outNames[] = {outputName_.c_str()};
            auto outputs = session_->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);

            const float* data = outputs.front().GetTensorData<float>();
            const auto outShape = outputs.front().GetTensorTypeAndShapeInfo().GetShape();
            size_t dim = 1;
            for (size_t i = 1; i < outShape.size(); ++i) {
                dim *= static_cast<size_t>(std::max<int64_t>(outShape[i], 1));
            }

            std::vector<float> emb(data, data + dim);
            double norm = 0.0;
            for (float v : emb) norm += static_cast<double>(v) * v;
            norm = std::sqrt(std::max(norm, 1e-12));
            for (float& v : emb) v = static_cast<float>(v / norm);
            return emb;
        } catch (const Ort::Exception&) {
            return {};
        }
    }

private:
    void load(bool tryGpu, QString* backend, QString* error) {
        try {
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(
                static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2)));
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            usingGpu_ = false;
            std::string gpuName;
            if (tryGpu && tryEnableDml(opts, &gpuName)) {
                usingGpu_ = true;
                if (backend) {
                    *backend = QStringLiteral("DirectML (%1)").arg(QString::fromStdString(gpuName));
                }
            } else if (backend) {
                *backend = QStringLiteral("CPU");
            }

            const std::wstring wpath = modelPath_.toStdWString();
            session_ = std::make_unique<Ort::Session>(*env_, wpath.c_str(), opts);

            Ort::AllocatorWithDefaultOptions alloc;
            inputName_ = session_->GetInputNameAllocated(0, alloc).get();
            outputName_ = session_->GetOutputNameAllocated(0, alloc).get();

            // Some exports take [B, 80, T] instead of [B, T, 80].
            const auto shape =
                session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
            featureMajor_ = shape.size() == 3 && shape[1] == 80;
        } catch (const std::exception& e) {
            session_.reset();
            if (error) {
                *error = QStringLiteral("Could not load the speaker model: %1")
                             .arg(QString::fromUtf8(e.what()));
            }
        }
    }

    QString modelPath_;
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::string inputName_;
    std::string outputName_;
    bool featureMajor_ = false;
    bool usingGpu_ = false;
    std::mutex runMutex_;
};

// --- VAD ------------------------------------------------------------------

// Marks 10 ms frames as speech when their RMS is within `relativeDb` of the
// loudest frame. Good enough for studio/TTS material, which is what this app
// consumes; it only needs to keep silence out of the embedding windows.
std::vector<bool> energyVad(const AudioBuffer& audio, double relativeDb, int frameSamples) {
    const size_t frames = audio.samples.size() / static_cast<size_t>(frameSamples);
    std::vector<double> rms(frames, 0.0);
    double peak = 1e-12;

    for (size_t f = 0; f < frames; ++f) {
        double sum = 0.0;
        for (int i = 0; i < frameSamples; ++i) {
            const float v = audio.samples[f * frameSamples + i];
            sum += static_cast<double>(v) * v;
        }
        rms[f] = std::sqrt(sum / frameSamples);
        peak = std::max(peak, rms[f]);
    }

    const double threshold = peak * std::pow(10.0, relativeDb / 20.0);
    std::vector<bool> speech(frames, false);
    for (size_t f = 0; f < frames; ++f) speech[f] = rms[f] > threshold;
    return speech;
}

// --- clustering -----------------------------------------------------------

struct ClusterResult {
    std::vector<int> labels;
    int clusterCount = 0;
};

// Average-linkage agglomerative clustering on cosine distance. Stops at
// `targetK` clusters, or when the closest pair exceeds `maxDistance` if
// targetK <= 0.
ClusterResult agglomerative(const std::vector<std::vector<float>>& embeddings,
                            int targetK, int maxK, double maxDistance) {
    ClusterResult result;
    const int n = static_cast<int>(embeddings.size());
    if (n == 0) return result;

    result.labels.assign(static_cast<size_t>(n), 0);
    if (n == 1) {
        result.clusterCount = 1;
        return result;
    }

    // Pairwise cosine distance (embeddings are already L2-normalised).
    std::vector<std::vector<double>> dist(static_cast<size_t>(n),
                                          std::vector<double>(static_cast<size_t>(n), 0.0));
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            double dot = 0.0;
            const size_t dim = std::min(embeddings[i].size(), embeddings[j].size());
            for (size_t d = 0; d < dim; ++d) {
                dot += static_cast<double>(embeddings[i][d]) * embeddings[j][d];
            }
            const double d1 = 1.0 - dot;
            dist[i][j] = d1;
            dist[j][i] = d1;
        }
    }

    std::vector<std::vector<int>> clusters(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) clusters[static_cast<size_t>(i)] = {i};

    auto averageLinkage = [&](const std::vector<int>& a, const std::vector<int>& b) {
        double sum = 0.0;
        for (int i : a) {
            for (int j : b) sum += dist[i][j];
        }
        return sum / (a.size() * b.size());
    };

    while (static_cast<int>(clusters.size()) > 1) {
        double best = std::numeric_limits<double>::max();
        size_t bi = 0;
        size_t bj = 1;
        for (size_t i = 0; i < clusters.size(); ++i) {
            for (size_t j = i + 1; j < clusters.size(); ++j) {
                const double d = averageLinkage(clusters[i], clusters[j]);
                if (d < best) {
                    best = d;
                    bi = i;
                    bj = j;
                }
            }
        }

        const int current = static_cast<int>(clusters.size());
        if (targetK > 0) {
            if (current <= targetK) break;
        } else {
            if (current <= maxK && best > maxDistance) break;
            if (current <= 1) break;
        }

        clusters[bi].insert(clusters[bi].end(), clusters[bj].begin(), clusters[bj].end());
        clusters.erase(clusters.begin() + static_cast<ptrdiff_t>(bj));
    }

    // Label clusters in order of first appearance so ids are stable.
    std::vector<size_t> order(clusters.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return *std::min_element(clusters[a].begin(), clusters[a].end()) <
               *std::min_element(clusters[b].begin(), clusters[b].end());
    });

    for (size_t rank = 0; rank < order.size(); ++rank) {
        for (int idx : clusters[order[rank]]) {
            result.labels[static_cast<size_t>(idx)] = static_cast<int>(rank);
        }
    }
    result.clusterCount = static_cast<int>(clusters.size());
    return result;
}

struct Vote {
    int label = -1;
    float confidence = 0.0f;
};

Vote majority(const std::vector<int>& labels, int clusterCount) {
    Vote v;
    if (labels.empty() || clusterCount <= 0) return v;

    std::vector<int> counts(static_cast<size_t>(clusterCount), 0);
    for (int l : labels) {
        if (l >= 0 && l < clusterCount) counts[static_cast<size_t>(l)]++;
    }
    const auto maxIt = std::max_element(counts.begin(), counts.end());
    if (maxIt == counts.end() || *maxIt == 0) return v;

    const int top = *maxIt;
    int second = 0;
    for (size_t i = 0; i < counts.size(); ++i) {
        if (static_cast<ptrdiff_t>(i) == std::distance(counts.begin(), maxIt)) continue;
        second = std::max(second, counts[i]);
    }
    const int total = std::accumulate(counts.begin(), counts.end(), 0);

    v.label = static_cast<int>(std::distance(counts.begin(), maxIt));
    v.confidence = total > 0 ? static_cast<float>(top - second) / total : 0.0f;
    return v;
}

} // namespace

DiarizeReport diarize(QList<Segment>& segments,
                      const QList<Word>& words,
                      const AudioBuffer& audio,
                      const DiarizeOptions& opts,
                      const std::function<void(int)>& progress) {
    DiarizeReport report;
    auto report_ = [&](int pct) { if (progress) progress(pct); };

    if (segments.isEmpty()) {
        report.error = QStringLiteral("There are no subtitle segments to assign.");
        return report;
    }

    // Language guess is computed either way: on its own it is the fallback,
    // and alongside the audio it is the cross-check.
    std::vector<double> langScore(static_cast<size_t>(segments.size()), 0.0);
    for (int i = 0; i < segments.size(); ++i) {
        langScore[static_cast<size_t>(i)] = germanEnglishScore(segments.at(i).text);
    }

    QString modelPath = opts.modelPath.isEmpty() ? speakerModelPath() : opts.modelPath;
    const bool haveModel = !modelPath.isEmpty() && QFileInfo::exists(modelPath);

    std::vector<int> windowLabels;
    std::vector<qint64> windowCenterMs;
    int clusterCount = 0;

    if (haveModel && !audio.isEmpty()) {
        QString backend;
        QString loadError;
        SpeakerEmbedder embedder(modelPath, &backend, &loadError);

        if (!embedder.valid()) {
            report.warning = loadError;
        } else {
            report.backend = backend;
            report_(5);

            const int frameSamples = audio.sampleRate / 100; // 10 ms
            const std::vector<bool> speech = energyVad(audio, opts.vadRelativeDb, frameSamples);

            const int windowSamples = static_cast<int>(opts.windowSec * audio.sampleRate);
            const int hopSamples = static_cast<int>(opts.hopSec * audio.sampleRate);
            const FbankComputer fbank;

            // Window offsets that are mostly speech.
            std::vector<size_t> starts;
            const size_t total = audio.samples.size();
            for (size_t start = 0; start + windowSamples <= total; start += hopSamples) {
                const size_t f0 = start / frameSamples;
                const size_t f1 = std::min(speech.size(), (start + windowSamples) / frameSamples);
                if (f1 <= f0) continue;
                int speechFrames = 0;
                for (size_t f = f0; f < f1; ++f) {
                    if (speech[f]) speechFrames++;
                }
                if (speechFrames * 2 < static_cast<int>(f1 - f0)) continue;
                starts.push_back(start);
            }

            std::vector<std::vector<float>> embeddings;
            bool inferenceWorks = false;

            if (!starts.empty()) {
                // Probe once before committing to the whole file: the DirectML
                // EP fails per-kernel on some adapters, and retrying on the CPU
                // is much better than dropping to the text-only heuristic.
                const FbankResult probeFeats =
                    fbank.compute(audio.samples.data() + starts.front(),
                                  static_cast<size_t>(windowSamples));
                inferenceWorks = !embedder.embed(probeFeats).empty();
                if (!inferenceWorks && embedder.usingGpu()) {
                    if (embedder.fallbackToCpu(&report.backend)) {
                        inferenceWorks = !embedder.embed(probeFeats).empty();
                    }
                }
            }

            if (inferenceWorks) {
                for (size_t i = 0; i < starts.size(); ++i) {
                    const size_t start = starts.at(i);
                    const FbankResult feats =
                        fbank.compute(audio.samples.data() + start,
                                      static_cast<size_t>(windowSamples));
                    std::vector<float> emb = embedder.embed(feats);
                    if (emb.empty()) continue;

                    embeddings.push_back(std::move(emb));
                    windowCenterMs.push_back(static_cast<qint64>(
                        (start + windowSamples / 2) * 1000 / audio.sampleRate));

                    report_(5 + static_cast<int>(70.0 * i / starts.size()));
                }
            }

            report.embeddingWindows = static_cast<int>(embeddings.size());
            if (embeddings.size() >= 2) {
                const ClusterResult cr = agglomerative(
                    embeddings, opts.speakerCount, opts.maxSpeakers, 0.45);
                windowLabels = cr.labels;
                clusterCount = cr.clusterCount;
            } else if (!inferenceWorks) {
                report.warning = QStringLiteral(
                    "The speaker model could not run on this machine, so speakers were "
                    "assigned from the subtitle text instead - check the table.");
            } else {
                report.warning = QStringLiteral(
                    "Not enough speech was detected to compare voices; speakers were "
                    "assigned from the subtitle text instead.");
            }
        }
    } else if (!haveModel) {
        report.warning = QStringLiteral(
            "The speaker-embedding model is not installed (run scripts/fetch_deps.ps1). "
            "Speakers were assigned from the subtitle text instead - check the table.");
    }

    report_(80);

    // --- assign ------------------------------------------------------------
    const bool haveAudioLabels = clusterCount > 0 && !windowLabels.empty();

    // cluster index -> speakerId; identity unless the language check says the
    // clusters should be swapped so that German lands on speaker 0.
    std::vector<int> remap(static_cast<size_t>(std::max(clusterCount, 1)));
    std::iota(remap.begin(), remap.end(), 0);

    if (haveAudioLabels) {
        // Per segment: majority vote over the windows whose centre falls in it.
        std::vector<Vote> votes(static_cast<size_t>(segments.size()));
        for (int i = 0; i < segments.size(); ++i) {
            std::vector<int> inSegment;
            for (size_t w = 0; w < windowCenterMs.size(); ++w) {
                if (segments.at(i).containsTime(windowCenterMs[w])) {
                    inSegment.push_back(windowLabels[w]);
                }
            }
            if (inSegment.empty()) {
                // Short segment between windows: take the nearest window.
                qint64 bestDelta = std::numeric_limits<qint64>::max();
                int bestLabel = -1;
                const qint64 mid = (segments.at(i).startMs + segments.at(i).endMs) / 2;
                for (size_t w = 0; w < windowCenterMs.size(); ++w) {
                    const qint64 delta = std::abs(windowCenterMs[w] - mid);
                    if (delta < bestDelta) {
                        bestDelta = delta;
                        bestLabel = windowLabels[w];
                    }
                }
                votes[static_cast<size_t>(i)] = Vote{bestLabel, 0.0f};
            } else {
                votes[static_cast<size_t>(i)] = majority(inSegment, clusterCount);
            }
        }

        // Order clusters so that the most German-sounding one becomes speaker 0.
        // Without a usable language signal, first appearance decides (which the
        // clusterer already guarantees).
        std::vector<double> clusterLang(static_cast<size_t>(clusterCount), 0.0);
        double langStrength = 0.0;
        for (int i = 0; i < segments.size(); ++i) {
            const int l = votes[static_cast<size_t>(i)].label;
            if (l < 0 || l >= clusterCount) continue;
            clusterLang[static_cast<size_t>(l)] += langScore[static_cast<size_t>(i)];
            langStrength += std::abs(langScore[static_cast<size_t>(i)]);
        }

        const bool languageUsable =
            opts.useLanguageCrossCheck && clusterCount == 2 &&
            langStrength >= 0.5 * segments.size() &&
            clusterLang[0] * clusterLang[1] < 0.0; // the two clusters disagree on language
        if (languageUsable && clusterLang[0] < clusterLang[1]) {
            remap[0] = 1;
            remap[1] = 0;
        }

        for (int i = 0; i < segments.size(); ++i) {
            Segment& s = segments[i];
            const Vote& v = votes[static_cast<size_t>(i)];
            s.speakerId = (v.label >= 0 && v.label < clusterCount)
                              ? remap[static_cast<size_t>(v.label)]
                              : 0;
            s.confidence = v.confidence;
            s.needsReview = false;
            s.reviewReason.clear();

            if (v.confidence < opts.minConfidence) {
                s.needsReview = true;
                s.reviewReason = QStringLiteral("Voices in this line are hard to tell apart.");
            }
            if (languageUsable) {
                const double score = langScore[static_cast<size_t>(i)];
                if (std::abs(score) >= 0.5) {
                    const int langSpeaker = score > 0 ? 0 : 1;
                    if (langSpeaker != s.speakerId) {
                        s.needsReview = true;
                        s.reviewReason = QStringLiteral(
                            "The voice and the language of the text disagree.");
                    }
                }
            }
        }

        report.speakerCount = clusterCount;
    } else {
        // Text-only assignment: German -> 0, English -> 1.
        for (int i = 0; i < segments.size(); ++i) {
            Segment& s = segments[i];
            const double score = langScore[static_cast<size_t>(i)];
            s.speakerId = score >= 0.0 ? 0 : 1;
            s.confidence = static_cast<float>(std::min(1.0, std::abs(score)));
            s.needsReview = std::abs(score) < 0.34;
            s.reviewReason = s.needsReview
                                 ? QStringLiteral("The language of this line is unclear.")
                                 : QString();
        }
        report.speakerCount = 2;
        if (report.backend.isEmpty()) report.backend = QStringLiteral("text only");
    }

    report_(90);

    // --- split segments that contain a speaker change ----------------------
    if (opts.splitOnSpeakerChange && haveAudioLabels && !words.isEmpty()) {
        QList<Segment> rebuilt;
        rebuilt.reserve(segments.size());

        for (const Segment& s : segments) {
            // Words inside this segment, each labelled by its nearest window.
            QList<Word> inner;
            QList<int> labels;
            for (const Word& w : words) {
                const qint64 mid = (w.startMs + w.endMs) / 2;
                if (mid < s.startMs || mid >= s.endMs) continue;
                qint64 bestDelta = std::numeric_limits<qint64>::max();
                int bestLabel = 0; // raw cluster label, remapped below
                for (size_t k = 0; k < windowCenterMs.size(); ++k) {
                    const qint64 delta = std::abs(windowCenterMs[k] - mid);
                    if (delta < bestDelta) {
                        bestDelta = delta;
                        bestLabel = windowLabels[k];
                    }
                }
                inner.append(w);
                labels.append(bestLabel);
            }

            // Only act on a single clean transition; anything noisier is left
            // alone and stays flagged for review instead.
            int changeAt = -1;
            bool clean = inner.size() >= 4;
            for (int i = 1; i < labels.size() && clean; ++i) {
                if (labels.at(i) != labels.at(i - 1)) {
                    if (changeAt >= 0) clean = false;
                    changeAt = i;
                }
            }
            if (!clean || changeAt < 2 || changeAt > inner.size() - 2) {
                rebuilt.append(s);
                continue;
            }

            QStringList head;
            QStringList tail;
            for (int i = 0; i < inner.size(); ++i) {
                (i < changeAt ? head : tail) << inner.at(i).text;
            }

            Segment a = s;
            a.endMs = inner.at(changeAt - 1).endMs;
            a.text = head.join(QLatin1Char(' '));

            Segment b = s;
            b.startMs = inner.at(changeAt).startMs;
            b.text = tail.join(QLatin1Char(' '));

            // Re-derive each half's speaker from its own words.
            auto assignFrom = [&](Segment& seg, int from, int to) {
                std::vector<int> ls;
                for (int i = from; i < to; ++i) ls.push_back(labels.at(i));
                const Vote v = majority(ls, clusterCount);
                if (v.label >= 0 && v.label < clusterCount) {
                    seg.speakerId = remap[static_cast<size_t>(v.label)];
                    seg.confidence = v.confidence;
                }
            };
            assignFrom(a, 0, changeAt);
            assignFrom(b, changeAt, inner.size());

            rebuilt.append(a);
            rebuilt.append(b);
            report.splitCount++;
        }

        segments = rebuilt;
    }

    for (const Segment& s : segments) {
        if (s.needsReview) report.reviewCount++;
    }

    report_(100);
    return report;
}

} // namespace dvs
