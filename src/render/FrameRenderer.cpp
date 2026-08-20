#include "FrameRenderer.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace dvs {

namespace {

// Text sizes, padding and radii are fractions of the canvas's *shorter* side.
// Keying them to the height would make a portrait video's type enormous
// relative to its width; for 16:9 this is the height, so landscape layouts are
// unchanged.
double styleScale(const QSize& canvas) {
    return std::min(canvas.width(), canvas.height());
}

QFont buildFont(const SubtitleStyle& style, const QSize& canvas) {
    QFont font(style.fontFamily);
    font.setPixelSize(std::max(1, qRound(style.fontSize * styleScale(canvas))));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(style.fontWeight, 1, 1000)));
    font.setItalic(style.italic);
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

// One word, already resolved to the colour and weight it will be drawn with.
// A highlighted word is stored split so that only the letters take the accent
// colour - surrounding punctuation stays in the normal text colour, which is
// what "colour the word Wörterbuch" is understood to mean in "Wörterbuch,".
struct Token {
    QString prefix; // leading punctuation, base colour
    QString core;   // the word itself, accent colour when highlighted
    QString suffix; // trailing punctuation, base colour
    QColor color;
    bool bold = false;
    double advance = 0.0;      // prefix + core + suffix
    double prefixAdvance = 0.0;
    double coreAdvance = 0.0;
    bool hardBreak = false;    // an explicit newline, not a word
};

struct Line {
    int first = 0;
    int count = 0;
    double width = 0.0;
};

struct LayoutResult {
    QRectF box; // final box in canvas pixels
    QList<Token> tokens;
    QList<Line> lines;
    double lineHeight = 0.0;
    double spaceAdvance = 0.0;
    QFont font;
    QFont boldFont;
};

// Splits `text` into words, resolving each against the project's highlight
// table so a highlighted word keeps its colour wherever it appears.
QList<Token> buildTokens(const QString& text, const SubtitleStyle& style,
                         const QList<Highlight>& highlights, const QFontMetricsF& fm,
                         const QFontMetricsF& fmBold) {
    QList<Token> tokens;
    const QStringList paragraphs = text.split(QLatin1Char('\n'));

    for (int para = 0; para < paragraphs.size(); ++para) {
        if (para > 0) {
            Token br;
            br.hardBreak = true;
            tokens.append(br);
        }
        const QStringList words = paragraphs.at(para).split(
            QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
        for (const QString& word : words) {
            Token t;
            t.core = word;
            t.color = style.textColor;

            const QString key = normaliseWord(word);
            const Highlight* match = nullptr;
            if (!key.isEmpty()) {
                for (const Highlight& h : highlights) {
                    if (normaliseWord(h.word) == key) {
                        match = &h;
                        break;
                    }
                }
            }

            if (match) {
                t.color = match->color;
                t.bold = match->bold;

                // Peel off punctuation so only the letters take the colour.
                int begin = 0;
                int end = word.size();
                while (begin < end && !word.at(begin).isLetterOrNumber()) ++begin;
                while (end > begin && !word.at(end - 1).isLetterOrNumber()) --end;
                t.prefix = word.left(begin);
                t.core = word.mid(begin, end - begin);
                t.suffix = word.mid(end);
            }

            const QFontMetricsF& coreFm = t.bold ? fmBold : fm;
            t.prefixAdvance = t.prefix.isEmpty() ? 0.0 : fm.horizontalAdvance(t.prefix);
            t.coreAdvance = coreFm.horizontalAdvance(t.core);
            const double suffixAdvance =
                t.suffix.isEmpty() ? 0.0 : fm.horizontalAdvance(t.suffix);
            t.advance = t.prefixAdvance + t.coreAdvance + suffixAdvance;

            tokens.append(t);
        }
    }
    return tokens;
}

// Greedy wrap over tokens. A single word wider than the box overflows rather
// than being broken mid-word.
QList<Line> wrapTokens(const QList<Token>& tokens, double maxWidth, double spaceAdvance) {
    QList<Line> lines;
    Line current;
    current.first = 0;

    auto flush = [&] {
        if (current.count > 0 || lines.isEmpty()) lines.append(current);
    };

    for (int i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens.at(i);
        if (t.hardBreak) {
            flush();
            current = Line{i + 1, 0, 0.0};
            continue;
        }

        const double extra = current.count == 0 ? t.advance : spaceAdvance + t.advance;
        if (current.count > 0 && current.width + extra > maxWidth) {
            lines.append(current);
            current = Line{i, 1, t.advance};
        } else {
            if (current.count == 0) current.first = i;
            current.count++;
            current.width += extra;
        }
    }
    flush();
    return lines;
}

LayoutResult layoutSubtitle(const SubtitleStyle& style, const QString& text,
                            const QList<Highlight>& highlights, const QSize& canvas) {
    LayoutResult out;
    out.font = buildFont(style, canvas);
    out.boldFont = out.font;
    out.boldFont.setWeight(
        static_cast<QFont::Weight>(std::clamp(std::max(style.fontWeight + 300, 700), 1, 1000)));

    const QFontMetricsF fm(out.font);
    const QFontMetricsF fmBold(out.boldFont);
    out.spaceAdvance = fm.horizontalAdvance(QLatin1Char(' '));

    const double padX = style.paddingX * styleScale(canvas);
    const double padY = style.paddingY * styleScale(canvas);

    QRectF box(style.box.x() * canvas.width(), style.box.y() * canvas.height(),
               style.box.width() * canvas.width(), style.box.height() * canvas.height());

    const double innerWidth = std::max(1.0, box.width() - 2 * padX);
    out.tokens = buildTokens(text, style, highlights, fm, fmBold);
    out.lines = wrapTokens(out.tokens, innerWidth, out.spaceAdvance);
    out.lineHeight = fm.height() * style.lineSpacing;

    if (style.autoHeight) {
        const double needed = out.lines.size() * out.lineHeight + 2 * padY;
        const double centerY = box.center().y();
        box.setHeight(needed);
        box.moveTop(centerY - needed / 2.0);
    }

    out.box = box;
    return out;
}

void drawTextLines(QPainter& p, const LayoutResult& layout, const SubtitleStyle& style,
                   const QSize& canvas) {
    if (layout.lines.isEmpty()) return;

    const QFontMetricsF fm(layout.font);
    const double padX = style.paddingX * styleScale(canvas);
    const double padY = style.paddingY * styleScale(canvas);
    const QRectF inner = layout.box.adjusted(padX, padY, -padX, -padY);

    const auto align = static_cast<Qt::Alignment>(style.textAlign);
    const double blockHeight = layout.lines.size() * layout.lineHeight;

    double top = inner.top();
    if (align & Qt::AlignVCenter) {
        top = inner.top() + (inner.height() - blockHeight) / 2.0;
    } else if (align & Qt::AlignBottom) {
        top = inner.bottom() - blockHeight;
    }

    const double outlineWidth = style.outlineWidth * styleScale(canvas);
    const double shadowOffset = style.shadowOffset * styleScale(canvas);

    for (int i = 0; i < layout.lines.size(); ++i) {
        const Line& line = layout.lines.at(i);
        if (line.count == 0) continue;

        double x = inner.left();
        if (align & Qt::AlignHCenter) {
            x = inner.left() + (inner.width() - line.width) / 2.0;
        } else if (align & Qt::AlignRight) {
            x = inner.right() - line.width;
        }

        const double baseline = top + i * layout.lineHeight + fm.ascent();

        for (int t = line.first; t < line.first + line.count; ++t) {
            const Token& token = layout.tokens.at(t);

            // Draws one run of the token at `runX` and returns nothing; the
            // caller advances x itself so the pieces stay glued together.
            const auto drawRun = [&](const QString& text, double runX, const QFont& font,
                                     const QColor& color) {
                if (text.isEmpty()) return;

                if (style.shadowEnabled) {
                    QPainterPath path;
                    path.addText(QPointF(runX + shadowOffset, baseline + shadowOffset), font, text);
                    p.fillPath(path, style.shadowColor);
                }

                QPainterPath path;
                path.addText(QPointF(runX, baseline), font, text);

                if (style.outlineEnabled && outlineWidth > 0.0) {
                    QPen pen(style.outlineColor, outlineWidth * 2.0);
                    pen.setJoinStyle(Qt::RoundJoin);
                    p.setPen(pen);
                    p.setBrush(Qt::NoBrush);
                    p.drawPath(path);
                }

                p.fillPath(path, color);
            };

            const QFont& coreFont = token.bold ? layout.boldFont : layout.font;
            drawRun(token.prefix, x, layout.font, style.textColor);
            drawRun(token.core, x + token.prefixAdvance, coreFont, token.color);
            drawRun(token.suffix, x + token.prefixAdvance + token.coreAdvance, layout.font,
                    style.textColor);

            x += token.advance + layout.spaceAdvance;
        }
    }
}

// Paints one styled caption box - fill, then word-coloured text. Shared by the
// speaker caption, the translation line, and text overlays, so all three
// behave identically.
void drawStyledBox(QPainter& p, const SubtitleStyle& style, const QString& text,
                   const QList<Highlight>& highlights, const QSize& canvas,
                   double alpha = 1.0, double yOffset = 0.0) {
    if (text.isEmpty() || alpha <= 0.002) return;

    // One opacity for the whole box means the fill and the text fade together;
    // fading them separately would let the text ghost over a solid panel.
    const double previousOpacity = p.opacity();
    const bool transformed = std::abs(yOffset) > 0.01;
    if (alpha < 0.998) p.setOpacity(previousOpacity * alpha);
    if (transformed) {
        p.save();
        p.translate(0.0, yOffset);
    }

    const LayoutResult layout = layoutSubtitle(style, text, highlights, canvas);

    QColor fill = style.boxColor;
    fill.setAlphaF(fill.alphaF() * std::clamp(style.boxOpacity, 0.0, 1.0));

    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    const double radius = style.cornerRadius * styleScale(canvas);
    if (radius > 0.5) {
        p.drawRoundedRect(layout.box, radius, radius);
    } else {
        p.drawRect(layout.box);
    }

    drawTextLines(p, layout, style, canvas);

    if (transformed) p.restore();
    p.setOpacity(previousOpacity);
}

} // namespace

QRectF FrameRenderer::subtitleBoxRect(const SubtitleStyle& style, const QString& text,
                                      const QList<Highlight>& highlights, const QSize& canvas) {
    return layoutSubtitle(style, text, highlights, canvas).box;
}

void FrameRenderer::clearCache() { cache_.clear(); }

QImage FrameRenderer::sceneImage(const Scene& scene, const QSize& canvas) const {
    const QString key = QStringLiteral("%1|%2x%3|%4")
                            .arg(scene.imagePath)
                            .arg(canvas.width())
                            .arg(canvas.height())
                            .arg(static_cast<int>(scene.fit));
    const auto it = cache_.constFind(key);
    if (it != cache_.constEnd()) return it.value();

    QImage source(scene.imagePath);
    if (source.isNull()) {
        cache_.insert(key, QImage());
        return {};
    }

    const Qt::AspectRatioMode mode = scene.fit == ImageFit::Cover
                                         ? Qt::KeepAspectRatioByExpanding
                                         : Qt::KeepAspectRatio;
    QImage scaled = source.scaled(canvas, mode, Qt::SmoothTransformation);

    // Cover: centre-crop the overflow so the result is exactly canvas-sized.
    if (scene.fit == ImageFit::Cover && scaled.size() != canvas) {
        const int x = (scaled.width() - canvas.width()) / 2;
        const int y = (scaled.height() - canvas.height()) / 2;
        scaled = scaled.copy(x, y, canvas.width(), canvas.height());
    }

    // Keep the cache from growing without bound when scrubbing a long project.
    if (cache_.size() > 24) cache_.clear();
    cache_.insert(key, scaled);
    return scaled;
}

QImage FrameRenderer::overlayImage(const Overlay& overlay, const QSize& target) const {
    if (target.isEmpty()) return {};

    const QString key = QStringLiteral("ov|%1|%2x%3|%4")
                            .arg(overlay.imagePath)
                            .arg(target.width())
                            .arg(target.height())
                            .arg(overlay.keepAspect ? 1 : 0);
    const auto it = cache_.constFind(key);
    if (it != cache_.constEnd()) return it.value();

    QImage source(overlay.imagePath);
    if (source.isNull()) {
        cache_.insert(key, QImage());
        return {};
    }

    const QImage scaled = source.scaled(
        target, overlay.keepAspect ? Qt::KeepAspectRatio : Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation);

    if (cache_.size() > 32) cache_.clear();
    cache_.insert(key, scaled);
    return scaled;
}

void FrameRenderer::drawOverlays(QPainter& p, const Project& project, qint64 timeMs,
                                 bool onTop) const {
    for (const Overlay& overlay : project.overlays) {
        if (overlay.onTop != onTop) continue;
        const double fade = overlay.alphaAt(timeMs, project.transitionMs);
        if (fade <= 0.002) continue;

        const QRectF rect(overlay.style.box.x() * project.canvas.width(),
                          overlay.style.box.y() * project.canvas.height(),
                          overlay.style.box.width() * project.canvas.width(),
                          overlay.style.box.height() * project.canvas.height());
        if (rect.width() < 1.0 || rect.height() < 1.0) continue;

        const double previous = p.opacity();
        p.setOpacity(previous * std::clamp(overlay.opacity, 0.0, 1.0) * fade);

        if (overlay.isImage) {
            const QImage image = overlayImage(overlay, rect.size().toSize());
            if (!image.isNull()) {
                // Keep-aspect images end up smaller than the box; centre them
                // inside it so the handle you dragged still frames the logo.
                const QPointF at(rect.left() + (rect.width() - image.width()) / 2.0,
                                 rect.top() + (rect.height() - image.height()) / 2.0);
                p.drawImage(at, image);
            }
        } else {
            drawStyledBox(p, overlay.style, overlay.text, project.highlights, project.canvas);
        }

        p.setOpacity(previous);
    }
}

QImage FrameRenderer::renderFrame(const Project& project, qint64 timeMs) const {
    QImage frame(project.canvas, QImage::Format_ARGB32_Premultiplied);
    frame.fill(project.backgroundColor);

    QPainter p(&frame);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                     QPainter::SmoothPixmapTransform);

    const int sceneIndex = project.sceneAt(timeMs);
    if (sceneIndex >= 0) {
        const QImage image = sceneImage(project.scenes.at(sceneIndex), project.canvas);
        if (!image.isNull()) {
            // Contain-fitted images are smaller than the canvas; centre them.
            const QPoint at((project.canvas.width() - image.width()) / 2,
                            (project.canvas.height() - image.height()) / 2);
            p.drawImage(at, image);
        }
    }

    drawOverlays(p, project, timeMs, /*onTop=*/false);

    // Usually one caption; two while a hand-over is in progress, with their
    // opacities summing to one so the join never flickers or dips.
    const double rise = project.transitionRise * styleScale(project.canvas);
    for (const Project::ActiveSegment& active : project.activeSegmentsAt(timeMs)) {
        const Segment& segment = project.segments.at(active.index);
        // Slide up into place as it fades in, and back down as it leaves.
        const double yOffset = rise * (1.0 - active.alpha);

        // An unassigned line still has to be readable - fall back to the first
        // speaker's look rather than dropping the caption.
        const Speaker* speaker = project.speakerFor(segment);
        if (!speaker && !project.speakers.isEmpty()) speaker = &project.speakers.first();
        if (speaker) {
            drawStyledBox(p, speaker->style, segment.text, project.highlights, project.canvas,
                          active.alpha, yOffset);
        }
        // The translation is part of the same caption, so it fades with it.
        if (project.translationEnabled && !segment.translation.isEmpty()) {
            drawStyledBox(p, project.translationStyle, segment.translation, project.highlights,
                          project.canvas, active.alpha, yOffset);
        }
    }

    drawOverlays(p, project, timeMs, /*onTop=*/true);

    p.end();
    return frame;
}

} // namespace dvs
