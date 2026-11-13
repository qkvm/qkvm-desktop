#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/settings.h>

namespace qkvm::ui {

// How the program looks, as far as whoever sits at it chooses: dark, light, or as Windows is set.
// Dark until somebody chooses otherwise, and the choice is kept on this PC. The screens take their
// colours from what is chosen here (Theme.qml reads what the application is told); the button on
// the bar goes from one choice to the next, as the page's does. On the interface thread; the
// settings outlive it.
class Look : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // "dark", "light" or "system".
    Q_PROPERTY(QString theme READ theme NOTIFY themeChanged)
    // The page's icon for the choice, and what its button says: "Dark theme (click for light theme)",
    // also where a row of More names it beside its icon (app.js, the theme's row).
    Q_PROPERTY(QString themeSymbol READ themeSymbol NOTIFY themeChanged)
    Q_PROPERTY(QString themeHint READ themeHint NOTIFY themeChanged)

public:
    // `tell` is how the application is told: Qt's own way, unless a test hands in its own (Qt's
    // platform without a screen takes no telling).
    explicit Look(core::Settings& settings, std::function<void(Qt::ColorScheme)> tell = tellQt,
                  QObject* parent = nullptr);

    // Tells Qt, and through it Windows' title bar: "as Windows is set" is what Qt does when it is
    // told nothing.
    static void tellQt(Qt::ColorScheme scheme);

    QString theme() const { return m_theme; }
    QString themeSymbol() const;
    QString themeHint() const;
    // The next of the three, round again; applied at once and written down.
    Q_INVOKABLE void nextTheme();
    // One of the three by its name, the same way. What is none of them changes nothing.
    Q_INVOKABLE void choose(const QString& theme);

signals:
    void themeChanged();

private:
    void apply() const;

    core::Settings& m_settings;
    std::function<void(Qt::ColorScheme)> m_tell;
    QString m_theme;
};

} // namespace qkvm::ui
