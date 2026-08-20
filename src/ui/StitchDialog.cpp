#include "StitchDialog.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace dvs {

namespace {

QSettings appSettings() {
    return QSettings(QStringLiteral("DialogVideoStudio"), QStringLiteral("DialogVideoStudio"));
}

} // namespace

StitchDialog::StitchDialog(const QString& currentProjectPath, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Join batches into one video"));
    resize(660, 560);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "Build each stretch of the video as its own project - its own artwork, caption "
        "positions and overlays - then list them here in order. Saved projects are "
        "rendered now; videos you already have are used as they are."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    list_ = new QListWidget;
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(list_, 1);

    auto* buttons = new QHBoxLayout;
    auto* addProject = new QPushButton(QStringLiteral("Add project..."));
    auto* addVideo = new QPushButton(QStringLiteral("Add video..."));
    auto* up = new QPushButton(QStringLiteral("Up"));
    auto* down = new QPushButton(QStringLiteral("Down"));
    auto* remove = new QPushButton(QStringLiteral("Remove"));
    for (QPushButton* b : {addProject, addVideo, up, down, remove}) buttons->addWidget(b);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* form = new QFormLayout;
    auto* outputRow = new QHBoxLayout;
    outputEdit_ = new QLineEdit;
    auto* browse = new QPushButton(QStringLiteral("Browse..."));
    outputRow->addWidget(outputEdit_, 1);
    outputRow->addWidget(browse);
    form->addRow(QStringLiteral("Save as"), outputRow);

    sizeCombo_ = new QComboBox;
    sizeCombo_->addItem(QStringLiteral("Portrait 1080 x 1920"), QSize(1080, 1920));
    sizeCombo_->addItem(QStringLiteral("Landscape 1920 x 1080"), QSize(1920, 1080));
    sizeCombo_->addItem(QStringLiteral("Square 1080 x 1080"), QSize(1080, 1080));
    sizeCombo_->setToolTip(QStringLiteral(
        "Every part is brought to this shape. Projects are rendered at it; existing "
        "videos of a different shape are fitted inside and padded, never cropped."));
    form->addRow(QStringLiteral("Video size"), sizeCombo_);

    fps_ = new QSpinBox;
    fps_->setRange(1, 120);
    fps_->setValue(30);
    form->addRow(QStringLiteral("Frame rate"), fps_);

    crf_ = new QSpinBox;
    crf_->setRange(0, 51);
    crf_->setValue(18);
    crf_->setToolTip(QStringLiteral("Lower is better quality and a bigger file."));
    form->addRow(QStringLiteral("Quality (CRF)"), crf_);
    layout->addLayout(form);

    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setVisible(false);
    layout->addWidget(progress_);

    status_ = new QLabel;
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    cancelButton_ = new QPushButton(QStringLiteral("Close"));
    buildButton_ = new QPushButton(QStringLiteral("Build video"));
    buildButton_->setDefault(true);
    actions->addWidget(cancelButton_);
    actions->addWidget(buildButton_);
    layout->addLayout(actions);

    // --- state ---
    const int sizeAt = sizeCombo_->findText(
        appSettings().value(QStringLiteral("stitch/size")).toString());
    if (sizeAt >= 0) sizeCombo_->setCurrentIndex(sizeAt);

    if (!currentProjectPath.isEmpty() && QFileInfo::exists(currentProjectPath)) {
        items_.append({currentProjectPath, {}});
        refreshList();
    }

    stitcher_ = new VideoStitcher(this);
    watcher_ = new QFutureWatcher<StitchReport>(this);

    connect(addProject, &QPushButton::clicked, this, [this] { addParts(true); });
    connect(addVideo, &QPushButton::clicked, this, [this] { addParts(false); });
    connect(up, &QPushButton::clicked, this, [this] { move(-1); });
    connect(down, &QPushButton::clicked, this, [this] { move(1); });
    connect(remove, &QPushButton::clicked, this, &StitchDialog::removeSelected);
    connect(browse, &QPushButton::clicked, this, &StitchDialog::chooseOutput);
    connect(buildButton_, &QPushButton::clicked, this, &StitchDialog::start);
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        if (watcher_->isRunning()) {
            stitcher_->cancel();
            status_->setText(QStringLiteral("Stopping..."));
        } else {
            accept();
        }
    });

    connect(stitcher_, &VideoStitcher::progress, this, [this](int pct) {
        progress_->setValue(pct);
    });
    connect(stitcher_, &VideoStitcher::status, this, [this](const QString& s) {
        status_->setText(s);
    });
    connect(watcher_, &QFutureWatcher<StitchReport>::finished, this, &StitchDialog::onFinished);
}

StitchDialog::~StitchDialog() {
    if (watcher_->isRunning()) {
        stitcher_->cancel();
        watcher_->waitForFinished();
    }
}

void StitchDialog::refreshList() {
    list_->clear();
    for (int i = 0; i < items_.size(); ++i) {
        const StitchItem& item = items_.at(i);
        list_->addItem(QStringLiteral("%1.  %2   [%3]")
                           .arg(i + 1)
                           .arg(QFileInfo(item.path).fileName(),
                                item.isProject() ? QStringLiteral("project - will be rendered")
                                                 : QStringLiteral("video")));
    }
}

void StitchDialog::addParts(bool projects) {
    const QString key = projects ? QStringLiteral("dir/project") : QStringLiteral("dir/export");
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        projects ? QStringLiteral("Add projects") : QStringLiteral("Add videos"),
        appSettings().value(key, QDir::homePath()).toString(),
        projects ? QStringLiteral("Dialog Video Studio project (*.dvsproj)")
                 : QStringLiteral("Video (*.mp4 *.mov *.mkv *.avi *.webm);;All files (*)"));
    if (paths.isEmpty()) return;

    appSettings().setValue(key, QFileInfo(paths.first()).absolutePath());
    for (const QString& path : paths) items_.append({path, {}});
    refreshList();
}

void StitchDialog::move(int delta) {
    const int row = list_->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= items_.size()) return;
    items_.swapItemsAt(row, target);
    refreshList();
    list_->setCurrentRow(target);
}

void StitchDialog::removeSelected() {
    QList<int> rows;
    for (const QModelIndex& index : list_->selectionModel()->selectedIndexes()) {
        rows << index.row();
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        if (row >= 0 && row < items_.size()) items_.removeAt(row);
    }
    refreshList();
}

void StitchDialog::chooseOutput() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save the joined video"),
        outputEdit_->text().isEmpty()
            ? appSettings().value(QStringLiteral("dir/export"), QDir::homePath()).toString()
            : outputEdit_->text(),
        QStringLiteral("MP4 video (*.mp4)"));
    if (!path.isEmpty()) outputEdit_->setText(path);
}

void StitchDialog::setRunning(bool running) {
    buildButton_->setEnabled(!running);
    list_->setEnabled(!running);
    progress_->setVisible(running);
    cancelButton_->setText(running ? QStringLiteral("Stop") : QStringLiteral("Close"));
}

void StitchDialog::start() {
    if (items_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to join"),
                                 QStringLiteral("Add at least one project or video first."));
        return;
    }
    if (outputEdit_->text().trimmed().isEmpty()) {
        chooseOutput();
        if (outputEdit_->text().trimmed().isEmpty()) return;
    }

    StitchSettings settings;
    settings.outputPath = outputEdit_->text().trimmed();
    settings.canvas = sizeCombo_->currentData().toSize();
    settings.fps = fps_->value();
    settings.crf = crf_->value();
    appSettings().setValue(QStringLiteral("stitch/size"), sizeCombo_->currentText());

    stitcher_->reset();
    setRunning(true);
    progress_->setValue(0);
    status_->setText(QStringLiteral("Starting..."));

    const QList<StitchItem> items = items_;
    watcher_->setFuture(
        QtConcurrent::run([this, items, settings] { return stitcher_->run(items, settings); }));
}

void StitchDialog::onFinished() {
    setRunning(false);
    const StitchReport report = watcher_->result();

    if (!report.ok()) {
        status_->setText(QStringLiteral("Failed."));
        QMessageBox::warning(this, QStringLiteral("Could not build the video"), report.error);
        return;
    }

    status_->setText(QStringLiteral("Done - %1 part(s)%2.")
                         .arg(report.parts)
                         .arg(report.streamCopied
                                  ? QStringLiteral(", joined without re-compressing")
                                  : QStringLiteral(", re-encoded to line the parts up")));
    QMessageBox::information(this, QStringLiteral("Video ready"),
                             QStringLiteral("Saved to:\n%1").arg(outputEdit_->text().trimmed()));
}

} // namespace dvs
