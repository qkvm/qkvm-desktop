#pragma once

#include <optional>
#include <vector>

#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <qkvm/base/result.h>

// A JSON text read by the book, for a file that anybody may have made (a wall as a file:
// exchange.h). What a QKVM server takes of such a file it reads with its language's own reader,
// and this one takes nothing that one refuses: a text that Qt's reader lets through and the
// server's does not (an escape that is none, a number written half, a sign below the blank
// inside a text) would be a file read here and refused there.
//
// Stricter than the server's reader where that one is lenient or has no limit of its own:
// * Nothing is nested deeper than `deepestNesting`, and a text holds no more than `mostValues`
//   values: what reading costs is bounded by that, in memory and on the stack.
// * A number is at most `longestNumber` signs long and one a PC's own numbers hold: no
//   "NaN", no "Infinity", and none so large or so small that it is neither.
// * Half a character (a surrogate by itself, spelled out with a backslash) is refused here and
//   now. The server reads it, and then refuses the whole file for it.
// As the server's: a name that stands twice in one object means what was written last.
namespace qkvm::core::json {

inline constexpr int deepestNesting = 32;
inline constexpr qsizetype mostValues = 250'000;
inline constexpr qsizetype longestNumber = 64;

struct Member;

struct Value {
    enum class Kind { Null, Flag, Number, Text, List, Object };

    Kind kind = Kind::Null;
    bool flag = false;
    double number = 0;
    // The number was written whole: digits only, with no point and no exponent. (To the
    // server's reader such a one is a number of another kind than "1.0", and a file's version
    // has to be one.)
    bool whole = false;
    QString text;
    std::vector<Value> list;
    std::vector<Member> members; // of an object, in the order they were written

    bool isText() const noexcept { return kind == Kind::Text; }
    bool isObject() const noexcept { return kind == Kind::Object; }
    bool isList() const noexcept { return kind == Kind::List; }
    bool isFlag() const noexcept { return kind == Kind::Flag; }
    // A number, of either kind. (A yes or a no is none, here as there.)
    bool isNumber() const noexcept { return kind == Kind::Number; }
    // What an object has under a name: the last that was written under it, or nothing.
    const Value* find(QStringView name) const noexcept;
};

struct Member {
    QString name;
    Value value;
};

// Why a text was not read. For the tests, which say which rule refused; whoever reads a file
// says of all of them the same thing.
inline constexpr const char* notUtf8 = "json:not_utf8";
inline constexpr const char* notJson = "json:not_json";
inline constexpr const char* halfACharacter = "json:half_a_character";
inline constexpr const char* tooDeep = "json:too_deep";
inline constexpr const char* tooMany = "json:too_many";
inline constexpr const char* notANumber = "json:not_a_number";

// The value a text is: its bytes are UTF-8, by the book as well (no sign written longer than
// it needs to be, no surrogate, nothing above the last sign there is). What is refused costs no
// more than what is read. From any thread.
base::Result<Value> parse(QByteArrayView text);

// How many signs (code points) a text has, which is how a QKVM server counts a file's length;
// nothing where its bytes are not UTF-8.
std::optional<qsizetype> signsIn(QByteArrayView text) noexcept;

} // namespace qkvm::core::json
