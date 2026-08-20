#pragma once

#include <QFutureWatcher>
#include <QMainWindow>
#include <QString>

#include "core/AudioDecoder.h"
#include "core/Diarizer.h"
#include "core/Project.h"
#include "core/Segmenter.h"
#include "core/TranslationService.h"

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QProgressBar;
class QPushButton;
class QSlider;
class QTabWidget;
class QTableView;

namespace dvs {

class OverlayPanel;
class PreviewCanvas;
class SegmentTableModel;
class SpeakerDelegate;
class SpeakerStylePanel;
class TimelineWidget;
class TranslationPanel;
class VideoExporter;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void openAudio();
    void openSubtitles();
    void addImages();
    void runDiarization();
    void onDiarizationFinished();
    void exportVideo();
    void onExportFinished();
    void newProject();
    void openProject();
    bool saveProject();
    bool saveProjectAs();
    void showSettings();
    void showHighlights();
    void showSegmentContextMenu(const QPoint& pos);
    void pairTranslationsNow();
    void unpairTranslationsNow();
    void translateNow();
    void onTranslationFinished();
    void mergeSameSpeakerNow();
    void unmergeNow();
    void showStitchDialog();

private:
    void buildUi();
    void buildMenus();
    void refreshAll();
    void seek(qint64 ms);
    void togglePlayback();
    void loadAudioFile(const QString& path);
    void loadSubtitleFile(const QString& path);
    void setBusy(bool busy, const QString& what = {});
    void markDirty();
    bool confirmDiscard();
    void applyCanvasPreset(int width, int height);

    Project project_;
    QList<Word> words_;
    AudioBuffer audio_;
    QString projectPath_;
    bool dirty_ = false;
    bool busy_ = false;

    // Widgets
    PreviewCanvas* preview_ = nullptr;
    TimelineWidget* timeline_ = nullptr;
    QTableView* table_ = nullptr;
    SegmentTableModel* model_ = nullptr;
    SpeakerDelegate* speakerDelegate_ = nullptr;
    SpeakerStylePanel* stylePanel_ = nullptr;
    TranslationPanel* translationPanel_ = nullptr;
    OverlayPanel* overlayPanel_ = nullptr;
    QTabWidget* rightTabs_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QMediaPlayer* player_ = nullptr;
    QAudioOutput* audioOut_ = nullptr;

    // Background work
    QFutureWatcher<DiarizeReport>* diarizeWatcher_ = nullptr;
    QFutureWatcher<QString>* exportWatcher_ = nullptr;
    VideoExporter* exporter_ = nullptr;
    QFutureWatcher<TranslationReport>* translateWatcher_ = nullptr;
    std::atomic<bool> translateCancelled_{false};
    QList<Segment> pendingSegments_; // written by the diarization/translation workers
};

} // namespace dvs
