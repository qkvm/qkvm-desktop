# What ships with QKVM Desktop

Every library that is handed out with the program, its licence, and where its source is offered.
The About box shows the same, read from the libraries themselves. No dependency application code is copied into the program's own source. Qt, FFmpeg and
libdatachannel are DLLs; the WebRTC support libraries below are linked into datachannel.dll.

| Library | Version | Licence | Used for | Source |
|---|---|---|---|---|
| Qt (Core, Gui, Network, OpenGL, Qml, QmlMeta, QmlModels, QmlWorkerScript, Quick, Quick Layouts, Quick Shapes, Quick Templates, Svg, and their plugins below) | 6.12.0, open-source edition | LGPL-3.0-only; the parts of other projects inside them under their own licences (`licenses/Qt-bundled-parts.md`) | Everything that is seen, and the calls to the server | https://download.qt.io/archive/qt/6.12/6.12.0/ |
| The icons, which are the page's: the Feather set, written into `ui/qml/controls/Icon.qml` as paths | as the page has them (`app/qkvm/static/app.js`) | MIT | Every small drawing on a button or a card | https://github.com/feathericons/feather |
| FFmpeg (avcodec and avutil), the project's own build: the H.264, JPEG and PNG decoders, the H.264 parser and Direct3D 11 decoding, nothing else | 9.0.2 | LGPL-3.0-or-later (built with `--enable-version3`, without GPL or non-free parts, linking no outside library; its notice: `licenses/FFmpeg-LGPL-3.0.txt`) | Decoding H.264, JPEG and PNG | https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz; the same archive, and how it is built (`tools/build_ffmpeg.ps1`), with every release's third-party sources |
| zlib, inside avcodec (for the PNG decoder) | 1.3.2 | Zlib (its notice: `licenses/zlib.txt`) | Unpacking PNG pictures | https://github.com/madler/zlib/releases/tag/v1.3.2; also with every release's third-party sources |
| The MinGW-w64 runtime, inside avcodec and avutil: the start-up and support code the MinGW-w64 compiler links into every library it makes | 11.0.1, as Ubuntu 24.04 packages it (mingw-w64-x86-64-dev 11.0.1-3build1) | ZPL-2.1, and the licences of its other parts (its notice, with every part's: `licenses/mingw-w64-runtime.txt`) | Starting and running the FFmpeg DLLs | https://www.mingw-w64.org/; https://sourceforge.net/projects/mingw-w64/ |
| libgcc, GCC's support library, inside avcodec and avutil (linked in with `-static-libgcc`) | GCC 13.2.0, as Ubuntu 24.04 packages it (gcc-mingw-w64-x86-64-win32 13.2.0-6ubuntu1+26.1) | GPL-3.0-or-later with the GCC Runtime Library Exception 3.1, under which what GCC compiles may be handed out on terms of its maker's choosing: it asks for no notice | The compiler's own helpers, such as those of arithmetic and of the stack | https://gcc.gnu.org/ |
| Unicode CLDR's keyboard data (the layouts a JetKVM's computer may use), as the web's `app/qkvm/layouts.py` has it, copied to `core/data/jetkvm_layouts.py` and made into tables at build time | release 43 | Unicode License (its notice: `licenses/Unicode-CLDR.txt`) | Typing a text on a JetKVM in its computer's keyboard layout | https://github.com/unicode-org/cldr (keyboards/windows, keyboards/chromeos) |
| Microsoft Visual C++ x64 runtime | From the installed Visual Studio 2022 redistributable directory; exact bytes in the package manifest | Microsoft redistributable software terms | Running the MSVC-built application and Qt | https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution |
| Inno Setup's setup and uninstall program: inside the setup (`qKVM-Desktop-<version>-windows-x64-setup.exe`), and left by it in the program's folder as `unins000.exe`; not in the portable ZIP | 6.7.4 | Inno Setup License (its notice: `licenses/Inno-Setup-license.txt`). It carries zlib's decompressor (Zlib, `licenses/zlib.txt`) and the LZMA SDK's (public domain), and RemObjects Pascal Script (below) | Installing, upgrading and uninstalling the program | https://jrsoftware.org/isinfo.php; https://github.com/jrsoftware/issrc/tree/is-6_7_4 |
| RemObjects Pascal Script, inside Inno Setup's setup and uninstall program: the setup and its uninstaller are made using RemObjects Pascal Script, which can be found at https://github.com/remobjects/pascalscript | as Inno Setup 6.7.4 carries it | Its own licence (zlib-like; its notice: `licenses/RemObjects-Pascal-Script-license.md`) | Running the setup's own script (the update mode's wait) | https://github.com/remobjects/pascalscript |

Which builds these are, to the byte, is written down in [tools/fetch_qt.ps1](tools/fetch_qt.ps1),
[tools/fetch_inno.ps1](tools/fetch_inno.ps1) and
[tools/build_ffmpeg.ps1](tools/build_ffmpeg.ps1): each archive with its size and SHA-256 (the compiler
FFmpeg is built with too, as Ubuntu packages it; it is a tool and does not ship, but for the runtime and
libgcc it links into the two DLLs, above). A development build may
use the FFmpeg of [tools/fetch_ffmpeg.ps1](tools/fetch_ffmpeg.ps1) instead, which also carries
`swresample-7.dll`; a release never does (`tools/audit_release.ps1 -Beta` refuses it).

## Every library file in the package

`tools/audit_release.ps1` refuses a package with a library whose file name is not in this list, one
with a runtime linked into a library that the table above does not name or whose notice is not in
`licenses/`, and one with a library that imports another that neither ships nor is Windows' own.

| Library | Its files |
|---|---|
| Qt | `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Network.dll`, `Qt6OpenGL.dll` (Qt Quick links it, though nothing is drawn with OpenGL), `Qt6Qml.dll`, `Qt6QmlMeta.dll`, `Qt6QmlModels.dll`, `Qt6QmlWorkerScript.dll`, `Qt6Quick.dll`, `Qt6QuickLayouts.dll`, `Qt6QuickShapes.dll`, `Qt6QuickTemplates2.dll`, `Qt6Svg.dll` |
| Qt's plugins | `plugins/platforms/qwindows.dll`, `plugins/tls/qschannelbackend.dll` (TLS by Windows' own Schannel), `plugins/networkinformation/qnetworklistmanager.dll`, `plugins/iconengines/qsvgicon.dll`, `plugins/imageformats/qgif.dll`, `qico.dll`, `qjpeg.dll`, `qsvg.dll` |
| Qt's QML modules | `qml/QtQml/qmlplugin.dll`, `qml/QtQml/Models/modelsplugin.dll`, `qml/QtQml/WorkerScript/workerscriptplugin.dll`, `qml/QtQuick/qtquick2plugin.dll`, `qml/QtQuick/Layouts/qquicklayoutsplugin.dll`, `qml/QtQuick/Shapes/qmlshapesplugin.dll`, `qml/QtQuick/Templates/qtquicktemplates2plugin.dll` |
| FFmpeg, with zlib inside avcodec, and the MinGW-w64 runtime and libgcc inside both | `avcodec-63.dll`, `avutil-61.dll` (and `swresample-7.dll` in a development build only) |
| libdatachannel, with libjuice, libsrtp, usrsctp, plog and Mbed TLS linked in | `datachannel.dll` |
| Microsoft Visual C++ runtime | `concrt140.dll`, `msvcp140.dll`, `msvcp140_1.dll`, `msvcp140_2.dll`, `msvcp140_atomic_wait.dll`, `msvcp140_codecvt_ids.dll`, `vccorlib140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`, `vcruntime140_threads.dll` (Microsoft's redistributable files, signed by Microsoft, unchanged) |

No Direct3D file ships. The program draws with Direct3D 11 alone (`ui/main.cpp`): the shader compiler
Qt may ask for, `d3dcompiler_47.dll`, is part of Windows since Windows 8.1, and Direct3D 12's
`dxcompiler.dll` and `dxil.dll` are never loaded. `tools/package.ps1` tells Qt's deployment tool to
leave all three out.

The parts of other projects that Qt carries inside these files (PCRE2, zlib, FreeType, HarfBuzz,
libpng, libjpeg-turbo, double-conversion, md4c and others), each with its licence, version, copyright
lines and source, are in [licenses/Qt-bundled-parts.md](licenses/Qt-bundled-parts.md), made from Qt's
own bills of materials, which ship in `licenses/qt-sbom/`. Portions of this software are copyright (C)
The FreeType Project (https://freetype.org). All rights reserved. This software is based in part on
the work of the Independent JPEG Group.

The icons are the one thing here that is inside the program rather than beside it. They are
drawings, not code, under a licence that asks for nothing but its own text to be passed on; the
page ships the same ones. Whether to keep them or to have a set drawn for QKVM is the owner's to
say (README.md: no code is copied from another project).

Only modules of Qt that are offered under the LGPL are used. Qt has modules that are not (Qt Shader
Tools, Qt Charts and others): look a module up in Qt's list of licences before adding it.

What a release carries besides:

* the project's own FFmpeg (above), built with zlib inside it for the PNG decoder (Qt carries a zlib
  of its own, in its own files); its source, zlib's, and those of the WebRTC stack, as built, are a
  file of every release (`tools/release_files.ps1`);
* the licence texts: the application's, LGPL/GPL, the icons' and the native transport's notices,
  zlib's, the MinGW-w64 runtime's, Qt's component inventories and their parts, and the app-local Microsoft runtime from
  `VC/Redist/MSVC`, never compiler or debug DLLs;
* of Qt's plugins for TLS only the one for Windows' own (Schannel). The program uses no other, and
  OpenSSL is not among what ships.

Used to build and to test, and not handed out: Microsoft's compiler and Windows SDK, CMake, Ninja,
clang-format and clang-tidy, Qt Test and Qt WebSockets (test stand-ins only), and Inno Setup's compiler,
which makes the setup: unpacked by its own installer in its portable mode, run at low integrity, never
installed (`tools/fetch_inno.ps1`).

The standalone `qkvm-setup.exe` uses Windows SDK APIs and C++/WinRT headers, with
the same approved Microsoft C/C++ runtime linked statically so recovery does not
depend on Qt or a replaced runtime directory. It adds no downloaded dependency.
The application continues to use app-local redistributable runtime DLLs. Both
forms remain subject to Microsoft's redistribution terms and the release audit.

`tools/package.ps1` creates the self-contained folder and ZIP. The Microsoft runtime files keep
Microsoft's own signatures and are listed by hash in the manifest. Microsoft's
[deployment guidance](https://learn.microsoft.com/en-us/cpp/windows/deployment-in-visual-cpp?view=msvc-170)
describes app-local runtime deployment; updating those runtime files is the app maintainer's
responsibility.

`tools/audit_release.ps1` records every release gate of a package and whether it is open, and with
`-Beta` the owner's reason for each one accepted open for a beta; the notices and FFmpeg gates are
never accepted open. The automated audit is not a legal compliance certification or a public-release
approval.

## Approved native receiver

The owner approved a local WebRTC dependency trial on 2026-10-19. On 2026-10-20 the owner selected the C++ option for production integration. The C/C++ components below now supply `datachannel.dll` beside the desktop executable, using the same pinned local build under `.deps/webrtc-trial`. Exact pins, hashes and build settings are recorded in [the experiment](experiments/webrtc/README.md).

| Component | License declaration | Purpose |
|---|---|---|
| libdatachannel 0.24.6 and its pinned libjuice | MPL-2.0 | C++ WebRTC and ICE |
| Pinned libsrtp and usrsctp | BSD license texts in their source roots | SRTP and SCTP |
| Pinned plog | MIT | Transport logging |
| Mbed TLS 3.6.7 | Apache-2.0 OR GPL-2.0-or-later; Apache-2.0 option selected | DTLS, with DTLS-SRTP enabled |

Retain each source distribution's notices, including bundled components such as Mbed TLS's Everest and p256-m. The pinned corresponding sources and notices are retained by the reproducible fetch script. Before distribution, M4 must package the original license/notice files and offer the exact corresponding sources, including any modifications. Mbed TLS 3.6 support and migration are release-maintenance gates; the Rust compiler and crate graph do not ship with the application.

Corresponding-source locations: [libdatachannel](https://github.com/paullouisageneau/libdatachannel/tree/6b1e2e620f1e37f0eafeee702eaea0043cb305fd), [libjuice](https://github.com/paullouisageneau/libjuice/tree/b89c792e3612faf2f12cf35bcc56857313a06be3), [libsrtp](https://github.com/cisco/libsrtp/tree/24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5), [usrsctp](https://github.com/paullouisageneau/usrsctp/tree/fec583d54493f879d2ae44a743423bf8a04371ab), [plog](https://github.com/SergiusTheBest/plog/tree/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef), and [Mbed TLS](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7). The original source licenses govern each component; this inventory does not replace their texts.
