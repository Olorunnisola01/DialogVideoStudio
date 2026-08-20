#pragma once

#include <QList>
#include <QString>

#include "SrtParser.h"

namespace dvs {

struct Word {
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString text;
};

// One of the lines that was folded into a merged caption. Kept verbatim so
// merging can be undone exactly - joining the texts with a space is not
// reversible on its own.
struct MergedPiece {
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString text;
    QString translation;
    qint64 translationStartMs = 0;
    bool translationEcho = false;
};

// A subtitle unit: one sentence, one speaker, one on-screen box.
struct Segment {
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString text;
    int speakerId = -1;   // index into Project::speakers, -1 = unassigned
    float confidence = 0.0f;
    bool needsReview = false;
    QString reviewReason;

    // Second line shown beneath the main caption. Filled in by pairing the
    // spoken translation that follows each line (see Translator.h), or typed
    // by hand in the segment table.
    QString translation;
    // Where the translation's own audio started, so pairing can be undone
    // exactly. 0 when this segment was never paired.
    qint64 translationStartMs = 0;
    // A second appearance of the same pair, covering the stretch where the
    // translation itself is being spoken. Happens when the recording reads
    // several lines in a row and only then their translations.
    bool translationEcho = false;

    // The lines this caption was merged from, in order. Empty when it is a
    // single line.
    QList<MergedPiece> mergedFrom;

    qint64 durationMs() const { return endMs - startMs; }
    bool containsTime(qint64 ms) const { return ms >= startMs && ms < endMs; }
};

struct SegmenterOptions {
    // Segments shorter than this with fewer than 3 words are folded into the
    // previous segment rather than flashing on screen on their own.
    qint64 mergeShorterThanMs = 350;
};

// Flattens cues into a single word stream, giving each word a start/end by
// distributing its cue's span across the words in proportion to their length.
// Whisper-style SRTs break mid-sentence, so cue boundaries are deliberately
// discarded here.
QList<Word> wordsFromCues(const QList<SrtCue>& cues);

// Regroups words into sentence-sized segments, splitting after terminal
// punctuation while guarding against abbreviations and initials.
QList<Segment> segmentsFromWords(const QList<Word>& words,
                                 const SegmenterOptions& opts = {});

inline QList<Segment> segmentsFromCues(const QList<SrtCue>& cues,
                                       const SegmenterOptions& opts = {}) {
    return segmentsFromWords(wordsFromCues(cues), opts);
}

struct MergeOptions {
    // Consecutive lines further apart than this are treated as separate
    // thoughts and left alone, even when the same person says both.
    qint64 maxGapMs = 900;
    // A caption nobody can finish reading is worse than two captions, so a
    // merge that would grow past this many characters is not made.
    int maxCharacters = 180;
    // ...and one that would sit on screen longer than this, likewise.
    qint64 maxDurationMs = 9000;
};

struct MergeReport {
    int merged = 0;     // captions produced by folding two or more lines together
    int absorbed = 0;   // lines that disappeared into them
    int before = 0;
    int after = 0;
};

// Folds runs of consecutive lines spoken by the same person into single
// captions that stay up for the whole run. Sentence splitting deliberately cuts
// at every full stop, which is right when speakers alternate but leaves one
// person's two-sentence turn as two flashing captions; this puts those back
// together. Lines with no speaker are left alone, and an echo row only ever
// merges with another echo row, so the result is the same whether this runs
// before or after pairing.
MergeReport mergeSameSpeakerRuns(QList<Segment>& segments, const MergeOptions& opts = {});

// Exact inverse: expands every merged caption back into the lines it came from.
int unmergeSegments(QList<Segment>& segments);

} // namespace dvs
