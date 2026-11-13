#include "package_check.h"

#include <iostream>
#include <memory>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSslSocket>

#include "application.h"
#include "mark.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

int checkPackage()
{
    const QString folder = QCoreApplication::applicationDirPath();
    QCoreApplication::setLibraryPaths({folder + u"/plugins"_s});
    if (QGuiApplication::platformName() != "windows"_L1) {
        qCritical("Package check requires the Windows platform plugin.");
        return 1;
    }
    if (!QSslSocket::setActiveBackend(u"schannel"_s) || !QSslSocket::supportsSsl()) {
        qCritical("The packaged Windows TLS backend is unavailable.");
        return 1;
    }
    if (markImage(32).isNull()) {
        qCritical("The bundled SVG icon could not be rendered.");
        return 1;
    }

    QQmlEngine engine;
    keepOffTheNetwork(engine);
    // No fallback to Qt's build-time QML folder can conceal a missing packaged import.
    engine.setImportPathList({folder + u"/qml"_s, u"qrc:/qt/qml"_s, u"qrc:/qt-project.org/imports"_s});
    QQmlComponent main(&engine);
    main.loadFromModule(u"Qkvm"_s, u"Main"_s, QQmlComponent::PreferSynchronous);
    if (!main.isReady()) {
        qCritical().noquote() << main.errorString();
        return 1;
    }
    // Instantiate a real bundled control, including Theme and the Quick Templates plugin.
    // Main is compiled above but not instantiated: it cannot ask for a profile or a source.
    QQmlComponent control(&engine);
    control.setData("import Qkvm\nButton { text: \"Package check\" }", QUrl(u"qrc:/package-check.qml"_s));
    const std::unique_ptr<QObject> made(control.create());
    if (made == nullptr) {
        qCritical().noquote() << control.errorString();
        return 1;
    }
    // A GUI process must report this without an inherited Qt logging override.
    std::cout << "QKVM_PACKAGE_OK: Windows platform, bundled QML, controls, SVG and Windows TLS loaded.\n";
    return 0;
}

} // namespace qkvm::ui
