// Headless driver for the whole pipeline. Lets the SRT/diarize/render/export
// path be exercised and verified from a terminal, without the UI.
//
//   dvs_cli --srt in.srt --audio in.mp3 --speakers 2 --dump-segments
//   dvs_cli --srt in.srt --audio in.mp3 --image a.png --frame 5000 --png out.png
//   dvs_cli --srt in.srt --audio in.mp3 --image a.png --image b.png --out out.mp4

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTextStream>

#include "core/AudioDecoder.h"
#include "core/Diarizer.h"
#include "core/Paths.h"
#include "core/Project.h"
#include "core/Segmenter.h"
#include "core/SecretStore.h"
#include "core/SrtParser.h"
#include "core/TranslationService.h"
#include "core/Translator.h"
#include "render/FrameRenderer.h"
#include "render/VideoExporter.h"
#include "render/VideoStitcher.h"

using namespace dvs;

namespace {

QTextStream& out() {
    static QTextStream s(stdout);
    return s;
}

int fail(const QString& message) {
    QTextStream(stderr) << "error: " << message << "\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    // QGuiApplication (not QCoreApplication) because the renderer needs the
    // font database. No window is ever shown; pass `-platform offscreen`
    // explicitly when running somewhere without a desktop session.
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DialogVideoStudio"));
    QCoreApplication::setApplicationName(QStringLiteral("DialogVideoStudio"));

    QCommandLineParser parser;
    parser.setApplicationDescription("DialogVideoStudio headless pipeline driver");
    parser.addHelpOption();
    const QCommandLineOption srtOpt({"s", "srt"}, "Subtitle file (.srt)", "path");
    const QCommandLineOption audioOpt({"a", "audio"}, "Audio file (.mp3/.wav/...)", "path");
    const QCommandLineOption imageOpt({"i", "image"}, "Scene image (repeatable)", "path");
    const QCommandLineOption speakersOpt("speakers", "Speaker count (0 = auto)", "n", "2");
    const QCommandLineOption dumpOpt("dump-segments", "Print the segment table");
    const QCommandLineOption noDiarizeOpt("no-diarize", "Skip speaker assignment");
    const QCommandLineOption frameOpt("frame", "Timestamp for --png, in ms", "ms", "5000");
    const QCommandLineOption pngOpt("png", "Write a single rendered frame here", "path");
    const QCommandLineOption outOpt({"o", "out"}, "Write the finished MP4 here", "path");
    const QCommandLineOption projOpt("project", "Load/save a .dvsproj here", "path");
    const QCommandLineOption sizeOpt("size", "Canvas size, e.g. 1920x1080", "WxH", "1920x1080");
    const QCommandLineOption hlOpt("highlight",
                                   "Colour one word everywhere, e.g. fuehlst=#1E6FE8 or "
                                   "wohl=#E8341E:bold (repeatable)",
                                   "word=colour");
    const QCommandLineOption pairOpt("pair", "Pair each German line with the English that follows");
    const QCommandLineOption unpairOpt("unpair", "Undo pairing");
    const QCommandLineOption logoOpt("logo", "Logo image, placed bottom-left", "path");
    const QCommandLineOption subOpt("subscribe", "Subscribe image, placed bottom-right", "path");
    const QCommandLineOption titleOpt("title", "Title banner text, across the top", "text");
    const QCommandLineOption translateOpt(
        "translate", "Machine-translate: google|mymemory|openai|anthropic|groq|openrouter",
        "provider");
    const QCommandLineOption sourceLangOpt("source-lang", "Source language code", "code", "de");
    const QCommandLineOption targetLangOpt("target-lang", "Target language code", "code", "en");
    const QCommandLineOption modelOpt("model", "Model for LLM providers", "id");
    const QCommandLineOption apiKeyOpt("api-key",
                                       "API key (else the one saved in the app is used)", "key");
    const QCommandLineOption redoOpt("retranslate", "Replace translations that already exist");
    const QCommandLineOption transitionOpt("transition",
                                           "Caption fade in ms, centred on the timestamp (0 = cut)",
                                           "ms");
    const QCommandLineOption riseOpt("rise", "Caption slide-in, fraction of the frame", "amount");
    const QCommandLineOption mergeOpt("merge",
                                      "Merge consecutive lines by the same speaker into one caption");
    const QCommandLineOption unmergeOpt("unmerge", "Undo merging");
    const QCommandLineOption mergeGapOpt("merge-gap", "Largest gap to merge across, ms", "ms",
                                         "900");
    const QCommandLineOption mergeCharsOpt("merge-chars", "Longest merged caption, characters",
                                           "n", "180");
    parser.addOptions({srtOpt, audioOpt, imageOpt, speakersOpt, dumpOpt, noDiarizeOpt,
                       frameOpt, pngOpt, outOpt, projOpt, sizeOpt, hlOpt,
                       pairOpt, unpairOpt, logoOpt, subOpt, titleOpt,
                       translateOpt, sourceLangOpt, targetLangOpt, modelOpt, apiKeyOpt, redoOpt,
                       transitionOpt, riseOpt,
                       mergeOpt, unmergeOpt, mergeGapOpt, mergeCharsOpt});
    const QCommandLineOption stitchOpt("stitch", "Join parts into one video here", "out.mp4");
    const QCommandLineOption addOpt("add", "A .dvsproj or video to join (repeatable, in order)",
                                    "path");
    parser.addOptions({stitchOpt, addOpt});
    parser.process(app);

    // --- stitching ----------------------------------------------------------
    // Standalone mode: joins finished parts, so none of the editing options
    // below apply.
    if (parser.isSet(stitchOpt)) {
        QList<StitchItem> items;
        for (const QString& path : parser.values(addOpt)) items.append({path, {}});

        StitchSettings ss;
        ss.outputPath = parser.value(stitchOpt);
        const QStringList wh = parser.value(sizeOpt).split(QLatin1Char('x'));
        if (wh.size() == 2) ss.canvas = QSize(wh.at(0).toInt(), wh.at(1).toInt());

        VideoStitcher stitcher;
        QObject::connect(&stitcher, &VideoStitcher::status, [&](const QString& s) {
            out() << "  " << s << "\n";
            out().flush();
        });
        const StitchReport report = stitcher.run(items, ss);
        if (!report.ok()) return fail(report.error);
        out() << "joined " << report.parts << " part(s) into " << ss.outputPath
              << (report.streamCopied ? " (stream copy)" : " (re-encoded)") << "\n";
        out().flush();
        return 0;
    }

    Project project = Project::makeDefault();
    if (parser.isSet(projOpt) && QFileInfo::exists(parser.value(projOpt))) {
        QString err;
        project = Project::load(parser.value(projOpt), &err);
        if (!err.isEmpty()) return fail(err);
    }

    {
        const QStringList wh = parser.value(sizeOpt).split(QLatin1Char('x'));
        if (wh.size() == 2) {
            const QSize requested(wh.at(0).toInt(), wh.at(1).toInt());
            const bool flipped = (requested.height() > requested.width()) !=
                                 (project.canvas.height() > project.canvas.width());
            project.canvas = requested;
            if (flipped) project.applyDefaultLayout();
        }
    }

    if (parser.isSet(transitionOpt)) project.transitionMs = parser.value(transitionOpt).toInt();
    if (parser.isSet(riseOpt)) project.transitionRise = parser.value(riseOpt).toDouble();

    out() << "ffmpeg: " << (ffmpegPath().isEmpty() ? QStringLiteral("<not found>") : ffmpegPath()) << "\n";
    out() << "speaker model: "
          << (speakerModelPath().isEmpty() ? QStringLiteral("<not installed>") : speakerModelPath())
          << "\n";

    // --- subtitles ---------------------------------------------------------
    QList<Word> words;
    if (parser.isSet(srtOpt)) {
        project.srtPath = parser.value(srtOpt);
        const SrtParseResult srt = parseSrtFile(project.srtPath);
        if (!srt.ok()) return fail(srt.error);
        words = wordsFromCues(srt.cues);
        project.segments = segmentsFromWords(words);
        out() << "parsed " << srt.cues.size() << " cues -> " << project.segments.size()
              << " segments\n";
    }

    // --- audio -------------------------------------------------------------
    AudioBuffer audio;
    if (parser.isSet(audioOpt)) {
        project.audioPath = parser.value(audioOpt);
        const AudioDecodeResult decoded = decodeToMono(project.audioPath);
        if (!decoded.ok()) return fail(decoded.error);
        audio = decoded.audio;
        project.durationMs = audio.durationMs();
        out() << "decoded " << audio.samples.size() << " samples @" << audio.sampleRate
              << " Hz (" << audio.durationMs() / 1000.0 << " s)\n";
    }

    // --- diarization -------------------------------------------------------
    if (!parser.isSet(noDiarizeOpt) && !project.segments.isEmpty()) {
        DiarizeOptions opts;
        opts.speakerCount = parser.value(speakersOpt).toInt();
        int lastPct = -10;
        const DiarizeReport report =
            diarize(project.segments, words, audio, opts, [&](int pct) {
                if (pct >= lastPct + 10) {
                    lastPct = pct;
                    out() << "  diarizing... " << pct << "%\n";
                    out().flush();
                }
            });
        if (!report.ok()) return fail(report.error);
        out() << "diarization: " << report.speakerCount << " speakers, backend="
              << report.backend << ", windows=" << report.embeddingWindows
              << ", splits=" << report.splitCount
              << ", needs review=" << report.reviewCount << "\n";
        if (!report.warning.isEmpty()) out() << "warning: " << report.warning << "\n";

        while (project.speakers.size() < report.speakerCount) {
            Speaker extra = project.speakers.last();
            extra.name = QStringLiteral("Speaker %1").arg(project.speakers.size() + 1);
            project.speakers.append(extra);
        }
    }

    // --- merging same-speaker runs ------------------------------------------
    if (parser.isSet(unmergeOpt)) {
        out() << "unmerged " << unmergeSegments(project.segments) << " caption(s)\n";
    }
    if (parser.isSet(mergeOpt)) {
        MergeOptions mo;
        mo.maxGapMs = parser.value(mergeGapOpt).toLongLong();
        mo.maxCharacters = parser.value(mergeCharsOpt).toInt();
        const MergeReport mr = mergeSameSpeakerRuns(project.segments, mo);
        out() << "merged " << mr.merged << " caption(s) absorbing " << mr.absorbed
              << " line(s): " << mr.before << " -> " << mr.after << " segments\n";
    }

    // --- translation pairing -----------------------------------------------
    if (parser.isSet(unpairOpt)) {
        out() << "unpaired " << unpairTranslations(project.segments) << " segment(s)\n";
    }
    if (parser.isSet(pairOpt)) {
        const PairingReport pairing = pairTranslations(project.segments);
        out() << "paired " << pairing.paired << " of " << pairing.total << " lines ("
              << pairing.unpairedGerman << " German line(s) had no translation after them) -> "
              << project.segments.size() << " segments\n";
    }

    // --- machine translation ------------------------------------------------
    if (parser.isSet(translateOpt)) {
        TranslationSettings ts;
        if (!providerFromId(parser.value(translateOpt), &ts.provider)) {
            return fail(QStringLiteral("Unknown translation provider: %1")
                            .arg(parser.value(translateOpt)));
        }
        const ProviderInfo info = providerInfo(ts.provider);
        ts.sourceLang = parser.value(sourceLangOpt);
        ts.targetLang = parser.value(targetLangOpt);
        ts.model = parser.isSet(modelOpt) ? parser.value(modelOpt) : info.defaultModel;
        ts.overwriteExisting = parser.isSet(redoOpt);
        if (info.needsKey) {
            ts.apiKey = parser.isSet(apiKeyOpt) ? parser.value(apiKeyOpt)
                                                : secrets::apiKey(info.id);
        }

        out() << "translating with " << info.displayName;
        if (info.needsKey) out() << " (" << ts.model << ")";
        out() << " " << ts.sourceLang << " -> " << ts.targetLang << "\n";
        out().flush();

        int lastPct = -20;
        const TranslationReport report =
            translateSegments(project.segments, ts, nullptr, [&](int pct) {
                if (pct >= lastPct + 20) {
                    lastPct = pct;
                    out() << "  translating... " << pct << "%\n";
                    out().flush();
                }
            });
        if (!report.ok()) return fail(report.error);
        out() << "translated " << report.translated << ", skipped " << report.skipped
              << ", failed " << report.failed << "\n";
        if (!report.warning.isEmpty()) out() << "warning: " << report.warning << "\n";
    }

    project.normalise();

    // --- overlays ----------------------------------------------------------
    if (parser.isSet(logoOpt)) {
        project.overlays.append(Overlay::makeLogo(parser.value(logoOpt)));
    }
    if (parser.isSet(subOpt)) {
        project.overlays.append(Overlay::makeSubscribe(parser.value(subOpt)));
    }
    if (parser.isSet(titleOpt)) {
        project.overlays.append(Overlay::makeTitleBanner(parser.value(titleOpt)));
    }

    // --- scenes ------------------------------------------------------------
    const QStringList images = parser.values(imageOpt);
    if (!images.isEmpty()) {
        project.scenes.clear();
        for (const QString& path : images) {
            Scene s;
            s.imagePath = path;
            project.scenes.append(s);
        }
        project.autoLayoutScenes();
    }

    // --- word colours ------------------------------------------------------
    for (const QString& spec : parser.values(hlOpt)) {
        const int eq = spec.indexOf(QLatin1Char('='));
        if (eq <= 0) return fail(QStringLiteral("Bad --highlight value: %1").arg(spec));
        QString value = spec.mid(eq + 1);
        bool bold = false;
        if (value.endsWith(QStringLiteral(":bold"), Qt::CaseInsensitive)) {
            bold = true;
            value.chop(5);
        }
        const QColor color(value);
        if (!color.isValid()) {
            return fail(QStringLiteral("Bad colour in --highlight: %1").arg(value));
        }
        project.setHighlight(spec.left(eq), color, bold);
        out() << "highlight: " << normaliseWord(spec.left(eq)) << " -> "
              << color.name() << (bold ? " (bold)" : "") << "\n";
    }

    // --- report ------------------------------------------------------------
    if (parser.isSet(dumpOpt)) {
        out() << "\n  #  start        end          spk  conf  text\n";
        for (int i = 0; i < project.segments.size(); ++i) {
            const Segment& s = project.segments.at(i);
            out() << QStringLiteral("%1  %2  %3  %4   %5  %6%7\n")
                         .arg(i + 1, 3)
                         .arg(formatTimestamp(s.startMs), formatTimestamp(s.endMs))
                         .arg(s.speakerId, 3)
                         .arg(QString::number(s.confidence, 'f', 2))
                         .arg(s.needsReview ? QStringLiteral("[review] ") : QString())
                         .arg(s.text);
            if (!s.translation.isEmpty()) {
                out() << QStringLiteral("                                          -> %1\n")
                             .arg(s.translation);
            }
        }
        out() << "\n";
    }

    // --- outputs -----------------------------------------------------------
    if (parser.isSet(pngOpt)) {
        FrameRenderer renderer;
        const QImage frame = renderer.renderFrame(project, parser.value(frameOpt).toLongLong());
        if (!frame.save(parser.value(pngOpt))) {
            return fail(QStringLiteral("Could not write %1").arg(parser.value(pngOpt)));
        }
        out() << "wrote " << parser.value(pngOpt) << " (" << frame.width() << "x"
              << frame.height() << ")\n";
    }

    if (parser.isSet(outOpt)) {
        project.exportSettings.outputPath = parser.value(outOpt);
        VideoExporter exporter;
        int lastPct = -10;
        QObject::connect(&exporter, &VideoExporter::progress, [&](int pct) {
            if (pct >= lastPct + 10) {
                lastPct = pct;
                out() << "  encoding... " << pct << "%\n";
                out().flush();
            }
        });
        QObject::connect(&exporter, &VideoExporter::log, [&](const QString& line) {
            QTextStream(stderr) << "ffmpeg: " << line << "\n";
        });
        const QString err = exporter.run(project);
        if (!err.isEmpty()) return fail(err);
        out() << "wrote " << project.exportSettings.outputPath << "\n";
    }

    if (parser.isSet(projOpt)) {
        const QString err = project.save(parser.value(projOpt));
        if (!err.isEmpty()) return fail(err);
        out() << "saved " << parser.value(projOpt) << "\n";
    }

    out().flush();
    return 0;
}
