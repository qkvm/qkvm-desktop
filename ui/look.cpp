#include "look.h"

#include <algorithm>
#include <array>
#include <utility>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QStyleHints>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// In the order the button goes through them.
constexpr std::array themes{"dark", "light", "system"};

QString words(const char* text)
{
    return QCoreApplication::translate("Look", text);
}

QString nameOf(const QString& theme)
{
    return theme == "light"_L1    ? words("Light theme")
           : theme == "system"_L1 ? words("System theme")
                                  : words("Dark theme");
}

QString after(const QString& theme)
{
    for (std::size_t at = 0; at < themes.size(); ++at) {
        if (theme == QLatin1StringView(themes.at(at))) {
            return QString::fromLatin1(themes.at((at + 1) % themes.size()));
        }
    }
    return QString::fromLatin1(themes.front());
}

} // namespace

Look::Look(core::Settings& settings, std::function<void(Qt::ColorScheme)> tell, QObject* parent)
    : QObject(parent), m_settings(settings), m_tell(std::move(tell))
{
    // What is remembered is one of the three, or nothing (see core::Settings): nothing is dark.
    m_theme =
        m_settings.remembered().theme.isEmpty() ? QString::fromLatin1(themes.front()) : m_settings.remembered().theme;
    apply();
}

void Look::tellQt(Qt::ColorScheme scheme)
{
    QGuiApplication::styleHints()->setColorScheme(scheme);
}

void Look::apply() const
{
    m_tell(m_theme == "light"_L1    ? Qt::ColorScheme::Light
           : m_theme == "system"_L1 ? Qt::ColorScheme::Unknown
                                    : Qt::ColorScheme::Dark);
}

QString Look::themeSymbol() const
{
    return m_theme == "light"_L1 ? u"sun"_s : m_theme == "system"_L1 ? u"system"_s : u"moon"_s;
}

QString Look::themeHint() const
{
    return words("%1 (click for %2)").arg(nameOf(m_theme), nameOf(after(m_theme)).toLower());
}

void Look::nextTheme()
{
    choose(after(m_theme));
}

void Look::choose(const QString& theme)
{
    const bool isOne = std::ranges::any_of(themes, [&](const char* one) { return theme == QLatin1StringView(one); });
    if (!isOne || theme == m_theme) {
        return;
    }
    m_theme = theme;
    apply();
    emit themeChanged();
    core::Remembered remembered = m_settings.remembered();
    remembered.theme = m_theme;
    // A PC whose settings cannot be written has the look that was chosen all the same, until the
    // program ends.
    static_cast<void>(m_settings.remember(remembered));
}

} // namespace qkvm::ui
