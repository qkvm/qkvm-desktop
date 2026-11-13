# Native WebRTC: libdatachannel and Mbed TLS

The program's native WebRTC stack, `datachannel.dll`, is built here from pinned sources:
libdatachannel 0.24.6 with libjuice, libsrtp, usrsctp and plog, over Mbed TLS 3.6.7.
`tools\fetch_webrtc.ps1` runs the two scripts below; nothing is installed for the whole PC.

```powershell
.\experiments\webrtc\fetch-cpp-dependencies.ps1
.\experiments\webrtc\build-cpp-dependencies.ps1
.\experiments\webrtc\run-cpp.ps1
```

Run them with PowerShell 7 from the `desktop` folder, with the compiler, Qt and FFmpeg of the
README's *Building* in place. They write the sources, the build and its logs under
`.deps/webrtc-trial`, which git does not keep.

- [dependencies.json](dependencies.json) records the exact source pins, the official archive
  hashes and the build choices. The checkout keeps upstream's dependency links and notices.
- Mbed TLS's default configuration does not enable the DTLS-SRTP extension libdatachannel's
  media needs: the scripts define `MBEDTLS_SSL_DTLS_SRTP` alike for the TLS build and the
  transport. C++ exceptions stay on (`/EHsc`).
- `run-cpp.ps1` runs [cpp/](cpp/README.md), a standalone trial of the stack: two peers bound to
  127.0.0.1, with no ICE servers, send a generated H.264 clip through the library's H.264
  packetizer, DTLS/SRTP and depacketizer, and the received pictures are decoded and compared
  with the reference. `cpp/make_fixture.ps1` makes the clip with the FFmpeg the program uses;
  it refuses to write over a file, and reads no screen or media of anybody's.

The library is used through its C interface across the DLL boundary, so the release DLL also
serves the debug and sanitizer builds of the program.
