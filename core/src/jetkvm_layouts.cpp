#include <qkvm/core/jetkvm_layouts.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

#include <QJsonValue>
#include <QRegularExpression>

#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core::jetkvm {

namespace {

// One character of a layout: the keys to press for it one after the other, each as modifier bits << 8 | USB usage
// (two where it takes a dead key first).
struct LayoutKey {
    char32_t character;
    std::uint16_t steps[2];
    std::uint8_t count;
};
struct LayoutTable {
    const char* name;
    const char* title;
    const LayoutKey* keys;
    std::size_t size;
};

// (Made at build time: core/tools/make_jetkvm_layouts.cpp.)
#include "jetkvm_layouts.inc"

const LayoutTable* tableOf(const QString& layout)
{
    const auto found = std::ranges::find_if(
        layoutTables, [&layout](const LayoutTable& table) { return layout == QLatin1StringView(table.name); });
    return found == std::end(layoutTables) ? nullptr : &*found;
}

base::Error invalid(const QString& why)
{
    return {"invalid", why.toStdString()};
}

} // namespace

QString layoutOf(const json::Value& said)
{
    if (said.isText() && said.text.isEmpty()) {
        return QString::fromLatin1(usLayout);
    }
    static const QRegularExpression name(uR"(\A[A-Za-z]{2,3}(?:[-_][A-Za-z0-9]{1,8}){0,3}\z)"_s);
    if (!said.isText() || !name.match(said.text).hasMatch()) {
        QString shown;
        if (said.isText()) {
            shown = kvmd::plain(QJsonValue(said.text), 20);
        } else if (said.isNumber()) {
            shown = kvmd::plain(QJsonValue(said.number), 20);
        }
        return u"("_s + (shown.isEmpty() ? u"none"_s : shown) + u")"_s;
    }
    QString layout = said.text;
    return layout.replace(u'_', u'-');
}

base::Result<std::vector<typed::Report>> textReports(const QString& text, const QString& layout)
{
    // Counted as the web counts it, in characters, a "\r\n" made one Enter first.
    QString plain = text;
    plain.replace(u"\r\n"_s, u"\n"_s);
    const QList<uint> characters = QStringView(plain).toUcs4();
    if (text.isEmpty() || QStringView(text).toUcs4().size() > typed::mostCharacters) {
        return invalid(u"Text must be 1 to %1 characters"_s.arg(typed::mostCharacters));
    }
    std::vector<typed::Report> reports;
    if (layout == QLatin1StringView(usLayout)) {
        for (qsizetype place = 0; place < characters.size(); ++place) {
            const char32_t one = characters[place];
            const base::Result<std::vector<typed::Report>> typed = typed::text(QString::fromUcs4(&one, 1));
            if (!typed || typed.value().size() != 1) {
                return invalid(u"Nothing was typed: character %1 of the text cannot be typed on the US keyboard layout "
                               "this JetKVM's computer is set to"_s.arg(place + 1));
            }
            reports.push_back(typed.value().front());
        }
        return reports;
    }
    const LayoutTable* table = tableOf(layout);
    if (table == nullptr) {
        return invalid(u"Nothing was typed: this JetKVM's computer is set to the keyboard layout %1, which qKVM has no "
                       "table for"_s.arg(layout));
    }
    const LayoutKey* const first = table->keys;
    const LayoutKey* const last = table->keys + table->size;
    for (qsizetype place = 0; place < characters.size(); ++place) {
        const char32_t one = characters[place];
        const LayoutKey* found = std::lower_bound(
            first, last, one, [](const LayoutKey& key, char32_t character) { return key.character < character; });
        if (found == last || found->character != one) {
            return invalid(u"Nothing was typed: character %1 of the text cannot be typed on the %2 keyboard layout "
                           "this JetKVM's computer is set to"_s.arg(place + 1)
                               .arg(QString::fromLatin1(table->title)));
        }
        for (std::uint8_t step = 0; step < found->count; ++step) {
            typed::Report report;
            report.modifiers = static_cast<std::uint8_t>(found->steps[step] >> 8);
            report.keys.push_back(static_cast<std::uint8_t>(found->steps[step] & 0xFF));
            reports.push_back(std::move(report));
        }
    }
    return reports;
}

QStringList layouts()
{
    QStringList names;
    for (const LayoutTable& table : layoutTables) {
        names.append(QString::fromLatin1(table.name));
    }
    return names;
}

QString titleOf(const QString& layout)
{
    const LayoutTable* table = tableOf(layout);
    return table == nullptr ? QString() : QString::fromLatin1(table->title);
}

} // namespace qkvm::core::jetkvm
