#include "clipmodel.h"

#include <algorithm>
#include <cmath>

ClipModel::ClipModel(QObject *parent) : QAbstractListModel(parent) {}

int ClipModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : count();
}

bool ClipModel::validIndex(int index) const {
    return index >= 0 && index < count();
}

QVariant ClipModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.model() != this || !validIndex(index.row()) || index.column() != 0)
        return {};
    const auto &clip = m_clips[index.row()];
    switch (role) {
    case SourceStartRole: return clip.start;
    case SourceEndRole: return clip.end;
    case TimelineStartRole: return timelineStart(index.row());
    case LengthRole: return clip.end - clip.start;
    default: return {};
    }
}

QHash<int, QByteArray> ClipModel::roleNames() const {
    return {{SourceStartRole, "sourceStartSec"}, {SourceEndRole, "sourceEndSec"},
            {TimelineStartRole, "timelineStartSec"}, {LengthRole, "lengthSec"}};
}

double ClipModel::timelineStart(int index) const {
    double result = 0;
    for (int i = 0; i < index; ++i)
        result += m_clips[i].end - m_clips[i].start;
    return result;
}

double ClipModel::duration() const { return timelineStart(count()); }

void ClipModel::reset(double sourceDuration) {
    beginResetModel();
    m_sourceDuration = std::isfinite(sourceDuration) && sourceDuration > 0 ? sourceDuration : 0;
    m_clips.clear();
    if (m_sourceDuration > 0)
        m_clips.append({0, m_sourceDuration});
    m_selected = m_clips.isEmpty() ? -1 : 0;
    endResetModel();
    emit changed();
    emit selectionChanged();
}

void ClipModel::select(int index) {
    if ((!validIndex(index) && index != -1) || m_selected == index)
        return;
    m_selected = index;
    emit selectionChanged();
}

void ClipModel::notifyRanges() {
    // Edits also shift all subsequent timelineStartSec roles.
    if (!m_clips.isEmpty())
        emit dataChanged(index(0), index(count() - 1));
    emit changed();
}

int ClipModel::clipAt(double seconds) const {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > duration() || m_clips.isEmpty())
        return -1;
    double end = 0;
    for (int i = 0; i < count(); ++i) {
        end += m_clips[i].end - m_clips[i].start;
        if (seconds < end)
            return i;
    }
    return count() - 1;
}

double ClipModel::sourceTime(double seconds) const {
    const int i = clipAt(seconds);
    return i < 0 ? -1 : m_clips[i].start + seconds - timelineStart(i);
}

double ClipModel::timelineTime(int i, double seconds) const {
    if (!validIndex(i) || !std::isfinite(seconds) || seconds < m_clips[i].start || seconds > m_clips[i].end)
        return -1;
    return timelineStart(i) + seconds - m_clips[i].start;
}

bool ClipModel::split(double seconds) {
    const int i = clipAt(seconds);
    if (i < 0)
        return false;
    const Clip original = m_clips[i];
    const double cut = sourceTime(seconds);
    // Tolerate floating-point rounding at the minimum-length boundary.
    if (cut - original.start < MinimumLength - 1e-9 || original.end - cut < MinimumLength - 1e-9)
        return false;
    beginInsertRows({}, i + 1, i + 1);
    m_clips[i].end = cut;
    m_clips.insert(i + 1, {cut, original.end});
    endInsertRows();
    m_selected = i + 1;
    emit selectionChanged();
    notifyRanges();
    return true;
}

bool ClipModel::removeSelected() {
    if (!validIndex(m_selected))
        return false;
    const int removed = m_selected;
    beginRemoveRows({}, removed, removed);
    m_clips.removeAt(removed);
    m_selected = m_clips.isEmpty() ? -1 : std::min(removed, count() - 1);
    endRemoveRows();
    // Notify even if the numeric index is unchanged: it now names a new clip.
    emit selectionChanged();
    notifyRanges();
    return true;
}

bool ClipModel::resizeClip(int i, double start, double end) {
    if (!validIndex(i) || !std::isfinite(start) || !std::isfinite(end))
        return false;
    const double lower = i == 0 ? 0 : m_clips[i - 1].end;
    const double upper = i == count() - 1 ? m_sourceDuration : m_clips[i + 1].start;
    if (start < lower || end > upper || end - start < std::min(MinimumLength, m_sourceDuration) - 1e-9)
        return false;
    if (m_clips[i].start == start && m_clips[i].end == end)
        return true;
    m_clips[i] = {start, end};
    notifyRanges();
    return true;
}

QVariantList ClipModel::snapshot() const {
    QVariantList result;
    for (const auto &clip : m_clips)
        result.append(QVariantMap{{"sourceStartSec", clip.start}, {"sourceEndSec", clip.end}});
    return result;
}
