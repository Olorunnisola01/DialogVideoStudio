#pragma once

#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>

#include "core/Project.h"

class QPainter;

namespace dvs {

// Draws one video frame. The preview widget and the exporter both go through
// here, so what you position on screen is exactly what lands in the MP4.
//
// Frames are always composed at Project::canvas resolution; callers that need
// a smaller picture (the preview, thumbnails) scale the result down.
class FrameRenderer {
public:
    QImage renderFrame(const Project& project, qint64 timeMs) const;

    // Paints one segment's subtitle box onto an existing painter. Exposed so
    // the preview can draw selection handles in the same coordinate system.
    static QRectF subtitleBoxRect(const SubtitleStyle& style, const QString& text,
                                  const QList<Highlight>& highlights, const QSize& canvas);

    void clearCache();

private:
    QImage sceneImage(const Scene& scene, const QSize& canvas) const;
    QImage overlayImage(const Overlay& overlay, const QSize& target) const;
    void drawOverlays(QPainter& p, const Project& project, qint64 timeMs, bool onTop) const;

    mutable QHash<QString, QImage> cache_;
};

} // namespace dvs
