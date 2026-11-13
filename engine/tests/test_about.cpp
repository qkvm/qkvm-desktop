#include <string>

#include <QTest>

#include <qkvm/engine/about.h>

// The FFmpeg that is linked is the one that may ship, and it can do what the engine will ask of it.
// tools/build_ffmpeg.ps1 asks the same of the build it makes (and fetch_ffmpeg.ps1 of the development one it
// fetches); this asks it of the build that was
// linked, which is the one that counts.
class TestAbout : public QObject {
    Q_OBJECT

private slots:
    void theDecoderLibrarySaysWhatItIs()
    {
        const qkvm::engine::DecoderLibrary library = qkvm::engine::decoderLibrary();
        QVERIFY(!library.version.empty());
        QVERIFY(!library.licence.empty());
        QVERIFY(!library.configuration.empty());
    }

    void theDecoderLibraryIsLgpl()
    {
        const qkvm::engine::DecoderLibrary library = qkvm::engine::decoderLibrary();
        QVERIFY2(library.licence.find("LGPL") != std::string::npos, library.licence.c_str());
        QVERIFY2(library.configuration.find("--enable-gpl") == std::string::npos, "built with GPL parts");
        QVERIFY2(library.configuration.find("--enable-nonfree") == std::string::npos, "built with non-free parts");
    }

    void itDecodesWhatTheEngineNeeds()
    {
        const qkvm::engine::DecoderAbilities abilities = qkvm::engine::decoderAbilities();
        QVERIFY(abilities.h264);
        QVERIFY(abilities.h264OnTheCard);
        QVERIFY(abilities.jpeg);
        QVERIFY(abilities.png);
    }
};

QTEST_APPLESS_MAIN(TestAbout)
#include "test_about.moc"
