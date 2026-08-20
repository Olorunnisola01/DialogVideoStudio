#pragma once

#include <QAbstractTableModel>
#include <QStyledItemDelegate>

#include "core/Project.h"

namespace dvs {

// Table view over Project::segments. The model does not own the project; the
// window passes a pointer and calls refresh() after bulk changes.
class SegmentTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        ColIndex = 0,
        ColStart,
        ColEnd,
        ColSpeaker,
        ColText,
        ColTranslation,
        ColumnCount
    };

    explicit SegmentTableModel(QObject* parent = nullptr);

    void setProject(Project* project);
    void refresh();

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    // Bulk reassignment for a multi-selection.
    void assignSpeaker(const QList<int>& rows, int speakerId);

signals:
    void segmentsChanged();

private:
    Project* project_ = nullptr;
};

// Renders the speaker column as a combo box listing the project's speakers.
class SpeakerDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit SpeakerDelegate(Project* project, QObject* parent = nullptr);

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override;

private:
    Project* project_ = nullptr;
};

} // namespace dvs
