#include "playqueue.h"

#include <QAbstractItemModelTester>
#include <QRandomGenerator>
#include <QTest>

namespace {

Track makeTrack(qint64 id, const QString &album = QStringLiteral("A"))
{
    Track t;
    t.id = id;
    t.title = QStringLiteral("t%1").arg(id);
    t.artist = QStringLiteral("artist");
    t.album = album;
    return t;
}

QList<Track> makeTracks(int n)
{
    QList<Track> out;
    for (int i = 1; i <= n; ++i)
        out.append(makeTrack(i));
    return out;
}

QList<qint64> ids(const PlayQueue &q)
{
    QList<qint64> out;
    for (int i = 0; i < q.count(); ++i)
        out.append(q.at(i)->id);
    return out;
}

} // namespace

class TestPlayQueue : public QObject
{
    Q_OBJECT

private slots:
    void replaceSetsCurrent()
    {
        PlayQueue q;
        QAbstractItemModelTester tester(&q, QAbstractItemModelTester::FailureReportingMode::QtTest);
        q.replace(makeTracks(5), 2);
        QCOMPARE(q.count(), 5);
        QCOMPARE(q.currentIndex(), 2);
        QCOMPARE(q.current()->id, 3);

        q.replace(makeTracks(3), 99);   // clamped
        QCOMPARE(q.currentIndex(), 2);
        q.replace({}, 0);
        QCOMPARE(q.currentIndex(), -1);
        QVERIFY(!q.current());
    }

    void insertNextAndAppend()
    {
        PlayQueue q;
        QAbstractItemModelTester tester(&q, QAbstractItemModelTester::FailureReportingMode::QtTest);
        q.replace(makeTracks(3), 1);
        q.insertNext(makeTrack(10));
        q.append(makeTrack(11));
        q.append(makeTrack(10));   // duplicates are allowed
        QCOMPARE(ids(q), (QList<qint64>{1, 2, 10, 3, 11, 10}));
        QCOMPARE(q.currentIndex(), 1);
        QVERIFY(q.uidAt(2) != q.uidAt(5));
    }

    void insertIntoEmpty()
    {
        PlayQueue q;
        q.insertNext(makeTrack(1));
        QCOMPARE(q.count(), 1);
        QCOMPARE(q.currentIndex(), -1);   // the Player decides whether to start
        QVERIFY(q.setCurrentIndex(0));
        QCOMPARE(q.current()->id, 1);
    }

    void removeAdjustsCurrent()
    {
        PlayQueue q;
        QAbstractItemModelTester tester(&q, QAbstractItemModelTester::FailureReportingMode::QtTest);
        q.replace(makeTracks(5), 2);        // current = id 3
        q.removeAt(0);                      // before current
        QCOMPARE(q.currentIndex(), 1);
        QCOMPARE(q.current()->id, 3);
        q.removeAt(3);                      // after current
        QCOMPARE(q.currentIndex(), 1);
        QCOMPARE(q.current()->id, 3);
        q.removeAt(1);                      // the current one: the next slides in
        QCOMPARE(q.currentIndex(), 1);
        QCOMPARE(q.current()->id, 4);
        q.removeAt(1);                      // current and last: clamp
        QCOMPARE(q.currentIndex(), 0);
        QCOMPARE(q.current()->id, 2);
        q.removeAt(0);
        QCOMPARE(q.currentIndex(), -1);
        QCOMPARE(q.count(), 0);
        q.removeAt(0);                      // out of range is a no-op
    }

    void moveAdjustsCurrent()
    {
        PlayQueue q;
        QAbstractItemModelTester tester(&q, QAbstractItemModelTester::FailureReportingMode::QtTest);
        q.replace(makeTracks(5), 2);        // [1 2 (3) 4 5]
        q.move(2, 4);                       // move current down
        QCOMPARE(ids(q), (QList<qint64>{1, 2, 4, 5, 3}));
        QCOMPARE(q.currentIndex(), 4);
        q.move(0, 4);                       // from above current to its slot
        QCOMPARE(ids(q), (QList<qint64>{2, 4, 5, 3, 1}));
        QCOMPARE(q.currentIndex(), 3);
        QCOMPARE(q.current()->id, 3);
        q.move(4, 0);                       // from below current to the top
        QCOMPARE(ids(q), (QList<qint64>{1, 2, 4, 5, 3}));
        QCOMPARE(q.currentIndex(), 4);
        q.move(1, 2);                       // unrelated rows
        QCOMPARE(ids(q), (QList<qint64>{1, 4, 2, 5, 3}));
        QCOMPARE(q.current()->id, 3);
        q.move(0, 9);                       // invalid: no-op
        QCOMPARE(ids(q), (QList<qint64>{1, 4, 2, 5, 3}));
    }

    void nextPrevious()
    {
        PlayQueue q;
        QCOMPARE(q.nextIndex(true), -1);
        QCOMPARE(q.previousIndex(true), -1);
        q.replace(makeTracks(3), 0);
        QCOMPARE(q.nextIndex(false), 1);
        QCOMPARE(q.previousIndex(false), -1);
        QCOMPARE(q.previousIndex(true), 2);
        q.setCurrentIndex(2);
        QCOMPARE(q.nextIndex(false), -1);
        QCOMPARE(q.nextIndex(true), 0);
        QVERIFY(!q.setCurrentIndex(3));
        QCOMPARE(q.currentIndex(), 2);
    }

    void shuffleKeepsCurrentAndRestores()
    {
        QRandomGenerator rng(42);
        PlayQueue q;
        QAbstractItemModelTester tester(&q, QAbstractItemModelTester::FailureReportingMode::QtTest);
        q.setRandomGenerator(&rng);
        q.replace(makeTracks(20), 5);
        const QList<qint64> before = ids(q);

        q.setShuffled(true);
        QCOMPARE(q.currentIndex(), 5);
        QCOMPARE(q.current()->id, 6);
        const QList<qint64> shuffled = ids(q);
        QCOMPARE(shuffled.mid(0, 6), before.mid(0, 6));   // history untouched
        QVERIFY(shuffled != before);                     // seed 42 does reorder 14 items
        QList<qint64> sorted = shuffled;
        std::sort(sorted.begin(), sorted.end());
        QCOMPARE(sorted, before);

        q.setShuffled(false);
        QCOMPARE(ids(q), before);
        QCOMPARE(q.currentIndex(), 5);
    }

    void shuffleTracksEditsForRestore()
    {
        QRandomGenerator rng(7);
        PlayQueue q;
        q.setRandomGenerator(&rng);
        q.replace(makeTracks(6), 1);        // current = id 2
        q.setShuffled(true);
        q.insertNext(makeTrack(100));
        q.append(makeTrack(200));
        QCOMPARE(q.at(q.currentIndex() + 1)->id, 100);
        // drop id 4 wherever the shuffle put it
        for (int i = 0; i < q.count(); ++i) {
            if (q.at(i)->id == 4) {
                q.removeAt(i);
                break;
            }
        }
        q.setShuffled(false);
        QCOMPARE(ids(q), (QList<qint64>{1, 2, 100, 3, 5, 6, 200}));
        QCOMPARE(q.current()->id, 2);
    }

    void replaceWhileShuffledStartsWithChosen()
    {
        QRandomGenerator rng(1);
        PlayQueue q;
        q.setRandomGenerator(&rng);
        q.setShuffled(true);
        q.replace(makeTracks(8), 4);
        QCOMPARE(q.currentIndex(), 0);
        QCOMPARE(q.current()->id, 5);
        q.setShuffled(false);
        QCOMPARE(ids(q), (QList<qint64>{1, 2, 3, 4, 5, 6, 7, 8}));
        QCOMPARE(q.currentIndex(), 4);
    }

    void dividersFollowAlbums()
    {
        PlayQueue q;
        q.replace({makeTrack(1, QStringLiteral("A")), makeTrack(2, QStringLiteral("A")),
                   makeTrack(3, QStringLiteral("B"))}, 0);
        const auto div = [&](int row) { return q.data(q.index(row), PlayQueue::Divider).toString(); };
        QCOMPARE(div(0), QStringLiteral("A — artist"));
        QCOMPARE(div(1), QString());
        QCOMPARE(div(2), QStringLiteral("B — artist"));
    }

    void updateTrackRefreshesSnapshots()
    {
        PlayQueue q;
        q.replace({makeTrack(1), makeTrack(2), makeTrack(1)}, 0);
        Track liked = makeTrack(1);
        liked.liked = true;
        q.updateTrack(liked);
        QVERIFY(q.at(0)->liked);
        QVERIFY(!q.at(1)->liked);
        QVERIFY(q.at(2)->liked);
    }
};

QTEST_GUILESS_MAIN(TestPlayQueue)
#include "tst_playqueue.moc"
