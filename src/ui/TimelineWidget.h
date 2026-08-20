#pragma once

#include <QList>
#include <QWidget>

#include "core/AudioDecoder.h"
#include "core/Project.h"

namespace dvs {

// Waveform + speaker strip + scene strip, with a playhead. Click to seek, drag
// scene edges to retime them, drop image files to add scenes.
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setProject(Project* project);
    void setPeaks(const QList<PeakBucket>& peaks);
    void setTime(qint64 ms);

    int selectedScene() const { return selectedScene_; }

signals:
    void seeked(qint64 ms);
    void scenesChanged();
    void sceneSelected(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    enum class Drag { None, Seek, SceneStart, SceneEnd, SceneMove };

    qint64 duration() const;
    qint64 xToTime(double x) const;
    double timeToX(qint64 ms) const;
    QRectF waveRect() const;
    QRectF speakerRect() const;
    QRectF sceneRect() const;
    int sceneAtX(double x) const;

    Project* project_ = nullptr;
    QList<PeakBucket> peaks_;
    qint64 timeMs_ = 0;

    Drag drag_ = Drag::None;
    int dragScene_ = -1;
    qint64 dragGrabOffsetMs_ = 0;
    int selectedScene_ = -1;
};

} // namespace dvs
