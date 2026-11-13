"""A QKVM with as many made-up KVMs as are asked for, to look at a large wall and to measure it.

    cd ../app
    python ../desktop/tools/stand_in_wall.py 64

It is the app's own stand-in (python -m tests.demo) with another wall: the same made-up KVMs
the app's tests use, each with a picture of its own colour, in groups of eight, and the same
password to sign in with (DEMO_PASSWORD in the app's tests/demo.py). On this PC alone, at port
8789; what it knows is in a folder that is thrown away when it ends.

    --port N             another port than 8789
    --jpeg FILE          every KVM sends this JPEG as its picture, as a KVM sends JPEGs, in place
                         of the made-up ones' own pictures, which are PNGs: a PNG costs whoever
                         shows it far more memory, and a wall measured with them looks worse
                         than a real one is
    --away AFTER,FOR     the first KVM stops answering AFTER seconds from the start, for FOR
                         seconds: to see what the program says of a machine that goes away
"""

from __future__ import annotations

import argparse
import colorsys
import os
import sys
import tempfile
import threading
import time
from pathlib import Path

# The app's own package and its tests: this is run from the app's folder, as its stand-in is.
sys.path.insert(0, os.getcwd())

import uvicorn  # noqa: E402

from qkvm.core import Core  # noqa: E402
from qkvm.server import create_app  # noqa: E402
from qkvm.store import DeviceRecord, UserRecord  # noqa: E402
from tests.demo import DEMO_PASSWORD, KVM_PASSWORD  # noqa: E402
from tests.fake_kvm import FakeKvm, FakeState  # noqa: E402


def colour_of(number: int, of: int) -> tuple[int, int, int]:
    """A colour for each KVM, round the wheel, so that a card is told from its neighbour."""
    red, green, blue = colorsys.hsv_to_rgb(number / max(1, of), 0.55, 0.6)
    return int(red * 255), int(green * 255), int(blue * 255)


def main() -> None:
    asked = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    asked.add_argument("machines", type=int, nargs="?", default=64)
    asked.add_argument("--port", type=int, default=8789)
    asked.add_argument("--away", default="")
    asked.add_argument("--jpeg", default="")
    said = asked.parse_args()
    if not 1 <= said.machines <= 256:
        sys.exit("Between 1 and 256 machines.")
    if said.port in (8787, 8788):
        sys.exit("That port is a QKVM's own, or the app's stand-in's: another one.")

    picture = Path(said.jpeg).read_bytes() if said.jpeg else b""
    fakes = [
        FakeKvm(
            FakeState(
                colour=colour_of(number, said.machines),
                picture=picture,
                picture_type="image/jpeg" if picture else "image/png",
            )
        ).start()
        for number in range(said.machines)
    ]

    if said.away:
        after, lasting = (float(part) for part in said.away.split(","))

        def go_away() -> None:
            time.sleep(after)
            fakes[0].state.stall = lasting
            time.sleep(lasting)
            fakes[0].state.stall = 0.0

        threading.Thread(target=go_away, daemon=True).start()

    with tempfile.TemporaryDirectory() as folder:
        core = Core(Path(folder))
        core.store.users = [UserRecord(id="demo", name="admin", role="admin", auth=core.vault.create(DEMO_PASSWORD))]
        for number, fake in enumerate(fakes):
            record = DeviceRecord(
                id=f"wall{number}",
                name=f"Machine {number + 1}",
                url=fake.url,
                kind="kvmd",
                group=f"Rack {number // 8 + 1}",
            )
            record.secret = core.vault.encrypt(KVM_PASSWORD, record.id)
            core.store.devices.append(record)
        core.store.save()
        core.vault.lock()
        print(f"A wall of {said.machines}: http://127.0.0.1:{said.port}/", flush=True)
        uvicorn.run(create_app(core), host="127.0.0.1", port=said.port, log_level="warning", access_log=False)


if __name__ == "__main__":
    main()
