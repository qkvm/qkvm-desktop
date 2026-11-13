#pragma once

#include <functional>

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

// What the About box shows: this program's version and licence, and one row for each library that
// ships with it, with its licence and where its source is offered. Made in C++ and handed to the
// screen; lives on the interface thread.
class AboutModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The About box is handed its model by the program.")
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString licence READ licence CONSTANT)
    // Where this program's own source is offered: its public repository at the tag of this version.
    Q_PROPERTY(QString source READ source CONSTANT)
    Q_PROPERTY(bool diagnosticsAvailable READ diagnosticsAvailable NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString diagnosticsProblem READ diagnosticsProblem NOTIFY diagnosticsChanged)

public:
    enum Role { NameRole = Qt::UserRole + 1, VersionRole, LicenceRole, SourceRole };

    explicit AboutModel(QObject* parent = nullptr);

    QString version() const;
    QString licence() const;
    QString source() const;
    bool diagnosticsAvailable() const { return bool(m_diagnostics); }
    QString diagnosticsProblem() const { return m_diagnosticsProblem; }
    void useDiagnostics(std::function<bool()> open);
    Q_INVOKABLE void openDiagnostics();

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void diagnosticsChanged();

private:
    std::function<bool()> m_diagnostics;
    QString m_diagnosticsProblem;
    struct Library {
        QString name;
        QString version;
        QString licence;
        QString source;
    };
    QList<Library> m_libraries;
};

} // namespace qkvm::ui
