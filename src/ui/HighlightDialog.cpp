#include "HighlightDialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace dvs {

namespace {

void paintSwatch(QPushButton* button, const QColor& color) {
    button->setText(color.name(QColor::HexRgb).toUpper());
    const QString fg = color.lightnessF() > 0.55 ? QStringLiteral("#111") : QStringLiteral("#fff");
    button->setStyleSheet(QStringLiteral("background:%1; color:%2; padding:3px;")
                              .arg(color.name(QColor::HexRgb), fg));
}

} // namespace

bool promptHighlightColor(QWidget* parent, Project* project, const QString& word) {
    if (!project) return false;
    const QString key = normaliseWord(word);
    if (key.isEmpty()) return false;

    const Highlight* existing = project->highlightFor(key);
    const QColor initial = existing ? existing->color : QColor(0x1E, 0x6F, 0xE8);
    const bool bold = existing ? existing->bold : false;

    const QColor picked = QColorDialog::getColor(
        initial, parent, QStringLiteral("Colour for \"%1\"").arg(key));
    if (!picked.isValid()) return false;

    project->setHighlight(key, picked, bold);
    return true;
}

HighlightDialog::HighlightDialog(Project* project, QWidget* parent)
    : QDialog(parent), project_(project) {
    setWindowTitle(QStringLiteral("Word colours"));
    resize(460, 380);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral(
        "Words listed here are drawn in their own colour everywhere they appear.\n"
        "Matching ignores capitals and punctuation, so \"wohl\" also matches \"Wohl?\".")));

    table_ = new QTableWidget(0, 3);
    table_->setHorizontalHeaderLabels(
        {QStringLiteral("Word"), QStringLiteral("Colour"), QStringLiteral("Bold")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->setColumnWidth(1, 110);
    table_->setColumnWidth(2, 55);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(table_, 1);

    auto* buttons = new QHBoxLayout;
    auto* addButton = new QPushButton(QStringLiteral("Add word..."));
    auto* suggestButton = new QPushButton(QStringLiteral("Pick from subtitles..."));
    removeButton_ = new QPushButton(QStringLiteral("Remove"));
    buttons->addWidget(addButton);
    buttons->addWidget(suggestButton);
    buttons->addWidget(removeButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(box);

    connect(addButton, &QPushButton::clicked, this, &HighlightDialog::addWord);
    connect(suggestButton, &QPushButton::clicked, this, &HighlightDialog::suggestFromSubtitles);
    connect(removeButton_, &QPushButton::clicked, this, &HighlightDialog::removeSelected);

    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (updating_ || !project_ || item->column() != 0) return;
        const int row = item->row();
        if (row < 0 || row >= project_->highlights.size()) return;
        project_->highlights[row].word = normaliseWord(item->text());
        emit highlightsChanged();
    });

    reload();
}

void HighlightDialog::reload() {
    updating_ = true;
    table_->setRowCount(0);
    if (project_) {
        for (int i = 0; i < project_->highlights.size(); ++i) addRow(i);
    }
    updating_ = false;
    removeButton_->setEnabled(table_->rowCount() > 0);
}

void HighlightDialog::addRow(int index) {
    const Highlight& h = project_->highlights.at(index);
    const int row = table_->rowCount();
    table_->insertRow(row);

    table_->setItem(row, 0, new QTableWidgetItem(h.word));

    auto* swatch = new QPushButton;
    paintSwatch(swatch, h.color);
    connect(swatch, &QPushButton::clicked, this, [this, row, swatch] {
        if (row >= project_->highlights.size()) return;
        const QColor picked = QColorDialog::getColor(
            project_->highlights.at(row).color, this, QStringLiteral("Choose a colour"));
        if (!picked.isValid()) return;
        project_->highlights[row].color = picked;
        paintSwatch(swatch, picked);
        emit highlightsChanged();
    });
    table_->setCellWidget(row, 1, swatch);

    auto* bold = new QCheckBox;
    bold->setChecked(h.bold);
    connect(bold, &QCheckBox::toggled, this, [this, row](bool on) {
        if (row >= project_->highlights.size()) return;
        project_->highlights[row].bold = on;
        emit highlightsChanged();
    });
    table_->setCellWidget(row, 2, bold);
}

void HighlightDialog::addWord() {
    bool ok = false;
    const QString word = QInputDialog::getText(this, QStringLiteral("Add word"),
                                               QStringLiteral("Word to highlight:"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || normaliseWord(word).isEmpty()) return;
    if (!promptHighlightColor(this, project_, word)) return;
    reload();
    emit highlightsChanged();
}

void HighlightDialog::suggestFromSubtitles() {
    if (!project_) return;

    // Offer every distinct word in the subtitles, longest first - the target
    // vocabulary is rarely a two-letter function word.
    QMap<QString, int> counts;
    for (const Segment& s : project_->segments) {
        const QStringList words =
            s.text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
        for (const QString& w : words) {
            const QString key = normaliseWord(w);
            if (key.size() >= 3) counts[key]++;
        }
    }
    if (counts.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("No subtitles"),
                                 QStringLiteral("Load a subtitle file first."));
        return;
    }

    QStringList choices = counts.keys();
    std::sort(choices.begin(), choices.end(), [&](const QString& a, const QString& b) {
        if (counts[a] != counts[b]) return counts[a] > counts[b];
        return a < b;
    });

    bool ok = false;
    const QString word = QInputDialog::getItem(this, QStringLiteral("Pick from subtitles"),
                                               QStringLiteral("Word to highlight:"), choices, 0,
                                               false, &ok);
    if (!ok || word.isEmpty()) return;
    if (!promptHighlightColor(this, project_, word)) return;
    reload();
    emit highlightsChanged();
}

void HighlightDialog::removeSelected() {
    const int row = table_->currentRow();
    if (!project_ || row < 0 || row >= project_->highlights.size()) return;
    project_->highlights.removeAt(row);
    reload();
    emit highlightsChanged();
}

void HighlightDialog::focusWord(const QString& word) {
    if (!project_) return;
    const QString key = normaliseWord(word);
    if (key.isEmpty()) return;

    if (!project_->highlightFor(key)) {
        project_->setHighlight(key, QColor(0x1E, 0x6F, 0xE8), false);
        reload();
        emit highlightsChanged();
    }
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (table_->item(row, 0) && normaliseWord(table_->item(row, 0)->text()) == key) {
            table_->selectRow(row);
            table_->scrollToItem(table_->item(row, 0));
            return;
        }
    }
}

} // namespace dvs
