# C++ local WebRTC/H.264 trial

This standalone experiment tests the approved libdatachannel + Mbed TLS candidate without changing the desktop's build or device drivers. `main.cpp` is original project code built against published APIs. It does not incorporate upstream examples. No dependencies are downloaded by this CMake project.

## What a successful run demonstrates

1. Parse a generated raw H.264 clip with the existing FFmpeg parser and decode reference frames with its H.264 software decoder.
2. Create two libdatachannel peers explicitly bound to **127.0.0.1**, with no ICE servers, no ICE TCP, and fingerprint verification enabled.
3. Gather candidates, reject any nonnumeric/non-loopback/non-UDP/non-host candidate, then exchange complete SDP **in process memory**. No signaling listener, DNS discovery, multicast, STUN server, TURN server, or device address is used.
4. Check and print each peer's selected candidate pair, including the actual loopback UDP ports.
5. Send one real H.264 access unit per RTP timestamp through the library's H.264 packetizer, DTLS/SRTP transport, and H.264 depacketizer. The deliberately small 600-byte fragmentation limit exercises fragmented NAL delivery with the generated moving clip.
6. Compare each received access unit's NAL payloads exactly, allowing only Annex B start-code normalization, and verify RTP timestamps/order.
7. Decode received units using the existing FFmpeg libraries and compare dimensions, pixel format, and checksums of active decoded image bytes with the reference decode.
8. Repeat connection/transfer/close cycles and immediately close another connection after starting gathering. Callbacks hold weak references to their cycle's bounded inbox, never a session or raw object pointer; closing first marks that inbox inactive. Close and global cleanup have bounded waits.

The default is five 30-frame cycles plus immediate close after requesting gathering. It is a local transport/decoder experiment: there is no device signaling, HID, audio, viewer window, D3D11 rendering, GPU decode, production-engine integration, loss injection, browser interoperability test, or long-run leak proof. The `elapsed_ms` output covers the complete trial cycle, including deliberate sending delays; it is **not video latency**. Callback ownership and repeated close testing do not replace sanitizer or prolonged lifecycle testing.

If binding loopback produces no candidates, the trial fails. Do not relax it to `0.0.0.0` or forward LAN candidates to make it pass. Only the libjuice backend implements the selected `bindAddress` configuration. Dependency construction must select libjuice, as the approved proposal does.

## Windows build and invocation

Run from an MSVC developer shell with CMake/Ninja already available. The dependency coordinator builds pinned libdatachannel 0.24.6 with media enabled, libjuice, and Mbed TLS 3.6.7, then installs it into a trial prefix. The existing FFmpeg SDK needs its headers, `avcodec.lib`, `avutil.lib`, and corresponding DLLs. The dependency install prefix and FFmpeg `bin` must be on the launched process's PATH (plus any separately installed runtime dependency DLL folders). No DLL installation into Windows is required.

Example paths below are confined to the ignored trial dependency/build area; adapt `install` to the actual chosen prefix:

Run from the desktop directory after entering its existing build environment.

```powershell
$ffmpegRoot = Join-Path (Get-Location) '.deps/ffmpeg'
$trialRoot = Join-Path (Get-Location) '.deps/webrtc-trial'
$trialPrefix = Join-Path $trialRoot 'install'
$trialBuild = Join-Path $trialRoot 'cpp-build'
$trialFixture = Join-Path $trialRoot 'moving-320x180.h264'

& ./experiments/webrtc/cpp/make_fixture.ps1 `
    -Ffmpeg (Join-Path $ffmpegRoot 'bin\ffmpeg.exe') -Output $trialFixture
cmake -S experiments/webrtc/cpp -B $trialBuild -G Ninja `
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$trialPrefix" `
    "-DQKVM_FFMPEG_DIR=$ffmpegRoot" "-DQKVM_TRIAL_FIXTURE=$trialFixture"
cmake --build $trialBuild
$env:PATH = "$trialPrefix\bin;$ffmpegRoot\bin;$env:PATH"
ctest --test-dir $trialBuild --output-on-failure
```

CTest enforces an overall 180-second deadline. The fixture generator refuses to overwrite an existing file and reads no real screen or user media. For individual runs:

```powershell
& "$trialBuild\qkvm_webrtc_cpp_trial.exe" $trialFixture 5 30
```

The optional numbers are cycle count (1 to 20) and access units per cycle (1 to 120). The generated clip contains 60 frames. Use CTest or another process timeout for manual runs too. A failure exits nonzero; successful output includes the selected candidate pairs, matched access-unit/frame counts, closure results, and `result=PASS`. Keep the full build/runtime logs with the dependency manifest and comparison report. Source presence alone is not a passing trial result.

## API references consulted

- [libdatachannel 0.24.6 configuration](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.6/include/rtc/configuration.hpp): loopback binding, ICE settings, and negotiation control.
- [Peer connection API](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.6/include/rtc/peerconnection.hpp): descriptions, selected candidates, and lifecycle.
- [Track API](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.6/include/rtc/track.hpp), [H.264 packetizer](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.6/include/rtc/h264rtppacketizer.hpp), and [H.264 depacketizer](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.6/include/rtc/h264rtpdepacketizer.hpp).
