#pragma once

#include <optional>

#include <QChar>
#include <QString>
#include <QStringView>

namespace qkvm::ui {

// What a machine is known by on a wall that shows the machines of several places at once: the id
// of the source it comes from (this PC, or a QKVM server), a separator, and the machine's own id
// at that source, "<source>/<machine>". A source's id is never empty and never holds the
// separator; a machine's id may hold anything, the separator too, or be empty. So a key is taken
// apart at its first separator only, and no two pairs make one key.
//
// A key is made here and looked up where the wall keeps what each of its keys reaches
// (WallMachines::reach): a key that is not there reaches nothing, however it is made. Taking a key
// apart says which pair would have made it, and is never how a machine is found.
inline constexpr QChar wallKeySeparator = u'/';

// The two a key is made of.
struct WallKeyParts {
    QString source;
    QString machine;

    bool operator==(const WallKeyParts&) const = default;
};

// Whether this is a source's id as a key takes one: not empty, and without the separator.
inline bool isSourceId(QStringView id)
{
    return !id.isEmpty() && !id.contains(wallKeySeparator);
}

// The key of this source's machine; nothing where the source's id is none.
inline std::optional<QString> wallKey(const QString& source, const QString& machine)
{
    if (!isSourceId(source)) {
        return std::nullopt;
    }
    return source + wallKeySeparator + machine;
}

// The source's id and the machine's id this key was made of; nothing for what is no key (no
// separator, or nothing before the first one).
inline std::optional<WallKeyParts> wallKeyParts(const QString& key)
{
    const qsizetype at = key.indexOf(wallKeySeparator);
    if (at <= 0) {
        return std::nullopt;
    }
    return WallKeyParts{key.first(at), key.sliced(at + 1)};
}

} // namespace qkvm::ui
