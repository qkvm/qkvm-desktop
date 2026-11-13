#include <qkvm/core/kvm_probe.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <utility>

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QStringList>
#include <QTimer>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The ports every address of its kind has anyway, and the largest there is (which has five digits).
constexpr int portOfHttps = 443;
constexpr int portOfHttp = 80;
constexpr int largestPort = 65535;
constexpr qsizetype mostPortDigits = 5;
constexpr int noPort = -1; // as QUrl has it

// Numbers as an address is made of them: four of at most three digits and 255, or eight groups of
// at most four digits to the base of sixteen, with one "::" for groups that are nothing.
constexpr qsizetype partsOfIpv4 = 4;
constexpr qsizetype mostDigitsOfAPart = 3;
constexpr quint32 largestPart = 255;
constexpr qsizetype groupsOfIpv6 = 8;
constexpr qsizetype mostDigitsOfAGroup = 4;
constexpr qsizetype fewestColonPartsOfIpv6 = 3;

bool isDigit(QChar sign)
{
    return sign >= u'0' && sign <= u'9';
}

// One of the twenty-six in its small form, and every other sign as it is. Not QChar's own: that
// knows letters of other alphabets which become one of these (the sign for Kelvin becomes "k").
QChar small(QChar sign)
{
    return sign >= u'A' && sign <= u'Z' ? QChar(sign.unicode() - u'A' + u'a') : sign;
}

// Letters, digits, dots and hyphens, as a host name is made; and the underscore, which no host
// name has by the book and many a name on a home network has all the same. (The server's words.)
bool ofAHostName(QChar sign)
{
    const QChar letter = small(sign);
    return (letter >= u'a' && letter <= u'z') || isDigit(sign) || sign == u'.' || sign == u'_' || sign == u'-';
}

// Whether a sign prints, as the server's language has it: what does not is written out by its
// number in the server's sentence.
bool prints(char32_t sign)
{
    if (sign == U' ') {
        return true;
    }
    switch (QChar::category(sign)) {
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_PrivateUse:
    case QChar::Other_NotAssigned:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
    case QChar::Separator_Space:
        return false;
    default:
        return true;
    }
}

// A text in quotes, as the server's language writes one out for a person to read (Python's repr):
// in single quotes, or in double ones where it has a single one in it and no double one; a
// backslash before a backslash and before the quote it stands in; and what does not print by its
// number.
QString quoted(QStringView text)
{
    const QChar quote = text.contains(u'\'') && !text.contains(u'"') ? u'"' : u'\'';
    const auto byNumber = [](char32_t sign, QLatin1StringView mark, int digits) {
        return mark + QString::number(static_cast<uint>(sign), 16).rightJustified(digits, u'0');
    };
    QString written(quote);
    const QList<uint> signs = text.toUcs4();
    for (const uint one : signs) {
        const char32_t sign = one;
        if (sign == quote.unicode() || sign == U'\\') {
            written += u'\\';
            written += sign == U'\\' ? QChar(u'\\') : quote;
        } else if (sign == U'\t') {
            written += "\\t"_L1;
        } else if (sign == U'\n') {
            written += "\\n"_L1;
        } else if (sign == U'\r') {
            written += "\\r"_L1;
        } else if (prints(sign)) {
            written += QString::fromUcs4(&sign, 1);
        } else if (sign < 0x100) {
            written += byNumber(sign, "\\x"_L1, 2);
        } else if (sign < 0x10000) {
            written += byNumber(sign, "\\u"_L1, 4);
        } else {
            written += byNumber(sign, "\\U"_L1, 8);
        }
    }
    return written + quote;
}

// The server's sentence for a text that is no address, as its page shows it. The server writes
// "not a usable device address: " and the text in quotes, and puts a capital in front the way its
// language does that, which puts everything after the first letter in small letters: so the text
// is shown in small letters, whatever was typed.
base::Error notUsable(QStringView text)
{
    QString shown = text.toString();
    if (shown.size() > kvm::longestTypedAddress) {
        // (Longer than is read at all: as much of it as is read, and not through the middle of a
        // sign that takes two places.)
        const qsizetype kept =
            kvm::longestTypedAddress - (shown.at(kvm::longestTypedAddress - 1).isHighSurrogate() ? 1 : 0);
        shown = shown.left(kept) + "..."_L1;
    }
    return {"invalid", (u"Not a usable device address: "_s + quoted(shown).toLower()).toStdString()};
}

// The port as it was typed: -1 where none was (a colon with nothing behind it is none), nothing at
// all where it is no port. Digits only, however many noughts stand in front, and 1 to 65535.
std::optional<int> portFrom(QStringView typed)
{
    if (typed.isEmpty()) {
        return noPort;
    }
    int port = 0;
    qsizetype counted = 0; // its digits, the noughts in front aside
    for (const QChar sign : typed) {
        if (!isDigit(sign)) {
            return std::nullopt;
        }
        port = port * 10 + (sign.unicode() - u'0');
        if (port != 0 && ++counted > mostPortDigits) {
            return std::nullopt;
        }
    }
    return port >= 1 && port <= largestPort ? std::optional<int>(port) : std::nullopt;
}

// Four numbers with dots between them, each written the plain way: digits only, no nought in
// front, none above 255. As the server reads them, and stricter than Windows and Qt, which take
// "127.1" for 127.0.0.1 and "010.1.1.1" for 8.1.1.1.
std::optional<quint32> ipv4From(QStringView text)
{
    const QList<QStringView> parts = text.split(u'.');
    if (parts.size() != partsOfIpv4) {
        return std::nullopt;
    }
    quint32 address = 0;
    for (const QStringView part : parts) {
        if (part.isEmpty() || part.size() > mostDigitsOfAPart || (part.size() > 1 && part.front() == u'0') ||
            !std::ranges::all_of(part, isDigit)) {
            return std::nullopt;
        }
        quint32 number = 0;
        for (const QChar digit : part) {
            number = number * 10 + (digit.unicode() - u'0');
        }
        if (number > largestPart) {
            return std::nullopt;
        }
        address = (address << 8) | number;
    }
    return address;
}

// One group of an IPv6 address: one to four digits to the base of sixteen, and nothing else.
std::optional<quint16> groupFrom(QStringView text)
{
    if (text.isEmpty() || text.size() > mostDigitsOfAGroup) {
        return std::nullopt;
    }
    quint16 group = 0;
    for (const QChar sign : text) {
        const QChar letter = small(sign);
        if (isDigit(sign)) {
            group = static_cast<quint16>(group * 16 + (sign.unicode() - u'0'));
        } else if (letter >= u'a' && letter <= u'f') {
            group = static_cast<quint16>(group * 16 + (letter.unicode() - u'a' + 10));
        } else {
            return std::nullopt;
        }
    }
    return group;
}

// An IPv6 address as it stands between the brackets: eight groups, or fewer and one "::" for the
// ones that are nothing, the last two written as four plain numbers where somebody likes that. As
// the server reads one (Python's ipaddress, step for step), and stricter than Qt, which takes a
// group with "0x" or a plus sign in front (tried: core/tests/test_kvm_probe.cpp).
std::optional<Q_IPV6ADDR> ipv6From(QStringView text)
{
    const QList<QStringView> split = text.split(u':');
    if (split.size() < fewestColonPartsOfIpv6 || split.size() > groupsOfIpv6 + 1) {
        return std::nullopt;
    }
    QStringList parts;
    for (const QStringView part : split) {
        parts.append(part.toString());
    }
    if (parts.constLast().contains(u'.')) {
        const std::optional<quint32> plain = ipv4From(parts.takeLast());
        if (!plain) {
            return std::nullopt;
        }
        parts.append(QString::number(*plain >> 16, 16));
        parts.append(QString::number(*plain & 0xFFFF, 16));
    }
    if (parts.size() > groupsOfIpv6 + 1) {
        return std::nullopt;
    }
    // Where the "::" is: a part that is nothing, the two ends aside. One at most.
    std::optional<qsizetype> gap;
    for (qsizetype index = 1; index + 1 < parts.size(); ++index) {
        if (parts.at(index).isEmpty()) {
            if (gap) {
                return std::nullopt;
            }
            gap = index;
        }
    }
    // How many groups stand before the "::" and how many behind it. A colon at an end is half of
    // a "::" there, or it is a mistake; and a "::" stands for one group at least.
    qsizetype before = parts.size();
    qsizetype behind = 0;
    if (gap) {
        const bool beginsWithAColon = parts.constFirst().isEmpty();
        const bool endsWithAColon = parts.constLast().isEmpty();
        before = *gap - (beginsWithAColon ? 1 : 0);
        behind = parts.size() - *gap - 1 - (endsWithAColon ? 1 : 0);
        if ((beginsWithAColon && before != 0) || (endsWithAColon && behind != 0) || before + behind >= groupsOfIpv6) {
            return std::nullopt;
        }
    } else if (parts.size() != groupsOfIpv6) {
        return std::nullopt;
    }
    Q_IPV6ADDR address{};
    const auto put = [&address, &parts](qsizetype group, qsizetype part) {
        const std::optional<quint16> number = groupFrom(parts.at(part));
        if (number) {
            address[static_cast<int>(group * 2)] = static_cast<quint8>(*number >> 8);
            address[static_cast<int>(group * 2 + 1)] = static_cast<quint8>(*number & 0xFF);
        }
        return number.has_value();
    };
    for (qsizetype index = 0; index < before; ++index) {
        if (!put(index, index)) {
            return std::nullopt;
        }
    }
    for (qsizetype index = 0; index < behind; ++index) {
        if (!put(groupsOfIpv6 - behind + index, parts.size() - behind + index)) {
            return std::nullopt;
        }
    }
    return address;
}

// Whether an answer is an object that has `name` in it, whatever stands under it: how a JetKVM and
// a server's controller are known. Read as every answer of a device is: not beyond its limit, and
// what is no JSON is no answer.
bool says(const QByteArray& answer, QLatin1StringView name)
{
    if (answer.size() > kvm::largestProbeAnswerBytes) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(answer);
    return document.isObject() && document.object().contains(name);
}

QByteArray shownBy(const QSslConfiguration& configuration)
{
    const QSslCertificate shown = configuration.peerCertificate();
    return shown.isNull() ? QByteArray() : shown.digest(QCryptographicHash::Sha256);
}

// Whether an answer's number is one under which what stands before a KVM says that the KVM does
// not answer it: the three the driver reads so (kvmd_stream.cpp). Whose words they are, kvmd's
// own or a gateway's, is told by the shape of what comes with them.
constexpr bool saysWhatIsBehindIsAway(int status)
{
    return status == 502 || status == 503 || status == 504;
}

// One asking of one address: the three questions, and what came of them. It has lines of its own
// (a QNetworkAccessManager made for it), and that is what the word on the certificate rests on:
// * No line is there from an earlier asking. On a line that was there already Qt says nothing of
//   the certificate again, and what Windows had objected to would then pass for trusted.
// * Qt passes over an objection only where it was told to here, and it is told only when a
//   question has heard the objection, which is noted. So where no objection was noted, every line
//   that came up was one Windows trusts, also a line that came up with no question waiting for it,
//   which nobody hears of: Qt lets none up that Windows objects to by itself.
// And nothing but the three questions ever travels on these lines: what Qt was told to pass over
// holds for all of them, which is why no line of a probe is ever one a password goes out on.
//
// An asking that is held to what a KVM is known by (`heldTo`) passes nothing over that the KVM's
// own driver would not: the rule is the driver's (kvm::objectedTo), and so is the look at the
// certificate once the two have agreed on how to speak, before a byte of a question goes out.
// A line that does not pass hears nothing, and where one did not, nothing that answered counts.
class Asking : public QObject {
public:
    // `jetKvmAlone`: the JetKVM's question and no other (KvmProbe::askForAJetKvm).
    Asking(QUrl address, KvmProbe::Pace pace, std::optional<KvmProbe::Known> heldTo, bool jetKvmAlone,
           std::function<void(const KvmProbe::Found&)> hand, QObject* parent)
        : QObject(parent), m_address(std::move(address)), m_pace(pace), m_heldTo(std::move(heldTo)),
          m_jetKvmAlone(jetKvmAlone), m_hand(std::move(hand))
    {
        // Never somewhere else by itself, and straight to the address: a KVM is on the local network,
        // where the proxy this PC may use for the web is not.
        m_network.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
        m_network.setProxy(QNetworkProxy::NoProxy);
    }

    void begin()
    {
        // What each family answers to anyone. kvmd says who is signed in, in its own shape whether
        // somebody is or not; a JetKVM says how it is; a controller shows the root of its service.
        // (Beside an address, where a JetKVM may answer the other way, its question alone: the two
        // that are not asked are over before they began, and answered nothing.)
        if (m_jetKvmAlone) {
            m_questions[IsKvmd].over = true;
            m_questions[IsRedfish].over = true;
        } else {
            put(IsKvmd, u"/api/auth/check"_s);
            put(IsRedfish, u"/redfish/v1/"_s);
        }
        put(IsJetKvm, u"/device/status"_s);
    }

private:
    enum Which { IsKvmd, IsJetKvm, IsRedfish, Questions };

    struct Question {
        bool wentOut = false;    // Qt has said that it went
        bool gaveUp = false;     // it was not let in, or took too long
        bool tooMuch = false;    // the answer is larger than is taken
        bool packed = false;     // or packed, which was not asked for
        bool over = false;       // it has ended, one way or another
        bool answered = false;   // something answered it, whatever it said
        bool saidYes = false;    // and said that it is of this family
        bool byAGateway = false; // a gateway answered it, and said that what is behind it is away
        bool refused = false;    // its line showed what the KVM is not known by: it was not let out
        bool asked = false;      // it was put at all
        bool turnedAway = false; // its line was refused as it called: nothing listens there
        QByteArray soFar;        // what has arrived of the answer
        QByteArray certificate;  // what the line it travelled on showed
    };

    void put(Which which, const QString& path)
    {
        m_questions[which].asked = true;
        QUrl url = m_address;
        url.setPath(path);
        QNetworkRequest request(url);
        // (A controller's root, as long as a controller's driver waits for it: Pace::rootQuietMs.)
        const int quietMs = which == IsRedfish ? std::max(m_pace.quietMs, m_pace.rootQuietMs) : m_pace.quietMs;
        request.setTransferTimeout(quietMs);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        // Qt's store of cookies is neither written nor read: what answers here sets nothing that a
        // later question would carry.
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        // Nor does Qt sign on to anything by itself. Asked to in some ways (NTLM, Negotiate), it
        // would answer with the account of whoever is signed in to Windows, and send the question
        // again: an address nothing is known about is not handed that.
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        // One way of speaking, the plain one, in which Qt says when a question has gone out.
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        // It is asked not to pack its answer: what unpacks to how much is then nobody's guess.
        request.setRawHeader("Accept-Encoding"_ba, "identity"_ba);
        const bool encrypted = m_address.scheme() == "https"_L1;
        if (encrypted && m_heldTo && m_heldTo->byCertificate) {
            // Held to a certificate, and known by that alone: Windows vouches for nobody here and
            // objects to every certificate, the KVM's own included, so that the look below is made
            // for every line that is made (as the KVM's driver has it).
            request.setSslConfiguration(kvm::knownByItsCertificateAlone(QSslConfiguration::defaultConfiguration()));
        }
        QNetworkReply* reply = m_network.get(request);

        // An answer is not gathered by Qt beyond what is taken while the interface is busy elsewhere.
        reply->setReadBufferSize(kvm::largestProbeAnswerBytes + 1);
        const auto wentOut = [this, which] { m_questions[which].wentOut = true; };
        connect(reply, &QNetworkReply::requestSent, this, wentOut);
        connect(reply, &QNetworkReply::metaDataChanged, this, wentOut);
        // To be let in at all it has a moment: an address where nothing is takes Windows far longer
        // to give up on. (Qt's word that a question went comes a moment after it went: a question
        // that is told "nothing there" for that is asked again by whoever typed the address.)
        QTimer::singleShot(m_pace.connectWithinMs, reply, [this, which, reply] {
            if (!m_questions[which].wentOut && !reply->isFinished()) {
                m_questions[which].gaveUp = true;
                reply->abort();
            }
        });
        // Nor may it keep a question open for as long as it likes, a byte at a time.
        QTimer::singleShot(m_pace.withinMs + (quietMs - m_pace.quietMs), reply, [this, which, reply] {
            if (!reply->isFinished()) {
                m_questions[which].gaveUp = true;
                reply->abort();
            }
        });
        // A controller's root is waited for that long only where something at the address has answered by the time
        // an answer's quiet is over: a controller answers the other questions at once (a 404, a 401). Where nothing
        // has, it is given up on then, as every question is: a line taken by something that says nothing at all is
        // no slow controller, and a box that asks there is not kept waiting the longer time for it.
        if (quietMs > m_pace.quietMs) {
            QTimer::singleShot(m_pace.quietMs, reply, [this, which, reply] {
                if (!reply->isFinished() && std::ranges::none_of(m_questions, &Question::answered)) {
                    m_questions[which].gaveUp = true;
                    reply->abort();
                }
            });
        }

        // Windows does not trust what was shown. That is no reason to stop here: nothing secret is
        // sent, and the answer only says what is there. It is noted, and passed over for exactly
        // what was objected to.
        connect(reply, &QNetworkReply::sslErrors, this, [this, which, reply](const QList<QSslError>& errors) {
            m_objected = true;
            note(which, reply);
            if (!m_heldTo) {
                reply->ignoreSslErrors(errors);
                return;
            }
            // Held to what a KVM is known by: passed over only for the certificate it is known
            // by, and only where every objection names that certificate itself. Anything else is
            // not the KVM: Qt is told nothing, lets the line fall, and no question goes out on it.
            const kvm::Objected objected = kvm::objectedTo(reply->sslConfiguration().peerCertificate(), errors,
                                                           m_heldTo->byCertificate, m_heldTo->certificate);
            if (objected == kvm::Objected::PassedOver) {
                reply->ignoreSslErrors(errors);
                return;
            }
            refuse(which, objected);
        });
        // The two have agreed on how to speak, and nothing has been sent yet.
        connect(reply, &QNetworkReply::encrypted, this, [this, which, reply] {
            note(which, reply);
            if (m_heldTo && m_heldTo->byCertificate && shownBy(reply->sslConfiguration()) != m_heldTo->certificate) {
                refuse(which, kvm::Objected::Changed);
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::metaDataChanged, this, [this, which, reply] {
            const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
            if (length.isValid() && length.toLongLong() > kvm::largestProbeAnswerBytes) {
                m_questions[which].tooMuch = true;
                reply->abort();
                return;
            }
            const QByteArray packing = reply->rawHeader("Content-Encoding"_ba).trimmed().toLower();
            if (!packing.isEmpty() && packing != "identity"_ba) {
                m_questions[which].packed = true;
                reply->abort();
            }
        });
        // Nor did every answer say how large it is. What arrives is taken out of Qt's store as it
        // comes, and counted here. (Left lying there until the answer is whole, an answer that is
        // larger than the store never becomes more than the store holds, and is not seen to be
        // too much: it stops arriving, and ends as one that took too long. Tried:
        // core/tests/test_kvm_probe.cpp.)
        connect(reply, &QNetworkReply::readyRead, this, [this, which, reply] {
            gather(which, reply);
            if (m_questions[which].tooMuch) {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, which, reply] { came(which, reply); });
    }

    // Takes what has arrived of an answer, and lets go of all of it once it is more than is taken.
    void gather(Which which, QNetworkReply* reply)
    {
        Question& question = m_questions[which];
        if (!reply->isOpen()) {
            return; // given up, or ended by the line: nothing is there to take, and Qt says so aloud
        }
        question.soFar += reply->readAll();
        if (question.soFar.size() > kvm::largestProbeAnswerBytes) {
            question.tooMuch = true;
            question.soFar.clear();
        }
    }

    // A question's line showed what the KVM is not known by. The first word on that counts.
    void refuse(Which which, kvm::Objected how)
    {
        m_questions[which].refused = true;
        if (!m_refused) {
            m_refused = how;
        }
    }

    // The certificate of the line a question is on, wherever Qt says it: the last word counts,
    // which is the line the answer came on.
    void note(Which which, const QNetworkReply* reply)
    {
        if (const QByteArray shown = shownBy(reply->sslConfiguration()); !shown.isEmpty()) {
            m_questions[which].certificate = shown;
        }
    }

    void came(Which which, QNetworkReply* reply)
    {
        Question& question = m_questions[which];
        if (std::exchange(question.over, true)) {
            return;
        }
        reply->deleteLater();
        note(which, reply);
        gather(which, reply);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError error = reply->error();
        // Refused as it called, and nothing else: whatever let the line in, kept it waiting, or failed
        // in another way may be something that listens.
        question.turnedAway = error == QNetworkReply::ConnectionRefusedError;
        // Of the line, and not what was answered: Qt numbers those below 200, and one more.
        const bool ofTheLine = error != QNetworkReply::NoError &&
                               (static_cast<int>(error) < 200 || error == QNetworkReply::ProtocolFailure);
        if (question.tooMuch || question.packed) {
            // Too much, or packed oddly: somebody is there all the same. (The server's words. It
            // unpacks what was packed the common way and reads on; this program unpacks nothing
            // a device sends, so a KVM behind something that packs regardless is "something else".)
            question.answered = true;
            // (Under a gateway's number it is a gateway's page all the same: more than kvmd ever
            // says, or packed, these are not kvmd's words, as the driver has it for a stream.)
            question.byAGateway = saysWhatIsBehindIsAway(status);
        } else if (!question.gaveUp && !question.refused && !ofTheLine && status != 0) {
            question.answered = true;
            // What stands before a KVM says 502, 503 or 504 of a KVM that does not answer it, on
            // a page of its own. In kvmd's own shape it is kvmd that speaks, and that is no
            // gateway's word.
            question.byAGateway = saysWhatIsBehindIsAway(status) && !kvmd::isKvmd(question.soFar);
            switch (which) {
            case IsKvmd:
                // In its own shape, whether it says "you are in" or "you are not".
                question.saidYes = (status == 200 || status == 401 || status == 403) && kvmd::isKvmd(question.soFar);
                break;
            case IsJetKvm:
                question.saidYes = status == 200 && says(question.soFar, "isSetup"_L1);
                break;
            case IsRedfish:
                question.saidYes = status == 200 && says(question.soFar, "RedfishVersion"_L1);
                break;
            case Questions:
                break;
            }
        }
        question.soFar.clear();
        if (std::ranges::all_of(m_questions, &Question::over)) {
            decide();
        }
    }

    void decide()
    {
        using Kind = KvmProbe::Kind;
        // In the server's order where something answers as two (detect_kind). And otherwise than
        // the server in one thing: there, whatever is neither a JetKVM nor a controller is taken to
        // speak kvmd, and is found out when it is signed in to. Here it is a kvmd only if it
        // answered in kvmd's shape (as the server's search for devices has it): a password is not
        // sent to something to find out what it is.
        //
        // One address, as it was given. A JetKVM serves plain HTTP, and HTTPS beside it once its
        // owner turns that on, and the server asks it both ways whatever was typed: the way beside
        // an address is asked by itself (askForAJetKvm), and what follows from the two for where a
        // password may go is weighed by whoever asked both (kvm::wayOf).
        static constexpr std::array order{std::pair{IsJetKvm, Kind::JetKvm}, std::pair{IsRedfish, Kind::Redfish},
                                          std::pair{IsKvmd, Kind::Kvmd}};
        KvmProbe::Found found;
        if (m_refused) {
            // Held to what a KVM is known by, and a line showed something else: what answers
            // there is not taken for the KVM, whatever another line said. What that line showed
            // is told, for whoever puts it before a person.
            const auto refused = std::ranges::find_if(m_questions, &Question::refused);
            found.refused = m_refused;
            found.shown = refused != m_questions.end() ? refused->certificate : QByteArray();
            m_hand(found);
            deleteLater();
            return;
        }
        const Question* by = nullptr;
        for (const auto& [which, kind] : order) {
            if (by == nullptr && m_questions[which].saidYes) {
                found.kind = kind;
                by = &m_questions[which];
            }
        }
        if (by == nullptr) {
            // None of them. Something else, if anything answered at all; and what showed a
            // certificate and then said nothing has still shown it.
            const auto answered = std::ranges::find_if(m_questions, &Question::answered);
            const auto showed =
                std::ranges::find_if(m_questions, [](const Question& one) { return !one.certificate.isEmpty(); });
            if (answered != m_questions.end()) {
                found.kind = Kind::SomethingElse;
                by = &*answered;
                // Where every answer there was is a gateway's word that what is behind it is
                // away, that is told with it: something answers, and says nothing of what the
                // thing behind it is. One answer of another sort (a page, a 404, a 401) is
                // somebody who speaks for themselves, and it stays what it is.
                found.saidByAGatewayAlone = std::ranges::all_of(
                    m_questions, [](const Question& one) { return !one.answered || one.byAGateway; });
            } else if (showed != m_questions.end()) {
                by = &*showed;
            }
        }
        if (by != nullptr) {
            found.shown = by->certificate;
        }
        // Nothing listens there only where every question that was put was refused as it called.
        found.nothingListens =
            std::ranges::any_of(m_questions, &Question::asked) &&
            std::ranges::all_of(m_questions, [](const Question& one) { return !one.asked || one.turnedAway; });
        // (Held to a certificate, Windows was given nobody to vouch for anybody, and objected to
        // the KVM's own: that says nothing of whether it would trust it.)
        const bool windowsWasAsked = !m_heldTo || !m_heldTo->byCertificate;
        found.trustedByWindows = windowsWasAsked && !found.shown.isEmpty() && !m_objected;
        m_hand(found);
        deleteLater();
    }

    QUrl m_address;
    KvmProbe::Pace m_pace;
    // What the KVM is known by, where the asking is held to that; nothing: it is asked of
    // whatever answers, and Windows' objections are passed over.
    std::optional<KvmProbe::Known> m_heldTo;
    bool m_jetKvmAlone = false;
    std::function<void(const KvmProbe::Found&)> m_hand;
    std::array<Question, Questions> m_questions;
    bool m_objected = false; // Windows objected to a certificate shown on one of the lines
    // A line showed what the KVM is not known by, and how: of an asking that is held.
    std::optional<kvm::Objected> m_refused;
    // Last, so that it goes first, and the questions with it, while what they are noted in is
    // still there.
    QNetworkAccessManager m_network;
};

} // namespace

namespace kvm {

base::Result<QUrl> addressFrom(const QString& typed)
{
    const QString text = typed.trimmed();
    const auto refused = [&text] { return notUsable(text); };
    if (typed.size() > longestTypedAddress) {
        return refused();
    }
    // Nothing is taken out of the text before it is read. The server's reader of addresses drops
    // a tab and a line's end wherever they stand and reads on, so that "192.168.1.20", a line's
    // end and "1" are 192.168.1.201 to it: another host than anybody saw. Here such a sign is in
    // the host or the port, where it is refused, or behind them, where it is dropped with the rest.
    QStringView rest(text);
    bool encrypted = true;
    if (schemeWasTyped(text)) {
        // What stands before the first colon says how to speak, in letters small or large.
        const qsizetype colon = rest.indexOf(u':');
        QString scheme;
        for (const QChar sign : rest.left(colon)) {
            scheme += small(sign);
        }
        rest = rest.mid(colon + 1);
        if ((scheme != "https"_L1 && scheme != "http"_L1) || !rest.startsWith("//"_L1)) {
            return refused();
        }
        encrypted = scheme == "https"_L1;
        rest = rest.mid(2);
    }
    // The host and its port, as far as where a path, a question mark or a cross begins.
    const auto ends = [](QChar sign) { return sign == u'/' || sign == u'?' || sign == u'#'; };
    const QStringView authority = rest.left(std::ranges::find_if(rest, ends) - rest.begin());
    if (authority.contains(u'@')) {
        // A name or a password in an address: they are typed where they are asked for.
        return refused();
    }
    QString host;
    QStringView behind; // the port, where there is one
    std::optional<Q_IPV6ADDR> numbers;
    if (authority.startsWith(u'[')) {
        // Brackets hold an IPv6 address, and behind them is nothing or a colon and the port. (The
        // server's reader has a second thing brackets may hold, which no device is ever at, and
        // says of a bracket that is alone in other words than its own sentence.)
        const qsizetype close = authority.indexOf(u']');
        behind = close < 0 ? QStringView() : authority.mid(close + 1);
        numbers = close < 0 ? std::nullopt : ipv6From(authority.mid(1, close - 1));
        if (!numbers || (!behind.isEmpty() && behind.front() != u':')) {
            return refused();
        }
        behind = behind.mid(1);
        // Written the short way, so that "::1" and "0:0:0:0:0:0:0:1" are one address.
        host = u'[' + QHostAddress(*numbers).toString() + u']';
    } else {
        const qsizetype colon = authority.indexOf(u':');
        behind = colon < 0 ? QStringView() : authority.mid(colon + 1);
        for (const QChar sign : colon < 0 ? authority : authority.left(colon)) {
            if (!ofAHostName(sign)) {
                return refused();
            }
            host += small(sign);
        }
        // One device, one way to write where it is: with capitals, or with a dot after its name,
        // it would be another address each time, and a tile each.
        while (host.endsWith(u'.')) {
            host.chop(1);
        }
        if (host.isEmpty()) {
            return refused();
        }
        // What ends in a number is numbers, and those are written the one way too.
        const QStringView last = QStringView(host).mid(host.lastIndexOf(u'.') + 1);
        if ((std::ranges::all_of(last, isDigit) || last.startsWith("0x"_L1)) && !ipv4From(host)) {
            return refused();
        }
    }
    const std::optional<int> port = portFrom(behind);
    if (!port) {
        return refused();
    }
    QUrl address;
    address.setScheme(encrypted ? u"https"_s : u"http"_s);
    address.setHost(host, QUrl::StrictMode);
    address.setPort(*port == (encrypted ? portOfHttps : portOfHttp) ? noPort : *port);
    // What will have to connect to it has the last word on whether it is an address, as with the
    // server: there the library that makes the lines, here Qt. And Qt is to have made of the host
    // what was written here, and nothing else. Where it will not have a name (a part of it that
    // is longer than one may be, or begins with a hyphen), or reads one otherwise ("0b0" is
    // numbers to the base of two to it, and 0.0.0.0), that is no address here.
    const bool asWritten =
        numbers ? QHostAddress(address.host()) == QHostAddress(*numbers) : address.host(QUrl::FullyEncoded) == host;
    if (!address.isValid() || !asWritten) {
        return refused();
    }
    return address;
}

bool schemeWasTyped(const QString& typed)
{
    return typed.contains("://"_L1);
}

std::optional<QUrl> otherWayOf(const QUrl& address)
{
    const QString scheme = address.scheme();
    if ((scheme != "https"_L1 && scheme != "http"_L1) || address.host().isEmpty()) {
        return std::nullopt;
    }
    // (No port, or the one its way has anyway: anything else is somebody's own arrangement.)
    const bool encrypted = scheme == "https"_L1;
    if (address.port() != noPort && address.port() != (encrypted ? portOfHttps : portOfHttp)) {
        return std::nullopt;
    }
    QUrl other;
    other.setScheme(encrypted ? u"http"_s : u"https"_s);
    other.setHost(address.host(QUrl::FullyEncoded), QUrl::StrictMode);
    return other.isValid() ? std::optional<QUrl>(other) : std::nullopt;
}

std::optional<QUrl> bothWaysOf(const QString& typed, const QUrl& address)
{
    if (schemeWasTyped(typed)) {
        return otherWayOf(address);
    }
    // The same text, read as every address is read, the plain way: so the port is the one that was
    // typed, and the one plain HTTP has anyway is not written (the web's device_url of the same).
    const base::Result<QUrl> plain = addressFrom(u"http://"_s + typed.trimmed());
    if (!plain || plain.value().host() != address.host()) {
        return std::nullopt;
    }
    return plain.value();
}

} // namespace kvm

KvmProbe::KvmProbe(QObject* parent) : KvmProbe(Pace{}, parent) {}

// Windows' own TLS, as for a QKVM server (ServerClient) and for a KVM (KvmdDevice): whether
// Windows trusts a certificate is asked of Windows, on every PC.
KvmProbe::KvmProbe(Pace pace, QObject* parent) : QObject(parent), m_pace(pace), m_windowsOwnTls(useWindowsOwnTls()) {}

void KvmProbe::ask(const QUrl& address, std::function<void(Found)> done)
{
    asked(address, std::nullopt, false, std::move(done));
}

void KvmProbe::askAsKnown(const QUrl& address, const Known& known, std::function<void(Found)> done)
{
    asked(address, known, false, std::move(done));
}

void KvmProbe::askForAJetKvm(const QUrl& address, std::function<void(Found)> done)
{
    asked(address, std::nullopt, true, std::move(done));
}

void KvmProbe::asked(const QUrl& address, const std::optional<Known>& heldTo, bool jetKvmAlone,
                     std::function<void(Found)> done)
{
    // Always a turn of the loop later, whichever way it comes about, and not at all once the
    // probe is gone.
    auto hand = [this, done = std::move(done)](const Found& found) {
        QTimer::singleShot(0, this, [done, found = Found(found)] { done(found); });
    };
    const QString scheme = address.scheme();
    if (!address.isValid() || address.host().isEmpty() || (scheme != "https"_L1 && scheme != "http"_L1)) {
        hand(Found{});
        return;
    }
    if (refusedForItsTls(m_windowsOwnTls, address)) {
        // What a probe tells of a certificate is whether Windows trusts it: not asked of anything
        // else, and so nothing is asked there at all.
        Found none;
        none.kind = Kind::NotAsked;
        hand(none);
        return;
    }
    // The place, and nothing of a page on it. Nor a name or a password, should an address ever
    // carry one: nothing that says who asks is handed to Qt at all.
    QUrl place;
    place.setScheme(scheme);
    place.setHost(address.host(QUrl::FullyEncoded));
    place.setPort(address.port());
    // The probe's own from here on: it goes with the probe, or by itself when it has been told.
    std::make_unique<Asking>(place, m_pace, heldTo, jetKvmAlone, std::move(hand), this).release()->begin();
}

} // namespace qkvm::core
