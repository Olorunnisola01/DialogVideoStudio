#pragma once

#include <QList>
#include <QString>

#include <functional>

#include "AudioDecoder.h"
#include "Segmenter.h"

namespace dvs {

struct DiarizeOptions {
    int speakerCount = 2;      // 0 = decide automatically
    int maxSpeakers = 4;       // upper bound when speakerCount == 0
    double windowSec = 1.5;
    double hopSec = 0.25;
    double vadRelativeDb = -40.0; // speech threshold relative to peak frame
    float minConfidence = 0.60f;  // below this a segment is flagged for review
    QString modelPath;            // empty -> Paths::speakerModelPath()
    bool useLanguageCrossCheck = true;
    bool splitOnSpeakerChange = true;
};

struct DiarizeReport {
    int speakerCount = 0;
    QString backend;          // "DirectML (<gpu>)", "CPU", or "text only"
    int embeddingWindows = 0;
    int reviewCount = 0;
    int splitCount = 0;
    QString warning;          // non-fatal, e.g. model missing
    QString error;

    bool ok() const { return error.isEmpty(); }
};

// Assigns a speakerId to every segment, in place. `words` is used to split a
// segment when the speaker changes inside it; pass an empty list to disable.
// Progress is reported as 0..100.
DiarizeReport diarize(QList<Segment>& segments,
                      const QList<Word>& words,
                      const AudioBuffer& audio,
                      const DiarizeOptions& opts = {},
                      const std::function<void(int)>& progress = {});

// German-vs-English score for one line of text. Returns a value in [-1, 1]:
// positive means German, negative means English, near zero means undecidable.
double germanEnglishScore(const QString& text);

} // namespace dvs
