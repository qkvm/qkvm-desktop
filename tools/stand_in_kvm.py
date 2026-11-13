"""One made-up KVM by itself, with no QKVM in front of it: to try the program's own way of speaking
to a KVM (SPEC.md section 6.3) against something that speaks as a real one does.

    cd ../app
    python ../desktop/tools/stand_in_kvm.py

It is the made-up KVM of the app's own tests (tests/fake_kvm.py): what kvmd answers, from a real
web server, with lines that are kept and a WebSocket. On this PC alone, at port 8792. It says its
address, and where it has a certificate, that certificate's SHA-256, which is what the program
knows a KVM by. Its password is the one the app's tests give it (FakeState.password): a test
value, for this stand-in and nothing else.

    --port N        another port than 8792
    --tls FOLDER    over HTTPS, with a certificate as these KVMs make their own: named
                    "localhost" and "good" from 1970 to 1979. Made in FOLDER and kept there, so
                    that a KVM that is started again is still that KVM
    --rests         its video rests until somebody watches, as kvmd's does
    --atx           it has a power board
    --ports N       N computers behind it, the last with nothing on it
    --no-signal     the computer sends no picture
"""

from __future__ import annotations

import argparse
import hashlib
import os
import ssl
import sys
import time
from pathlib import Path

# The app's own tests: this is run from the app's folder, as its stand-ins are.
sys.path.insert(0, os.getcwd())

from tests.fake_kvm import FakeKvm, FakeState  # noqa: E402


def main() -> None:
    asked = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    asked.add_argument("--port", type=int, default=8792)
    asked.add_argument("--tls", default="")
    asked.add_argument("--rests", action="store_true")
    asked.add_argument("--atx", action="store_true")
    asked.add_argument("--ports", type=int, default=0)
    asked.add_argument("--no-signal", action="store_true")
    said = asked.parse_args()
    if said.port in (8787, 8788):
        sys.exit("That port is a QKVM's own, or the app's stand-in's: another one.")
    if not 0 <= said.ports <= 16:
        sys.exit("Between 0 and 16 computers.")

    state = FakeState(idle_until_viewer=said.rests, atx=said.atx, signal=not said.no_signal)
    if said.ports:
        state.ports = [(32 + 40 * (number % 5), 74, 135) for number in range(said.ports - 1)] + [None]
    kvm = FakeKvm(state, port=said.port, tls_dir=Path(said.tls) if said.tls else None).start()
    print(f"A made-up KVM is at {kvm.url}", flush=True)
    if said.tls:
        pem = (Path(said.tls) / "cert.pem").read_text(encoding="ascii")
        print("certificate SHA-256: " + hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).hexdigest(), flush=True)
    try:
        while True:
            time.sleep(1)
            # What it was asked to do, as it comes: to see that a key or a button arrived, and once.
            while state.actions:
                print("did: " + state.actions.pop(0), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        kvm.stop()


if __name__ == "__main__":
    main()
