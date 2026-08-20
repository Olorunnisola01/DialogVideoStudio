#include "TranslationPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include "core/SecretStore.h"
#include "ui/StyleEditor.h"

namespace dvs {

namespace {

struct LangEntry {
    const char* code;
    const char* name;
};

const QList<LangEntry>& languages() {
    static const QList<LangEntry> list = {
        {"de", "German"},   {"en", "English"},  {"fr", "French"},  {"es", "Spanish"},
        {"it", "Italian"},  {"pt", "Portuguese"}, {"nl", "Dutch"}, {"pl", "Polish"},
        {"tr", "Turkish"},  {"ru", "Russian"},  {"ar", "Arabic"},  {"yo", "Yoruba"},
        {"ha", "Hausa"},    {"ig", "Igbo"},     {"zh", "Chinese"}, {"ja", "Japanese"},
        {"ko", "Korean"},
    };
    return list;
}

void fillLanguages(QComboBox* combo, const QString& initial) {
    for (const LangEntry& entry : languages()) {
        combo->addItem(QStringLiteral("%1 (%2)").arg(QLatin1String(entry.name),
                                                     QLatin1String(entry.code)),
                       QLatin1String(entry.code));
    }
    const int at = combo->findData(initial);
    combo->setCurrentIndex(at >= 0 ? at : 0);
}

QSettings appSettings() {
    return QSettings(QStringLiteral("DialogVideoStudio"), QStringLiteral("DialogVideoStudio"));
}

} // namespace

TranslationPanel::TranslationPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    enabled_ = new QCheckBox(QStringLiteral("Show the second subtitle line"));
    outer->addWidget(enabled_);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    auto* form = new QVBoxLayout(inner);
    form->setContentsMargins(0, 0, 6, 0);

    // --- where the text comes from ---
    auto* sourceGroup = new QGroupBox(QStringLiteral("Where the text comes from"));
    auto* sourceLayout = new QVBoxLayout(sourceGroup);

    auto* pairHint = new QLabel(QStringLiteral(
        "<b>From the recording.</b> If your audio speaks each line and then its "
        "translation, pairing folds the two together so both are on screen at once. "
        "That uses your own wording and needs no internet."));
    pairHint->setWordWrap(true);
    sourceLayout->addWidget(pairHint);

    auto* pairRow = new QHBoxLayout;
    auto* pair = new QPushButton(QStringLiteral("Pair with audio"));
    auto* unpair = new QPushButton(QStringLiteral("Undo pairing"));
    pairRow->addWidget(pair);
    pairRow->addWidget(unpair);
    sourceLayout->addLayout(pairRow);

    auto* mtHint = new QLabel(QStringLiteral(
        "<b>Machine translation.</b> For material where the translation is not spoken. "
        "This sends your subtitle text to the service you pick below."));
    mtHint->setWordWrap(true);
    sourceLayout->addSpacing(6);
    sourceLayout->addWidget(mtHint);

    auto* mtForm = new QFormLayout;
    providerCombo_ = new QComboBox;
    for (const ProviderInfo& info : translationProviders()) {
        providerCombo_->addItem(info.displayName, info.id);
    }
    sourceLang_ = new QComboBox;
    targetLang_ = new QComboBox;
    fillLanguages(sourceLang_, QStringLiteral("de"));
    fillLanguages(targetLang_, QStringLiteral("en"));
    modelEdit_ = new QLineEdit;
    keyEdit_ = new QLineEdit;
    keyEdit_->setEchoMode(QLineEdit::Password);
    keyEdit_->setPlaceholderText(QStringLiteral("Paste your API key"));
    keyHint_ = new QLabel;
    keyHint_->setOpenExternalLinks(true);
    keyHint_->setWordWrap(true);
    batchSize_ = new QSpinBox;
    batchSize_->setRange(1, 100);
    batchSize_->setValue(20);
    batchSize_->setToolTip(QStringLiteral(
        "How many lines to send per request. Larger batches are faster and give the "
        "model more context; smaller batches are easier to recover from."));
    overwrite_ = new QCheckBox(QStringLiteral("Replace translations that already exist"));

    mtForm->addRow(QStringLiteral("Service"), providerCombo_);
    mtForm->addRow(QStringLiteral("From"), sourceLang_);
    mtForm->addRow(QStringLiteral("Into"), targetLang_);
    mtForm->addRow(QStringLiteral("Model"), modelEdit_);
    mtForm->addRow(QStringLiteral("API key"), keyEdit_);
    mtForm->addRow(QString(), keyHint_);
    mtForm->addRow(QStringLiteral("Lines per request"), batchSize_);
    mtForm->addRow(QString(), overwrite_);
    sourceLayout->addLayout(mtForm);

    auto* mtRow = new QHBoxLayout;
    testButton_ = new QPushButton(QStringLiteral("Test"));
    translateButton_ = new QPushButton(QStringLiteral("Translate all lines"));
    mtRow->addWidget(testButton_);
    mtRow->addWidget(translateButton_, 1);
    sourceLayout->addLayout(mtRow);

    form->addWidget(sourceGroup);

    editor_ = new StyleEditor;
    form->addWidget(editor_);
    form->addStretch(1);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    // --- wiring ---
    connect(pair, &QPushButton::clicked, this, &TranslationPanel::pairRequested);
    connect(unpair, &QPushButton::clicked, this, &TranslationPanel::unpairRequested);
    connect(translateButton_, &QPushButton::clicked, this, [this] {
        saveKey();
        emit translateRequested();
    });
    connect(testButton_, &QPushButton::clicked, this, &TranslationPanel::testProvider);

    connect(editor_, &StyleEditor::changed, this, &TranslationPanel::styleChanged);
    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_ || !project_) return;
        project_->translationEnabled = on;
        editor_->setEnabled(on);
        emit styleChanged();
    });

    connect(providerCombo_, &QComboBox::currentIndexChanged, this,
            &TranslationPanel::onProviderChanged);
    connect(keyEdit_, &QLineEdit::editingFinished, this, &TranslationPanel::saveKey);

    // Remember the language pair and batch size across sessions.
    const auto remember = [this] {
        if (updating_) return;
        QSettings s = appSettings();
        s.setValue(QStringLiteral("translate/provider"), providerCombo_->currentData());
        s.setValue(QStringLiteral("translate/source"), sourceLang_->currentData());
        s.setValue(QStringLiteral("translate/target"), targetLang_->currentData());
        s.setValue(QStringLiteral("translate/batch"), batchSize_->value());
        s.setValue(QStringLiteral("translate/model/%1").arg(currentProvider().id),
                   modelEdit_->text());
    };
    connect(sourceLang_, &QComboBox::currentIndexChanged, this, remember);
    connect(targetLang_, &QComboBox::currentIndexChanged, this, remember);
    connect(batchSize_, &QSpinBox::valueChanged, this, remember);
    connect(modelEdit_, &QLineEdit::editingFinished, this, remember);
    connect(providerCombo_, &QComboBox::currentIndexChanged, this, remember);

    // Restore the last choices.
    updating_ = true;
    QSettings s = appSettings();
    const int providerAt =
        providerCombo_->findData(s.value(QStringLiteral("translate/provider")));
    if (providerAt >= 0) providerCombo_->setCurrentIndex(providerAt);
    const int sourceAt = sourceLang_->findData(s.value(QStringLiteral("translate/source")));
    if (sourceAt >= 0) sourceLang_->setCurrentIndex(sourceAt);
    const int targetAt = targetLang_->findData(s.value(QStringLiteral("translate/target")));
    if (targetAt >= 0) targetLang_->setCurrentIndex(targetAt);
    batchSize_->setValue(s.value(QStringLiteral("translate/batch"), 20).toInt());
    updating_ = false;

    onProviderChanged();
}

ProviderInfo TranslationPanel::currentProvider() const {
    TranslationProvider provider = TranslationProvider::GoogleFree;
    providerFromId(providerCombo_->currentData().toString(), &provider);
    return providerInfo(provider);
}

void TranslationPanel::onProviderChanged() {
    const ProviderInfo info = currentProvider();

    modelEdit_->setEnabled(info.needsKey);
    keyEdit_->setEnabled(info.needsKey);
    batchSize_->setEnabled(info.needsKey);

    updating_ = true;
    if (info.needsKey) {
        const QString saved =
            appSettings()
                .value(QStringLiteral("translate/model/%1").arg(info.id), info.defaultModel)
                .toString();
        modelEdit_->setText(saved);
        // Never put the stored key back on screen; just say whether there is one.
        keyEdit_->clear();
        keyEdit_->setPlaceholderText(secrets::hasApiKey(info.id)
                                         ? QStringLiteral("A key is saved - type to replace it")
                                         : QStringLiteral("Paste your API key"));
        keyHint_->setText(QStringLiteral(
                              "Stored encrypted for your Windows account. "
                              "<a href=\"%1\">Get a key</a>")
                              .arg(info.keyUrl));
    } else {
        modelEdit_->clear();
        keyEdit_->clear();
        keyEdit_->setPlaceholderText(QStringLiteral("Not needed"));
        keyHint_->setText(
            info.provider == TranslationProvider::GoogleFree
                ? QStringLiteral("No key and no account. Uses the endpoint Google Translate's "
                                 "own web page calls, so it can rate-limit or change without "
                                 "notice - check the results.")
                : QStringLiteral("No key. A documented free tier with a modest daily "
                                 "allowance; quality is below the paid services."));
    }
    updating_ = false;
}

void TranslationPanel::saveKey() {
    const ProviderInfo info = currentProvider();
    if (!info.needsKey || keyEdit_->text().isEmpty()) return;
    secrets::setApiKey(info.id, keyEdit_->text().trimmed());
    keyEdit_->clear();
    keyEdit_->setPlaceholderText(QStringLiteral("A key is saved - type to replace it"));
}

TranslationSettings TranslationPanel::translationSettings() const {
    TranslationSettings ts;
    const ProviderInfo info = currentProvider();
    ts.provider = info.provider;
    ts.sourceLang = sourceLang_->currentData().toString();
    ts.targetLang = targetLang_->currentData().toString();
    ts.model = modelEdit_->text().trimmed().isEmpty() ? info.defaultModel
                                                      : modelEdit_->text().trimmed();
    ts.batchSize = batchSize_->value();
    ts.overwriteExisting = overwrite_->isChecked();
    if (info.needsKey) {
        ts.apiKey = keyEdit_->text().trimmed().isEmpty() ? secrets::apiKey(info.id)
                                                         : keyEdit_->text().trimmed();
    }
    return ts;
}

void TranslationPanel::testProvider() {
    saveKey();

    bool ok = false;
    const QString sample = QInputDialog::getText(
        this, QStringLiteral("Test the translator"),
        QStringLiteral("Send one line to %1 and show what comes back:")
            .arg(currentProvider().displayName),
        QLineEdit::Normal, QStringLiteral("Guten Tag, wie geht es Ihnen?"), &ok);
    if (!ok || sample.trimmed().isEmpty()) return;

    setCursor(Qt::BusyCursor);
    QString error;
    const QString result = translateOne(sample.trimmed(), translationSettings(), &error);
    unsetCursor();

    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Test failed"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("Test result"),
                             QStringLiteral("%1\n\n%2").arg(sample.trimmed(), result));
}

void TranslationPanel::setProject(Project* project) {
    project_ = project;
    reload();
}

void TranslationPanel::reload() {
    updating_ = true;
    enabled_->setChecked(project_ ? project_->translationEnabled : false);
    updating_ = false;
    editor_->setStyle(project_ ? &project_->translationStyle : nullptr);
    editor_->setEnabled(project_ && project_->translationEnabled);
}

void TranslationPanel::refreshValues() { editor_->refresh(); }

} // namespace dvs
