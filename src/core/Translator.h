#pragma once

#include <QList>

#include "Segmenter.h"

namespace dvs {

struct PairingReport {
    int paired = 0;
    int unpairedGerman = 0; // German lines with no English line after them
    int total = 0;
};

// Lesson audio for this format speaks a German sentence and then its English
// translation. `pairTranslations` folds each such pair into one segment that
// stays on screen for both, carrying the English in Segment::translation - so
// the caption and its translation are shown together rather than one after the
// other.
//
// Pairing uses the same German/English scorer the diarizer cross-checks
// against, and only merges when the first line scores clearly German and the
// next clearly English, they are adjacent, and the gap between them is small.
PairingReport pairTranslations(QList<Segment>& segments);

// Exact inverse of pairTranslations for segments that still carry
// translationStartMs. Segments whose translation was typed by hand (no
// recorded split point) are left alone.
int unpairTranslations(QList<Segment>& segments);

} // namespace dvs
