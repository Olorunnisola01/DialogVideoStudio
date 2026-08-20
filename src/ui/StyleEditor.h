#pragma once

#include <QWidget>

#include "core/Project.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QGroupBox;
class QPushButton;
class QSpinBox;

namespace dvs {

// Edits one SubtitleStyle. Used for speaker captions, the translation line and
// text overlays, so all three offer exactly the same controls.
class StyleEditor : public QWidget {
    Q_OBJECT

public:
    explicit StyleEditor(QWidget* parent = nullptr);

    // The editor does not own the style; pass nullptr to disable it.
    void setStyle(SubtitleStyle* style);
    void refresh(); // re-read the style, e.g. after dragging the box

    // Image overlays only need the geometry section.
    void setTextSectionsVisible(bool visible);

signals:
    void changed();

private:
    void pushToUi();
    void pullFromUi();
    void pickColor(QPushButton* button, QColor* target);
    static void updateColorButton(QPushButton* button, const QColor& color);

    SubtitleStyle* style_ = nullptr;
    bool updating_ = false;

    QGroupBox* boxGroup_ = nullptr;
    QGroupBox* posGroup_ = nullptr;
    QGroupBox* textGroup_ = nullptr;
    QGroupBox* fxGroup_ = nullptr;

    QPushButton* boxColorButton_ = nullptr;
    QPushButton* textColorButton_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    QDoubleSpinBox* radius_ = nullptr;
    QDoubleSpinBox* padX_ = nullptr;
    QDoubleSpinBox* padY_ = nullptr;
    QCheckBox* autoHeight_ = nullptr;

    QFontComboBox* fontCombo_ = nullptr;
    QDoubleSpinBox* fontSize_ = nullptr;
    QSpinBox* fontWeight_ = nullptr;
    QCheckBox* italic_ = nullptr;
    QDoubleSpinBox* lineSpacing_ = nullptr;
    QComboBox* alignCombo_ = nullptr;

    QCheckBox* shadow_ = nullptr;
    QPushButton* shadowColorButton_ = nullptr;
    QDoubleSpinBox* shadowOffset_ = nullptr;
    QCheckBox* outline_ = nullptr;
    QPushButton* outlineColorButton_ = nullptr;
    QDoubleSpinBox* outlineWidth_ = nullptr;

    QDoubleSpinBox* boxX_ = nullptr;
    QDoubleSpinBox* boxY_ = nullptr;
    QDoubleSpinBox* boxW_ = nullptr;
    QDoubleSpinBox* boxH_ = nullptr;
};

} // namespace dvs
