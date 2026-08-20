#pragma once

#include <QDialog>

#include "core/Project.h"

class QPushButton;
class QTableWidget;

namespace dvs {

// Manages the project's word -> colour table. Edits apply immediately so the
// preview updates behind the dialog.
class HighlightDialog : public QDialog {
    Q_OBJECT

public:
    HighlightDialog(Project* project, QWidget* parent = nullptr);

    // Scrolls to and selects `word`, adding a row for it if it is not listed
    // yet. Used by the "colour this word" shortcut in the segment table.
    void focusWord(const QString& word);

signals:
    void highlightsChanged();

private:
    void reload();
    void addRow(int index);
    void addWord();
    void removeSelected();
    void suggestFromSubtitles();

    Project* project_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    bool updating_ = false;
};

// Prompts for a colour and adds/updates the highlight for `word`. Returns true
// if the project was changed.
bool promptHighlightColor(QWidget* parent, Project* project, const QString& word);

} // namespace dvs
