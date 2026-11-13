// The program's own way of speaking to a KVM (core/kvmd_device), by itself and without a window:
// signs in to one KVM, says who it is and how it is, takes a picture of its screen, and does what
// it is told to. For trying the driver against something that speaks as a real KVM does
// (tools/stand_in_kvm.py), and later, where the owner names one, against a real KVM.
//
//     qkvm-kvmd-rig <address> [--known <the certificate's SHA-256>] [--width N] [--pictures N]
//                   [--do <action>]... [--keys <keys>] [--type <text>] [--show <computer>]
//                   [--stream <pictures>] [--new <pictures>] [--live <seconds>] [--line <seconds>]
//                   [--touch <folder>]
//     qkvm-kvmd-rig <address> --probe 1 [--known <the certificate's SHA-256> | --known windows]
//
// With --probe nothing is signed in to and no password is used: the address is asked what answers
// there, as the box that adds a KVM asks it first, and the rig says what it is and which
// certificate it showed. With --known beside it the asking is held to what the KVM is known by,
// as this PC's wall asks a KVM of its own before its password goes anywhere: by that certificate,
// or ("windows") by Windows' word for its name; what shows anything else hears nothing, and the
// rig says that it was refused and how long the asking took. With --stream the KVM's stream of
// pictures is read until that many have come, or ten seconds have passed. With --new so many
// pictures are asked for as captured, each when the one before has come, and the rig says how
// fast they came and how many of them were the one before over again (a screen that does not
// change). With --live the KVM is had live for so many seconds, as the window that shows it large
// has it (its stream where it serves one, else its pictures one after the other), and the rig
// says how many pictures a second came: only pictures are asked for, and keyboard and mouse are
// not. With --line the line for keyboard and mouse is opened and
// held for so many seconds, and closed: no key and no move is said on it, only the line's own
// asking whether the KVM is there. With --touch something IS sent to the KVM's computer, the
// least that shows keyboard and mouse arrive, and only where its owner allowed exactly this: the
// left Shift key down and up, once; then the pointer to the middle of the screen, and to a place
// a few pixels to the right of it; no button. A picture is kept in the folder after each of the
// two places (a.jpg, b.jpg), for the pointer to be looked for in both.
//
// The password comes in QKVM_RIG_KVM_PASSWORD, and from nowhere else: not from the command line,
// where every program on the PC can read it. Ends with 0 where everything it was asked for came
// about, and says each step in a line.

#include <algorithm>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>

#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_live.h>
#include <qkvm/core/kvmd_stream.h>
#include <qkvm/core/live.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

void say(const QString& line)
{
    std::fputs(qPrintable(line + u'\n'), stdout);
    std::fflush(stdout);
}

QString why(const base::Error& error)
{
    return u"%1: %2"_s.arg(QString::fromStdString(error.code), QString::fromStdString(error.message));
}

QString yesNo(const std::optional<bool>& value)
{
    if (!value) {
        return u"not known"_s;
    }
    return *value ? u"yes"_s : u"no"_s;
}

// One picture, asked for as a card asks: again a little later where the KVM says so.
// NOLINTNEXTLINE(misc-no-recursion): it asks again by calling itself, a counted number of times
void takePicture(core::KvmdDevice& device, QCoreApplication& app, int width, int triesLeft, int& failed,
                 const std::function<void()>& next)
{
    auto watch = std::make_shared<QElapsedTimer>();
    watch->start();
    device.askSnapshot(
        width, [&device, &app, width, triesLeft, &failed, next, watch](const base::Result<core::Snapshot>& picture) {
            if (picture) {
                const QByteArray& data = picture.value().picture;
                say(u"a picture: %1 bytes, %2, signal %3, after %4 ms"_s.arg(data.size())
                        .arg(data.startsWith(QByteArray::fromHex("ffd8")) ? u"a JPEG"_s : u"a PNG"_s,
                             picture.value().signal ? u"yes"_s : u"no"_s)
                        .arg(watch->elapsed()));
                QTimer::singleShot(300, &app, next);
            } else if (picture.error().code == "no_stream" && triesLeft > 0) {
                say(u"  not yet: "_s + why(picture.error()));
                QTimer::singleShot(600, &app, [&device, &app, width, triesLeft, &failed, next] {
                    takePicture(device, app, width, triesLeft - 1, failed, next);
                });
            } else {
                say(u"no picture: "_s + why(picture.error()));
                ++failed;
                next();
            }
        });
}

// What answers at an address, asked with nothing that says who asks; and, where it is said what
// the KVM there is known by, on lines that are held to that.
int probe(const QString& typed, const std::optional<core::KvmProbe::Known>& known)
{
    const base::Result<QUrl> address = core::kvm::addressFrom(typed);
    if (!address) {
        say(u"no address: "_s + why(address.error()));
        return 2;
    }
    core::KvmProbe asking;
    auto watch = std::make_shared<QElapsedTimer>();
    watch->start();
    const auto told = [watch, held = known.has_value()](const core::KvmProbe::Found& found) {
        const char* kind = "nothing answers there";
        switch (found.kind) {
        case core::KvmProbe::Kind::Kvmd:
            kind = "a KVM that runs kvmd";
            break;
        case core::KvmProbe::Kind::JetKvm:
            kind = "a JetKVM";
            break;
        case core::KvmProbe::Kind::Redfish:
            kind = "a server's management controller";
            break;
        case core::KvmProbe::Kind::SomethingElse:
            kind = "something else";
            break;
        case core::KvmProbe::Kind::NotAsked:
            kind = "not asked: this program does not speak Windows' own TLS, and speaks to nothing over HTTPS";
            break;
        case core::KvmProbe::Kind::Nothing:
            break;
        }
        say(u"what is there: "_s + QLatin1StringView(kind));
        say(u"certificate shown (SHA-256): "_s +
            (found.shown.isEmpty() ? u"none"_s : QString::fromLatin1(found.shown.toHex())));
        say(u"trusted by Windows for that name: "_s + (found.trustedByWindows ? u"yes"_s : u"no"_s));
        if (held) {
            say(found.refused ? u"held to what it is known by: a line showed something else, and heard nothing"_s
                              : u"held to what it is known by: every line that came up was the KVM's"_s);
        }
        say(u"asked in %1 ms"_s.arg(watch->elapsed()));
        QCoreApplication::exit(found.kind == core::KvmProbe::Kind::Kvmd ? 0 : 1);
    };
    if (known) {
        asking.askAsKnown(address.value(), *known, told);
    } else {
        asking.ask(address.value(), told);
    }
    return QCoreApplication::exec();
}

// The KVM's stream of pictures, read until so many have come or ten seconds have passed.
void readStream(core::KvmdDevice& device, QCoreApplication& app, int wanted, int& failed,
                const std::function<void()>& next)
{
    struct Seen {
        std::unique_ptr<core::KvmdStream> stream;
        QElapsedTimer watch;
        int pictures = 0;
        qint64 bytes = 0;
        qint64 firstAfterMs = -1;
        bool over = false;
    };
    auto seen = std::make_shared<Seen>();
    seen->watch.start();
    seen->stream = device.watch();
    const auto finish = [seen, &failed, next, wanted](const QString& how) {
        if (seen->over) {
            return;
        }
        seen->over = true;
        const qint64 took = seen->watch.elapsed();
        say(u"the stream: %1 pictures, %2 bytes in all, the first after %3 ms, %4 ms in all; %5"_s.arg(seen->pictures)
                .arg(seen->bytes)
                .arg(seen->firstAfterMs)
                .arg(took)
                .arg(how));
        if (seen->pictures < wanted) {
            ++failed;
        }
        // (Not from inside its own word: a turn of the loop later.)
        seen->stream.release()->deleteLater();
        next();
    };
    QObject::connect(
        seen->stream.get(), &core::KvmdStream::pictureCame, &app, [seen, finish, wanted](const QByteArray& jpeg) {
            if (seen->pictures == 0) {
                seen->firstAfterMs = seen->watch.elapsed();
                say(u"  its first picture: %1 bytes, %2"_s.arg(jpeg.size())
                        .arg(jpeg.startsWith(QByteArray::fromHex("ffd8")) ? u"a JPEG"_s : u"not a JPEG"_s));
            }
            ++seen->pictures;
            seen->bytes += jpeg.size();
            if (seen->pictures >= wanted) {
                seen->stream->close();
                finish(u"closed from here"_s);
            }
        });
    QObject::connect(seen->stream.get(), &core::KvmdStream::ended, &app,
                     [finish](const QString& code, const QString& message) { finish(code + u": "_s + message); });
    QTimer::singleShot(10'000, &app, [seen, finish] {
        if (!seen->over) {
            seen->stream->close();
            finish(u"ten seconds were up"_s);
        }
    });
}

// Pictures as captured, one after the other: what came of them so far.
struct OneAfterTheOther {
    QElapsedTimer watch;
    int came = 0;
    int same = 0;
    int refused = 0;
    qint64 bytes = 0;
    qint64 slowestMs = 0;
    QByteArray before;
};

// NOLINTNEXTLINE(misc-no-recursion): it asks for the next by calling itself, a counted number of times
void askAnew(core::KvmdDevice& device, QCoreApplication& app, const std::shared_ptr<OneAfterTheOther>& seen, int left,
             int& failed, const std::function<void()>& next)
{
    if (left == 0) {
        const qint64 took = seen->watch.elapsed();
        say(u"one after the other: %1 pictures in %2 ms, %3 a second, the slowest %4 ms, %5 bytes in all; %6 were "
            u"the one before over again; %7 times the KVM had none"_s.arg(seen->came)
                .arg(took)
                .arg(took > 0 ? seen->came * 1000.0 / double(took) : 0, 0, 'f', 1)
                .arg(seen->slowestMs)
                .arg(seen->bytes)
                .arg(seen->same)
                .arg(seen->refused));
        if (seen->came == 0) {
            ++failed;
        }
        next();
        return;
    }
    const qint64 askedAt = seen->watch.elapsed();
    device.askNewSnapshot(core::kvmd::longestPictureSide, [&device, &app, seen, left, &failed, next,
                                                           askedAt](const base::Result<core::Snapshot>& answer) {
        if (answer) {
            ++seen->came;
            seen->bytes += answer.value().picture.size();
            seen->slowestMs = std::max(seen->slowestMs, seen->watch.elapsed() - askedAt);
            if (answer.value().picture == seen->before) {
                ++seen->same;
            }
            seen->before = answer.value().picture;
        } else {
            ++seen->refused;
        }
        // (Not from inside the driver's own word; and a KVM that had none is given a moment.)
        QTimer::singleShot(
            answer ? 0 : 200, &app,
            // NOLINTNEXTLINE(misc-no-recursion): see above
            [&device, &app, seen, left, &failed, next] { askAnew(device, app, seen, left - 1, failed, next); });
    });
}

// The KVM had live for so many seconds: how many pictures came, and how they came.
void watchLive(core::KvmdDevice& device, QCoreApplication& app, int seconds, int& failed,
               const std::function<void()>& next)
{
    struct Seen {
        std::unique_ptr<core::KvmdLive> live;
        std::unique_ptr<core::Live> one;
        QElapsedTimer watch;
        int pictures = 0;
        qint64 bytes = 0;
        qint64 firstAfterMs = -1;
        qint64 lastAtMs = 0;
        qint64 longestWaitMs = 0;
        QString size = u"of no size that was said"_s;
        bool over = false;
    };
    auto seen = std::make_shared<Seen>();
    seen->watch.start();
    seen->live = std::make_unique<core::KvmdLive>(device);
    seen->one = seen->live->open({1920, 30});
    const auto finish = [seen, &failed, next](const QString& how) {
        if (seen->over) {
            return;
        }
        seen->over = true;
        const qint64 took = seen->watch.elapsed();
        const qint64 shown = seen->firstAfterMs < 0 ? 0 : took - seen->firstAfterMs;
        const double aSecond = shown > 0 && seen->pictures > 1 ? (seen->pictures - 1) * 1000.0 / double(shown) : 0;
        say(u"live: %1 pictures, %2, the first after %3 ms, then %4 a second, the longest wait between two %5 ms, "
            u"%6 bytes in all; %7; %8"_s.arg(seen->pictures)
                .arg(seen->size)
                .arg(seen->firstAfterMs)
                .arg(aSecond, 0, 'f', 1)
                .arg(seen->longestWaitMs)
                .arg(seen->bytes)
                .arg(seen->live->servesNoStream() ? u"asked one after the other (it serves no stream)"_s
                                                  : u"by its stream, or not known yet"_s,
                     how));
        if (seen->pictures == 0) {
            ++failed;
        }
        // (Not from inside its own word: a turn of the loop later.)
        seen->one->close();
        seen->one.release()->deleteLater();
        seen->live.release()->deleteLater();
        next();
    };
    QObject::connect(seen->one.get(), &core::Live::videoChanged, &app, [seen](const core::Live::Video& video) {
        seen->size = u"%1 by %2"_s.arg(video.width).arg(video.height);
    });
    QObject::connect(seen->one.get(), &core::Live::pictureCame, &app, [seen](const core::Live::Picture& picture) {
        const qint64 now = seen->watch.elapsed();
        if (seen->pictures == 0) {
            seen->firstAfterMs = now;
        } else {
            seen->longestWaitMs = std::max(seen->longestWaitMs, now - seen->lastAtMs);
        }
        seen->lastAtMs = now;
        ++seen->pictures;
        seen->bytes += picture.data.size();
    });
    QObject::connect(seen->one.get(), &core::Live::closed, &app,
                     [finish, &failed](const QString& code, const QString& message) {
                         ++failed;
                         finish(u"ended by itself, "_s + code + u": "_s + message);
                     });
    QTimer::singleShot(seconds * 1000, &app,
                       [finish, seconds] { finish(u"closed from here after %1 s"_s.arg(seconds)); });
}

// The line for keyboard and mouse, opened and held, with nothing said on it.
void holdLine(core::KvmdDevice& device, QCoreApplication& app, int seconds, int& failed,
              const std::function<void()>& next)
{
    struct Held {
        std::unique_ptr<core::KvmdConsoleLine> line;
        QElapsedTimer watch;
        bool over = false;
    };
    auto held = std::make_shared<Held>();
    held->watch.start();
    device.openConsole([held, &app, seconds, &failed, next](base::Result<std::unique_ptr<core::KvmdConsoleLine>> made) {
        if (!made) {
            say(u"no line for keyboard and mouse: "_s + why(made.error()));
            ++failed;
            next();
            return;
        }
        held->line = std::move(made).value();
        const auto finish = [held, next](const QString& how) {
            if (held->over) {
                return;
            }
            held->over = true;
            say(u"the line for keyboard and mouse: "_s + how);
            held->line.release()->deleteLater();
            next();
        };
        QObject::connect(held->line.get(), &core::KvmdConsoleLine::opened, &app,
                         [held] { say(u"  the KVM let it in after %1 ms"_s.arg(held->watch.elapsed())); });
        QObject::connect(held->line.get(), &core::KvmdConsoleLine::closed, &app,
                         [finish, &failed](const QString& code, const QString& message) {
                             ++failed;
                             finish(u"ended by itself, "_s + code + u": "_s + message);
                         });
        QTimer::singleShot(seconds * 1000, &app, [held, finish, seconds] {
            if (held->over) {
                return;
            }
            const bool open = held->line->isOpen();
            held->line->close();
            finish(open ? u"still open after %1 s, with nothing said on it but its own asking; closed from here"_s.arg(
                              seconds)
                        : u"never let in within %1 s"_s.arg(seconds));
        });
    });
}

// The least that shows keyboard and mouse arrive: see the top of this file.
void touch(core::KvmdDevice& device, QCoreApplication& app, const QString& folder, int& failed,
           const std::function<void()>& next)
{
    struct Touching {
        std::unique_ptr<core::KvmdConsoleLine> line;
        bool over = false;
    };
    auto touching = std::make_shared<Touching>();
    const auto finish = [touching, next](const QString& how) {
        if (touching->over) {
            return;
        }
        touching->over = true;
        say(u"touching: "_s + how);
        if (touching->line != nullptr) {
            touching->line->close();
            touching->line.release()->deleteLater();
        }
        next();
    };
    // A picture as the KVM captured it, kept under a name; asked again a few times where the KVM
    // says "not yet".
    const auto keep = std::make_shared<std::function<void(QString, int, std::function<void()>)>>();
    *keep = [&device, &app, folder, &failed, finish, keep](const QString& name, int triesLeft,
                                                           const std::function<void()>& then) {
        device.askSnapshot(16384, [&app, folder, name, triesLeft, then, &failed, finish,
                                   keep](const base::Result<core::Snapshot>& picture) {
            if (!picture) {
                if (picture.error().code == "no_stream" && triesLeft > 0) {
                    QTimer::singleShot(500, &app,
                                       [keep, name, triesLeft, then] { (*keep)(name, triesLeft - 1, then); });
                    return;
                }
                ++failed;
                finish(u"no picture, "_s + why(picture.error()));
                return;
            }
            QFile file(QDir(folder).filePath(name));
            if (!file.open(QIODevice::WriteOnly) || file.write(picture.value().picture) < 0) {
                ++failed;
                finish(u"the picture could not be kept"_s);
                return;
            }
            say(u"  kept %1, %2 bytes"_s.arg(name).arg(picture.value().picture.size()));
            then();
        });
    };
    device.openConsole(
        [touching, &app, &failed, finish, keep](base::Result<std::unique_ptr<core::KvmdConsoleLine>> made) {
            if (!made) {
                ++failed;
                finish(u"no line for keyboard and mouse, "_s + why(made.error()));
                return;
            }
            touching->line = std::move(made).value();
            core::KvmdConsoleLine* line = touching->line.get();
            QObject::connect(line, &core::KvmdConsoleLine::closed, &app,
                             [finish, &failed](const QString& code, const QString& message) {
                                 ++failed;
                                 finish(u"the line ended by itself, "_s + code + u": "_s + message);
                             });
            QObject::connect(line, &core::KvmdConsoleLine::opened, &app, [touching, &app, line, finish, keep] {
                constexpr int leftShift = 0xE1;
                constexpr int middle = 16384;
                constexpr int aLittle = 164; // of 32767 across the picture: some thirteen pixels of 2560
                say(u"  the line is in: left Shift down"_s);
                line->key(leftShift, true);
                QTimer::singleShot(80, &app, [touching, &app, line, finish, keep] {
                    if (touching->over) {
                        return;
                    }
                    line->key(leftShift, false);
                    say(u"  left Shift up; the pointer to the middle of the screen"_s);
                    line->pointerAt(middle, middle);
                    QTimer::singleShot(600, &app, [touching, &app, line, finish, keep] {
                        (*keep)(u"a.jpg"_s, 10, [touching, &app, line, finish, keep] {
                            if (touching->over) {
                                return;
                            }
                            say(u"  the pointer a few pixels to the right"_s);
                            line->pointerAt(middle + aLittle, middle);
                            QTimer::singleShot(600, &app, [finish, keep] {
                                (*keep)(u"b.jpg"_s, 10, [finish] { finish(u"done, and the line is closed"_s); });
                            });
                        });
                    });
                });
            });
        });
}

int run(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList asked = QCoreApplication::arguments();
    if (asked.size() < 2) {
        say(u"qkvm-kvmd-rig <address> [--known <sha256>] [--width N] [--pictures N] [--do <action>] "
            u"[--keys <keys>] [--type <text>] [--show <computer>]"_s);
        return 2;
    }
    if (asked.size() == 4 && asked.at(2) == "--probe"_L1) {
        return probe(asked.at(1), std::nullopt);
    }
    if (asked.size() == 6 && asked.at(2) == "--probe"_L1 && asked.at(4) == "--known"_L1) {
        core::KvmProbe::Known known;
        if (asked.at(5) != "windows"_L1) {
            known.byCertificate = true;
            known.certificate = QByteArray::fromHex(asked.at(5).toLatin1());
            if (known.certificate.size() != 32) {
                say(u"--known takes the 64 signs of a SHA-256, or the word windows"_s);
                return 2;
            }
        }
        return probe(asked.at(1), known);
    }
    core::KvmdDevice::Where where;
    where.address = QUrl(asked.at(1));
    int width = 480;
    int pictures = 1;
    int streamed = 0;
    int anew = 0;
    int liveSeconds = 0;
    int lineSeconds = 0;
    QString touchInto;
    QList<core::Action> actions;
    for (qsizetype index = 2; index + 1 < asked.size(); index += 2) {
        const QString& name = asked.at(index);
        const QString& value = asked.at(index + 1);
        if (name == "--known"_L1) {
            where.knownByCertificate = true;
            where.certificate = QByteArray::fromHex(value.toLatin1());
        } else if (name == "--width"_L1) {
            width = value.toInt();
        } else if (name == "--pictures"_L1) {
            pictures = value.toInt();
        } else if (name == "--stream"_L1) {
            streamed = value.toInt();
        } else if (name == "--new"_L1) {
            anew = value.toInt();
        } else if (name == "--live"_L1) {
            liveSeconds = value.toInt();
        } else if (name == "--line"_L1) {
            lineSeconds = value.toInt();
        } else if (name == "--touch"_L1) {
            touchInto = value;
        } else if (name == "--do"_L1) {
            actions.append(core::Action::plain(value));
        } else if (name == "--keys"_L1) {
            actions.append(core::Action::press(value));
        } else if (name == "--type"_L1) {
            actions.append(core::Action::type(value, true));
        } else if (name == "--show"_L1) {
            actions.append(core::Action::show(value.toInt()));
        } else {
            say(u"not understood: "_s + name);
            return 2;
        }
    }

    core::KvmdDevice device(where, qEnvironmentVariable("QKVM_RIG_KVM_PASSWORD"));
    int failed = 0;
    // The steps, one after the other: each says when it is over, and the next begins.
    std::deque<std::function<void(std::function<void()>)>> steps;
    steps.emplace_back([&](const std::function<void()>& next) {
        device.identify([&, next](const base::Result<core::KvmdDevice::Identity>& who) {
            if (who) {
                say(u"it is: model \"%1\", version \"%2\", name \"%3\""_s.arg(who.value().model, who.value().version,
                                                                              who.value().hostname));
            } else {
                say(u"who it is, not known: "_s + why(who.error()));
                ++failed;
            }
            next();
        });
    });
    steps.emplace_back([&](const std::function<void()>& next) {
        QObject::connect(
            &device, &core::KvmdDevice::looked, &app,
            [&, next] {
                const core::Status status = device.status();
                say(u"how it is: %1, picture %2 by %3, signal %4, keyboard %5, power board %6, %7 computers, shown %8"_s
                        .arg(status.state)
                        .arg(status.width)
                        .arg(status.height)
                        .arg(yesNo(status.signal), yesNo(status.keyboard), status.powerIsKnown ? u"yes"_s : u"no"_s)
                        .arg(status.ports.size())
                        .arg(status.shownPort));
                if (!status.errorCode.isEmpty()) {
                    say(u"  "_s + status.errorCode + u": "_s + status.errorMessage);
                    ++failed;
                }
                next();
            },
            Qt::SingleShotConnection);
        device.look();
    });
    for (int number = 0; number < pictures; ++number) {
        steps.emplace_back(
            [&](const std::function<void()>& next) { takePicture(device, app, width, 20, failed, next); });
    }
    if (pictures > 0) {
        // How it is once it is watched: a KVM whose video rests says little of its picture before.
        steps.emplace_back([&](const std::function<void()>& next) {
            QObject::connect(
                &device, &core::KvmdDevice::looked, &app,
                [&, next] {
                    const core::Status status = device.status();
                    say(u"how it is now: %1, picture %2 by %3, signal %4"_s.arg(status.state)
                            .arg(status.width)
                            .arg(status.height)
                            .arg(yesNo(status.signal)));
                    next();
                },
                Qt::SingleShotConnection);
            device.look();
        });
    }
    if (streamed > 0) {
        steps.emplace_back([&](const std::function<void()>& next) { readStream(device, app, streamed, failed, next); });
    }
    if (anew > 0) {
        steps.emplace_back([&](const std::function<void()>& next) {
            auto seen = std::make_shared<OneAfterTheOther>();
            seen->watch.start();
            askAnew(device, app, seen, anew, failed, next);
        });
    }
    if (liveSeconds > 0) {
        steps.emplace_back(
            [&](const std::function<void()>& next) { watchLive(device, app, liveSeconds, failed, next); });
    }
    if (lineSeconds > 0) {
        steps.emplace_back(
            [&](const std::function<void()>& next) { holdLine(device, app, lineSeconds, failed, next); });
    }
    if (!touchInto.isEmpty()) {
        steps.emplace_back([&](const std::function<void()>& next) { touch(device, app, touchInto, failed, next); });
    }
    for (const core::Action& action : std::as_const(actions)) {
        steps.emplace_back([&, action](const std::function<void()>& next) {
            device.act(action, [&, action, next](const base::Result<core::Done>& done) {
                if (done) {
                    say(u"done: "_s + action.kind);
                } else {
                    say(u"not done, %1: %2%3"_s.arg(action.kind, why(done.error()),
                                                    core::KvmdDevice::mayHaveBeenDone(done.error())
                                                        ? u" (it may have been done all the same)"_s
                                                        : QString()));
                    ++failed;
                }
                next();
            });
        });
    }
    std::function<void()> next = [&] {
        if (steps.empty()) {
            QCoreApplication::exit(failed == 0 ? 0 : 1);
            return;
        }
        const auto step = std::move(steps.front());
        steps.pop_front();
        step(next);
    };
    QTimer::singleShot(0, &app, next);
    return QCoreApplication::exec();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "qkvm-kvmd-rig ended: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("qkvm-kvmd-rig ended on something that was thrown.\n", stderr);
        return 1;
    }
}
