#pragma once

#include <QWidget>

#include "core/Project.h"

class QComboBox;
class QLineEdit;
class QPushButton;

namespace dvs {

class StyleEditor;

// Picks a speaker and edits their caption look.
class SpeakerStylePanel : public QWidget {
    Q_OBJECT

public:
    explicit SpeakerStylePanel(QWidget* parent = nullptr);

    void setProject(Project* project);
    void setSpeaker(int speakerId);
    int speakerId() const { return speakerId_; }

    // Re-reads the model (after diarization added speakers, or a project load).
    void reload();

    // Re-reads only the current speaker's values, e.g. after the box was
    // dragged in the preview.
    void refreshValues();

signals:
    void styleChanged();
    void speakersChanged();

private:
    Speaker* current() const;
    void bind();

    Project* project_ = nullptr;
    int speakerId_ = 0;
    bool updating_ = false;

    QComboBox* speakerCombo_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPushButton* tintButton_ = nullptr;
    QPushButton* addButton_ = nullptr;
    StyleEditor* editor_ = nullptr;
};

} // namespace dvs
