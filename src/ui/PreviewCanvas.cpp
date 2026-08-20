#include "PreviewCanvas.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace dvs {

namespace {
constexpr double kHandlePx = 9.0; // grab radius in widget pixels
}

PreviewCanvas::PreviewCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    setMouseTracking(true);
    setAutoFillBackground(false);
}

void PreviewCanvas::setProject(Project* project) {
    project_ = project;
    selected_ = Target::Subtitle;
    selectedIndex_ = -1;
    invalidate();
}

void PreviewCanvas::setTime(qint64 ms) {
    if (timeMs_ == ms) return;
    timeMs_ = ms;
    invalidate();
}

void PreviewCanvas::setSelection(Target target, int index) {
    selected_ = target;
    selectedIndex_ = index;
    update();
}

void PreviewCanvas::invalidate() {
    frameValid_ = false;
    update();
}

QRectF PreviewCanvas::viewportRect() const {
    if (!project_ || project_->canvas.isEmpty()) return rect();

    const double aspect = static_cast<double>(project_->canvas.width()) / project_->canvas.height();
    double w = width();
    double h = w / aspect;
    if (h > height()) {
        h = height();
        w = h * aspect;
    }
    return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
}

QPointF PreviewCanvas::toCanvas(const QPointF& widgetPos) const {
    const QRectF vp = viewportRect();
    if (!project_ || vp.width() <= 0 || vp.height() <= 0) return widgetPos;
    return QPointF((widgetPos.x() - vp.left()) / vp.width() * project_->canvas.width(),
                   (widgetPos.y() - vp.top()) / vp.height() * project_->canvas.height());
}

QRectF PreviewCanvas::toWidget(const QRectF& canvasRect) const {
    const QRectF vp = viewportRect();
    if (!project_ || project_->canvas.isEmpty()) return canvasRect;
    const double sx = vp.width() / project_->canvas.width();
    const double sy = vp.height() / project_->canvas.height();
    return QRectF(vp.left() + canvasRect.left() * sx, vp.top() + canvasRect.top() * sy,
                  canvasRect.width() * sx, canvasRect.height() * sy);
}

int PreviewCanvas::activeSegmentIndex() const {
    return project_ ? project_->segmentAt(timeMs_) : -1;
}

SubtitleStyle* PreviewCanvas::styleFor(Target target, int index) const {
    if (!project_) return nullptr;

    switch (target) {
    case Target::Subtitle: {
        const int seg = activeSegmentIndex();
        if (seg < 0) return nullptr;
        int speakerId = project_->segments.at(seg).speakerId;
        if (speakerId < 0 || speakerId >= project_->speakers.size()) speakerId = 0;
        if (project_->speakers.isEmpty()) return nullptr;
        return &project_->speakers[speakerId].style;
    }
    case Target::Translation: {
        const int seg = activeSegmentIndex();
        if (seg < 0 || !project_->translationEnabled) return nullptr;
        if (project_->segments.at(seg).translation.isEmpty()) return nullptr;
        return &project_->translationStyle;
    }
    case Target::Overlay: {
        if (index < 0 || index >= project_->overlays.size()) return nullptr;
        if (!project_->overlays.at(index).visibleAt(timeMs_)) return nullptr;
        return &project_->overlays[index].style;
    }
    case Target::None:
        break;
    }
    return nullptr;
}

QRectF PreviewCanvas::boxRectFor(Target target, int index) const {
    const SubtitleStyle* style = styleFor(target, index);
    if (!style || !project_) return {};
    // The declared box, not the auto-height result: that is what dragging edits.
    return QRectF(style->box.x() * project_->canvas.width(),
                  style->box.y() * project_->canvas.height(),
                  style->box.width() * project_->canvas.width(),
                  style->box.height() * project_->canvas.height());
}

PreviewCanvas::Handle PreviewCanvas::handleFor(const QPointF& pos, Target target,
                                               int index) const {
    const QRectF box = toWidget(boxRectFor(target, index));
    if (box.isEmpty()) return Handle::None;

    const bool nearLeft = std::abs(pos.x() - box.left()) <= kHandlePx;
    const bool nearRight = std::abs(pos.x() - box.right()) <= kHandlePx;
    const bool nearTop = std::abs(pos.y() - box.top()) <= kHandlePx;
    const bool nearBottom = std::abs(pos.y() - box.bottom()) <= kHandlePx;
    const bool insideY = pos.y() >= box.top() - kHandlePx && pos.y() <= box.bottom() + kHandlePx;
    const bool insideX = pos.x() >= box.left() - kHandlePx && pos.x() <= box.right() + kHandlePx;

    if (nearLeft && nearTop) return Handle::TopLeft;
    if (nearRight && nearTop) return Handle::TopRight;
    if (nearLeft && nearBottom) return Handle::BottomLeft;
    if (nearRight && nearBottom) return Handle::BottomRight;
    if (nearLeft && insideY) return Handle::Left;
    if (nearRight && insideY) return Handle::Right;
    if (nearTop && insideX) return Handle::Top;
    if (nearBottom && insideX) return Handle::Bottom;
    if (box.contains(pos)) return Handle::Move;
    return Handle::None;
}

bool PreviewCanvas::pick(const QPointF& pos, Target* target, int* index, Handle* handle) const {
    if (!project_) return false;

    // The current selection wins, so a box lying under another stays grabbable
    // once you have selected it in its panel.
    Handle h = handleFor(pos, selected_, selectedIndex_);
    if (h != Handle::None) {
        *target = selected_;
        *index = selectedIndex_;
        *handle = h;
        return true;
    }

    for (int i = project_->overlays.size() - 1; i >= 0; --i) {
        h = handleFor(pos, Target::Overlay, i);
        if (h != Handle::None) {
            *target = Target::Overlay;
            *index = i;
            *handle = h;
            return true;
        }
    }

    for (Target t : {Target::Translation, Target::Subtitle}) {
        h = handleFor(pos, t, -1);
        if (h != Handle::None) {
            *target = t;
            *index = -1;
            *handle = h;
            return true;
        }
    }

    return false;
}

void PreviewCanvas::applyCursor(Handle handle) {
    switch (handle) {
    case Handle::Move: setCursor(Qt::SizeAllCursor); break;
    case Handle::Left:
    case Handle::Right: setCursor(Qt::SizeHorCursor); break;
    case Handle::Top:
    case Handle::Bottom: setCursor(Qt::SizeVerCursor); break;
    case Handle::TopLeft:
    case Handle::BottomRight: setCursor(Qt::SizeFDiagCursor); break;
    case Handle::TopRight:
    case Handle::BottomLeft: setCursor(Qt::SizeBDiagCursor); break;
    default: unsetCursor(); break;
    }
}

void PreviewCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1E, 0x1E, 0x1E));

    if (!project_) return;

    if (!frameValid_) {
        frame_ = renderer_.renderFrame(*project_, timeMs_);
        frameValid_ = true;
    }

    const QRectF vp = viewportRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(vp, frame_);

    // Selection chrome for whichever box is being edited.
    const QRectF box = toWidget(boxRectFor(selected_, selectedIndex_));
    if (!box.isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(0x2E, 0x9B, 0xFF));
        pen.setWidthF(1.5);
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(box);

        p.setPen(QPen(QColor(0x2E, 0x9B, 0xFF), 1.0));
        p.setBrush(QColor(0xFF, 0xFF, 0xFF));
        const double r = 3.5;
        const QList<QPointF> corners = {
            box.topLeft(), {box.center().x(), box.top()}, box.topRight(),
            {box.left(), box.center().y()}, {box.right(), box.center().y()},
            box.bottomLeft(), {box.center().x(), box.bottom()}, box.bottomRight(),
        };
        for (const QPointF& c : corners) p.drawEllipse(c, r, r);
    }
}

void PreviewCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !project_) return;

    Target target = Target::None;
    int index = -1;
    Handle handle = Handle::None;
    if (!pick(event->position(), &target, &index, &handle)) return;

    const SubtitleStyle* style = styleFor(target, index);
    if (!style) return;

    selected_ = target;
    selectedIndex_ = index;
    dragTarget_ = target;
    dragIndex_ = index;
    dragHandle_ = handle;
    dragStartCanvas_ = toCanvas(event->position());
    dragStartBox_ = style->box;

    switch (target) {
    case Target::Overlay:
        emit overlayActivated(index);
        break;
    case Target::Translation:
        emit translationActivated();
        break;
    case Target::Subtitle: {
        const int seg = activeSegmentIndex();
        if (seg >= 0) emit speakerActivated(std::max(0, project_->segments.at(seg).speakerId));
        break;
    }
    case Target::None:
        break;
    }
    update();
}

void PreviewCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (dragHandle_ == Handle::None) {
        Target target = Target::None;
        int index = -1;
        Handle handle = Handle::None;
        applyCursor(pick(event->position(), &target, &index, &handle) ? handle : Handle::None);
        return;
    }

    SubtitleStyle* style = styleFor(dragTarget_, dragIndex_);
    if (!style || !project_) return;

    const QPointF now = toCanvas(event->position());
    const double dx = (now.x() - dragStartCanvas_.x()) / project_->canvas.width();
    const double dy = (now.y() - dragStartCanvas_.y()) / project_->canvas.height();

    QRectF box = dragStartBox_;
    constexpr double kMin = 0.02;

    switch (dragHandle_) {
    case Handle::Move:
        box.moveTo(box.x() + dx, box.y() + dy);
        break;
    case Handle::Left:
        box.setLeft(std::min(box.left() + dx, box.right() - kMin));
        break;
    case Handle::Right:
        box.setRight(std::max(box.right() + dx, box.left() + kMin));
        break;
    case Handle::Top:
        box.setTop(std::min(box.top() + dy, box.bottom() - kMin));
        break;
    case Handle::Bottom:
        box.setBottom(std::max(box.bottom() + dy, box.top() + kMin));
        break;
    case Handle::TopLeft:
        box.setLeft(std::min(box.left() + dx, box.right() - kMin));
        box.setTop(std::min(box.top() + dy, box.bottom() - kMin));
        break;
    case Handle::TopRight:
        box.setRight(std::max(box.right() + dx, box.left() + kMin));
        box.setTop(std::min(box.top() + dy, box.bottom() - kMin));
        break;
    case Handle::BottomLeft:
        box.setLeft(std::min(box.left() + dx, box.right() - kMin));
        box.setBottom(std::max(box.bottom() + dy, box.top() + kMin));
        break;
    case Handle::BottomRight:
        box.setRight(std::max(box.right() + dx, box.left() + kMin));
        box.setBottom(std::max(box.bottom() + dy, box.top() + kMin));
        break;
    case Handle::None:
        break;
    }

    // Keep at least a sliver on screen so a box can never be lost.
    box.moveLeft(std::clamp(box.left(), -box.width() + 0.05, 1.0 - 0.05));
    box.moveTop(std::clamp(box.top(), -box.height() + 0.05, 1.0 - 0.05));

    style->box = box;
    invalidate();
    emit styleEdited();
}

void PreviewCanvas::mouseReleaseEvent(QMouseEvent*) {
    dragHandle_ = Handle::None;
    dragTarget_ = Target::None;
    dragIndex_ = -1;
}

void PreviewCanvas::leaveEvent(QEvent*) {
    if (dragHandle_ == Handle::None) unsetCursor();
}

} // namespace dvs
