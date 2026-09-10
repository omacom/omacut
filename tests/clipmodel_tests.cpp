#include <QtTest>
#include <QAbstractItemModelTester>
#include <limits>
#include "clipmodel.h"

class ClipModelTests : public QObject {
    Q_OBJECT
private slots:
    void resetAndRoles() {
        ClipModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(model.count(), 0);
        QCOMPARE(model.selectedIndex(), -1);
        model.reset(30);
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.duration(), 30.0);
        QCOMPARE(model.selectedIndex(), 0);
        QCOMPARE(model.data(model.index(0), ClipModel::SourceStartRole).toDouble(), 0.0);
        QCOMPARE(model.data(model.index(0), ClipModel::SourceEndRole).toDouble(), 30.0);
        QCOMPARE(model.roleNames()[ClipModel::TimelineStartRole], QByteArray("timelineStartSec"));
        QVERIFY(!model.data({}, ClipModel::LengthRole).isValid());
    }
    void repeatedSplits() {
        ClipModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.reset(30);
        QVERIFY(model.split(10));
        QVERIFY(model.split(20));
        QCOMPARE(model.count(), 3);
        QCOMPARE(model.duration(), 30.0);
        QCOMPARE(model.selectedIndex(), 2);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(model.data(model.index(i), ClipModel::SourceStartRole).toDouble(), i * 10.0);
            QCOMPARE(model.data(model.index(i), ClipModel::LengthRole).toDouble(), 10.0);
        }
    }
    void splitEdgesAndInvalidTimes() {
        ClipModel model;
        QVERIFY(!model.split(0));
        model.reset(10);
        for (double t : {-1.0, 0.0, 0.05, 9.95, 10.0, 11.0,
                         std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
            QVERIFY(!model.split(t));
        QVERIFY(model.split(0.1));
        QVERIFY(!model.split(0.1));
        QCOMPARE(model.count(), 2);
    }
    void deletionClosesGapAndMapsTime() {
        ClipModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.reset(30);
        model.split(10);
        model.split(20);
        model.select(1);
        QVERIFY(model.removeSelected());
        QCOMPARE(model.duration(), 20.0);
        QCOMPARE(model.selectedIndex(), 1);
        QCOMPARE(model.clipAt(9.5), 0);
        QCOMPARE(model.clipAt(10), 1);
        QCOMPARE(model.sourceTime(10), 20.0);
        QCOMPARE(model.sourceTime(15), 25.0);
        QCOMPARE(model.sourceTime(20), 30.0);
        QCOMPARE(model.timelineTime(1, 25), 15.0);
        QCOMPARE(model.timelineTime(1, 15), -1.0);
        QCOMPARE(model.sourceTime(21), -1.0);
        QVERIFY(model.split(15));
        QCOMPARE(model.snapshot()[1].toMap()["sourceEndSec"].toDouble(), 25.0);
    }
    void deletingLastClip() {
        ClipModel model;
        model.reset(10);
        model.split(5);
        QVERIFY(model.removeSelected());
        QCOMPARE(model.selectedIndex(), 0);
        QVERIFY(model.removeSelected());
        QCOMPARE(model.selectedIndex(), -1);
        QCOMPARE(model.count(), 0);
        QCOMPARE(model.duration(), 0.0);
        QCOMPARE(model.clipAt(0), -1);
        QCOMPARE(model.sourceTime(0), -1.0);
        QVERIFY(!model.removeSelected());
        QVERIFY(!model.split(0));
    }
    void resizingBoundsAndNeighbors() {
        ClipModel model;
        model.reset(30);
        model.split(10);
        model.split(20);
        QVERIFY(!model.resizeClip(1, 9, 20));
        QVERIFY(!model.resizeClip(1, 10, 21));
        QVERIFY(!model.resizeClip(0, -1, 10));
        QVERIFY(!model.resizeClip(2, 20, 31));
        QVERIFY(!model.resizeClip(1, 15, 15));
        QVERIFY(!model.resizeClip(1, 16, 15));
        QVERIFY(!model.resizeClip(1, 15, std::numeric_limits<double>::quiet_NaN()));
        QVERIFY(!model.resizeClip(-1, 0, 1));
        QVERIFY(model.resizeClip(1, 12, 18));
        QCOMPARE(model.duration(), 26.0);
        QCOMPARE(model.sourceTime(10), 12.0);
        QCOMPARE(model.sourceTime(16), 20.0);
        model.select(1);
        model.removeSelected();
        QVERIFY(model.resizeClip(0, 0, 15));
        QCOMPARE(model.duration(), 25.0);
    }
    void snapshotIsIndependent() {
        ClipModel model;
        model.reset(10);
        const auto snapshot = model.snapshot();
        model.split(5);
        model.removeSelected();
        QCOMPARE(snapshot.size(), 1);
        QCOMPARE(snapshot[0].toMap()["sourceEndSec"].toDouble(), 10.0);
        QCOMPARE(model.snapshot()[0].toMap()["sourceEndSec"].toDouble(), 5.0);
    }
    void resetInvalidAndShortSources() {
        ClipModel model;
        for (double duration : {-1.0, 0.0, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()}) {
            model.reset(duration);
            QCOMPARE(model.count(), 0);
        }
        model.reset(0.05);
        QCOMPARE(model.count(), 1);
        QVERIFY(!model.split(0.025));
        QVERIFY(model.resizeClip(0, 0, 0.05));
        model.reset(10);
        QCOMPARE(model.selectedIndex(), 0);
    }
    void selectionAndNotifications() {
        ClipModel model;
        QSignalSpy changed(&model, &ClipModel::changed);
        QSignalSpy selected(&model, &ClipModel::selectionChanged);
        model.reset(10);
        QCOMPARE(changed.count(), 1);
        model.select(99);
        QCOMPARE(model.selectedIndex(), 0);
        model.select(-1);
        QVERIFY(!model.removeSelected());
        QVERIFY(model.split(5));
        QCOMPARE(selected.count(), 3);
        QCOMPARE(changed.count(), 2);
        QVERIFY(!model.split(5));
        QCOMPARE(changed.count(), 2);
    }
};

QTEST_GUILESS_MAIN(ClipModelTests)
#include "clipmodel_tests.moc"
