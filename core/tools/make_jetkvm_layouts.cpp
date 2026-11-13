// Makes, at build time, the C++ tables of the keyboard layouts a JetKVM's computer may use (core/src/jetkvm_layouts.cpp
// includes what this writes), from core/data/jetkvm_layouts.py: a copy, byte for byte, of the web's
// app/qkvm/layouts.py, which the web's tools/make_layouts.py made from Unicode CLDR's keyboard data, under Unicode's
// licence, which it carries (core/data/jetkvm_layouts-from.txt says from where). Nothing of it is changed: it is read
// as the web's own module has it (NAMES, and LAYOUTS: each character and the keys to press for it, modifier bits << 8 |
// USB usage), and written as C++, the notice with it.
//
//     qkvm-make-jetkvm-layouts <layouts.py> <out.inc>
//
// It stops (exit 1, and no file) on anything it does not read as that module is written.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Reader {
    const std::string& text;
    std::size_t at = 0;

    void blank()
    {
        while (at < text.size()) {
            const char sign = text[at];
            if (sign == ' ' || sign == '\n' || sign == '\r' || sign == '\t') {
                ++at;
            } else if (sign == '#') {
                while (at < text.size() && text[at] != '\n') {
                    ++at;
                }
            } else {
                break;
            }
        }
    }
    bool take(char sign)
    {
        blank();
        if (at < text.size() && text[at] == sign) {
            ++at;
            return true;
        }
        return false;
    }
    bool sees(char sign)
    {
        blank();
        return at < text.size() && text[at] == sign;
    }
    static std::optional<std::uint32_t> hexOf(const std::string& digits)
    {
        if (digits.empty()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (const char digit : digits) {
            value <<= 4;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        return value;
    }
    // A Python string literal, as repr() writes one: its code points.
    std::optional<std::vector<std::uint32_t>> literal()
    {
        blank();
        if (at >= text.size() || (text[at] != '\'' && text[at] != '"')) {
            return std::nullopt;
        }
        const char quote = text[at++];
        std::vector<std::uint32_t> points;
        while (at < text.size() && text[at] != quote) {
            const auto sign = static_cast<unsigned char>(text[at++]);
            if (sign >= 0x80) {
                return std::nullopt; // (the module is plain ASCII)
            }
            if (sign != '\\') {
                points.push_back(sign);
                continue;
            }
            if (at >= text.size()) {
                return std::nullopt;
            }
            const char escape = text[at++];
            const auto digits = [this](std::size_t count) -> std::optional<std::uint32_t> {
                if (at + count > text.size()) {
                    return std::nullopt;
                }
                const std::optional<std::uint32_t> value = hexOf(text.substr(at, count));
                at += count;
                return value;
            };
            std::optional<std::uint32_t> point;
            switch (escape) {
            case '\\':
                point = '\\';
                break;
            case '\'':
                point = '\'';
                break;
            case '"':
                point = '"';
                break;
            case 't':
                point = '\t';
                break;
            case 'n':
                point = '\n';
                break;
            case 'r':
                point = '\r';
                break;
            case 'x':
                point = digits(2);
                break;
            case 'u':
                point = digits(4);
                break;
            case 'U':
                point = digits(8);
                break;
            default:
                return std::nullopt;
            }
            if (!point || *point > 0x10FFFF) {
                return std::nullopt;
            }
            points.push_back(*point);
        }
        if (at >= text.size()) {
            return std::nullopt;
        }
        ++at;
        return points;
    }
    std::optional<std::uint32_t> number()
    {
        blank();
        if (text.compare(at, 2, "0x") != 0) {
            return std::nullopt;
        }
        at += 2;
        const std::size_t from = at;
        while (at < text.size() && std::isxdigit(static_cast<unsigned char>(text[at])) != 0) {
            ++at;
        }
        return hexOf(text.substr(from, at - from));
    }
};

std::string asciiOf(const std::vector<std::uint32_t>& points)
{
    std::string ascii;
    for (const std::uint32_t point : points) {
        if (point < 0x20 || point > 0x7E || point == '"' || point == '\\') {
            return {};
        }
        ascii.push_back(static_cast<char>(point));
    }
    return ascii;
}

struct Key {
    std::uint32_t character = 0;
    std::vector<std::uint32_t> steps;
};

int refuse(const char* why)
{
    std::fprintf(stderr, "qkvm-make-jetkvm-layouts: %s\n", why);
    return 1;
}

} // namespace

namespace {

int made(int argc, char** argv)
{
    if (argc != 3) {
        return refuse("usage: qkvm-make-jetkvm-layouts <layouts.py> <out.inc>");
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        return refuse("the layouts module cannot be read");
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Unicode's notice, as the module carries it, carried on.
    const std::size_t noticeFrom = text.find("    COPYRIGHT AND PERMISSION NOTICE");
    const std::size_t noticeTo = text.find("\"\"\"", noticeFrom);
    if (noticeFrom == std::string::npos || noticeTo == std::string::npos) {
        return refuse("Unicode's notice is not in the module");
    }
    const std::string notice = text.substr(noticeFrom, noticeTo - noticeFrom);

    Reader read{text};
    std::map<std::string, std::string> names;
    read.at = text.find("NAMES: dict[str, str] = {");
    if (read.at == std::string::npos) {
        return refuse("no NAMES");
    }
    read.at = text.find('{', read.at) + 1;
    while (!read.take('}')) {
        const auto name = read.literal();
        if (!name || !read.take(':')) {
            return refuse("NAMES is not as the module writes it");
        }
        const auto title = read.literal();
        if (!title || !read.take(',')) {
            return refuse("NAMES is not as the module writes it");
        }
        names[asciiOf(*name)] = asciiOf(*title);
    }
    read.at = text.find("LAYOUTS: dict[str, dict[str, tuple[int, ...]]] = {");
    if (read.at == std::string::npos) {
        return refuse("no LAYOUTS");
    }
    read.at = text.find("= {", read.at) + 3;
    std::map<std::string, std::vector<Key>> layouts;
    while (!read.take('}')) {
        const auto name = read.literal();
        if (!name || !read.take(':') || !read.take('{')) {
            return refuse("LAYOUTS is not as the module writes it");
        }
        std::vector<Key>& keys = layouts[asciiOf(*name)];
        while (!read.take('}')) {
            const auto character = read.literal();
            if (!character || character->size() != 1 || !read.take(':') || !read.take('(')) {
                return refuse("a layout's character is not as the module writes it");
            }
            Key key{character->front(), {}};
            while (!read.take(')')) {
                const auto step = read.number();
                if (!step || *step > 0xFFFF || !read.take(',')) {
                    return refuse("a layout's keys are not as the module writes them");
                }
                key.steps.push_back(*step);
            }
            if (key.steps.empty() || key.steps.size() > 2 || !read.take(',')) {
                return refuse("a character takes one key, or a dead key and one");
            }
            keys.push_back(std::move(key));
        }
        if (!read.take(',')) {
            return refuse("LAYOUTS is not as the module writes it");
        }
        std::ranges::sort(keys, {}, &Key::character);
    }
    if (names.empty() || layouts.size() != names.size()) {
        return refuse("NAMES and LAYOUTS do not name the same layouts");
    }

    std::ostringstream out;
    out << "// Made at build time by core/tools/make_jetkvm_layouts.cpp from core/data/jetkvm_layouts.py, a copy of "
           "the\n"
           "// web's app/qkvm/layouts.py (core/data/jetkvm_layouts-from.txt): do not change it. The data is Unicode "
           "CLDR's,\n"
           "// under this notice:\n//\n";
    std::istringstream lines(notice);
    for (std::string line; std::getline(lines, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        out << (line.empty() ? "//" : "// " + line) << '\n';
    }
    out << '\n';
    int number = 0;
    for (const auto& [name, keys] : layouts) {
        if (names.find(name) == names.end() || name.empty()) {
            return refuse("a layout NAMES does not name");
        }
        out << "constexpr LayoutKey layoutKeys" << number++ << "[] = {\n";
        for (const Key& key : keys) {
            out << "    {0x" << std::hex << key.character << ", {0x" << key.steps[0] << ", 0x"
                << (key.steps.size() > 1 ? key.steps[1] : 0) << "}, " << std::dec << key.steps.size() << "},\n";
        }
        out << "};\n";
    }
    out << "constexpr LayoutTable layoutTables[] = {\n";
    number = 0;
    for (const auto& [name, keys] : layouts) {
        out << "    {\"" << name << "\", \"" << names[name] << "\", layoutKeys" << number << ", std::size(layoutKeys"
            << number << ")},\n";
        ++number;
    }
    out << "};\n";
    std::ofstream written(argv[2], std::ios::binary | std::ios::trunc);
    written << out.str();
    if (!written) {
        return refuse("the tables could not be written");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return made(argc, argv);
    } catch (...) {
        return refuse("out of memory");
    }
}
