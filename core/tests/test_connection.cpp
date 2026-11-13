#include <QTest>

#include <qkvm/core/connection.h>

using qkvm::core::after;
using qkvm::core::Connection;
using Event = qkvm::core::ConnectionEvent;

Q_DECLARE_METATYPE(Connection)
Q_DECLARE_METATYPE(Event)

// What follows what with a server: every state with everything that can happen to it.
class TestConnection : public QObject {
    Q_OBJECT

private slots:
    void whatFollowsWhat_data()
    {
        QTest::addColumn<Connection>("from");
        QTest::addColumn<Event>("event");
        QTest::addColumn<Connection>("to");

        QTest::newRow("a server is named") << Connection::None << Event::Named << Connection::Connecting;
        QTest::newRow("it says nobody is signed in")
            << Connection::Connecting << Event::NobodySignedIn << Connection::WantsSignIn;
        QTest::newRow("it says somebody still is")
            << Connection::Connecting << Event::SomebodySignedIn << Connection::SignedIn;
        QTest::newRow("it is not there") << Connection::Connecting << Event::NoAnswer << Connection::None;
        QTest::newRow("a password is sent") << Connection::WantsSignIn << Event::SignInSent << Connection::SigningIn;
        QTest::newRow("the password is right") << Connection::SigningIn << Event::SignInTaken << Connection::SignedIn;
        QTest::newRow("the password is wrong")
            << Connection::SigningIn << Event::SignInRefused << Connection::WantsSignIn;
        QTest::newRow("the server falls silent with the password")
            << Connection::SigningIn << Event::NoAnswer << Connection::WantsSignIn;
        QTest::newRow("the server stops answering") << Connection::SignedIn << Event::ContactLost << Connection::Away;
        QTest::newRow("the server answers again") << Connection::Away << Event::ContactBack << Connection::SignedIn;
        QTest::newRow("signed out from the wall")
            << Connection::SignedIn << Event::SignedOut << Connection::WantsSignIn;
        QTest::newRow("signed out while the server was away")
            << Connection::Away << Event::SignedOut << Connection::WantsSignIn;

        QTest::newRow("it shows a certificate nobody has said yes to")
            << Connection::Connecting << Event::CertificateShown << Connection::WantsTrust;
        QTest::newRow("somebody says yes to it") << Connection::WantsTrust << Event::Trusted << Connection::Connecting;
        QTest::newRow("another certificate while the password is with it")
            << Connection::SigningIn << Event::CertificateShown << Connection::WantsTrust;
        QTest::newRow("another certificate on the wall")
            << Connection::SignedIn << Event::CertificateShown << Connection::WantsTrust;
        QTest::newRow("another certificate when the server comes back")
            << Connection::Away << Event::CertificateShown << Connection::WantsTrust;
        QTest::newRow("a certificate without a server")
            << Connection::None << Event::CertificateShown << Connection::None;
        QTest::newRow("a yes nobody was asked for")
            << Connection::Connecting << Event::Trusted << Connection::Connecting;
        QTest::newRow("a yes on the wall") << Connection::SignedIn << Event::Trusted << Connection::SignedIn;
        QTest::newRow("an answer while the certificate waits")
            << Connection::WantsTrust << Event::SomebodySignedIn << Connection::WantsTrust;
        QTest::newRow("a password while the certificate waits")
            << Connection::WantsTrust << Event::SignInSent << Connection::WantsTrust;

        // What means nothing where it happens changes nothing: a second address while the first is
        // being asked, a password nobody was asked for, an answer that comes after the wall was left.
        QTest::newRow("a second address while the first is asked")
            << Connection::Connecting << Event::Named << Connection::Connecting;
        QTest::newRow("an address on the wall") << Connection::SignedIn << Event::Named << Connection::SignedIn;
        QTest::newRow("a password before a server") << Connection::None << Event::SignInSent << Connection::None;
        QTest::newRow("a second password while the first is with the server")
            << Connection::SigningIn << Event::SignInSent << Connection::SigningIn;
        QTest::newRow("a password on the wall") << Connection::SignedIn << Event::SignInSent << Connection::SignedIn;
        QTest::newRow("a right password nobody sent")
            << Connection::WantsSignIn << Event::SignInTaken << Connection::WantsSignIn;
        QTest::newRow("signed in without a server") << Connection::None << Event::SomebodySignedIn << Connection::None;
        QTest::newRow("signing out where nobody is signed in")
            << Connection::WantsSignIn << Event::SignedOut << Connection::WantsSignIn;
        QTest::newRow("contact lost before the wall")
            << Connection::WantsSignIn << Event::ContactLost << Connection::WantsSignIn;
        QTest::newRow("contact back that was never lost")
            << Connection::SignedIn << Event::ContactBack << Connection::SignedIn;
        QTest::newRow("contact lost twice") << Connection::Away << Event::ContactLost << Connection::Away;
    }

    void whatFollowsWhat()
    {
        QFETCH(Connection, from);
        QFETCH(Event, event);
        QFETCH(Connection, to);
        QCOMPARE(after(from, event), to);
    }

    void anotherServerCanBeNamedFromAnywhere()
    {
        for (const Connection from :
             {Connection::None, Connection::Connecting, Connection::WantsTrust, Connection::WantsSignIn,
              Connection::SigningIn, Connection::SignedIn, Connection::Away}) {
            QCOMPARE(after(from, Event::Left), Connection::None);
        }
    }

    void nobodyReachesTheWallWithoutTheServerSayingSo()
    {
        // Whatever happens, in whatever state: the wall is only entered by the server's own word,
        // that the session is somebody's or that the sign-in was taken.
        const auto onTheWall = [](Connection state) {
            return state == Connection::SignedIn || state == Connection::Away;
        };
        for (const Connection from : {Connection::None, Connection::Connecting, Connection::WantsTrust,
                                      Connection::WantsSignIn, Connection::SigningIn}) {
            for (const Event event :
                 {Event::Named, Event::NobodySignedIn, Event::SomebodySignedIn, Event::NoAnswer,
                  Event::CertificateShown, Event::Trusted, Event::SignInSent, Event::SignInTaken, Event::SignInRefused,
                  Event::SignedOut, Event::ContactLost, Event::ContactBack, Event::Left}) {
                const bool theServersWord = (from == Connection::Connecting && event == Event::SomebodySignedIn) ||
                                            (from == Connection::SigningIn && event == Event::SignInTaken);
                QCOMPARE(onTheWall(after(from, event)), theServersWord);
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestConnection)

#include "test_connection.moc"
