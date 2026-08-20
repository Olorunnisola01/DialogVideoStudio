#include "SpeakerStylePanel.h"

#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/StyleEditor.h"

namespace dvs {

namespace {

void paintSwatch(QPushButton* button, const QColor& color) {
    button->setText(color.name(QColor::HexRgb).toUpper());
    const QString fg = color.lightnessF() > 0.55 ? QStringLiteral("#111") : QStringLiteral("#fff");
    button->setStyleSheet(QStringLiteral("background:%1; color:%2; padding:4px;")
                              .arg(color.name(QColor::HexRgb), fg));
}

} // namespace

SpeakerStylePanel::SpeakerStylePanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* header = new QHBoxLayout;
    speakerCombo_ = new QComboBox;
    addButton_ = new QPushButton(QStringLiteral("+"));
    addButton_->setFixedWidth(28);
    addButton_->setToolTip(QStringLiteral("Add another speaker"));
    header->addWidget(new QLabel(QStringLiteral("Speaker")));
    header->addWidget(speakerCombo_, 1);
    header->addWidget(addButton_);
    outer->addLayout(header);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    auto* form = new QVBoxLayout(inner);
    form->setContentsMargins(0, 0, 6, 0);

    auto* idGroup = new QGroupBox(QStringLiteral("Identity"));
    auto* idForm = new QFormLayout(idGroup);
    nameEdit_ = new QLineEdit;
    tintButton_ = new QPushButton;
    idForm->addRow(QStringLiteral("Name"), nameEdit_);
    idForm->addRow(QStringLiteral("Timeline colour"), tintButton_);
    form->addWidget(idGroup);

    editor_ = new StyleEditor;
    form->addWidget(editor_);
    form->addStretch(1);

    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    connect(editor_, &StyleEditor::changed, this, &SpeakerStylePanel::styleChanged);

    connect(speakerCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_) return;
        setSpeaker(index);
    });
    connect(addButton_, &QPushButton::clicked, this, [this] {
        if (!project_) return;
        Speaker s = project_->speakers.isEmpty() ? Project::makeDefault().speakers.first()
                                                 : project_->speakers.last();
        s.name = QStringLiteral("Speaker %1").arg(project_->speakers.size() + 1);
        s.style.box.moveTop(std::min(0.90, s.style.box.top() + 0.06));
        project_->speakers.append(s);
        reload();
        setSpeaker(project_->speakers.size() - 1);
        emit speakersChanged();
    });

    connect(nameEdit_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (updating_ || !current()) return;
        current()->name = text;
        updating_ = true;
        speakerCombo_->setItemText(speakerId_, text);
        updating_ = false;
        emit speakersChanged();
    });

    connect(tintButton_, &QPushButton::clicked, this, [this] {
        Speaker* s = current();
        if (!s) return;
        const QColor picked =
            QColorDialog::getColor(s->tint, this, QStringLiteral("Timeline colour"));
        if (!picked.isValid()) return;
        s->tint = picked;
        paintSwatch(tintButton_, picked);
        emit speakersChanged();
    });
}

void SpeakerStylePanel::setProject(Project* project) {
    project_ = project;
    speakerId_ = 0;
    reload();
}

Speaker* SpeakerStylePanel::current() const {
    if (!project_ || speakerId_ < 0 || speakerId_ >= project_->speakers.size()) return nullptr;
    return &project_->speakers[speakerId_];
}

void SpeakerStylePanel::bind() {
    Speaker* s = current();
    editor_->setStyle(s ? &s->style : nullptr);
    nameEdit_->setEnabled(s != nullptr);
    tintButton_->setEnabled(s != nullptr);
    if (!s) return;

    updating_ = true;
    nameEdit_->setText(s->name);
    paintSwatch(tintButton_, s->tint);
    updating_ = false;
}

void SpeakerStylePanel::reload() {
    updating_ = true;
    speakerCombo_->clear();
    if (project_) {
        for (const Speaker& s : project_->speakers) speakerCombo_->addItem(s.name);
    }
    if (speakerId_ >= speakerCombo_->count()) speakerId_ = 0;
    speakerCombo_->setCurrentIndex(speakerId_);
    updating_ = false;
    bind();
}

void SpeakerStylePanel::refreshValues() { editor_->refresh(); }

void SpeakerStylePanel::setSpeaker(int speakerId) {
    if (!project_ || speakerId < 0 || speakerId >= project_->speakers.size()) return;
    speakerId_ = speakerId;
    updating_ = true;
    speakerCombo_->setCurrentIndex(speakerId);
    updating_ = false;
    bind();
}

} // namespace dvs
