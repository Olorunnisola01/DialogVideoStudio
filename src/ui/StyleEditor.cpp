#include "StyleEditor.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace dvs {

namespace {

QDoubleSpinBox* makeSpin(double min, double max, double step, int decimals) {
    auto* box = new QDoubleSpinBox;
    box->setRange(min, max);
    box->setSingleStep(step);
    box->setDecimals(decimals);
    return box;
}

} // namespace

StyleEditor::StyleEditor(QWidget* parent) : QWidget(parent) {
    auto* form = new QVBoxLayout(this);
    form->setContentsMargins(0, 0, 0, 0);

    // --- box ---
    boxGroup_ = new QGroupBox(QStringLiteral("Background"));
    auto* boxForm = new QFormLayout(boxGroup_);
    boxColorButton_ = new QPushButton;
    opacity_ = makeSpin(0.0, 1.0, 0.05, 2);
    radius_ = makeSpin(0.0, 0.20, 0.002, 3);
    padX_ = makeSpin(0.0, 0.20, 0.002, 3);
    padY_ = makeSpin(0.0, 0.20, 0.002, 3);
    autoHeight_ = new QCheckBox(QStringLiteral("Shrink to fit the text"));
    boxForm->addRow(QStringLiteral("Fill"), boxColorButton_);
    boxForm->addRow(QStringLiteral("Opacity"), opacity_);
    boxForm->addRow(QStringLiteral("Corner radius"), radius_);
    boxForm->addRow(QStringLiteral("Padding X"), padX_);
    boxForm->addRow(QStringLiteral("Padding Y"), padY_);
    boxForm->addRow(QString(), autoHeight_);
    form->addWidget(boxGroup_);

    // --- position ---
    posGroup_ = new QGroupBox(QStringLiteral("Position (fraction of frame)"));
    auto* posForm = new QFormLayout(posGroup_);
    boxX_ = makeSpin(-1.0, 2.0, 0.005, 3);
    boxY_ = makeSpin(-1.0, 2.0, 0.005, 3);
    boxW_ = makeSpin(0.02, 2.0, 0.005, 3);
    boxH_ = makeSpin(0.02, 2.0, 0.005, 3);
    posForm->addRow(QStringLiteral("X"), boxX_);
    posForm->addRow(QStringLiteral("Y"), boxY_);
    posForm->addRow(QStringLiteral("Width"), boxW_);
    posForm->addRow(QStringLiteral("Height"), boxH_);
    posGroup_->setToolTip(QStringLiteral("You can also drag the box directly in the preview."));
    form->addWidget(posGroup_);

    // --- text ---
    textGroup_ = new QGroupBox(QStringLiteral("Text"));
    auto* textForm = new QFormLayout(textGroup_);
    textColorButton_ = new QPushButton;
    fontCombo_ = new QFontComboBox;
    fontSize_ = makeSpin(0.005, 0.40, 0.002, 3);
    fontWeight_ = new QSpinBox;
    fontWeight_->setRange(100, 900);
    fontWeight_->setSingleStep(100);
    italic_ = new QCheckBox(QStringLiteral("Italic"));
    lineSpacing_ = makeSpin(0.7, 2.5, 0.02, 2);
    alignCombo_ = new QComboBox;
    alignCombo_->addItem(QStringLiteral("Centre"), static_cast<int>(Qt::AlignCenter));
    alignCombo_->addItem(QStringLiteral("Left"),
                         static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter));
    alignCombo_->addItem(QStringLiteral("Right"),
                         static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
    alignCombo_->addItem(QStringLiteral("Top left"),
                         static_cast<int>(Qt::AlignLeft | Qt::AlignTop));
    textForm->addRow(QStringLiteral("Colour"), textColorButton_);
    textForm->addRow(QStringLiteral("Font"), fontCombo_);
    textForm->addRow(QStringLiteral("Size"), fontSize_);
    textForm->addRow(QStringLiteral("Weight"), fontWeight_);
    textForm->addRow(QStringLiteral("Line spacing"), lineSpacing_);
    textForm->addRow(QStringLiteral("Alignment"), alignCombo_);
    textForm->addRow(QString(), italic_);
    form->addWidget(textGroup_);

    // --- effects ---
    fxGroup_ = new QGroupBox(QStringLiteral("Effects"));
    auto* fxForm = new QFormLayout(fxGroup_);
    shadow_ = new QCheckBox(QStringLiteral("Drop shadow"));
    shadowColorButton_ = new QPushButton;
    shadowOffset_ = makeSpin(0.0, 0.05, 0.001, 3);
    outline_ = new QCheckBox(QStringLiteral("Outline"));
    outlineColorButton_ = new QPushButton;
    outlineWidth_ = makeSpin(0.0, 0.03, 0.001, 3);
    fxForm->addRow(QString(), shadow_);
    fxForm->addRow(QStringLiteral("Shadow colour"), shadowColorButton_);
    fxForm->addRow(QStringLiteral("Shadow offset"), shadowOffset_);
    fxForm->addRow(QString(), outline_);
    fxForm->addRow(QStringLiteral("Outline colour"), outlineColorButton_);
    fxForm->addRow(QStringLiteral("Outline width"), outlineWidth_);
    form->addWidget(fxGroup_);

    // --- wiring ---
    connect(boxColorButton_, &QPushButton::clicked, this, [this] {
        if (style_) pickColor(boxColorButton_, &style_->boxColor);
    });
    connect(textColorButton_, &QPushButton::clicked, this, [this] {
        if (style_) pickColor(textColorButton_, &style_->textColor);
    });
    connect(shadowColorButton_, &QPushButton::clicked, this, [this] {
        if (style_) pickColor(shadowColorButton_, &style_->shadowColor);
    });
    connect(outlineColorButton_, &QPushButton::clicked, this, [this] {
        if (style_) pickColor(outlineColorButton_, &style_->outlineColor);
    });

    const auto onEdit = [this] { pullFromUi(); };
    for (QDoubleSpinBox* box : {opacity_, radius_, padX_, padY_, fontSize_, lineSpacing_,
                                shadowOffset_, outlineWidth_, boxX_, boxY_, boxW_, boxH_}) {
        connect(box, &QDoubleSpinBox::valueChanged, this, onEdit);
    }
    connect(fontWeight_, &QSpinBox::valueChanged, this, onEdit);
    for (QCheckBox* box : {autoHeight_, italic_, shadow_, outline_}) {
        connect(box, &QCheckBox::toggled, this, onEdit);
    }
    connect(fontCombo_, &QFontComboBox::currentFontChanged, this, onEdit);
    connect(alignCombo_, &QComboBox::currentIndexChanged, this, onEdit);

    pushToUi();
}

void StyleEditor::setStyle(SubtitleStyle* style) {
    style_ = style;
    pushToUi();
}

void StyleEditor::refresh() { pushToUi(); }

void StyleEditor::setTextSectionsVisible(bool visible) {
    boxGroup_->setVisible(visible);
    textGroup_->setVisible(visible);
    fxGroup_->setVisible(visible);
}

void StyleEditor::updateColorButton(QPushButton* button, const QColor& color) {
    button->setText(color.name(QColor::HexRgb).toUpper());
    const QString fg = color.lightnessF() > 0.55 ? QStringLiteral("#111") : QStringLiteral("#fff");
    button->setStyleSheet(QStringLiteral("background:%1; color:%2; padding:4px;")
                              .arg(color.name(QColor::HexRgb), fg));
}

void StyleEditor::pickColor(QPushButton* button, QColor* target) {
    const QColor picked = QColorDialog::getColor(*target, this, QStringLiteral("Choose a colour"),
                                                 QColorDialog::ShowAlphaChannel);
    if (!picked.isValid()) return;
    *target = picked;
    updateColorButton(button, picked);
    emit changed();
}

void StyleEditor::pushToUi() {
    setEnabled(style_ != nullptr);
    if (!style_) return;

    updating_ = true;
    const SubtitleStyle& st = *style_;

    updateColorButton(boxColorButton_, st.boxColor);
    updateColorButton(textColorButton_, st.textColor);
    updateColorButton(shadowColorButton_, st.shadowColor);
    updateColorButton(outlineColorButton_, st.outlineColor);

    opacity_->setValue(st.boxOpacity);
    radius_->setValue(st.cornerRadius);
    padX_->setValue(st.paddingX);
    padY_->setValue(st.paddingY);
    autoHeight_->setChecked(st.autoHeight);

    boxX_->setValue(st.box.x());
    boxY_->setValue(st.box.y());
    boxW_->setValue(st.box.width());
    boxH_->setValue(st.box.height());

    fontCombo_->setCurrentFont(QFont(st.fontFamily));
    fontSize_->setValue(st.fontSize);
    fontWeight_->setValue(st.fontWeight);
    italic_->setChecked(st.italic);
    lineSpacing_->setValue(st.lineSpacing);
    const int alignAt = alignCombo_->findData(st.textAlign);
    alignCombo_->setCurrentIndex(alignAt >= 0 ? alignAt : 0);

    shadow_->setChecked(st.shadowEnabled);
    shadowOffset_->setValue(st.shadowOffset);
    outline_->setChecked(st.outlineEnabled);
    outlineWidth_->setValue(st.outlineWidth);

    updating_ = false;
}

void StyleEditor::pullFromUi() {
    if (updating_ || !style_) return;

    SubtitleStyle& st = *style_;
    st.boxOpacity = opacity_->value();
    st.cornerRadius = radius_->value();
    st.paddingX = padX_->value();
    st.paddingY = padY_->value();
    st.autoHeight = autoHeight_->isChecked();
    st.box = QRectF(boxX_->value(), boxY_->value(), boxW_->value(), boxH_->value());

    st.fontFamily = fontCombo_->currentFont().family();
    st.fontSize = fontSize_->value();
    st.fontWeight = fontWeight_->value();
    st.italic = italic_->isChecked();
    st.lineSpacing = lineSpacing_->value();
    st.textAlign = alignCombo_->currentData().toInt();

    st.shadowEnabled = shadow_->isChecked();
    st.shadowOffset = shadowOffset_->value();
    st.outlineEnabled = outline_->isChecked();
    st.outlineWidth = outlineWidth_->value();

    emit changed();
}

} // namespace dvs
