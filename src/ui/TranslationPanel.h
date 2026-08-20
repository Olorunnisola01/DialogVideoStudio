#pragma once

#include <QWidget>

#include "core/Project.h"
#include "core/TranslationService.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace dvs {

class StyleEditor;

// Turns the English line on and off, styles it, and drives the two ways of
// getting the text: pairing it out of the recording, or machine translation.
class TranslationPanel : public QWidget {
    Q_OBJECT

public:
    explicit TranslationPanel(QWidget* parent = nullptr);

    void setProject(Project* project);
    void reload();
    void refreshValues();

    // Provider, languages, model and key as currently shown. The key comes from
    // the encrypted store unless the field was edited this session.
    TranslationSettings translationSettings() const;

signals:
    void styleChanged();
    void pairRequested();
    void unpairRequested();
    void translateRequested();

private:
    void onProviderChanged();
    void saveKey();
    void testProvider();
    ProviderInfo currentProvider() const;

    Project* project_ = nullptr;
    bool updating_ = false;

    QCheckBox* enabled_ = nullptr;
    StyleEditor* editor_ = nullptr;

    QComboBox* providerCombo_ = nullptr;
    QComboBox* sourceLang_ = nullptr;
    QComboBox* targetLang_ = nullptr;
    QLineEdit* modelEdit_ = nullptr;
    QLineEdit* keyEdit_ = nullptr;
    QLabel* keyHint_ = nullptr;
    QSpinBox* batchSize_ = nullptr;
    QCheckBox* overwrite_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QPushButton* translateButton_ = nullptr;
};

} // namespace dvs
