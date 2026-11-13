#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "shared_live.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {
class Line final : public core::Live {
public:
    bool isOpen() const override { return open; }
    void setNeed(Need value) override { wanted = value; }
    void askForAKeyFrame() override { ++keysAsked; }
    void askControl(bool on) override { controls.append(on); }
    ControlOwner controlOwner() const override { return owner; }
    void takeOver(const QString& expected) override { takeovers.append(expected); }
    void answer(bool on)
    {
        held = on;
        if (on)
            owner = {};
        emit controlChanged(on, {}, {});
    }
    bool hasControl() const override { return held; }
    void chooseRelativeMouse(bool relative) override { relativeMouse = relative; }
    void key(int, bool) override { ++input; }
    void pointerAt(int, int) override { ++input; }
    void pointerBy(int, int) override { ++input; }
    void button(int, bool) override { ++input; }
    void wheel(int, int) override { ++input; }
    void releaseAll() override
    {
        ++released;
        if (endsOnRelease)
            emit closed(u"unreachable"_s, u"late"_s);
    }
    void close() override { open = false; }
    bool endsForGood(const QString& code) const override { return code == u"a_newer_servers_word"_s; }
    quint64 picturesLetGo() const override { return letGo; }
    quint64 letGo = 0;
    Need wanted;
    QList<bool> controls;
    QStringList takeovers;
    ControlOwner owner;
    int input = 0;
    int released = 0;
    int keysAsked = 0;
    bool open = true;
    bool held = false;
    bool relativeMouse = false;
    bool endsOnRelease = false;
};

class Place final : public QObject, public core::WallSource {
public:
    void openLive(const QString&, core::Live::Need, ui::SharedLive::Done done) override
    {
        ++asked;
        waiting.append(std::move(done));
    }
    Line* answer()
    {
        auto live = std::make_unique<Line>();
        auto* result = live.get();
        waiting.takeFirst()(std::unique_ptr<core::Live>(std::move(live)));
        return result;
    }
    void askWall(std::function<void(base::Result<core::Wall>)>) override {}
    void askStatus(std::function<void(base::Result<QHash<QString, core::Status>>)>) override {}
    void askSnapshot(const QString&, int, double, std::function<void(base::Result<core::Snapshot>)>) override {}
    void act(const QString&, const core::Action&, std::function<void(base::Result<core::Done>)>) override {}
    bool mayHaveBeenDone(const QString& /*machine*/, const base::Error& /*error*/) const override { return false; }
    void askSavedActions(std::function<void(base::Result<QList<core::SavedAction>>)>) override {}
    void askWakeTargets(const QString&, std::function<void(base::Result<QList<core::WakeTarget>>)>) override {}
    void askConsole(const QString&, std::function<void(base::Result<QUrl>)>) override {}
    void arrange(const QStringList&, std::function<void(base::Result<QStringList>)>) override {}
    QList<ui::SharedLive::Done> waiting;
    int asked = 0;
};

void join(ui::SharedLive& pool, Place& source, std::unique_ptr<core::Live>& output,
          const QString& identity = u"server-and-sign-in"_s, core::Live::Need need = {800, 15})
{
    pool.open(identity, source, source, u"a"_s, need,
              [&output](auto result) { output = result ? std::move(result).value() : nullptr; });
}
} // namespace

class TestSharedLive : public QObject {
    Q_OBJECT
private slots:
    // M6 2c, the second reading (L5, L6): what the shared line let go is said to each window from when
    // that window joined it, and never less than it said before, also once the line is closed.
    void eachWindowCountsWhatTheLineLetGoSinceItJoined()
    {
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        QTRY_COMPARE(source.asked, 1);
        auto* line = source.answer();
        QTRY_VERIFY(left && left->isOpen());
        line->letGo = 5;
        QCOMPARE(left->picturesLetGo(), quint64{5});
        join(pool, source, right);
        QTRY_VERIFY(right && right->isOpen());
        QCOMPARE(right->picturesLetGo(), quint64{0});
        line->letGo = 8;
        QCOMPARE(left->picturesLetGo(), quint64{8});
        QCOMPARE(right->picturesLetGo(), quint64{3});
        line->open = false;
        QCOMPARE(left->picturesLetGo(), quint64{8});
        QCOMPARE(right->picturesLetGo(), quint64{3});
    }

    // Part D: whether the shared line ended for good is said to every window that shared it, after the
    // line itself is gone, for the code it ended with and no other.
    void everyWindowHearsWhetherTheSharedLineEndedForGood_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<bool>("forGood");
        QTest::newRow("a word the line says is for good") << u"a_newer_servers_word"_s << true;
        QTest::newRow("a passing end") << u"closed"_s << false;
    }
    void everyWindowHearsWhetherTheSharedLineEndedForGood()
    {
        QFETCH(QString, code);
        QFETCH(bool, forGood);
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        join(pool, source, right);
        QTRY_COMPARE(source.asked, 1);
        auto* line = source.answer();
        QTRY_VERIFY(left && right && left->isOpen());
        QSignalSpy leftClosed(left.get(), &core::Live::closed);
        QSignalSpy rightClosed(right.get(), &core::Live::closed);
        emit line->closed(code, u"said"_s);
        QTRY_COMPARE(leftClosed.count(), 1);
        QTRY_COMPARE(rightClosed.count(), 1);
        QTest::qWait(10); // the shared line is thrown away meanwhile
        QCOMPARE(left->endsForGood(code), forGood);
        QCOMPARE(right->endsForGood(code), forGood);
        QVERIFY(!left->endsForGood(u"another_word"_s));
    }
    void probeAWindowThatClosesWhileItsReleaseEndsTheLine()
    {
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        join(pool, source, right);
        QTRY_COMPARE(source.asked, 1);
        auto* line = source.answer();
        QTRY_VERIFY(left && right && left->isOpen());
        left->askControl(true);
        line->answer(true);
        QVERIFY(left->hasControl());
        line->endsOnRelease = true;
        QSignalSpy leftClosed(left.get(), &core::Live::closed);
        QSignalSpy rightClosed(right.get(), &core::Live::closed);
        std::unique_ptr<core::Live> going = std::move(left); // as LivePicture::endLine does
        going->close();                                      // detach -> control(false) -> releaseAll
        QCOMPARE(line->released, 1);                         // the release was attempted
        QCOMPARE(rightClosed.count(), 1);                    // the other window hears the shared line ended
        QCOMPARE(leftClosed.count(), 0);                     // live.h:112: `closed` is not said for close()
        QCOMPARE(line->controls, QList<bool>{true}); // No release request/deadline after synchronous termination.
        going.reset();
        QTest::qWait(10);
    }
    void reportedOwnersAreSharedButTakeoverCannotStealAnotherWindowsInput()
    {
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        join(pool, source, right);
        QTRY_COMPARE(source.asked, 1);
        auto* line = source.answer();
        QTRY_VERIFY(left && right && left->isOpen());
        line->owner = {u"Operator"_s, u"server-owner"_s, false};
        emit line->controlOwnerChanged();
        QCOMPARE(left->controlOwner().name, u"Operator"_s);
        QCOMPARE(right->controlOwner().reference, u"server-owner"_s);
        left->takeOver(u"server-owner"_s);
        QCOMPARE(line->takeovers, QStringList{u"server-owner"_s});
        QVERIFY(right->controlOwner().anotherWindow);
        right->takeOver(u"server-owner"_s);
        QCOMPARE(line->takeovers.size(), 1);
        line->answer(true);
        QVERIFY(left->hasControl());
        QVERIFY(right->controlOwner().anotherWindow);
        right->key(4, true);
        QCOMPARE(line->input, 0);
        left->askControl(false);
        line->answer(false);
        QVERIFY(!right->controlOwner().anotherWindow);
        QVERIFY(right->controlOwner().name.isEmpty());
    }

    void oneConnectionFanoutAndLastViewerCleanup()
    {
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        join(pool, source, right, u"server-and-sign-in"_s, {1600, 30});
        QVERIFY(left == nullptr); // Always delivered asynchronously.
        QTRY_COMPARE(source.asked, 1);
        QTRY_VERIFY(left && right);
        auto* line = source.answer();
        QCOMPARE(line->wanted, (core::Live::Need{1600, 30}));
        QSignalSpy first(left.get(), &core::Live::pictureCame);
        QSignalSpy second(right.get(), &core::Live::pictureCame);
        emit line->pictureCame({false, true, 1, 0, "jpeg"});
        QCOMPARE(first.size(), 1);
        QCOMPARE(second.size(), 1);
        right->setNeed({1200, 20});
        QCOMPARE(line->wanted, (core::Live::Need{1200, 20}));
        right.reset();
        QVERIFY(line->open);
        QCOMPARE(line->wanted, (core::Live::Need{800, 15}));
        emit line->pictureCame({false, true, 2, 0, "next"});
        QCOMPARE(first.size(), 2);
        left->close();
        QVERIFY(!line->open);
        join(pool, source, right);
        QTRY_COMPARE(source.asked, 2);
    }

    void lateH264ViewerWaitsForAKeyFrame()
    {
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        QTRY_COMPARE(source.asked, 1);
        auto* line = source.answer();
        emit line->videoChanged({true, 1280, 720});
        emit line->pictureCame({true, true, 1, 0, "key"});
        join(pool, source, right);
        QTRY_VERIFY(right != nullptr);
        QSignalSpy first(left.get(), &core::Live::pictureCame);
        QSignalSpy second(right.get(), &core::Live::pictureCame);
        QVERIFY(line->keysAsked > 0);
        emit line->pictureCame({true, false, 2, 0, "delta"});
        QCOMPARE(first.size(), 1);
        QCOMPARE(second.size(), 0);
        emit line->pictureCame({true, true, 3, 0, "key"});
        QCOMPARE(first.size(), 2);
        QCOMPARE(second.size(), 1);
    }

    void onlyControllerSendsInputAndReleaseMustBeConfirmed()
    {
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        join(pool, source, right);
        QTRY_COMPARE(source.asked, 1);
        auto* line = source.answer();
        QSignalSpy refused(right.get(), &core::Live::controlChanged);
        left->chooseRelativeMouse(true);
        left->askControl(true);
        QVERIFY(line->relativeMouse);
        right->askControl(true);
        QCOMPARE(refused.size(), 1);
        QCOMPARE(line->controls, QList<bool>{true});
        line->answer(true);
        QVERIFY(left->hasControl());
        QVERIFY(!right->hasControl());
        right->key(4, true);
        right->pointerAt(2, 3);
        right->pointerBy(2, 3);
        right->button(1, true);
        right->wheel(0, 1);
        right->releaseAll();
        QCOMPARE(line->input, 0);
        QCOMPARE(line->released, 0);
        left->key(4, true);
        QCOMPARE(line->input, 1);
        left.reset();
        QVERIFY(line->open);
        QVERIFY(line->released > 0);
        right->askControl(true);
        QCOMPARE(line->controls, (QList<bool>{true, false}));
        line->answer(false);
        right->askControl(true);
        line->answer(true);
        QVERIFY(right->hasControl());
    }

    void cancellingAPendingControlRequestDoesNotAddUnmatchedReleases()
    {
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        join(pool, source, right);
        QTRY_COMPARE(source.asked, 1);
        auto* line = source.answer();
        left->askControl(true);
        left.reset();
        line->answer(true);
        QCOMPARE(line->controls, (QList<bool>{true, false}));
        QVERIFY(!right->hasControl());
        line->answer(false);
        right->askControl(true);
        line->answer(true);
        QVERIFY(right->hasControl());
    }

    void missingReleaseReplyReconnectsWithoutGrantingAnotherWindow()
    {
        ui::SharedLive pool(30);
        Place source;
        std::unique_ptr<core::Live> left;
        std::unique_ptr<core::Live> right;
        join(pool, source, left);
        join(pool, source, right);
        QTRY_COMPARE(source.asked, 1);
        auto* line = source.answer();
        QSignalSpy ended(right.get(), &core::Live::closed);
        left->askControl(true);
        line->answer(true);
        left.reset();
        right->askControl(true);
        QVERIFY(!right->hasControl());
        QTRY_COMPARE(ended.size(), 1);
        QVERIFY(!right->isOpen());
        join(pool, source, right);
        QTRY_COMPARE(source.asked, 2);
    }

    void identityPartitionsAndFailuresCanReconnect()
    {
        ui::SharedLive pool;
        Place source;
        std::unique_ptr<core::Live> first;
        std::unique_ptr<core::Live> second;
        join(pool, source, first, u"server-one/account-one"_s);
        join(pool, source, second, u"server-two/account-one"_s);
        QTRY_COMPARE(source.asked, 2);
        QSignalSpy ended(first.get(), &core::Live::closed);
        source.waiting.takeFirst()(base::Error{"no_live", "No live video"});
        QCOMPARE(ended.size(), 1);
        QVERIFY(!first->isOpen());
        source.answer();
        QVERIFY(second->isOpen());
        join(pool, source, first, u"server-one/account-two"_s);
        QTRY_COMPARE(source.asked, 3);
        first.reset(); // Leave before opening completed; the late connection is discarded.
        auto abandoned = std::make_unique<Line>();
        QPointer<Line> late = abandoned.get();
        source.waiting.takeFirst()(std::unique_ptr<core::Live>(std::move(abandoned)));
        QVERIFY(late == nullptr);
    }

    void closingTheFirstSourceWhileOpeningDoesNotStrandOtherViewers()
    {
        ui::SharedLive pool;
        auto source = std::make_unique<Place>();
        std::unique_ptr<core::Live> first;
        std::unique_ptr<core::Live> second;
        join(pool, *source, first);
        QTRY_COMPARE(source->asked, 1);
        QSignalSpy ended(first.get(), &core::Live::closed);
        int otherEnded = 0;
        pool.open(u"server-and-sign-in"_s, *source, *source, u"a"_s, {}, [&](auto result) {
            second = std::move(result).value();
            connect(second.get(), &core::Live::closed, this, [&] { ++otherEnded; });
        });
        source.reset();
        QCOMPARE(ended.size(), 1);
        QTRY_VERIFY(second != nullptr);
        // The join was queued as the source closed. It must receive the same end after handover.
        QTRY_COMPARE(otherEnded, 1);
    }
};

QTEST_GUILESS_MAIN(TestSharedLive)
#include "test_shared_live.moc"
