#include <algorithm>

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QTest>

#include <qkvm/core/server_types.h>

#include "details_view.h"
#include "wall_cards.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

// The words the server and this program share (the web's W8): its states and their labels, a device's codes and the
// tile state each is shown under, the wall's refusals with their HTTP statuses, the kinds of device and the words of
// `can`, as its app/qkvm/static/words.json has them. A copy of that file is kept beside the program (ui/words.json,
// where it came from in ui/words-from.txt); every word of it is to be in Words.qml with the server's label, and a
// machine's card is to call each state what the server calls it. A word the server has and this program lacks fails
// here, in every gate. (A word of this program's own, that the server does not have, is let be.)
class TestWords : public QObject {
    Q_OBJECT

    QJsonObject m_server;
    QQmlEngine m_engine;
    QObject* m_words = nullptr;

    // The page writes three dots where this program writes the one sign for them; nothing else may differ.
    static QString asWritten(QString label) { return label.replace("..."_L1, QString(QChar(0x2026))); }

    QVariantMap table(const char* name) const { return m_words->property(name).toMap(); }

    // Every word of the server's section is in the program's table, with the same label.
    void sameLabels(const char* section, const char* property) const
    {
        const QJsonObject server = m_server[QLatin1StringView(section)].toObject();
        QVERIFY2(!server.isEmpty(), section);
        const QVariantMap ours = table(property);
        for (auto word = server.begin(); word != server.end(); ++word) {
            QVERIFY2(ours.contains(word.key()),
                     qPrintable(u"Words.%1 lacks \"%2\""_s.arg(QLatin1StringView(property), word.key())));
            QCOMPARE(ours.value(word.key()).toString(), asWritten(word.value().toString()));
        }
    }

private slots:
    void initTestCase()
    {
        QFile file(QStringLiteral(QKVM_WORDS_JSON));
        QVERIFY2(file.open(QIODevice::ReadOnly), "ui/words.json is not there");
        QJsonParseError problem{};
        const QJsonDocument read = QJsonDocument::fromJson(file.readAll(), &problem);
        QVERIFY2(read.isObject(), qPrintable(problem.errorString()));
        m_server = read.object();
        m_words = m_engine.singletonInstance<QObject*>("Qkvm", "Words");
        QVERIFY(m_words != nullptr);
    }

    void everyStateIsCalledWhatTheServerCallsIt() { sameLabels("states", "states"); }
    void aServerThatIsOffIsCalledWhatTheServerCallsIt() { sameLabels("server_states", "serverStates"); }
    void everyKindIsCalledWhatTheServerCallsIt() { sameLabels("kinds", "kinds"); }
    void everyWordOfCanIsCalledWhatTheServerCallsIt() { sameLabels("can", "can"); }

    void everyCodeIsShownUnderTheStateTheServerShowsItUnder()
    {
        const QJsonObject server = m_server["codes"_L1].toObject();
        QVERIFY(!server.isEmpty());
        const QVariantMap ours = table("codes");
        for (auto code = server.begin(); code != server.end(); ++code) {
            QVERIFY2(ours.contains(code.key()), qPrintable(u"Words.codes lacks \"%1\""_s.arg(code.key())));
            QCOMPARE(ours.value(code.key()).toString(), code.value().toString());
        }
    }

    void everyRefusalComesWithTheStatusesTheServerSendsItWith()
    {
        const QJsonObject server = m_server["refusals"_L1].toObject();
        QVERIFY(!server.isEmpty());
        const QVariantMap ours = table("refusals");
        for (auto refusal = server.begin(); refusal != server.end(); ++refusal) {
            QVERIFY2(ours.contains(refusal.key()), qPrintable(u"Words.refusals lacks \"%1\""_s.arg(refusal.key())));
            QList<int> theirs;
            const QJsonArray statuses = refusal.value().toArray();
            for (const auto& status : statuses) {
                theirs.append(status.toInt());
            }
            QList<int> said;
            for (const QVariant& status : ours.value(refusal.key()).toList()) {
                said.append(status.toInt());
            }
            std::ranges::sort(theirs);
            std::ranges::sort(said);
            QCOMPARE(said, theirs);
        }
    }

    // Details says a state in the same word, where an alert says what a machine went from and to.
    void detailsCallEveryStateWhatTheServerCallsIt()
    {
        const QJsonObject states = m_server["states"_L1].toObject();
        QVERIFY(!states.isEmpty());
        for (auto state = states.begin(); state != states.end(); ++state) {
            QCOMPARE(ui::DetailsView::stateWord(state.key()), asWritten(state.value().toString()));
        }
        // (One added since is said as it came.)
        QCOMPARE(ui::DetailsView::stateWord(u"sleeping"_s), u"sleeping"_s);
    }

    // A machine's card says its state in the same word: the card is where a person reads it.
    void aCardCallsEveryStateWhatTheServerCallsIt()
    {
        core::Machine kvm;
        kvm.id = u"a"_s;
        kvm.name = u"A"_s;
        const QJsonObject states = m_server["states"_L1].toObject();
        QVERIFY(!states.isEmpty());
        for (auto state = states.begin(); state != states.end(); ++state) {
            core::Status status;
            status.state = state.key();
            QCOMPARE(ui::cardFor(kvm, &status, 0, false).label, asWritten(state.value().toString()));
        }
        core::Machine server = kvm;
        server.hasPicture = false;
        const QJsonObject serverStates = m_server["server_states"_L1].toObject();
        QVERIFY(!serverStates.isEmpty());
        for (auto state = serverStates.begin(); state != serverStates.end(); ++state) {
            core::Status status;
            status.state = state.key();
            status.hasPicture = false;
            QCOMPARE(ui::cardFor(server, &status, 0, false).label, asWritten(state.value().toString()));
        }
    }
};

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QGuiApplication app(argc, argv);
    TestWords test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_words.moc"
