"""Run the native compatibility client against a real, isolated Python web app.

Requires the web candidate's existing test dependencies; installs nothing. Device ASGI
responses alone are delayed for one device. Server admission, authentication, KDF and
revocation timers retain their production settings. Never use this with production data.

Beside the five KVMs of the live line, the web's own stand-ins of the families 0.3.0 added to
(b11): a KVM with a power board, a Fingerbot and virtual media, a VNC server, servers whose
controllers are AMI's and OpenBMC's, PCs with Intel AMT, a Proxmox guest, and a KVM whose screen
is watched. Each is added only where the web candidate's own stand-ins have it (0.2.2 has no
virtual media and no screen watch). Only the screen watch's minute and the alert round's pace are
made shorter, as the web's own tests make them; everything the server answers is otherwise as it
ships. What the server was asked and answered is kept in exchange.jsonl, as the server saw it
(method, path, status, Retry-After, who asked: never a body, a cookie or a pass); what the
stand-ins were sent, in device-observations.json; what the native client showed and sent, in its
own desktop-observations.json.
"""

import argparse
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


# A controller slow enough that the server's answer to Start into BIOS setup comes after the two and a half minutes the
# desktop gives any other action, and within the four it gives this one: each of its two changes of the boot override
# answered 65 s late (past the server's 60 s for one: the change, then its clearing), and its reads late too. The server
# then says the override may be set (504 timeout), about 156 s after it was asked (measured on this PC, 2026-11-08).
SLOW_SETUP = {"slow_boot_changes": 2, "slow_boot_by": 65.0, "system_takes": 15.0, "root_takes": 15.0}

# Query fields that could carry a secret are never written down, whatever they hold.
SECRET_FIELDS = {"key", "token", "ticket", "pass", "password", "passwd", "secret", "code", "session"}


class Exchange:
    """What the server was asked and how it answered, as the server saw it: an ASGI wrapper around the web app."""

    def __init__(self, app, said, began):
        self.app, self.said, self.began = app, said, began

    async def __call__(self, scope, receive, send):
        if scope["type"] not in ("http", "websocket"):
            await self.app(scope, receive, send)
            return
        headers = {name.decode("latin-1").lower(): value.decode("latin-1") for name, value in scope.get("headers", [])}
        agent = headers.get("user-agent", "")
        query = []
        for part in scope.get("query_string", b"").decode("latin-1").split("&"):
            name, _, value = part.partition("=")
            if name:
                query.append(f"{name}=*" if name.lower() in SECRET_FIELDS else f"{name}={value[:80]}")
        record = {"t": round(time.monotonic() - self.began, 3),
                  "who": "fixture" if agent.startswith(("python-httpx", "Python/")) else "desktop",
                  "m": scope.get("method", "WS"), "p": scope.get("path", "")[:200], "q": "&".join(query)}
        started = time.monotonic()

        async def sending(message):
            if message["type"] == "http.response.start":
                record["s"] = message["status"]
                for name, value in message.get("headers", []):
                    if name.lower() == b"retry-after":
                        record["ra"] = value.decode("latin-1")
            elif message["type"] == "websocket.accept":
                record["s"] = 101
            elif message["type"] == "websocket.close" and "s" not in record:
                record["s"] = message.get("code", 1000)
            await send(message)

        try:
            await self.app(scope, receive, sending)
        finally:
            record["ms"] = round((time.monotonic() - started) * 1000)
            self.said.append(record)


def rfb_seen(screen, since):
    """What an RFB stand-in (fake_vnc's VncState) was sent since the last look: key events, pointer events, what is down."""
    keys = [list(key) for key in screen.keys[since.get("keys", 0):]]
    pointers = screen.pointers[since.get("pointers", 0):]
    since["keys"], since["pointers"] = len(screen.keys), len(screen.pointers)
    down = set()
    for symbol, pressed in screen.keys:
        (down.add if pressed else down.discard)(symbol)
    return {"keys": keys[-80:], "pointers": len(pointers), "last_pointer": list(pointers[-1]) if pointers else None,
            "down": sorted(down), "buttons": screen.pointers[-1][2] if screen.pointers else 0}


def fixture_extras(state_fields, folder):
    """The stand-ins of the families 0.3.0 added to, as far as this web candidate's own stand-ins have them."""
    from tests.fake_kvm import FakeKvm, FakeState
    from tests.fake_vnc import FakeVnc, VncState
    from tests.fake_redfish import BmcState, FakeBmc
    from tests.fake_amt import OTHER_GUID, AmtState, FakeAmt
    from tests.fake_proxmox import Guest, FakeProxmox, PveState
    from tests.aspeed_frames import console_screen
    from tests.test_live import a_jpeg
    from tests.test_openbmc_console import ikvm

    jpeg = {"picture_type": "image/jpeg"}
    refused = '{"ok": false, "result": {"error": "AtxOperationError", "error_msg": "The board is busy"}}'
    extras = {
        # A power board's press: answered, refused by the KVM (a no), and broken off on its way (it may have pressed).
        "presser": FakeKvm(FakeState(picture=a_jpeg((90, 10, 40)), atx=True, **jpeg)),
        "refuser": FakeKvm(FakeState(picture=a_jpeg((90, 40, 10)), atx=True, atx_reply=(409, refused), **jpeg)),
        "dropper": FakeKvm(FakeState(picture=a_jpeg((90, 90, 10)), atx=True, atx_reply=(0, ""), **jpeg)),
        "fingerbot": FakeKvm(FakeState(picture=a_jpeg((10, 90, 40)), fingerbot=True, fingerbot_battery=12, **jpeg)),
        "vnc": FakeVnc(VncState(width=160, height=100)),
        "openbmc": FakeBmc(BmcState(layout="openbmc", kvm_screen=ikvm())),
        "ami": FakeBmc(BmcState(own_console=True, screen=[console_screen(640, 480, seed=5)])),
        # Start into BIOS setup: one that does it, and one whose changes and reads are slow (the server's longest answer,
        # "may be set": SLOW_SETUP).
        "setup": FakeBmc(BmcState()),
        "slowsetup": FakeBmc(BmcState()),
        "amt": FakeAmt(),
        "amtsetup": FakeAmt(AmtState(power=8, guid=OTHER_GUID, hostname="desk-8", serial="PC0000000002")),
        "proxmox": FakeProxmox(PveState(guests={100: Guest(100, "web", screen=VncState(width=320, height=200, name="QEMU (web)"))}),
                               tls_dir=Path(folder) / "pve"),
        "watch": FakeKvm(FakeState(picture=a_jpeg((60, 60, 60)), **jpeg)),
    }
    if "msd" in state_fields:
        images = {"debian-13.iso": {"complete": True, "removable": True, "size": 700_000_000, "mod_ts": 1_700_000_000.0, "sha256": ""}}
        extras["media"] = FakeKvm(FakeState(picture=a_jpeg((40, 40, 140)), msd=True, msd_images=images, msd_write_rate=6_000_000, **jpeg))
    return extras


def seen_of(name, fake, since):
    """What one of the extra stand-ins was sent since the last look, kept short (a phase's answer is 4 KiB at most)."""
    mark = since.setdefault(name, {})
    state = fake.state
    if name in ("vnc",):
        return rfb_seen(state, mark)
    if name == "openbmc":
        return {**rfb_seen(state.kvm_screen, mark), "logins": state.logins}
    if name == "amt":
        return {**rfb_seen(state.screen, mark), "kvm_sessions": state.kvm_sessions}
    if name == "proxmox":
        return rfb_seen(state.guests[100].screen, mark)
    if name == "ami":
        hid = [list(entry) for entry in state.hid[mark.get("hid", 0):] if entry[0] == "key"]
        mark["hid"] = len(state.hid)
        return {"keys": [[entry[1], list(entry[2])] for entry in hid][-80:], "bad": len(state.bad_hid)}
    if name in ("setup", "slowsetup"):
        return {"boot_override": list(state.boot_override), "actions": state.actions[-10:]}
    if name == "amtsetup":
        first = state.redirected_at[0] if state.redirected_at else None
        return {"redirections": state.redirections, "kvm_sessions": state.kvm_sessions, "actions": state.actions[-10:],
                "opened_before_on": first is not None and first < state.came_on_at}
    seen = {"actions": state.actions[mark.get("actions", 0):][-40:]}
    mark["actions"] = len(state.actions)
    if name == "media":
        seen["drive"] = dict(state.msd_drive)
        seen["images"] = sorted(state.msd_images)
        seen["writes"] = [{key: write[key] for key in ("name", "said", "received", "complete", "removed")} for write in state.msd_writes][-5:]
        seen["sha256"] = {image: about.get("sha256", "") for image, about in state.msd_images.items()}
    return seen


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--web-app", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hold-seconds", type=int, default=0)
    parser.add_argument("--browser", action="store_true", help="Use the existing Playwright installation")
    parser.add_argument("--cases", nargs="*", default=[], help="Only these cases of the native client (a trial: the checks of the whole run are left out)")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.web_app.resolve()))
    os.environ["QKVM_UPDATE_API"] = "http://127.0.0.1:9"
    if args.browser:
        os.environ.setdefault("PLAYWRIGHT_BROWSERS_PATH", str(Path(os.environ["LOCALAPPDATA"]) / "ms-playwright"))
    from tests import fake_kvm, test_live
    from tests.fake_kvm import FakeKvm, FakeState
    from tests.test_live import Wall, a_jpeg
    from tests.test_wall import PASSWORD
    from tests.test_totp import switch_on, code_for
    from tests.test_alerts import Inbox
    from tests.fake_proxmox import SECRET as PVE_SECRET, TOKEN_ID as PVE_TOKEN
    from qkvm._version import __version__ as web_version

    # What the server is asked and answers, as it saw it (see Exchange): both walls.
    exchange = []
    began = time.monotonic()
    made_app = test_live.create_app
    test_live.create_app = lambda core: Exchange(made_app(core), exchange, began)
    # The screen watch (0.3.0): a minute of two seconds, the alert round every half second, every picture compared, as
    # the web's own tests make them (test_screens_on_a_wall.py). Nothing else of the server is changed.
    watches = importlib.util.find_spec("qkvm.screens") is not None
    if watches:
        from qkvm import alerts, kvm, screens
        screens.MINUTE = 2.0
        alerts.CHECK_EVERY = 0.5
        kvm.FRAME_MIN_AGE = 0
    newer = tuple(int(part) for part in web_version.split(".")[:2]) >= (0, 3)
    inbox = Inbox()

    devices = []
    extras = {}
    factory = fake_kvm.make_fake_kvm
    try:
        for name in ("absolute", "relative", "slow", "contested", "ownership"):
            state = FakeState(picture=a_jpeg((32, 74, 135)), picture_type="image/jpeg",
                              mouse_absolute=name != "relative")
            if name == "slow":
                def delayed_factory(state):
                    app = factory(state)
                    attempts = 0
                    async def delayed(scope, receive, send):
                        nonlocal attempts
                        if scope["type"] == "websocket" and b"stream=0" in scope.get("query_string", b""):
                            # Two three-second device operations exceed the old desktop deadline
                            # while each has ample margin under the driver's six-second limit.
                            attempts += 1
                            await asyncio.sleep(3)
                            if attempts == 1:
                                from starlette.responses import Response
                                from starlette.websockets import WebSocket
                                await WebSocket(scope, receive, send).send_denial_response(Response(status_code=401))
                                return
                        await app(scope, receive, send)
                    return delayed
                fake_kvm.make_fake_kvm = delayed_factory
            elif name == "ownership":
                def ownership_factory(state):
                    app = factory(state)
                    async def delayed(scope, receive, send):
                        flag = output / "delay-takeover"
                        if scope["type"] == "websocket" and b"stream=0" in scope.get("query_string", b"") and flag.exists():
                            flag.unlink()
                            (output / "takeover-opening").touch()
                            await asyncio.sleep(3)
                        await app(scope, receive, send)
                    return delayed
                fake_kvm.make_fake_kvm = ownership_factory
            device = FakeKvm(state).start()
            fake_kvm.make_fake_kvm = factory
            devices.append((name, device))
        # (In the output folder: the PC's own Temp is swept while a run is under way.)
        # (Its stand-ins are stopped after it is cleaned up, past an early return: what they hold open is let be.)
        with tempfile.TemporaryDirectory(prefix="qkvm-joint-", dir=str(output), ignore_cleanup_errors=True) as folder:
            os.environ["HOME"] = folder
            os.environ["USERPROFILE"] = folder
            extras.update(fixture_extras(FakeState.__dataclass_fields__, folder))
            for fake in extras.values():
                fake.start()
            with Wall(Path(folder) / "data") as wall, Wall(Path(folder) / "enrollment") as enrollment:
                automation = chromium = page = browser = owner_page = owner_line = replacement = None
                try:
                    admin = wall.browser()
                    config = {"url": wall.url, "password": PASSWORD, "version": web_version}
                    for name, device in devices:
                        config[name] = wall.add(admin, device.url, name=f"Fixture {name}")
                    for name, fake in extras.items():
                        extra = {"name": f"Fixture {name}"}
                        url = fake.url
                        if name in ("openbmc", "ami", "setup", "slowsetup"):
                            extra["kind"] = "redfish"
                        elif name == "proxmox":
                            url = fake.guest_url(100)
                            extra.update({"kind": "proxmox", "username": PVE_TOKEN, "password": PVE_SECRET})
                        elif name == "watch" and watches:
                            extra["unchanged_minutes"] = 1
                        config[name] = wall.add(admin, url, **extra)
                    config["device_count"] = len(devices) + len(extras)
                    # What 0.3.0 has and 0.2.2 has not, which the native client then expects to be offered or not.
                    config["expect"] = {"media": newer and "media" in extras, "amt_view": newer, "vnc_live": newer,
                                        "openbmc_keys": newer, "screen_watch": newer and watches, "words_copied": newer}
                    # Two saved actions, of each kind, for the menus of the machines that take keys.
                    saved = {}
                    for body in ({"name": "Joint keys", "kind": "keys", "keys": "ControlLeft,AltLeft,Delete"},
                                 {"name": "Joint text", "kind": "text", "text": "joint saved", "enter": False}):
                        reply = admin.post("/api/v1/actions", json=body)
                        assert reply.status_code in (200, 201), reply.text
                        saved[body["kind"]] = reply.json()["action"]["id"]
                    config["saved"] = saved
                    # Alerts on, to a receiver of the fixture's own on loopback: the screen watch's are written down.
                    reply = admin.put("/api/v1/alerts", json={"enabled": True, "webhook": inbox.url})
                    assert reply.status_code == 200, reply.text
                    users = {}
                    for name, role in (("native_mfa", "viewer"), ("native_operator", "operator"),
                                       ("native_admin", "admin"), ("browser_owner", "admin"), ("replacement", "operator"),
                                       ("native_lines", "operator"), ("native_looker", "viewer")):
                        reply = admin.post("/api/v1/users", json={"name": name, "role": role, "password": PASSWORD})
                        assert reply.status_code == 201, reply.text
                        users[name] = reply.json()["user"]["id"]
                    # Slow from now on, past its adding (see fixture_extras): SLOW_SETUP.
                    for field, value in SLOW_SETUP.items():
                        setattr(extras["slowsetup"].state, field, value)
                    since = {}
                    mfa = wall.browser("native_mfa")
                    mfa_secret = switch_on(mfa, PASSWORD)
                    enrollment_admin = enrollment.browser()
                    reply = enrollment_admin.post("/api/v1/users", json={"name": "native_enroll", "role": "viewer", "password": PASSWORD})
                    assert reply.status_code == 201, reply.text
                    switch_on(enrollment_admin, PASSWORD)
                    reply = enrollment_admin.put("/api/v1/security", json={"require_totp": "viewer"})
                    assert reply.status_code == 200, reply.text
                    config["enrollment_url"] = enrollment.url
                    phases = output / "phases"
                    phases.mkdir()
                    config["phases"] = str(phases)
                    if args.browser:
                        from playwright.sync_api import sync_playwright, expect
                        from tests.test_in_a_browser import sign_in, tile, top
                        automation = sync_playwright().start()
                        chromium = automation.chromium.launch()
                        page = chromium.new_page(viewport={"width": 1280, "height": 800})
                        page.set_default_timeout(15000)
                        sign_in(page, wall, name="admin")
                        page.screenshot(path=str(output / "web-wall.png"))
                        tile(page, "Fixture contested").click()
                        page.locator("#focus-control").click()
                        expect(page.locator("#focus-control")).to_have_attribute("aria-pressed", "true")
                        page.screenshot(path=str(output / "web-control.png"))
                        owner_page = chromium.new_page(viewport={"width": 1280, "height": 800})
                        owner_page.set_default_timeout(15000)
                        sign_in(owner_page, wall, name="browser_owner")
                        tile(owner_page, "Fixture ownership").click()
                        owner_page.wait_for_function("() => Number(document.querySelector('#focus-live').dataset.drawn) >= 1")
                        owner_page.keyboard.press("c")
                        expect(owner_page.locator("#focus-control")).to_have_attribute("aria-pressed", "true")
                        owner_page.keyboard.down("ControlLeft")
                    else:
                        browser = wall.line(admin, config["contested"], video=["jpeg"])
                        browser.say(t="control", on=True)
                        assert browser.word("control")["on"] is True
                        owner_line = wall.line(wall.browser("browser_owner"), config["ownership"], video=["jpeg"])
                        owner_line.say(t="control", on=True)
                        assert owner_line.word("control")["on"] is True
                        owner_line.say(t="k", u=224, d=True)
                    config_path = output / "ready.json"
                    config_path.write_text(json.dumps(config), encoding="utf-8")
                    print(f"Disposable fixture ready: {wall.url}", flush=True)
                    environment = os.environ.copy()
                    environment["QKVM_WEB_FIXTURE"] = str(config_path)
                    environment["QT_FORCE_STDERR_LOGGING"] = "1"
                    with (output / "native-tests.txt").open("w", encoding="utf-8") as log:
                        process = subprocess.Popen([str(args.client.resolve()), *args.cases], env=environment,
                                                   stdout=log, stderr=subprocess.STDOUT)
                        completed = set()
                        try:
                            # (The 0.3.0 cases wait for the server's own times: a pass's 30 s, Start into BIOS setup's
                            # longest answer, the screen watch's minute.)
                            deadline = time.monotonic() + 1500
                            while process.poll() is None:
                                assert time.monotonic() < deadline, "Native compatibility test timed out"
                                for phase in ("mfa-code", "demote", "hide-device", "cancelled-takeover", "native-took-over",
                                              "browser-cancelled", "browser-takes-over", "replace-owner", "restore-browser-owner",
                                              "arm-takeover-delay", "demote-during-takeover", "demotion-protected-holder", "details",
                                              *sorted(request.name[:-len(".request")] for request in phases.glob("*.request")
                                                      if request.name.startswith(("look-", "do-")))):
                                    if phase in completed or not (phases / (phase + ".request")).exists():
                                        continue
                                    response = {"ok": True}
                                    if phase.startswith("look-"):
                                        # What a stand-in was sent since the last look at it; or what the desktop asked
                                        # of the live passes, and was answered.
                                        name = phase[len("look-"):].rsplit("-", 1)[0]
                                        if name == "lives":
                                            response["lives"] = [[record.get("s"), record.get("ra")] for record in list(exchange)
                                                                 if record["who"] == "desktop" and record["m"] == "POST"
                                                                 and record["p"].endswith("/live")][-60:]
                                        elif name in extras:
                                            response.update(seen_of(name, extras[name], since))
                                        else:
                                            response["ok"] = False
                                    elif phase.startswith("do-"):
                                        what = phase[len("do-"):].rsplit("-", 1)[0]
                                        if what in ("rename-media", "name-media-back"):
                                            # The machine is called otherwise on the server while the desktop's question stands.
                                            named = "Fixture media renamed" if what == "rename-media" else "Fixture media"
                                            reply = admin.put(f"/api/v1/devices/{config['media']}",
                                                              json={"url": extras["media"].url, "username": "admin", "name": named})
                                            response["ok"] = reply.status_code == 200
                                        elif what == "change-watch":
                                            extras["watch"].state.picture = a_jpeg((200, 200, 200))
                                        else:
                                            response["ok"] = False
                                    elif phase == "mfa-code":
                                        # The next normal TOTP window is accepted by the production verifier;
                                        # it was not consumed by setup and remains valid while the client signs in.
                                        response["code"] = code_for(mfa_secret, 1)
                                    elif phase == "details":
                                        machine = config["contested"]
                                        summary = admin.get(f"/api/v1/devices/{machine}/history")
                                        samples = admin.get(f"/api/v1/devices/{machine}/history?what=samples&every=900&limit=5000")
                                        activity = admin.get(f"/api/v1/audit?device={machine}&limit=100")
                                        assert all(reply.status_code == 200 for reply in (summary, samples, activity))
                                        assert summary.json()["share"] is not None
                                        assert any(line["unit"] == "C" and line["points"][-1][1] == 42.5 for line in samples.json()["series"])
                                        assert any(entry["event"] == "live_control" for entry in activity.json()["entries"])
                                        if page:
                                            # Open a fresh observer after the ownership scenarios. The original
                                            # controller keeps its live line, and its one-minute history cache
                                            # is not mistaken for a just-issued read of the activity log.
                                            details_page = chromium.new_page(viewport={"width": 1280, "height": 800})
                                            try:
                                                sign_in(details_page, wall, name="admin")
                                                tile(details_page, "Fixture contested").click()
                                                details_page.locator("#focus-details summary").click()
                                                history = details_page.locator("#focus-history")
                                                expect(history).to_contain_text("Last seen")
                                                expect(history).to_contain_text("Online")
                                                expect(history).to_contain_text("Done to it")
                                                expect(history).to_contain_text("Keyboard and mouse taken")
                                                expect(history.locator('.history-graph[data-unit="C"]')).to_be_visible()
                                                details_page.screenshot(path=str(output / "web-history-activity.png"), animations="disabled")
                                            finally:
                                                details_page.close()
                                        response["known"] = True
                                        response["temperature"] = 42.5
                                        (output / "details-result.json").write_text(json.dumps({
                                            "known_history": True, "temperature": 42.5,
                                            "control_activity": True, "browser_panel": bool(page)}), encoding="utf-8")
                                    elif phase in ("demote", "hide-device"):
                                        body = {"name": "native_operator", "role": "viewer"}
                                        if phase == "hide-device":
                                            body["devices"] = []
                                        reply = admin.put(f"/api/v1/users/{users['native_operator']}", json=body)
                                        assert reply.status_code == 200, reply.text
                                    elif phase in ("cancelled-takeover", "demotion-protected-holder"):
                                        if owner_page:
                                            expect(owner_page.locator("#focus-control")).to_have_attribute("aria-pressed", "true")
                                    elif phase == "native-took-over":
                                        if owner_page:
                                            expect(owner_page.locator("#focus-control")).to_have_text("Take over from native_admin")
                                            expect(owner_page.locator("#focus-keyboard-toggle")).to_be_disabled()
                                            owner_page.keyboard.up("ControlLeft")
                                            owner_page.keyboard.press("KeyB")
                                            owner_page.screenshot(path=str(output / "web-displaced-owner.png"))
                                        else:
                                            assert owner_line.word("control")["by"] == "native_admin"
                                        held = [v["event"]["state"] for v in devices[4][1].state.hid
                                                if v["event_type"] == "key" and v["event"]["key"] == "ControlLeft"]
                                        assert held == [True, False], held
                                    elif phase in ("browser-cancelled", "browser-takes-over"):
                                        if owner_page:
                                            owner_page.locator("#focus-control").click()
                                            expect(top(owner_page)).to_contain_text("native_admin has the keyboard and mouse of Fixture ownership")
                                            if phase == "browser-cancelled":
                                                top(owner_page).get_by_role("button", name="Cancel", exact=True).click()
                                            else:
                                                owner_page.screenshot(path=str(output / "web-takeover-question.png"))
                                                top(owner_page).get_by_role("button", name="Take over", exact=True).click()
                                                expect(owner_page.locator("#focus-control")).to_have_attribute("aria-pressed", "true")
                                        elif phase == "browser-takes-over":
                                            owner_line.say(t="control", on=True, take=True, **{"from": "native_admin"})
                                            assert owner_line.word("control")["on"] is True
                                    elif phase == "replace-owner":
                                        if owner_page:
                                            owner_page.locator("#focus-control").click()
                                            expect(owner_page.locator("#focus-control")).to_have_attribute("aria-pressed", "false")
                                        else:
                                            owner_line.say(t="control", on=False)
                                            assert owner_line.word("control")["on"] is False
                                        replacement = wall.line(wall.browser("replacement"), config["ownership"], video=["jpeg"])
                                        replacement.say(t="control", on=True)
                                        assert replacement.word("control")["on"] is True
                                    elif phase == "restore-browser-owner":
                                        replacement.say(t="control", on=False)
                                        assert replacement.word("control")["on"] is False
                                        replacement.close()
                                        replacement = None
                                        if owner_page:
                                            owner_page.locator("#focus-control").click()
                                            expect(owner_page.locator("#focus-control")).to_have_attribute("aria-pressed", "true")
                                        else:
                                            owner_line.say(t="control", on=True)
                                            assert owner_line.word("control")["on"] is True
                                    elif phase == "arm-takeover-delay":
                                        (output / "delay-takeover").touch()
                                    elif phase == "demote-during-takeover":
                                        until = time.monotonic() + 15
                                        while not (output / "takeover-opening").exists():
                                            assert time.monotonic() < until, "Takeover never reached HID opening"
                                            time.sleep(0.01)
                                        reply = admin.put(f"/api/v1/users/{users['native_admin']}",
                                                          json={"name": "native_admin", "role": "operator"})
                                        assert reply.status_code == 200, reply.text
                                    temporary = phases / (phase + ".tmp")
                                    temporary.write_text(json.dumps(response), encoding="utf-8")
                                    temporary.replace(phases / (phase + ".json"))
                                    completed.add(phase)
                                time.sleep(0.02)
                        finally:
                            if process.poll() is None:
                                process.terminate()
                                try:
                                    process.wait(timeout=5)
                                except subprocess.TimeoutExpired:
                                    process.kill()
                                    process.wait()
                        result = subprocess.CompletedProcess(process.args, process.returncode)
                        (output / "completed-phases.json").write_text(json.dumps(sorted(completed)), encoding="utf-8")
                    time.sleep(0.2)
                    observations = {name: {"actions": device.state.actions, "hid": device.state.hid,
                                           "unsupported_mouse_messages": device.state.pointer_dropped}
                                    for name, device in devices}
                    observations["extras"] = {name: seen_of(name, fake, {}) for name, fake in extras.items()}
                    (output / "device-observations.json").write_text(json.dumps(observations, indent=2, default=str), encoding="utf-8")
                    # What the server says of the extra machines at the end, as it said it (for the record beside what
                    # the desktop showed of them).
                    listed = {device["id"]: device for device in admin.get("/api/v1/wall").json()["devices"]}
                    status = admin.get("/api/v1/status").json()
                    (output / "server-said.json").write_text(json.dumps(
                        {name: {"wall": listed.get(config[name]), "status": status.get(config[name])} for name in extras},
                        indent=2), encoding="utf-8")
                    with (output / "exchange.jsonl").open("w", encoding="utf-8") as written:
                        for record in list(exchange):
                            written.write(json.dumps(record) + "\n")
                    assert all(d.state.pointer_dropped == 0 for _, d in devices), "Unsupported pointer input arrived"
                    if not config["expect"]["amt_view"]:
                        # A server without the AMT views was asked for none (b11).
                        views = [record for record in exchange if record["p"].endswith("/view") and record["m"] == "PUT"]
                        assert not views, f"A change of view went to a server that has none: {views}"
                    if result.returncode == 0 and not args.cases:
                        assert all(d.state.hid for _, d in devices[:2]), "Supported pointer input never arrived"
                        assert devices[0][1].state.actions.count("type 'native fixture'") == 1, "Text was missing or replayed"
                        assert "type 'revoked must not arrive'" not in devices[0][1].state.actions
                        held = [item["event"]["state"] for item in devices[0][1].state.hid
                                if item["event_type"] == "key" and item["event"]["key"] == "ControlLeft"]
                        assert held == [True, False], "Role revocation did not release the held key"
                        screen_keys = [(item["event"]["key"], item["event"]["state"])
                                       for item in devices[0][1].state.hid
                                       if item["event_type"] == "key" and item["event"]["key"] != "ControlLeft"]
                        assert screen_keys == [("ShiftLeft", True), ("KeyB", True),
                                               ("KeyB", False), ("ShiftLeft", False)], screen_keys
                        assert not devices[3][1].state.actions, "Another holder's protection was bypassed"
                        owner_keys = [(v["event"]["key"], v["event"]["state"]) for v in devices[4][1].state.hid
                                      if v["event_type"] == "key"]
                        assert owner_keys == [("ControlLeft", True), ("ControlLeft", False),
                                              ("ShiftLeft", True), ("ShiftLeft", False)], owner_keys
                        assert not devices[4][1].state.actions, "Displaced owner sent an action"
                    if page is not None:
                        expect(page.locator("#focus-control")).to_have_attribute("aria-pressed", "true")
                        page.locator("#focus-keyboard-toggle").click()
                        expect(page.locator("#focus-keyboard")).to_be_visible()
                        page.locator('#focus-keyboard [data-code="ShiftLeft"]').click()
                        page.locator('#focus-keyboard [data-code="KeyA"]').click()
                        expect(page.locator('#focus-keyboard [data-code="ShiftLeft"]')).to_have_attribute("aria-pressed", "false")
                        expected_screen_keys = [("ShiftLeft", True), ("KeyA", True),
                                                ("KeyA", False), ("ShiftLeft", False)]
                        deadline = time.monotonic() + 10
                        def browser_keys():
                            return [(item["event"]["key"], item["event"]["state"])
                                    for item in devices[3][1].state.hid if item["event_type"] == "key"]
                        while browser_keys() != expected_screen_keys and time.monotonic() < deadline:
                            page.wait_for_timeout(50)
                        assert browser_keys() == expected_screen_keys, browser_keys()
                        page.screenshot(path=str(output / "web-keyboard.png"))
                        for width in (640, 800, 1280):
                            page.set_viewport_size({"width": width, "height": 800})
                            expect(page.locator("#focus-close")).to_be_visible()
                            expect(page.locator("#focus-statusbar")).to_be_visible()
                            # Wait on measured bounds, not a guessed animation delay.
                            page.wait_for_function("""() => {
                                const k = document.querySelector('#focus-keyboard').getBoundingClientRect();
                                const s = document.querySelector('#focus-statusbar').getBoundingClientRect();
                                return k.left >= 0 && k.right <= innerWidth && k.top >= 0 && k.bottom <= innerHeight
                                    && s.bottom <= innerHeight && s.top >= 0;
                            }""")
                            page.screenshot(path=str(output / f"web-console-keyboard-{width}.png"))
                        page.locator("#focus-keyboard-toggle").click()
                        page.locator("#focus-type").click()
                        top(page).locator("textarea").fill("browser fixture")
                        top(page).get_by_label("Press Enter afterwards").uncheck()
                        top(page).get_by_role("button", name="Continue", exact=True).click()
                        top(page).get_by_role("button", name="Type text", exact=True).click()
                        expect(page.locator("#focus-control")).to_have_attribute("aria-pressed", "true")
                        deadline = time.monotonic() + 15
                        while "type 'browser fixture'" not in devices[3][1].state.actions and time.monotonic() < deadline:
                            page.wait_for_timeout(50)
                        assert devices[3][1].state.actions.count("type 'browser fixture'") == 1
                        expect(page.locator("dialog[open]")).to_have_count(0)
                        expect(page.locator("#focus-control")).to_have_attribute("aria-pressed", "true")
                        page.locator("#focus-control").click()
                        expect(page.locator("#focus-control")).to_have_attribute("aria-pressed", "false")
                        page.locator("#focus-details summary").click()
                        page.screenshot(path=str(output / "web-details.png"), animations="disabled")
                        (output / "browser-result.json").write_text(json.dumps({"ownership_survived_native_requests": True,
                            "typed_once": True, "floating_keyboard": True, "details_panel": True,
                            "screen_chord_balanced": True, "responsive_console": True}), encoding="utf-8")
                        (output / "browser-device-observations.json").write_text(json.dumps({
                            "keys": browser_keys(), "actions": devices[3][1].state.actions}, indent=2), encoding="utf-8")
                    else:
                        browser.say(t="k", u=4, d=True)
                        browser.say(t="up")
                        browser.close()
                    print(f"Native suite exit: {result.returncode}. Observations retained in {output}", flush=True)
                    if result.returncode:
                        return result.returncode
                    if args.hold_seconds:
                        print("Fixture remains available for browser comparison; create stop.txt to end early.", flush=True)
                        deadline = time.monotonic() + min(args.hold_seconds, 1800)
                        while time.monotonic() < deadline and not (output / "stop.txt").exists():
                            time.sleep(0.5)
                finally:
                    if chromium is not None:
                        chromium.close()
                    if automation is not None:
                        automation.stop()
                    if browser is not None:
                        browser.close()
                    if owner_line is not None:
                        owner_line.close()
                    if replacement is not None:
                        replacement.close()
                    wall.core.history.stop()
                    wall.core.activity.stop()
                    enrollment.core.history.stop()
                    enrollment.core.activity.stop()
    finally:
        fake_kvm.make_fake_kvm = factory
        test_live.create_app = made_app
        for _, device in reversed(devices):
            device.stop()
        for fake in reversed(list(extras.values())):
            fake.stop()
        inbox.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
