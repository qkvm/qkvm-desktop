#include "settings_view.h"

#include <QCoreApplication>
#include <QStringList>
#include <QVariantMap>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Settings", text);
}

// The choices of adapter, in the order they are shown in.
const QStringList cards{u"most-powerful"_s, u"power-saving"_s, u"windows"_s};

engine::AdapterChoice adapterChoiceOf(const QString& word)
{
    return word == "power-saving"_L1 ? engine::AdapterChoice::PowerSaving
           : word == "windows"_L1    ? engine::AdapterChoice::AsWindowsChooses
                                     : engine::AdapterChoice::MostPowerful;
}

} // namespace

SettingsView::SettingsView(core::AutoStart& start, core::Settings& settings, QObject* parent)
    : QObject(parent), m_start(start), m_settings(settings), m_startsWithWindows(start.isOn()),
      m_cardAtStart(settings.remembered().graphicsCard), m_section(settings.remembered().settingsSection)
{}

SettingsView::~SettingsView()
{
    // (Whoever made this keeps the settings for longer: main.cpp has them made first.)
    closed();
}

void SettingsView::choose(bool core::Remembered::* which, bool chosen)
{
    core::Remembered remembered = m_settings.remembered();
    if (remembered.*which == chosen) {
        return;
    }
    remembered.*which = chosen;
    // A PC whose settings cannot be written has what was chosen all the same, until the program ends.
    static_cast<void>(m_settings.remember(remembered));
    emit changed();
}

void SettingsView::setStaysInTray(bool stays)
{
    choose(&core::Remembered::staysInTray, stays);
}

void SettingsView::setTellsNews(bool tells)
{
    choose(&core::Remembered::tellsNews, tells);
}

void SettingsView::setReleaseKey(const QString& key)
{
    if (!core::Remembered::namesAReleaseKey(key) || releaseKey() == key) {
        return;
    }
    core::Remembered remembered = m_settings.remembered();
    remembered.releaseKey = key;
    const auto written = m_settings.remember(remembered);
    m_problem = written ? QString() : words("The release key changed for this run, but could not be saved.");
    emit changed();
}

void SettingsView::setHardwareDecoding(const QString& word)
{
    if (!core::Remembered::namesAHardwareDecoding(word) || hardwareDecoding() == word) {
        return;
    }
    core::Remembered remembered = m_settings.remembered();
    remembered.hardwareDecoding = word;
    // (As every choice of the box: where it cannot be written, it is so until the program ends.)
    static_cast<void>(m_settings.remember(remembered));
    emit changed();
    emit decodingChosen();
}

engine::DecodingChoice SettingsView::decoding() const
{
    const QString word = hardwareDecoding();
    return word == "on"_L1    ? engine::DecodingChoice::GraphicsCard
           : word == "off"_L1 ? engine::DecodingChoice::Software
                              : engine::DecodingChoice::Automatic;
}

void SettingsView::setGraphicsCard(const QString& word)
{
    if (!core::Remembered::namesAGraphicsCard(word) || graphicsCard() == word) {
        return;
    }
    core::Remembered remembered = m_settings.remembered();
    remembered.graphicsCard = word;
    static_cast<void>(m_settings.remember(remembered));
    emit changed();
}

QVariantList SettingsView::graphicsCards() const
{
    QVariantList listed;
    for (const QString& card : cards) {
        listed.append(QVariantMap{{u"value"_s, card},
                                  {u"adapter"_s, m_adapterNames.value(card)},
                                  {u"monitor"_s, m_adapterMonitors.value(card, true)}});
    }
    return listed;
}

engine::AdapterChoice SettingsView::startedOn() const
{
    return adapterChoiceOf(m_cardAtStart);
}

void SettingsView::nameAdaptersWith(std::function<QString(engine::AdapterChoice)> names)
{
    m_names = std::move(names);
    nameAdapters();
    emit changed();
}

void SettingsView::seeMonitorsWith(std::function<bool(engine::AdapterChoice)> monitors)
{
    m_monitors = std::move(monitors);
    nameAdapters();
    emit changed();
}

void SettingsView::nameAdapters()
{
    m_adapterNames.clear();
    m_adapterMonitors.clear();
    for (const QString& card : cards) {
        if (m_names) {
            m_adapterNames.insert(card, m_names(adapterChoiceOf(card)));
        }
        if (m_monitors) {
            m_adapterMonitors.insert(card, m_monitors(adapterChoiceOf(card)));
        }
    }
}

void SettingsView::setStartsWithWindows(bool on)
{
    const base::Result<void> taken = m_start.set(on);
    m_problem = taken ? QString()
                : on  ? words("Windows did not take it: qKVM will not start with Windows.")
                      : words("Windows did not take it: qKVM still starts with Windows.");
    // What is so is what Windows says now, whatever was asked for.
    m_startsWithWindows = m_start.isOn();
    emit changed();
}

void SettingsView::lookAgain()
{
    m_problem.clear();
    m_startsWithWindows = m_start.isOn();
    // (A graphics card may have come or gone since: one of a dock, or a driver put in.)
    nameAdapters();
    emit changed();
}

QVariantList SettingsView::sectionsFor(const QString& wall)
{
    const auto group = [](const QString& key, const QStringList& sections) {
        return QVariantMap{{u"group"_s, key}, {u"sections"_s, sections}};
    };
    QVariantList groups;
    // This PC's wall's own: only where the window shows that wall.
    if (wall == u"pc"_s) {
        groups.append(group(u"pc"_s, {u"kvms"_s, u"actions"_s, u"files"_s}));
    }
    groups.append(
        group(u"program"_s, {u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s, u"about"_s}));
    // A server's wall as a file, as the page lists it for an admin (app.js, SETTINGS): written and
    // read by the server, only where whoever is signed in manages it. (The rest of a server's
    // settings stay on its page.) Last: where somebody comes to manage the wall while the box is
    // open, it comes in below what is listed, and nothing that was there moves.
    if (wall == u"managed"_s) {
        groups.append(group(u"server"_s, {u"files"_s}));
    }
    return groups;
}

namespace {

// The keys of the sections listed for a wall, in their order.
QStringList listedFor(const QString& wall)
{
    QStringList keys;
    for (const QVariant& group : SettingsView::sectionsFor(wall)) {
        keys += group.toMap().value(u"sections"_s).toStringList();
    }
    return keys;
}

} // namespace

QString SettingsView::opensAt(const QString& wall) const
{
    const QStringList listed = listedFor(wall);
    return listed.contains(m_section) ? m_section : listed.value(0);
}

void SettingsView::show(const QString& section)
{
    // (A section that no wall lists is none.)
    if (section == m_section || !listedFor(u"pc"_s).contains(section)) {
        return;
    }
    m_section = section;
    emit sectionChanged();
}

void SettingsView::openIncidents()
{
    show(u"alerts"_s);
    emit incidentsAsked();
}

void SettingsView::closed()
{
    core::Remembered remembered = m_settings.remembered();
    if (remembered.settingsSection == m_section) {
        return;
    }
    remembered.settingsSection = m_section;
    // (As every choice of the box: where it cannot be written, it is so until the program ends.)
    static_cast<void>(m_settings.remember(remembered));
}

} // namespace qkvm::ui
