#include <QTest>

#include "about_model.h"

using namespace Qt::StringLiterals;
using qkvm::ui::AboutModel;

// The About box's model, without a screen.
class TestAboutModel : public QObject {
    Q_OBJECT

private:
    static QString text(const AboutModel& model, int row, AboutModel::Role role)
    {
        return model.data(model.index(row), role).toString();
    }

    static int rowNamed(const AboutModel& model, const QString& name)
    {
        for (int row = 0; row < model.rowCount(); ++row) {
            if (text(model, row, AboutModel::NameRole) == name) {
                return row;
            }
        }
        return -1;
    }

private slots:
    void diagnosticFolderOpensOnlyOnRequestAndReportsFailure()
    {
        AboutModel model;
        QVERIFY(!model.diagnosticsAvailable());
        model.openDiagnostics();
        int opened = 0;
        model.useDiagnostics([&] {
            ++opened;
            return false;
        });
        QCOMPARE(opened, 0);
        QVERIFY(model.diagnosticsAvailable());
        model.openDiagnostics();
        QCOMPARE(opened, 1);
        QVERIFY(!model.diagnosticsProblem().isEmpty());
        model.useDiagnostics([&] {
            ++opened;
            return true;
        });
        model.openDiagnostics();
        QCOMPARE(opened, 2);
        QVERIFY(model.diagnosticsProblem().isEmpty());
    }

    void theProgramSaysItsVersionAndLicence()
    {
        const AboutModel model;
        QVERIFY(!model.version().isEmpty());
        QCOMPARE(model.licence(), u"AGPL-3.0-only"_s);
    }

    void theProgramSaysWhereItsOwnSourceIsAtItsVersion()
    {
        // (Asked as the screen asks it, by the property's name.)
        const AboutModel model;
        QCOMPARE(model.property("source").toString(),
                 u"https://github.com/qkvm/qkvm-desktop/tree/v"_s + model.version());
    }

    void ffmpegAndItsZlibAreOfferedWithTheRelease()
    {
        // The project's own FFmpeg, with zlib inside it: their sources are a file of every release.
        const AboutModel model;
        const QString release = u"https://github.com/qkvm/qkvm-desktop/releases/tag/v"_s + model.version();
        QCOMPARE(text(model, rowNamed(model, u"FFmpeg"_s), AboutModel::SourceRole), release);
        const int zlib = rowNamed(model, u"zlib"_s);
        QVERIFY(zlib >= 0);
        QCOMPARE(text(model, zlib, AboutModel::LicenceRole), u"Zlib"_s);
        QCOMPARE(text(model, zlib, AboutModel::SourceRole), release);
    }

    void everyLibraryThatShipsHasARowThatIsWhole()
    {
        const AboutModel model;
        QVERIFY(rowNamed(model, u"Qt"_s) >= 0);
        QVERIFY(rowNamed(model, u"FFmpeg"_s) >= 0);
        for (int row = 0; row < model.rowCount(); ++row) {
            for (const AboutModel::Role role :
                 {AboutModel::NameRole, AboutModel::VersionRole, AboutModel::LicenceRole, AboutModel::SourceRole}) {
                QVERIFY2(!text(model, row, role).isEmpty(), qPrintable(u"row %1, role %2"_s.arg(row).arg(int(role))));
            }
            QVERIFY(text(model, row, AboutModel::SourceRole).startsWith(u"https://"));
        }
    }

    void qtIsTheOneThatIsRunning()
    {
        const AboutModel model;
        const int row = rowNamed(model, u"Qt"_s);
        QCOMPARE(text(model, row, AboutModel::VersionRole), QString::fromLatin1(qVersion()));
        QVERIFY(text(model, row, AboutModel::SourceRole).endsWith(u"/%1/"_s.arg(QString::fromLatin1(qVersion()))));
    }

    void theRolesHaveTheNamesTheScreenAsksFor()
    {
        const AboutModel model;
        const QList<QByteArray> names = model.roleNames().values();
        for (const char* name : {"name", "version", "licence", "source"}) {
            QVERIFY2(names.contains(QByteArray(name)), name);
        }
    }

    void aRowThatIsNotThereGivesNothing()
    {
        const AboutModel model;
        QVERIFY(!model.data(model.index(model.rowCount()), AboutModel::NameRole).isValid());
        QVERIFY(!model.data(model.index(0), Qt::UserRole).isValid());
    }
};

QTEST_GUILESS_MAIN(TestAboutModel)
#include "test_about_model.moc"
