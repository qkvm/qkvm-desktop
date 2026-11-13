#pragma once

#include "jetkvm_by_hand.h"
#include "jetkvm_peer_by_hand.h"
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMetaObject>
#include <QPoint>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace qkvm::core::testing {
// A JetKVM's one viewer, by hand (M6 2f), on its JetKvmByHand: each offer its signalling line hears is answered by a
// peer of its own, and the viewer before is handed over first: told otherSessionConnected on its rpc, and closed a
// second later (as a real one does; fake_jetkvm.py hand_over). At rpc's open it says
// otaState, localVersion, videoInputState and usbState; it answers the five status questions (ATX both lights off
// while its extension is off) and anything else -32601 (fake_jetkvm.py:360-424, 664-670). Its switches are read on
// the library's threads; what it is asked to do is done on the interface thread.
//
// Its keyboard and its two mice (fake_jetkvm.py's, from the JetKVM's source at 0.5.9), across every connection, as
// the computer behind it has them: `hidrpc`'s handshake (`01 1` answered `01 hidVersion`, unheard for hidDeafForMs
// after the channel opened), `02` the whole keyboard, `05` one key (one that is no modifier let go of 100 ms after it
// was pressed or last kept alive by a `09`), `03` and `06` the absolute and the relative mouse, each with buttons of
// its own; and after every change of the keyboard `33 m k1..k6` to whoever has the viewer, on hidrpc where it shook
// hands, else as `keysDownState` on rpc. At a hand-over and as the viewer's connection ends it lets go of the
// keyboard, never of a mouse button; what an old connection still sends is done all the same.
class JetKvmViewerByHand {
public:
    explicit JetKvmViewerByHand(JetKvmByHand& jet, const char* film = "jetkvm-1080p60.h264") : m_jet(jet), m_film(film)
    {
        jet.takesOffer = [this](const QByteArray& sdp) { return took(sdp); };
        jet.addresses = [this] { return m_peers.empty() ? QList<QByteArray>{} : m_peers.back().peer->candidates(); };
    }
    ~JetKvmViewerByHand()
    {
        m_jet.takesOffer = nullptr;
        m_jet.addresses = nullptr;
        m_peers.clear();
    }
    JetKvmViewerByHand(const JetKvmViewerByHand&) = delete;
    JetKvmViewerByHand& operator=(const JetKvmViewerByHand&) = delete;
    JetKvmViewerByHand(JetKvmViewerByHand&&) = delete;
    JetKvmViewerByHand& operator=(JetKvmViewerByHand&&) = delete;

    // Switches.
    std::atomic<bool> signal{true}; // its computer's screen is there
    std::atomic<bool> answersCalls{true};
    std::atomic<bool> answersTwoTheOtherWayRound{false}; // the first of two calls is answered after the second
    int everyMs = 16;                                    // a picture so often (0: none)
    int firstPictureAfterMs = 0; // its first picture only so long after the offer (a rested JetKVM)
    int handOverCloseMs = 1000;  // the viewer before is closed so long after it was told
    std::string extension;       // what getActiveExtension says ("atx-power" or none); set before
    // What getKeyboardLayout says (set before); where it is not said, it has no such question (-32601, an older one).
    std::string keyboardLayout = "en-US";
    std::atomic<bool> saysItsLayout{true};
    std::atomic<int> layoutsAsked{0};
    // A method never answered, and one at whose call another viewer takes it first (and it is never answered); what
    // getWakeOnLanDevices says (a JSON list). Set before.
    std::string silentOn;
    std::string handOverOn;
    std::string wakeTargets = "[]";
    std::atomic<int> videoStateAfterMs{0}; // getVideoState answered so long after it was asked (as it was then)
    // Where set, what a call is answered with in place of its own answer (any number of texts).
    std::function<QList<QByteArray>(const QByteArray& call)> answersInstead;
    std::atomic<bool> hidrpc{true};          // it answers the hidrpc handshake (false: an older firmware)
    std::atomic<int> hidVersion{1};          // ...with this version
    std::atomic<int> hidDeafForMs{0};        // ...but not within so long of the channel's opening
    std::atomic<bool> hidAnswersAgain{true}; // ...and answers a handshake asked again later
    // Its JSON-RPC reports (keyboardReport, absMouseReport, relMouseReport, wheelReport): answered so long after they
    // came (0: at once), and done as they are answered (a JetKVM does each in a task of its own); where answersReports
    // is false, done as they came and never answered. handOverAtPress: at that press from now (1: the next report that
    // presses), another viewer takes it first, and that report is done and answered after the word (rule 24, W14b).
    std::atomic<int> reportsAnswerAfterMs{0};
    std::atomic<int> pressesAnswerAfterMs{0}; // ...those that press, where set: so that a letting go is done first
    std::atomic<bool> answersReports{true};
    std::atomic<int> handOverAtPress{0};

    // Counts.
    int sessions = 0;       // offers answered
    int peersAliveMost = 0; // the most peers that were up at one time
    std::atomic<int> calls{0};

    // Its keyboard and mice, as the computer behind it has them.
    struct Keyboard {
        int modifier = 0;
        QList<int> down;
        bool operator==(const Keyboard&) const = default;
    };
    // One message on hidrpc, as it came: over which connection (its number, from 1), and whether that one had been
    // handed over by then.
    struct HidMessage {
        int peer = 0;
        QByteArray bytes;
        bool handed = false;
    };
    Keyboard keyboard()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        expire(steady());
        return m_keyboard;
    }
    int buttons()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_absButtons | m_relButtons;
    }
    int absButtons()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_absButtons;
    }
    // The absolute mouse's buttons after every report of it, over every connection.
    QList<int> absButtonsSeen()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_absButtonsSeen;
    }
    int relButtons()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_relButtons;
    }
    QPoint place()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_place;
    }
    // Every message on hidrpc, over every connection (0) or one.
    QList<HidMessage> hidHeard(int peer = 0)
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        QList<HidMessage> heard;
        for (const HidMessage& one : m_hidHeard) {
            if (peer == 0 || one.peer == peer) {
                heard.append(one);
            }
        }
        return heard;
    }
    // Of a kind (its first byte).
    int hidCount(int kind, int peer = 0)
    {
        return static_cast<int>(std::ranges::count_if(hidHeard(peer), [kind](const HidMessage& one) {
            return static_cast<unsigned char>(one.bytes.at(0)) == kind;
        }));
    }
    int keepAlives()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_keepAlives;
    }
    // When (steady, in microseconds) the last message came on hidrpc over a connection, and when that connection went
    // (0: not yet).
    qint64 lastHidAt(int peer)
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_lastHidAt.value(peer);
    }
    qint64 goneAt(int peer)
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_goneAt.value(peer);
    }
    // The JSON-RPC reports that came, over every connection, in order: (method, params); and the most that were in its
    // hands at once (came and not answered).
    QList<std::pair<QString, QJsonObject>> reports()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_reports;
    }
    int mostReportsInFlight()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_mostInFlight;
    }
    // Every call that came on rpc but the reports, over every connection: (method, params).
    QList<std::pair<QString, QJsonObject>> rpcCalls()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        return m_rpcCalls;
    }
    QList<int> autoReleased()
    {
        const std::lock_guard<std::mutex> lock(m_hidLock);
        expire(steady());
        return m_autoReleased;
    }
    // The number of the connection that has the viewer now (0: none).
    int currentNumber() const { return m_peers.empty() || m_peers.back().handed ? 0 : m_peers.back().number; }
    // Something its own: a message on hidrpc to the viewer now (the test's).
    bool sayOnHid(const QByteArray& bytes) const
    {
        JetKvmPeerByHand* peer = current();
        return peer != nullptr && peer->sendBinary("hidrpc", bytes);
    }

    JetKvmPeerByHand* current() const
    {
        return m_peers.empty() || m_peers.back().handed ? nullptr : m_peers.back().peer.get();
    }
    int peersAlive() const { return static_cast<int>(m_peers.size()); }
    // Another viewer takes it: the one there now is told, and its connection is closed handOverCloseMs later.
    void handOver()
    {
        if (m_peers.empty() || m_peers.back().handed) {
            return;
        }
        Held& old = m_peers.back();
        old.handed = true;
        {
            const std::lock_guard<std::mutex> lock(m_hidLock);
            m_handed.insert(old.number);
            m_current = 0;
        }
        old.peer->sendText("rpc", R"({"jsonrpc":"2.0","method":"otherSessionConnected","params":null})");
        // (A real one let go of a key the viewer before held, as the next one came; never of a mouse button.)
        letGoOfTheKeyboard();
        QTimer::singleShot(handOverCloseMs, &m_jet, [this, number = old.number] { drop(number); });
    }
    // The screen came or went: said as it changes, as a real one says it.
    void saySignal(bool ready)
    {
        signal = ready;
        if (JetKvmPeerByHand* peer = current()) {
            peer->sendText("rpc", videoState("videoInputState", ready));
        }
    }
    bool sayOnRpc(const QByteArray& text) const
    {
        JetKvmPeerByHand* peer = current();
        return peer != nullptr && peer->sendText("rpc", text);
    }
    // Its screen changes its size: another film, from its first picture, over the connection it has now.
    bool filmAnew(const char* film) const
    {
        JetKvmPeerByHand* peer = current();
        return peer != nullptr && peer->filmAnew(film);
    }
    // Its pictures stop, or go on from the film's next key frame.
    void pictures(bool on) const
    {
        if (JetKvmPeerByHand* peer = current()) {
            peer->film().keyFrameNext = on;
            peer->film().sending = on;
        }
    }
    // The viewer's connection ends without a word (a restart of its program, the network).
    void dropCurrent()
    {
        if (!m_peers.empty()) {
            drop(m_peers.back().number);
        }
    }

private:
    struct Held {
        int number = 0;
        bool handed = false;
        std::unique_ptr<JetKvmPeerByHand> peer;
    };
    QByteArray took(const QByteArray& sdp)
    {
        handOver();
        auto peer = std::make_unique<JetKvmPeerByHand>(m_film);
        peer->everyMs = everyMs;
        const int number = ++m_made;
        peer->onOpen = [this, number](JetKvmPeerByHand& on, const QByteArray& label) {
            if (label == "hidrpc") {
                const std::lock_guard<std::mutex> lock(m_hidLock);
                m_openedAt[number] = steady();
            }
            if (label == "rpc") {
                on.sendText("rpc", R"({"jsonrpc":"2.0","method":"otaState","params":{"updating":false,)"
                                   R"("appUpdatePending":false,"systemUpdatePending":false}})");
                on.sendText("rpc", R"({"jsonrpc":"2.0","method":"localVersion","params":{"appVersion":"0.5.9",)"
                                   R"("systemVersion":"0.2.7"}})");
                on.sendText("rpc", videoState("videoInputState", signal));
                on.sendText("rpc", R"({"jsonrpc":"2.0","method":"usbState","params":"configured"})");
            }
        };
        peer->onMessage = [this, number](JetKvmPeerByHand& on, const QByteArray& label, const QByteArray& bytes,
                                         bool text) {
            if (label == "rpc" && text) {
                answer(on, number, bytes);
            } else if (label == "hidrpc" && !text) {
                hidCame(on, number, bytes);
            }
        };
        // The viewer's connection ends: its keyboard is let go of (of the viewer it has now only), no mouse button.
        peer->onGone = [this, number](JetKvmPeerByHand& /*on*/) {
            bool viewers = false;
            {
                const std::lock_guard<std::mutex> lock(m_hidLock);
                m_goneAt[number] = steady();
                viewers = m_current == number;
                if (viewers) {
                    m_current = 0;
                }
            }
            if (viewers) {
                letGoOfTheKeyboard();
            }
        };
        {
            const std::lock_guard<std::mutex> lock(m_hidLock);
            m_current = number;
        }
        if (firstPictureAfterMs > 0) {
            peer->film().sending = false;
            QTimer::singleShot(firstPictureAfterMs, &m_jet, [this, number] {
                for (Held& held : m_peers) {
                    if (held.number == number) {
                        held.peer->film().keyFrameNext = true;
                        held.peer->film().sending = true;
                    }
                }
            });
        }
        QByteArray answered = peer->answer(sdp);
        if (answered.isEmpty()) {
            return {};
        }
        ++sessions;
        m_peers.push_back(Held{number, false, std::move(peer)});
        peersAliveMost = std::max(peersAliveMost, peersAlive());
        return answered;
    }
    void drop(int number)
    {
        bool viewers = false;
        {
            const std::lock_guard<std::mutex> lock(m_hidLock);
            viewers = m_current == number;
            if (viewers) {
                m_current = 0;
            }
        }
        if (viewers) {
            letGoOfTheKeyboard();
        }
        std::erase_if(m_peers, [number](const Held& held) { return held.number == number; });
    }
    static qint64 steady()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    // One message on hidrpc, done as the device's queue for it does it. On the library's thread.
    void hidCame(JetKvmPeerByHand& on, int number, const QByteArray& bytes)
    {
        if (bytes.isEmpty()) {
            return;
        }
        const auto kind = static_cast<unsigned char>(bytes.at(0));
        const auto byte = [&bytes](qsizetype at) { return static_cast<unsigned char>(bytes.at(at)); };
        bool echo = false;
        bool answerShake = false;
        {
            const std::lock_guard<std::mutex> lock(m_hidLock);
            const qint64 now = steady();
            m_hidHeard.append(HidMessage{number, bytes, m_handed.contains(number)});
            m_lastHidAt[number] = now;
            echo = expire(now);
            if (kind == 0x01) {
                const qint64 opened = m_openedAt.value(number, now);
                const bool deaf = now - opened < qint64{hidDeafForMs.load()} * 1000;
                const bool again = m_shaken.contains(number) || m_shakesHeard.value(number) > 0;
                ++m_shakesHeard[number];
                if (hidrpc && !deaf && (hidAnswersAgain || !again)) {
                    m_shaken.insert(number);
                    answerShake = true;
                }
            } else if (kind == 0x02 && bytes.size() >= 2) {
                Keyboard now2;
                now2.modifier = byte(1);
                for (qsizetype at = 2; at < std::min<qsizetype>(bytes.size(), 8); ++at) {
                    if (byte(at) != 0) {
                        now2.down.append(byte(at));
                    }
                }
                m_releaseAt.clear();
                echo = setKeyboard(now2) || echo;
            } else if (kind == 0x05 && bytes.size() >= 3) {
                echo = keypress(byte(1), byte(2) == 1, now) || echo;
            } else if (kind == 0x09) {
                ++m_keepAlives;
                for (auto key = m_releaseAt.begin(); key != m_releaseAt.end(); ++key) {
                    key.value() = now + releaseAfterUs;
                }
            } else if (kind == 0x03 && bytes.size() >= 10) {
                const auto x = static_cast<qint32>((quint32{byte(1)} << 24) | (quint32{byte(2)} << 16) |
                                                   (quint32{byte(3)} << 8) | quint32{byte(4)});
                const auto y = static_cast<qint32>((quint32{byte(5)} << 24) | (quint32{byte(6)} << 16) |
                                                   (quint32{byte(7)} << 8) | quint32{byte(8)});
                m_place = QPoint(x, y);
                m_absButtons = byte(9);
                m_absButtonsSeen.append(m_absButtons);
            } else if (kind == 0x06 && bytes.size() >= 4) {
                m_relButtons = byte(3);
            }
        }
        if (answerShake) {
            on.sendBinary("hidrpc", QByteArray::fromHex("01") + QByteArray(1, static_cast<char>(hidVersion.load())));
        }
        if (echo) {
            sayKeysLater();
        }
    }
    // With m_hidLock held. Whether the keyboard changed.
    bool setKeyboard(const Keyboard& now)
    {
        if (now == m_keyboard) {
            return false;
        }
        m_keyboard = now;
        return true;
    }
    bool keypress(int key, bool press, qint64 now)
    {
        Keyboard next = m_keyboard;
        const bool modifier = key >= 0xE0 && key <= 0xE7;
        if (modifier) {
            const int bit = 1 << (key - 0xE0);
            next.modifier = press ? next.modifier | bit : next.modifier & ~bit;
        } else if (press) {
            if (!next.down.contains(key) && next.down.size() < 6) {
                next.down.append(key);
            }
            m_releaseAt[key] = now + releaseAfterUs;
        } else {
            next.down.removeAll(key);
            m_releaseAt.remove(key);
        }
        return setKeyboard(next);
    }
    // Keys pressed by a keypress message that no keep-alive came for in time, let go of by the device itself, at
    // `now` at the latest. With m_hidLock held. Whether the keyboard changed.
    bool expire(qint64 now)
    {
        bool changed = false;
        for (auto key = m_releaseAt.begin(); key != m_releaseAt.end();) {
            if (key.value() <= now) {
                m_autoReleased.append(key.key());
                Keyboard next = m_keyboard;
                next.down.removeAll(key.key());
                changed = setKeyboard(next) || changed;
                key = m_releaseAt.erase(key);
            } else {
                ++key;
            }
        }
        return changed;
    }
    void letGoOfTheKeyboard()
    {
        bool changed = false;
        {
            const std::lock_guard<std::mutex> lock(m_hidLock);
            m_releaseAt.clear();
            changed = setKeyboard(Keyboard{});
        }
        if (changed) {
            sayKeysLater();
        }
    }
    // The keyboard as it is now, to whoever has the viewer: said from the interface thread (where the connections
    // live), a turn later.
    void sayKeysLater()
    {
        QMetaObject::invokeMethod(
            &m_context,
            [this] {
                Keyboard now;
                bool shaken = false;
                int number = 0;
                {
                    const std::lock_guard<std::mutex> lock(m_hidLock);
                    now = m_keyboard;
                    number = m_current;
                    shaken = m_shaken.contains(number);
                }
                JetKvmPeerByHand* peer = current();
                if (peer == nullptr || number == 0 || currentNumber() != number) {
                    return;
                }
                if (shaken) {
                    QByteArray said = QByteArray::fromHex("33");
                    said.append(static_cast<char>(now.modifier));
                    for (int at = 0; at < 6; ++at) {
                        said.append(static_cast<char>(at < now.down.size() ? now.down.at(at) : 0));
                    }
                    peer->sendBinary("hidrpc", said);
                } else {
                    QJsonArray keys;
                    for (const int key : now.down) {
                        keys.append(key);
                    }
                    peer->sendText(
                        "rpc", QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                                         {QStringLiteral("method"), QStringLiteral("keysDownState")},
                                                         {QStringLiteral("params"),
                                                          QJsonObject{{QStringLiteral("modifier"), now.modifier},
                                                                      {QStringLiteral("keys"), keys}}}})
                                   .toJson(QJsonDocument::Compact));
                }
            },
            Qt::QueuedConnection);
    }
    QByteArray videoState(const char* method, bool ready) const
    {
        QJsonObject params{{QStringLiteral("ready"), ready},
                           {QStringLiteral("width"), 1920},
                           {QStringLiteral("height"), 1080},
                           {QStringLiteral("fps"), 60}};
        QJsonObject said{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}};
        if (method != nullptr) {
            said.insert(QStringLiteral("method"), QString::fromLatin1(method));
            said.insert(QStringLiteral("params"), params);
        }
        return QJsonDocument(said).toJson(QJsonDocument::Compact);
    }
    // A report over JSON-RPC, on the library's thread: done as it is answered, as the switches say.
    void reportCame(JetKvmPeerByHand& on, int number, const QJsonObject& call)
    {
        const QString method = call.value(QStringLiteral("method")).toString();
        const QJsonObject params = call.value(QStringLiteral("params")).toObject();
        const QJsonValue id = call.value(QStringLiteral("id"));
        const bool presses =
            (method == QStringLiteral("keyboardReport") && (!params.value(QStringLiteral("keys")).toArray().isEmpty() ||
                                                            params.value(QStringLiteral("modifier")).toInt() != 0)) ||
            ((method == QStringLiteral("absMouseReport") || method == QStringLiteral("relMouseReport")) &&
             params.value(QStringLiteral("buttons")).toInt() != 0);
        {
            const std::lock_guard<std::mutex> lock(m_hidLock);
            m_reports.append({method, params});
            m_mostInFlight = std::max(m_mostInFlight, ++m_inFlight);
        }
        bool handNow = false;
        if (presses) {
            const int left = handOverAtPress.load();
            if (left > 0) {
                handNow = left == 1;
                handOverAtPress = left - 1;
            }
        }
        if (!answersReports || !answersCalls) {
            apply(method, params);
            return;
        }
        const QByteArray reply = QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                                           {QStringLiteral("id"), id},
                                                           {QStringLiteral("result"), QJsonValue::Null}})
                                     .toJson(QJsonDocument::Compact);
        const int after =
            presses && pressesAnswerAfterMs > 0 ? pressesAnswerAfterMs.load() : reportsAnswerAfterMs.load();
        if (after <= 0 && !handNow) {
            apply(method, params);
            {
                const std::lock_guard<std::mutex> lock(m_hidLock);
                --m_inFlight;
            }
            on.sendText("rpc", reply);
            return;
        }
        QMetaObject::invokeMethod(
            &m_context,
            [this, number, method, params, reply, after, handNow] {
                if (handNow) {
                    handOver();
                }
                QTimer::singleShot(std::max(0, after), &m_context, [this, number, method, params, reply] {
                    apply(method, params);
                    {
                        const std::lock_guard<std::mutex> lock(m_hidLock);
                        --m_inFlight;
                    }
                    for (const Held& held : m_peers) {
                        if (held.number == number) {
                            held.peer->sendText("rpc", reply);
                        }
                    }
                });
            },
            Qt::QueuedConnection);
    }
    // What a report over JSON-RPC does to the keyboard and the mice.
    void apply(const QString& method, const QJsonObject& params)
    {
        bool echo = false;
        {
            const std::lock_guard<std::mutex> lock(m_hidLock);
            if (method == QStringLiteral("keyboardReport")) {
                Keyboard now;
                now.modifier = params.value(QStringLiteral("modifier")).toInt();
                for (const auto& key : params.value(QStringLiteral("keys")).toArray()) {
                    if (key.toInt() != 0 && now.down.size() < 6) {
                        now.down.append(key.toInt());
                    }
                }
                m_releaseAt.clear();
                echo = setKeyboard(now);
            } else if (method == QStringLiteral("absMouseReport")) {
                m_place = QPoint(params.value(QStringLiteral("x")).toInt(), params.value(QStringLiteral("y")).toInt());
                m_absButtons = params.value(QStringLiteral("buttons")).toInt();
                m_absButtonsSeen.append(m_absButtons);
            } else if (method == QStringLiteral("relMouseReport")) {
                m_relButtons = params.value(QStringLiteral("buttons")).toInt();
            }
        }
        if (echo) {
            sayKeysLater();
        }
    }
    // On the library's thread.
    void answer(JetKvmPeerByHand& on, int number, const QByteArray& bytes)
    {
        ++calls;
        const QJsonObject call = QJsonDocument::fromJson(bytes).object();
        const QString method = call.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("keyboardReport") || method == QStringLiteral("absMouseReport") ||
            method == QStringLiteral("relMouseReport") || method == QStringLiteral("wheelReport")) {
            reportCame(on, number, call);
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(m_hidLock);
            m_rpcCalls.append({method, call.value(QStringLiteral("params")).toObject()});
        }
        if (method.toStdString() == handOverOn) {
            QMetaObject::invokeMethod(&m_context, [this] { handOver(); }, Qt::QueuedConnection);
            return;
        }
        if (!answersCalls || method.toStdString() == silentOn) {
            return;
        }
        if (answersInstead) {
            for (const QByteArray& text : answersInstead(bytes)) {
                on.sendText("rpc", text);
            }
            return;
        }
        QJsonObject reply{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("id"), call.value(QStringLiteral("id"))}};
        if (method == QStringLiteral("getVideoState")) {
            QJsonObject state{{QStringLiteral("ready"), signal.load()},
                              {QStringLiteral("streaming"), 1},
                              {QStringLiteral("width"), 1920},
                              {QStringLiteral("height"), 1080},
                              {QStringLiteral("fps"), 60}};
            if (!signal) {
                state.insert(QStringLiteral("error"), QStringLiteral("no_signal"));
            }
            reply.insert(QStringLiteral("result"), state);
        } else if (method == QStringLiteral("getUSBState")) {
            reply.insert(QStringLiteral("result"), QStringLiteral("configured"));
        } else if (method == QStringLiteral("getLocalVersion")) {
            reply.insert(QStringLiteral("result"),
                         QJsonObject{{QStringLiteral("appVersion"), QStringLiteral("0.5.9")},
                                     {QStringLiteral("systemVersion"), QStringLiteral("0.2.7")}});
        } else if (method == QStringLiteral("getActiveExtension")) {
            reply.insert(QStringLiteral("result"), QString::fromStdString(extension));
        } else if (method == QStringLiteral("getKeyboardLayout") && saysItsLayout) {
            ++layoutsAsked;
            reply.insert(QStringLiteral("result"), QString::fromStdString(keyboardLayout));
        } else if (method == QStringLiteral("setATXPowerAction") || method == QStringLiteral("reboot") ||
                   method == QStringLiteral("sendWOLMagicPacket") || method == QStringLiteral("setJigglerState")) {
            reply.insert(QStringLiteral("result"), QJsonValue::Null);
        } else if (method == QStringLiteral("getWakeOnLanDevices")) {
            reply.insert(QStringLiteral("result"),
                         QJsonDocument::fromJson(QByteArray::fromStdString(wakeTargets)).array());
        } else if (method == QStringLiteral("getATXState")) {
            reply.insert(QStringLiteral("result"),
                         QJsonObject{{QStringLiteral("power"), false}, {QStringLiteral("hdd"), false}});
        } else {
            reply.insert(QStringLiteral("error"),
                         QJsonObject{{QStringLiteral("code"), -32601},
                                     {QStringLiteral("message"), QStringLiteral("Method not found")}});
        }
        const QByteArray text = QJsonDocument(reply).toJson(QJsonDocument::Compact);
        if (method == QStringLiteral("getVideoState") && videoStateAfterMs > 0) {
            // (Made as it was asked, and said later: what is said meanwhile is newer than it.)
            QMetaObject::invokeMethod(
                &m_context,
                [this, number, text, after = videoStateAfterMs.load()] {
                    QTimer::singleShot(after, &m_context, [this, number, text] {
                        for (const Held& held : m_peers) {
                            if (held.number == number) {
                                held.peer->sendText("rpc", text);
                            }
                        }
                    });
                },
                Qt::QueuedConnection);
            return;
        }
        if (answersTwoTheOtherWayRound) {
            QByteArray first;
            {
                const std::lock_guard<std::mutex> lock(m_holding);
                if (m_held.isEmpty()) {
                    m_held = text;
                    return;
                }
                first = std::exchange(m_held, {});
            }
            on.sendText("rpc", text);
            on.sendText("rpc", first);
            return;
        }
        on.sendText("rpc", text);
    }

    JetKvmByHand& m_jet;
    const char* m_film;
    int m_made = 0;
    std::mutex m_holding;
    QByteArray m_held;
    // Its keyboard and mice (above), across its connections, and what came on hidrpc.
    static constexpr qint64 releaseAfterUs = 100'000;
    std::mutex m_hidLock;
    Keyboard m_keyboard;
    QHash<int, qint64> m_releaseAt; // keys pressed by a keypress message: when each is let go of
    int m_absButtons = 0;
    QList<int> m_absButtonsSeen; // the absolute mouse's buttons, after every report of it
    int m_relButtons = 0;
    QPoint m_place{-1, -1};
    QList<HidMessage> m_hidHeard;
    int m_keepAlives = 0;
    QList<int> m_autoReleased;
    QSet<int> m_handed;
    int m_current = 0; // the connection that has the viewer
    QHash<int, qint64> m_openedAt;
    QHash<int, qint64> m_lastHidAt;
    QHash<int, qint64> m_goneAt;
    QList<std::pair<QString, QJsonObject>> m_reports;
    int m_inFlight = 0;
    int m_mostInFlight = 0;
    QList<std::pair<QString, QJsonObject>> m_rpcCalls;
    QSet<int> m_shaken;
    QHash<int, int> m_shakesHeard;
    QObject m_context; // (what the interface thread is handed: gone with this)
    std::vector<Held> m_peers;
};
} // namespace qkvm::core::testing
