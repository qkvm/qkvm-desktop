// A JetKVM spoken to by the program itself (core/jetkvm_device, its viewer and its live), without a window: what
// it is, how it is, its card's picture, its picture live, and its one viewer given up to another and taken back.
// For trying the driver against another implementation of a JetKVM, the web's own fake (aiortc:
// tools/test_jetkvm_joint.py), and later, where the owner names one, against a real JetKVM: its picture and how it
// is, and nothing more.
//
//     qkvm-jetkvm-rig <address> [--cards N] [--live <seconds>] [--hand-over] [--resume]
//
// In order: what it is (identify: its version), how it is (a look, which never connects), its card's picture (asked
// as a card asks, every 250 ms until one comes, at most 30 s: --cards that many times, each after the one before),
// its picture live for so many seconds (a window's Live: how many pictures, how many key frames, the first after how
// long), and how it is while that is open (asked over its connection). With --hand-over the rig then says
// "waiting for another viewer" and waits (30 s at most) for its window to be told "in_use", and a card and a look
// after it. With --resume it then takes the picture back ("Show here") and asks a card again.
//
// At the end, each connection's way in as the video's diagnostics have it (JetKvmConnection: asked, the offer sent,
// the answer, its channel open, its first picture; kept in a folder of the rig's own, thrown away after), and the
// first connection's first picture as "measured: first_key_frame_ms": a card's first ask to the first key frame in
// its box (plan 4, M1), without the 250 ms between a card's asks.
//
// Each step is a line; the numbers are "measured: <name> <value>" lines. The password comes in
// QKVM_RIG_KVM_PASSWORD and from nowhere else. A JetKVM at an address that is not this PC's is spoken to only
// where its host stands in QKVM_RIG_NAMED_BY_THE_OWNER, and then only its picture and how it is: --hand-over and
// --resume are refused there (a hand-over and a "Show here" take its one viewer from whoever has it). Its card's
// picture takes that viewer all the same: a JetKVM shows its picture to one viewer at a time, and the rig's video
// connection is one. Whoever had it, its own page or a wall of the web's that shows it, steps aside and says "in
// use" until somebody presses "Show here" there, also long after the rig has ended; the rig says so in a line
// before it begins (the reading of e40a02f, L5). Ends with 0 where everything it was asked for came about.

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/live.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/video_diagnostics.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

void say(const QString& line)
{
    std::fputs(qPrintable(line + u'\n'), stdout);
    std::fflush(stdout);
}

void measured(const char* name, qint64 value)
{
    say(u"measured: %1 %2"_s.arg(QLatin1StringView(name)).arg(value));
}

QString why(const base::Error& error)
{
    return u"%1: %2"_s.arg(QString::fromStdString(error.code), QString::fromStdString(error.message));
}

bool onThisPc(const QUrl& address)
{
    return address.host() == u"localhost"_s || QHostAddress(address.host()).isLoopback();
}

bool namedByTheOwner(const QUrl& address)
{
    for (const QString& host : qEnvironmentVariable("QKVM_RIG_NAMED_BY_THE_OWNER").split(u',', Qt::SkipEmptyParts)) {
        if (host.trimmed().compare(address.host(), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

// Each connection's way in, as the video's diagnostics wrote it: a line for each, and the first one's first picture.
void sayTheWaysIn(const QString& path)
{
    QFile written(path);
    if (!written.open(QIODevice::ReadOnly)) {
        say(u"the ways in: not written"_s);
        return;
    }
    QStringList steps;
    int connection = 0;
    qint64 firstKeyFrameMs = -1;
    const auto sayOne = [&steps, &connection] {
        if (!steps.isEmpty()) {
            say(u"the way in of connection %1, ms since it was asked for: %2"_s.arg(connection).arg(steps.join(u", ")));
        }
        steps.clear();
    };
    for (const QByteArray& line : written.readAll().split('\n')) {
        const QJsonObject one = QJsonDocument::fromJson(line).object();
        const QString event = one.value(u"event"_s).toString();
        if (!event.startsWith(u"jetkvm_"_s)) {
            continue;
        }
        const qint64 ms = one.value(u"values"_s).toObject().value(u"ms"_s).toInteger(-1);
        if (event == u"jetkvm_asked"_s) {
            sayOne();
            ++connection;
        }
        const bool key = one.value(u"values"_s).toObject().value(u"key"_s).toInteger() == 1;
        steps.append(u"%1 %2%3"_s.arg(event.mid(7).replace(u'_', u' '))
                         .arg(ms)
                         .arg(event == u"jetkvm_first_picture"_s && !key ? u" (no key frame)"_s : QString()));
        // (Only where that first picture was a key frame: a connection hands on key frames only from its channel's
        // opening, and a picture can come before that. The reading of 077e539..2c3f523, L1.)
        if (event == u"jetkvm_first_picture"_s && connection == 1 && key) {
            firstKeyFrameMs = ms;
        }
    }
    sayOne();
    if (firstKeyFrameMs >= 0) {
        measured("first_key_frame_ms", firstKeyFrameMs);
    } else if (connection > 0) {
        say(u"the first connection's first picture was no key frame, or none came: first_key_frame_ms not measured"_s);
    }
}

struct Rig {
    std::unique_ptr<core::JetKvmDevice> device;
    std::unique_ptr<core::OwnLive> live;
    std::unique_ptr<core::Live> window;
    int failed = 0;
};

// How it is, by a look: a line.
void lookAt(Rig& rig, const QString& when, const std::function<void(const core::Status&)>& then)
{
    auto heard = std::make_shared<QMetaObject::Connection>();
    *heard = QObject::connect(rig.device.get(), &core::OwnDevice::looked, rig.device.get(), [&rig, heard, when, then] {
        QObject::disconnect(*heard);
        const core::Status status = rig.device->status();
        say(u"how it is %1: %2%3, %4x%5, version %6, keyboard %7"_s.arg(when, status.state)
                .arg(status.errorCode.isEmpty() ? QString() : u" ("_s + status.errorCode + u')')
                .arg(status.width)
                .arg(status.height)
                .arg(status.version.isEmpty() ? u"not said"_s : status.version)
                .arg(!status.keyboard ? u"not known"_s : (*status.keyboard ? u"connected"_s : u"not connected"_s)));
        then(status);
    });
    rig.device->look();
}

// A card's picture: asked every 250 ms until one comes, 30 s at the most.
// NOLINTNEXTLINE(misc-no-recursion): it asks again by calling itself, a counted number of times
void cardOf(Rig& rig, const std::shared_ptr<QElapsedTimer>& since, int triesLeft,
            const std::function<void(const base::Result<core::Snapshot>&, qint64)>& then)
{
    rig.device->askSnapshot(320, [&rig, since, triesLeft, then](const base::Result<core::Snapshot>& picture) {
        if (!picture && picture.error().code == "no_stream" && triesLeft > 0) {
            QTimer::singleShot(250, rig.device.get(),
                               [&rig, since, triesLeft, then] { cardOf(rig, since, triesLeft - 1, then); });
            return;
        }
        then(picture, since->elapsed());
    });
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        say(u"qkvm-jetkvm-rig <address> [--cards N] [--live <seconds>] [--hand-over] [--resume]"_s);
        return 2;
    }
    const QUrl address(args.at(1));
    int cards = 1;
    int liveSeconds = 0;
    bool handOver = false;
    bool resume = false;
    for (qsizetype at = 2; at < args.size(); ++at) {
        if (args.at(at) == u"--cards"_s && at + 1 < args.size()) {
            cards = std::max(1, args.at(++at).toInt());
        } else if (args.at(at) == u"--live"_s && at + 1 < args.size()) {
            liveSeconds = std::max(0, args.at(++at).toInt());
        } else if (args.at(at) == u"--hand-over"_s) {
            handOver = true;
        } else if (args.at(at) == u"--resume"_s) {
            resume = true;
        } else {
            say(u"not understood: "_s + args.at(at));
            return 2;
        }
    }
    if (!address.isValid() || (address.scheme() != u"http"_s && address.scheme() != u"https"_s)) {
        say(u"no address: http://... or https://..."_s);
        return 2;
    }
    if (!onThisPc(address)) {
        if (!namedByTheOwner(address)) {
            say(u"not this PC's address, and its owner did not name it (QKVM_RIG_NAMED_BY_THE_OWNER): nothing is asked"_s);
            return 2;
        }
        if (handOver || resume) {
            say(u"a JetKVM the owner named is asked its picture and how it is, and nothing more: no --hand-over, no "
                "--resume"_s);
            return 2;
        }
        say(u"its picture takes this JetKVM's one viewer: its own page, or a wall that shows it, says \"in use\" "
            "from then on, until somebody presses \"Show here\" there"_s);
    }
    if (!core::useWindowsOwnTls()) {
        say(u"this copy of the program does not speak Windows' own TLS"_s);
        return 2;
    }

    // (The video's diagnostics in a folder of the rig's own, thrown away at its end: numbers only.)
    const QTemporaryDir diagnosticsFolder;
    const QString diagnosticsPath = diagnosticsFolder.filePath(u"video.jsonl"_s);
    const core::VideoDiagnostics diagnostics(diagnosticsPath);
    Rig rig;
    core::JetKvmDevice::Where where;
    where.address = address;
    core::JetKvmDevice::Pace pace;
    pace.pacing = std::make_shared<core::SignInPacing>();
    rig.device = std::make_unique<core::JetKvmDevice>(where, qEnvironmentVariable("QKVM_RIG_KVM_PASSWORD"), pace);

    // The steps, each calling the next.
    std::function<void()> identify;
    std::function<void()> look;
    std::function<void(int)> card;
    std::function<void()> goLive;
    std::function<void()> stepAside;
    std::function<void()> takeBack;
    const auto finish = [&rig, &diagnostics, &diagnosticsPath] {
        if (diagnostics.isOpen()) {
            // (Written by the diagnostics' own thread: all of it, before it is read.)
            diagnostics.flush();
            sayTheWaysIn(diagnosticsPath);
        }
        say(rig.failed == 0 ? u"all came about"_s : u"%1 did not come about"_s.arg(rig.failed));
        rig.window.reset();
        rig.live.reset();
        QTimer::singleShot(200, QCoreApplication::instance(),
                           [&rig] { QCoreApplication::exit(rig.failed == 0 ? 0 : 1); });
    };

    identify = [&] {
        auto since = std::make_shared<QElapsedTimer>();
        since->start();
        rig.device->identify([&, since](const base::Result<core::JetKvmDevice::Identity>& who) {
            if (!who) {
                say(u"what it is: "_s + why(who.error()));
                ++rig.failed;
            } else {
                say(u"what it is: %1, version %2, %3 (in %4 ms)"_s
                        .arg(who.value().model, who.value().version.isEmpty() ? u"not said"_s : who.value().version,
                             !who.value().number             ? u"its number not read"_s
                             : who.value().number->isEmpty() ? u"no number of its own"_s
                                                             : u"known by its number"_s)
                        .arg(since->elapsed()));
            }
            look();
        });
    };
    look = [&] { lookAt(rig, u"(a look, which connects nothing)"_s, [&](const core::Status&) { card(cards); }); };
    card = [&](int left) {
        auto since = std::make_shared<QElapsedTimer>();
        since->start();
        cardOf(rig, since, 120, [&, left](const base::Result<core::Snapshot>& picture, qint64 afterMs) {
            if (!picture) {
                say(u"no card's picture: "_s + why(picture.error()));
                ++rig.failed;
            } else {
                const QByteArray& still = picture.value().picture;
                const bool keyFrame = still.startsWith(QByteArray::fromHex("00000001")) && still.size() > 4 &&
                                      (static_cast<unsigned char>(still.at(4)) & 0x1FU) == 7U;
                say(u"a card's picture: %1 bytes, %2, signal %3, after %4 ms"_s.arg(still.size())
                        .arg(keyFrame ? u"an H.264 key frame with its SPS"_s : u"no key frame"_s,
                             picture.value().signal ? u"yes"_s : u"no"_s)
                        .arg(afterMs));
                if (left == cards) {
                    measured("first_card_picture_ms", afterMs);
                }
                if (!keyFrame) {
                    ++rig.failed;
                }
            }
            if (left > 1) {
                QTimer::singleShot(500, rig.device.get(), [&, left] { card(left - 1); });
            } else {
                goLive();
            }
        });
    };
    goLive = [&] {
        if (liveSeconds == 0) {
            stepAside();
            return;
        }
        struct Seen {
            QElapsedTimer since;
            int pictures = 0;
            int keys = 0;
            qint64 firstMs = -1;
            qint64 bytes = 0;
        };
        auto seen = std::make_shared<Seen>();
        seen->since.start();
        rig.live = rig.device->makeLive();
        rig.window = rig.live->open({1920, 60});
        QObject::connect(rig.window.get(), &core::Live::pictureCame, rig.window.get(),
                         [seen](const core::Live::Picture& picture) {
                             if (seen->pictures == 0) {
                                 seen->firstMs = seen->since.elapsed();
                                 say(u"live: pictures coming"_s);
                             }
                             ++seen->pictures;
                             seen->keys += picture.keyFrame ? 1 : 0;
                             seen->bytes += picture.data.size();
                         });
        QTimer::singleShot(liveSeconds * 1000, rig.device.get(), [&, seen] {
            const qint64 ms = seen->since.elapsed();
            say(u"live for %1 ms: %2 pictures (%3 key frames), the first after %4 ms, %5 bytes"_s.arg(ms)
                    .arg(seen->pictures)
                    .arg(seen->keys)
                    .arg(seen->firstMs)
                    .arg(seen->bytes));
            measured("first_live_picture_ms", seen->firstMs);
            measured("live_pictures_per_10s", ms > 0 ? qint64{seen->pictures} * 10'000 / ms : 0);
            if (seen->pictures == 0 || seen->keys == 0) {
                ++rig.failed;
            }
            lookAt(rig, u"(while live, asked over its connection)"_s, [&](const core::Status& status) {
                if (status.state != u"online"_s) {
                    ++rig.failed;
                }
                stepAside();
            });
        });
    };
    stepAside = [&] {
        if (!handOver) {
            takeBack();
            return;
        }
        if (!rig.window) {
            rig.live = rig.device->makeLive();
            rig.window = rig.live->open({1920, 60});
        }
        auto told = std::make_shared<bool>(false);
        QObject::connect(rig.window.get(), &core::Live::closed, rig.window.get(),
                         [&, told](const QString& code, const QString&) {
                             *told = true;
                             say(u"its window was told: "_s + code);
                             if (code != u"in_use"_s) {
                                 ++rig.failed;
                             }
                             QTimer::singleShot(300, rig.device.get(), [&] {
                                 rig.device->askSnapshot(320, [&](const base::Result<core::Snapshot>& picture) {
                                     say(picture ? u"a card after it: a picture (it should be in_use)"_s
                                                 : u"a card after it: "_s + why(picture.error()));
                                     if (picture || picture.error().code != "in_use") {
                                         ++rig.failed;
                                     }
                                     lookAt(rig, u"(stepped aside)"_s, [&](const core::Status& status) {
                                         if (status.state != u"busy"_s) {
                                             ++rig.failed;
                                         }
                                         takeBack();
                                     });
                                 });
                             });
                         });
        say(u"waiting for another viewer"_s);
        QTimer::singleShot(30'000, rig.device.get(), [&, told] {
            if (!*told) {
                say(u"no other viewer came"_s);
                ++rig.failed;
                finish();
            }
        });
    };
    takeBack = [&] {
        if (!resume) {
            finish();
            return;
        }
        auto since = std::make_shared<QElapsedTimer>();
        since->start();
        rig.device->act(core::Action::plain(u"resume"_s), [&, since](const base::Result<core::Done>& done) {
            say(done ? u"taken back (\"Show here\") in %1 ms"_s.arg(since->elapsed())
                     : u"not taken back: "_s + why(done.error()));
            if (!done) {
                ++rig.failed;
                finish();
                return;
            }
            measured("resume_ms", since->elapsed());
            cardOf(rig, since, 120, [&](const base::Result<core::Snapshot>& picture, qint64 afterMs) {
                say(picture ? u"a card after it: a picture, %1 ms after \"Show here\""_s.arg(afterMs)
                            : u"a card after it: "_s + why(picture.error()));
                if (!picture) {
                    ++rig.failed;
                }
                lookAt(rig, u"(taken back)"_s, [&](const core::Status&) { finish(); });
            });
        });
    };

    QTimer::singleShot(0, &app, identify);
    return QCoreApplication::exec();
}
