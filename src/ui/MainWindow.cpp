#include "MainWindow.h"

#include <QAction>
#include <QAudioOutput>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QIcon>
#include <QMediaPlayer>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

#include "core/Paths.h"
#include "core/SrtParser.h"
#include "render/VideoExporter.h"
#include "core/Translator.h"
#include "ui/HighlightDialog.h"
#include "ui/OverlayPanel.h"
#include "ui/PreviewCanvas.h"
#include "ui/SegmentTableModel.h"
#include "ui/SpeakerStylePanel.h"
#include "ui/StitchDialog.h"
#include "ui/TimelineWidget.h"
#include "ui/TranslationPanel.h"

namespace dvs {

namespace {

QString formatClock(qint64 ms) {
    const qint64 total = ms / 1000;
    return QStringLiteral("%1:%2.%3")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'))
        .arg((ms % 1000) / 100);
}

QString lastDir(const QString& key) {
    return QSettings(QStringLiteral("DialogVideoStudio"), QStringLiteral("DialogVideoStudio"))
        .value(key, QDir::homePath())
        .toString();
}

void rememberDir(const QString& key, const QString& filePath) {
    QSettings(QStringLiteral("DialogVideoStudio"), QStringLiteral("DialogVideoStudio"))
        .setValue(key, QFileInfo(filePath).absolutePath());
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    project_ = Project::makeDefault();
    buildUi();
    buildMenus();
    refreshAll();
    setWindowTitle(QStringLiteral("Dialog Video Studio"));
    resize(1500, 900);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    preview_ = new PreviewCanvas;
    timeline_ = new TimelineWidget;
    table_ = new QTableView;
    model_ = new SegmentTableModel(this);
    speakerDelegate_ = new SpeakerDelegate(&project_, this);
    stylePanel_ = new SpeakerStylePanel;

    model_->setProject(&project_);
    table_->setModel(model_);
    table_->setItemDelegateForColumn(SegmentTableModel::ColSpeaker, speakerDelegate_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(SegmentTableModel::ColText,
                                                     QHeaderView::Stretch);
    table_->setColumnWidth(SegmentTableModel::ColIndex, 40);
    table_->setColumnWidth(SegmentTableModel::ColStart, 100);
    table_->setColumnWidth(SegmentTableModel::ColEnd, 100);
    table_->setColumnWidth(SegmentTableModel::ColSpeaker, 110);
    table_->horizontalHeader()->setSectionResizeMode(SegmentTableModel::ColTranslation,
                                                     QHeaderView::Stretch);
    table_->setWordWrap(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableView::customContextMenuRequested, this,
            &MainWindow::showSegmentContextMenu);

    translationPanel_ = new TranslationPanel;
    overlayPanel_ = new OverlayPanel;

    preview_->setProject(&project_);
    timeline_->setProject(&project_);
    stylePanel_->setProject(&project_);
    translationPanel_->setProject(&project_);
    overlayPanel_->setProject(&project_);

    rightTabs_ = new QTabWidget;
    rightTabs_->addTab(stylePanel_, QStringLiteral("Speakers"));
    rightTabs_->addTab(translationPanel_, QStringLiteral("English"));
    rightTabs_->addTab(overlayPanel_, QStringLiteral("Overlays"));

    // --- transport ---
    playButton_ = new QPushButton(QStringLiteral("Play"));
    playButton_->setFixedWidth(80);
    timeLabel_ = new QLabel(QStringLiteral("00:00.0"));
    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setVisible(false);
    progress_->setFixedWidth(220);

    auto* transport = new QHBoxLayout;
    transport->addWidget(playButton_);
    transport->addWidget(timeLabel_);
    transport->addStretch(1);
    transport->addWidget(progress_);

    auto* centreWidget = new QWidget;
    auto* centreLayout = new QVBoxLayout(centreWidget);
    centreLayout->setContentsMargins(0, 0, 0, 0);
    centreLayout->addWidget(preview_, 1);
    centreLayout->addLayout(transport);
    centreLayout->addWidget(timeline_);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(table_);
    splitter->addWidget(centreWidget);
    splitter->addWidget(rightTabs_);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 6);
    splitter->setStretchFactor(2, 3);
    setCentralWidget(splitter);

    statusLabel_ = new QLabel(QStringLiteral("Open an audio file and a subtitle file to begin."));
    statusBar()->addWidget(statusLabel_, 1);

    // --- playback ---
    player_ = new QMediaPlayer(this);
    audioOut_ = new QAudioOutput(this);
    player_->setAudioOutput(audioOut_);

    connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        preview_->setTime(ms);
        timeline_->setTime(ms);
        timeLabel_->setText(formatClock(ms));
    });
    connect(player_, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                playButton_->setText(state == QMediaPlayer::PlayingState
                                         ? QStringLiteral("Pause")
                                         : QStringLiteral("Play"));
            });
    connect(playButton_, &QPushButton::clicked, this, &MainWindow::togglePlayback);

    // --- cross-widget wiring ---
    connect(timeline_, &TimelineWidget::seeked, this, &MainWindow::seek);
    connect(timeline_, &TimelineWidget::scenesChanged, this, [this] {
        preview_->invalidate();
        markDirty();
    });
    connect(preview_, &PreviewCanvas::styleEdited, this, [this] {
        stylePanel_->refreshValues();
        translationPanel_->refreshValues();
        overlayPanel_->refreshValues();
        markDirty();
    });
    connect(preview_, &PreviewCanvas::speakerActivated, this, [this](int speakerId) {
        stylePanel_->setSpeaker(speakerId);
        rightTabs_->setCurrentWidget(stylePanel_);
    });
    connect(preview_, &PreviewCanvas::translationActivated, this,
            [this] { rightTabs_->setCurrentWidget(translationPanel_); });
    connect(preview_, &PreviewCanvas::overlayActivated, this, [this](int index) {
        overlayPanel_->selectOverlay(index);
        rightTabs_->setCurrentWidget(overlayPanel_);
    });

    // Switching tabs moves the drag handles onto the box that tab edits.
    connect(rightTabs_, &QTabWidget::currentChanged, this, [this] {
        QWidget* tab = rightTabs_->currentWidget();
        if (tab == translationPanel_) {
            preview_->setSelection(PreviewCanvas::Target::Translation);
        } else if (tab == overlayPanel_) {
            preview_->setSelection(PreviewCanvas::Target::Overlay,
                                   overlayPanel_->selectedOverlay());
        } else {
            preview_->setSelection(PreviewCanvas::Target::Subtitle);
        }
    });

    connect(stylePanel_, &SpeakerStylePanel::styleChanged, this, [this] {
        preview_->invalidate();
        markDirty();
    });
    connect(translationPanel_, &TranslationPanel::styleChanged, this, [this] {
        preview_->invalidate();
        markDirty();
    });
    connect(translationPanel_, &TranslationPanel::pairRequested, this,
            &MainWindow::pairTranslationsNow);
    connect(translationPanel_, &TranslationPanel::unpairRequested, this,
            &MainWindow::unpairTranslationsNow);
    connect(translationPanel_, &TranslationPanel::translateRequested, this,
            &MainWindow::translateNow);
    connect(overlayPanel_, &OverlayPanel::overlaysChanged, this, [this] {
        preview_->invalidate();
        markDirty();
    });
    connect(overlayPanel_, &OverlayPanel::selectionChanged, this, [this](int index) {
        preview_->setSelection(PreviewCanvas::Target::Overlay, index);
    });
    connect(stylePanel_, &SpeakerStylePanel::speakersChanged, this, [this] {
        model_->refresh();
        preview_->invalidate();
        timeline_->update();
        markDirty();
    });
    connect(model_, &SegmentTableModel::segmentsChanged, this, [this] {
        project_.normalise();
        preview_->invalidate();
        timeline_->update();
        markDirty();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current) {
                if (!current.isValid() || current.row() >= project_.segments.size()) return;
                seek(project_.segments.at(current.row()).startMs);
                const int speakerId = project_.segments.at(current.row()).speakerId;
                if (speakerId >= 0) stylePanel_->setSpeaker(speakerId);
            });

    // --- background work ---
    diarizeWatcher_ = new QFutureWatcher<DiarizeReport>(this);
    connect(diarizeWatcher_, &QFutureWatcher<DiarizeReport>::finished, this,
            &MainWindow::onDiarizationFinished);

    translateWatcher_ = new QFutureWatcher<TranslationReport>(this);
    connect(translateWatcher_, &QFutureWatcher<TranslationReport>::finished, this,
            &MainWindow::onTranslationFinished);

    exporter_ = new VideoExporter(this);
    exportWatcher_ = new QFutureWatcher<QString>(this);
    connect(exportWatcher_, &QFutureWatcher<QString>::finished, this,
            &MainWindow::onExportFinished);
    connect(exporter_, &VideoExporter::progress, this,
            [this](int percent) { progress_->setValue(percent); });
}

void MainWindow::buildMenus() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New project"), QKeySequence::New, this,
                        &MainWindow::newProject);
    fileMenu->addAction(QStringLiteral("&Open project..."), QKeySequence::Open, this,
                        &MainWindow::openProject);
    fileMenu->addAction(QStringLiteral("&Save project"), QKeySequence::Save, this,
                        [this] { saveProject(); });
    fileMenu->addAction(QStringLiteral("Save project &as..."), QKeySequence::SaveAs, this,
                        [this] { saveProjectAs(); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Open &audio..."), this, &MainWindow::openAudio);
    fileMenu->addAction(QStringLiteral("Open s&ubtitles..."), this, &MainWindow::openSubtitles);
    fileMenu->addAction(QStringLiteral("Add scene &images..."), this, &MainWindow::addImages);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Export MP4..."), QKeySequence(Qt::CTRL | Qt::Key_E), this,
                        &MainWindow::exportVideo);
    fileMenu->addAction(QStringLiteral("&Join batches into one video..."), this,
                        &MainWindow::showStitchDialog);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* speakerMenu = menuBar()->addMenu(QStringLiteral("&Speakers"));
    speakerMenu->addAction(QStringLiteral("&Split speakers from audio"),
                           QKeySequence(Qt::CTRL | Qt::Key_D), this, &MainWindow::runDiarization);
    speakerMenu->addSeparator();
    for (int i = 0; i < 4; ++i) {
        speakerMenu->addAction(
            QStringLiteral("Assign selection to speaker %1").arg(i + 1),
            QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this, [this, i] {
                QList<int> rows;
                for (const QModelIndex& index : table_->selectionModel()->selectedRows()) {
                    rows << index.row();
                }
                model_->assignSpeaker(rows, i);
                preview_->invalidate();
                timeline_->update();
                markDirty();
            });
    }

    speakerMenu->addSeparator();
    speakerMenu->addAction(QStringLiteral("&Word colours..."),
                           QKeySequence(Qt::CTRL | Qt::Key_H), this, &MainWindow::showHighlights);

    auto* subtitleMenu = menuBar()->addMenu(QStringLiteral("Su&btitles"));
    subtitleMenu->addAction(QStringLiteral("&Pair each line with its English translation"),
                            QKeySequence(Qt::CTRL | Qt::Key_T), this,
                            &MainWindow::pairTranslationsNow);
    subtitleMenu->addAction(QStringLiteral("&Undo pairing"), this,
                            &MainWindow::unpairTranslationsNow);
    subtitleMenu->addSeparator();
    subtitleMenu->addAction(QStringLiteral("&Merge consecutive lines by the same speaker..."),
                            QKeySequence(Qt::CTRL | Qt::Key_M), this,
                            &MainWindow::mergeSameSpeakerNow);
    subtitleMenu->addAction(QStringLiteral("U&ndo merging"), this, &MainWindow::unmergeNow);
    subtitleMenu->addSeparator();
    subtitleMenu->addAction(QStringLiteral("&Machine-translate every line..."), this,
                            &MainWindow::translateNow);

    auto* videoMenu = menuBar()->addMenu(QStringLiteral("&Video"));
    videoMenu->addAction(QStringLiteral("Landscape 1920 x 1080"), this,
                         [this] { applyCanvasPreset(1920, 1080); });
    videoMenu->addAction(QStringLiteral("Portrait 1080 x 1920"), this,
                         [this] { applyCanvasPreset(1080, 1920); });
    videoMenu->addAction(QStringLiteral("Square 1080 x 1080"), this,
                         [this] { applyCanvasPreset(1080, 1080); });
    videoMenu->addSeparator();
    videoMenu->addAction(QStringLiteral("&Settings..."), this, &MainWindow::showSettings);

    auto* toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setMovable(false);
    toolbar->addAction(QStringLiteral("Audio"), this, &MainWindow::openAudio);
    toolbar->addAction(QStringLiteral("Subtitles"), this, &MainWindow::openSubtitles);
    toolbar->addAction(QStringLiteral("Split speakers"), this, &MainWindow::runDiarization);
    toolbar->addAction(QStringLiteral("Images"), this, &MainWindow::addImages);
    toolbar->addAction(QStringLiteral("Word colours"), this, &MainWindow::showHighlights);
    toolbar->addAction(QStringLiteral("Pair English"), this, &MainWindow::pairTranslationsNow);
    toolbar->addSeparator();
    toolbar->addAction(QStringLiteral("Merge lines"), this, &MainWindow::mergeSameSpeakerNow);
    toolbar->addSeparator();
    toolbar->addAction(QStringLiteral("Export MP4"), this, &MainWindow::exportVideo);
    toolbar->addAction(QStringLiteral("Join batches"), this, &MainWindow::showStitchDialog);
}

// --- loading ---------------------------------------------------------------

void MainWindow::openAudio() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open audio"), lastDir(QStringLiteral("dir/audio")),
        QStringLiteral("Audio (*.mp3 *.wav *.m4a *.flac *.ogg *.aac);;All files (*)"));
    if (path.isEmpty()) return;
    rememberDir(QStringLiteral("dir/audio"), path);
    loadAudioFile(path);
}

void MainWindow::loadAudioFile(const QString& path) {
    setBusy(true, QStringLiteral("Decoding audio..."));
    const AudioDecodeResult decoded = decodeToMono(path);
    setBusy(false);

    if (!decoded.ok()) {
        QMessageBox::warning(this, QStringLiteral("Could not read the audio"), decoded.error);
        return;
    }

    audio_ = decoded.audio;
    project_.audioPath = path;
    project_.durationMs = audio_.durationMs();
    if (project_.scenes.size() == 1 && project_.scenes.first().endMs == 0) {
        project_.autoLayoutScenes();
    }

    timeline_->setPeaks(computePeaks(audio_, 2000));
    player_->setSource(QUrl::fromLocalFile(path));
    statusLabel_->setText(QStringLiteral("%1 - %2 s")
                              .arg(QFileInfo(path).fileName())
                              .arg(project_.durationMs / 1000.0, 0, 'f', 1));
    refreshAll();
    markDirty();
}

void MainWindow::openSubtitles() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open subtitles"), lastDir(QStringLiteral("dir/srt")),
        QStringLiteral("SubRip (*.srt);;All files (*)"));
    if (path.isEmpty()) return;
    rememberDir(QStringLiteral("dir/srt"), path);
    loadSubtitleFile(path);
}

void MainWindow::loadSubtitleFile(const QString& path) {
    const SrtParseResult srt = parseSrtFile(path);
    if (!srt.ok()) {
        QMessageBox::warning(this, QStringLiteral("Could not read the subtitles"), srt.error);
        return;
    }

    project_.srtPath = path;
    words_ = wordsFromCues(srt.cues);
    project_.segments = segmentsFromWords(words_);
    project_.normalise();

    statusLabel_->setText(
        QStringLiteral("%1 cues split into %2 subtitle lines. Run \"Split speakers\" next.")
            .arg(srt.cues.size())
            .arg(project_.segments.size()));
    refreshAll();
    markDirty();
}

void MainWindow::addImages() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add scene images"), lastDir(QStringLiteral("dir/images")),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All files (*)"));
    if (paths.isEmpty()) return;
    rememberDir(QStringLiteral("dir/images"), paths.first());

    const bool wasEmpty = project_.scenes.isEmpty();
    for (const QString& path : paths) {
        Scene s;
        s.imagePath = path;
        project_.scenes.append(s);
    }
    if (wasEmpty) {
        project_.autoLayoutScenes();
    } else {
        // Re-spread everything so newly added images actually get screen time.
        project_.autoLayoutScenes();
    }
    project_.normalise();
    refreshAll();
    markDirty();
}

// --- diarization -----------------------------------------------------------

void MainWindow::runDiarization() {
    if (busy_) return;
    if (project_.segments.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to split"),
                                 QStringLiteral("Open a subtitle file first."));
        return;
    }

    bool ok = false;
    const int speakerCount = QInputDialog::getInt(
        this, QStringLiteral("Split speakers"),
        QStringLiteral("How many people are speaking?\n(0 lets the app decide)"), 2, 0, 8, 1, &ok);
    if (!ok) return;

    DiarizeOptions opts;
    opts.speakerCount = speakerCount;

    pendingSegments_ = project_.segments;
    const QList<Word> words = words_;
    const AudioBuffer audio = audio_;

    setBusy(true, QStringLiteral("Comparing voices..."));
    progress_->setVisible(true);
    progress_->setValue(0);

    diarizeWatcher_->setFuture(QtConcurrent::run([this, opts, words, audio] {
        return diarize(pendingSegments_, words, audio, opts,
                       [this](int percent) { QMetaObject::invokeMethod(
                           progress_, "setValue", Qt::QueuedConnection, Q_ARG(int, percent)); });
    }));
}

void MainWindow::onDiarizationFinished() {
    setBusy(false);
    progress_->setVisible(false);

    const DiarizeReport report = diarizeWatcher_->result();
    if (!report.ok()) {
        QMessageBox::warning(this, QStringLiteral("Could not split the speakers"), report.error);
        return;
    }

    project_.segments = pendingSegments_;
    while (project_.speakers.size() < report.speakerCount) {
        Speaker extra = project_.speakers.last();
        extra.name = QStringLiteral("Speaker %1").arg(project_.speakers.size() + 1);
        extra.style.box.moveTop(std::min(0.90, extra.style.box.top() + 0.06));
        project_.speakers.append(extra);
    }
    project_.normalise();
    refreshAll();
    markDirty();

    QString summary = QStringLiteral("%1 speakers, %2 lines")
                          .arg(report.speakerCount)
                          .arg(project_.segments.size());
    if (!report.backend.isEmpty()) summary += QStringLiteral(" (%1)").arg(report.backend);
    if (report.splitCount > 0) {
        summary += QStringLiteral(", %1 line(s) split at a speaker change").arg(report.splitCount);
    }
    summary += report.reviewCount == 0
                   ? QStringLiteral(" - every line is confident.")
                   : QStringLiteral(" - %1 line(s) highlighted for review.").arg(report.reviewCount);
    statusLabel_->setText(summary);

    if (!report.warning.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Speaker split"), report.warning);
    }
}

// --- export ----------------------------------------------------------------

void MainWindow::exportVideo() {
    if (busy_) return;
    if (project_.audioPath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to export"),
                                 QStringLiteral("Open an audio file first."));
        return;
    }

    QString suggested = project_.exportSettings.outputPath;
    if (suggested.isEmpty()) {
        suggested = QFileInfo(project_.audioPath).absolutePath() + QLatin1Char('/') +
                    QFileInfo(project_.audioPath).completeBaseName() + QStringLiteral(".mp4");
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export MP4"), suggested,
                                                      QStringLiteral("MP4 video (*.mp4)"));
    if (path.isEmpty()) return;

    project_.exportSettings.outputPath = path;
    rememberDir(QStringLiteral("dir/export"), path);

    if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();

    setBusy(true, QStringLiteral("Rendering video..."));
    progress_->setVisible(true);
    progress_->setValue(0);

    const Project snapshot = project_; // the worker must not see later edits
    exporter_->reset();
    exportWatcher_->setFuture(
        QtConcurrent::run([this, snapshot] { return exporter_->run(snapshot); }));
}

void MainWindow::onExportFinished() {
    setBusy(false);
    progress_->setVisible(false);

    const QString error = exportWatcher_->result();
    if (error.isEmpty()) {
        statusLabel_->setText(
            QStringLiteral("Exported %1").arg(project_.exportSettings.outputPath));
        QMessageBox::information(
            this, QStringLiteral("Export finished"),
            QStringLiteral("Saved to:\n%1").arg(project_.exportSettings.outputPath));
    } else {
        statusLabel_->setText(QStringLiteral("Export failed."));
        QMessageBox::warning(this, QStringLiteral("Export failed"), error);
    }
}

// --- project ---------------------------------------------------------------

void MainWindow::newProject() {
    if (!confirmDiscard()) return;
    project_ = Project::makeDefault();
    words_.clear();
    audio_ = {};
    projectPath_.clear();
    dirty_ = false;
    player_->setSource({});
    timeline_->setPeaks({});
    refreshAll();
    statusLabel_->setText(QStringLiteral("New project."));
}

void MainWindow::openProject() {
    if (!confirmDiscard()) return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open project"), lastDir(QStringLiteral("dir/project")),
        QStringLiteral("Dialog Video Studio project (*.dvsproj);;All files (*)"));
    if (path.isEmpty()) return;

    QString error;
    Project loaded = Project::load(path, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Could not open the project"), error);
        return;
    }

    project_ = loaded;
    projectPath_ = path;
    rememberDir(QStringLiteral("dir/project"), path);
    dirty_ = false;

    // Re-derive the word stream so speaker splitting still works after reload.
    words_.clear();
    if (!project_.srtPath.isEmpty() && QFileInfo::exists(project_.srtPath)) {
        const SrtParseResult srt = parseSrtFile(project_.srtPath);
        if (srt.ok()) words_ = wordsFromCues(srt.cues);
    }
    if (!project_.audioPath.isEmpty() && QFileInfo::exists(project_.audioPath)) {
        const AudioDecodeResult decoded = decodeToMono(project_.audioPath);
        if (decoded.ok()) {
            audio_ = decoded.audio;
            timeline_->setPeaks(computePeaks(audio_, 2000));
        }
        player_->setSource(QUrl::fromLocalFile(project_.audioPath));
    }

    refreshAll();
    statusLabel_->setText(QStringLiteral("Opened %1").arg(QFileInfo(path).fileName()));
}

bool MainWindow::saveProject() {
    if (projectPath_.isEmpty()) return saveProjectAs();
    const QString error = project_.save(projectPath_);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Could not save"), error);
        return false;
    }
    dirty_ = false;
    setWindowModified(false);
    statusLabel_->setText(QStringLiteral("Saved %1").arg(QFileInfo(projectPath_).fileName()));
    return true;
}

bool MainWindow::saveProjectAs() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save project"), lastDir(QStringLiteral("dir/project")),
        QStringLiteral("Dialog Video Studio project (*.dvsproj)"));
    if (path.isEmpty()) return false;
    projectPath_ = path;
    rememberDir(QStringLiteral("dir/project"), path);
    return saveProject();
}

void MainWindow::showSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Settings"));
    auto* form = new QFormLayout(&dialog);

    auto* ffmpegEdit = new QLineEdit(ffmpegPath());
    auto* browse = new QPushButton(QStringLiteral("Browse..."));
    auto* ffmpegRow = new QHBoxLayout;
    ffmpegRow->addWidget(ffmpegEdit, 1);
    ffmpegRow->addWidget(browse);
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const QString picked = QFileDialog::getOpenFileName(
            &dialog, QStringLiteral("Locate ffmpeg.exe"), ffmpegEdit->text(),
            QStringLiteral("ffmpeg (ffmpeg.exe);;All files (*)"));
        if (!picked.isEmpty()) ffmpegEdit->setText(picked);
    });
    form->addRow(QStringLiteral("ffmpeg"), ffmpegRow);

    auto* codec = new QComboBox;
    codec->addItem(QStringLiteral("H.264 (libx264, CPU)"), QStringLiteral("libx264"));
    codec->addItem(QStringLiteral("H.264 (NVIDIA NVENC)"), QStringLiteral("h264_nvenc"));
    codec->addItem(QStringLiteral("H.264 (AMD AMF)"), QStringLiteral("h264_amf"));
    const int codecAt = codec->findData(project_.exportSettings.videoCodec);
    codec->setCurrentIndex(codecAt >= 0 ? codecAt : 0);
    form->addRow(QStringLiteral("Video encoder"), codec);

    auto* crf = new QSpinBox;
    crf->setRange(0, 51);
    crf->setValue(project_.exportSettings.crf);
    crf->setToolTip(QStringLiteral("Lower is better quality and a bigger file. 18 is visually lossless."));
    form->addRow(QStringLiteral("Quality (CRF)"), crf);

    auto* fps = new QSpinBox;
    fps->setRange(1, 120);
    fps->setValue(project_.fps);
    form->addRow(QStringLiteral("Frame rate"), fps);

    auto* transition = new QSpinBox;
    transition->setRange(0, 1000);
    transition->setSingleStep(20);
    transition->setSuffix(QStringLiteral(" ms"));
    transition->setValue(project_.transitionMs);
    transition->setToolTip(QStringLiteral(
        "How long a caption takes to fade in or out. The fade is centred on the "
        "subtitle's timestamp, so the caption still changes exactly on time - only the "
        "edge is softened. Set 0 for hard cuts."));
    form->addRow(QStringLiteral("Caption fade"), transition);

    auto* rise = new QDoubleSpinBox;
    rise->setRange(0.0, 0.06);
    rise->setSingleStep(0.002);
    rise->setDecimals(3);
    rise->setValue(project_.transitionRise);
    rise->setToolTip(QStringLiteral(
        "How far a caption slides up into place while it fades in, as a fraction of the "
        "frame. 0 is a pure fade."));
    form->addRow(QStringLiteral("Caption slide"), rise);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    setFfmpegOverride(ffmpegEdit->text().trimmed());
    project_.exportSettings.videoCodec = codec->currentData().toString();
    project_.exportSettings.crf = crf->value();
    project_.fps = fps->value();
    project_.transitionMs = transition->value();
    project_.transitionRise = rise->value();
    preview_->invalidate();
    markDirty();
}

void MainWindow::showHighlights() {
    HighlightDialog dialog(&project_, this);
    connect(&dialog, &HighlightDialog::highlightsChanged, this, [this] {
        preview_->invalidate();
        markDirty();
    });
    dialog.exec();
}

void MainWindow::showSegmentContextMenu(const QPoint& pos) {
    const QModelIndex index = table_->indexAt(pos);
    QMenu menu(this);

    if (index.isValid() && index.row() < project_.segments.size()) {
        // Colour a single word straight from the line it appears in - the
        // fastest path to "make this vocabulary word stand out".
        auto* wordMenu = menu.addMenu(QStringLiteral("Colour word"));
        const QStringList words = project_.segments.at(index.row())
                                      .text.split(QRegularExpression(QStringLiteral(R"(\s+)")),
                                                  Qt::SkipEmptyParts);
        QSet<QString> seen;
        for (const QString& word : words) {
            const QString key = normaliseWord(word);
            if (key.isEmpty() || seen.contains(key)) continue;
            seen.insert(key);

            QAction* action = wordMenu->addAction(key);
            const Highlight* existing = project_.highlightFor(key);
            if (existing) {
                QPixmap swatch(12, 12);
                swatch.fill(existing->color);
                action->setIcon(QIcon(swatch));
            }
            connect(action, &QAction::triggered, this, [this, key] {
                if (!promptHighlightColor(this, &project_, key)) return;
                preview_->invalidate();
                markDirty();
            });
        }
        if (wordMenu->isEmpty()) wordMenu->setEnabled(false);
        menu.addSeparator();
    }

    menu.addAction(QStringLiteral("Word colours..."), this, &MainWindow::showHighlights);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void MainWindow::applyCanvasPreset(int width, int height) {
    const bool wasPortrait = project_.canvas.height() > project_.canvas.width();
    const bool willBePortrait = height > width;
    project_.canvas = QSize(width, height);

    // A layout tuned for one orientation is unusable in the other, so offer to
    // rearrange rather than silently keeping or silently discarding it.
    if (wasPortrait != willBePortrait) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Rearrange the boxes?"),
            QStringLiteral("This video is now %1.\n\nMove the caption, translation and overlay "
                           "boxes to sensible positions for that shape?")
                .arg(willBePortrait ? QStringLiteral("vertical") : QStringLiteral("wide")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) project_.applyDefaultLayout();
    }

    refreshAll();
    markDirty();
}

// --- helpers ---------------------------------------------------------------

void MainWindow::refreshAll() {
    model_->refresh();
    stylePanel_->reload();
    translationPanel_->reload();
    overlayPanel_->reload();
    preview_->invalidate();
    timeline_->update();
}

void MainWindow::pairTranslationsNow() {
    if (project_.segments.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to pair"),
                                 QStringLiteral("Open a subtitle file first."));
        return;
    }

    const PairingReport report = pairTranslations(project_.segments);
    project_.normalise();
    refreshAll();
    markDirty();

    if (report.paired == 0) {
        QMessageBox::information(
            this, QStringLiteral("No translations found"),
            QStringLiteral(
                "None of the lines looked like a German sentence followed by its English "
                "translation, so nothing was paired. You can still type the English into "
                "the Translation column yourself."));
        return;
    }

    QString summary = QStringLiteral("Paired %1 line(s) with their translation")
                          .arg(report.paired);
    if (report.unpairedGerman > 0) {
        summary += QStringLiteral("; %1 had no translation after them").arg(report.unpairedGerman);
    }
    statusLabel_->setText(summary + QLatin1Char('.'));
}

void MainWindow::mergeSameSpeakerNow() {
    if (project_.segments.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to merge"),
                                 QStringLiteral("Open a subtitle file first."));
        return;
    }

    bool anySpeaker = false;
    for (const Segment& s : project_.segments) {
        if (s.speakerId >= 0) {
            anySpeaker = true;
            break;
        }
    }
    if (!anySpeaker) {
        QMessageBox::information(
            this, QStringLiteral("No speakers yet"),
            QStringLiteral("Merging joins lines spoken by the same person, so run "
                           "\"Split speakers\" first."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Merge consecutive lines"));
    auto* form = new QFormLayout(&dialog);

    auto* intro = new QLabel(QStringLiteral(
        "Where the same person says two or more lines in a row, show them as one caption "
        "that stays up for the whole run instead of flashing line by line."));
    intro->setWordWrap(true);
    intro->setMinimumWidth(380);
    form->addRow(intro);

    auto* gap = new QSpinBox;
    gap->setRange(0, 10000);
    gap->setSingleStep(100);
    gap->setSuffix(QStringLiteral(" ms"));
    gap->setValue(900);
    gap->setToolTip(QStringLiteral(
        "Lines further apart than this are treated as separate thoughts and left alone."));
    form->addRow(QStringLiteral("Largest gap to join across"), gap);

    auto* chars = new QSpinBox;
    chars->setRange(20, 1000);
    chars->setSingleStep(20);
    chars->setValue(180);
    chars->setToolTip(QStringLiteral(
        "A caption nobody can finish reading is worse than two captions."));
    form->addRow(QStringLiteral("Longest merged caption"), chars);

    auto* seconds = new QSpinBox;
    seconds->setRange(1, 60);
    seconds->setSuffix(QStringLiteral(" s"));
    seconds->setValue(9);
    form->addRow(QStringLiteral("Longest time on screen"), seconds);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    MergeOptions opts;
    opts.maxGapMs = gap->value();
    opts.maxCharacters = chars->value();
    opts.maxDurationMs = seconds->value() * 1000LL;

    const MergeReport report = mergeSameSpeakerRuns(project_.segments, opts);
    project_.normalise();
    refreshAll();
    markDirty();

    statusLabel_->setText(
        report.merged == 0
            ? QStringLiteral("No consecutive lines by the same speaker were close enough to merge.")
            : QStringLiteral("Merged %1 caption(s), absorbing %2 line(s): %3 -> %4 captions.")
                  .arg(report.merged)
                  .arg(report.absorbed)
                  .arg(report.before)
                  .arg(report.after));
}

void MainWindow::unmergeNow() {
    const int count = unmergeSegments(project_.segments);
    project_.normalise();
    refreshAll();
    markDirty();
    statusLabel_->setText(QStringLiteral("Split %1 merged caption(s) back apart.").arg(count));
}

void MainWindow::showStitchDialog() {
    if (dirty_ && !projectPath_.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Save first?"),
            QStringLiteral("Joining renders projects from their saved files, so unsaved "
                           "changes to this one would not appear.\n\nSave it now?"),
            QMessageBox::Save | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) return;
        if (answer == QMessageBox::Save && !saveProject()) return;
    }

    StitchDialog dialog(projectPath_, this);
    dialog.exec();
}

void MainWindow::translateNow() {
    if (busy_) return;
    if (project_.segments.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to translate"),
                                 QStringLiteral("Open a subtitle file first."));
        return;
    }

    const TranslationSettings settings = translationPanel_->translationSettings();
    const ProviderInfo info = providerInfo(settings.provider);

    // Translating sends the subtitle text off this machine - say so, name the
    // service, and let the user back out.
    const int lines = static_cast<int>(project_.segments.size());
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Send subtitles to %1?").arg(info.displayName),
        QStringLiteral("This will send the text of %1 subtitle line(s) to %2 over the "
                       "internet to be translated from %3 into %4.\n\nContinue?")
            .arg(lines)
            .arg(info.displayName, settings.sourceLang, settings.targetLang),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    pendingSegments_ = project_.segments;
    translateCancelled_.store(false);

    setBusy(true, QStringLiteral("Translating with %1...").arg(info.displayName));
    progress_->setVisible(true);
    progress_->setValue(0);

    translateWatcher_->setFuture(QtConcurrent::run([this, settings] {
        return translateSegments(pendingSegments_, settings, &translateCancelled_, [this](int pct) {
            QMetaObject::invokeMethod(progress_, "setValue", Qt::QueuedConnection,
                                      Q_ARG(int, pct));
        });
    }));
}

void MainWindow::onTranslationFinished() {
    setBusy(false);
    progress_->setVisible(false);

    const TranslationReport report = translateWatcher_->result();
    if (!report.ok()) {
        QMessageBox::warning(this, QStringLiteral("Could not translate"), report.error);
        return;
    }

    project_.segments = pendingSegments_;
    refreshAll();
    markDirty();

    statusLabel_->setText(
        QStringLiteral("Translated %1 line(s); %2 already had a translation; %3 failed.")
            .arg(report.translated)
            .arg(report.skipped)
            .arg(report.failed));

    if (!report.warning.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Translation finished"), report.warning);
    }
}

void MainWindow::unpairTranslationsNow() {
    const int count = unpairTranslations(project_.segments);
    project_.normalise();
    refreshAll();
    markDirty();
    statusLabel_->setText(QStringLiteral("Split %1 line(s) back apart.").arg(count));
}

void MainWindow::seek(qint64 ms) {
    preview_->setTime(ms);
    timeline_->setTime(ms);
    timeLabel_->setText(formatClock(ms));
    if (player_->source().isValid()) player_->setPosition(ms);
}

void MainWindow::togglePlayback() {
    if (!player_->source().isValid()) return;
    if (player_->playbackState() == QMediaPlayer::PlayingState) {
        player_->pause();
    } else {
        player_->play();
    }
}

void MainWindow::setBusy(bool busy, const QString& what) {
    busy_ = busy;
    setCursor(busy ? Qt::BusyCursor : Qt::ArrowCursor);
    menuBar()->setEnabled(!busy);
    if (busy && !what.isEmpty()) statusLabel_->setText(what);
    QCoreApplication::processEvents();
}

void MainWindow::markDirty() {
    dirty_ = true;
    setWindowModified(true);
    setWindowTitle(QStringLiteral("Dialog Video Studio - %1[*]")
                       .arg(projectPath_.isEmpty() ? QStringLiteral("untitled")
                                                   : QFileInfo(projectPath_).fileName()));
}

bool MainWindow::confirmDiscard() {
    if (!dirty_) return true;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Unsaved changes"),
        QStringLiteral("Save the current project first?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return saveProject();
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (busy_) {
        exporter_->cancel();
        exportWatcher_->waitForFinished();
        diarizeWatcher_->waitForFinished();
    }
    if (!confirmDiscard()) {
        event->ignore();
        return;
    }
    event->accept();
}

} // namespace dvs
