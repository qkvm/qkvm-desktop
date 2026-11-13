#include <qkvm/core/kvm_adding.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

#include <QList>
#include <QPointer>
#include <QStringView>
#include <QTimer>

#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// How many times as long as what is kept of it a text may be as it was typed: the blanks around
// it and the signs nobody sees take room in a box without counting, and not without end. The
// server's own bound. What is longer is not gone through at all.
constexpr qsizetype timesAsLongTyped = 4;

// What a KVM is called when nobody has named it: the same on every one of its make, and so no
// name to tell it by. (The server's list; nothing at all is on it too.)
constexpr std::array namesOfEveryKvm{"glkvm"_L1, "pikvm"_L1, "localhost"_L1, "localhost.localdomain"_L1};

// The user every KVM of this family comes with.
constexpr QLatin1StringView usualUser = "admin"_L1;

// A blank as the server's language has one: what it takes off the two ends of a text, and what it
// parts a text at. Written out, since Qt's own list is shorter by four (U+001C to U+001F, which
// part the records of a file): at an end of a name those are blanks to the server, and gone.
bool isBlank(QChar sign)
{
    const char16_t code = sign.unicode();
    return (code >= 0x09 && code <= 0x0D) || (code >= 0x1C && code <= 0x20) || code == 0x85 || code == 0xA0 ||
           code == 0x1680 || (code >= 0x2000 && code <= 0x200A) || code == 0x2028 || code == 0x2029 || code == 0x202F ||
           code == 0x205F || code == 0x3000;
}

// What moves the cursor and nothing else: the signs Unicode calls "control", these and no others.
bool isControl(char16_t code)
{
    return code < 0x20 || (code >= 0x7F && code <= 0x9F);
}

// What ends a line or a paragraph, and what turns round the direction of the text after it.
bool isNotOfALine(char16_t code)
{
    return code == 0x2028 || code == 0x2029 || (code >= 0x202A && code <= 0x202E) || (code >= 0x2066 && code <= 0x2069);
}

// A blank of no width.
bool hasNoWidth(char16_t code)
{
    return code == 0x200B || code == 0x2060 || code == 0xFEFF;
}

QStringView withoutBlanksAround(QStringView text)
{
    while (!text.isEmpty() && isBlank(text.front())) {
        text = text.mid(1);
    }
    while (!text.isEmpty() && isBlank(text.back())) {
        text.chop(1);
    }
    return text;
}

// The same, and every run of blanks inside it one plain blank.
QString withOneBlankEach(QStringView text)
{
    QString spaced;
    bool owed = false; // a blank, before the next sign that is none
    for (const QChar sign : text) {
        if (isBlank(sign)) {
            owed = !spaced.isEmpty();
        } else {
            if (owed) {
                spaced += u' ';
            }
            owed = false;
            spaced += sign;
        }
    }
    return spaced;
}

// A text as one line that reads as it is written; nothing where it cannot be made one. The
// server's rule for a name, sign for sign (as_a_line in ../app/qkvm/store.py).
std::optional<QString> asALine(QStringView text)
{
    QString line;
    line.reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar sign = text.at(index);
        const char16_t code = sign.unicode();
        if (code == u'\t') {
            line += u' ';
        } else if (hasNoWidth(code)) {
            // Typed without knowing, pasted without seeing: gone without a word.
        } else if (sign.isHighSurrogate() && index + 1 < text.size() && text.at(index + 1).isLowSurrogate()) {
            // A sign from far up, which takes two places: none of those is turned away.
            line += sign;
            line += text.at(++index);
        } else if (sign.isSurrogate() || isControl(code) || isNotOfALine(code)) {
            return std::nullopt;
        } else {
            line += sign;
        }
    }
    return line;
}

// One of the lines that are typed beside an address: what the server calls it, how long it may
// be, and whether a run of blanks in it is one blank.
struct Field {
    QLatin1StringView called;
    qsizetype longest = 0;
    bool oneBlank = false;
    // Something has to be kept of it: one of which nothing is, is refused.
    bool needed = false;
};

// The server's sentence for what a field does not take, as its page shows it: the field, and the
// rule. Never what was typed: it may be a password.
base::Error notTaken(QLatin1StringView field, const QString& rule)
{
    return {"invalid", (QString(field) + u": "_s + rule).toStdString()};
}

base::Error tooLong(QLatin1StringView field, qsizetype longest)
{
    return notTaken(field, u"this may have %1 characters at the most"_s.arg(longest));
}

// What is kept of a field as it was typed (_a_line in ../app/qkvm/server.py).
base::Result<QString> lineFrom(const QString& typed, const Field& field)
{
    if (typed.size() > field.longest * timesAsLongTyped) {
        // (The server says this one in the words of the library that reads its forms. The rule
        // is the same, and here it is said as the rule.)
        return tooLong(field.called, field.longest);
    }
    const std::optional<QString> line = asALine(withoutBlanksAround(typed));
    if (!line) {
        return notTaken(field.called, u"this has to be one line: no line breaks or control characters, and nothing "
                                      "that turns the direction of the writing round"_s);
    }
    // Once more without the blanks around it: a blank of no width may have stood between them
    // and the end. And only then counted.
    QString kept = field.oneBlank ? withOneBlankEach(*line) : withoutBlanksAround(*line).toString();
    if (kept.size() > field.longest) {
        return tooLong(field.called, field.longest);
    }
    if (field.needed && kept.isEmpty()) {
        return notTaken(field.called, u"a name is needed"_s);
    }
    return kept;
}

// Where a KVM is, as the server writes it: its host, and its port where it has one of its own.
QString hostOf(const QUrl& address)
{
    return address.authority(QUrl::FullyEncoded);
}

// The server's words for these: a box shows them as they are, whoever said them.
base::Error unreachable()
{
    return {"unreachable", "Device is unreachable"};
}

base::Error onTheWall()
{
    return {"duplicate", "This device is already on the wall"};
}

// And this program's own, where the server has no words: it signs in to whatever answers, speaks
// to all three families itself, and adds as many at a time as are asked of it.
base::Error somethingElse()
{
    return {"not_a_kvm", "Something answers at this address, but it is not a KVM that this program speaks to"};
}

// What the box that asks has a plain address typed for: the adding box adds it (the web's word, in both of
// its boxes); the change box keeps the KVM over plain HTTP where it is, or moves it there so (`changing`:
// the address of the KVM that is changed).
enum class Doing { Adding, Keeping, Moving };

Doing doingOf(const QUrl& plain, const std::optional<QUrl>& changing)
{
    if (!changing) {
        return Doing::Adding;
    }
    return changing->toString(QUrl::FullyEncoded) == plain.toString(QUrl::FullyEncoded) ? Doing::Keeping
                                                                                        : Doing::Moving;
}

// The web's words where a JetKVM answers only the plain way beside an HTTPS address (detect_kind
// in ../app/qkvm/server.py): "https://" was typed; or nothing was, and something on the HTTPS port
// did not answer in time. Each says what to type to have it the plain way.
base::Error answersOnlyThePlainWay(const QUrl& plain, const std::optional<QUrl>& changing)
{
    QString toDo = u"To add it that way"_s;
    if (const Doing doing = doingOf(plain, changing); doing != Doing::Adding) {
        toDo = doing == Doing::Keeping ? u"To keep it over plain HTTP"_s : u"To move it there over plain HTTP"_s;
    }
    return {"plain_only", (u"Nothing answers over HTTPS at this address, but a JetKVM answers over plain HTTP. "_s +
                           toDo + u", type "_s + plain.toString(QUrl::FullyEncoded))
                              .toStdString()};
}

base::Error somethingElseListensOverHttps(const QUrl& plain, const QUrl& encrypted, const std::optional<QUrl>& changing)
{
    QString toDo = u"To add the JetKVM"_s;
    if (const Doing doing = doingOf(plain, changing); doing != Doing::Adding) {
        toDo = doing == Doing::Keeping ? u"To keep the JetKVM over plain HTTP"_s
                                       : u"To move the JetKVM there over plain HTTP"_s;
    }
    return {"plain_only", u"A JetKVM answers over plain HTTP at this address, and something on its HTTPS port did "
                          "not answer in time. %1, type %2. For the device on HTTPS, type %3 and try again."_s
                              .arg(toDo, plain.toString(QUrl::FullyEncoded), encrypted.toString(QUrl::FullyEncoded))
                              .toStdString()};
}

base::Error givenUp()
{
    return {"given_up", "The adding was given up. Nothing was added."};
}

// What a family this program speaks to by itself is called in a sentence: as its screens call it.
QString calledInASentence(KvmProbe::Kind kind)
{
    if (kind == KvmProbe::Kind::Redfish) {
        return u"server's management controller (BMC)"_s;
    }
    return kind == KvmProbe::Kind::JetKvm ? u"JetKVM"_s : u"GL.iNet Comet / PiKVM"_s;
}

base::Error underWay()
{
    return {"busy", "Another KVM is being added at this moment. Wait for it to finish."};
}

} // namespace

namespace kvm {

std::optional<base::Error> inTheWay(const QList<OwnKvm>& there, const OwnKvm& kvm, const QByteArray& shown)
{
    // At its place: one address is written one way (kvm::addressFrom), and a KVM that is there
    // over HTTPS is not put there again in the clear, or the other way round.
    if (std::ranges::any_of(there, [&kvm](const OwnKvm& known) { return LocalWall::atOnePlace(known, kvm); })) {
        return onTheWall();
    }
    // What the line showed is the certificate one of them is known by: that one, whoever vouches for that
    // certificate here (it has the key to it). Proven, and before any number that is said. (By the wall's one rule
    // for that, LocalWall::sameCertificate: never of a server's controller, whichever of the two.)
    if (!shown.isEmpty()) {
        OwnKvm showing = kvm;
        showing.knownByCertificate = true;
        showing.certificate = shown;
        const auto twin = std::ranges::find_if(
            there, [&showing](const OwnKvm& known) { return LocalWall::sameCertificate(known, showing); });
        if (twin != there.end()) {
            return LocalWall::sameDevice(*twin);
        }
    }
    // Under another address: a KVM makes its certificate itself, so the one it is known by is its
    // own, by whichever address it is reached. (The wall's own rule and sentence, which it keeps
    // by itself where it is told to know a KVM by a certificate.)
    if (const std::optional<OwnKvm> twin = LocalWall::twinOf(there, kvm)) {
        return LocalWall::sameDevice(*twin, !LocalWall::sameCertificate(*twin, kvm));
    }
    return std::nullopt;
}

base::Result<QString> nameFrom(const QString& typed)
{
    return lineFrom(typed, Field{"name"_L1, longestName});
}

base::Result<QString> groupFrom(const QString& typed)
{
    return lineFrom(typed, Field{"group"_L1, longestGroup, true});
}

base::Result<QString> userFrom(const QString& typed)
{
    base::Result<QString> user = lineFrom(typed, Field{"username"_L1, longestUser});
    if (user && user.value().isEmpty()) {
        // (Otherwise than the server, which takes blanks for "admin" and refuses nothing at all:
        // its own page does not send a box that is empty.)
        return QString(usualUser);
    }
    return user;
}

base::Result<QString> neededLineFrom(const QString& typed, QLatin1StringView called, qsizetype longest)
{
    return lineFrom(typed, Field{called, longest, false, true});
}

QString nameWhereNoneWasTyped(const QString& hostname, const QUrl& address)
{
    // Compared as the server compares them: in the small letters its language makes of a text,
    // which knows a letter of another alphabet that becomes one of these (the sign for Kelvin
    // is a "k").
    const QString small = hostname.toLower();
    const bool saysNothing = small.isEmpty() || std::ranges::any_of(namesOfEveryKvm, [&small](QLatin1StringView one) {
                                 return small == one;
                             });
    QString name = saysNothing ? hostOf(address) : hostname;
    if (name.size() > longestName) {
        // (No longer than a name may be: a KVM named by an address of ninety letters could not be
        // read back from the settings. And not cut through the middle of a sign that takes two
        // places.)
        name.truncate(name.at(longestName - 1).isHighSurrogate() ? longestName - 1 : longestName);
    }
    return name;
}

base::Result<void> passwordFrom(const QString& typed)
{
    if (typed.size() > longestPassword) {
        return tooLong("password"_L1, longestPassword);
    }
    return {};
}

std::optional<KvmProbe::Kind> foundAs(const QString& family)
{
    if (family == "kvmd"_L1) {
        return KvmProbe::Kind::Kvmd;
    }
    if (family == "jetkvm"_L1) {
        return KvmProbe::Kind::JetKvm;
    }
    if (family == "redfish"_L1) {
        return KvmProbe::Kind::Redfish;
    }
    return std::nullopt;
}

QString familyOf(KvmProbe::Kind kind)
{
    switch (kind) {
    case KvmProbe::Kind::Kvmd:
        return u"kvmd"_s;
    case KvmProbe::Kind::JetKvm:
        return u"jetkvm"_s;
    case KvmProbe::Kind::Redfish:
        return u"redfish"_s;
    case KvmProbe::Kind::SomethingElse:
    case KvmProbe::Kind::Nothing:
    case KvmProbe::Kind::NotAsked:
        break;
    }
    return {};
}

base::Error whyNotSignedInTo(KvmProbe::Kind kind)
{
    switch (kind) {
    case KvmProbe::Kind::SomethingElse:
        return somethingElse();
    case KvmProbe::Kind::NotAsked:
        // (Nothing was asked of it, and nothing is: this copy of the program speaks to nothing
        // over HTTPS.)
        return damagedTls();
    case KvmProbe::Kind::Kvmd:
    case KvmProbe::Kind::JetKvm:
    case KvmProbe::Kind::Redfish:
    case KvmProbe::Kind::Nothing:
        break;
    }
    return unreachable();
}

base::Error ofAnotherFamily(KvmProbe::Kind found, const QString& family, AskedAt at)
{
    // (Both by what the screens call them: the one that answers, and the one the KVM is kept as.)
    const std::optional<KvmProbe::Kind> kept = foundAs(family);
    const QString there = calledInASentence(found);
    const QString keptAs = kept ? calledInASentence(*kept) : family;
    // Where this KVM stands, the other is not added while it does ("duplicate"): this one goes first.
    const QString todo =
        at == AskedAt::ItsAddress
            ? u"To have the %1 here, remove this KVM from the wall and add the %1 as a KVM of its own."_s
            : u"To have the %1 here, add it as a KVM of its own."_s;
    return {"other_family", (u"A %1 answers at this address, and this KVM is on the wall as a %2. "_s + todo)
                                .arg(there, keptAs)
                                .toStdString()};
}

Ways waysFor(const QString& typed, const QUrl& address, const std::function<std::optional<QUrl>(const QUrl&)>& otherWay)
{
    Ways ways;
    ways.address = address;
    ways.schemeTyped = schemeWasTyped(typed);
    if (ways.schemeTyped) {
        ways.twin = otherWay ? otherWay(address) : std::nullopt;
    } else {
        ways.twin = bothWaysOf(typed, address);
    }
    if (ways.twin && *ways.twin == address) {
        ways.twin.reset();
    }
    // A port, where nothing was typed before the address: written in the address itself, or in the plain
    // way beside it (443, which the HTTPS address does not write).
    ways.portTyped = !ways.schemeTyped && (address.port() >= 0 || (ways.twin && ways.twin->port() >= 0));
    return ways;
}

base::Result<Way> wayOf(const Ways& ways, const std::optional<QUrl>& changing)
{
    using Kind = KvmProbe::Kind;
    const QUrl& address = ways.address;
    const KvmProbe::Found& there = ways.atAddress;
    const bool encrypted = address.scheme() == "https"_L1;
    // (An HTTPS address that was not asked, TLS not being Windows' own, is said as that, whatever
    // answered beside it: nothing of this copy of the program goes out over HTTPS.)
    if (encrypted && there.kind == Kind::NotAsked) {
        return Way{address, there};
    }
    const auto isAJetKvm = [](const KvmProbe::Found& found) { return found.kind == Kind::JetKvm && !found.refused; };
    const bool beside = ways.twin.has_value() && *ways.twin != address;
    const bool besideEncrypted = beside && ways.twin->scheme() == "https"_L1;
    // 1. A JetKVM over HTTPS, at the address or beside it: there, known by what was shown on that line.
    if (encrypted && isAJetKvm(there)) {
        return Way{address, there};
    }
    if (besideEncrypted && isAJetKvm(ways.atTwin)) {
        return Way{*ways.twin, ways.atTwin};
    }
    // 2. A JetKVM the plain way: where the plain way was typed, there.
    if (!encrypted && isAJetKvm(there)) {
        return Way{address, there};
    }
    // Beside an HTTPS address, only where nothing at all answered there, and that HTTPS was not
    // what was asked for in so many words: what answers there is what was meant, and what says
    // "JetKVM" beside it the plain way may be somebody else (the other port of a gateway, anybody
    // on the way), and is sent no password.
    if (encrypted && beside && !besideEncrypted && isAJetKvm(ways.atTwin)) {
        if (there.kind != Kind::Nothing) {
            return Way{address, there};
        }
        if (ways.schemeTyped) {
            return answersOnlyThePlainWay(*ways.twin, changing);
        }
        // Nothing answered over HTTPS in the time given, and yet something may be there: a device that
        // is only slow is no "nothing", and the password typed for it does not go the plain way beside
        // it unless somebody types that. (Where a port was typed, the two ways are one port, and what
        // answers the plain way there is all that listens on it.)
        if (!ways.portTyped && !there.nothingListens) {
            return somethingElseListensOverHttps(*ways.twin, address, changing);
        }
        return Way{*ways.twin, ways.atTwin};
    }
    // 3. What answers at the address, as it was typed.
    return Way{address, there};
}

void knowByWhatItShowed(OwnKvm& kvm, const QUrl& address, const KvmProbe::Found& found)
{
    // Over plain HTTP there is nothing to know it by. Over HTTPS it is the certificate it has just
    // shown; unless Windows trusts that one for the address as it is, which is somebody's front
    // door, whose certificate changes every few weeks: not one to remember. (A line that showed
    // no certificate is none that Qt makes. Were there one, the KVM would be known by none
    // either, and so not let in unless Windows trusts it.)
    const bool encrypted = address.scheme() == "https"_L1;
    kvm.knownByCertificate = encrypted && !found.trustedByWindows && !found.shown.isEmpty();
    kvm.certificate = kvm.knownByCertificate ? found.shown : QByteArray();
}

} // namespace kvm

DriverPaces KvmAdding::Pace::drivers() const
{
    DriverPaces paces = DriverPaces::alike(driver);
    if (jetkvm) {
        paces.jetkvm = *jetkvm;
    }
    if (redfish) {
        paces.redfish = *redfish;
    }
    // (Where a JetKVM's other way is, the wall has as the boxes have it.)
    paces.jetkvm.otherWay = otherWay;
    return paces;
}

KvmAdding::KvmAdding(LocalWall& wall, QObject* parent) : KvmAdding(wall, Pace{}, parent) {}

KvmAdding::KvmAdding(LocalWall& wall, Pace pace, QObject* parent)
    : QObject(parent), m_wall(wall), m_pace(std::move(pace)), m_probe(m_pace.probe)
{}

KvmAdding::~KvmAdding() = default;

void KvmAdding::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

void KvmAdding::add(Typed typed, std::function<void(base::Result<Added>)> done)
{
    if (m_underWay) {
        later([done = std::move(done)] { done(underWay()); });
        return;
    }
    m_underWay = true;
    m_over = false;
    m_done = std::move(done);

    // What was typed beside the address, in the order the server looks at it.
    const base::Result<QString> name = kvm::nameFrom(typed.name);
    if (!name) {
        finish(name.error());
        return;
    }
    const base::Result<QString> user = kvm::userFrom(typed.user);
    if (!user) {
        finish(user.error());
        return;
    }
    if (const base::Result<void> password = kvm::passwordFrom(typed.password); !password) {
        finish(password.error());
        return;
    }
    const base::Result<QString> group = kvm::groupFrom(typed.group);
    if (!group) {
        finish(group.error());
        return;
    }
    const base::Result<QUrl> address = kvm::addressFrom(typed.address);
    if (!address) {
        finish(address.error());
        return;
    }
    m_address = address.value();
    m_kvm = OwnKvm{};
    m_kvm.name = name.value();
    m_kvm.address = m_address.toString(QUrl::FullyEncoded);
    m_kvm.user = user.value();
    m_kvm.group = group.value();
    m_password = SecretText::takenFrom(typed.password);
    m_keepBoth = typed.keepBoth;
    // One that is on the wall is not asked what it is: that is known.
    if (const std::optional<base::Error> refusal = kvm::inTheWay(m_wall.kvms(), m_kvm)) {
        finish(*refusal);
        return;
    }
    // Asked both ways at once (an address where nothing answers costs one wait, and not two): the
    // three questions at the address, and the JetKVM's alone beside it, where there is a way beside it.
    m_ways = kvm::waysFor(typed.address, m_address, m_pace.otherWay);
    m_waysOut = m_ways.twin ? 2 : 1;
    m_probe.ask(m_address, [this, turn = m_turn](const KvmProbe::Found& found) {
        if (turn == m_turn) {
            m_ways.atAddress = found;
            askedOneWay();
        }
    });
    if (m_ways.twin) {
        m_probe.askForAJetKvm(*m_ways.twin, [this, turn = m_turn](const KvmProbe::Found& found) {
            if (turn == m_turn) {
                m_ways.atTwin = found;
                askedOneWay();
            }
        });
    }
}

void KvmAdding::askedOneWay()
{
    if (--m_waysOut > 0) {
        return;
    }
    // What each HTTPS that was asked showed is judged first, whatever it answered and as whatever family, before a way
    // is chosen (the wall's one judgement, LocalWall::judgeItsHttps; from the outside review, F1): what shows the
    // certificate a KVM of the wall is known by is that one, and nothing goes, whatever a later look does not see.
    for (const auto& [at, found] :
         {std::pair{m_ways.address, m_ways.atAddress}, std::pair{m_ways.twin.value_or(QUrl()), m_ways.atTwin}}) {
        if (at.scheme() != "https"_L1) {
            continue;
        }
        if (const std::optional<base::Error> refusal = m_wall.judgeItsHttps(QString(), QUrl(), found)) {
            finish(*refusal);
            return;
        }
    }
    const base::Result<kvm::Way> way = kvm::wayOf(m_ways);
    if (!way) {
        finish(way.error());
        return;
    }
    // The way chosen is where it is added, and where it is held to every rule from here on.
    m_address = way.value().address;
    m_kvm.address = m_address.toString(QUrl::FullyEncoded);
    probed(way.value().found);
}

void KvmAdding::probed(const KvmProbe::Found& found)
{
    // Before any password goes anywhere: only what answered as a family this program speaks to by
    // itself is signed in to (a kvmd in its own shape, a JetKVM in its).
    const QString family = kvm::familyOf(found.kind);
    if (family.isEmpty()) {
        finish(kvm::whyNotSignedInTo(found.kind));
        return;
    }
    // (Of the family it said it is of: the one it is kept as, and signed in to as.)
    m_kvm.kind = family;
    // What it is known by from now on: the certificate it has just shown, or Windows' word.
    kvm::knowByWhatItShowed(m_kvm, m_address, found);
    // The same KVM under another address is said before its password goes out a second way: by what it showed,
    // whoever vouches for that (from the third reading of the twins).
    m_shown = found.shown;
    if (const std::optional<base::Error> refusal = kvm::inTheWay(m_wall.kvms(), m_kvm, m_shown)) {
        finish(*refusal);
        return;
    }
    // One sign-in, by a driver of its family of its own: on lines of its own, none of them the
    // probe's, and each looked at for the certificate before a byte goes out on it. A JetKVM over plain HTTP has its
    // HTTPS looked at on that sign-in, as the wall's own driver has: where that shows the certificate a KVM of the wall
    // is known by, whatever it answers, that one may be what answers the plain way too, and nothing goes to it (from
    // the third reading of the twins, L4).
    // (Nothing is known of which JetKVM it is, nor of which server a controller serves: what answers is taken, J5.)
    const OneSignIn::Where where{m_address, m_kvm.user, m_kvm.knownByCertificate, m_kvm.certificate, QString()};
    const DriverPaces paces = m_pace.drivers();
    // (A text of it for the driver's making, which takes a copy of its own: written over here once it has.)
    QString typed = m_password.view().toString();
    const WipedOnLeaving overTyped(typed);
    // (Typed just now: tried as for the first time, whatever was refused before.)
    OneSignIn::tryAfresh(m_kvm.kind, where, typed, paces);
    base::Result<std::unique_ptr<OneSignIn>> made =
        OneSignIn::of(m_kvm.kind, where, typed, paces, m_wall.twinCheckFor(QString()));
    if (!made) {
        finish(made.error());
        return;
    }
    m_device = std::move(made).value();
    m_device->identify([this, turn = m_turn](const base::Result<OneSignIn::Said>& who) {
        if (turn == m_turn) {
            identified(who);
        }
    });
}

void KvmAdding::identified(const base::Result<OneSignIn::Said>& who)
{
    if (!who) {
        finish(who.error());
        return;
    }
    if (m_kvm.name.isEmpty()) {
        // (A JetKVM calls itself nothing: it is named by where it is.)
        m_kvm.name = kvm::nameWhereNoneWasTyped(who.value().hostname, m_address);
    }
    // The number it gives for itself (a JetKVM's, a server's computer's), which tells it from another whatever its
    // address: a second card of a KVM that is on the wall is refused below (inTheWay, the web's same_device).
    m_kvm.number = who.value().number.value_or(QString());
    // A server's: the mark it is known by from now on, as its driver asked it (the server that answered, taken).
    m_kvm.machine = who.value().machine;
    // Where it says by its number that it is a KVM of the wall, and the person said to keep both: kept beside it (the
    // web's keep_both). A certificate's twin is not answered so (inTheWay goes by that first, whatever is kept); nor
    // is a server's (a server has one card on this PC's wall: the owner's word, M7 Q2).
    if (m_keepBoth && m_kvm.kind != "redfish"_L1) {
        if (const std::optional<OwnKvm> twin = LocalWall::twinOf(m_wall.kvms(), m_kvm);
            twin && !LocalWall::sameCertificate(*twin, m_kvm)) {
            m_kvm.keptBesideItsTwin = true;
        }
    }
    // The KVM was spoken to for a while, and the wall may have changed meanwhile: what was looked
    // at before that is looked at again.
    if (const std::optional<base::Error> refusal = kvm::inTheWay(m_wall.kvms(), m_kvm, m_shown)) {
        finish(*refusal);
        return;
    }
    Added added;
    added.name = m_kvm.name;
    added.model = who.value().model;
    added.version = who.value().version;
    added.overPlainHttp = m_address.scheme() == "http"_L1;
    added.certificate = m_kvm.knownByCertificate ? m_kvm.certificate : QByteArray();
    // The wall says aloud that it has changed while it adds, and whoever hears that may end this
    // there and then. So the driver is its own before that, the password is out of here, and
    // nothing of this is touched after it unless this is still there.
    letTheDriverGo();
    // (A text of it for the wall, which keeps what it keeps of it: written over here once the wall has it.)
    QString password = m_password.view().toString();
    const WipedOnLeaving overPassword(password);
    m_password = SecretText();
    const QPointer<KvmAdding> here(this);
    // (And from here on it is not given up: it is on the wall in a moment.)
    m_over = true;
    const base::Result<QString> id = m_wall.add(m_kvm, password);
    if (!here) {
        return;
    }
    if (!id) {
        finish(id.error());
        return;
    }
    added.id = id.value();
    finish(std::move(added));
}

void KvmAdding::giveUp()
{
    if (m_underWay && !m_over) {
        finish(givenUp());
    }
}

void KvmAdding::letTheDriverGo()
{
    // The driver that signed in was made for this, and goes with it: a turn of the loop later,
    // since it may be in the middle of handing over what it found.
    if (OneSignIn* over = m_device.release()) {
        over->deleteLater();
    }
}

void KvmAdding::finish(base::Result<Added> result)
{
    // Over: whatever of it is still to answer, answers nobody.
    m_over = true;
    ++m_turn;
    // Nothing of what was typed is kept here any longer, the password least of all.
    letTheDriverGo();
    m_password = SecretText();
    m_keepBoth = false;
    m_kvm = OwnKvm{};
    m_address.clear();
    m_ways = kvm::Ways{};
    m_waysOut = 0;
    m_shown.clear();
    // Always a turn of the loop later, whichever way it came about, and not at all once this
    // is gone.
    later([this, result = std::move(result)] {
        // Over before it is said, so that whoever is told may begin the next.
        m_underWay = false;
        const std::function<void(base::Result<Added>)> done = std::exchange(m_done, nullptr);
        done(result);
    });
}

} // namespace qkvm::core
