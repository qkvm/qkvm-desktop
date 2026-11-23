#include <QByteArray>
#include <QNetworkCookie>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>

#include "../../bmc/fuzz/fuzz_ami_console.h"
#include "../../bmc/fuzz/fuzz_aspeed.h"
#include "../../rfb/fuzz/fuzz_inflate.h"
#include "../fuzz/fuzz_h264_rtp.h"
#include "../fuzz/fuzz_jetkvm.h"
#include "../fuzz/fuzz_jpeg.h"
#include "../fuzz/fuzz_kvmd.h"
#include "../fuzz/fuzz_kvmd_media.h"
#include "../fuzz/fuzz_multipart.h"
#include "../fuzz/fuzz_redfish.h"
#include "../fuzz/fuzz_rfb.h"
#include "../fuzz/fuzz_rtc_sdp.h"
#include "../fuzz/fuzz_strict_json.h"
#include "../fuzz/fuzz_vnc.h"
#include "../src/jetkvm_words.h"

// What the fuzz targets ever found (DESIGN.md section 8): every input that once ended one of them (a crash, a hang, a
// promise broken), as its bytes, run through the very harness that found it, in every preset; the empty input through
// each, so that each harness is seen to run here; and the input with which each found its canary (fuzz_promise.h), held
// to reaching that deep state still, so that a harness or a reader that no longer reaches it is seen here, in every
// build, and not only in a canary proof. A row that fails ends this program with the promise it broke
// (base/tests/quiet_failures.cpp), or with the address sanitizer's words in its preset.
//
// A new row: the input libFuzzer kept (build\fuzz\fuzz\findings\<target>\crash-..., tools/fuzz.ps1), as hexadecimal,
// with what it found and when; the fault fixed first, then the row added, seen red without the fix.
using namespace Qt::StringLiterals;

namespace {

using Harness = std::function<void(std::span<const std::uint8_t>)>;

Harness harnessOf(const QByteArray& name)
{
    namespace fuzz = qkvm::fuzz;
    if (name == "bmc_aspeed") {
        return fuzz::readAspeed;
    }
    if (name == "bmc_ami_console") {
        return fuzz::readAmiConsole;
    }
    if (name == "rfb_inflate") {
        return fuzz::readInflate;
    }
    if (name == "rfb") {
        return fuzz::readRfb;
    }
    if (name == "jpeg") {
        return fuzz::readJpeg;
    }
    if (name == "vnc") {
        return fuzz::readVnc;
    }
    if (name == "redfish") {
        return fuzz::readRedfish;
    }
    if (name == "kvmd") {
        return fuzz::readKvmd;
    }
    if (name == "kvmd_media") {
        return fuzz::readKvmdMedia;
    }
    if (name == "multipart") {
        return fuzz::readMultipart;
    }
    if (name == "strict_json") {
        return fuzz::readStrictJson;
    }
    if (name == "rtc_sdp") {
        return fuzz::readRtcSdp;
    }
    if (name == "h264_rtp") {
        return fuzz::readH264Rtp;
    }
    if (name == "jetkvm") {
        return fuzz::readJetKvm;
    }
    return {};
}

// Every harness, by the name its fuzz target's seeds go under.
constexpr const char* harnesses[] = {"bmc_aspeed",  "bmc_ami_console", "rfb_inflate", "rfb",        "jpeg",
                                     "vnc",         "redfish",         "kvmd",        "kvmd_media", "multipart",
                                     "strict_json", "rtc_sdp",         "h264_rtp",    "jetkvm"};

struct Found {
    const char* harness;
    const char* what; // what it found, and when
    const char* hex;  // the input
};

// What was found, oldest first. No reader fault has been found: each of these broke a promise of its harness's own that
// was stricter than its reader is meant to be, and the promise was put right (the reader as the web's is).
constexpr Found found[] = {
    // 2026-11-17, fuzz_jpeg: a frame header of a side of nought, which the walk takes and the decoder makes no pixels
    // of (the harness had a side of nought as "a JPEG of no size was taken").
    {"jpeg", "a JPEG of a side of nought (2026-11-17)",
     "ffd8ffc00011080000001003011100021100031100ffda0008010100003f001234ffd9"},
    // 2026-11-17, fuzz_kvmd: a text of the KVM's cut at its line's end once made one line, a blank ending it (the web's
    // plain cuts what it joined; the harness had it as "blanks that are not one blank").
    {"kvmd", "a text cut at its line's end after a blank (2026-11-17)", "026162206364"},
    // 2026-11-17, fuzz_rtc_sdp: a description whose last line was not ended, which its addresses' reader ends (one sign
    // longer: the harness had it as "a description longer without its addresses").
    {"rtc_sdp", "a description whose last line is not ended (2026-11-17)", "00763d30"},
    // 2026-11-17, fuzz_vnc: a server's name cut at its line's end after a blank, as the web's plain cuts what it joined
    // (the harness had it as "a server's name holds blanks that are not one blank").
    {"vnc", "a server's name cut at its line's end after a blank (2026-11-17)",
     "c0524642203030332e3030380a01023031323334353637383961626364656600000000001000102018080100ff00ff00ff10"
     "0800000000000000ffffffffffffff5a00000001000000000008000800000000000000ee070005ee0e000aee150027ee1c00"
     "14ee31323334353637383961626364656600000000001000102018000100ff00ff00ff100800000000000000ffffffffffff"
     "ff5a076120677565524642203030332e3030380a01023031323334353600000000001000102018080100ff00ff00ff100800"
     "000000000000ffff7374000000010000000000080008000000003d0000ee070005ee0e000aee000000000000000000000000"
     "0000000000000000000000000000000000000015000fee1c0014ee230019ee2a001eee310023ee000d05ee070d0dee0e0d40"
     "524642203030332e230019ee2a4000524642201e30"},
    // 2026-11-17, fuzz_redfish, on its own seed once its answers were taken at all (a status of 200): OpenBMC's
    // console,
    // which is bmcweb's WebSocket /kvm/0 and no place of Redfish's (the harness held it to Redfish's places).
    {"redfish", "OpenBMC's console, no place of Redfish's (2026-11-17)",
     "007b22406f646174612e6964223a222f726564666973682f76312f4d616e61676572732f626d63222c224d6f64656c223a22"
     "4f70656e426d63222c2255554944223a2230616131623263332d643465352d663630372d313832392d336134623563366437"
     "653866222c0a2253657276696365456e747279506f696e7455554944223a2231323334353637382d396162632d646566302d"
     "313233342d353637383961626364656630222c224669726d7761726556657273696f6e223a22312e34322e30222c0a224772"
     "6170686963616c436f6e736f6c65223a7b2253657276696365456e61626c6564223a747275652c22436f6e6e656374547970"
     "6573537570706f72746564223a5b224b564d4950225d2c224d6178436f6e63757272656e7453657373696f6e73223a347d2c"
     "0a224f656d223a7b224f70656e426d63223a7b22406f646174612e6964223a222f726564666973682f76312f4d616e616765"
     "72732f626d63232f4f656d2f4f70656e426d63227d7d2c0a22416374696f6e73223a7b22234d616e616765722e5265736574"
     "223a7b22746172676574223a222f726564666973682f76312f4d616e61676572732f626d632f416374696f6e732f4d616e61"
     "6765722e5265736574222c2252657365745479706540526564666973682e416c6c6f7761626c6556616c756573223a5b2247"
     "7261636566756c52657374617274225d7d7d7d"},
};

// The input with which each target found its canary (tools/fuzz.ps1 -Canary), and the state it names.
struct Reached {
    const char* harness;
    const char* state;
    const char* hex;
};

constexpr Reached reached[] = {
    {"bmc_aspeed", "a frame of other data read onto the picture an earlier frame made, changing it",
     "a0007800a00078000004070100000000a0007800a000780000040700a4000001000111000000001100000000"},
    {"bmc_ami_console", "two whole frames of the console, fed in parts of under eight bytes, each drawn",
     "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d0017000000"
     "0000000013000100000001000119007403000000000000000000003c0058000000000000400058000000000000000000000000000000"
     "00000000000000000000000000050100040000000000000000000000000000000000000000001c040000000000000000000000000000"
     "00d4f700e0aa55c416cd634dd2221d7ae5d33fcbf59203e16362280fd2263d641d1d0a08b45c3f5491b5ae89226a7af8209418bc3f88"
     "d2540d3f8ab4974a499e9bf9007e8c035750d01e1f51ffff2fea7ffffc6753fcc7c77f1ffee24ae27f13f9ff975f5b26fe3fd07fe8fb"
     "bf55fdc3c8fdb38cff4331fea7fffab7f9aafeb7a27fb1fb9fbfff39f3156100a1ff59eeffebfcc3b15bfee71f5f27c27f07d79fd9ff"
     "43e6ff1cd59f286f54ff4352fdc7f67f4bf35f19ff79e2f3ff7fcecc80c133e72ebd3a9c913b6abb0f83cdd5bec3853f38969d59ab56"
     "b270eb198b283ff4ba036a4f8acc6492b1b9b6e519b2b4488dd2e09925ee618a900071288053d9d092f6419d9cd7e58add6f6be1593c"
     "e9685de9fbbf35a6f9c2999d07210158b344537eae8b83b46ad50fbcc1e2931af0f6d9b6f912d88d3e23e8ebbf4c5cc7a67f965e2407"
     "c2e723ffddd4ff7afc7785fff18ffff0fc3f12f9af09ff2be8fffffc7f0bc87fa3f6dfae467fa5f9cfdafea5e4fa4f8cff479ca8ffff"
     "d966fc3f1992fdff3d9ff28ffa972594f15fc54afef3fe7f81fe7f12fa7fdaff3c3ee250a2ff64a9ff470f9f87feafe77e99f80d1e4e"
     "ff1beeffd15fe67ffef862d8f93f32fe83ffc0fd978ffd4f8cf88f6bfeec8fe05011fff72c5715fbcfaff0c8fefc6754fff90f789279"
     "0af325d9ff1ccfff89fd5999f95f09fde792ff03c9fe7cbfbcfe5f3bfdf7e5f15f42fe7fc2ff3ff96fa4fef7b0fffae23f80fc357f3d"
     "67172fcc7fcafe5f7d7f19facf8ffb3ff3fff3f3fea1ff44f8e2d3e537ff23f9effa5fc5e58ae43f2dc73f52ffb1bffe8be3b1d43fa2"
     "82b51f092a4b5000ba37bf4b02c4a1724e6543a8d907752826e8ed7f634718ff3c2cf140f1a4b1d3471d710bdf33785805b4a90517db"
     "11fe152af5bb134d6cd51a0081af8544f776d532ffb7f95fff2a5f981eff99d07f50fe3f62f9cff8ffbf3aa1fff96fe16e95e47fa4fa"
     "d7ccffd7ffc5dbf8df69fe93731f8dacff687fff52f27fff5ff75f27f1bf5941f86fe070bd7fc5e708fb1fe0c505be18539f939f5e51"
     "440660a520870b771fd93aab7d2cbfe1a2e17444eec00746d1553955aa958ee3211a7327f84b646991190002010000000000009bdf04"
     "37e25000ddbda72501ece37e78d50c5e96ff39e8ff8cfc67157f2cf29f21fcefdb713af33ff9ff41ff4ae8ff17dff06bd40aff83f5ca"
     "f8afff17fbaf3858ff4b1dfe37f5bffc57bcecfcaf9e6317f45f64c1171cc3f3bfecdfffd4fe2f8173385248fee738aeeff73fff2bc6"
     "ff2fe91fc2c2ff52f19f21ff331feb9df8c87f28f5c747fb43c5fcff1affa5e2bfdff73ba1667f25f2ffd2e6d7ff27b857b7d47fdc2f"
     "f15ffe69bc65fc3866c381c7fe3a0eea9f9affff279ef8ff2c47a2ffcc8f5765fc17f77f45f31fdfea9fab0de37fd5e1bf5dff9f6bfe"
     "2b85ff48f87fa1f9bfcdfe23e3800497ff000000000000000000000000390000000000000019005800000000000000000000007c0066"
     "000000000000800068000000000000000000000000000000000000000000000000000000000301000600000000000000000000000000"
     "0000000000000000e4060000000000000000000000000000001900e606000000000000afff5b0ff444f8ef7f98f85f1fffe3eef5df64"
     "3eef08fff9049e88fcbf3e99ffffddbfa292ff2ae37e67fe7b4fe225c76fe2f9fef7dfe4e1f9efe05a83eaffa0ff085ef858fd838c33"
     "e37fcbb0da9ff6e0efaafffe73b8cfbfea1f453fd9ff01fecffec5f27f655d61931def4a7450b838a94bc9c882a5788c97ceb74faf28"
     "223fff1c2fd5e0fee7d47ffdcf3e34dfb701fba7997ffd83fdabff6fc95ffeebdf5e10e27f3f6dffebffeb48ecf1f94752f5ccfc07df"
     "c4a9fc35ff0df5fe54fe1f27f9afbfeb6fe14fe3bffefadcb0ff30fc670ef0fd53dcff7f3efe13a9ff67eabf46fe7f98fffaf87ed5f6"
     "8727e2ffc3fb7f95fd2f92ffc8439fb8fffe6777ffd7f43ff55143fff57abe9068078c18f2afe2da820eab052128318f182a4467c538"
     "aadb8237f9737a5d57116f4f20779cf7dd45a39f9643c233435115cd01006aeeffddfeff07fa9fe9ff239feb99f96febfff3f97fbbfb"
     "9fbffe94e615f98fd4abffb6fffc9feafff8acff8dff48fd5b57fd57c4ed8fb0ff3facfa1f3eb3ffdcdb41ffadff5dd5ff8581af449c"
     "67990a603aa0a1b870f761b2b3da774af607c762316bd587b78863d9ea4c962a55700d194ed97c8108456f23ffb7e87f81fcfc07f14e"
     "3feb3f56e6ff08e14f52ff7761c2ff12a2f99f5dfc7f68ff59b6fe17d07fea99d3fe712ef58f52ff3fc93d3ba1ff90ec6fd797fbfffb"
     "e3e120fafffa77bf34ffabeaff9afabef0d0a8e043707610e7f180ad3f57018c3bcaaedf04374b5000dd9ba12501e23b39a7b2000000"
     "2000000000000000000000000039000000000000000488eae49cca57ecb60fb8704f287796387b11d258d90f2c143773c2ffd8ffcf7f"
     "fe23fa57c47f85fdaf7ff2d75f467f16c644ff6770ff83f1df251744d1d19f895fff01d6628affcac2ff9abf8ae5ff5ef39fdbfa3f13"
     "f4ff98f05fdeffc87ffd8b8f0565f097ff227a304c91d9e0f19c870ffcce8d2dac8b09e4d32b8ac300ac14ef70e1ee8e6567b5e0fd9f"
     "09fea384ff27fc3f11ffaff97f6ae67fc77d7fb5f1fe83d87f4cf7df85fdf799ffafe63fd1b0e87ff8bbe6bff712ff9f3e7facf99ff9"
     "0fd5e757ecff2cf9ffcfff1b8f7da14b070c98f27f40fecfe4ff2effd1fff914ffe7f304f3df2cec7fc9acf63fff4357c27f301cefb8"
     "ffade5d07f797df0efa9fff967f0ffc45f0fc27f877d5da9ffcf72ffa9df78fbfc4fe1fffc73e67ffe3acf6dfee57f637ffdc7f6af59"
     "f55f12fa93d47ff8d384ffdf65feff27fe86f9f33fd6fcf83ffb4a14fe3fcb1f7070d047c5ff1da5cee1fff9a7baffbd1691ff3fbaff"
     "df7fae9fcf22ff0df3ffccfca788af15f2e1f2ffacfe57a9ff23f6ff2527f37f6a9f7f60ffffe7d6149fe97f353fff80fbc2ff1bf5f8"
     "2ff22b9fd57fcb99ff4df85fdafd77ac7fe2fbd797fbfffbe3e120fafffa77bf34ffabeaff9afabef0d0a8e043707610e7f180ad3f57"
     "018c3bcaaedf04374b5000dd9ba12501e23b39a7b2000000200000000000000000000000003900000000000000f9a7baffbd1691ff3f"
     "baffdf7fae9fcf22ff0df3ffccfca788af15f2e1f2ffacfe57a9ff23f6ff2527f37f6a9f7f60ffffe7d6149fe97f353fff80fbc2ff1b"
     "f5f82ff22b9fd57fcb99ff4df85fdafd77ac7fe2fbd797fbfffbe3e120fafffa77bf34ffabeaff9afabef0d0a8e043707610e7f180ad"
     "3f57018c3bcaaedf04374b5000dd9ba12501e23b39a7b20000002000000000000000000000000039000000000000000488eae49cca57"
     "ecb60fb8704f287796387b11d258d90f2c143773c2ffd8ffcf7ffe23fa57c47f85fdaf7ff2d75f467f16c644ff6770ff83f1df251744"
     "d1d19f895fff01d6628affcac2ff9abf8ae5ff5ef39fdbfa3f13f4ff98f05fdeffc87ffd8b8f0565f097ff227a304c91d9e0f19c870f"
     "fcce8d2dac8b09e4d32b8ac300ac14ef70e1ee8e6567b5e0fd9f09fea384ff27fc3f11ffaff97f6ae67fc77d7fb5f1fe83d87f4cf7df"
     "85fdf799ffafe63fd1b0e87ff8bbe6bff712ff9f3e7facf99ff90fd5e757ecff2cf9ffcfff1b8f7da14b070c98f27f40fecfe4ff2eff"
     "d1fff914ffe7f304f3df2cec7fc9acf63fff4357c27f301cefb8ffade5d07f797df0efa9fff967f0ffc45f0fc27f877d5da9ffcf72ff"
     "a9df78fbfc4fe1fffc73e67ffe3acf6dfee57f637ffdc7f6af59f55f12fa93d47ff8d384ffdf65feff27fe86f9f33fd6fcf83ffb4a14"
     "fe3fcb1f7070d047c5ff1da5cee1fff9a7baffbd1691ff3fbaffdf7fae9fcf22ff0df3ffccfca788af15f2e1f2ffacfe57a9ff23f6ff"
     "2527f37f6a9f7f60ffffe7d6149fe97f353fff80fbc2ff1bf5f82ff22b9fd57fcb99ff4df85fdafd77ac7fe2fbd797fbfffbe3e120fa"
     "fffa77bf34ffabeaff9afabef0d0a8e043707610e7f180ad3f57018c3bcaaedf04374b5000dd9ba12501e23b39a7b200000020000000"
     "0000000000000000003900000000000000"},
    {"rfb_inflate", "a stream of a dynamic block, fed in two parts or more, read to its end as zlib reads it",
     "08070078daedc13101000000c2a0aceb5fc2147e4001000000006f034b6f66f2"},
    {"rfb", "a Tight JPEG decoded into pixels and drawn away from the screen's top left corner",
     "524642203030332e3030380a010100000000010001002018000100ff00ff00ff10080000000000000001730000000100120000000800"
     "0800000007908e05ffd8ffe000104a46494600010101006400640000ffdb00430006040506050406060506070706080a100a0a09090a"
     "140e0f0c1017141818171416161a1d251f1a1b231c1616202c20232627292a29191f2d302d283025282928ffdb0043010707070a080a"
     "130a0a13281a161a28282828282828282828282828282828282828282828282828282828282828282828282828282828282828282828"
     "28282828ffc00011080008000803012200021101031101ffc4001f000001050101010101010000000000000000010203040506070809"
     "0a0bffc400b5100002010303020403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1"
     "f02433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a73747576"
     "7778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7"
     "d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f010003010101010101010101000000000000010203040506070809"
     "0a0bffc400b51100020102040403040705040400010277000102031104052131061241510761711322328108144291a1b1c109233352"
     "f0156272d10a162434e125f11718191a262728292a35363738393a434445464748494a535455565758595a636465666768696a737475"
     "767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5"
     "d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9faffda000c03010002110311003f00ccf09f85bfb9fb9fd28a28af2f1b9b627d"
     "abf78e8e19cf319f518fbc7fffd9"},
    {"jpeg", "a JPEG the walk took, decoded whole at a side that is no multiple of eight",
     "ffd8ffc0001108001000fa03011100021100031100ffda0008010000001234ffd9"},
    {"vnc", "a session signed in by VNC authentication onto a screen larger than the seeds', an update drawn",
     "7f524642203030332e3030380a01023031323334353637383961626364656600000000182000101001000000ff00ff00ff1008000000"
     "00000000076120677565737400000001000000000008000800000000000000ee070005ee0e000aee15000fee1c0014ee230019ee2a00"
     "1eee310023ee000d05ee070d0aee0e0d0fee150d14ee1c0d19ee230d1eee2a0d23ee310d28ee001a0aee071a0fee0e1a14ee151a19ee"
     "1c1a1eee231a23ee2a281aee311a2dee00270fee072714ee0e2719ee15271eee1c2723ee232728ee2a272dee312732ee003414ee0734"
     "19ee0e341eee153423ee1c3428ee23342dee2a3432ee313437ee004119ee07411eee0e4123ee154128ee1c412dee234132ee2a4137ee"
     "31413cee004e1eee074e23ee0e4e28ee154e2dee1c4e32ee234e37ee2a4e3cee314e41ee005b23ee075b28ee0e5b2dee155b32ee1c5b"
     "37ee235b3cee2a5b41ee315b46ee"},
    {"redfish", "a Thermal taken, a temperature of 90 degrees or more read from it beside a fan and its reading",
     "007b2240223a22222c2254656d706572617475726573223a5b7b2252656164696e6743656c73697573223a39307d5d2c2246616e7322"
     "3a5b7b2252656164696e67223a322c22537461747573223a7b225374617465223a22456e61626c6564227d7d5d7d"},
    {"kvmd", "a switch's computers read, the one shown the third or later, a computer on it",
     "507b22726573756c74223a7b22766964656f223a7b226c696e6b73223a5b747275652c66616c73652c747275655d7d2c2273756d6d61"
     "7279223a7b226163746976655f706f7274223a327d7d7d"},
    {"kvmd_media", "two msd events merged into an image on the drive beside two images in the storage",
     "7b226576656e745f74797065223a226d7364222c226576656e74223a7b22656e61626c6564223a747275652c226f6e6c696e65223a66"
     "616c73652c226472697665223a7b22636f6e6e6563746564223a747275657d2c2273746f72616765223a7b22696d61676573223a7b22"
     "62223a7b2200223a317d2c2274223a7b7d7d7d7d7d0a7b226576656e745f74797065223a226d7364222c226576656e74223a7b7d7d"},
    {"multipart", "three pictures handed out of one stream, each part read whole",
     "006d756c7469706172742f782d6d697865642d7265706c6163653b626f756e646172793d626f756e64617279646f6e6f7463726f7373"
     "0a2d2d626f756e64617279646f6e6f7463726f73730d0a74683a2031360d0a436f6e74656e742d4c656e6774683a203636370d0a0d0a"
     "ffd8ffe000104a46494600010101006400640000ffdb00430006040506050406060506070706080a100a0a393a434445464748494a53"
     "5455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6"
     "b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f0100030101010101"
     "01010101fefffffd00000102030405060708090a0bffc400b51100020100000001000000000102030405060708090a0bffc400b51000"
     "02010303020403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a"
     "161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a737475767778797a83848586"
     "8788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5"
     "e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f0100030101010101010101010000000000000102030405060708090a0bffc400b51100"
     "020102040403040705040400010277000102031104052131061241510761711322328108000091a1b1c109233352f0156272d10a1624"
     "34e125f11718191a262728292a35363738393a434445464748494a535455565758595a636465666768696a737475767778797a828384"
     "85868788898a92939495969798999aa2a3a4a5a6a7a8b4aab3b2b5a9b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4"
     "e5e6e7e8e9eaf2f3f4f5f6f7f8f9faffda000c03010002110311003f00f1fd17c33f77e4fd2bbdd17c33f77e4fd2bb1d17c33f771202"
     "d4422e83cc087e4fd28a15c386789b6d4fffd90d0a2d2d626f756e64617279646f6e6f7463726f73730d0a430d0a436f6e74656e742d"
     "4c656e6774683a203636370d0a0d0affd8ffe000104a46494600010101006400640000ffdb0043000604050605040606050607070608"
     "0a100a0a393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a9293949596979899"
     "9aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7"
     "f8f9faffc4001f01000346494600010101006400640000ffdb0043000604050605040606006400640000ffdb00430092939495969798"
     "ffdb004300060405070706080a0d0a0000ffdb0043000604050605040606006400640000ffdb004300060405070706080a0d0a757677"
     "78797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8b4aab3b2b5a9b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7"
     "d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9faffda000c03010002110311003f00f1fd17c33f77e4fd2bbdd17c33f77e4fd2bb1d"
     "17c33f771202d4422e83cc087e4fd28a15c386789b6d4fffd90d0a2d2d626f756e64617279646f6e6f7463726f73730d0a4365676574"
     "31360d0a436f6e74656e742d4c656e6774683a2034300d0a0d0affd8ffe000101d4a4649460001010604050605040606006400640000"
     "ffdb004300060405070706080a0d0a0405070706080a0d0a0000ffdb0043000604050605040606006400640000ffdb00430006040507"
     "0706080a0d0a75767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8b4aab3b2b5a9b6b7b8b9bac2c3c4c5c6c7"
     "c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9faffda000c03010002110311003f00f1fd17c33f77e4fd2bbd"
     "d17c33f77e4fd2bb1d17c33f771202d4422e83cc087e4fd28a15c386789b6d4fffd90d0a2d2d626f756e64617279646f6e6f7463726f"
     "73730d0a436567657431360d0a436f6e74656e742d4c656e6774683a2034300d0a0d0affd8ffe000101d4a4649460001010604050605"
     "040606006400640000ffdb004300060405070706080a0d0a"},
    {"strict_json", "a text holding a character beyond the first plane, nested three deep or more",
     "5b5b225c75643833645c7564646135225d5d"},
    {"rtc_sdp", "an answer whose video was chosen with the parameter sets it carried, beside a sound address",
     "02763d300d0a613d66696e6765727072696e743a7368612d32353620410a6d3d766964656f2039205544502f544c532f5254502f5341"
     "5650462039360d0a613d6d69643a780a613d7274706d61703a393620483236342f39303030300a613d666d74703a3936207061636b65"
     "74697a6174696f6e2d6d6f64653d313b7370726f702d706172616d657465722d736574733d5a30672c614d610a613d63616e64696461"
     "74653a31203120756470203220392e382e312e3020342074797020686f73740a"},
    {"h264_rtp", "after a packet was lost, a key picture assembled from two or more packets of one time",
     "00001d80e00001000000010000007b7800056742e01faa000368ceaa0002001d8022000e000200000002000000e00002000000020000"
     "007b65aa000000020000007b4122000e80e00002000000020000007b65aa"},
    {"jetkvm", "an answer read through its base64 and JSON to a video chosen, and then a sound address",
     "00033b7b2274797065223a22616e73776572222c2264617461223a2265794a7a5a4841694f694a3250544263636c7875627a30744944"
     "51324d5445334d7a45304d4441304d7a41774e54457a4d7a59674d69424a5469424a554451674d5449334c6a41754d43347858484a63"
     "626e4d394c56787958473530505441674d4678795847356850576479623356774f6b4a56546b524d52534177494445674d6c78795847"
     "356850575a70626d646c636e4279615735304f6e4e6f595330794e5459674d4441364d5445364d6a49364d7a4d364e4451364e545536"
     "4e6a59364e7a63364f4467364f546b3651554536516b493651304d365245513652555536526b59364d4441364d5445364d6a49364d7a"
     "4d364e4451364e5455364e6a59364e7a63364f4467364f546b3651554536516b493651304d365245513652555536526b5a63636c7875"
     "625431326157526c62794135494656455543395554464d76556c52514c314e42566c4247494445774d6c78795847356a50556c4f4945"
     "6c514e4341774c6a41754d43347758484a63626d453962576c6b4f6a4263636c78755954317a5a57356b623235736556787958473568"
     "50584a306347316863446f784d444967534449324e4338354d4441774d4678795847356850575a74644841364d5441794947786c646d"
     "56734c57467a655731745a58527965533168624778766432566b505445376347466a613256306158706864476c76626931746232526c"
     "5054453761484a765a6d6c735a5331735a585a6c624331705a4431354d6d55774d575a63636c78755954316a5957356b615752686447"
     "55364e5341784948566b634341794d544d774e7a41324e444d7849474578596a4a6a4d3251304c6d787659324673494455774d444179"
     "494852356343426f62334e3058484a63626d30395958427762476c6a59585270623234674f534256524641765246524d557939545131"
     "52514948646c596e4a305979316b5958526859326868626d356c624678795847356a50556c4f49456c514e4341774c6a41754d433477"
     "58484a63626d453962576c6b4f6a4663636c78755954317a593352774c584276636e51364e5441774d467879584734694c434a306558"
     "426c496a6f695957357a64325679496e303d227d0000697b2274797065223a226e65772d6963652d63616e646964617465222c226461"
     "7461223a7b2263616e646964617465223a2263616e6469646174653a322031205544502032313232323630323233203232302e302e30"
     "2e352035343332312074797020686f7374227d7d"},
};

QByteArray heard;

void hear(const char* state)
{
    heard = state;
}

QByteArray bytesOf(const char* text)
{
    return {text};
}

} // namespace

class TestFuzzFound : public QObject {
    Q_OBJECT

private slots:
    void eachHarnessRunsHere_data()
    {
        QTest::addColumn<QByteArray>("harness");
        for (const char* name : harnesses) {
            QTest::newRow(name) << QByteArray(name);
        }
    }
    void eachHarnessRunsHere()
    {
        QFETCH(QByteArray, harness);
        const Harness read = harnessOf(harness);
        QVERIFY(read);
        read({});
        const std::uint8_t one[] = {0};
        read(one);
    }

    void whatWasFoundIsReadSafely_data()
    {
        QTest::addColumn<QByteArray>("harness");
        QTest::addColumn<QByteArray>("input");
        for (const Found& one : found) {
            QTest::newRow(one.what) << QByteArray(one.harness) << QByteArray::fromHex(one.hex);
        }
    }
    void whatWasFoundIsReadSafely()
    {
        QFETCH(QByteArray, harness);
        QFETCH(QByteArray, input);
        const Harness read = harnessOf(harness);
        QVERIFY(read);
        QVERIFY(!input.isEmpty());
        read({reinterpret_cast<const std::uint8_t*>(input.constData()), static_cast<std::size_t>(input.size())});
    }

    void eachCanaryIsReachedStill_data()
    {
        QTest::addColumn<QByteArray>("harness");
        QTest::addColumn<QByteArray>("state");
        QTest::addColumn<QByteArray>("input");
        for (const Reached& one : reached) {
            QTest::newRow(one.harness) << QByteArray(one.harness) << QByteArray(one.state)
                                       << QByteArray::fromHex(one.hex);
        }
    }
    void eachCanaryIsReachedStill()
    {
        QFETCH(QByteArray, harness);
        QFETCH(QByteArray, state);
        QFETCH(QByteArray, input);
        const Harness read = harnessOf(harness);
        QVERIFY(read);
        QVERIFY(!input.isEmpty());
        heard.clear();
        qkvm::fuzz::listener() = &hear;
        read({reinterpret_cast<const std::uint8_t*>(input.constData()), static_cast<std::size_t>(input.size())});
        qkvm::fuzz::listener() = nullptr;
        QCOMPARE(heard, state);
    }

    // Every harness's canary, by the target it is of: each is one of the rows above, and each row's input is read here.
    void everyHarnessHasItsCanaryRow()
    {
        for (const char* name : harnesses) {
            const bool there = std::ranges::any_of(reached, [name](const Reached& one) {
                return QByteArray(one.harness) == name && QByteArray(one.hex).size() > 0;
            });
            QVERIFY2(there, name);
        }
    }

    // The JetKVM's words as its connection and its driver read them (jetkvm_words.h, the seam its fuzz target reads
    // through): each kind of word read as before the seam, field for field.
    void theJetKvmsWordsAreReadAsItsConnectionReadThem()
    {
        namespace words = qkvm::core::jetkvm_words;
        using Signalling = words::Signalling;
        // Its signalling line: a refusal in its words before the answer only, the answer's data, an address.
        const Signalling refused = words::signalling(bytesOf("{\"error\": \"Another\\nsession\"}"), false);
        QCOMPARE(refused.kind, Signalling::Kind::Refused);
        QCOMPARE(refused.refusal, u"Another session"_s);
        QCOMPARE(words::signalling(bytesOf("{\"error\": \"Another\"}"), true).kind, Signalling::Kind::Nothing);
        for (const char* none : {"{\"error\": null}", "{\"error\": false}", "{\"error\": \"\"}", "pong", "[1]"}) {
            QCOMPARE(words::signalling(bytesOf(none), false).kind, Signalling::Kind::Nothing);
        }
        const Signalling answer = words::signalling(bytesOf("{\"type\": \"answer\", \"data\": \"e30=\"}"), false);
        QCOMPARE(answer.kind, Signalling::Kind::Answer);
        QCOMPARE(answer.data.text, u"e30="_s);
        QCOMPARE(words::signalling(bytesOf("{\"type\": \"answer\", \"data\": \"e30=\"}"), true).kind,
                 Signalling::Kind::Nothing);
        const Signalling address = words::signalling(
            bytesOf("{\"type\": \"new-ice-candidate\", \"data\": {\"candidate\": \"candidate:1 1 UDP 1 10.0.0.1 9 typ "
                    "host\", \"sdpMid\": \"0\"}}"),
            true);
        QCOMPARE(address.kind, Signalling::Kind::Address);
        QCOMPARE(address.address, QByteArray("candidate:1 1 UDP 1 10.0.0.1 9 typ host"));
        QCOMPARE(
            words::signalling(bytesOf("{\"type\": \"new-ice-candidate\", \"data\": {\"candidate\": 7}}"), true).kind,
            Signalling::Kind::Nothing);
        // Its answer's description: base64 of JSON whose `sdp` is a text, not empty.
        const QByteArray description = QByteArray("{\"type\": \"answer\", \"sdp\": \"v=0\"}").toBase64();
        qkvm::core::json::Value data;
        data.kind = qkvm::core::json::Value::Kind::Text;
        data.text = QString::fromLatin1(description);
        QCOMPARE(words::answerSdp(data), std::optional<QByteArray>("v=0"));
        data.text = u"not base64!"_s;
        QVERIFY(!words::answerSdp(data));
        data.text = QString::fromLatin1(QByteArray("{\"sdp\": \"\"}").toBase64());
        QVERIFY(!words::answerSdp(data));
        // On `rpc`: an answer to a whole number from 1, in its words where it refused; its events, clamped.
        using Rpc = words::Rpc;
        const Rpc result = words::rpc(bytesOf("{\"jsonrpc\": \"2.0\", \"id\": 3, \"result\": {\"ready\": true}}"));
        QCOMPARE(result.kind, Rpc::Kind::Answer);
        QCOMPARE(result.id, qint64{3});
        QVERIFY(!result.refused && result.result.isObject());
        const Rpc failed =
            words::rpc(bytesOf("{\"id\": 4, \"error\": {\"message\": \"Internal error\", \"data\": \"not ready\"}}"));
        QVERIFY(failed.refused);
        QCOMPARE(failed.words, u"not ready"_s);
        QCOMPARE(words::rpc(bytesOf("{\"id\": 5, \"error\": {\"message\": \"Internal error\"}}")).words,
                 u"Internal error"_s);
        for (const char* none : {"{\"id\": 0, \"result\": 1}", "{\"id\": 1.5, \"result\": 1}",
                                 "{\"id\": \"1\", \"result\": 1}", "{\"method\": 7}", "{\"jsonrpc\": \"2.0\"}"}) {
            QCOMPARE(words::rpc(bytesOf(none)).kind, Rpc::Kind::Nothing);
        }
        QCOMPARE(words::rpc(bytesOf("{\"id\": \"x\", \"method\": \"otherSessionConnected\"}")).kind,
                 Rpc::Kind::OtherSession);
        const Rpc video = words::rpc(bytesOf("{\"method\": \"videoInputState\", \"params\": {\"ready\": false, "
                                             "\"width\": 99999, \"height\": -4, \"fps\": 1e9}}"));
        QCOMPARE(video.kind, Rpc::Kind::Video);
        QCOMPARE(video.ready, std::optional<bool>(false));
        QCOMPARE(video.width, 16384);
        QCOMPARE(video.height, 0);
        QCOMPARE(video.fps, 1000.0);
        QCOMPARE(words::rpc(bytesOf("{\"method\": \"usbState\", \"params\": \"configured\"}")).words, u"configured"_s);
        QCOMPARE(words::rpc(bytesOf("{\"method\": \"atxState\", \"params\": {\"power\": true}}")).kind, Rpc::Kind::Atx);
        QCOMPARE(words::rpc(bytesOf("{\"method\": \"localVersion\", \"params\": {\"appVersion\": \"0.5.9\"}}")).words,
                 u"0.5.9"_s);
        const Rpc keys = words::rpc(
            bytesOf("{\"method\": \"keysDownState\", \"params\": {\"modifier\": 2, \"keys\": [4, 0, 300]}}"));
        QCOMPARE(keys.kind, Rpc::Kind::Keys);
        QCOMPARE(keys.modifiers, 2);
        QCOMPARE(keys.keys, (QList<int>{4, 255}));
        QCOMPARE(words::rpc(bytesOf("{\"method\": \"keysDownState\", \"params\": {\"keys\": [1,1,1,1,1,1,1,1,1,1,1,1,1,"
                                    "1,1,1,1]}}"))
                     .kind,
                 Rpc::Kind::Nothing);
        QVERIFY(words::otherSessionConnected(bytesOf("{\"method\": \"otherSessionConnected\"}")));
        QVERIFY(!words::otherSessionConnected(bytesOf("{\"method\": \"usbState\", \"x\": \"otherSessionConnected\"}")));
        // On `hidrpc`: its handshake, another version's, its keyboard.
        QCOMPARE(words::hid(QByteArray("\x01\x01", 2)).kind, words::Hid::Kind::Shaken);
        QCOMPARE(words::hid(QByteArray("\x01\x02", 2)).kind, words::Hid::Kind::OtherVersion);
        const words::Hid board = words::hid(QByteArray("\x33\x02\x04\x00\x05\x06\x07\x08\x09", 9));
        QCOMPARE(board.kind, words::Hid::Kind::Keys);
        QCOMPARE(board.modifiers, 2);
        QCOMPARE(board.keys, (QList<int>{4, 5, 6, 7, 8}));
        QCOMPARE(words::hid(QByteArray("\x01", 1)).kind, words::Hid::Kind::Nothing);
        QCOMPARE(words::hid({}).kind, words::Hid::Kind::Nothing);
        // To its driver: its pass, its wait, its number, whether it is set up, its version.
        const QByteArray pass = words::passFrom(QNetworkCookie::parseCookies("authToken=a0b1.c2; Path=/; HttpOnly"));
        QCOMPARE(pass, QByteArray("a0b1.c2"));
        QVERIFY(words::passFrom(QNetworkCookie::parseCookies("authToken=a\nauthToken=b")).isEmpty());
        QCOMPARE(words::comeBackAfterMs("30", 60'000), qint64{30'000});
        QCOMPARE(words::comeBackAfterMs("inf", 60'000), qint64{60'000});
        QCOMPARE(words::comeBackAfterMs("-1", 60'000), qint64{0});
        QCOMPARE(words::comeBackAfterMs("soon", 60'000), qint64{0});
        QVERIFY(words::numberFrom(bytesOf("{\"deviceId\": \"a1b2c3d4e5f6\"}")).startsWith(u"jetkvm:"_s));
        QVERIFY(words::numberFrom(bytesOf("{\"deviceId\": \"aaaaaaaaaaaa\"}")).isEmpty());
        QVERIFY(words::saysNotSetUp(bytesOf("{\"isSetup\": false}")));
        QVERIFY(!words::saysNotSetUp(bytesOf("{\"isSetup\": true}")));
        QVERIFY(!words::saysNotSetUp(bytesOf("{}")));
        QCOMPARE(words::metadataVersion(
                     bytesOf("{\"type\": \"device-metadata\", \"data\": {\"deviceVersion\": \"0.5.9\"}}")),
                 std::optional<QString>(u"0.5.9"_s));
        QVERIFY(!words::metadataVersion(bytesOf("{\"type\": \"other\"}")));
    }
};

QTEST_GUILESS_MAIN(TestFuzzFound)
#include "test_fuzz_found.moc"
