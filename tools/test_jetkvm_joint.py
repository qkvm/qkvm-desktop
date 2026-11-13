"""The program's own JetKVM driver against another implementation of a JetKVM: the web app's own fake (aiortc).

    python -B tools/test_jetkvm_joint.py --web-app <the app's folder> --rig <qkvm-jetkvm-rig.exe> --output <folder>

M6 2i (plan-increment2.md section 3.2): the web's FakeJetKvm (tests/fake_jetkvm.py, which follows the device's
firmware) on 127.0.0.1 only, and the program's qkvm-jetkvm-rig against it: what it is, how it is, a card's picture (an
H.264 key frame with its SPS), its picture live, how it is over its connection, its one viewer taken by another
(the fake's hand_over, as when another viewer's offer comes) and the program stepping aside ("in_use", a look says
"busy"), and "Show here" taking it back. The fake is libdatachannel's other side here (aiortc), as the real one is
pion: what the rig and the fake each say is kept in the output folder.

Runs in a Python that has the web app's test requirements (the scratch environment), outside the gate, as
test_web_compat.py does; installs nothing, and writes nothing into the web app's folder (-B: no bytecode).
Ends with 0 where every check held.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import threading
import time


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--web-app", type=Path, required=True)
    parser.add_argument("--rig", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--live-seconds", type=int, default=5)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.web_app.resolve()))
    from tests.fake_jetkvm import FakeJetKvm, JetState

    state = JetState(password="joint-secret")
    fake = FakeJetKvm(state).start()
    lines: list[str] = []
    checks: list[tuple[str, bool]] = []
    try:
        if not fake.url.startswith("http://127.0.0.1:"):
            raise SystemExit(f"the fake is not on this PC's loopback: {fake.url}")
        environment = dict(os.environ, QKVM_RIG_KVM_PASSWORD="joint-secret")
        environment.pop("QKVM_RIG_NAMED_BY_THE_OWNER", None)
        rig = subprocess.Popen(
            [str(args.rig), fake.url, "--cards", "2", "--live", str(args.live_seconds), "--hand-over", "--resume"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
        sessions_at_hand_over = -1

        def read() -> None:
            nonlocal sessions_at_hand_over
            assert rig.stdout is not None
            for line in rig.stdout:
                line = line.rstrip("\n")
                lines.append(line)
                print(line, flush=True)
                if line == "waiting for another viewer":
                    # Another viewer takes the JetKVM now: told over the rig's connection, which is closed a second later.
                    sessions_at_hand_over = state.sessions
                    time.sleep(0.5)
                    fake.hand_over()

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        try:
            code = rig.wait(timeout=180)
        except subprocess.TimeoutExpired:
            rig.kill()
            code = -1
        reader.join(timeout=5)

        def said(start: str) -> list[str]:
            return [line for line in lines if line.startswith(start)]

        checks += [
            ("the rig ended well", code == 0),
            ("what it is: its version, as the fake says it", any("version 0.5.9" in line for line in said("what it is:"))),
            ("its cards' pictures are H.264 key frames with their SPS",
             len([line for line in said("a card's picture:") if "an H.264 key frame" in line]) == 2),
            ("its picture came live, key frames among them",
             any(" pictures (" in line and not line.startswith("live for 0") for line in said("live for "))),
            ("how it is while live: online, the fake's size, over its connection",
             any("online" in line and "640x360" in line for line in said("how it is (while live"))),
            ("another viewer took it: its window was told in_use", said("its window was told:") == ["its window was told: in_use"]),
            # (The rig's line where it was so, and not its "a picture (it should be in_use)": the reading of e40a02f, L6.)
            ("a card after that is in_use",
             [line.startswith("a card after it: in_use: ") for line in said("a card after it:")[:1]] == [True]),
            ("a look after that says busy", any(": busy" in line for line in said("how it is (stepped aside)"))),
            ("\"Show here\" took it back", bool(said("taken back"))),
            ("a card's picture came after it", any("a picture," in line for line in said("a card after it:")[1:])),
            ("one sign-in for all of it", state.logins == 1),
            ("the fake made a new connection for \"Show here\", and none while aside",
             sessions_at_hand_over >= 1 and state.sessions == sessions_at_hand_over + 1),
            ("every offer was video it only receives, H.264, and nothing else",
             bool(state.video_offers) and all("H264/90000" in sdp and "a=recvonly" in sdp and "VP8" not in sdp
                                              for sdp in state.video_offers)),
        ]
    finally:
        fake.stop()
    report = ["# The JetKVM joint test (M6 2i)", "", f"fake: the web's FakeJetKvm (aiortc), {fake.url}", ""]
    report += [f"- {'held' if held else 'DID NOT HOLD'}: {name}" for name, held in checks]
    report += ["", "## What the rig said", "", "```", *lines, "```", ""]
    (output / "report.md").write_text("\n".join(report), encoding="utf-8")
    for name, held in checks:
        print(f"{'held' if held else 'DID NOT HOLD'}: {name}")
    return 0 if checks and all(held for _, held in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
