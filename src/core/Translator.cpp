#include "Translator.h"

#include <algorithm>

#include "Diarizer.h"

namespace dvs {

namespace {

// How decisive the language score must be before a line counts as German or
// English. The scorer returns -1..1; 0.34 is the same threshold the diarizer
// treats as "clear enough to act on".
constexpr double kLanguageThreshold = 0.34;

// A block of translations follows its source block closely. Anything further
// apart is a new thought, not a translation.
constexpr qint64 kMaxGapMs = 4000;

bool isGerman(const Segment& s) { return germanEnglishScore(s.text) >= kLanguageThreshold; }
bool isEnglish(const Segment& s) { return germanEnglishScore(s.text) <= -kLanguageThreshold; }

} // namespace

PairingReport pairTranslations(QList<Segment>& segments) {
    PairingReport report;
    report.total = static_cast<int>(segments.size());
    if (segments.size() < 2) return report;

    QList<Segment> result;
    result.reserve(segments.size() * 2);

    int i = 0;
    while (i < segments.size()) {
        // Leave anything already carrying a translation exactly as the user
        // left it.
        if (!segments.at(i).translation.isEmpty() || !isGerman(segments.at(i))) {
            result.append(segments.at(i));
            ++i;
            continue;
        }

        // A run of source lines, then the run of translations that follows it.
        // The recording may read one line and translate it, or read several
        // and then translate them all; both come out as runs here.
        int g0 = i;
        int g1 = i;
        while (g1 < segments.size() && isGerman(segments.at(g1)) &&
               segments.at(g1).translation.isEmpty()) {
            ++g1;
        }
        int e1 = g1;
        while (e1 < segments.size() && isEnglish(segments.at(e1)) &&
               segments.at(e1).translation.isEmpty()) {
            ++e1;
        }

        const int sourceCount = g1 - g0;
        const int translationCount = e1 - g1;
        const qint64 gap = translationCount > 0
                               ? segments.at(g1).startMs - segments.at(g1 - 1).endMs
                               : -1;

        if (translationCount == 0 || gap < 0 || gap > kMaxGapMs) {
            for (int k = g0; k < g1; ++k) {
                report.unpairedGerman++;
                result.append(segments.at(k));
            }
            i = g1;
            continue;
        }

        const int pairs = std::min(sourceCount, translationCount);
        for (int k = 0; k < pairs; ++k) {
            const Segment& source = segments.at(g0 + k);
            const Segment& translation = segments.at(g1 + k);

            Segment paired = source;
            paired.translation = translation.text;
            paired.translationStartMs = translation.startMs;
            if (translation.speakerId != source.speakerId) {
                paired.needsReview = true;
                paired.reviewReason = QStringLiteral(
                    "The line and its translation were assigned to different speakers.");
            }
            result.append(paired);

            // Keep the pair on screen while its translation is spoken too.
            // Where the two are adjacent this echo is merged back into the
            // segment above by the pass below, leaving a single row.
            Segment echo = paired;
            echo.startMs = translation.startMs;
            echo.endMs = translation.endMs;
            echo.translationEcho = true;
            result.append(echo);

            report.paired++;
        }
        // Odd ones out on either side stay as plain lines.
        for (int k = pairs; k < sourceCount; ++k) {
            report.unpairedGerman++;
            result.append(segments.at(g0 + k));
        }
        for (int k = pairs; k < translationCount; ++k) {
            result.append(segments.at(g1 + k));
        }

        i = e1;
    }

    std::stable_sort(result.begin(), result.end(),
                     [](const Segment& a, const Segment& b) { return a.startMs < b.startMs; });

    // Collapse an echo that sits directly after its own segment: the caption
    // would be identical across the join, so one row reading straight through
    // both is what the viewer sees anyway.
    QList<Segment> collapsed;
    collapsed.reserve(result.size());
    for (const Segment& s : result) {
        if (!collapsed.isEmpty() && s.translationEcho) {
            Segment& previous = collapsed.last();
            if (!previous.translationEcho && previous.text == s.text &&
                previous.translation == s.translation && s.startMs <= previous.endMs + 250) {
                previous.endMs = s.endMs;
                continue;
            }
        }
        collapsed.append(s);
    }

    segments = collapsed;
    return report;
}

int unpairTranslations(QList<Segment>& segments) {
    QList<Segment> split;
    split.reserve(segments.size() * 2);
    int count = 0;

    for (const Segment& s : segments) {
        if (s.translation.isEmpty()) {
            split.append(s);
            continue;
        }

        if (s.translationEcho) {
            // This row only ever existed to hold the pair on screen during the
            // translation's audio - it becomes the translation line again.
            Segment plain = s;
            plain.text = s.translation;
            plain.translation.clear();
            plain.translationStartMs = 0;
            plain.translationEcho = false;
            split.append(plain);
            count++;
            continue;
        }

        if (s.translationStartMs > s.startMs && s.translationStartMs < s.endMs) {
            // Collapsed pair: cut it back at the recorded boundary.
            Segment source = s;
            source.endMs = s.translationStartMs;
            source.translation.clear();
            source.translationStartMs = 0;

            Segment translation = s;
            translation.startMs = s.translationStartMs;
            translation.text = s.translation;
            translation.translation.clear();
            translation.translationStartMs = 0;

            split.append(source);
            split.append(translation);
            count++;
            continue;
        }

        // The translation lives in its own echo row further down; just drop
        // the attachment here.
        Segment source = s;
        source.translation.clear();
        source.translationStartMs = 0;
        split.append(source);
        count++;
    }

    segments = split;
    return count;
}

} // namespace dvs
