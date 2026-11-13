# License text provenance

- `AGPL-3.0.txt`: the project's existing `app/LICENSE`, copied verbatim.
- `LGPL-3.0.txt`: the approved FFmpeg distribution's `LICENSE.txt`, copied verbatim;
  the same LGPL version applies to the selected Qt modules.
- `GPL-3.0.txt`: the incorporated GPL terms from
  <https://www.gnu.org/licenses/gpl-3.0.txt>, retrieved 2026-10-22;
  SHA-256 `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`.
- `Feather-MIT.txt`: the icon project's original notice from
  <https://github.com/feathericons/feather/blob/main/LICENSE>, retrieved 2026-10-22.
- `Unicode-CLDR.txt`: Unicode's copyright and permission notice for CLDR's keyboard data, as the
  web's `app/qkvm/layouts.py` carries it (copied, with the tables, to `core/data/jetkvm_layouts.py`).
- `mingw-w64-runtime.txt`: the notice of the MinGW-w64 runtime that the project's own FFmpeg DLLs are
  statically linked against, as its licence asks of a binary: a few lines that say so, then, unchanged,
  the copyright file of the Ubuntu package it was taken from (mingw-w64-x86-64-dev 11.0.1-3build1,
  pinned by its SHA-256 in `tools/build_ffmpeg.ps1`), `usr/share/doc/mingw-w64-x86-64-dev/copyright`,
  SHA-256 `af23297b7d17e8e31817a5d58088de6fdafe34705bb5a7d1d330f08711b31314`.
- `Inno-Setup-license.txt`: the licence of Inno Setup, whose setup and uninstall program are inside the
  setup (`tools/build_setup.ps1`) and left by it in the program's folder: `license.txt` as Inno Setup 6.7.4
  unpacks it (`tools/fetch_inno.ps1`; SHA-256
  `2e5346868c2a18434489824e11d65c3031620f792fefc415d05f19cd441abf5c`, Windows line ends), the same text as
  <https://jrsoftware.org/files/is/license.txt> on 2026-11-09, kept here with Unix line ends.
- `RemObjects-Pascal-Script-license.md`: the licence of RemObjects Pascal Script, which runs the setup's own
  script inside Inno Setup's setup and uninstall program: `LICENSE.md` of
  <https://github.com/remobjects/pascalscript> on its default branch, as
  <https://raw.githubusercontent.com/remobjects/pascalscript/master/LICENSE.md> gave it on 2026-11-09 (git blob
  `3d899f86ead380c10eef85710794e2ad2a5adeab`; SHA-256 `a08799bb2385a3af99a9bbb7c746d0ad9ac7dd07be0fb1473f2f4d1dc3e11bd1`,
  Windows line ends), kept here with Unix line ends. Which commit of it Inno Setup 6.7.4 is built with is not
  told by its package; the default branch's is taken. Its third clause asks for a visible line in the program's
  about box or documentation that it is made using RemObjects Pascal Script and where that can be found:
  `THIRD-PARTY.md` has that line, and the setup's own About box (Inno Setup's) names it too.
- `Qt-bundled-parts.md`: the parts of other projects inside the Qt files that ship, with their
  licences, versions, copyright lines and sources, made from Qt 6.12.0's own SPDX bills of materials
  (the `qt-sbom` files the package carries) on 2026-11-06, for the Qt libraries and plugins
  `tools/package.ps1` deploys.

The packaging script also copies the installed Qt software bills of materials and
the pinned native transport's original license/notice files. The component inventory
and corresponding-source locations are in `THIRD-PARTY.md`. A development package
is not a completed public-release license/source audit.
