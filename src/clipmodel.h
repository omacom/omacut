#pragma once

#include <QAbstractListModel>
#include <QVariantList>
#include <QVector>

// Non-destructive, source-ordered ranges from one video. Timeline time is the
// concatenation of retained ranges; source time always refers to the input.
class ClipModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(double duration READ duration NOTIFY changed)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE select NOTIFY selectionChanged)

public:
    enum Role { SourceStartRole = Qt::UserRole + 1, SourceEndRole,
                TimelineStartRole, LengthRole };
    explicit ClipModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_clips.size(); }
    double duration() const;
    int selectedIndex() const { return m_selected; }

    Q_INVOKABLE void reset(double sourceDuration);
    Q_INVOKABLE void select(int index);
    // Splitting uses concatenated timeline time and selects the right half.
    Q_INVOKABLE bool split(double timelineSeconds);
    Q_INVOKABLE bool removeSelected();
    // Reject invalid edits rather than silently clamping a handle's value.
    // Edges may expand into unused source, but never overlap a neighbor.
    Q_INVOKABLE bool resizeClip(int index, double sourceStart, double sourceEnd);
    // Interior boundaries belong to the right clip; the final endpoint belongs
    // to the last clip. Invalid/empty positions return -1.
    Q_INVOKABLE int clipAt(double timelineSeconds) const;
    Q_INVOKABLE double sourceTime(double timelineSeconds) const;
    Q_INVOKABLE double timelineTime(int index, double sourceSeconds) const;
    Q_INVOKABLE QVariantList snapshot() const;

signals:
    void changed();
    void selectionChanged();

private:
    struct Clip { double start; double end; };
    static constexpr double MinimumLength = 0.1;
    bool validIndex(int index) const;
    double timelineStart(int index) const;
    void notifyRanges();
    QVector<Clip> m_clips;
    double m_sourceDuration = 0;
    int m_selected = -1;
};
