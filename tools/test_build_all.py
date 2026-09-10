#!/usr/bin/env python3
"""Host tests for tools/build-all.sh — no real ESP-IDF, no network.

build-all.sh is exercised through a sandbox copy of the repository tree with a
fake idf.py on PATH that records every invocation as JSON. The fake simulates
just enough of idf.py (global-option parsing, sdkconfig generation, build-dir
creation, configurable failures) to pin down the script's contract:

  * idf.py global options precede the action verb (argument order),
  * --isolated gives every target its own build dir + sdkconfig under the
    run's log dir and does not override SDKCONFIG_DEFAULTS (the config is
    generated from the project's sdkconfig.defaults),
  * project-local sdkconfigs are never modified or deleted — a stale one
    fails the target in default mode and is bypassed by --isolated,
  * --help/--list work with no idf.py on PATH and have no side effects,
  * exit codes: 0 all pass, 1 target failure, 2 usage error.

Run: python3 tools/test_build_all.py [-v]
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
SCRIPT = TOOLS_DIR / "build-all.sh"

# Stand-in idf.py. Behaviour:
#   * logs one JSON line per invocation to $CANDIS_FAKE_IDF_LOG ({cwd, argv, rc}),
#   * `--version` prints a banner, exits 0,
#   * `build` creates the -B dir with a marker file and generates the
#     sdkconfig (at -D SDKCONFIG=..., else ./sdkconfig) only when it does not
#     exist yet — exactly where real idf.py would put it — then exits 0,
#   * if cwd contains $CANDIS_FAKE_IDF_FAIL_SUBSTR the build exits with
#     $CANDIS_FAKE_IDF_FAIL_RC (default 42) without touching anything.
FAKE_IDF = r"""#!/usr/bin/env python3
import json
import os
import sys


def main():
    args = sys.argv[1:]
    rec = {"cwd": os.getcwd(), "argv": args}
    if args[:1] == ["--version"]:
        print("fake-idf: ESP-IDF v6.1-rc1 (fake)")
        rc = 0
    elif args[-1:] == ["build"]:
        bdir = None
        defines = {}
        bad = None
        i = 0
        while i < len(args) - 1:
            tok = args[i]
            if tok == "--preview":
                i += 1
            elif tok in ("-B", "-D") and i + 2 <= len(args) - 1:
                if tok == "-B":
                    bdir = args[i + 1]
                else:
                    key, _, val = args[i + 1].partition("=")
                    defines[key] = val
                i += 2
            else:
                bad = tok
                break
        fail_substr = os.environ.get("CANDIS_FAKE_IDF_FAIL_SUBSTR")
        if bad is not None:
            print("fake-idf: unexpected global option %r" % bad, file=sys.stderr)
            rc = 99
        elif fail_substr and fail_substr in os.getcwd():
            print("fake-idf: simulated build failure", file=sys.stderr)
            rc = int(os.environ.get("CANDIS_FAKE_IDF_FAIL_RC", "42"))
        else:
            if bdir:
                os.makedirs(bdir, exist_ok=True)
                with open(os.path.join(bdir, "fake-build-ok"), "w") as f:
                    f.write("ok\n")
            sdkconfig = defines.get("SDKCONFIG") or os.path.join(os.getcwd(), "sdkconfig")
            if not os.path.exists(sdkconfig):
                with open(sdkconfig, "w") as f:
                    f.write('CONFIG_IDF_TARGET="%s"\n' % defines.get("IDF_TARGET", ""))
            print("fake-idf: built target=%s sdkconfig=%s"
                  % (defines.get("IDF_TARGET"), sdkconfig))
            rc = 0
    else:
        print("fake-idf: unsupported invocation: %r" % (args,), file=sys.stderr)
        rc = 99
    rec["rc"] = rc
    with open(os.environ["CANDIS_FAKE_IDF_LOG"], "a") as f:
        f.write(json.dumps(rec) + "\n")
    sys.exit(rc)


main()
"""

# Mirror of build-all.sh's dir mapping, used to create sandbox fixtures.
FIRMWARE_ROOT_DIRS = {"factory": "firmware/factory"}
TESTAPP_ROOT_DIRS = {
    "candis_s31": "components/candis_s31/test_apps",
    "cst820": "vendor/idf-extra-components/esp_lcd_touch_cst820/test_apps",
}


def expected_targets():
    """Parse the target table out of the script instead of duplicating it."""
    src = SCRIPT.read_text()
    fw = re.search(r"^FIRMWARE_TARGETS=\(([^)]*)\)", src, re.M).group(1).split()
    ta = re.search(r"^TEST_APPS=\(([^)]*)\)", src, re.M).group(1).split()
    return fw + ["testapp:%s" % t for t in ta]


def target_dir_rel(target):
    if target.startswith("testapp:"):
        name = target[len("testapp:"):]
        return TESTAPP_ROOT_DIRS.get(
            name, "vendor/idf-extra-components/%s/test_apps" % name)
    return FIRMWARE_ROOT_DIRS.get(target, "examples/esp-idf/%s" % target)


def parse_build_argv(argv):
    """Split a fake idf.py build invocation into its global options.

    Fails unless every token before the trailing `build` action is an idf.py
    global option (-B dir, -D KEY=VALUE, --preview): global options must
    precede the action verb.
    """
    assert argv[-1] == "build", argv
    opts = {"preview": False, "B": None, "D": {}}
    rest = argv[:-1]
    i = 0
    while i < len(rest):
        tok = rest[i]
        if tok == "--preview":
            opts["preview"] = True
            i += 1
        elif tok in ("-B", "-D"):
            assert i + 2 <= len(rest), "dangling %s in %r" % (tok, argv)
            if tok == "-B":
                opts["B"] = rest[i + 1]
            else:
                key, _, val = rest[i + 1].partition("=")
                opts["D"][key] = val
            i += 2
        else:
            raise AssertionError("non-option %r before action verb in %r" % (tok, argv))
    return opts


class Harness:
    """Sandbox: a copy of build-all.sh with a fake idf.py on PATH."""

    def __init__(self, root: Path):
        self.root = root
        self.repo = root / "repo"
        self.tools = self.repo / "tools"
        self.tools.mkdir(parents=True)
        shutil.copy2(SCRIPT, self.tools / "build-all.sh")
        (self.tools / "build-all.sh").chmod(0o755)
        self.bin = root / "bin"
        self.bin.mkdir()
        (self.bin / "idf.py").write_text(FAKE_IDF)
        (self.bin / "idf.py").chmod(0o755)
        self.logs = root / "build-logs"
        self.logs.mkdir()
        self.ws = root / "ws"
        self.ws.mkdir()  # no .tools/esp-idf: IDF activation here would die(2)
        self.idf_log = root / "idf-invocations.jsonl"
        self.idf_log.touch()
        self.extra_env = {}

    def env(self, fake_idf=True):
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("IDF_", "CANDIS_FAKE_"))
               and k not in ("CANDIS_WS", "BUILD_LOG_ROOT")}
        env["PATH"] = os.pathsep.join(
            [str(self.bin), "/usr/bin", "/bin"] if fake_idf else ["/usr/bin", "/bin"])
        env["HOME"] = str(self.root)
        env["CANDIS_WS"] = str(self.ws)
        env["BUILD_LOG_ROOT"] = str(self.logs)
        if fake_idf:
            env["CANDIS_FAKE_IDF_LOG"] = str(self.idf_log)
        env.update(self.extra_env)
        return env

    def run(self, *args, fake_idf=True):
        return subprocess.run(
            [str(self.tools / "build-all.sh")] + list(args),
            env=self.env(fake_idf), cwd=str(self.root),
            capture_output=True, text=True, timeout=120)

    def project(self, rel):
        d = self.repo / rel
        d.mkdir(parents=True, exist_ok=True)
        return d

    @property
    def run_dir(self):
        return (self.logs / "latest").resolve(strict=True)

    def invocations(self):
        return [json.loads(line)
                for line in self.idf_log.read_text().splitlines() if line.strip()]

    def builds(self):
        return [r for r in self.invocations() if r["argv"][-1] == "build"]

    def summary(self):
        return (self.run_dir / "summary.txt").read_text()


class BuildAllTestBase(unittest.TestCase):
    def setUp(self):
        root = Path(tempfile.mkdtemp(prefix="build-all-test-")).resolve()
        self.h = Harness(root)
        self.addCleanup(shutil.rmtree, str(root), True)

    def assertSummaryRow(self, name, result):
        self.assertRegex(self.h.summary(),
                         re.compile(r"^%s\s+%s" % (re.escape(name), result), re.M))


class HelpAndListWithoutIdf(BuildAllTestBase):
    """--help/--list must work with no idf.py on PATH and no IDF tree."""

    def test_help_without_idf(self):
        # No idf.py on PATH and no $CANDIS_WS/.tools/esp-idf/export.sh: if the
        # script attempted IDF activation here it would exit 2.
        for flag in ("--help", "-h"):
            with self.subTest(flag=flag):
                r = self.h.run(flag, fake_idf=False)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("--isolated", r.stdout)
                self.assertIn("--list", r.stdout)
                self.assertIn("sdkconfig", r.stdout)

    def test_list_without_idf_matches_target_table(self):
        r = self.h.run("--list", fake_idf=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.splitlines(), expected_targets())

    def test_help_and_list_have_no_side_effects(self):
        for flag in ("--help", "--list"):
            r = self.h.run(flag, fake_idf=False)
            self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(list(self.h.logs.iterdir()), [])  # no RUN_DIR created
        self.assertFalse((self.h.logs / "latest").exists())


class DefaultMode(BuildAllTestBase):
    def test_firmware_command_shape_unchanged(self):
        factory = self.h.project("firmware/factory")
        r = self.h.run("factory")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        builds = self.h.builds()
        self.assertEqual(len(builds), 1)
        opts = parse_build_argv(builds[0]["argv"])
        self.assertTrue(opts["preview"])
        self.assertIsNone(opts["B"])  # default ./build inside the project
        self.assertEqual(opts["D"], {"IDF_TARGET": "esp32s31"})  # no SDKCONFIG override
        self.assertIn('CONFIG_IDF_TARGET="esp32s31"', (factory / "sdkconfig").read_text())
        self.assertFalse((self.h.run_dir / "iso").exists())

    def test_testapp_command_shape_unchanged(self):
        self.h.project("components/candis_s31/test_apps")
        r = self.h.run("testapp:candis_s31")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        opts = parse_build_argv(self.h.builds()[0]["argv"])
        self.assertEqual(opts["B"], "build_esp32s31")
        self.assertEqual(opts["D"], {"IDF_TARGET": "esp32s31"})

    def test_stale_sdkconfig_fails_target_and_is_preserved(self):
        factory = self.h.project("firmware/factory")
        original = 'CONFIG_IDF_TARGET="esp32"\n# USER-TWEAK-42\n'
        (factory / "sdkconfig").write_text(original)
        r = self.h.run("factory")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.h.builds(), [])  # idf.py never invoked for the target
        self.assertEqual((factory / "sdkconfig").read_text(), original)  # byte-identical
        self.assertIn("--isolated", r.stdout)  # failure explains the remedy
        self.assertSummaryRow("factory", "FAIL")

    def test_matching_sdkconfig_builds_and_is_left_alone(self):
        factory = self.h.project("firmware/factory")
        original = 'CONFIG_IDF_TARGET="esp32s31"\n# USER-TWEAK-42\n'
        (factory / "sdkconfig").write_text(original)
        r = self.h.run("factory")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(len(self.h.builds()), 1)
        self.assertEqual((factory / "sdkconfig").read_text(), original)

    def test_unknown_target_is_a_usage_error(self):
        self.assertEqual(self.h.run("nope").returncode, 2)
        self.assertEqual(self.h.run("--isolated", "nope").returncode, 2)

    def test_missing_project_dir_fails_run(self):
        r = self.h.run("testapp:cst820")
        self.assertEqual(r.returncode, 1)
        self.assertIn("missing dir", self.h.summary())


class IsolatedMode(BuildAllTestBase):
    def test_arg_order_and_per_target_isolation(self):
        factory = self.h.project("firmware/factory")
        wifi = self.h.project("examples/esp-idf/wifi")
        r = self.h.run("--isolated", "factory", "wifi")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

        run_dir = self.h.run_dir
        builds = self.h.builds()
        self.assertEqual(len(builds), 2)
        for rec, name, proj in zip(builds, ("factory", "wifi"), (factory, wifi)):
            opts = parse_build_argv(rec["argv"])
            self.assertTrue(opts["preview"])
            self.assertEqual(opts["B"], str(run_dir / "iso" / name / "build"))
            self.assertEqual(sorted(opts["D"]), ["IDF_TARGET", "SDKCONFIG"])
            self.assertEqual(opts["D"]["SDKCONFIG"],
                             str(run_dir / "iso" / name / "sdkconfig"))
            self.assertEqual(opts["D"]["IDF_TARGET"], "esp32s31")
            # Each target generated its own fresh sdkconfig.
            cfg = Path(opts["D"]["SDKCONFIG"])
            self.assertIn('CONFIG_IDF_TARGET="esp32s31"', cfg.read_text())
            self.assertTrue((Path(opts["B"]) / "fake-build-ok").is_file())
            # Nothing leaked into the project tree.
            self.assertEqual(os.listdir(proj), [])

        self.assertEqual(sorted(os.listdir(run_dir / "iso")), ["factory", "wifi"])
        self.assertIn("fake-idf: built target=esp32s31",
                      (run_dir / "factory.log").read_text())
        self.assertSummaryRow("factory", "PASS")
        self.assertSummaryRow("wifi", "PASS")

    def test_every_target_gets_its_own_config(self):
        targets = expected_targets()
        for target in targets:
            self.h.project(target_dir_rel(target))
        r = self.h.run("--isolated")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        run_dir = self.h.run_dir
        builds = self.h.builds()
        self.assertEqual(len(builds), len(targets))
        sdkconfigs = {parse_build_argv(b["argv"])["D"]["SDKCONFIG"] for b in builds}
        builddirs = {parse_build_argv(b["argv"])["B"] for b in builds}
        self.assertEqual(len(sdkconfigs), len(targets))  # all distinct
        self.assertEqual(len(builddirs), len(targets))
        for path in sdkconfigs:
            self.assertTrue(path.startswith(str(run_dir / "iso")))
            self.assertIn('CONFIG_IDF_TARGET="esp32s31"', Path(path).read_text())
        self.assertIn("TOTAL pass=%d fail=0" % len(targets), self.h.summary())

    def test_isolated_bypasses_stale_project_sdkconfig_without_touching_it(self):
        factory = self.h.project("firmware/factory")
        original = 'CONFIG_IDF_TARGET="esp32"\n# USER-TWEAK-42\n'
        (factory / "sdkconfig").write_text(original)
        r = self.h.run("--isolated", "factory")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((factory / "sdkconfig").read_text(), original)  # untouched
        self.assertEqual(os.listdir(factory), ["sdkconfig"])  # no build dir either
        opts = parse_build_argv(self.h.builds()[0]["argv"])
        iso_cfg = Path(opts["D"]["SDKCONFIG"])
        self.assertIn('CONFIG_IDF_TARGET="esp32s31"', iso_cfg.read_text())
        self.assertNotIn("USER-TWEAK-42", iso_cfg.read_text())  # fresh, not copied

    def test_isolated_flag_accepted_after_targets(self):
        self.h.project("firmware/factory")
        r = self.h.run("factory", "--isolated")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        opts = parse_build_argv(self.h.builds()[0]["argv"])
        self.assertIn(str(self.h.run_dir / "iso" / "factory"), opts["B"])

    def test_relative_log_root_is_resolved_before_project_chdir(self):
        self.h.project("firmware/factory")
        self.h.extra_env["BUILD_LOG_ROOT"] = "build-logs"
        r = self.h.run("--isolated", "factory")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        opts = parse_build_argv(self.h.builds()[0]["argv"])
        self.assertEqual(opts["B"], str(self.h.run_dir / "iso" / "factory" / "build"))
        self.assertTrue(Path(opts["D"]["SDKCONFIG"]).is_absolute())


class FailureCodes(BuildAllTestBase):
    def test_target_failure_returns_1_and_does_not_stop_queue(self):
        factory = self.h.project("firmware/factory")
        wifi = self.h.project("examples/esp-idf/wifi")
        self.h.extra_env["CANDIS_FAKE_IDF_FAIL_SUBSTR"] = str(wifi.resolve())

        r = self.h.run("factory", "wifi")
        self.assertEqual(r.returncode, 1)
        builds = self.h.builds()
        self.assertEqual([b["cwd"] for b in builds],
                         [str(factory.resolve()), str(wifi.resolve())])
        self.assertEqual([b["rc"] for b in builds], [0, 42])
        self.assertSummaryRow("factory", "PASS")
        self.assertSummaryRow("wifi", "FAIL")
        self.assertIn("rc=42", self.h.summary())  # subcommand exit code preserved
        self.assertIn("TOTAL pass=1 fail=1", self.h.summary())

        # A failing target first must not stop the later one either.
        r = self.h.run("wifi", "factory")
        self.assertEqual(r.returncode, 1)
        tail = self.h.builds()[-2:]
        self.assertEqual([b["cwd"] for b in tail],
                         [str(wifi.resolve()), str(factory.resolve())])
        self.assertEqual([b["rc"] for b in tail], [42, 0])
        self.assertIn("TOTAL pass=1 fail=1", self.h.summary())


if __name__ == "__main__":
    unittest.main()
