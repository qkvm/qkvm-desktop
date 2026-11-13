#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QTest>

#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;
using namespace qkvm::core;

Q_DECLARE_METATYPE(qkvm::core::Action)

namespace {

QByteArray written(const QJsonObject& said)
{
    return QJsonDocument(said).toJson(QJsonDocument::Compact);
}

// kvmd's own shape for every answer.
QByteArray answered(const QJsonObject& result)
{
    return written({{u"ok"_s, true}, {u"result"_s, result}});
}

QString sign(char32_t number)
{
    return QString::fromUcs4(&number, 1);
}

// One maker's answer to "how is the video": it says whether the computer sends a picture.
QJsonObject streamerOfOneMaker(bool signal, int width = 1920, int height = 1080)
{
    const QJsonObject source{{u"online"_s, signal},
                             {u"captured_fps"_s, 60},
                             {u"resolution"_s, QJsonObject{{u"width"_s, width}, {u"height"_s, height}}}};
    return {{u"streamer"_s, QJsonObject{{u"hdmi"_s, QJsonObject{{u"signal"_s, signal}}}, {u"source"_s, source}}}};
}

// A KVM with several computers behind it, as the maker's published source shapes it.
QJsonObject switchOf(const QList<bool>& plugged, const QJsonValue& shown)
{
    QJsonArray ports;
    QJsonArray links;
    for (qsizetype index = 0; index < plugged.size(); ++index) {
        ports.append(QJsonObject{{u"unit"_s, 0}, {u"channel"_s, index}, {u"name"_s, u""_s}});
        links.append(plugged.at(index));
    }
    return {{u"model"_s, QJsonObject{{u"ports"_s, ports}}},
            {u"summary"_s, QJsonObject{{u"active_port"_s, shown}}},
            {u"video"_s, QJsonObject{{u"links"_s, links}}}};
}

QByteArray png(quint32 width, quint32 height)
{
    QByteArray data("\x89PNG\r\n\x1a\n", 8);
    data += QByteArray("\x00\x00\x00\x0dIHDR", 8);
    for (const quint32 side : {width, height}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            data += static_cast<char>((side >> shift) & 0xFF);
        }
    }
    return data + QByteArray(8, '\0');
}

// A JPEG as far as where it says its size, with a part of `before` bytes ahead of that.
QByteArray jpeg(int width, int height, int before = 14)
{
    QByteArray data("\xff\xd8\xff\xe0", 4);
    data += static_cast<char>(((before + 2) >> 8) & 0xFF);
    data += static_cast<char>((before + 2) & 0xFF);
    data += QByteArray(before, 'x');
    data += QByteArray("\xff\xc0\x00\x11\x08", 5);
    data += static_cast<char>((height >> 8) & 0xFF);
    data += static_cast<char>(height & 0xFF);
    data += static_cast<char>((width >> 8) & 0xFF);
    data += static_cast<char>(width & 0xFF);
    return data + QByteArray(12, '\0');
}

} // namespace

class TestKvmdAnswers : public QObject {
    Q_OBJECT

private slots:
    void aTextOfAKvmIsOneShortLine()
    {
        QCOMPARE(kvmd::plain(u"  Comet\tPro \n"_s), u"Comet Pro"_s);
        // What does not print is a blank: a line end forges no second line in a log, a sign that
        // turns a line around turns nothing.
        QCOMPARE(kvmd::plain(u"a\r\nb"_s + sign(0x202E) + u"c"_s + sign(0x200B) + u"d"_s), u"a b c d"_s);
        QCOMPARE(kvmd::plain(u"a"_s + sign(0x00A0) + sign(0x2028) + u"b"_s), u"a b"_s);
        // A bell, an escape, a sign for private use and one that is none. Half a sign of two
        // places is the mark for what cannot be read, which shows.
        QCOMPARE(kvmd::plain(u"a"_s + sign(0x07) + u"b"_s + sign(0x1B) + u"[31mc"_s), u"a b [31mc"_s);
        QCOMPARE(kvmd::plain(u"a"_s + sign(0xE000) + u"b"_s + sign(0x0378) + u"c"_s), u"a b c"_s);
        QCOMPARE(kvmd::plain(u"a"_s + QString(QChar(0xD800)) + u"b"_s), u"a"_s + sign(0xFFFD) + u"b"_s);
        QCOMPARE(kvmd::plain(QString(500, u'x'), 40), QString(40, u'x'));
        // Cut before it is gone through, and not through the middle of a sign of two places.
        QCOMPARE(kvmd::plain(QString(39, u'x') + sign(0x1F600) + u"yy"_s, 40), QString(39, u'x'));
        QCOMPARE(kvmd::plain(QString(38, u'x') + sign(0x1F600) + u"yy"_s, 40), QString(38, u'x') + sign(0x1F600));
        QCOMPARE(kvmd::plain(QString(100'000, u' ') + u"late"_s, 10), QString());
    }

    void onlyATextOrANumberIsAText()
    {
        QCOMPARE(kvmd::plain(1920), u"1920"_s);
        QCOMPARE(kvmd::plain(42.5), u"42.5"_s);
        QCOMPARE(kvmd::plain(true), QString());
        QCOMPARE(kvmd::plain(QJsonValue()), QString());
        QCOMPARE(kvmd::plain(QJsonObject{{u"a"_s, 1}}), QString());
        QCOMPARE(kvmd::plain(QJsonArray{u"a"_s}), QString());
        QCOMPARE(kvmd::plain(u""_s), QString());
    }

    void anAnswerIsOfKvmdsShapeOrNone()
    {
        const auto result = kvmd::resultFrom(answered({{u"a"_s, 1}}));
        QVERIFY(result.ok());
        QCOMPARE(result.value().value("a"_L1).toInt(), 1);
        for (const QByteArray& none : {QByteArray(), "[]"_ba, "{}"_ba, "{\"ok\":true}"_ba, "{\"result\":[1]}"_ba,
                                       "{\"result\":\"x\"}"_ba, "<html>"_ba, "{\"result\":{}"_ba}) {
            const auto refused = kvmd::resultFrom(none);
            QVERIFY2(!refused.ok(), none.constData());
            QCOMPARE(refused.error().code, std::string("bad_response"));
        }
        // More than a KVM has reason to send is not read at all.
        QByteArray huge = "{\"result\":{\"a\":\""_ba + QByteArray(kvmd::largestReplyBytes, 'x') + "\"}}"_ba;
        QVERIFY(!kvmd::resultFrom(huge).ok());
    }

    void whatIsNestedThousandsDeepIsNotReadAndNothingFallsOver()
    {
        // A device can send brackets without end. What reads them gives up; it does not walk them
        // down a stack of its own until that ends. (The server's drivers are held to the same.)
        for (const int depth : {2000, 100'000}) {
            const QByteArray objects =
                "{\"ok\":true,\"result\":"_ba + "{\"a\":"_ba.repeated(depth) + "1"_ba + "}"_ba.repeated(depth) + "}"_ba;
            const QByteArray lists =
                "{\"ok\":true,\"result\":{\"a\":"_ba + "["_ba.repeated(depth) + "]"_ba.repeated(depth) + "}}"_ba;
            for (const QByteArray& deep : {objects, lists}) {
                QVERIFY(deep.size() < kvmd::largestReplyBytes);
                const auto refused = kvmd::resultFrom(deep);
                QVERIFY(!refused.ok());
                QCOMPARE(refused.error().code, std::string("bad_response"));
                QVERIFY(!kvmd::isKvmd(deep));
                QCOMPARE(kvmd::tokenFrom(deep, {}), QByteArray());
                // Why a KVM did not do something is read from its answer too: here it says nothing.
                QVERIFY(!kvmd::refusalOf(500, deep).code.empty());
            }
        }
    }

    void aKvmdIsKnownByHowItAnswersUnsigned()
    {
        // Signed in or not, it answers in its own shape.
        QVERIFY(kvmd::isKvmd("{\"ok\":false,\"result\":{\"error\":\"UnauthorizedError\"}}"_ba));
        QVERIFY(kvmd::isKvmd(answered({})));
        QVERIFY(!kvmd::isKvmd("{\"result\":{}}"_ba));
        QVERIFY(!kvmd::isKvmd("{\"ok\":true,\"result\":[]}"_ba));
        QVERIFY(!kvmd::isKvmd("{\"isSetup\":true}"_ba));
        QVERIFY(!kvmd::isKvmd("<html>401</html>"_ba));
        QVERIFY(!kvmd::isKvmd(QByteArray()));
    }

    void aPassIsOnlyWhatIsSafeInAHeader()
    {
        QVERIFY(kvmd::isToken("0123abcdefABCDEF._~+/=-"_ba));
        QVERIFY(kvmd::isToken(QByteArray(512, 'a')));
        QVERIFY(!kvmd::isToken(QByteArray(513, 'a')));
        QVERIFY(!kvmd::isToken(QByteArray()));
        for (const QByteArray& unsafe : {"a b"_ba, "a;b"_ba, "a\r\nX-More: 1"_ba, "a\nb"_ba, "a\"b"_ba, "a,b"_ba,
                                         QByteArray("a\0b", 3), "a\xe9"_ba}) {
            QVERIFY2(!kvmd::isToken(unsafe), unsafe.toHex().constData());
        }
    }

    void thePassComesInTheAnswerOrAsOneCookie()
    {
        const QNetworkCookie cookie("auth_token"_ba, "fromcookie"_ba);
        const QNetworkCookie other("something"_ba, "else"_ba);
        // One maker: in the answer.
        QCOMPARE(kvmd::tokenFrom(answered({{u"token"_s, u"frombody"_s}}), {}), "frombody"_ba);
        // The other: as a cookie, and the answer says nothing of it.
        QCOMPARE(kvmd::tokenFrom(answered({}), {other, cookie}), "fromcookie"_ba);
        QCOMPARE(kvmd::tokenFrom("not json"_ba, {cookie}), "fromcookie"_ba);
        QCOMPARE(kvmd::tokenFrom(answered({{u"token"_s, QJsonValue()}}), {cookie}), "fromcookie"_ba);
        QCOMPARE(kvmd::tokenFrom(answered({{u"token"_s, u""_s}}), {cookie}), "fromcookie"_ba);
        // What the answer says is the pass, whatever a cookie says beside it.
        QCOMPARE(kvmd::tokenFrom(answered({{u"token"_s, u"frombody"_s}}), {cookie}), "frombody"_ba);
    }

    void aPassThatIsNoneIsNone()
    {
        const QNetworkCookie cookie("auth_token"_ba, "fromcookie"_ba);
        // Said in the answer, and no pass: a cookie beside it does not stand in for it.
        QCOMPARE(kvmd::tokenFrom(answered({{u"token"_s, u"a b"_s}}), {cookie}), QByteArray());
        QCOMPARE(kvmd::tokenFrom(answered({{u"token"_s, 12345}}), {cookie}), QByteArray());
        QCOMPARE(kvmd::tokenFrom(answered({{u"token"_s, true}}), {cookie}), QByteArray());
        QCOMPARE(kvmd::tokenFrom(answered({{u"token"_s, u"a\r\nSet-Cookie: x=y"_s}}), {}), QByteArray());
        // A KVM that sets its session twice has not said which one it is.
        QCOMPARE(kvmd::tokenFrom(answered({}), {cookie, QNetworkCookie("auth_token"_ba, "second"_ba)}), QByteArray());
        QCOMPARE(kvmd::tokenFrom(answered({}), {QNetworkCookie("auth_token"_ba, "a b"_ba)}), QByteArray());
        QCOMPARE(kvmd::tokenFrom(answered({}), {}), QByteArray());
    }

    void whatAKvmSaysItIs()
    {
        const kvmd::About about = kvmd::aboutFrom({{u"model"_s, u"RM10"_s}, {u"version"_s, u"V1.10.1 release2"_s}});
        QCOMPARE(about.model, u"RM10"_s);
        QCOMPARE(about.version, u"V1.10.1 release2"_s);
        QCOMPARE(kvmd::aboutFrom({{u"model"_s, QString(100, u'm')}}).model, QString(40, u'm'));
        QCOMPARE(kvmd::aboutFrom({{u"model"_s, QJsonArray{1}}}), kvmd::About{});
        QCOMPARE(kvmd::hostnameFrom({{u"hostname"_s, u"glkvm\n"_s}, {u"success"_s, true}}), u"glkvm"_s);
        QCOMPARE(kvmd::hostnameFrom({}), QString());
    }

    void theComputersBehindAKvmWithSeveral()
    {
        const auto said = kvmd::portsFrom(switchOf({true, false, true, true}, 2));
        QVERIFY(said.has_value());
        const kvmd::Ports ports = said.value_or(kvmd::Ports{});
        QCOMPARE(ports.list.size(), 4);
        QCOMPARE(ports.shown, 2);
        QVERIFY(!ports.list.at(0).empty);
        QVERIFY(ports.list.at(1).empty);

        // A name of the KVM's own for one of them, cut to a line.
        QJsonObject named = switchOf({true, true}, 0);
        QJsonObject model = named.value("model"_L1).toObject();
        QJsonArray list = model.value("ports"_L1).toArray();
        list[1] = QJsonObject{{u"name"_s, u" build\nserver "_s}};
        model[u"ports"_s] = list;
        named[u"model"_s] = model;
        QCOMPARE(kvmd::portsFrom(named).value_or(kvmd::Ports{}).list.value(1).name, u"build server"_s);
    }

    void aKvmWithOneComputerHasNone()
    {
        // The empty shell of the answer, which a KVM without a switch may still give.
        QVERIFY(!kvmd::portsFrom({}).has_value());
        QVERIFY(!kvmd::portsFrom(switchOf({}, 0)).has_value());
        QVERIFY(!kvmd::portsFrom(switchOf({true}, 0)).has_value());
        // And one that says something else than which computer is shown.
        QVERIFY(!kvmd::portsFrom(switchOf({true, true}, 2)).has_value());
        QVERIFY(!kvmd::portsFrom(switchOf({true, true}, -1)).has_value());
        QVERIFY(!kvmd::portsFrom(switchOf({true, true}, 0.5)).has_value());
        QVERIFY(!kvmd::portsFrom(switchOf({true, true}, true)).has_value());
        QVERIFY(!kvmd::portsFrom(switchOf({true, true}, u"0"_s)).has_value());
        QVERIFY(!kvmd::portsFrom(switchOf({true, true}, QJsonValue())).has_value());
    }

    void noMoreComputersThanAKvmHas()
    {
        const auto ports = kvmd::portsFrom(switchOf(QList<bool>(200, true), 3));
        QVERIFY(ports.has_value());
        QCOMPARE(ports.value_or(kvmd::Ports{}).list.size(), kvmd::mostPorts);
        QVERIFY(!kvmd::portsFrom(switchOf(QList<bool>(200, true), 16)).has_value());
        // "Is a computer on it" that is no yes or no is not known, and is not "nothing there".
        QJsonObject odd = switchOf({true, true}, 0);
        odd[u"video"_s] = QJsonObject{{u"links"_s, QJsonArray{u"no"_s, 0}}};
        const QList<Port> oddOnes = kvmd::portsFrom(odd).value_or(kvmd::Ports{}).list;
        QCOMPARE(oddOnes.size(), 2);
        QVERIFY(!oddOnes.at(0).empty);
        QVERIFY(!oddOnes.at(1).empty);
    }

    void howAKvmIsFromItsAnswers()
    {
        kvmd::Looked looked;
        looked.streamer = streamerOfOneMaker(true, 2560, 1440);
        looked.info = QJsonObject{{u"health"_s, QJsonObject{{u"temp"_s, QJsonObject{{u"cpu"_s, 42.5}}}}}};
        looked.hid = QJsonObject{{u"keyboard"_s, QJsonObject{{u"online"_s, true}}}};
        looked.atx = QJsonObject{{u"enabled"_s, true}, {u"leds"_s, QJsonObject{{u"power"_s, true}}}};
        looked.about = {u"RM10"_s, u"V1.10.1"_s};
        looked.ports = kvmd::portsFrom(switchOf({true, true, false}, 1));
        const Status status = kvmd::statusFrom(looked);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(status.errorCode.isEmpty());
        QCOMPARE(status.signal, std::optional<bool>(true));
        QCOMPARE(status.width, 2560);
        QCOMPARE(status.height, 1440);
        QCOMPARE(status.keyboard, std::optional<bool>(true));
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(true));
        QCOMPARE(status.temperature, std::optional<double>(42.5));
        QCOMPARE(status.model, u"RM10"_s);
        QCOMPARE(status.version, u"V1.10.1"_s);
        QCOMPARE(status.ports.size(), 3);
        QCOMPARE(status.shownPort, 1);
        QVERIFY(status.hasPicture);
        QCOMPARE(kvmd::sourceWidthFrom(looked.streamer), 2560);
    }

    void thePowerIsGlkvmsWordAndTheLightOnlyWhereThereIsNone()
    {
        // A real Comet with its power board said "power": "on" and a light that was off, under a running PC (the web's
        // power_said, 2026-10-14): shown "Power off", a press on that word would shut it down.
        const auto board = [](const QJsonValue& word, const QJsonValue& light) {
            QJsonObject said{{u"enabled"_s, true}, {u"busy"_s, false}, {u"leds"_s, QJsonObject{{u"power"_s, light}}}};
            if (!word.isUndefined()) {
                said.insert(u"power"_s, word);
            }
            return said;
        };
        QCOMPARE(kvmd::powerSaid(board(u"on"_s, false)), std::optional<bool>(true));
        // A computer asleep is on: the power is there, and a short press wakes it.
        QCOMPARE(kvmd::powerSaid(board(u"sleep"_s, false)), std::optional<bool>(true));
        QCOMPARE(kvmd::powerSaid(board(u"off"_s, true)), std::optional<bool>(false));
        // A word of another kind, or one not known, says nothing: the light beside it is no witness there.
        QCOMPARE(kvmd::powerSaid(board(true, true)), std::nullopt);
        QCOMPARE(kvmd::powerSaid(board(u"On"_s, true)), std::nullopt);
        QCOMPARE(kvmd::powerSaid(board(u"hibernate"_s, true)), std::nullopt);
        QCOMPARE(kvmd::powerSaid(board(QJsonValue(), true)), std::nullopt);
        // Stock kvmd has no word: its light it is.
        QCOMPARE(kvmd::powerSaid(board(QJsonValue(QJsonValue::Undefined), true)), std::optional<bool>(true));
        QCOMPARE(kvmd::powerSaid(board(QJsonValue(QJsonValue::Undefined), false)), std::optional<bool>(false));
        QCOMPARE(kvmd::powerSaid(board(QJsonValue(QJsonValue::Undefined), u"yes"_s)), std::nullopt);

        // And so a card says it.
        kvmd::Looked looked;
        looked.streamer = streamerOfOneMaker(true);
        looked.atx = board(u"on"_s, false);
        Status status = kvmd::statusFrom(looked);
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(true));
        // A board that says it is not there (a Comet's, while it answers somebody else) says nothing of the computer
        // either: its "off" beside that is no word on the power.
        looked.atx =
            QJsonObject{{u"enabled"_s, false}, {u"power"_s, u"off"_s}, {u"leds"_s, QJsonObject{{u"power"_s, false}}}};
        status = kvmd::statusFrom(looked);
        QVERIFY(!status.powerIsKnown);
        QCOMPARE(status.power, std::nullopt);
        looked.atx = QJsonObject{{u"enabled"_s, false}, {u"leds"_s, QJsonObject{{u"power"_s, true}}}};
        QCOMPARE(kvmd::statusFrom(looked).power, std::nullopt);
    }

    void aComputerThatSendsNoPictureIsNoSignal()
    {
        kvmd::Looked looked;
        looked.streamer = streamerOfOneMaker(false);
        const Status status = kvmd::statusFrom(looked);
        QCOMPARE(status.state, u"no_signal"_s);
        QCOMPARE(status.signal, std::optional<bool>(false));
        // And nothing that was not said is made up.
        QVERIFY(!status.keyboard.has_value());
        QVERIFY(!status.powerIsKnown);
        QVERIFY(!status.power.has_value());
        QVERIFY(!status.temperature.has_value());
        QVERIFY(status.ports.isEmpty());
        QCOMPARE(status.shownPort, -1);
    }

    void theOtherMakerSaysItAnotherWay()
    {
        // No "hdmi" at all: whether its capture is there says it.
        kvmd::Looked looked;
        const QJsonObject source{{u"online"_s, false}};
        looked.streamer = QJsonObject{{u"streamer"_s, QJsonObject{{u"source"_s, source}}}};
        looked.info =
            QJsonObject{{u"hw"_s, QJsonObject{{u"health"_s, QJsonObject{{u"temp"_s, QJsonObject{{u"cpu"_s, 51}}}}}}}};
        Status status = kvmd::statusFrom(looked);
        QCOMPARE(status.state, u"no_signal"_s);
        QCOMPARE(status.temperature, std::optional<double>(51));

        // While its video rests it says nothing of it: that is not "no picture".
        looked.streamer = QJsonObject{{u"streamer"_s, QJsonValue()}};
        status = kvmd::statusFrom(looked);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.signal.has_value());
        QCOMPARE(kvmd::sourceWidthFrom(looked.streamer), 0);

        // The first maker's place is there, and says nothing of it: the other way is asked.
        looked.streamer = QJsonObject{
            {u"streamer"_s, QJsonObject{{u"hdmi"_s, QJsonObject{}}, {u"source"_s, QJsonObject{{u"online"_s, false}}}}}};
        QCOMPARE(kvmd::statusFrom(looked).state, u"no_signal"_s);

        // One that says it in the first maker's place, and not as a yes or no, has not said it.
        looked.streamer = QJsonObject{{u"streamer"_s, QJsonObject{{u"hdmi"_s, QJsonObject{{u"signal"_s, u"no"_s}}},
                                                                  {u"source"_s, QJsonObject{{u"online"_s, false}}}}}};
        QVERIFY(!kvmd::statusFrom(looked).signal.has_value());
    }

    void numbersOfAKvmAreHeldToWhatTheyCanBe()
    {
        kvmd::Looked looked;
        looked.streamer = streamerOfOneMaker(true, 200'000, -5);
        looked.info = QJsonObject{{u"health"_s, QJsonObject{{u"temp"_s, QJsonObject{{u"cpu"_s, 5000}}}}}};
        looked.atx = QJsonObject{{u"enabled"_s, u"yes"_s}, {u"leds"_s, QJsonObject{{u"power"_s, 1}}}};
        looked.hid = QJsonObject{{u"keyboard"_s, QJsonObject{{u"online"_s, u"true"_s}}}};
        const Status status = kvmd::statusFrom(looked);
        QCOMPARE(status.width, 0);
        QCOMPARE(status.height, 0);
        QVERIFY(!status.temperature.has_value());
        QVERIFY(!status.powerIsKnown);
        QVERIFY(!status.power.has_value());
        QVERIFY(!status.keyboard.has_value());
        QCOMPARE(kvmd::sourceWidthFrom(looked.streamer), 0);
        QCOMPARE(kvmd::sourceWidthFrom(streamerOfOneMaker(true, 100'000)), 100'000);
        // A width with a fraction is none.
        QJsonObject odd = streamerOfOneMaker(true);
        QJsonObject streamer = odd.value("streamer"_L1).toObject();
        streamer[u"source"_s] = QJsonObject{{u"resolution"_s, QJsonObject{{u"width"_s, 1920.5}}}};
        odd[u"streamer"_s] = streamer;
        QCOMPARE(kvmd::sourceWidthFrom(odd), 0);
    }

    void aPictureIsAskedNoLargerThanItIsDrawn_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("source");
        QTest::addColumn<QString>("size");
        QTest::newRow("a small card") << 240 << 1920 << u"480"_s;
        QTest::newRow("just its size") << 480 << 1920 << u"480"_s;
        QTest::newRow("a little more") << 481 << 1920 << u"640"_s;
        QTest::newRow("the largest of the small") << 960 << 1920 << u"960"_s;
        QTest::newRow("beyond them") << 961 << 1920 << u"full"_s;
        QTest::newRow("nothing to shrink") << 600 << 640 << u"full"_s;
        QTest::newRow("a small computer picture") << 300 << 320 << u"full"_s;
        QTest::newRow("not known how wide") << 300 << 0 << u"480"_s;
        QTest::newRow("still something to shrink") << 600 << 641 << u"640"_s;
    }
    void aPictureIsAskedNoLargerThanItIsDrawn()
    {
        QFETCH(int, width);
        QFETCH(int, source);
        QFETCH(QString, size);
        QCOMPARE(kvmd::sizeFor(width, source), size);
    }

    void whatAPictureIsAskedWith()
    {
        // Always: kvmd's own picture in place of one where the computer sends none, and not an error.
        QCOMPARE(kvmd::snapshotQuery(u"full"_s).toString(), u"allow_offline=1"_s);
        const QUrlQuery small = kvmd::snapshotQuery(u"640"_s);
        QCOMPARE(small.queryItemValue(u"allow_offline"_s), u"1"_s);
        QCOMPARE(small.queryItemValue(u"preview"_s), u"1"_s);
        QCOMPARE(small.queryItemValue(u"preview_max_width"_s), u"640"_s);
        QCOMPARE(small.queryItemValue(u"preview_max_height"_s), u"360"_s);
        QCOMPARE(small.queryItemValue(u"preview_quality"_s), u"88"_s);
        QCOMPARE(kvmd::snapshotQuery(u"480"_s).queryItemValue(u"preview_quality"_s), u"86"_s);
        QCOMPARE(kvmd::snapshotQuery(u"960"_s).queryItemValue(u"preview_max_height"_s), u"540"_s);
        QCOMPARE(kvmd::snapshotQuery(u"something"_s).toString(), u"allow_offline=1"_s);
    }

    void whatAPictureSaysItsSizeIs()
    {
        QCOMPARE(kvmd::pictureSize(png(1920, 1080)), std::optional<QSize>(QSize(1920, 1080)));
        QCOMPARE(kvmd::pictureSize(jpeg(2560, 1440)), std::optional<QSize>(QSize(2560, 1440)));
        // Past a long part before it, and past padding.
        QCOMPARE(kvmd::pictureSize(jpeg(640, 360, 5000)), std::optional<QSize>(QSize(640, 360)));
        QByteArray padded = jpeg(640, 360);
        padded.insert(2, "\xff\xff\xff");
        QCOMPARE(kvmd::pictureSize(padded), std::optional<QSize>(QSize(640, 360)));
        // And past a table, which is numbered among the parts that say the size and says none.
        QByteArray table("\xff\xc4\x00\x09", 4);
        table += QByteArray("\x01\x02\x03\x04\x05\x06\x07", 7);
        QByteArray withTable = jpeg(640, 360);
        withTable.insert(2, table);
        QCOMPARE(kvmd::pictureSize(withTable), std::optional<QSize>(QSize(640, 360)));
    }

    void whatIsNoPictureSaysNoSize()
    {
        QVERIFY(!kvmd::pictureSize(QByteArray()).has_value());
        QVERIFY(!kvmd::pictureSize("<html><body>a page</body></html>"_ba).has_value());
        QVERIFY(!kvmd::pictureSize(png(1920, 1080).left(23)).has_value());
        QVERIFY(!kvmd::pictureSize(QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(40, 'x')).has_value());
        // A JPEG that breaks off before it says, one whose parts lead nowhere, one of no length.
        QVERIFY(!kvmd::pictureSize(jpeg(640, 360).left(20)).has_value());
        QVERIFY(!kvmd::pictureSize(QByteArray("\xff\xd8", 2) + QByteArray(64, 'x')).has_value());
        QVERIFY(!kvmd::pictureSize(QByteArray("\xff\xd8\xff\xe0\x00\x00", 6) + QByteArray(64, '\0')).has_value());
        QVERIFY(!kvmd::pictureSize(QByteArray("\xff\xd8\xff\xe0\x00\x01", 6) + QByteArray(64, '\0')).has_value());
        // One that goes on and on without saying it is not looked through to its end.
        QByteArray endless("\xff\xd8", 2);
        for (int part = 0; part < 5000; ++part) {
            endless += QByteArray("\xff\xe1\x00\x02", 4);
        }
        endless += jpeg(640, 360).mid(2);
        QVERIFY(!kvmd::pictureSize(endless).has_value());
    }

    void aPictureOfAnEnormousStatedSizeIsNotShown()
    {
        QVERIFY(kvmd::isShown(QSize(1920, 1080)));
        QVERIFY(kvmd::isShown(QSize(7680, 4320)));
        QVERIFY(kvmd::isShown(QSize(16384, 2000)));
        QVERIFY(!kvmd::isShown(QSize(16385, 100)));
        QVERIFY(!kvmd::isShown(QSize(100, 16385)));
        QVERIFY(!kvmd::isShown(QSize(8000, 5001)));
        QVERIFY(!kvmd::isShown(QSize(0, 100)));
        QVERIFY(!kvmd::isShown(QSize(100, 0)));
        // A few bytes that say they are four thousand million pixels wide.
        const auto stated = kvmd::pictureSize(png(0xFFFFFFFF, 0xFFFFFFFF));
        QVERIFY(stated.has_value());
        QVERIFY(!kvmd::isShown(stated.value_or(QSize(1, 1))));
    }

    void whatEachActionIsForAKvm()
    {
        auto call = kvmd::callFor(Action::plain(u"atx_power"_s), 0);
        QVERIFY(call.ok());
        QCOMPARE(call.value().path, u"/api/atx/click"_s);
        QCOMPARE(call.value().query.toString(), u"button=power"_s);
        QVERIFY(call.value().body.isEmpty());
        QCOMPARE(call.value().quietMs, 0);
        QCOMPARE(kvmd::callFor(Action::plain(u"atx_power_long"_s), 0).value().query.toString(), u"button=power_long"_s);
        QCOMPARE(kvmd::callFor(Action::plain(u"atx_reset"_s), 0).value().query.toString(), u"button=reset"_s);
        QCOMPARE(kvmd::callFor(Action::plain(u"reboot"_s), 0).value().path, u"/api/upgrade/reboot"_s);
        QVERIFY(kvmd::callFor(Action::plain(u"reboot"_s), 0).value().query.isEmpty());
        QCOMPARE(kvmd::callFor(Action::plain(u"jiggler_on"_s), 0).value().query.toString(), u"jiggler=1"_s);
        QCOMPARE(kvmd::callFor(Action::plain(u"jiggler_off"_s), 0).value().query.toString(), u"jiggler=0"_s);
        QCOMPARE(kvmd::callFor(Action::plain(u"ctrl_alt_del"_s), 0).value().query.queryItemValue(u"keys"_s),
                 u"ControlLeft,AltLeft,Delete"_s);

        call = kvmd::callFor(Action::press(u"MetaLeft,KeyL"_s), 0);
        QCOMPARE(call.value().path, u"/api/hid/events/send_shortcut"_s);
        QCOMPARE(call.value().query.queryItemValue(u"keys"_s), u"MetaLeft,KeyL"_s);

        call = kvmd::callFor(Action::wake(u"AA:bb:01:02:03:0f"_s), 0);
        QCOMPARE(call.value().path, u"/api/wol/wake"_s);
        QCOMPARE(call.value().query.queryItemValue(u"mac"_s), u"AA:bb:01:02:03:0f"_s);

        // Another computer on its screen: the KVM answers once it has switched.
        call = kvmd::callFor(Action::show(2), 4);
        QCOMPARE(call.value().path, u"/api/switch/set_active"_s);
        QCOMPARE(call.value().query.toString(), u"port=2"_s);
        QVERIFY(call.value().quietMs >= 30'000);
    }

    void aTextIsTypedAsItIs()
    {
        // (A letter beyond the plain ones, and what an address would take for more than a letter.)
        const QString typed = u"p"_s + sign(0xE4) + u"ss w&rd=1+2"_s;
        auto call = kvmd::callFor(Action::type(typed, false), 0);
        QVERIFY(call.ok());
        QCOMPARE(call.value().path, u"/api/hid/print"_s);
        QCOMPARE(call.value().query.toString(), u"limit=0"_s);
        QCOMPARE(call.value().body, typed.toUtf8());
        // Typing takes the KVM a while, and it answers when it is done.
        QVERIFY(call.value().quietMs >= 90'000);
        // Enter after it is a line end after it.
        QCOMPARE(kvmd::callFor(Action::type(u"ls"_s, true), 0).value().body, "ls\n"_ba);
        // No text is refused; Enter alone is a text of one sign.
        QVERIFY(!kvmd::callFor(Action::type(QString(), false), 0).ok());
        QCOMPARE(kvmd::callFor(Action::type(QString(), true), 0).value().body, "\n"_ba);
    }

    void aTextIsCountedInCharacters()
    {
        QVERIFY(kvmd::callFor(Action::type(QString(2000, u'x'), false), 0).ok());
        QVERIFY(!kvmd::callFor(Action::type(QString(2001, u'x'), false), 0).ok());
        // With Enter it is one more.
        QVERIFY(!kvmd::callFor(Action::type(QString(2000, u'x'), true), 0).ok());
        QVERIFY(kvmd::callFor(Action::type(QString(1999, u'x'), true), 0).ok());
        // A sign of two places is one character, as the server counts it.
        QString faces;
        for (int count = 0; count < 2000; ++count) {
            faces += sign(0x1F600);
        }
        QVERIFY(kvmd::callFor(Action::type(faces, false), 0).ok());
        QVERIFY(!kvmd::callFor(Action::type(faces + sign(0x1F600), false), 0).ok());
        const auto refused = kvmd::callFor(Action::type(QString(2001, u'x'), false), 0);
        QCOMPARE(refused.error().code, std::string("invalid"));
        QCOMPARE(refused.error().message, std::string("Text must be 1 to 2000 characters"));
    }

    void whatIsNoActionOfAKvmsIsRefused_data()
    {
        QTest::addColumn<Action>("action");
        QTest::addColumn<int>("ports");
        QTest::newRow("a server's power button") << Action::plain(u"power_on"_s) << 0;
        QTest::newRow("a server's light") << Action::plain(u"locate_on"_s) << 0;
        QTest::newRow("nothing") << Action::plain(QString()) << 0;
        QTest::newRow("a path") << Action::plain(u"/api/atx/click"_s) << 0;
        QTest::newRow("a saved action, which is the wall's") << Action::run(SavedAction{}) << 0;
        QTest::newRow("no keys") << Action::press(QString()) << 0;
        QTest::newRow("keys that are no names") << Action::press(u"ctrl+alt+del"_s) << 0;
        QTest::newRow("a key and more") << Action::press(u"KeyA&button=power"_s) << 0;
        QTest::newRow("keys with a line end") << Action::press(u"KeyA\n"_s) << 0;
        QTest::newRow("nine keys") << Action::press(u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG,KeyH,KeyI"_s) << 0;
        QTest::newRow("no address to wake") << Action::wake(QString()) << 0;
        QTest::newRow("half an address") << Action::wake(u"AA:BB:CC"_s) << 0;
        QTest::newRow("an address and more") << Action::wake(u"AA:BB:CC:DD:EE:FF&x=1"_s) << 0;
        QTest::newRow("an address with a line end") << Action::wake(u"AA:BB:CC:DD:EE:FF\n"_s) << 0;
        QTest::newRow("a computer on a KVM with one") << Action::show(0) << 0;
        QTest::newRow("a computer it has not") << Action::show(4) << 4;
        QTest::newRow("a computer before the first") << Action::show(-1) << 4;
    }
    void whatIsNoActionOfAKvmsIsRefused()
    {
        QFETCH(Action, action);
        QFETCH(int, ports);
        const auto refused = kvmd::callFor(action, ports);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("invalid"));
    }

    void keysThatNameTheKeyboardsPowerKeyAreRefused_data()
    {
        QTest::addColumn<QString>("keys");
        QTest::newRow("by itself") << u"Power"_s;
        QTest::newRow("held with others") << u"ControlLeft,AltLeft,Power"_s;
        QTest::newRow("before another") << u"Power,KeyA"_s;
        QTest::newRow("among eight") << u"KeyA,KeyB,KeyC,Power,KeyE,KeyF,KeyG,KeyH"_s;
    }
    void keysThatNameTheKeyboardsPowerKeyAreRefused()
    {
        // It shuts a machine down without the question a power action is asked first: an action
        // that names it is refused as invalid before anything is sent, in the web's words (kvm.py,
        // POWER_KEY_REFUSED), where kvmd would be told the name as it came and has it.
        QFETCH(QString, keys);
        QVERIFY(kvmd::namesThePowerKey(keys));
        const auto refused = kvmd::callFor(Action::press(keys), 0);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("invalid"));
        QCOMPARE(refused.error().message,
                 std::string("The keyboard's Power key is not sent. A machine's power is under Power."));
        QCOMPARE(std::string(kvmd::powerKeyRefused), refused.error().message);
    }

    void onlyTheNamePowerIsThePowerKey()
    {
        // By the whole name between commas, as the web splits it: other names that begin or end
        // so are other keys (or none kvmd has, which it lets fall).
        for (const QString& keys : {u"PowerOff"_s, u"KeyP,Powe"_s, u"ControlLeft,XPower"_s, u"F1"_s}) {
            QVERIFY2(!kvmd::namesThePowerKey(keys), qPrintable(keys));
            QVERIFY2(kvmd::callFor(Action::press(keys), 0).ok(), qPrintable(keys));
        }
    }

    void eightKeysAreTheMost()
    {
        QVERIFY(kvmd::callFor(Action::press(u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG,KeyH"_s), 0).ok());
        QVERIFY(kvmd::callFor(Action::press(u"F1"_s), 0).ok());
        QVERIFY(kvmd::callFor(Action::show(3), 4).ok());
        QVERIFY(kvmd::callFor(Action::wake(u"aa-bb-cc-dd-ee-ff"_s), 0).ok());
    }

    void whyAKvmDidNotDoIt()
    {
        const QByteArray said = written({{u"ok"_s, false},
                                         {u"result"_s, QJsonObject{{u"error"_s, u"BadRequestError"_s},
                                                                   {u"error_msg"_s, u"ATX is not\navailable"_s}}}});
        qkvm::base::Error refusal = kvmd::refusalOf(400, said);
        QCOMPARE(refusal.code, std::string("action_failed"));
        QCOMPARE(refusal.message, std::string("ATX is not available"));
        // Without a sentence, its word for it; without either, that it refused.
        refusal = kvmd::refusalOf(400, written({{u"result"_s, QJsonObject{{u"error"_s, u"BadRequestError"_s}}}}));
        QCOMPARE(refusal.message, std::string("BadRequestError"));
        QCOMPARE(kvmd::refusalOf(502, "<html>"_ba).message, std::string("The device refused (HTTP 502)"));
        QCOMPARE(kvmd::refusalOf(500, QByteArray()).message, std::string("The device refused (HTTP 500)"));
        // A sentence without end is a line.
        const QByteArray longOne = written({{u"result"_s, QJsonObject{{u"error_msg"_s, QString(5000, u'x')}}}});
        QCOMPARE(kvmd::refusalOf(400, longOne).message.size(), std::size_t{200});
    }

    void theComputersAKvmCanWake()
    {
        const QJsonArray devices{
            QJsonObject{{u"mac"_s, u"AA:BB:CC:DD:EE:01"_s}, {u"name"_s, u"build server"_s}},
            QJsonObject{{u"mac"_s, u"AA:BB:CC:DD:EE:02"_s}, {u"hostname"_s, u"nas"_s}, {u"ip"_s, u"192.168.1.20"_s}},
            QJsonObject{{u"mac"_s, u"AA:BB:CC:DD:EE:03"_s}, {u"ip"_s, u"192.168.1.21"_s}},
            QJsonObject{{u"mac"_s, u"AA:BB:CC:DD:EE:04"_s}},
            QJsonObject{{u"mac"_s, u"not an address"_s}, {u"name"_s, u"left out"_s}},
            QJsonObject{{u"name"_s, u"no address"_s}},
            u"not an entry"_s,
        };
        const QList<WakeTarget> targets = kvmd::wakeTargetsFrom({{u"devices"_s, devices}});
        QCOMPARE(targets.size(), 4);
        QCOMPARE(targets.at(0).name, u"build server"_s);
        QCOMPARE(targets.at(0).mac, u"AA:BB:CC:DD:EE:01"_s);
        QCOMPARE(targets.at(1).name, u"nas"_s);
        QCOMPARE(targets.at(2).name, u"192.168.1.21"_s);
        QVERIFY(targets.at(3).name.isEmpty());
        QVERIFY(kvmd::wakeTargetsFrom({}).isEmpty());
        QVERIFY(kvmd::wakeTargetsFrom({{u"devices"_s, u"none"_s}}).isEmpty());
    }

    void noMoreToWakeThanTheProgramLists()
    {
        QJsonArray devices;
        for (int count = 0; count < mostWakeTargets + 50; ++count) {
            devices.append(QJsonObject{{u"mac"_s, u"AA:BB:CC:DD:EE:01"_s}});
        }
        QCOMPARE(kvmd::wakeTargetsFrom({{u"devices"_s, devices}}).size(), mostWakeTargets);
    }
};

QTEST_APPLESS_MAIN(TestKvmdAnswers)
#include "test_kvmd_answers.moc"
