"""A QKVM whose live connection (SPEC.md section 6.2, draft 2) sends a film of a known size and pace
in place of a KVM's own pictures: to look at one machine live in the program, and to measure how
smoothly the program shows it (bench/live_measure.h), with nothing real behind it.

    cd ../app
    python ../desktop/tools/stand_in_live.py ../desktop/.testdata/desktop-1080p30.h264

It is the app's own stand-in (python -m tests.demo) with a small wall of made-up KVMs, the same
password to sign in with (DEMO_PASSWORD in the app's tests/demo.py), and the two calls of the
draft in place of the app's own: its wall says that it has live pictures, POST /api/v1/devices/{id}/live gives a pass,
and a WebSocket at the same place takes it and sends pictures. Every machine sends the same film, round and round: an H.264 file with
each picture behind a delimiter, as tools\\make_testdata.ps1 makes them, at the size and pace
its name says (desktop-1080p30.h264). Keyboard and mouse are given to whoever asks first, and
what is then sent is counted, not kept. On this PC alone, at port 8790; what it knows is in a
folder that is thrown away when it ends.

    --port N             another port than 8790
    --machines N         how many made-up KVMs (4)
    --stills FOLDER      the JPEGs of a folder in place of a film, as a server's own screen
                         is sent: each a whole picture
    --fps N              pictures a second, where the film's name does not say, and for stills (2)
    --no-live ID         this machine has no live picture: the server says so in its own words
    --drop-every N       every Nth picture is not sent, as on a line that loses some: the
                         program then shows nothing until a key frame, and asks for one
"""

from __future__ import annotations

import argparse
import asyncio
import colorsys
import json
import os
import re
import secrets
import struct
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

# The app's own package and its tests: this is run from the app's folder, as its stand-in is.
sys.path.insert(0, os.getcwd())

import uvicorn  # noqa: E402
from fastapi import WebSocket  # noqa: E402
from fastapi.responses import JSONResponse  # noqa: E402

from qkvm.core import Core  # noqa: E402
from qkvm.server import create_app  # noqa: E402
from qkvm.store import DeviceRecord, UserRecord  # noqa: E402
from tests.demo import DEMO_PASSWORD, KVM_PASSWORD  # noqa: E402
from tests.fake_kvm import FakeKvm, FakeState  # noqa: E402

FORM = 2  # the form of the live connection this speaks
PASS_LASTS = 30.0  # seconds


def pictures_of(film: bytes) -> list[tuple[bool, bytes]]:
    """The film's pictures, each from its delimiter to the next, and whether it is a key frame."""
    starts = []
    at = film.find(b"\x00\x00\x01\x09")
    while at >= 0:
        starts.append(at - 1 if at > 0 and film[at - 1] == 0 else at)
        at = film.find(b"\x00\x00\x01\x09", at + 4)
    pictures = []
    for number, begin in enumerate(starts):
        unit = film[begin : starts[number + 1] if number + 1 < len(starts) else len(film)]
        key, at = False, unit.find(b"\x00\x00\x01")
        while at >= 0 and not key:
            key = at + 3 < len(unit) and unit[at + 3] & 0x1F == 5
            at = unit.find(b"\x00\x00\x01", at + 3)
        pictures.append((key, unit))
    return pictures


def main() -> None:
    asked = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    asked.add_argument("film", nargs="?", default="")
    asked.add_argument("--port", type=int, default=8790)
    asked.add_argument("--machines", type=int, default=4)
    asked.add_argument("--stills", default="")
    asked.add_argument("--fps", type=float, default=0.0)
    asked.add_argument("--no-live", default="")
    asked.add_argument("--drop-every", type=int, default=0)
    said = asked.parse_args()
    if not 1 <= said.machines <= 64:
        sys.exit("Between 1 and 64 machines.")
    if said.port in (8787, 8788, 8789, 8791, 8793):
        sys.exit("That port is a QKVM's own, or another stand-in's: another one.")

    if said.stills:
        files = sorted(Path(said.stills).glob("*.jpg"))
        pictures = [(True, file.read_bytes()) for file in files]
        codec, width, height, pace = "jpeg", 0, 0, said.fps or 2.0
    elif said.film:
        pictures = pictures_of(Path(said.film).read_bytes())
        named = re.search(r"(\d+)p(\d+)", Path(said.film).name)
        height = int(named.group(1)) if named else 0
        codec, width, pace = "h264", height * 16 // 9, said.fps or (float(named.group(2)) if named else 30.0)
    else:
        sys.exit("A film (an H.264 file), or --stills with a folder of JPEGs.")
    if not pictures or not pictures[0][0]:
        sys.exit("No pictures in that, or it does not begin with a key frame.")
    print(f"{len(pictures)} pictures, {codec}, {pace:g} a second", flush=True)
    if sys.platform == "win32":
        # Windows' timer goes in steps of 15.6 ms unless a program asks for finer ones: the film's
        # pace would wobble by as much, and so would what is measured of the program showing it.
        import ctypes

        ctypes.windll.winmm.timeBeginPeriod(1)

    fakes = [
        FakeKvm(FakeState(colour=tuple(int(part * 255) for part in colorsys.hsv_to_rgb(number / said.machines, 0.55, 0.6)))).start()
        for number in range(said.machines)
    ]

    with tempfile.TemporaryDirectory() as folder:
        core = Core(Path(folder))
        core.store.users = [UserRecord(id="demo", name="admin", role="admin", auth=core.vault.create(DEMO_PASSWORD))]
        for number, fake in enumerate(fakes):
            record = DeviceRecord(id=f"live{number}", name=f"Machine {number + 1}", url=fake.url, kind="kvmd")
            record.secret = core.vault.encrypt(KVM_PASSWORD, record.id)
            core.store.devices.append(record)
        core.store.save()
        core.vault.lock()
        app = create_app(core)
        # The app's own live calls, where it has them, make way for these.
        app.router.routes[:] = [route for route in app.router.routes if getattr(route, "path", "") != "/api/v1/devices/{device_id}/live"]

        @app.middleware("http")
        async def says_it_has_live(request: Any, call_next: Any) -> Any:
            """The wall says which form of the live connection this server speaks, as the draft has it."""
            answer = await call_next(request)
            if request.scope["path"] != "/api/v1/wall" or answer.status_code != 200:
                return answer
            body = b"".join([part async for part in answer.body_iterator])
            said = json.loads(body)
            said["live"] = FORM
            kept = {name: value for name, value in answer.headers.items() if name.lower() != "content-length"}
            return JSONResponse(said, status_code=200, headers=kept)

        passes: dict[str, tuple[str, float]] = {}  # a pass: for which machine, and until when
        holders: dict[str, WebSocket] = {}  # which line holds a machine's keyboard and mouse

        @app.post("/api/v1/devices/{device_id}/live")
        async def live_pass(device_id: str) -> Any:
            # (Whether the caller is signed in and may ask is the app's own guard's to say, as for every call.)
            if core.store.device(device_id) is None:
                return JSONResponse({"code": "not_found", "message": "There is no such KVM"}, status_code=404)
            if device_id == said.no_live:
                return JSONResponse({"code": "no_live", "message": "This machine has no live picture"}, status_code=404)
            now = time.monotonic()
            for old in [ticket for ticket, (_, until) in passes.items() if until < now]:
                del passes[old]
            ticket = secrets.token_urlsafe(24)
            passes[ticket] = (device_id, now + PASS_LASTS)
            return {"v": FORM, "ticket": ticket}

        @app.websocket("/api/v1/devices/{device_id}/live")
        async def live_line(line: WebSocket, device_id: str) -> None:
            await line.accept()
            try:
                hello = await asyncio.wait_for(line.receive_json(), timeout=10)
            except Exception:  # noqa: BLE001  (whatever it sent first, it was no hello)
                hello = None
            given = passes.pop(str(hello.get("ticket", "")), None) if isinstance(hello, dict) else None
            if given is None or given[0] != device_id or given[1] < time.monotonic() or hello.get("t") != "hello" or hello.get("v") != FORM:
                await line.send_json({"t": "bye", "code": "no_pass", "message": "This live line was not let in: it showed no pass this QKVM gave for it"})
                await line.close()
                return
            print(f"{device_id}: a line, for up to {hello.get('max_w')} wide and {hello.get('max_fps')} a second", flush=True)
            await line.send_json({"t": "video", "codec": codec, "w": width, "h": height})
            wants_key = asyncio.Event()
            counts: dict[str, int] = {}

            async def send_pictures() -> None:
                at, counter, due = 0, 0, time.monotonic()
                while True:
                    if wants_key.is_set():
                        wants_key.clear()
                        while not pictures[at][0]:
                            at = (at + 1) % len(pictures)
                    key, data = pictures[at]
                    at = (at + 1) % len(pictures)
                    counter += 1
                    if not (said.drop_every and counter % said.drop_every == 0):
                        head = struct.pack("<BBHIq", 1 if codec == "h264" else 2, 1 if key else 0, 0, counter & 0xFFFFFFFF, time.monotonic_ns() // 1000)
                        await line.send_bytes(head + data)
                    due += 1.0 / pace
                    await asyncio.sleep(max(0.0, due - time.monotonic()))

            sending = asyncio.create_task(send_pictures())
            try:
                while True:
                    word = await line.receive_json()
                    kind = word.get("t") if isinstance(word, dict) else None
                    if kind == "key_frame":
                        wants_key.set()
                        counts["key_frame"] = counts.get("key_frame", 0) + 1
                    elif kind == "shape":
                        print(f"{device_id}: now up to {word.get('max_w')} wide and {word.get('max_fps')} a second", flush=True)
                    elif kind == "control":
                        if word.get("on") and holders.get(device_id) not in (None, line):
                            await line.send_json({"t": "control", "on": False, "code": "taken", "message": "Somebody else has this machine's keyboard and mouse"})
                            continue
                        if word.get("on"):
                            holders[device_id] = line
                        elif holders.get(device_id) is line:
                            del holders[device_id]
                        await line.send_json({"t": "control", "on": bool(word.get("on"))})
                        print(f"{device_id}: keyboard and mouse {'taken' if word.get('on') else 'given back'}", flush=True)
                    elif kind in ("k", "m", "mr", "mb", "mw", "up") and holders.get(device_id) is line:
                        counts[kind] = counts.get(kind, 0) + 1
            except Exception:  # noqa: BLE001  (the line is gone, or said what is no word: either way it ends)
                pass
            finally:
                sending.cancel()
                if holders.get(device_id) is line:
                    del holders[device_id]
                print(f"{device_id}: the line ended; came over it: {counts or 'nothing'}", flush=True)

        print(f"A QKVM with live pictures: http://127.0.0.1:{said.port}/", flush=True)
        uvicorn.run(app, host="127.0.0.1", port=said.port, log_level="warning", access_log=False)


if __name__ == "__main__":
    main()
