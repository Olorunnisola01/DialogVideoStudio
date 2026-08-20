#pragma once

#include <QWidget>

#include "core/Project.h"

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace dvs {

class StyleEditor;

// The layer of things pinned over the video: logo, subscribe button, title
// banner, watermark. Each entry is positioned by dragging it in the preview or
// by typing its rectangle here.
class OverlayPanel : public QWidget {
    Q_OBJECT

public:
    explicit OverlayPanel(QWidget* parent = nullptr);

    void setProject(Project* project);
    void reload();
    void refreshValues();
    void selectOverlay(int index);
    int selectedOverlay() const;

signals:
    void overlaysChanged();
    void selectionChanged(int index);

private:
    Overlay* current() const;
    void bind();
    void addImageOverlay(const QString& presetName);
    void addTextOverlay();

    Project* project_ = nullptr;
    bool updating_ = false;

    QListWidget* list_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* sourceEdit_ = nullptr;   // image path or overlay text
    QPushButton* browseButton_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    QCheckBox* keepAspect_ = nullptr;
    QCheckBox* onTop_ = nullptr;
    QCheckBox* enabled_ = nullptr;
    QCheckBox* wholeVideo_ = nullptr;
    QSpinBox* startSec_ = nullptr;
    QSpinBox* endSec_ = nullptr;
    StyleEditor* editor_ = nullptr;
};

} // namespace dvs
