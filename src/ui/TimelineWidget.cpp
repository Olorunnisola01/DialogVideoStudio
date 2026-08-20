#include "TimelineWidget.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QImageReader>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace dvs {

namespace {
constexpr double kEdgeGrabPx = 5.0;
constexpr int kSceneRowHeight = 34;
constexpr int kSpeakerRowHeight = 18;

bool looksLikeImage(const QString& path) {
    return !QImageReader::imageFormat(path).isEmpty();
}
} // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(140);
    setAcceptDrops(true);
    setMouseTracking(true);
}

void TimelineWidget::setProject(Project* project) {
    project_ = project;
    selectedScene_ = -1;
    update();
}

void TimelineWidget::setPeaks(const QList<PeakBucket>& peaks) {
    peaks_ = peaks;
    update();
}

void TimelineWidget::setTime(qint64 ms) {
    if (timeMs_ == ms) return;
    timeMs_ = ms;
    update();
}

qint64 TimelineWidget::duration() const {
    if (!project_) return 0;
    qint64 d = project_->durationMs;
    for (const Segment& s : project_->segments) d = std::max(d, s.endMs);
    for (const Scene& s : project_->scenes) d = std::max(d, s.endMs);
    return std::max<qint64>(d, 1);
}

qint64 TimelineWidget::xToTime(double x) const {
    const double w = std::max(1, width());
    return static_cast<qint64>(std::clamp(x / w, 0.0, 1.0) * duration());
}

double TimelineWidget::timeToX(qint64 ms) const {
    return static_cast<double>(ms) / duration() * width();
}

QRectF TimelineWidget::waveRect() const {
    return QRectF(0, 0, width(), height() - kSceneRowHeight - kSpeakerRowHeight);
}

QRectF TimelineWidget::speakerRect() const {
    return QRectF(0, height() - kSceneRowHeight - kSpeakerRowHeight, width(), kSpeakerRowHeight);
}

QRectF TimelineWidget::sceneRect() const {
    return QRectF(0, height() - kSceneRowHeight, width(), kSceneRowHeight);
}

int TimelineWidget::sceneAtX(double x) const {
    if (!project_) return -1;
    const qint64 t = xToTime(x);
    for (int i = project_->scenes.size() - 1; i >= 0; --i) {
        if (project_->scenes.at(i).containsTime(t)) return i;
    }
    return -1;
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x24, 0x24, 0x28));
    if (!project_) return;

    // --- waveform ---
    const QRectF wave = waveRect();
    p.fillRect(wave, QColor(0x1B, 0x1B, 0x1F));
    if (!peaks_.isEmpty()) {
        p.setPen(QColor(0x5A, 0xC8, 0xFA));
        const double mid = wave.center().y();
        const double half = wave.height() / 2.0 - 2.0;
        for (int x = 0; x < static_cast<int>(wave.width()); ++x) {
            const int bucket = static_cast<int>(
                static_cast<qint64>(x) * peaks_.size() / std::max(1, static_cast<int>(wave.width())));
            if (bucket < 0 || bucket >= peaks_.size()) continue;
            const PeakBucket& b = peaks_.at(bucket);
            p.drawLine(QPointF(x, mid - b.max * half), QPointF(x, mid - b.min * half));
        }
    } else {
        p.setPen(QColor(0x77, 0x77, 0x80));
        p.drawText(wave, Qt::AlignCenter, QStringLiteral("Load an audio file to see the waveform"));
    }

    // --- speaker strip ---
    const QRectF speakers = speakerRect();
    p.fillRect(speakers, QColor(0x2A, 0x2A, 0x30));
    for (const Segment& s : project_->segments) {
        const Speaker* sp = project_->speakerFor(s);
        const double x0 = timeToX(s.startMs);
        const double x1 = timeToX(s.endMs);
        QColor c = sp ? sp->tint : QColor(0x66, 0x66, 0x66);
        if (s.needsReview) c = QColor(0xE0, 0x8A, 0x24);
        p.fillRect(QRectF(x0, speakers.top() + 3, std::max(1.0, x1 - x0), speakers.height() - 6), c);
    }

    // --- scene strip ---
    const QRectF scenes = sceneRect();
    p.fillRect(scenes, QColor(0x2A, 0x2A, 0x30));
    if (project_->scenes.isEmpty()) {
        p.setPen(QColor(0x77, 0x77, 0x80));
        p.drawText(scenes, Qt::AlignCenter,
                   QStringLiteral("Drop scene images here"));
    }
    for (int i = 0; i < project_->scenes.size(); ++i) {
        const Scene& s = project_->scenes.at(i);
        const QRectF r(timeToX(s.startMs), scenes.top() + 2,
                       std::max(2.0, timeToX(s.endMs) - timeToX(s.startMs)),
                       scenes.height() - 4);
        p.fillRect(r, i == selectedScene_ ? QColor(0x3C, 0x6E, 0xA8) : QColor(0x3A, 0x3A, 0x44));
        p.setPen(QColor(0x88, 0x88, 0x94));
        p.drawRect(r);
        p.setPen(QColor(0xDD, 0xDD, 0xE4));
        p.drawText(r.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(QFileInfo(s.imagePath).fileName(),
                                              Qt::ElideMiddle, static_cast<int>(r.width()) - 8));
    }

    // --- playhead ---
    const double x = timeToX(timeMs_);
    p.setPen(QPen(QColor(0xFF, 0x53, 0x53), 1.5));
    p.drawLine(QPointF(x, 0), QPointF(x, height()));
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (!project_ || event->button() != Qt::LeftButton) return;

    const QPointF pos = event->position();
    if (sceneRect().contains(pos)) {
        const int index = sceneAtX(pos.x());
        selectedScene_ = index;
        emit sceneSelected(index);
        if (index >= 0) {
            const Scene& s = project_->scenes.at(index);
            dragScene_ = index;
            if (std::abs(pos.x() - timeToX(s.startMs)) <= kEdgeGrabPx) {
                drag_ = Drag::SceneStart;
            } else if (std::abs(pos.x() - timeToX(s.endMs)) <= kEdgeGrabPx) {
                drag_ = Drag::SceneEnd;
            } else {
                drag_ = Drag::SceneMove;
                dragGrabOffsetMs_ = xToTime(pos.x()) - s.startMs;
            }
            update();
            return;
        }
        update();
    }

    drag_ = Drag::Seek;
    emit seeked(xToTime(pos.x()));
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();

    if (drag_ == Drag::None) {
        if (project_ && sceneRect().contains(pos)) {
            const int index = sceneAtX(pos.x());
            if (index >= 0) {
                const Scene& s = project_->scenes.at(index);
                const bool onEdge = std::abs(pos.x() - timeToX(s.startMs)) <= kEdgeGrabPx ||
                                    std::abs(pos.x() - timeToX(s.endMs)) <= kEdgeGrabPx;
                setCursor(onEdge ? Qt::SizeHorCursor : Qt::OpenHandCursor);
                return;
            }
        }
        unsetCursor();
        return;
    }

    if (drag_ == Drag::Seek) {
        emit seeked(xToTime(pos.x()));
        return;
    }

    if (!project_ || dragScene_ < 0 || dragScene_ >= project_->scenes.size()) return;
    Scene& s = project_->scenes[dragScene_];
    const qint64 t = xToTime(pos.x());
    constexpr qint64 kMinSceneMs = 200;

    switch (drag_) {
    case Drag::SceneStart:
        s.startMs = std::clamp<qint64>(t, 0, s.endMs - kMinSceneMs);
        break;
    case Drag::SceneEnd:
        s.endMs = std::clamp<qint64>(t, s.startMs + kMinSceneMs, duration());
        break;
    case Drag::SceneMove: {
        const qint64 length = s.endMs - s.startMs;
        qint64 start = std::clamp<qint64>(t - dragGrabOffsetMs_, 0, duration() - length);
        s.startMs = start;
        s.endMs = start + length;
        break;
    }
    default:
        break;
    }
    update();
    emit scenesChanged();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ != Drag::None && drag_ != Drag::Seek && project_) {
        project_->normalise();
        emit scenesChanged();
    }
    drag_ = Drag::None;
    dragScene_ = -1;
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && looksLikeImage(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    if (!project_) return;

    QStringList images;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (looksLikeImage(path)) images << path;
    }
    if (images.isEmpty()) return;

    // Dropped onto empty space: lay the batch out evenly across the timeline.
    // Dropped onto the strip with scenes already there: insert at the cursor,
    // taking three seconds each.
    if (project_->scenes.isEmpty()) {
        for (const QString& path : images) {
            Scene s;
            s.imagePath = path;
            project_->scenes.append(s);
        }
        project_->autoLayoutScenes();
    } else {
        qint64 at = xToTime(event->position().x());
        for (const QString& path : images) {
            Scene s;
            s.imagePath = path;
            s.startMs = at;
            s.endMs = std::min(at + 3000, duration());
            project_->scenes.append(s);
            at = s.endMs;
        }
        project_->normalise();
    }

    event->acceptProposedAction();
    update();
    emit scenesChanged();
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (!project_ || !sceneRect().contains(event->pos())) return;
    const int index = sceneAtX(event->pos().x());
    if (index < 0) return;

    QMenu menu(this);
    QAction* fitAll = menu.addAction(QStringLiteral("Stretch over the whole video"));
    QAction* spread = menu.addAction(QStringLiteral("Spread all scenes evenly"));
    menu.addSeparator();
    QAction* remove = menu.addAction(QStringLiteral("Remove this scene"));

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == remove) {
        project_->scenes.removeAt(index);
        selectedScene_ = -1;
    } else if (chosen == fitAll) {
        project_->scenes[index].startMs = 0;
        project_->scenes[index].endMs = duration();
    } else if (chosen == spread) {
        project_->durationMs = std::max(project_->durationMs, duration());
        project_->autoLayoutScenes();
    } else {
        return;
    }

    project_->normalise();
    update();
    emit scenesChanged();
}

} // namespace dvs
