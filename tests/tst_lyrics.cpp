#include "lyrics.h"

#include <QTest>

class TestLyrics : public QObject
{
    Q_OBJECT

private slots:
    void parsesLrc()
    {
        const Lyrics l = Lyrics::parse(QStringLiteral(
            "[ar:Neon Coastline]\n"
            "[ti:Midnight Drive]\n"
            "[00:12.50]Headlights on the empty road,\r\n"
            "[00:05]city glows in indigo,\n"
            "[01:02.005][00:30.1]midnight, midnight,\n"
            "[00:40.00]\n"));
        QVERIFY(l.timed);
        QCOMPARE(l.lines.size(), 5);
        QCOMPARE(l.lines.at(0).ms, 5000);
        QCOMPARE(l.lines.at(0).text, QStringLiteral("city glows in indigo,"));
        QCOMPARE(l.lines.at(1).ms, 12500);
        QCOMPARE(l.lines.at(2).ms, 30100);
        QCOMPARE(l.lines.at(2).text, QStringLiteral("midnight, midnight,"));
        QCOMPARE(l.lines.at(3).ms, 40000);
        QCOMPARE(l.lines.at(3).text, QString());   // instrumental gap
        QCOMPARE(l.lines.at(4).ms, 62005);
    }

    void indexFollowsTime()
    {
        const Lyrics l = Lyrics::parse(QStringLiteral("[00:01]a\n[00:03]b\n[00:05]c\n"));
        QCOMPARE(l.indexAt(0), -1);
        QCOMPARE(l.indexAt(1000), 0);
        QCOMPARE(l.indexAt(2999), 0);
        QCOMPARE(l.indexAt(3000), 1);
        QCOMPARE(l.indexAt(999'999), 2);
    }

    void plainText()
    {
        const Lyrics l = Lyrics::parse(QStringLiteral("\n\nfirst\n\nsecond\n\n"));
        QVERIFY(!l.timed);
        QCOMPARE(l.lines.size(), 3);
        QCOMPARE(l.lines.at(0).text, QStringLiteral("first"));
        QCOMPARE(l.lines.at(1).text, QString());
        QCOMPARE(l.lines.at(2).text, QStringLiteral("second"));
        QCOMPARE(l.indexAt(5000), -1);
    }

    void empty()
    {
        const Lyrics l = Lyrics::parse(QString());
        QVERIFY(!l.timed);
        QVERIFY(l.lines.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestLyrics)
#include "tst_lyrics.moc"
