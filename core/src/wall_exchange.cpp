#include <qkvm/core/wall_exchange.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QHash>
#include <QHostAddress>
#include <QPointer>

#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("WallExchange", text);
}

// Every laying-out of the program has a number of its own, whichever WallExchange made it: one
// that was made by another is not this one's, whatever was counted there.
std::atomic<quint64> layingsOut{0};

constexpr qsizetype sha256Bytes = 32;

base::Error notLaidOut()
{
    return {"not_laid_out",
            words("This was not laid out, or what was laid out is not there any more. Nothing of it was taken: read "
                  "the file again.")
                .toStdString()};
}

// A name and an address in one line, the name as the one function for that makes it: for a
// device of a file, and for a KVM of the wall that one is held against. The address alone where
// there is no name to show.
QString calledAt(const QString& name, const QString& address)
{
    const QString shown = exchange::nameSetOff(name);
    return shown.isEmpty() ? address : words("%1 at %2").arg(shown, address);
}

// Whether a host, as an address has it written the one way, is a name with a part in its
// "xn--" form. (Numbers in brackets have no such part.)
bool isInXnForm(const QString& host)
{
    if (host.startsWith(u'[')) {
        return false;
    }
    const QList<QStringView> parts = QStringView(host).split(u'.');
    return std::ranges::any_of(parts, [](QStringView part) { return part.startsWith("xn--"_L1, Qt::CaseInsensitive); });
}

// Whether a host is numbers of the kind that no router passes on: 169.254.x.x, [fe80::...],
// and the first written as the second kind writes it. A name is asked by nothing: none is
// looked up.
bool isNotRouted(const QString& host)
{
    const QHostAddress numbers(host.startsWith(u'[') ? host.mid(1, host.size() - 2) : host);
    return !numbers.isNull() && numbers.isLinkLocal();
}

// Who is where already, for "never twice": the KVMs of the wall, or the devices of a file that
// can be taken, each with what it is called in a sentence.
class Taken {
public:
    void add(const OwnKvm& kvm, const QString& called)
    {
        const LocalWall::Place place = LocalWall::placeOf(kvm);
        m_atHost[place.host].append(One{place, called});
        // (A server's controller is no twin by a certificate, whichever of the two: LocalWall::sameCertificate.)
        if (kvm.knownByCertificate && kvm.kind != "redfish"_L1 && !m_byCertificate.contains(kvm.certificate)) {
            m_byCertificate.insert(kvm.certificate, called);
        }
        if (!kvm.number.isEmpty() && !LocalWall::isKeptBeside(kvm) && !m_byNumber.contains(kvm.number)) {
            m_byNumber.insert(kvm.number, called);
        }
    }

    // The one that is at this KVM's place, by the wall's own rule for that; nothing: none is.
    // (Each address is read once: a file of two thousand is held against a wall of as many.)
    std::optional<QString> at(const OwnKvm& kvm) const
    {
        const LocalWall::Place place = LocalWall::placeOf(kvm);
        const auto there = m_atHost.constFind(place.host);
        if (there == m_atHost.constEnd()) {
            return std::nullopt;
        }
        const auto same =
            std::ranges::find_if(*there, [&place](const One& one) { return LocalWall::atOnePlace(one.place, place); });
        return same == there->constEnd() ? std::nullopt : std::optional<QString>(same->called);
    }

    // The one that is known by the certificate this KVM is to be known by; or, where none is, the
    // one that gives the number this KVM gives: that one is this KVM, under another address
    // (LocalWall::twinOf). Nothing: none is. `byNumber`: by the number, which is said.
    struct Twin {
        QString called;
        bool byNumber = false;
    };
    std::optional<Twin> twinOf(const OwnKvm& kvm) const
    {
        if (kvm.knownByCertificate && kvm.kind != "redfish"_L1) {
            if (const auto twin = m_byCertificate.constFind(kvm.certificate); twin != m_byCertificate.constEnd()) {
                return Twin{*twin, false};
            }
        }
        if (kvm.number.isEmpty() || LocalWall::isKeptBeside(kvm)) {
            return std::nullopt;
        }
        const auto twin = m_byNumber.constFind(kvm.number);
        return twin == m_byNumber.constEnd() ? std::nullopt : std::optional<Twin>(Twin{*twin, true});
    }

    qsizetype size() const { return m_count; }
    void counted() { ++m_count; }

private:
    struct One {
        LocalWall::Place place;
        QString called;
    };
    QHash<QString, QList<One>> m_atHost;
    QHash<QByteArray, QString> m_byCertificate;
    QHash<QString, QString> m_byNumber;
    qsizetype m_count = 0;
};

// The KVMs of a wall, each as a sentence calls it.
Taken thoseOn(const QList<OwnKvm>& wall)
{
    Taken onTheWall;
    for (const OwnKvm& kvm : wall) {
        onTheWall.add(kvm, calledAt(kvm.name, kvm.address));
    }
    return onTheWall;
}

// Why something cannot be taken: its code, and what is said after "It cannot be taken: ". `saidSo`: of a
// "same_device", by the number it gives for itself, which is said (Offered::saysItIsTheSame).
struct InTheWay {
    const char* code = "";
    QString why;
    bool saidSo = false;
};

QString verdictOf(const QString& why)
{
    return words("It cannot be taken: %1").arg(why);
}

// What of a wall is in a KVM's way: it is at the place of one that is there, or is that one
// under another address. Nothing: nothing of the wall is. Asked when a file is laid out, and
// once more of each device that is taken, against the wall as it is then.
std::optional<InTheWay> onTheWallAlready(const Taken& onTheWall, const OwnKvm& kvm)
{
    if (const std::optional<QString> there = onTheWall.at(kvm)) {
        return InTheWay{"duplicate", words("it is on the wall already (%1).").arg(*there)};
    }
    if (const std::optional<Taken::Twin> twin = onTheWall.twinOf(kvm)) {
        return InTheWay{"same_device",
                        twin->byNumber
                            ? words("it says it is the same device as one that is on the wall already at another "
                                    "address (%1): the file names the number that one gives for itself.")
                                  .arg(twin->called)
                            : words("it is the same device as one that is on the wall already at another address "
                                    "(%1): the file names the certificate that one is known by.")
                                  .arg(twin->called),
                        twin->byNumber};
    }
    return std::nullopt;
}

InTheWay wallIsFull()
{
    return {"too_many", words("this PC's wall is full.")};
}

// One device of a file as it is laid out, and as it would be handed to the wall: where
// nothing of the device itself is in the way of that (its family, its address, what it is
// called). Whether it CAN be taken is `row.canBeTaken`, which is also what of a wall, and of
// the devices before it in the file, is in its way.
struct Judged {
    WallExchange::Offered row;
    std::optional<LocalWall::FromAFile> asTaken;
};

void cannot(WallExchange::Offered& row, const InTheWay& what)
{
    row.canBeTaken = false;
    row.whyNot = QLatin1StringView(what.code);
    row.saysItIsTheSame = what.saidSo;
    row.verdict = verdictOf(what.why);
}

// How a device is spoken to and what it will be known by here, as plain values in its row and
// in the KVM as it would be known: the one rule for what a KVM from a file is known by. Handed
// back: that, in a sentence, for whoever says it of a device that can be taken. (A certificate's
// SHA-256 is in the row, to be compared, and in no sentence.)
QString knownAs(const exchange::Device& device, WallExchange::Offered& row, OwnKvm& kvm)
{
    using KnownBy = WallExchange::KnownBy;
    if (!device.address.startsWith("https://"_L1)) {
        row.passwordInTheClear = true;
        row.knownBy = KnownBy::Nothing;
        return words("It is spoken to over plain HTTP: its password goes in the clear, and so does everything "
                     "that is seen and typed there. Nothing shows who answers at that address.");
    }
    const bool namesOne = device.certificate.size() == sha256Bytes && !device.certificatePem.isEmpty();
    // (One of the three words a file has for it, and nothing else of the file's text.)
    for (const QLatin1StringView word : {"pinned"_L1, "system"_L1, "insecure"_L1}) {
        if (device.tls == word) {
            row.fileSaysOfTls = word;
        }
    }
    if (device.tls == "pinned"_L1 && namesOne) {
        // By the certificate the file names, and by nothing else: what shows another hears
        // nothing, the first time as every time.
        row.fileNamesACertificate = true;
        row.knownBy = KnownBy::ItsCertificate;
        row.fingerprint = kvm::fingerprint(device.certificate);
        kvm.knownByCertificate = true;
        kvm.certificate = device.certificate;
        return words("The file names its certificate. It will be known by that certificate and by no other.");
    }
    row.knownBy = KnownBy::WindowsWord;
    if (device.tls == "system"_L1) {
        return words("The file says that Windows vouches for its certificate. It will be known by Windows' "
                     "word: by a certificate that Windows trusts for its name, whichever that is.");
    }
    // "Pinned" with no certificate named, or "not checked": to a QKVM server that is "known by
    // whatever it shows first". Not here: nothing a device from a file shows first is taken on
    // trust. It is known by Windows' word, and what is said of that is what is so, both ways:
    // a KVM's own certificate does not have that word, is refused, and is put before a person;
    // and one that Windows trusts is signed in to on that word, the file's password with it,
    // with nobody having looked at any certificate. (The sentence said "nothing is sent to it
    // before somebody has looked at that certificate" of both once, which is untrue of the
    // second.)
    row.notByWhatItShowsFirst = true;
    const QString said = device.tls == "insecure"_L1
                             ? words("The file says that its certificate is not checked.")
                             : words("The file names no certificate for it: a qKVM server would know it by the first "
                                     "certificate it shows.");
    return words("%1 This program does not take the first certificate a device from a file shows. It will be "
                 "known by Windows' word. A KVM's own certificate does not have that: then its tile says "
                 "that its certificate is not trusted, and nothing is sent to it before somebody has looked "
                 "at that certificate and trusted it. Where Windows does trust the certificate it shows, it "
                 "is asked what it is and signed in to on Windows' word, with nobody having looked at that "
                 "certificate.")
        .arg(said);
}

// Every device of a file laid out against a wall: in the file's order, the first of two that are
// the same being the one that can be taken.
// `kept`: the KVMs the settings keep that this build does not read (LocalWall::keptAside), which
// take room and places, and are not on the wall.
QList<Judged> judged(const QList<exchange::Device>& devices, const QList<OwnKvm>& wall, const QList<OwnKvm>& kept)
{
    const Taken onTheWall = thoseOn(wall);
    const Taken keptThere = thoseOn(kept);
    // The devices of this file that can be taken, so far: each called by its place in the file.
    Taken ofTheFile;
    QList<Judged> all;
    all.reserve(devices.size());
    for (const exchange::Device& device : devices) {
        Judged one;
        WallExchange::Offered& row = one.row;
        row.place = device.place.counted();
        row.hasPassword = !device.password.isEmpty();

        // Its address, as it will be kept and may be shown: by the one function that hands out an
        // address in plain signs only, with a name in its "xn--" form left in that form.
        const std::optional<exchange::ConsoleAddress> shown =
            exchange::consoleAddress(exchange::LinkAsWritten(device.address));
        const bool showable = shown && shown->toOpen() == device.address;
        // A device that has no name in its file is named by its address there (the reader's
        // rule, and a server's). Here it has none, as a KVM has none that was added by hand
        // without one: it is called by where it is wherever it is shown, and so by where it is
        // then, after it was moved. (Kept as its name, the address it had stayed its name.)
        const bool hasAName = device.name != exchange::nameOfADeviceWithoutOne(device.address);
        // What it is called and who signs in, by the rules of the box that adds a KVM: what
        // this PC's wall keeps, and reads back at the next start.
        const base::Result<QString> name = kvm::nameFrom(hasAName ? device.name : QString());
        const base::Result<QString> user = kvm::userFrom(device.user);
        const base::Result<QString> group = kvm::groupFrom(device.group);
        const base::Result<void> password = kvm::passwordFrom(device.password);

        OwnKvm kvm;
        kvm.kind = device.kind;
        // The number it gives for itself, as the file carries it (read only where it is one a driver gives).
        kvm.number = device.ident;
        // And that somebody said it is to stay beside its twin (the web's keep_both).
        kvm.keptBesideItsTwin = device.keepBoth;
        // And the machine its card was added for (a server's: which server answers is asked by it first).
        kvm.machine = device.machine;
        kvm.name = name ? name.value() : QString();
        kvm.address = device.address;
        kvm.user = user ? user.value() : QString();
        kvm.group = group ? group.value() : QString();
        // What is said of it beside its verdict: of where its address is, of any device that can
        // be shown; and of how it will be known and of its password, of one that can be taken.
        QStringList ofItsAddress;
        QString ofHowItIsKnown;
        if (showable) {
            row.address = shown->toOpen();
            row.nameAndAddress = calledAt(name ? name.value() : device.name, row.address);
            row.nameInXnForm = isInXnForm(shown->host());
            if (shown->isThisPcsOwn()) {
                row.where = WallExchange::Where::ThisPc;
                ofItsAddress.append(words("Its address is this PC itself: whatever listens there on this PC would be "
                                          "taken for this KVM."));
            } else if (isNotRouted(shown->host())) {
                row.where = WallExchange::Where::NotRouted;
                ofItsAddress.append(words("Its address is of the kind that no router passes on (169.254.x.x, fe80::): "
                                          "it means whatever has that number on the network this PC is plugged "
                                          "into."));
            }
            if (row.nameInXnForm) {
                ofItsAddress.append(words("Its address is a name in its xn-- form, which stands for letters beyond "
                                          "the twenty-six. Such a name can read as another name does: compare the "
                                          "address sign for sign."));
            }
            ofHowItIsKnown = knownAs(device, row, kvm);
        }
        // (A JetKVM out of its box runs without a password: that the file has none for one is no
        // problem of its own, and is said calmly.)
        const QString ofItsPassword =
            row.hasPassword ? words("The file has its password. It is kept with Windows, for this address and for "
                                    "what the KVM is known by.")
            : device.kind == "jetkvm"_L1
                ? words("The file has no password for it. It is taken without one: fine for a JetKVM that runs "
                        "without one, and where it asks for one, it is typed on this PC.")
                : words("The file has no password for it. It is taken without one, and its password is to be typed "
                        "on this PC.");

        // Whether it can be taken: the first thing that is in its way is the one that is said.
        // First what of the device itself is: then nothing is made of it at all.
        if (!OwnKvm::namesAFamily(device.kind)) {
            cannot(row, {"not_yet", words("it is of a family of devices that this program does not know.")});
        } else if (!showable) {
            cannot(row, {"invalid", words("its address is not one this program can show as it would be kept.")});
        } else if (!name || !user || !group || !password) {
            // (The rule's own sentence, which names the field and never what stood in it.)
            const base::Error& rule = !name    ? name.error()
                                      : !user  ? user.error()
                                      : !group ? group.error()
                                               : password.error();
            cannot(row, {"invalid", words("this PC's wall does not keep what the file has for it (%1).")
                                        .arg(QString::fromStdString(rule.message))});
        } else {
            // A KVM this PC's wall could hold, as the wall would be handed it. Then what else
            // is in its way: the wall as it is, and the devices that stand before it.
            LocalWall::FromAFile taken;
            taken.kvm = kvm;
            taken.password = device.password;
            taken.certificatePem = kvm.knownByCertificate ? device.certificatePem : QString();
            one.asTaken = std::move(taken);
            if (const std::optional<InTheWay> there = onTheWallAlready(onTheWall, kvm)) {
                cannot(row, *there);
            } else if (keptThere.at(kvm)) {
                cannot(row, {"duplicate", words("a KVM that another version of qKVM added is kept at its address. This "
                                                "version does not show it, and puts no other there.")});
            } else if (const std::optional<QString> first = ofTheFile.at(kvm)) {
                cannot(row, {"duplicate",
                             words("device %1 of this file is at the same address and stands before it.").arg(*first)});
            } else if (const std::optional<Taken::Twin> firstTwin = ofTheFile.twinOf(kvm)) {
                cannot(row, {"same_device",
                             firstTwin->byNumber ? words("it says it is the same device as device %1 of this file, "
                                                         "which stands before it: the file names one number for both.")
                                                       .arg(firstTwin->called)
                                                 : words("it is the same device as device %1 of this file, which "
                                                         "stands before it: the file names one certificate for both.")
                                                       .arg(firstTwin->called),
                             firstTwin->byNumber});
            } else if (wall.size() + kept.size() + ofTheFile.size() >= Remembered::mostKvms) {
                cannot(row, wallIsFull());
            } else {
                row.canBeTaken = true;
                // (As the family it is of, which is what it is taken as.)
                row.verdict = kvm.kind == "jetkvm"_L1    ? words("It can be taken, as a JetKVM.")
                              : kvm.kind == "redfish"_L1 ? words("It can be taken, as a server's management "
                                                                 "controller (BMC).")
                                                         : words("It can be taken, as a KVM that runs kvmd "
                                                                 "(PiKVM, GL.iNet Comet).");
                ofTheFile.add(kvm, QString::number(row.place));
                ofTheFile.counted();
            }
        }
        // One that cannot be taken says why (its verdict), and nothing of how it would be known
        // or of its password: neither comes to pass. And that nothing was asked at its address
        // yet is so of every device of every file (`nothingAskedYet`): it is said once, by
        // whoever shows the list, and not of each.
        row.says = ofItsAddress;
        if (row.canBeTaken) {
            row.says.append(ofHowItIsKnown);
            row.says.append(ofItsPassword);
        }
        all.append(std::move(one));
    }
    return all;
}

// How many bytes a sign takes in a text of the program's.
constexpr qsizetype bytesOfASign = static_cast<qsizetype>(sizeof(QChar));

// A password's signs, as a text of the program's has them (two bytes each), in memory that is
// written over when it goes. Sign for sign, and not as bytes that say a text: a password is
// taken as it stands in the file, whatever is in it, half a character too.
SecretBytes signsOf(const QString& password)
{
    return SecretBytes::copyOf(
        QByteArrayView(reinterpret_cast<const char*>(password.constData()), password.size() * bytesOfASign));
}

// The text those signs are, for the moment the wall is handed it: a copy of its own, which
// whoever made it writes over once the wall has it (written over, below).
QString textOf(const SecretBytes& signs)
{
    QString text(signs.size() / bytesOfASign, Qt::Uninitialized);
    if (!text.isEmpty()) {
        std::memcpy(text.data(), signs.data(), static_cast<std::size_t>(text.size() * bytesOfASign));
    }
    return text;
}

// Writes over a text that nothing else holds. (Qt's texts are shared by whoever copied them,
// and one that is shared is another's too: this is for one that was made here and handed to
// nobody who kept it.)
void writeOver(QString& text)
{
    if (!text.isEmpty() && text.isDetached()) {
        wipe(text.data(), static_cast<std::size_t>(text.size() * bytesOfASign));
    }
    text.clear();
}

// The version of the program, as a file says which program wrote it: where it is a short word
// of plain signs, and nothing otherwise.
QString programAsAFileSaysIt()
{
    const QString version = QCoreApplication::applicationVersion();
    const bool plain = std::ranges::all_of(version, [](QChar sign) { return sign > u' ' && sign < QChar(0x7F); });
    return plain && version.size() <= exchange::longestProgram ? version : QString();
}

// A file as the writer wrote it, with the lines of this piece's own before the writer's: in the
// writer's own shape, so that the text is handed on only inside what hands it out for the word
// that the lines were looked at.
base::Result<exchange::Written> withLinesBeside(const base::Result<exchange::Written>& written,
                                                const QStringList& beside)
{
    if (!written) {
        return written.error();
    }
    const exchange::Written& file = written.value();
    return exchange::Written(file.file(exchange::NotInIt::wasLookedAt), beside + file.notInIt());
}

} // namespace

WallExchange::WallExchange(LocalWall& wall, QObject* parent) : WallExchange(wall, exchange::Sealing(), parent) {}

WallExchange::WallExchange(LocalWall& wall, exchange::Sealing sealing, QObject* parent)
    : WallExchange(wall, std::move(sealing), keptForMsAsItIs, parent)
{}

WallExchange::WallExchange(LocalWall& wall, exchange::Sealing sealing, int keptForMs, QObject* parent)
    : QObject(parent), m_wall(wall), m_keptForMs(keptForMs), m_work(std::move(sealing))
{
    m_forgetSoon.setSingleShot(true);
    // Kept for so long, and no longer: a box that was left open is one nobody is choosing from.
    // And that box is told, once it is forgotten: what it shows can be taken no more. (Last:
    // whoever hears it may end this there and then.)
    connect(&m_forgetSoon, &QTimer::timeout, this, [this] {
        forget();
        emit keptNoLonger();
    });
}

// (What is kept goes with it, each password written over: Takeable's own doing.)
WallExchange::~WallExchange() = default;

void WallExchange::forgetWhatWasLaidOut()
{
    m_forgetSoon.stop();
    m_laidOut.reset();
}

void WallExchange::forget()
{
    forgetWhatWasLaidOut();
    // And a file that is still being read is given up: what its reading comes to is not laid out.
    ++m_readings;
}

qsizetype WallExchange::passwordsHeld() const
{
    if (!m_laidOut) {
        return 0;
    }
    return std::ranges::count_if(m_laidOut->takeable, [](const Takeable& one) { return one.password.size() > 0; });
}

WallExchange::LaidOut WallExchange::laidOutFrom(const exchange::Content& content)
{
    Kept kept;
    kept.number = ++layingsOut;
    LaidOut laidOut;
    laidOut.m_number = kept.number;
    // (The kept KVMs of the settings, which this build does not read, take room and places too.)
    const QList<Judged> all =
        judged(content.devices.value_or(QList<exchange::Device>()), m_wall.kvms(), m_wall.keptAside());
    for (const Judged& one : all) {
        laidOut.m_devices.append(one.row);
        // Kept: what can be taken, as the wall would be handed it, and nothing else of the
        // file. Of a device that cannot be taken, why, and not its password: what was laid out
        // as not to be taken is taken by nothing that happens to the wall afterwards.
        if (one.row.canBeTaken && one.asTaken) {
            Takeable takeable;
            takeable.place = one.row.place;
            takeable.kvm = one.asTaken->kvm;
            takeable.password = signsOf(one.asTaken->password);
            takeable.certificatePem = one.asTaken->certificatePem;
            kept.takeable.push_back(std::move(takeable));
        } else {
            kept.refused.insert(one.row.place, Refused{one.row.whyNot, one.row.verdict});
        }
    }
    // Beside the list: every sentence of what the reader left out, as it is; and what the file
    // holds that is not taken from a file yet.
    laidOut.m_beside = content.leftOut;
    if (!content.devices) {
        laidOut.m_beside.append(words("This file holds no devices."));
    }
    QSet<QString> actionNames;
    qsizetype place = firstActionPlace;
    for (const exchange::Action& action : content.actions.value_or(QList<exchange::Action>())) {
        Offered row;
        row.kind = Offered::Kind::Action;
        row.place = place++;
        row.nameAndAddress = words("Saved action: %1").arg(exchange::nameSetOff(action.name));
        const QString name = OwnActions::nameForImport(action.name);
        const auto validName = action::nameFrom(action.name);
        const bool validBody =
            action.types ? (!action.text.isEmpty() && action::isTypedAsOne(action.text)) : action::areKeys(action.keys);
        if (!validName || !validBody) {
            cannot(row, {"invalid", words("its name, keys or text cannot be kept on this PC.")});
        } else if (m_wall.actions().hasName(action.name) || actionNames.contains(name)) {
            cannot(row, {"duplicate", words("a saved action of this name is already here or earlier in the file.")});
        } else if (m_wall.actions().all().size() + static_cast<qsizetype>(kept.actions.size()) >=
                   exchange::mostActions) {
            cannot(row, {"too_many", words("the saved-action list is full.")});
        } else {
            row.canBeTaken = true;
            row.hasText = action.types && !action.text.isEmpty();
            row.verdict = action.types ? words("Types saved text. Open Details and Show text to review it.")
                                       : words("Presses keys: %1").arg(action.keys);
            row.says.append(action.danger ? words("Asks before running.") : words("Does not ask before running."));
            if (action.types) {
                row.says.append(action.enter ? words("Presses Enter after the text.")
                                             : words("Does not press Enter after the text."));
            }
            Kept::Saved saved;
            saved.place = row.place;
            saved.action = action;
            saved.text = signsOf(action.text);
            saved.action.text.clear();
            kept.actions.push_back(std::move(saved));
            actionNames.insert(name);
        }
        if (!row.canBeTaken) {
            kept.refused.insert(row.place, Refused{row.whyNot, row.verdict});
        }
        laidOut.m_other.append(row);
    }
    if (content.wall) {
        Offered row;
        row.kind = Offered::Kind::Settings;
        row.place = settingsPlace;
        row.nameAndAddress = words("Wall settings");
        row.initiallyTicked = false;
        row.canBeTaken =
            content.wall->pictureEverySeconds.has_value() || content.wall->asksForNewerVersions.has_value();
        row.verdict = words("Replaces the selected shared settings on this PC. Left unchecked to keep your settings.");
        if (content.wall->pictureEverySeconds) {
            row.says.append(words("Picture interval: %1 seconds.").arg(*content.wall->pictureEverySeconds));
        }
        if (content.wall->asksForNewerVersions) {
            row.says.append(*content.wall->asksForNewerVersions
                                ? words("Automatic update checks: on. Installation remains your choice.")
                                : words("Automatic update checks: off."));
        }
        if (row.canBeTaken) {
            kept.settings = content.wall;
        } else {
            row.verdict = words("This section contains no supported settings.");
            kept.refused.insert(row.place, Refused{u"invalid"_s, row.verdict});
        }
        laidOut.m_other.append(row);
    }
    if (content.alerts) {
        laidOut.m_beside.append(words("Server alert settings are not supported on this PC. They stay in the file; "
                                      "Windows notifications are unchanged."));
    }
    if (!content.sealed && content.hasSecrets()) {
        laidOut.m_beside.append(words("This file contains secrets without a passphrase. Keep it somewhere private."));
    }
    m_laidOut = std::move(kept);
    m_forgetSoon.start(std::max(0, m_keptForMs));
    return laidOut;
}

QString WallExchange::actionText(quint64 laidOut, qsizetype place) const
{
    if (!m_laidOut || m_laidOut->number != laidOut) {
        return {};
    }
    const auto found =
        std::ranges::find_if(m_laidOut->actions, [place](const Kept::Saved& one) { return one.place == place; });
    return found == m_laidOut->actions.end() ? QString() : textOf(found->text);
}

void WallExchange::layOut(const QByteArray& file, const QString& passphrase, QObject* asker,
                          std::function<void(base::Result<LaidOut>)> done)
{
    // What was laid out before this is taken from no more, whatever comes of this file. (A
    // file that is still being read is not given up by this: the one that is asked for now is
    // refused as "busy", and the one before it is laid out when it was read.)
    forgetWhatWasLaidOut();
    m_work.read(file, passphrase, asker,
                [this, reading = m_readings, done = std::move(done)](const base::Result<exchange::Content>& read) {
                    // Forgotten while it was read (the box that asked has gone): nothing of this
                    // file is kept, for ten minutes and under a number that could be taken from,
                    // behind the back of whoever said so.
                    if (reading != m_readings) {
                        done(base::Error{
                            "given_up",
                            words("The file was given up before it was read. Nothing of it is kept.").toStdString()});
                        return;
                    }
                    if (!read) {
                        done(read.error());
                        return;
                    }
                    done(laidOutFrom(read.value()));
                });
}

QList<WallExchange::Took> WallExchange::take(quint64 laidOut, const QList<qsizetype>& places)
{
    // Each place once, and in the file's order, however they were named.
    QList<qsizetype> named = places;
    std::ranges::sort(named);
    named.erase(std::unique(named.begin(), named.end()), named.end());
    QList<Took> took;
    took.reserve(named.size());
    if (!m_laidOut || m_laidOut->number != laidOut) {
        // Not what this laid out last: nothing of it is taken.
        for (const qsizetype place : std::as_const(named)) {
            took.append(Took{place, notLaidOut()});
        }
        return took;
    }
    if (named.isEmpty()) {
        return took;
    }
    // The laying-out is over with this taking: what was kept of the file is this taking's own
    // from here on, and goes when it has ended, each password written over. (Before the wall is
    // handed anything: it says aloud that it has changed while it takes, and whoever hears that
    // may end this there and then, so nothing of this is touched after it.)
    const Kept kept = std::move(*m_laidOut);
    forget();
    QHash<qsizetype, qsizetype> takeableAt;
    for (std::size_t index = 0; index < kept.takeable.size(); ++index) {
        takeableAt.insert(kept.takeable[index].place, static_cast<qsizetype>(index));
    }
    // Each device once more against the wall as it is now: it may have changed since they were
    // laid out, and what came onto it meanwhile is not taken a second time. (Among themselves
    // they are no two the same: of two such, only the first was laid out as to be taken.)
    LocalWall& wall = m_wall;
    const auto onTheWall = thoseOn(wall.kvms());
    qsizetype room = Remembered::mostKvms - wall.kvms().size() - wall.keptAside().size();
    QList<LocalWall::FromAFile> those;
    QList<qsizetype> whose; // where in `took` each of them stands
    for (const qsizetype place : std::as_const(named)) {
        if (place >= firstActionPlace) {
            continue;
        }
        if (const auto refused = kept.refused.constFind(place); refused != kept.refused.constEnd()) {
            // Laid out as not to be taken: for the reason it was shown with, whatever the wall
            // is by now.
            took.append(Took{place, base::Error{refused->code.toStdString(), refused->verdict.toStdString()}});
            continue;
        }
        const auto found = takeableAt.constFind(place);
        if (found == takeableAt.constEnd()) {
            // Named, and never laid out: nothing of it is taken.
            took.append(Took{place, notLaidOut()});
            continue;
        }
        const Takeable& is = kept.takeable[static_cast<std::size_t>(*found)];
        const std::optional<InTheWay> inTheWay =
            room <= 0 ? std::optional<InTheWay>(wallIsFull()) : onTheWallAlready(onTheWall, is.kvm);
        if (inTheWay) {
            took.append(Took{place, base::Error{inTheWay->code, verdictOf(inTheWay->why).toStdString()}});
            continue;
        }
        --room;
        LocalWall::FromAFile one;
        one.kvm = is.kvm;
        one.password = textOf(is.password);
        one.certificatePem = is.certificatePem;
        whose.append(took.size());
        those.append(std::move(one));
        took.append(Took{place, notLaidOut()});
    }
    const QPointer<LocalWall> remaining(&wall);
    const QList<base::Result<QString>> came =
        those.isEmpty() ? QList<base::Result<QString>>() : wall.takeFromAFile(those);
    // The texts that were made of the passwords for the wall's sake: written over, each being
    // the one copy of it that was made here.
    for (LocalWall::FromAFile& one : those) {
        writeOver(one.password);
    }
    for (qsizetype index = 0; index < whose.size() && index < came.size(); ++index) {
        took[whose.at(index)].came = came.at(index);
    }
    for (const qsizetype place : std::as_const(named)) {
        if (place < firstActionPlace) {
            continue;
        }
        base::Result<QString> result = notLaidOut();
        if (const auto refused = kept.refused.constFind(place); refused != kept.refused.constEnd()) {
            result = base::Error{refused->code.toStdString(), refused->verdict.toStdString()};
        } else if (remaining && place == settingsPlace && kept.settings) {
            const auto saved = remaining->takeSettings(*kept.settings);
            result = saved ? base::Result<QString>(u"settings"_s) : base::Result<QString>(saved.error());
        } else if (remaining) {
            const auto found =
                std::ranges::find_if(kept.actions, [place](const Kept::Saved& one) { return one.place == place; });
            if (found != kept.actions.end()) {
                exchange::Action action = found->action;
                action.text = textOf(found->text);
                const auto saved = remaining->actions().takeFromAFile(action);
                writeOver(action.text);
                result = saved ? base::Result<QString>(saved.value().id) : base::Result<QString>(saved.error());
            }
        }
        took.append({place, result});
    }
    return took;
}

void WallExchange::write(const QString& passphrase, QObject* asker,
                         std::function<void(base::Result<exchange::Written>)> done)
{
    writeParts(Parts{}, passphrase, asker, std::move(done));
}

void WallExchange::writeParts(Parts parts, const QString& passphrase, QObject* asker,
                              std::function<void(base::Result<exchange::Written>)> done)
{
    if (asker == nullptr) {
        return;
    }
    const bool sealed = !passphrase.isEmpty();
    if (const std::optional<base::Error> refused = sealed ? exchange::passphraseRefused(passphrase) : std::nullopt) {
        // Told as everything is: a turn of the loop later, and to nobody once this or whoever
        // asked is gone.
        QMetaObject::invokeMethod(
            this,
            [whom = QPointer<QObject>(asker), done = std::move(done), why = *refused] {
                if (whom && done) {
                    done(why);
                }
            },
            Qt::QueuedConnection);
        return;
    }
    // The wall as a file holds one: with each password from the store for a sealed file, and
    // with none, and none read, for a plain one.
    LocalWall::AsAFile wall = parts.devices ? m_wall.asAFile(sealed) : LocalWall::AsAFile{};
    wall.content.program = programAsAFileSaysIt();
    // The lines of this piece's own beside the file, before the writer's.
    QStringList beside{words("Server alert settings and this PC's accounts, app password, window layout and "
                             "preferences are not in this file.")};
    if (parts.actions) {
        QList<exchange::Action> actions;
        if (sealed) {
            const OwnActions::WithTheirTexts saved = m_wall.actions().textsHandedOutForASealedFile();
            actions = saved.actions;
            // The writer refuses an action whose text could not be read; no incomplete backup.
        } else {
            for (const OwnAction& action : m_wall.actions().all()) {
                if (!action.types) {
                    actions.append({action.name, false, action.keys, {}, false, action.danger});
                }
            }
            if (std::ranges::any_of(m_wall.actions().all(), [](const OwnAction& action) { return action.types; })) {
                beside.append(words("Plain files omit device passwords and every saved text action. Use a passphrase "
                                    "to include them."));
            }
        }
        wall.content.actions = actions;
    }
    if (parts.wall) {
        wall.content.wall = m_wall.portableSettings();
    }
    const QList<exchange::Device> devices = wall.content.devices.value_or(QList<exchange::Device>());
    for (const qsizetype index : std::as_const(wall.withoutPassword)) {
        beside.append(words("%1: its password is not in this file. Windows did not hand it out, or none is kept for "
                            "this device as it is.")
                          .arg(exchange::deviceOfTheWall(index, devices.value(index).name)));
    }
    const bool hasCertificates =
        std::ranges::any_of(devices, [](const exchange::Device& device) { return !device.certificatePem.isEmpty(); });
    const exchange::Content content = wall.content;
    m_work.write(content, passphrase, asker,
                 [this, whom = QPointer<QObject>(asker), done = std::move(done), beside, content, passphrase,
                  hasCertificates](const base::Result<exchange::Written>& written) {
                     const bool tooLarge = !written && written.error().code == exchange::tooLarge;
                     if (!tooLarge || !hasCertificates || whom == nullptr) {
                         done(withLinesBeside(written, beside));
                         return;
                     }
                     // More than a file of QKVM's holds, and the certificates of its KVMs are in
                     // it: written once more without the certificates themselves, which fits
                     // unless the wall's names and passwords are long and of signs beyond ASCII
                     // throughout (wall_exchange.h says why, and why all of them; where it does
                     // not fit, the second writing is refused as the first was, and that is what
                     // is told). Every KVM is in the
                     // file; one that is known by a certificate as "pinned" with none named,
                     // and the writer's own line says so of each. This line stands first.
                     exchange::Content without = content;
                     QList<exchange::Device> lighter = without.devices.value_or(QList<exchange::Device>());
                     for (exchange::Device& device : lighter) {
                         device.certificatePem.clear();
                     }
                     without.devices = lighter;
                     QStringList besideWithout{
                         words("With the certificates of its KVMs in it, this wall is larger than a file of qKVM's "
                               "may be. The file was written without them: every KVM that is known by a certificate "
                               "is in it as pinned, with no certificate named. Whoever reads the file has those "
                               "certificates to look at anew, and a qKVM server takes the first one each shows.")};
                     besideWithout += beside;
                     m_work.write(without, passphrase, whom.data(),
                                  [done, besideWithout](const base::Result<exchange::Written>& again) {
                                      done(withLinesBeside(again, besideWithout));
                                  });
                 });
}

} // namespace qkvm::core
