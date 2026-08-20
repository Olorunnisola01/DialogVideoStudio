#pragma once

#include <QImage>
#include <QRectF>
#include <QWidget>

#include "core/Project.h"
#include "render/FrameRenderer.h"

namespace dvs {

// Shows the frame at the current time and lets any box on it be dragged and
// resized: the speaker's caption, the English line, or an overlay. Editing a
// caption box edits the *speaker's* style, so positioning it once sets the look
// for every line that speaker says.
class PreviewCanvas : public QWidget {
    Q_OBJECT

public:
    // What a drag is currently acting on.
    enum class Target { None, Subtitle, Translation, Overlay };

    explicit PreviewCanvas(QWidget* parent = nullptr);

    void setProject(Project* project);
    void setTime(qint64 ms);
    qint64 time() const { return timeMs_; }

    // Follows the panel the user switched to, so the handles appear on the box
    // they are about to edit.
    void setSelection(Target target, int index = -1);

    // Forces a re-render (styles, scenes or segments changed).
    void invalidate();

signals:
    void styleEdited();
    void speakerActivated(int speakerId);
    void overlayActivated(int index);
    void translationActivated();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class Handle { None, Move, Left, Right, Top, Bottom,
                        TopLeft, TopRight, BottomLeft, BottomRight };

    QRectF viewportRect() const;          // where the canvas is drawn, in widget px
    QPointF toCanvas(const QPointF& widgetPos) const;
    QRectF toWidget(const QRectF& canvasRect) const;

    int activeSegmentIndex() const;
    // The style behind a target, or nullptr when that target is not on screen
    // at the current time.
    SubtitleStyle* styleFor(Target target, int index) const;
    QRectF boxRectFor(Target target, int index) const; // canvas pixels
    Handle handleFor(const QPointF& widgetPos, Target target, int index) const;

    // Topmost box under the cursor, preferring the current selection so a box
    // underneath another can still be grabbed once selected.
    bool pick(const QPointF& widgetPos, Target* target, int* index, Handle* handle) const;

    void applyCursor(Handle handle);

    Project* project_ = nullptr;
    FrameRenderer renderer_;
    QImage frame_;
    bool frameValid_ = false;
    qint64 timeMs_ = 0;

    Target selected_ = Target::Subtitle;
    int selectedIndex_ = -1;

    Handle dragHandle_ = Handle::None;
    Target dragTarget_ = Target::None;
    int dragIndex_ = -1;
    QPointF dragStartCanvas_;
    QRectF dragStartBox_;      // normalised
};

} // namespace dvs
