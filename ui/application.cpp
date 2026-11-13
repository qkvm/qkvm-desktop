#include "application.h"

#include <memory>

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPixmap>
#include <QQmlEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QStyleHints>
#include <QUrl>

#include "mark.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// Asks nobody: whatever is asked of it is asked of an address of no kind, which is refused where it
// stands, before a name is looked up or a line is opened.
class AsksNobody final : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& /*asked*/, QIODevice* outgoing) override
    {
        return QNetworkAccessManager::createRequest(operation, QNetworkRequest(QUrl()), outgoing);
    }
};

class NoNetwork final : public QQmlNetworkAccessManagerFactory {
public:
    // (The engine makes one for each of its threads, and gives each its parent.)
    QNetworkAccessManager* create(QObject* parent) override { return std::make_unique<AsksNobody>(parent).release(); }
};

} // namespace

void keepOffTheNetwork(QQmlEngine& engine)
{
    // The engine does not own it, and may ask it from other threads than this one: it holds nothing.
    static NoNetwork none;
    engine.setNetworkAccessManagerFactory(&none);
}

void prepareApplication()
{
    // This internal name locates the existing settings and kept sign-in. Only the displayed name changes.
    QGuiApplication::setApplicationName(u"QKVM Desktop"_s);
    QGuiApplication::setApplicationDisplayName(u"qKVM"_s);
    QGuiApplication::setApplicationVersion(QStringLiteral(QKVM_DESKTOP_VERSION));

    // The page's letters, in the page's size. Windows 10 has no Segoe UI Variable and gets Segoe UI.
    // The one that is there is named, and no list of wishes: a control takes the program's font by
    // its one name, and a name that is not on the PC would give it whatever Windows finds instead.
    QFont font;
    for (const QString& family : {u"Segoe UI Variable Text"_s, u"Segoe UI"_s}) {
        if (QFontDatabase::hasFamily(family)) {
            font.setFamily(family);
            break;
        }
    }
    font.setPixelSize(13);
    QGuiApplication::setFont(font);

    // The program's mark, for its windows' title bars and for the taskbar: in the sizes Windows
    // asks for at the scales a monitor can have.
    QIcon mark;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 256}) {
        mark.addPixmap(QPixmap::fromImage(markImage(size)));
    }
    QGuiApplication::setWindowIcon(mark);

    // Windows' own title bar follows this too.
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
}

} // namespace qkvm::ui
