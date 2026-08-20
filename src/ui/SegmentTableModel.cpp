#include "SegmentTableModel.h"

#include <QBrush>
#include <QComboBox>
#include <QFont>

#include "core/SrtParser.h"

namespace dvs {

namespace {

// Accepts "hh:mm:ss,zzz", "mm:ss,zzz", "ss.zzz" or a plain millisecond count.
bool parseTimestampInput(const QString& text, qint64* out) {
    const QString trimmed = text.trimmed();
    bool plain = false;
    const qint64 ms = trimmed.toLongLong(&plain);
    if (plain) {
        *out = ms;
        return true;
    }
    const SrtParseResult probe =
        parseSrt(QStringLiteral("1\n%1 --> %1\nx\n").arg(trimmed));
    if (!probe.ok()) return false;
    *out = probe.cues.first().startMs;
    return true;
}

} // namespace

SegmentTableModel::SegmentTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void SegmentTableModel::setProject(Project* project) {
    beginResetModel();
    project_ = project;
    endResetModel();
}

void SegmentTableModel::refresh() {
    beginResetModel();
    endResetModel();
}

int SegmentTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !project_) return 0;
    return static_cast<int>(project_->segments.size());
}

int SegmentTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant SegmentTableModel::data(const QModelIndex& index, int role) const {
    if (!project_ || !index.isValid() || index.row() >= project_->segments.size()) {
        return {};
    }
    const Segment& s = project_->segments.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        switch (index.column()) {
        case ColIndex: return index.row() + 1;
        case ColStart: return formatTimestamp(s.startMs);
        case ColEnd: return formatTimestamp(s.endMs);
        case ColSpeaker: {
            const Speaker* sp = project_->speakerFor(s);
            return sp ? sp->name : QStringLiteral("(unassigned)");
        }
        case ColText: return s.text;
        case ColTranslation: return s.translation;
        default: return {};
        }

    case Qt::BackgroundRole: {
        if (s.needsReview) return QBrush(QColor(0xFF, 0xF0, 0xC2));
        if (index.column() == ColSpeaker) {
            const Speaker* sp = project_->speakerFor(s);
            if (sp) {
                QColor c = sp->tint;
                c.setAlpha(60);
                return QBrush(c);
            }
        }
        return {};
    }

    case Qt::ToolTipRole: {
        QStringList notes;
        if (s.needsReview && !s.reviewReason.isEmpty()) {
            notes << s.reviewReason
                  << QStringLiteral("Listen to this line and set the speaker yourself.");
        } else {
            notes << QStringLiteral("Confidence %1").arg(s.confidence, 0, 'f', 2);
        }
        if (s.translationEcho) {
            notes << QStringLiteral(
                "This row keeps the pair on screen while the translation is spoken.");
        }
        return notes.join(QLatin1Char('\n'));
    }

    case Qt::FontRole:
        if (s.needsReview && index.column() == ColIndex) {
            QFont f;
            f.setBold(true);
            return f;
        }
        return {};

    case Qt::TextAlignmentRole:
        return (index.column() == ColText || index.column() == ColTranslation)
                   ? QVariant(int(Qt::AlignLeft | Qt::AlignVCenter))
                   : QVariant(int(Qt::AlignCenter));

    default:
        return {};
    }
}

QVariant SegmentTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    switch (section) {
    case ColIndex: return QStringLiteral("#");
    case ColStart: return QStringLiteral("Start");
    case ColEnd: return QStringLiteral("End");
    case ColSpeaker: return QStringLiteral("Speaker");
    case ColText: return QStringLiteral("Subtitle");
    case ColTranslation: return QStringLiteral("Translation");
    default: return {};
    }
}

Qt::ItemFlags SegmentTableModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.column() != ColIndex) f |= Qt::ItemIsEditable;
    return f;
}

bool SegmentTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!project_ || role != Qt::EditRole || !index.isValid()) return false;
    if (index.row() >= project_->segments.size()) return false;

    Segment& s = project_->segments[index.row()];
    switch (index.column()) {
    case ColStart: {
        qint64 ms = 0;
        if (!parseTimestampInput(value.toString(), &ms)) return false;
        s.startMs = ms;
        break;
    }
    case ColEnd: {
        qint64 ms = 0;
        if (!parseTimestampInput(value.toString(), &ms)) return false;
        s.endMs = ms;
        break;
    }
    case ColSpeaker: {
        bool isInt = false;
        const int id = value.toInt(&isInt);
        if (!isInt || id < 0 || id >= project_->speakers.size()) return false;
        s.speakerId = id;
        // A manual choice settles the row.
        s.needsReview = false;
        s.reviewReason.clear();
        s.confidence = 1.0f;
        break;
    }
    case ColText:
        s.text = value.toString();
        break;
    case ColTranslation:
        s.translation = value.toString();
        break;
    default:
        return false;
    }

    emit dataChanged(index.sibling(index.row(), 0),
                     index.sibling(index.row(), ColumnCount - 1));
    emit segmentsChanged();
    return true;
}

void SegmentTableModel::assignSpeaker(const QList<int>& rows, int speakerId) {
    if (!project_ || speakerId < 0 || speakerId >= project_->speakers.size()) return;
    for (int row : rows) {
        if (row < 0 || row >= project_->segments.size()) continue;
        Segment& s = project_->segments[row];
        s.speakerId = speakerId;
        s.needsReview = false;
        s.reviewReason.clear();
        s.confidence = 1.0f;
    }
    refresh();
    emit segmentsChanged();
}

// --- SpeakerDelegate -------------------------------------------------------

SpeakerDelegate::SpeakerDelegate(Project* project, QObject* parent)
    : QStyledItemDelegate(parent), project_(project) {}

QWidget* SpeakerDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&,
                                       const QModelIndex&) const {
    auto* combo = new QComboBox(parent);
    if (project_) {
        for (int i = 0; i < project_->speakers.size(); ++i) {
            combo->addItem(project_->speakers.at(i).name, i);
        }
    }
    return combo;
}

void SpeakerDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    auto* combo = qobject_cast<QComboBox*>(editor);
    if (!combo || !project_) return;
    const int row = index.row();
    if (row < 0 || row >= project_->segments.size()) return;
    const int id = project_->segments.at(row).speakerId;
    const int at = combo->findData(id);
    combo->setCurrentIndex(at >= 0 ? at : 0);
}

void SpeakerDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                   const QModelIndex& index) const {
    auto* combo = qobject_cast<QComboBox*>(editor);
    if (!combo) return;
    model->setData(index, combo->currentData(), Qt::EditRole);
}

} // namespace dvs
