#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the Candis-S31 factory firmware through the EVT test flow.

The script opens the factory console over UART, sends each test command in a
fixed order, parses the machine-readable FACTORY_INFO / FACTORY_RESULT /
FACTORY_PROMPT / FACTORY_SUMMARY lines, forwards operator answers to the
board, and writes a JSON report plus the complete console log. It first
issues `report_reset`, so results persisted in NVS by an earlier run on the
same board cannot leak into the final summary.

Usage:
    python3 run_evt.py --port /dev/ttyUSB0 [--board-id EVT-0042]
    python3 run_evt.py --port /dev/ttyUSB0 --non-interactive   # skip prompts
    python3 run_evt.py --self-test    # pty-based dry run, no hardware
"""

import argparse
import datetime
import json
import os
import re
import select
import sys
import threading
import time

import serial

# Host-side composite stage marker, not a console command: for each peripheral
# behind a switched rail, run OFF -> scan -> ON -> scan -> OFF against the main
# I2C bus, proving that CST820 (0x15), ES8389 (0x20), and OV5640 (0x3c) answer
# only while their rail is on. A final scan with every rail off confirms none
# of them answers. EvtRunner.run() expands the marker into the real command
# sequence; --no-power-rail-scan drops it.
POWER_RAIL_SCAN = "power_rail_scan"

# Host-side proof that typec_test actually raised the FUSB303B control domain.
# The board is strapped for 0x21; 0x31 is a detectable assembly mismatch.
TYPE_C_POWER_SCAN = "type_c_power_scan"
FUSB303B_EXPECTED_ADDRESS = 0x21
FUSB303B_ALT_ADDRESS = 0x31

# Stage-boundary cleanup marker: the firmware command stops activity and
# switches every peripheral rail off. It prints text but no FACTORY_RESULT,
# so the runner drains it instead of waiting for a result.
POWER_ALL_OFF = "power_all_off"

# (peripheral_power name, main-bus address) for the devices behind switched
# rails that the POWER_RAIL_SCAN stage checks.
POWER_RAIL_DEVICES = (
    ("touch", 0x15),    # CST820 touch, ALDO2
    ("audio", 0x20),    # ES8389 codec, ALDO3
    ("camera", 0x3c),   # OV5640 SCCB, camera rails
)

# (command, console timeout in seconds, expected report test name or marker)
STAGES = [
    ("board_info", 10, "INFO"),
    ("safe_state", 10, "safe_state"),
    ("flash_test", 15, "flash"),
    ("psram_test", 20, "psram"),
    # First prove the boot safe state: switched-rail devices and FUSB303B are
    # expected to be silent here. Then raise only the Type-C control domain
    # and close that loop with a second, host-graded scan before USB Host owns
    # and tears down the port.
    ("i2c_scan main", 20, "i2c_main"),
    ("typec_test", 10, "type_c"),
    (TYPE_C_POWER_SCAN, 20, TYPE_C_POWER_SCAN),
    ("usb_host_test", 30, "usb_host"),
    ("i2c_scan lp", 20, "i2c_low_power"),
    ("pmic_test", 10, "pmic"),
    ("charge_test", 10, "charge"),
    ("rtc_test", 10, "rtc"),
    ("rtc_alarm", 80, "rtc_alarm"),  # waits for the next minute boundary
    ("irq_test", 10, "shared_irq"),
    ("buttons", 60, "buttons"),  # two operator key presses on the board
    ("wifi_scan", 30, "wifi"),
    ("ble_smoke", 20, "ble"),
    # First light-up: cap brightness before any pattern, per bring-up.md.
    # display_brightness prints no FACTORY_RESULT, so it reuses the "INFO"
    # marker semantics (send, wait out the timeout, record nothing).
    ("display_brightness 30", 5, "INFO"),
    ("display_test", 45, "display"),
    ("touch_test", 30, "touch"),
    ("display_sleep_test", 150, "display_sleep"),  # four operator questions
    (POWER_ALL_OFF, 5, "INFO"),  # clear display/touch before the next block
    ("led_test", 45, "rgb_led"),
    (POWER_ALL_OFF, 5, "INFO"),  # clear the LED block
    ("sdcard_test", 20, "sdcard"),
    ("speaker_test", 45, "speaker"),
    ("microphone_test", 30, "microphone"),
    (POWER_ALL_OFF, 5, "INFO"),  # clear the audio block
    ("camera_test", 20, "camera"),
    (POWER_ALL_OFF, 5, "INFO"),  # clear the camera block
    # Optional closed-loop rail check, expanded by the runner; must stay last
    # so every device test has already powered its rail once. The stage ends
    # with every switched rail OFF.
    (POWER_RAIL_SCAN, 120, POWER_RAIL_SCAN),
]

RESULT_PREFIX = "FACTORY_RESULT "
SUMMARY_PREFIX = "FACTORY_SUMMARY "
INFO_PREFIX = "FACTORY_INFO "
PROMPT_PREFIX = "FACTORY_PROMPT "

# scan_i2c_bus() in the firmware prints one of these per answering address.
FOUND_DEVICE_RE = re.compile(r"found I2C device at 0x([0-9a-fA-F]{2})$")


def build_stages(power_rail_scan=True):
    """Stage list plus an rtc_set built from the host clock (UTC).

    A fresh board powers up with the RX8130CE time invalid, so rtc_test
    alone always fails there. Setting the clock first makes the stage
    meaningful; the bring-up power-cycle check still verifies retention.

    power_rail_scan=False drops the optional POWER_RAIL_SCAN marker, which
    restores the exact stage set this script has always run.
    """
    now = datetime.datetime.now(datetime.timezone.utc)
    # firmware weekday convention: 0=Sunday .. 6=Saturday
    rtc_command = "rtc_set %s %d" % (now.strftime("%Y-%m-%d %H:%M:%S"),
                                     now.isoweekday() % 7)
    stages = []
    for command, timeout_s, expect in STAGES:
        if command == POWER_RAIL_SCAN and not power_rail_scan:
            continue
        if command == "rtc_test":
            stages.append((rtc_command, 10, "rtc"))
        stages.append((command, timeout_s, expect))
    return stages


def parse_payload(line, prefix):
    if not line.startswith(prefix):
        return None
    try:
        return json.loads(line[len(prefix):])
    except json.JSONDecodeError:
        return None


def stdin_answer(question, timeout_s):
    """Ask the local operator for y/n/s; returns the char or '' on timeout."""
    print("PROMPT: %s [y/n/s, %ds] " % (question, timeout_s), end="", flush=True)
    ready, _, _ = select.select([sys.stdin], [], [], timeout_s)
    if not ready:
        print("<no answer>")
        return ""
    answer = sys.stdin.read(1).lower()
    print(answer)
    return answer if answer in "yns" else ""


def skip_answer(question, timeout_s):
    print("PROMPT: %s -> auto 's' (skip)" % question)
    return "s"


class EvtRunner:
    def __init__(self, ser, out_dir, board_id=None, answer_fn=None,
                 timeout_cap=None, verbose=True):
        self.ser = ser
        self.out_dir = out_dir
        self.board_id = board_id
        self.answer_fn = answer_fn if answer_fn is not None else stdin_answer
        self.timeout_cap = timeout_cap
        self.verbose = verbose
        self.log_lines = []
        self.info = {}
        self.results = {}
        self.summary = None
        self.prompts_answered = []

    def _log(self, line):
        self.log_lines.append(line)
        if self.verbose:
            print(line)

    def _read_line(self, deadline):
        """Read one CR/LF-terminated line; returns None on timeout."""
        data = bytearray()
        while time.monotonic() < deadline:
            chunk = self.ser.read(1)
            if not chunk:
                continue
            if chunk == b"\r":
                continue
            if chunk == b"\n":
                return data.decode("utf-8", errors="replace")
            data += chunk
        return None

    def _handle_prompt(self, payload):
        question = payload.get("question", "operator check")
        timeout_s = int(payload.get("timeout_s", 30))
        answer = self.answer_fn(question, timeout_s)
        if answer:
            # The firmware accepts the first y/n/s byte; the trailing CRLF is
            # ignored by its reader and mirrors a human pressing Enter.
            self.ser.write(answer.encode("ascii") + b"\r\n")
            self.prompts_answered.append(
                {"test": payload.get("test"), "answer": answer})

    def _run_stage(self, command, timeout_s, expect):
        if self.timeout_cap is not None:
            timeout_s = min(timeout_s, self.timeout_cap)
        self._log(">>> " + command)
        self.ser.write(command.encode("ascii") + b"\r\n")
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            line = self._read_line(deadline)
            if line is None:
                break
            self._log(line)
            payload = parse_payload(line, PROMPT_PREFIX)
            if payload is not None:
                # The board applies its own prompt timeout; answering simply
                # consumes part of the stage budget.
                self._handle_prompt(payload)
                continue
            payload = parse_payload(line, RESULT_PREFIX)
            if payload is not None:
                self.results[payload.get("test", "?")] = payload
                if expect == payload.get("test"):
                    return True
                continue
            payload = parse_payload(line, INFO_PREFIX)
            if payload is not None:
                self.info = payload
                if expect == "INFO":
                    return True
        return False

    def _send_and_drain(self, command, settle_s):
        """Send a command with no FACTORY_RESULT and drain its text output.

        peripheral_power only prints on error, so a short fixed window keeps
        the log ordered without stalling the flow.
        """
        self._log(">>> " + command)
        self.ser.write(command.encode("ascii") + b"\r\n")
        deadline = time.monotonic() + settle_s
        while time.monotonic() < deadline:
            line = self._read_line(deadline)
            if line is None:
                break
            self._log(line)

    def _run_i2c_rescan(self, timeout_s):
        """Rescan the main bus; return (i2c_main payload or None, found set)."""
        if self.timeout_cap is not None:
            timeout_s = min(timeout_s, self.timeout_cap)
        self._log(">>> i2c_scan main")
        self.ser.write(b"i2c_scan main\r\n")
        found = set()
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            line = self._read_line(deadline)
            if line is None:
                break
            self._log(line)
            match = FOUND_DEVICE_RE.match(line)
            if match:
                found.add(int(match.group(1), 16))
                continue
            payload = parse_payload(line, PROMPT_PREFIX)
            if payload is not None:
                self._handle_prompt(payload)
                continue
            payload = parse_payload(line, RESULT_PREFIX)
            if payload is not None:
                self.results[payload.get("test", "?")] = payload
                if payload.get("test") == "i2c_main":
                    return payload, found
        return None, found

    def _run_type_c_power_scan(self, timeout_s):
        """Require the as-built FUSB303B address while its domain is on."""
        payload, found = self._run_i2c_rescan(timeout_s)
        findings = []
        if payload is None:
            findings.append("no i2c_main result with Type-C control on")
        if FUSB303B_EXPECTED_ADDRESS not in found:
            findings.append("FUSB303B 0x%02x silent with control on"
                            % FUSB303B_EXPECTED_ADDRESS)
        if FUSB303B_ALT_ADDRESS in found:
            findings.append("unexpected FUSB303B strap address 0x%02x"
                            % FUSB303B_ALT_ADDRESS)
        if findings:
            status = "FAIL"
            detail = "; ".join(findings)
            self._log("!!! %s FAIL: %s" % (TYPE_C_POWER_SCAN, detail))
        else:
            status = "PASS"
            detail = "FUSB303B answered at 0x%02x only while control was on" % (
                FUSB303B_EXPECTED_ADDRESS)
            self._log("%s PASS: %s" % (TYPE_C_POWER_SCAN, detail))
        self.results[TYPE_C_POWER_SCAN] = {
            "test": TYPE_C_POWER_SCAN,
            "status": status,
            "detail": detail,
        }

    def _run_power_rail_scan(self, timeout_s):
        """Toggle each switched-rail peripheral OFF->scan->ON->scan->OFF.

        The firmware grades a switched-rail device as allowed-silent whenever
        its rail reads back off, so only the host can close the loop: a device
        that still answers with its rail off (or stays silent with it back on)
        is recorded here under the synthetic power_rail_scan result. Every
        rail is left OFF at the end of its cycle, and a final scan with all
        rails off confirms none of the devices answers. The stage therefore
        ends with every switched rail off; the board-side i2c_main result
        reflects that all-off state.
        """
        scans = 2 * len(POWER_RAIL_DEVICES) + 1
        per_scan = max(1, timeout_s // scans)
        findings = []
        for name, address in POWER_RAIL_DEVICES:
            for enabled in (False, True):
                self._send_and_drain("peripheral_power %s %s"
                                     % (name, "on" if enabled else "off"), 0.5)
                payload, found = self._run_i2c_rescan(per_scan)
                state = "on" if enabled else "off"
                if payload is None:
                    findings.append("%s: no i2c_main result with rail %s"
                                    % (name, state))
                elif enabled and address not in found:
                    findings.append("%s: 0x%02x silent with rail on"
                                    % (name, address))
                elif not enabled and address in found:
                    findings.append("%s: 0x%02x answered with rail off"
                                    % (name, address))
            # Final OFF for this rail: command only, the closing all-off scan
            # below covers it.
            self._send_and_drain("peripheral_power %s off" % name, 0.5)
        payload, found = self._run_i2c_rescan(per_scan)
        if payload is None:
            findings.append("no i2c_main result for the closing all-off scan")
        else:
            for name, address in POWER_RAIL_DEVICES:
                if address in found:
                    findings.append("%s: 0x%02x answered after the final rail-off"
                                    % (name, address))
        if findings:
            status = "FAIL"
            detail = "; ".join(findings)
            self._log("!!! %s FAIL: %s" % (POWER_RAIL_SCAN, detail))
        else:
            status = "PASS"
            detail = "switched-rail devices answer only while powered; all rails left off"
            self._log("%s PASS: %s" % (POWER_RAIL_SCAN, detail))
        self.results[POWER_RAIL_SCAN] = {
            "test": POWER_RAIL_SCAN,
            "status": status,
            "detail": detail,
        }

    def _run_report_reset(self):
        """Clear results persisted in NVS by an earlier run on this board.

        Factory results survive power cycles, so without a reset a second
        EVT pass could mix stale entries into the board summary and the host
        result map. The command keeps the latest safe_state result, which the
        firmware establishes at boot and may re-evaluate through the
        owner-aware cleanup command.
        """
        timeout_s = self.timeout_cap or 10
        self._log(">>> report_reset")
        self.ser.write(b"report_reset\r\n")
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            line = self._read_line(deadline)
            if line is None:
                break
            self._log(line)
            if "Factory results reset" in line:
                return True
        self._log("!!! no report_reset acknowledgment within %ds" % timeout_s)
        return False

    def run(self, stages):
        if not self._run_report_reset():
            detail = "report_reset was not acknowledged; no EVT stages were run"
            self.results["report_reset"] = {
                "test": "report_reset",
                "status": "FAIL",
                "detail": detail,
            }
            self.summary = {
                "overall": "FAIL",
                "pass": 0,
                "fail": 1,
                "warn": 0,
                "skip": 0,
                "not_run": 0,
            }
            self._log("!!! aborting EVT run: " + detail)
            return self._write_report()
        for command, timeout_s, expect in stages:
            if command == POWER_RAIL_SCAN:
                # Host-side composite stage; the board has no such command.
                self._run_power_rail_scan(timeout_s)
                continue
            if command == TYPE_C_POWER_SCAN:
                # Host-side composite stage; the board has no such command.
                self._run_type_c_power_scan(timeout_s)
                continue
            if command == POWER_ALL_OFF:
                # Cleanup marker: prints text but no FACTORY_RESULT line.
                self._send_and_drain(command, 2.0)
                continue
            if not self._run_stage(command, timeout_s, expect) and expect != "INFO":
                self.results.setdefault(expect, {
                    "test": expect,
                    "status": "TIMEOUT",
                    "detail": "no FACTORY_RESULT within %ds" % timeout_s,
                })
                self._log("!!! timeout waiting for %s" % expect)

        self._log(">>> report")
        self.ser.write(b"report\r\n")
        deadline = time.monotonic() + (self.timeout_cap or 15)
        while time.monotonic() < deadline:
            line = self._read_line(deadline)
            if line is None:
                break
            self._log(line)
            payload = parse_payload(line, RESULT_PREFIX)
            if payload is not None:
                test = payload.get("test", "?")
                # Keep TIMEOUT markers: those stages never actually ran, and
                # the board-side report only knows its own NOT_RUN state.
                if self.results.get(test, {}).get("status") != "TIMEOUT":
                    self.results[test] = payload
                continue
            payload = parse_payload(line, SUMMARY_PREFIX)
            if payload is not None:
                self.summary = payload
                break

        return self._write_report()

    def _write_report(self):
        board = self.board_id or (
            self.info.get("mac", "unknown").replace(":", "") or "unknown")
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        os.makedirs(self.out_dir, exist_ok=True)
        base = os.path.join(self.out_dir, "evt_%s_%s" % (board, stamp))
        with open(base + ".log", "w", encoding="utf-8") as handle:
            handle.write("\n".join(self.log_lines) + "\n")
        report = {
            "board": board,
            "timestamp": stamp,
            "info": self.info,
            "results": self.results,
            "summary": self.summary,
            "prompts_answered": self.prompts_answered,
        }
        with open(base + ".json", "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)
        print("report written to %s.json / %s.log" % (base, base))
        report["_base"] = base  # consumed by the self test, not persisted
        return report


# ---------------------------------------------------------------------------
# Self test: a scripted fake firmware on the master side of a pty pair.
# ---------------------------------------------------------------------------

FAKE_RESPONSES = {
    "board_info": ['FACTORY_INFO {"board":"Candis-S31","board_revision":"EVT1",'
                   '"mac":"aa:bb:cc:dd:ee:ff","reset":"power_on"}'],
    "safe_state": ['FACTORY_RESULT {"test":"safe_state","status":"PASS","detail":"ok"}'],
    "flash_test": ['FACTORY_RESULT {"test":"flash","status":"PASS","detail":"size=16777216"}'],
    "psram_test": ['FACTORY_RESULT {"test":"psram","status":"PASS","detail":"tested=65536"}'],
    # i2c_scan main is answered from the simulated rail state below instead.
    "i2c_scan lp": ['FACTORY_RESULT {"test":"i2c_low_power","status":"PASS","detail":"devices=2"}'],
    "pmic_test": ['FACTORY_RESULT {"test":"pmic","status":"PASS","detail":"id=0x47"}'],
    "charge_test": ['FACTORY_RESULT {"test":"charge","status":"WARN",'
                    '"detail":"no vbus; charger path not exercised"}'],
    "rtc_test": ['FACTORY_RESULT {"test":"rtc","status":"PASS","detail":"valid=yes"}'],
    "rtc_alarm": ['FACTORY_RESULT {"test":"rtc_alarm","status":"PASS","detail":"fired"}'],
    "irq_test": ['FACTORY_RESULT {"test":"shared_irq","status":"PASS","detail":"released=yes"}'],
    "buttons": ['FACTORY_RESULT {"test":"buttons","status":"FAIL","detail":"boot=yes power=no"}'],
    "typec_test": ['FACTORY_RESULT {"test":"type_c","status":"WARN",'
                   '"detail":"attached but orientation unknown"}'],
    "usb_host_test": ['FACTORY_RESULT {"test":"usb_host","status":"PASS",'
                      '"detail":"vid=303a pid=1001"}'],
    "wifi_scan": ['FACTORY_RESULT {"test":"wifi","status":"PASS","detail":"aps_found=4"}'],
    "ble_smoke": ['FACTORY_RESULT {"test":"ble","status":"PASS","detail":"init ok"}'],
    "display_test": "PROMPT",      # asks, then scores the forwarded answer
    "touch_test": ['FACTORY_RESULT {"test":"touch","status":"PASS","detail":"quadrants=0x0f"}'],
    "display_sleep_test": ['FACTORY_RESULT {"test":"display_sleep","status":"NOT_RUN",'
                           '"detail":"confirmed=0/4; use mark"}'],
    "led_test": ['FACTORY_RESULT {"test":"rgb_led","status":"PASS","detail":"confirmed"}'],
    "sdcard_test": ['FACTORY_RESULT {"test":"sdcard","status":"FAIL","detail":"no card"}'],
    "speaker_test": ['FACTORY_RESULT {"test":"speaker","status":"PASS","detail":"confirmed"}'],
    "microphone_test": ['FACTORY_RESULT {"test":"microphone","status":"PASS","detail":"peak=1200"}'],
    "camera_test": "SILENT",       # exercises the runner timeout path
}


def _self_test():
    import pty
    import tempfile

    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)
    os.set_blocking(master_fd, False)
    received_answers = []
    received_commands = []
    i2c_scan_type_c_states = []
    stop = threading.Event()

    def fake_firmware():
        buffer = b""
        stale_results = True  # results persisted in NVS by an earlier run
        # Boot safe state leaves every optional/control rail off.
        rail_on = {name: False for name, _address in POWER_RAIL_DEVICES}
        type_c_on = False
        while not stop.is_set():
            try:
                chunk = os.read(master_fd, 256)
            except (BlockingIOError, InterruptedError):
                time.sleep(0.01)
                continue
            except OSError:
                break
            if not chunk:
                continue
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                command = line.strip().decode("ascii", errors="replace")
                if len(command) == 1 and command in "yns":
                    received_answers.append(command)
                    status = {"y": "PASS", "n": "FAIL"}.get(command, "NOT_RUN")
                    os.write(master_fd,
                             ('FACTORY_RESULT {"test":"display","status":"%s",'
                              '"detail":"operator answered"}\n' % status).encode())
                    continue
                if command.startswith("rtc_set "):
                    os.write(master_fd,
                             b'FACTORY_RESULT {"test":"rtc","status":"PASS",'
                             b'"detail":"set ok"}\n')
                    continue
                received_commands.append(command)
                if command == "report_reset":
                    stale_results = False
                    os.write(master_fd,
                             b"Factory results reset to NOT_RUN"
                             b" (latest safe_state kept)\n")
                    continue
                if command == "typec_test":
                    type_c_on = True
                if command == POWER_ALL_OFF:
                    type_c_on = False
                    for name in rail_on:
                        rail_on[name] = False
                    continue
                if command.startswith("peripheral_power "):
                    fields = command.split()
                    if len(fields) == 3 and fields[1] in rail_on:
                        rail_on[fields[1]] = fields[2] == "on"
                    continue
                if command == "i2c_scan main":
                    # Match the board contract: FUSB303B answers only while
                    # its direct control domain is enabled; switched-rail
                    # devices answer only while their own rail is on.
                    i2c_scan_type_c_states.append(type_c_on)
                    found = ([0x21] if type_c_on else []) + [
                        address for name, address in POWER_RAIL_DEVICES
                        if rail_on[name]
                    ]
                    for address in sorted(found):
                        os.write(master_fd, ("found I2C device at 0x%02x\n"
                                             % address).encode())
                    note = ("expected set present" if type_c_on else
                            "FUSB303B off with control domain")
                    os.write(master_fd,
                             ('FACTORY_RESULT {"test":"i2c_main","status":"PASS",'
                              '"detail":"%s (devices=%d missing=0x0 extra=0)"}\n'
                              % (note, len(found))).encode())
                    continue
                response = FAKE_RESPONSES.get(command)
                if response == "PROMPT":
                    os.write(master_fd,
                             b'FACTORY_PROMPT {"test":"display",'
                             b'"question":"Pattern ok?","timeout_s":5}\n')
                elif response == "SILENT" or response is None:
                    if command == "report":
                        if stale_results:
                            # The runner must have reset first; a stale
                            # board-side result would flip the summary into
                            # a false PASS.
                            os.write(master_fd,
                                     b'FACTORY_RESULT {"test":"camera",'
                                     b'"status":"PASS","detail":"stale from NVS"}\n'
                                     b'FACTORY_SUMMARY {"overall":"PASS","pass":14,'
                                     b'"fail":1,"warn":2,"skip":0,"not_run":3}\n')
                        else:
                            os.write(master_fd,
                                     b'FACTORY_RESULT {"test":"camera",'
                                     b'"status":"NOT_RUN","detail":"not executed"}\n'
                                     b'FACTORY_SUMMARY {"overall":"FAIL","pass":13,'
                                     b'"fail":2,"warn":2,"skip":0,"not_run":4}\n')
                else:
                    for out_line in response:
                        os.write(master_fd, (out_line + "\n").encode())

    thread = threading.Thread(target=fake_firmware, daemon=True)
    thread.start()

    ser = serial.Serial(slave_name, 115200, timeout=0.05)
    with tempfile.TemporaryDirectory() as out_dir:
        runner = EvtRunner(ser, out_dir, answer_fn=lambda q, t: "y",
                           timeout_cap=2, verbose=False)
        report = runner.run(build_stages())
        base = report.pop("_base")

        results = report["results"]
        failures = []

        def check(condition, message):
            if not condition:
                failures.append(message)

        check(report["board"] == "aabbccddeeff",
              "board id should come from the FACTORY_INFO MAC, got %r"
              % report["board"])
        check(received_commands and received_commands[0] == "report_reset",
              "report_reset must precede the first test stage, first commands: %r"
              % received_commands[:4])
        check("report" in received_commands and
              received_commands.index("report_reset") <
              received_commands.index("report"),
              "report_reset must be acknowledged before the final report")
        scan_positions = [i for i, command in enumerate(received_commands)
                          if command == "i2c_scan main"]
        type_c_position = received_commands.index("typec_test")
        usb_host_position = received_commands.index("usb_host_test")
        check(len(scan_positions) >= 3 and
              scan_positions[0] < type_c_position <
              scan_positions[1] < usb_host_position,
              "main-bus scan order must prove Type-C off then on before USB "
              "Host ownership, commands: %r" % received_commands)
        check(i2c_scan_type_c_states[0] is False and
              any(i2c_scan_type_c_states) and
              i2c_scan_type_c_states[-1] is False,
              "main-bus scans must cover FUSB303B off, on, and final-off "
              "states: %r" % i2c_scan_type_c_states)
        # Stage-boundary cleanup: power_all_off follows the display/touch,
        # LED, audio, and camera blocks, in that order.
        cleanups = [i for i, command in enumerate(received_commands)
                    if command == POWER_ALL_OFF]
        anchors = [command for command in
                   ("display_sleep_test", "led_test", "microphone_test",
                    "camera_test")
                   if command in received_commands]
        anchor_positions = [received_commands.index(command)
                            for command in anchors]
        check(len(cleanups) == 4 and len(anchor_positions) == 4 and
              all(anchor_positions[n] < cleanups[n] and
                  (n == 3 or cleanups[n] < anchor_positions[n + 1])
                  for n in range(4)),
              "power_all_off must follow each display/LED/audio/camera block, "
              "commands: %r" % received_commands)
        check(report["summary"]["overall"] == "FAIL",
              "stale NVS results must not leak into the final summary, got %r"
              % report["summary"])
        check(results["flash"]["status"] == "PASS", "flash PASS path broken")
        check(results["buttons"]["status"] == "FAIL", "buttons FAIL path broken")
        check(results["charge"]["status"] == "WARN", "charge WARN path broken")
        check(results["display_sleep"]["status"] == "NOT_RUN",
              "display_sleep NOT_RUN path broken")
        check(results["display"]["status"] == "PASS",
              "prompt y-answer should score display PASS")
        check(received_answers == ["y"],
              "prompt answer was not forwarded to the board: %r" % received_answers)
        check(report["prompts_answered"] == [{"test": "display", "answer": "y"}],
              "prompts_answered mismatch: %r" % report["prompts_answered"])
        check(results["camera"]["status"] == "TIMEOUT",
              "silent camera command should record TIMEOUT, got %r"
              % results["camera"])
        check(results["power_rail_scan"]["status"] == "PASS",
              "well-behaved fake rails should score power_rail_scan PASS, got %r"
              % results.get("power_rail_scan"))
        check(results["type_c_power_scan"]["status"] == "PASS",
              "FUSB303B powered scan should pass, got %r"
              % results.get("type_c_power_scan"))
        for name, _address in POWER_RAIL_DEVICES:
            off_command = "peripheral_power %s off" % name
            on_command = "peripheral_power %s on" % name
            offs = [i for i, command in enumerate(received_commands)
                    if command == off_command]
            ons = [i for i, command in enumerate(received_commands)
                   if command == on_command]
            check(len(offs) == 2 and len(ons) == 1 and
                  offs[0] < ons[0] < offs[1],
                  "power_rail_scan must run %s OFF->scan->ON->scan->OFF, "
                  "commands: %r" % (name, received_commands))
        power_positions = [i for i, command in enumerate(received_commands)
                           if command.startswith("peripheral_power ")]
        scan_positions = [i for i, command in enumerate(received_commands)
                          if command == "i2c_scan main"]
        check(power_positions and scan_positions and
              cleanups and cleanups[-1] < power_positions[0] and
              scan_positions[-1] > power_positions[-1],
              "the rail scan must start after the last cleanup and end with "
              "an all-off scan, commands: %r" % received_commands)
        check(report["summary"] is not None and
              report["summary"]["overall"] == "FAIL" and
              report["summary"]["warn"] == 2,
              "summary parse failed: %r" % report["summary"])
        check(os.path.exists(base + ".json") and os.path.exists(base + ".log"),
              "report files were not written")
        check(len(results) == len(set(s[2] for s in build_stages() if s[2] != "INFO")),
              "expected one result per unique stage name, got %d" % len(results))

        class SilentSerial:
            def __init__(self):
                self.writes = []

            def write(self, data):
                self.writes.append(data)

            def read(self, size):
                return b""

        silent_serial = SilentSerial()
        reset_runner = EvtRunner(silent_serial, out_dir, board_id="reset-failure",
                                 timeout_cap=0.01, verbose=False)
        reset_report = reset_runner.run([("safe_state", 1, "safe_state")])
        check(silent_serial.writes == [b"report_reset\r\n"],
              "a missing report_reset acknowledgment must abort before every "
              "EVT stage, writes: %r" % silent_serial.writes)
        check(reset_report["summary"]["overall"] == "FAIL" and
              reset_report["results"]["report_reset"]["status"] == "FAIL",
              "report_reset failure must produce a failed host report: %r"
              % reset_report)

    stop.set()
    ser.close()
    os.close(master_fd)
    os.close(slave_fd)
    thread.join(timeout=2)

    if failures:
        for failure in failures:
            print("SELF-TEST FAIL:", failure)
        return 1
    print("self-test passed: report_reset acknowledgment gates every stage, "
          "main-bus scans prove FUSB303B off then 0x21-on before USB Host and "
          "off again at teardown, PASS/FAIL/WARN/NOT_RUN/TIMEOUT paths, "
          "prompt forwarding, rail OFF->scan->ON->scan->OFF with a closing "
          "all-off scan, summary parse, and report files all verified")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port, for example /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--board-id",
                        help="override the board identity used in file names")
    parser.add_argument("--out", default="evt_logs", help="output directory")
    parser.add_argument("--non-interactive", action="store_true",
                        help="answer 's' (skip) to every operator prompt")
    parser.add_argument("--no-power-rail-scan", action="store_true",
                        help="skip the optional stage that toggles each switched "
                             "peripheral rail and rescans the main I2C bus")
    parser.add_argument("--self-test", action="store_true",
                        help="run the pty-based self test and exit")
    args = parser.parse_args()

    if args.self_test:
        return _self_test()

    if not args.port:
        parser.error("--port is required unless --self-test is given")

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    runner = EvtRunner(ser, args.out, board_id=args.board_id,
                       answer_fn=skip_answer if args.non_interactive else None)
    report = runner.run(build_stages(power_rail_scan=not args.no_power_rail_scan))
    summary = report.get("summary") or {}
    print("overall:", summary.get("overall", "no summary received"))
    return 0 if summary.get("overall") in ("PASS", "WARN") else 1


if __name__ == "__main__":
    sys.exit(main())
