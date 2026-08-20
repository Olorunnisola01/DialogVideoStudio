#include "Project.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>

namespace dvs {

namespace {

QJsonObject rectToJson(const QRectF& r) {
    return QJsonObject{{QStringLiteral("x"), r.x()},
                       {QStringLiteral("y"), r.y()},
                       {QStringLiteral("w"), r.width()},
                       {QStringLiteral("h"), r.height()}};
}

QRectF rectFromJson(const QJsonObject& o, const QRectF& fallback) {
    if (o.isEmpty()) return fallback;
    return QRectF(o.value(QStringLiteral("x")).toDouble(fallback.x()),
                  o.value(QStringLiteral("y")).toDouble(fallback.y()),
                  o.value(QStringLiteral("w")).toDouble(fallback.width()),
                  o.value(QStringLiteral("h")).toDouble(fallback.height()));
}

QString colorToJson(const QColor& c) { return c.name(QColor::HexArgb); }

// Cubic ease, symmetric about 0.5: smoothstep(x) + smoothstep(1-x) == 1. That
// identity is what lets two touching captions cross-fade at constant total
// opacity - a linear ramp would work too, but this has no visible corner where
// the fade starts and stops.
double smoothstep(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// Shared by captions and timed overlays: 0 outside the window, ramping across
// `transitionMs` centred on each edge.
double centredFade(qint64 ms, qint64 startMs, qint64 endMs, int transitionMs) {
    if (transitionMs <= 0) return (ms >= startMs && ms < endMs) ? 1.0 : 0.0;
    const double span = transitionMs;
    const double half = span / 2.0;
    const double rampIn = (ms - (startMs - half)) / span;
    const double rampOut = ((endMs + half) - ms) / span;
    // The min keeps a segment shorter than the fade from ever reaching full
    // opacity, instead of the two ramps fighting each other.
    return std::min(smoothstep(rampIn), smoothstep(rampOut));
}

QColor colorFromJson(const QJsonValue& v, const QColor& fallback) {
    if (!v.isString()) return fallback;
    const QColor c(v.toString());
    return c.isValid() ? c : fallback;
}

} // namespace

// --- SubtitleStyle ---------------------------------------------------------

QJsonObject SubtitleStyle::toJson() const {
    return QJsonObject{
        {QStringLiteral("box"), rectToJson(box)},
        {QStringLiteral("autoHeight"), autoHeight},
        {QStringLiteral("boxColor"), colorToJson(boxColor)},
        {QStringLiteral("textColor"), colorToJson(textColor)},
        {QStringLiteral("boxOpacity"), boxOpacity},
        {QStringLiteral("cornerRadius"), cornerRadius},
        {QStringLiteral("paddingX"), paddingX},
        {QStringLiteral("paddingY"), paddingY},
        {QStringLiteral("fontFamily"), fontFamily},
        {QStringLiteral("fontSize"), fontSize},
        {QStringLiteral("fontWeight"), fontWeight},
        {QStringLiteral("italic"), italic},
        {QStringLiteral("lineSpacing"), lineSpacing},
        {QStringLiteral("textAlign"), textAlign},
        {QStringLiteral("shadowEnabled"), shadowEnabled},
        {QStringLiteral("shadowColor"), colorToJson(shadowColor)},
        {QStringLiteral("shadowOffset"), shadowOffset},
        {QStringLiteral("outlineEnabled"), outlineEnabled},
        {QStringLiteral("outlineColor"), colorToJson(outlineColor)},
        {QStringLiteral("outlineWidth"), outlineWidth},
    };
}

SubtitleStyle SubtitleStyle::fromJson(const QJsonObject& o) {
    SubtitleStyle s;
    s.box = rectFromJson(o.value(QStringLiteral("box")).toObject(), s.box);
    s.autoHeight = o.value(QStringLiteral("autoHeight")).toBool(s.autoHeight);
    s.boxColor = colorFromJson(o.value(QStringLiteral("boxColor")), s.boxColor);
    s.textColor = colorFromJson(o.value(QStringLiteral("textColor")), s.textColor);
    s.boxOpacity = o.value(QStringLiteral("boxOpacity")).toDouble(s.boxOpacity);
    s.cornerRadius = o.value(QStringLiteral("cornerRadius")).toDouble(s.cornerRadius);
    s.paddingX = o.value(QStringLiteral("paddingX")).toDouble(s.paddingX);
    s.paddingY = o.value(QStringLiteral("paddingY")).toDouble(s.paddingY);
    s.fontFamily = o.value(QStringLiteral("fontFamily")).toString(s.fontFamily);
    s.fontSize = o.value(QStringLiteral("fontSize")).toDouble(s.fontSize);
    s.fontWeight = o.value(QStringLiteral("fontWeight")).toInt(s.fontWeight);
    s.italic = o.value(QStringLiteral("italic")).toBool(s.italic);
    s.lineSpacing = o.value(QStringLiteral("lineSpacing")).toDouble(s.lineSpacing);
    s.textAlign = o.value(QStringLiteral("textAlign")).toInt(s.textAlign);
    s.shadowEnabled = o.value(QStringLiteral("shadowEnabled")).toBool(s.shadowEnabled);
    s.shadowColor = colorFromJson(o.value(QStringLiteral("shadowColor")), s.shadowColor);
    s.shadowOffset = o.value(QStringLiteral("shadowOffset")).toDouble(s.shadowOffset);
    s.outlineEnabled = o.value(QStringLiteral("outlineEnabled")).toBool(s.outlineEnabled);
    s.outlineColor = colorFromJson(o.value(QStringLiteral("outlineColor")), s.outlineColor);
    s.outlineWidth = o.value(QStringLiteral("outlineWidth")).toDouble(s.outlineWidth);
    return s;
}

// --- Speaker ---------------------------------------------------------------

QJsonObject Speaker::toJson() const {
    return QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("tint"), colorToJson(tint)},
        {QStringLiteral("style"), style.toJson()},
    };
}

Speaker Speaker::fromJson(const QJsonObject& o) {
    Speaker sp;
    sp.name = o.value(QStringLiteral("name")).toString(sp.name);
    sp.tint = colorFromJson(o.value(QStringLiteral("tint")), sp.tint);
    sp.style = SubtitleStyle::fromJson(o.value(QStringLiteral("style")).toObject());
    return sp;
}

// --- Highlight -------------------------------------------------------------

QJsonObject Highlight::toJson() const {
    return QJsonObject{
        {QStringLiteral("word"), word},
        {QStringLiteral("color"), colorToJson(color)},
        {QStringLiteral("bold"), bold},
    };
}

Highlight Highlight::fromJson(const QJsonObject& o) {
    Highlight h;
    h.word = o.value(QStringLiteral("word")).toString();
    h.color = colorFromJson(o.value(QStringLiteral("color")), h.color);
    h.bold = o.value(QStringLiteral("bold")).toBool(h.bold);
    return h;
}

QString normaliseWord(const QString& token) {
    int begin = 0;
    int end = token.size();
    while (begin < end && !token.at(begin).isLetterOrNumber()) ++begin;
    while (end > begin && !token.at(end - 1).isLetterOrNumber()) --end;
    return token.mid(begin, end - begin).toLower();
}

// --- Scene -----------------------------------------------------------------

QJsonObject Scene::toJson() const {
    return QJsonObject{
        {QStringLiteral("imagePath"), imagePath},
        {QStringLiteral("startMs"), static_cast<double>(startMs)},
        {QStringLiteral("endMs"), static_cast<double>(endMs)},
        {QStringLiteral("fit"), fit == ImageFit::Cover ? QStringLiteral("cover")
                                                       : QStringLiteral("contain")},
    };
}

Scene Scene::fromJson(const QJsonObject& o) {
    Scene s;
    s.imagePath = o.value(QStringLiteral("imagePath")).toString();
    s.startMs = static_cast<qint64>(o.value(QStringLiteral("startMs")).toDouble());
    s.endMs = static_cast<qint64>(o.value(QStringLiteral("endMs")).toDouble());
    s.fit = o.value(QStringLiteral("fit")).toString() == QLatin1String("contain")
                ? ImageFit::Contain
                : ImageFit::Cover;
    return s;
}

// --- Overlay ---------------------------------------------------------------

double Overlay::alphaAt(qint64 ms, int transitionMs) const {
    if (!enabled) return 0.0;
    // Whole-video overlays have no edges to soften.
    if (startMs <= 0 && endMs <= 0) return 1.0;
    const qint64 effectiveEnd = endMs > 0 ? endMs : std::numeric_limits<qint64>::max() / 4;
    return centredFade(ms, startMs, effectiveEnd, transitionMs);
}

Overlay Overlay::makeLogo(const QString& imagePath) {
    Overlay o;
    o.name = QStringLiteral("Logo");
    o.isImage = true;
    o.imagePath = imagePath;
    o.style.box = QRectF(0.02, 0.86, 0.20, 0.12);
    return o;
}

Overlay Overlay::makeSubscribe(const QString& imagePath) {
    Overlay o;
    o.name = QStringLiteral("Subscribe");
    o.isImage = true;
    o.imagePath = imagePath;
    o.style.box = QRectF(0.66, 0.88, 0.32, 0.08);
    return o;
}

Overlay Overlay::makeTitleBanner(const QString& title) {
    Overlay o;
    o.name = QStringLiteral("Title");
    o.isImage = false;
    o.text = title;
    o.style.box = QRectF(0.0, 0.0, 1.0, 0.085);
    o.style.autoHeight = false;
    o.style.boxColor = QColor(0x8E, 0x1C, 0x22);
    o.style.textColor = QColor(0xFF, 0xFF, 0xFF);
    o.style.cornerRadius = 0.0;
    o.style.fontSize = 0.048;
    o.style.fontWeight = 700;
    o.style.textAlign = static_cast<int>(Qt::AlignCenter);
    return o;
}

QJsonObject Overlay::toJson() const {
    return QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("isImage"), isImage},
        {QStringLiteral("imagePath"), imagePath},
        {QStringLiteral("text"), text},
        {QStringLiteral("style"), style.toJson()},
        {QStringLiteral("opacity"), opacity},
        {QStringLiteral("keepAspect"), keepAspect},
        {QStringLiteral("onTop"), onTop},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("startMs"), static_cast<double>(startMs)},
        {QStringLiteral("endMs"), static_cast<double>(endMs)},
    };
}

Overlay Overlay::fromJson(const QJsonObject& o) {
    Overlay v;
    v.name = o.value(QStringLiteral("name")).toString(v.name);
    v.isImage = o.value(QStringLiteral("isImage")).toBool(v.isImage);
    v.imagePath = o.value(QStringLiteral("imagePath")).toString();
    v.text = o.value(QStringLiteral("text")).toString();
    v.style = SubtitleStyle::fromJson(o.value(QStringLiteral("style")).toObject());
    v.opacity = o.value(QStringLiteral("opacity")).toDouble(v.opacity);
    v.keepAspect = o.value(QStringLiteral("keepAspect")).toBool(v.keepAspect);
    v.onTop = o.value(QStringLiteral("onTop")).toBool(v.onTop);
    v.enabled = o.value(QStringLiteral("enabled")).toBool(v.enabled);
    v.startMs = static_cast<qint64>(o.value(QStringLiteral("startMs")).toDouble());
    v.endMs = static_cast<qint64>(o.value(QStringLiteral("endMs")).toDouble());
    return v;
}

// --- ExportSettings --------------------------------------------------------

QJsonObject ExportSettings::toJson() const {
    return QJsonObject{
        {QStringLiteral("outputPath"), outputPath},
        {QStringLiteral("videoCodec"), videoCodec},
        {QStringLiteral("preset"), preset},
        {QStringLiteral("crf"), crf},
        {QStringLiteral("audioBitrateKbps"), audioBitrateKbps},
    };
}

ExportSettings ExportSettings::fromJson(const QJsonObject& o) {
    ExportSettings e;
    e.outputPath = o.value(QStringLiteral("outputPath")).toString();
    e.videoCodec = o.value(QStringLiteral("videoCodec")).toString(e.videoCodec);
    e.preset = o.value(QStringLiteral("preset")).toString(e.preset);
    e.crf = o.value(QStringLiteral("crf")).toInt(e.crf);
    e.audioBitrateKbps = o.value(QStringLiteral("audioBitrateKbps")).toInt(e.audioBitrateKbps);
    return e;
}

// --- Segment (not a public struct method; local helpers) -------------------

namespace {

QJsonObject segmentToJson(const Segment& s) {
    return QJsonObject{
        {QStringLiteral("startMs"), static_cast<double>(s.startMs)},
        {QStringLiteral("endMs"), static_cast<double>(s.endMs)},
        {QStringLiteral("text"), s.text},
        {QStringLiteral("speakerId"), s.speakerId},
        {QStringLiteral("confidence"), s.confidence},
        {QStringLiteral("needsReview"), s.needsReview},
        {QStringLiteral("reviewReason"), s.reviewReason},
        {QStringLiteral("translation"), s.translation},
        {QStringLiteral("translationStartMs"), static_cast<double>(s.translationStartMs)},
        {QStringLiteral("translationEcho"), s.translationEcho},
        {QStringLiteral("mergedFrom"), [&] {
             QJsonArray pieces;
             for (const MergedPiece& p : s.mergedFrom) {
                 pieces.append(QJsonObject{
                     {QStringLiteral("startMs"), static_cast<double>(p.startMs)},
                     {QStringLiteral("endMs"), static_cast<double>(p.endMs)},
                     {QStringLiteral("text"), p.text},
                     {QStringLiteral("translation"), p.translation},
                     {QStringLiteral("translationStartMs"),
                      static_cast<double>(p.translationStartMs)},
                     {QStringLiteral("translationEcho"), p.translationEcho},
                 });
             }
             return pieces;
         }()},
    };
}

Segment segmentFromJson(const QJsonObject& o) {
    Segment s;
    s.startMs = static_cast<qint64>(o.value(QStringLiteral("startMs")).toDouble());
    s.endMs = static_cast<qint64>(o.value(QStringLiteral("endMs")).toDouble());
    s.text = o.value(QStringLiteral("text")).toString();
    s.speakerId = o.value(QStringLiteral("speakerId")).toInt(-1);
    s.confidence = static_cast<float>(o.value(QStringLiteral("confidence")).toDouble());
    s.needsReview = o.value(QStringLiteral("needsReview")).toBool();
    s.reviewReason = o.value(QStringLiteral("reviewReason")).toString();
    s.translation = o.value(QStringLiteral("translation")).toString();
    s.translationStartMs =
        static_cast<qint64>(o.value(QStringLiteral("translationStartMs")).toDouble());
    s.translationEcho = o.value(QStringLiteral("translationEcho")).toBool();
    for (const QJsonValue& v : o.value(QStringLiteral("mergedFrom")).toArray()) {
        const QJsonObject po = v.toObject();
        MergedPiece p;
        p.startMs = static_cast<qint64>(po.value(QStringLiteral("startMs")).toDouble());
        p.endMs = static_cast<qint64>(po.value(QStringLiteral("endMs")).toDouble());
        p.text = po.value(QStringLiteral("text")).toString();
        p.translation = po.value(QStringLiteral("translation")).toString();
        p.translationStartMs =
            static_cast<qint64>(po.value(QStringLiteral("translationStartMs")).toDouble());
        p.translationEcho = po.value(QStringLiteral("translationEcho")).toBool();
        s.mergedFrom.append(p);
    }
    return s;
}

} // namespace

// --- Project ---------------------------------------------------------------

Project Project::makeDefault() {
    Project p;

    Speaker a;
    a.name = QStringLiteral("Speaker 1");
    a.tint = QColor(0x7C, 0xB3, 0x42);
    a.style.box = QRectF(0.41, 0.47, 0.46, 0.24);
    a.style.boxColor = QColor(0xCB, 0xE8, 0x6B);
    a.style.cornerRadius = 0.0;
    a.style.textAlign = static_cast<int>(Qt::AlignCenter);

    Speaker b;
    b.name = QStringLiteral("Speaker 2");
    b.tint = QColor(0xE8, 0xC1, 0x1C);
    b.style.box = QRectF(0.03, 0.53, 0.52, 0.16);
    b.style.boxColor = QColor(0xFF, 0xE0, 0x1B);
    b.style.cornerRadius = 0.012;
    b.style.textAlign = static_cast<int>(Qt::AlignCenter);

    p.speakers = {a, b};

    // The English line: a wide band low in the frame, white on a soft dark
    // wash so it stays readable over any artwork without competing with the
    // German caption above it.
    p.translationStyle.box = QRectF(0.06, 0.78, 0.88, 0.11);
    p.translationStyle.boxColor = QColor(0x00, 0x00, 0x00);
    p.translationStyle.boxOpacity = 0.35;
    p.translationStyle.textColor = QColor(0xFF, 0xFF, 0xFF);
    p.translationStyle.cornerRadius = 0.006;
    p.translationStyle.fontSize = 0.042;
    p.translationStyle.fontWeight = 400;
    p.translationStyle.textAlign = static_cast<int>(Qt::AlignCenter);

    return p;
}

int Project::segmentAt(qint64 ms) const {
    int lo = 0;
    int hi = static_cast<int>(segments.size()) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const Segment& s = segments.at(mid);
        if (ms < s.startMs) {
            hi = mid - 1;
        } else if (ms >= s.endMs) {
            lo = mid + 1;
        } else {
            return mid;
        }
    }
    return -1;
}

double Project::segmentAlpha(int index, qint64 ms) const {
    if (index < 0 || index >= segments.size()) return 0.0;
    const Segment& s = segments.at(index);
    if (transitionMs <= 0) return (ms >= s.startMs && ms < s.endMs) ? 1.0 : 0.0;

    const double half = transitionMs / 2.0;
    constexpr qint64 kNoNeighbour = std::numeric_limits<qint64>::max() / 4;
    const qint64 gapBefore =
        index > 0 ? s.startMs - segments.at(index - 1).endMs : kNoNeighbour;
    const qint64 gapAfter = index + 1 < segments.size()
                                ? segments.at(index + 1).startMs - s.endMs
                                : kNoNeighbour;

    // With room to spare, straddle the timestamp. Butted up against a
    // neighbour, keep the whole ramp on this segment's own side of the join so
    // the two captions are never drawn over each other.
    const bool roomBefore = gapBefore >= static_cast<qint64>(half);
    const bool roomAfter = gapAfter >= static_cast<qint64>(half);

    const double inStart = roomBefore ? s.startMs - half : s.startMs;
    const double inEnd = s.startMs + half;
    const double outStart = s.endMs - half;
    const double outEnd = roomAfter ? s.endMs + half : s.endMs;

    const double rampIn = (ms - inStart) / std::max(1.0, inEnd - inStart);
    const double rampOut = (outEnd - ms) / std::max(1.0, outEnd - outStart);
    return std::min(smoothstep(rampIn), smoothstep(rampOut));
}

QList<Project::ActiveSegment> Project::activeSegmentsAt(qint64 ms) const {
    QList<ActiveSegment> active;
    if (segments.isEmpty()) return active;

    const qint64 half = transitionMs > 0 ? transitionMs / 2 : 0;

    // First segment whose fade-out has not finished by `ms`. Segments are sorted
    // and non-overlapping, so at most two are on screen at once.
    int lo = 0;
    int hi = static_cast<int>(segments.size()) - 1;
    int first = static_cast<int>(segments.size());
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (segments.at(mid).endMs + half > ms) {
            first = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    for (int i = first; i < segments.size(); ++i) {
        if (segments.at(i).startMs - half > ms) break;
        const double alpha = segmentAlpha(i, ms);
        if (alpha > 0.002) active.append({i, alpha});
    }
    return active;
}

int Project::sceneAt(qint64 ms) const {
    // Later scenes win on overlap, which makes "drop a new image on top"
    // behave the way the timeline looks.
    for (int i = static_cast<int>(scenes.size()) - 1; i >= 0; --i) {
        if (scenes.at(i).containsTime(ms)) return i;
    }
    return -1;
}

const Speaker* Project::speakerFor(const Segment& s) const {
    if (s.speakerId < 0 || s.speakerId >= speakers.size()) return nullptr;
    return &speakers.at(s.speakerId);
}

const Highlight* Project::highlightFor(const QString& token) const {
    if (highlights.isEmpty()) return nullptr;
    const QString key = normaliseWord(token);
    if (key.isEmpty()) return nullptr;
    for (const Highlight& h : highlights) {
        if (normaliseWord(h.word) == key) return &h;
    }
    return nullptr;
}

void Project::setHighlight(const QString& word, const QColor& color, bool bold) {
    const QString key = normaliseWord(word);
    if (key.isEmpty()) return;
    for (Highlight& h : highlights) {
        if (normaliseWord(h.word) == key) {
            h.color = color;
            h.bold = bold;
            return;
        }
    }
    Highlight h;
    h.word = normaliseWord(word);
    h.color = color;
    h.bold = bold;
    highlights.append(h);
}

void Project::removeHighlight(const QString& word) {
    const QString key = normaliseWord(word);
    for (int i = highlights.size() - 1; i >= 0; --i) {
        if (normaliseWord(highlights.at(i).word) == key) highlights.removeAt(i);
    }
}

void Project::normalise() {
    std::stable_sort(segments.begin(), segments.end(),
                     [](const Segment& a, const Segment& b) { return a.startMs < b.startMs; });
    std::stable_sort(scenes.begin(), scenes.end(),
                     [](const Scene& a, const Scene& b) { return a.startMs < b.startMs; });

    // Segments must not overlap, or segmentAt's binary search can miss.
    for (int i = 1; i < segments.size(); ++i) {
        if (segments[i].startMs < segments[i - 1].endMs) {
            segments[i - 1].endMs = segments[i].startMs;
        }
    }
    for (Segment& s : segments) {
        if (s.endMs < s.startMs) s.endMs = s.startMs;
    }
}

void Project::autoLayoutScenes() {
    if (scenes.isEmpty() || durationMs <= 0) return;
    const int n = static_cast<int>(scenes.size());
    for (int i = 0; i < n; ++i) {
        scenes[i].startMs = durationMs * i / n;
        scenes[i].endMs = durationMs * (i + 1) / n;
    }
}

void Project::applyDefaultLayout() {
    const bool portrait = canvas.height() > canvas.width();

    if (portrait) {
        // Vertical video: one wide caption band, speakers told apart by colour
        // rather than by position, with room for the title and the logos.
        for (Speaker& s : speakers) {
            s.style.box = QRectF(0.05, 0.56, 0.90, 0.13);
        }
        translationStyle.box = QRectF(0.05, 0.71, 0.90, 0.09);
    } else {
        // Wide video: the two speakers get opposite corners, as in a dialogue.
        for (int i = 0; i < speakers.size(); ++i) {
            speakers[i].style.box = (i % 2 == 0) ? QRectF(0.41, 0.44, 0.46, 0.22)
                                                 : QRectF(0.04, 0.50, 0.50, 0.16);
        }
        translationStyle.box = QRectF(0.06, 0.78, 0.88, 0.11);
    }

    for (Overlay& o : overlays) {
        if (o.name == QLatin1String("Logo")) {
            o.style.box = portrait ? QRectF(0.03, 0.88, 0.28, 0.09)
                                   : QRectF(0.02, 0.86, 0.20, 0.12);
        } else if (o.name == QLatin1String("Subscribe")) {
            o.style.box = portrait ? QRectF(0.55, 0.89, 0.42, 0.06)
                                   : QRectF(0.66, 0.88, 0.32, 0.08);
        } else if (o.name == QLatin1String("Title")) {
            o.style.box = QRectF(0.0, 0.0, 1.0, portrait ? 0.06 : 0.085);
        }
    }
}

QJsonObject Project::toJson() const {
    QJsonArray speakerArr;
    for (const Speaker& s : speakers) speakerArr.append(s.toJson());
    QJsonArray segmentArr;
    for (const Segment& s : segments) segmentArr.append(segmentToJson(s));
    QJsonArray sceneArr;
    for (const Scene& s : scenes) sceneArr.append(s.toJson());
    QJsonArray highlightArr;
    for (const Highlight& h : highlights) highlightArr.append(h.toJson());
    QJsonArray overlayArr;
    for (const Overlay& o : overlays) overlayArr.append(o.toJson());

    return QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("audioPath"), audioPath},
        {QStringLiteral("srtPath"), srtPath},
        {QStringLiteral("canvasWidth"), canvas.width()},
        {QStringLiteral("canvasHeight"), canvas.height()},
        {QStringLiteral("fps"), fps},
        {QStringLiteral("backgroundColor"), colorToJson(backgroundColor)},
        {QStringLiteral("durationMs"), static_cast<double>(durationMs)},
        {QStringLiteral("speakers"), speakerArr},
        {QStringLiteral("segments"), segmentArr},
        {QStringLiteral("scenes"), sceneArr},
        {QStringLiteral("highlights"), highlightArr},
        {QStringLiteral("overlays"), overlayArr},
        {QStringLiteral("translationEnabled"), translationEnabled},
        {QStringLiteral("translationStyle"), translationStyle.toJson()},
        {QStringLiteral("transitionMs"), transitionMs},
        {QStringLiteral("transitionRise"), transitionRise},
        {QStringLiteral("export"), exportSettings.toJson()},
    };
}

Project Project::fromJson(const QJsonObject& o) {
    Project p;
    p.audioPath = o.value(QStringLiteral("audioPath")).toString();
    p.srtPath = o.value(QStringLiteral("srtPath")).toString();
    p.canvas = QSize(o.value(QStringLiteral("canvasWidth")).toInt(1920),
                     o.value(QStringLiteral("canvasHeight")).toInt(1080));
    p.fps = o.value(QStringLiteral("fps")).toInt(30);
    p.backgroundColor = colorFromJson(o.value(QStringLiteral("backgroundColor")), p.backgroundColor);
    p.durationMs = static_cast<qint64>(o.value(QStringLiteral("durationMs")).toDouble());

    for (const QJsonValue& v : o.value(QStringLiteral("speakers")).toArray()) {
        p.speakers.append(Speaker::fromJson(v.toObject()));
    }
    for (const QJsonValue& v : o.value(QStringLiteral("segments")).toArray()) {
        p.segments.append(segmentFromJson(v.toObject()));
    }
    for (const QJsonValue& v : o.value(QStringLiteral("scenes")).toArray()) {
        p.scenes.append(Scene::fromJson(v.toObject()));
    }
    for (const QJsonValue& v : o.value(QStringLiteral("highlights")).toArray()) {
        p.highlights.append(Highlight::fromJson(v.toObject()));
    }
    for (const QJsonValue& v : o.value(QStringLiteral("overlays")).toArray()) {
        p.overlays.append(Overlay::fromJson(v.toObject()));
    }
    p.translationEnabled =
        o.value(QStringLiteral("translationEnabled")).toBool(p.translationEnabled);
    p.transitionMs = o.value(QStringLiteral("transitionMs")).toInt(p.transitionMs);
    p.transitionRise = o.value(QStringLiteral("transitionRise")).toDouble(p.transitionRise);
    if (o.contains(QStringLiteral("translationStyle"))) {
        p.translationStyle =
            SubtitleStyle::fromJson(o.value(QStringLiteral("translationStyle")).toObject());
    } else {
        p.translationStyle = makeDefault().translationStyle;
    }
    p.exportSettings = ExportSettings::fromJson(o.value(QStringLiteral("export")).toObject());

    if (p.speakers.isEmpty()) p.speakers = makeDefault().speakers;
    p.normalise();
    return p;
}

QString Project::save(const QString& path) const {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return QStringLiteral("Cannot write %1: %2").arg(path, f.errorString());
    }
    f.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        return QStringLiteral("Cannot save %1: %2").arg(path, f.errorString());
    }
    return {};
}

Project Project::load(const QString& path, QString* error) {
    if (error) error->clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot open %1: %2").arg(path, f.errorString());
        return makeDefault();
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("%1 is not a valid project file: %2")
                                .arg(path, perr.errorString());
        return makeDefault();
    }
    return fromJson(doc.object());
}

} // namespace dvs
