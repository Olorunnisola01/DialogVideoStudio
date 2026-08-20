#include "Segmenter.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace dvs {

namespace {

// Tokens that end in '.' but do not end a sentence.
const QSet<QString>& abbreviations() {
    static const QSet<QString> set = {
        // German
        QStringLiteral("z.b."), QStringLiteral("u.a."), QStringLiteral("d.h."),
        QStringLiteral("bzw."), QStringLiteral("usw."), QStringLiteral("ca."),
        QStringLiteral("nr."),  QStringLiteral("hr."),  QStringLiteral("fr."),
        QStringLiteral("dr."),  QStringLiteral("prof."), QStringLiteral("str."),
        QStringLiteral("bspw."), QStringLiteral("evtl."), QStringLiteral("ggf."),
        QStringLiteral("inkl."), QStringLiteral("zzgl."), QStringLiteral("vgl."),
        QStringLiteral("s.o."), QStringLiteral("s.u."), QStringLiteral("u.s.w."),
        // English
        QStringLiteral("mr."),  QStringLiteral("mrs."), QStringLiteral("ms."),
        QStringLiteral("e.g."), QStringLiteral("i.e."), QStringLiteral("etc."),
        QStringLiteral("vs."),  QStringLiteral("st."),  QStringLiteral("no."),
        QStringLiteral("approx."), QStringLiteral("fig."),
    };
    return set;
}

QString stripTrailingQuotes(QString s) {
    static const QString closers = QStringLiteral("\"'”’»)]}");
    while (!s.isEmpty() && closers.contains(s.back())) {
        s.chop(1);
    }
    return s;
}

// True if `token` terminates a sentence. `next` is the following token (empty
// at the end of the stream) and is used to reject lowercase continuations such
// as "3. Januar".
bool endsSentence(const QString& token, const QString& next) {
    const QString bare = stripTrailingQuotes(token);
    if (bare.isEmpty()) return false;

    const QChar last = bare.back();
    const bool terminal = (last == QLatin1Char('.') || last == QLatin1Char('!') ||
                           last == QLatin1Char('?') || last == QChar(0x2026)); // …
    if (!terminal) return false;

    // '!' and '?' are unambiguous; only '.' needs the abbreviation guards.
    if (last == QLatin1Char('.')) {
        if (abbreviations().contains(bare.toLower())) return false;

        // Single initial ("A." in "A. Meier") or a bare ordinal ("3.").
        const QString stem = bare.left(bare.size() - 1);
        if (stem.size() <= 1) return false;
        bool numeric = false;
        stem.toLongLong(&numeric);
        if (numeric) return false;
    }

    if (next.isEmpty()) return true;

    // A following lowercase word means the punctuation was not a full stop.
    const QChar firstNext = stripTrailingQuotes(next).front();
    if (firstNext.isLetter() && firstNext.isLower()) return false;

    return true;
}

int wordCount(const QString& text) {
    return static_cast<int>(
        text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts).size());
}

} // namespace

QList<Word> wordsFromCues(const QList<SrtCue>& cues) {
    QList<Word> words;

    for (const SrtCue& cue : cues) {
        const QStringList tokens =
            cue.text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
        if (tokens.isEmpty()) continue;

        const qint64 span = qMax<qint64>(cue.endMs - cue.startMs, 1);

        // Weight by token length so long words get proportionally more time
        // than short ones; +1 accounts for the separating space.
        qint64 totalWeight = 0;
        for (const QString& t : tokens) totalWeight += t.size() + 1;

        qint64 acc = 0;
        for (const QString& t : tokens) {
            const qint64 weight = t.size() + 1;
            Word w;
            w.startMs = cue.startMs + (span * acc) / totalWeight;
            acc += weight;
            w.endMs = cue.startMs + (span * acc) / totalWeight;
            w.text = t;
            words.append(w);
        }
    }

    // Cues can overlap slightly; keep the stream monotonic so downstream
    // timing maths never produces negative durations.
    for (int i = 1; i < words.size(); ++i) {
        if (words[i].startMs < words[i - 1].endMs) {
            words[i].startMs = words[i - 1].endMs;
        }
        if (words[i].endMs < words[i].startMs) {
            words[i].endMs = words[i].startMs;
        }
    }

    return words;
}

QList<Segment> segmentsFromWords(const QList<Word>& words, const SegmenterOptions& opts) {
    QList<Segment> segments;
    if (words.isEmpty()) return segments;

    QStringList pending;
    qint64 pendingStart = words.front().startMs;

    auto flush = [&](qint64 endMs) {
        if (pending.isEmpty()) return;
        Segment s;
        s.startMs = pendingStart;
        s.endMs = endMs;
        s.text = pending.join(QLatin1Char(' '));
        segments.append(s);
        pending.clear();
    };

    for (int i = 0; i < words.size(); ++i) {
        if (pending.isEmpty()) pendingStart = words.at(i).startMs;
        pending << words.at(i).text;

        const QString next = (i + 1 < words.size()) ? words.at(i + 1).text : QString();
        if (endsSentence(words.at(i).text, next)) {
            flush(words.at(i).endMs);
        }
    }
    flush(words.back().endMs);

    // Fold away fragments too short to read.
    for (int i = segments.size() - 1; i > 0; --i) {
        const Segment& s = segments.at(i);
        if (s.durationMs() < opts.mergeShorterThanMs && wordCount(s.text) < 3) {
            segments[i - 1].endMs = s.endMs;
            segments[i - 1].text += QLatin1Char(' ') + s.text;
            segments.removeAt(i);
        }
    }

    return segments;
}

namespace {

MergedPiece pieceOf(const Segment& s) {
    MergedPiece p;
    p.startMs = s.startMs;
    p.endMs = s.endMs;
    p.text = s.text;
    p.translation = s.translation;
    p.translationStartMs = s.translationStartMs;
    p.translationEcho = s.translationEcho;
    return p;
}

// An unassigned line has no speaker to match on, so it is never merged.
bool isMergeable(const Segment& s) { return s.speakerId >= 0; }

// Echo rows re-show a pair while its translation is spoken. They merge happily
// with each other, but never with an ordinary row - mixing the two would put
// the same words on screen for a stretch that covers both readings. Allowing
// echo-with-echo is what makes merging give the same answer whether it is run
// before or after pairing.
bool sameKind(const Segment& a, const Segment& b) {
    return a.translationEcho == b.translationEcho;
}

} // namespace

MergeReport mergeSameSpeakerRuns(QList<Segment>& segments, const MergeOptions& opts) {
    MergeReport report;
    report.before = static_cast<int>(segments.size());
    if (segments.size() < 2) {
        report.after = report.before;
        return report;
    }

    QList<Segment> result;
    result.reserve(segments.size());

    int i = 0;
    while (i < segments.size()) {
        const Segment& head = segments.at(i);
        if (!isMergeable(head)) {
            result.append(head);
            ++i;
            continue;
        }

        // Collect the run of following lines by the same speaker that still fit
        // inside the gap, length and duration limits.
        QList<MergedPiece> pieces{pieceOf(head)};
        int textLength = head.text.size();
        int last = i;

        for (int j = i + 1; j < segments.size(); ++j) {
            const Segment& next = segments.at(j);
            if (!isMergeable(next) || next.speakerId != head.speakerId) break;
            if (!sameKind(next, head)) break;
            if (next.startMs - segments.at(last).endMs > opts.maxGapMs) break;
            if (next.endMs - head.startMs > opts.maxDurationMs) break;
            if (textLength + 1 + next.text.size() > opts.maxCharacters) break;

            pieces.append(pieceOf(next));
            textLength += 1 + static_cast<int>(next.text.size());
            last = j;
        }

        if (pieces.size() == 1) {
            result.append(head);
            ++i;
            continue;
        }

        Segment merged = head;
        merged.endMs = segments.at(last).endMs;
        merged.mergedFrom = pieces;

        QStringList texts;
        QStringList translations;
        float lowestConfidence = head.confidence;
        for (int k = i; k <= last; ++k) {
            const Segment& part = segments.at(k);
            texts << part.text;
            if (!part.translation.isEmpty()) translations << part.translation;
            lowestConfidence = std::min(lowestConfidence, part.confidence);
            if (part.needsReview && !merged.needsReview) {
                merged.needsReview = true;
                merged.reviewReason = part.reviewReason;
            }
        }
        merged.text = texts.join(QLatin1Char(' '));
        merged.translation = translations.join(QLatin1Char(' '));
        // The join point is no longer meaningful on the merged caption; the
        // original values live on in mergedFrom.
        merged.translationStartMs = 0;
        merged.confidence = lowestConfidence;

        result.append(merged);
        report.merged++;
        report.absorbed += static_cast<int>(pieces.size()) - 1;
        i = last + 1;
    }

    segments = result;
    report.after = static_cast<int>(segments.size());
    return report;
}

int unmergeSegments(QList<Segment>& segments) {
    QList<Segment> expanded;
    expanded.reserve(segments.size());
    int count = 0;

    for (const Segment& s : segments) {
        if (s.mergedFrom.isEmpty()) {
            expanded.append(s);
            continue;
        }
        for (const MergedPiece& piece : s.mergedFrom) {
            Segment part = s;
            part.startMs = piece.startMs;
            part.endMs = piece.endMs;
            part.text = piece.text;
            part.translation = piece.translation;
            part.translationStartMs = piece.translationStartMs;
            part.translationEcho = piece.translationEcho;
            part.mergedFrom.clear();
            expanded.append(part);
        }
        count++;
    }

    segments = expanded;
    return count;
}

} // namespace dvs
