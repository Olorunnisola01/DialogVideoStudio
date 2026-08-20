#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QRectF>
#include <QSize>
#include <QString>

#include "Segmenter.h"

namespace dvs {

// All geometry is stored normalised to the canvas (0..1) so the on-screen
// preview and the full-resolution export agree exactly, whatever scale the
// preview happens to be drawn at.
struct SubtitleStyle {
    QRectF box{0.08, 0.70, 0.84, 0.20}; // x, y, w, h in canvas fractions
    bool autoHeight = true;             // shrink/grow the box to fit the text

    QColor boxColor{0xCB, 0xE8, 0x6B};  // fill
    QColor textColor{0x11, 0x11, 0x11};
    double boxOpacity = 1.0;
    double cornerRadius = 0.010;        // fraction of canvas height
    double paddingX = 0.022;            // fraction of canvas height
    double paddingY = 0.016;

    QString fontFamily = QStringLiteral("Verdana");
    double fontSize = 0.062;            // fraction of canvas height
    int fontWeight = 500;               // QFont::Weight numeric (100..900)
    bool italic = false;
    double lineSpacing = 1.12;
    int textAlign = 0x0084;             // Qt::AlignCenter

    bool shadowEnabled = false;
    QColor shadowColor{0, 0, 0, 140};
    double shadowOffset = 0.006;        // fraction of canvas height

    bool outlineEnabled = false;
    QColor outlineColor{0, 0, 0};
    double outlineWidth = 0.004;

    QJsonObject toJson() const;
    static SubtitleStyle fromJson(const QJsonObject& o);
};

struct Speaker {
    QString name;
    QColor tint{0x4C, 0x9A, 0xFF}; // timeline/table colour coding only
    SubtitleStyle style;

    QJsonObject toJson() const;
    static Speaker fromJson(const QJsonObject& o);
};

// A word that should be drawn in its own colour wherever it appears in a
// subtitle - the target vocabulary of the lesson, typically.
struct Highlight {
    QString word;
    QColor color{0x1E, 0x6F, 0xE8};
    bool bold = false;

    QJsonObject toJson() const;
    static Highlight fromJson(const QJsonObject& o);
};

// Strips surrounding punctuation and lowercases, so "wohl?" matches "wohl".
QString normaliseWord(const QString& token);

enum class ImageFit { Cover, Contain };

struct Scene {
    QString imagePath;
    qint64 startMs = 0;
    qint64 endMs = 0;
    ImageFit fit = ImageFit::Cover;

    bool containsTime(qint64 ms) const { return ms >= startMs && ms < endMs; }

    QJsonObject toJson() const;
    static Scene fromJson(const QJsonObject& o);
};

// A picture or a line of text pinned over the video: channel logo, subscribe
// button, episode title banner, watermark. Position and size are normalised to
// the canvas like everything else, so one layout works at any resolution.
struct Overlay {
    QString name = QStringLiteral("Overlay");
    bool isImage = true;
    QString imagePath;    // when isImage
    QString text;         // when !isImage
    SubtitleStyle style;  // style.box is the rect; text overlays use the rest
    double opacity = 1.0;
    bool keepAspect = true;   // images: fit inside the rect without distortion
    bool onTop = false;       // draw above the subtitles rather than below
    bool enabled = true;

    // Visible for the whole video when endMs <= 0.
    qint64 startMs = 0;
    qint64 endMs = 0;

    bool visibleAt(qint64 ms) const {
        if (!enabled) return false;
        if (ms < startMs) return false;
        return endMs <= 0 || ms < endMs;
    }

    // 0..1, with the same centred fade the captions use. An overlay that runs
    // for the whole video never fades.
    double alphaAt(qint64 ms, int transitionMs) const;

    // Presets matching the usual YouTube lesson layout.
    static Overlay makeLogo(const QString& imagePath);
    static Overlay makeSubscribe(const QString& imagePath);
    static Overlay makeTitleBanner(const QString& title);

    QJsonObject toJson() const;
    static Overlay fromJson(const QJsonObject& o);
};

struct ExportSettings {
    QString outputPath;
    QString videoCodec = QStringLiteral("libx264");
    QString preset = QStringLiteral("medium");
    int crf = 18;
    int audioBitrateKbps = 192;

    QJsonObject toJson() const;
    static ExportSettings fromJson(const QJsonObject& o);
};

struct Project {
    QString audioPath;
    QString srtPath;
    QSize canvas{1920, 1080};
    int fps = 30;
    QColor backgroundColor{0, 0, 0};

    QList<Speaker> speakers;
    QList<Segment> segments;
    QList<Scene> scenes;
    QList<Highlight> highlights;
    QList<Overlay> overlays;
    ExportSettings exportSettings;

    // The English line drawn beneath each caption.
    bool translationEnabled = true;
    SubtitleStyle translationStyle;

    // How long a caption takes to fade in or out, in milliseconds.
    //
    // A caption with room around it fades symmetrically about its timestamp -
    // half before, half after - so it changes exactly on time with a soft edge.
    // Where two captions touch, they hand over *sequentially* instead: the old
    // one is fully gone the instant the new one starts. Cross-dissolving them
    // would be smoother in the abstract, but two different sentences drawn over
    // each other at half opacity is an unreadable double-exposure - the whole
    // point is to be easier on the eyes, not harder. 0 turns fading off.
    int transitionMs = 140;
    // Distance a caption slides up into place while fading in, as a fraction of
    // the canvas's shorter side. 0 is a pure fade.
    double transitionRise = 0.0;

    qint64 durationMs = 0; // from the audio file; drives the export length

    // Two speakers styled like the reference frames: speaker 0 upper-right on
    // green, speaker 1 lower-left on yellow.
    static Project makeDefault();

    // Index of the segment covering `ms`, or -1. Segments are kept sorted by
    // start time, so this is a binary search.
    int segmentAt(qint64 ms) const;
    int sceneAt(qint64 ms) const;

    // A segment that is on screen at `ms`, with how opaque it is.
    struct ActiveSegment {
        int index = -1;
        double alpha = 0.0;
    };
    QList<ActiveSegment> activeSegmentsAt(qint64 ms) const;

    // Opacity of one segment at `ms`. Needs the index, not the segment, because
    // the fade shape depends on how much room there is either side of it.
    double segmentAlpha(int index, qint64 ms) const;

    const Speaker* speakerFor(const Segment& s) const;

    // The highlight matching `token` (punctuation and case are ignored), or
    // nullptr when the word is not highlighted.
    const Highlight* highlightFor(const QString& token) const;

    // Adds or replaces the highlight for `word`.
    void setHighlight(const QString& word, const QColor& color, bool bold);
    void removeHighlight(const QString& word);

    // Sorts segments and scenes by start time. Call after any edit that can
    // reorder them.
    void normalise();

    // Distributes `scenes` evenly across the timeline when the user has added
    // images without setting ranges.
    void autoLayoutScenes();

    // Repositions the caption, translation and overlay boxes to sensible
    // defaults for the current canvas shape. A layout tuned for 16:9 does not
    // work in 9:16, so this is offered when the canvas preset changes.
    void applyDefaultLayout();

    QJsonObject toJson() const;
    static Project fromJson(const QJsonObject& o);

    QString save(const QString& path) const;  // returns an error, or empty
    static Project load(const QString& path, QString* error);
};

} // namespace dvs
