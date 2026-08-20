#include "OverlayPanel.h"

#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ui/StyleEditor.h"

namespace dvs {

namespace {

QString lastImageDir() {
    return QSettings(QStringLiteral("DialogVideoStudio"), QStringLiteral("DialogVideoStudio"))
        .value(QStringLiteral("dir/overlay"), QDir::homePath())
        .toString();
}

void rememberImageDir(const QString& path) {
    QSettings(QStringLiteral("DialogVideoStudio"), QStringLiteral("DialogVideoStudio"))
        .setValue(QStringLiteral("dir/overlay"), QFileInfo(path).absolutePath());
}

} // namespace

OverlayPanel::OverlayPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    list_ = new QListWidget;
    list_->setMaximumHeight(130);
    outer->addWidget(list_);

    auto* addRow = new QHBoxLayout;
    auto* addLogo = new QPushButton(QStringLiteral("Logo"));
    auto* addSubscribe = new QPushButton(QStringLiteral("Subscribe"));
    auto* addTitle = new QPushButton(QStringLiteral("Title"));
    auto* addImage = new QPushButton(QStringLiteral("Image"));
    auto* remove = new QPushButton(QStringLiteral("Remove"));
    for (QPushButton* b : {addLogo, addSubscribe, addTitle, addImage, remove}) {
        addRow->addWidget(b);
    }
    outer->addLayout(addRow);
    addLogo->setToolTip(QStringLiteral("Add a channel logo, bottom-left"));
    addSubscribe->setToolTip(QStringLiteral("Add a subscribe button, bottom-right"));
    addTitle->setToolTip(QStringLiteral("Add a title banner across the top"));

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    auto* form = new QVBoxLayout(inner);
    form->setContentsMargins(0, 0, 6, 0);

    auto* group = new QGroupBox(QStringLiteral("Overlay"));
    auto* grid = new QFormLayout(group);
    nameEdit_ = new QLineEdit;
    sourceEdit_ = new QLineEdit;
    browseButton_ = new QPushButton(QStringLiteral("Browse..."));
    auto* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(sourceEdit_, 1);
    sourceRow->addWidget(browseButton_);
    opacity_ = new QDoubleSpinBox;
    opacity_->setRange(0.0, 1.0);
    opacity_->setSingleStep(0.05);
    opacity_->setDecimals(2);
    keepAspect_ = new QCheckBox(QStringLiteral("Keep the picture's proportions"));
    onTop_ = new QCheckBox(QStringLiteral("Draw over the subtitles"));
    enabled_ = new QCheckBox(QStringLiteral("Visible"));
    wholeVideo_ = new QCheckBox(QStringLiteral("Show for the whole video"));
    startSec_ = new QSpinBox;
    startSec_->setRange(0, 100000);
    startSec_->setSuffix(QStringLiteral(" s"));
    endSec_ = new QSpinBox;
    endSec_->setRange(0, 100000);
    endSec_->setSuffix(QStringLiteral(" s"));

    grid->addRow(QStringLiteral("Name"), nameEdit_);
    grid->addRow(QStringLiteral("Picture / text"), sourceRow);
    grid->addRow(QStringLiteral("Opacity"), opacity_);
    grid->addRow(QString(), keepAspect_);
    grid->addRow(QString(), onTop_);
    grid->addRow(QString(), enabled_);
    grid->addRow(QString(), wholeVideo_);
    grid->addRow(QStringLiteral("From"), startSec_);
    grid->addRow(QStringLiteral("To"), endSec_);
    form->addWidget(group);

    editor_ = new StyleEditor;
    form->addWidget(editor_);
    form->addStretch(1);

    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    // --- wiring ---
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (updating_) return;
        bind();
        emit selectionChanged(row);
    });

    connect(addLogo, &QPushButton::clicked, this,
            [this] { addImageOverlay(QStringLiteral("Logo")); });
    connect(addSubscribe, &QPushButton::clicked, this,
            [this] { addImageOverlay(QStringLiteral("Subscribe")); });
    connect(addImage, &QPushButton::clicked, this,
            [this] { addImageOverlay(QStringLiteral("Image")); });
    connect(addTitle, &QPushButton::clicked, this, &OverlayPanel::addTextOverlay);

    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (!project_ || row < 0 || row >= project_->overlays.size()) return;
        project_->overlays.removeAt(row);
        reload();
        emit overlaysChanged();
    });

    connect(browseButton_, &QPushButton::clicked, this, [this] {
        Overlay* o = current();
        if (!o || !o->isImage) return;
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Choose a picture"), lastImageDir(),
            QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All files (*)"));
        if (path.isEmpty()) return;
        rememberImageDir(path);
        o->imagePath = path;
        sourceEdit_->setText(path);
        emit overlaysChanged();
    });

    connect(nameEdit_, &QLineEdit::textEdited, this, [this](const QString& text) {
        Overlay* o = current();
        if (updating_ || !o) return;
        o->name = text;
        updating_ = true;
        if (list_->currentItem()) list_->currentItem()->setText(text);
        updating_ = false;
        emit overlaysChanged();
    });

    connect(sourceEdit_, &QLineEdit::textEdited, this, [this](const QString& text) {
        Overlay* o = current();
        if (updating_ || !o) return;
        (o->isImage ? o->imagePath : o->text) = text;
        emit overlaysChanged();
    });

    connect(opacity_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        Overlay* o = current();
        if (updating_ || !o) return;
        o->opacity = v;
        emit overlaysChanged();
    });

    const auto bindCheck = [this](QCheckBox* box, bool Overlay::*field) {
        connect(box, &QCheckBox::toggled, this, [this, field](bool on) {
            Overlay* o = current();
            if (updating_ || !o) return;
            o->*field = on;
            emit overlaysChanged();
        });
    };
    bindCheck(keepAspect_, &Overlay::keepAspect);
    bindCheck(onTop_, &Overlay::onTop);
    bindCheck(enabled_, &Overlay::enabled);

    connect(wholeVideo_, &QCheckBox::toggled, this, [this](bool on) {
        Overlay* o = current();
        startSec_->setEnabled(!on);
        endSec_->setEnabled(!on);
        if (updating_ || !o) return;
        if (on) {
            o->startMs = 0;
            o->endMs = 0;
        } else {
            o->startMs = startSec_->value() * 1000LL;
            o->endMs = endSec_->value() * 1000LL;
        }
        emit overlaysChanged();
    });

    const auto bindTime = [this](QSpinBox* box, qint64 Overlay::*field) {
        connect(box, &QSpinBox::valueChanged, this, [this, field](int v) {
            Overlay* o = current();
            if (updating_ || !o) return;
            o->*field = v * 1000LL;
            emit overlaysChanged();
        });
    };
    bindTime(startSec_, &Overlay::startMs);
    bindTime(endSec_, &Overlay::endMs);

    connect(editor_, &StyleEditor::changed, this, &OverlayPanel::overlaysChanged);

    bind();
}

void OverlayPanel::setProject(Project* project) {
    project_ = project;
    reload();
}

Overlay* OverlayPanel::current() const {
    const int row = list_->currentRow();
    if (!project_ || row < 0 || row >= project_->overlays.size()) return nullptr;
    return &project_->overlays[row];
}

int OverlayPanel::selectedOverlay() const { return list_->currentRow(); }

void OverlayPanel::selectOverlay(int index) {
    if (index < 0 || index >= list_->count()) return;
    list_->setCurrentRow(index);
}

void OverlayPanel::reload() {
    updating_ = true;
    const int previous = list_->currentRow();
    list_->clear();
    if (project_) {
        for (const Overlay& o : project_->overlays) list_->addItem(o.name);
    }
    if (previous >= 0 && previous < list_->count()) {
        list_->setCurrentRow(previous);
    } else if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
    updating_ = false;
    bind();
}

void OverlayPanel::refreshValues() {
    Overlay* o = current();
    if (!o) return;
    editor_->refresh();
}

void OverlayPanel::bind() {
    Overlay* o = current();
    editor_->setStyle(o ? &o->style : nullptr);
    // A picture has no text, font or fill of its own - only a rectangle.
    editor_->setTextSectionsVisible(o && !o->isImage);

    const QList<QWidget*> fields = {nameEdit_,  sourceEdit_, browseButton_, opacity_,
                                    keepAspect_, onTop_,     enabled_,      wholeVideo_,
                                    startSec_,   endSec_};
    for (QWidget* w : fields) w->setEnabled(o != nullptr);
    if (!o) return;

    updating_ = true;
    nameEdit_->setText(o->name);
    sourceEdit_->setText(o->isImage ? o->imagePath : o->text);
    browseButton_->setEnabled(o->isImage);
    keepAspect_->setEnabled(o->isImage);
    opacity_->setValue(o->opacity);
    keepAspect_->setChecked(o->keepAspect);
    onTop_->setChecked(o->onTop);
    enabled_->setChecked(o->enabled);

    const bool whole = o->endMs <= 0 && o->startMs <= 0;
    wholeVideo_->setChecked(whole);
    startSec_->setValue(static_cast<int>(o->startMs / 1000));
    endSec_->setValue(static_cast<int>(o->endMs / 1000));
    startSec_->setEnabled(!whole);
    endSec_->setEnabled(!whole);
    updating_ = false;
}

void OverlayPanel::addImageOverlay(const QString& presetName) {
    if (!project_) return;

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose a picture for the %1").arg(presetName.toLower()),
        lastImageDir(), QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All files (*)"));
    if (path.isEmpty()) return;
    rememberImageDir(path);

    Overlay overlay;
    if (presetName == QLatin1String("Logo")) {
        overlay = Overlay::makeLogo(path);
    } else if (presetName == QLatin1String("Subscribe")) {
        overlay = Overlay::makeSubscribe(path);
    } else {
        overlay.name = QFileInfo(path).completeBaseName();
        overlay.isImage = true;
        overlay.imagePath = path;
        overlay.style.box = QRectF(0.35, 0.40, 0.30, 0.20);
    }

    // Presets are written for 16:9; nudge them for a vertical canvas.
    if (project_->canvas.height() > project_->canvas.width()) {
        if (overlay.name == QLatin1String("Logo")) {
            overlay.style.box = QRectF(0.03, 0.88, 0.28, 0.09);
        } else if (overlay.name == QLatin1String("Subscribe")) {
            overlay.style.box = QRectF(0.55, 0.89, 0.42, 0.06);
        }
    }

    project_->overlays.append(overlay);
    reload();
    list_->setCurrentRow(project_->overlays.size() - 1);
    emit overlaysChanged();
}

void OverlayPanel::addTextOverlay() {
    if (!project_) return;
    bool ok = false;
    const QString text =
        QInputDialog::getText(this, QStringLiteral("Title banner"),
                              QStringLiteral("Text to show across the top:"), QLineEdit::Normal,
                              QString(), &ok);
    if (!ok || text.trimmed().isEmpty()) return;

    Overlay overlay = Overlay::makeTitleBanner(text.trimmed());
    if (project_->canvas.height() > project_->canvas.width()) {
        overlay.style.box = QRectF(0.0, 0.0, 1.0, 0.06);
    }
    project_->overlays.append(overlay);
    reload();
    list_->setCurrentRow(project_->overlays.size() - 1);
    emit overlaysChanged();
}

} // namespace dvs
