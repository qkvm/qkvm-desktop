#pragma once

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace Qt::StringLiterals;

// Disposable file handshake used only by the native/Python compatibility fixture.
inline QJsonObject fixturePhase(const QDir& folder, const QString& name, int withinMs = 20'000)
{
    QElapsedTimer clock;
    clock.start();
    QFile request(folder.filePath(name + u".request"_s));
    if (!request.open(QIODevice::WriteOnly)) {
        qWarning() << "PHASE" << name << "request-open" << clock.elapsed() << request.errorString();
        return {};
    }
    request.close();
    QFile reply(folder.filePath(name + u".json"_s));
    int openFailures = 0;
    // Existence and readability are separate on Windows. An atomic publisher
    // can finish while another reader temporarily holds a non-shared handle.
    const bool opened = QTest::qWaitFor(
        [&] {
            if (!reply.exists())
                return false;
            if (reply.open(QIODevice::ReadOnly))
                return true;
            ++openFailures;
            return false;
        },
        withinMs);
    if (!opened) {
        qWarning() << "PHASE" << name << "wait-or-open" << clock.elapsed() << reply.exists() << openFailures
                   << reply.errorString();
        return {};
    }
    const auto bytes = reply.read(4097);
    if (reply.error() != QFileDevice::NoError || bytes.size() > 4096) {
        qWarning() << "PHASE" << name << "read" << clock.elapsed() << bytes.size() << reply.errorString();
        return {};
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning() << "PHASE" << name << "json" << clock.elapsed() << bytes.size() << error.error;
        return {};
    }
    qInfo() << "PHASE" << name << "read" << clock.elapsed() << bytes.size() << openFailures
            << document.object().value(u"ok"_s).toBool();
    return document.object();
}
