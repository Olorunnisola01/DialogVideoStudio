#pragma once

#include <QDialog>
#include <QFutureWatcher>

#include "render/VideoStitcher.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QComboBox;

namespace dvs {

// Builds one long video out of several finished batches: saved projects, which
// are rendered here, and videos that already exist.
class StitchDialog : public QDialog {
    Q_OBJECT

public:
    // `currentProject` is offered as the first part when it has been saved.
    explicit StitchDialog(const QString& currentProjectPath, QWidget* parent = nullptr);
    ~StitchDialog() override;

private:
    void addParts(bool projects);
    void move(int delta);
    void removeSelected();
    void chooseOutput();
    void start();
    void onFinished();
    void setRunning(bool running);
    void refreshList();

    QList<StitchItem> items_;

    QListWidget* list_ = nullptr;
    QLineEdit* outputEdit_ = nullptr;
    QComboBox* sizeCombo_ = nullptr;
    QSpinBox* fps_ = nullptr;
    QSpinBox* crf_ = nullptr;
    QPushButton* buildButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;

    VideoStitcher* stitcher_ = nullptr;
    QFutureWatcher<StitchReport>* watcher_ = nullptr;
};

} // namespace dvs
