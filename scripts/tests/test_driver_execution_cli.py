"""Kernel-driver persistent CLI and public DLL agreement; no Ring3 adapter."""
import argparse
import ctypes
import json
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest

from test_execution_cli import LiveCLI, NeverDExecutionOptionsV1, HostAPI

OPTIONS = None


class DriverExecutionCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if OPTIONS is None:
            raise unittest.SkipTest("configured driver/CLI/library paths required")
        cls.cli = Path(OPTIONS.neverd).resolve()
        cls.fixtures = Path(OPTIONS.fixtures).resolve()
        library = Path(OPTIONS.library).resolve()
        cls.directory = os.add_dll_directory(str(library.parent)) if sys.platform == "win32" else None
        cls.api = HostAPI(ctypes.CDLL(str(library)))

    @classmethod
    def tearDownClass(cls):
        if cls.directory:
            cls.directory.close()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="neverd-driver-execution-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        source, error = self.call("neverd_module_set_create_v1", None)
        self.assertTrue(source, error)
        self.source = source
        self.addCleanup(lambda: self.call("neverd_module_set_destroy", source))
        declaration = {"id": "driver", "image": str(self.fixtures / "driver_invocation.sys"),
                       "debug": {"kind": "pdb", "path": str(self.fixtures / "driver_invocation.pdb")}}
        result, error = self.call("neverd_module_set_add_json", source, json.dumps(declaration).encode())
        self.assertEqual(result, 1, error)

    def take(self, pointer):
        if not pointer:
            return ""
        try:
            return ctypes.string_at(pointer).decode("utf-8")
        finally:
            self.api.function("neverd_free_string")(ctypes.cast(pointer, ctypes.c_char_p))

    def call(self, name, *args):
        error = ctypes.c_void_p()
        result = self.api.function(name)(*args, ctypes.byref(error))
        return result, self.take(error.value) if error.value else None

    def address(self, name):
        value, error = self.call("neverd_module_set_lookup_symbol_json", self.source,
                                 b"driver", name.encode())
        self.assertTrue(value, error)
        symbol = json.loads(self.take(value))
        self.assertTrue(symbol["found"])
        return int(symbol["symbol"]["address"], 16) - 0x180000000 + 0x190000000

    def config(self):
        return {"environment": {"kind": "windows-driver", "image": "driver_invocation.sys",
                "base_directory": str(self.fixtures), "backend": "unicorn",
                "contract": "checked-x64-v1", "scenario": {"load_address": "0x190000000"}},
                "run": {"pause_stages": ["after-entry"],
                        "limits": {"instructions": 100000, "events": 10000,
                                   "timeout_microseconds": 5000123}}}

    def live(self, config=None, modules=None):
        path = self.root / "execution.json"
        path.write_text(json.dumps(self.config() if config is None else config), encoding="utf-8")
        return LiveCLI(self, self.cli, path, modules)

    def direct(self, config=None):
        text = json.dumps(self.config() if config is None else config).encode()
        options = NeverDExecutionOptionsV1(ctypes.sizeof(NeverDExecutionOptionsV1), self.source, text)
        run, error = self.call("neverd_execution_create_v1", ctypes.byref(options))
        self.assertTrue(run, error)
        self.addCleanup(lambda: self.call("neverd_execution_destroy", run))
        return run

    def direct_command(self, run, op, **fields):
        text = json.dumps({"id": "direct", "op": op, **fields}).encode()
        result, error = self.call("neverd_execution_command_json", run, text)
        self.assertTrue(result, error)
        return json.loads(self.take(result))["result"]

    def test_kernel_call_and_report_match_the_public_dll(self):
        cli = self.live()
        direct = self.direct()
        self.assertEqual(cli.initial["result"]["instructions"], "0x0")
        self.assertEqual(cli.initial["result"]["environment_report"]["nt_status"], None)
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        self.assertEqual(self.direct_command(direct, "run")["reason"], "stage")
        request = {"entry": hex(self.address("ProbeNotify")), "abi": "win64",
                   "arguments": [{"value": 5}]}
        self.assertTrue(cli.request("invoke", "invoke", invocation=request)["ok"])
        self.direct_command(direct, "invoke", invocation=request)
        left = cli.request("call", "run")["result"]
        right = self.direct_command(direct, "run")
        for event in (left, right):
            self.assertEqual(event["reason"], "invocation_complete")
            self.assertEqual(event["stage"], "after-entry")
            self.assertEqual(event["invocation"]["value"], "0x12")
        self.assertEqual(left["invocation"]["instructions"], right["invocation"]["instructions"])
        left = cli.request("inspect", "inspect")["result"]["environment_report"]
        right = self.direct_command(direct, "inspect")["environment_report"]
        self.assertEqual(left["calls"], right["calls"])
        self.assertEqual(left["nt_status"], 0)
        self.assertTrue(left["observation_only"])
        cli.request("resume", "resume")
        self.assertEqual(cli.request("finish", "run")["result"]["state"], "completed")
        self.assertFalse(cli.request("final", "inspect")["result"]["environment_report"]["observation_only"])
        cli.request("quit", "quit")
        cli.finish()

    def test_repeated_buffers_and_exact_tail_limit(self):
        cli = self.live()
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        request = {"entry": hex(self.address("ProbeBuffers")), "abi": "win64",
                   "arguments": [{"buffer": 0}, {"value": 4}],
                   "buffers": [{"size": 4, "input_hex": "0080ff01"}]}
        for index, expected in enumerate(("29a9d628", "2aaad52b")):
            self.assertTrue(cli.request(f"invoke{index}", "invoke", invocation=request)["ok"])
            event = cli.request(f"run{index}", "run")["result"]
            self.assertEqual(event["invocation"]["buffers"][0]["bytes_hex"], expected)
        request = {"entry": hex(self.address("ProbeModelTail")), "abi": "win64",
                   "limits": {"instructions": 1}}
        self.assertTrue(cli.request("tail", "invoke", invocation=request)["ok"])
        event = cli.request("tailrun", "run")["result"]
        self.assertEqual(event["reason"], "invocation_complete")
        self.assertEqual(event["invocation"]["instructions"], "0x1")
        self.assertEqual(event["invocation"]["value"], "0x0")
        cli.request("quit", "quit")
        cli.finish()

    def test_bad_abi_preserves_the_stopped_stage_and_credits(self):
        cli = self.live()
        cli.request("entry", "run")
        before = cli.request("before", "inspect")["result"]
        request = {"entry": hex(self.address("ProbeSum")), "abi": "sysv-amd64"}
        response = cli.request("badabi", "invoke", invocation=request)
        self.assertFalse(response["ok"])
        after = cli.request("after", "inspect")["result"]
        self.assertEqual(after["event"], before["event"])
        self.assertEqual(after["instructions"], before["instructions"])
        self.assertEqual(after["events"], before["events"])
        cli.request("quit", "quit")
        cli.finish(2)

    def test_documented_driver_configuration_reaches_after_entry(self):
        document = Path(__file__).resolve().parents[2] / "docs/runtime-modules.md"
        section = document.read_text(encoding="utf-8").split(
            "## Persistent Windows driver environment", 1)[1]
        config = json.loads(section.split("```json\n", 1)[1].split("```", 1)[0])
        config["environment"]["base_directory"] = str(self.fixtures)
        config["environment"]["image"] = "driver_invocation.sys"
        path = self.root / "documented.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        cli = LiveCLI(self, self.cli, path)
        self.assertEqual(cli.initial["result"]["instructions"], "0x0")
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        self.assertEqual(cli.request("inspect", "inspect")["result"]["environment_report"]["nt_status"], 0)
        cli.request("quit", "quit")
        cli.finish()

    def test_named_resources_supply_driver_and_kernel_without_loading_unused_files(self):
        modules = {"base_directory": str(self.fixtures),
                   "module_defaults": {"load": "lazy"},
                   "modules": [
                       {"id": "entry", "image": "success.sys"},
                       {"id": "kernel", "image": "driver_kernel_debugger_reference.sys",
                        "debug": {"kind": "pdb", "path": "driver_kernel_debugger_reference.pdb"}},
                       {"id": "unused", "image": "not-provided.sys"}]}
        config = {"environment": {"kind": "windows-driver", "image_module": "entry",
                   "kernel_module": "kernel", "backend": "unicorn", "contract": "checked-x64-v1",
                   "scenario": {"windows_version": {"major": 10, "minor": 0, "build": 19041}}},
                  "run": {"pause_stages": ["after-entry"]}}
        module_path = self.root / "modules.json"
        module_path.write_text(json.dumps(modules), encoding="utf-8")
        config_path = self.root / "execution-from-modules.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        cli = LiveCLI(self, self.cli, config_path, module_path)
        self.assertEqual(cli.initial["result"]["instructions"], "0x0")
        report = cli.request("sources", "modules")["result"]
        rows = {row["id"]: row for row in report["modules"]}
        self.assertTrue(rows["entry"]["prepared"])
        self.assertTrue(rows["kernel"]["prepared"])
        self.assertFalse(rows["unused"]["prepared"])
        inputs = cli.initial["result"]["environment_report"]["input_modules"]
        self.assertEqual({row["module_id"] for row in inputs}, {"entry", "kernel"})
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        observed = cli.request("inspect", "inspect")["result"]["environment_report"]
        self.assertEqual(observed["nt_status"], 0)
        self.assertEqual(observed["configuration"]["kernel_image_sha256"], rows["kernel"]["image_sha256"])
        self.assertEqual(observed["configuration"]["kernel_pdb_sha256"], rows["kernel"]["debug_sha256"])
        cli.request("quit", "quit")
        cli.finish()

    def test_guest_kernel_module_is_published_and_called_by_cli_and_dll(self):
        declaration = {"id": "support", "image": str(self.fixtures / "kernel_module_support.sys"),
                       "debug": {"kind": "pdb", "path": str(self.fixtures / "kernel_module_support.pdb")}}
        result, error = self.call("neverd_module_set_add_json", self.source, json.dumps(declaration).encode())
        self.assertEqual(result, 1, error)
        modules = self.root / "guest-modules.json"
        modules.write_text(json.dumps({"modules": [declaration]}), encoding="utf-8")
        config = self.config()
        config["bindings"] = [{"id": "helper", "module_id": "support", "guest_name": "helper.sys",
                               "scope": "kernel", "address_space": "kernel", "base": "0x1b0000000"}]
        cli = self.live(config, modules)
        direct = self.direct(config)
        self.assertTrue(cli.initial["result"]["environment_report"]["capabilities"]["module_publication"])
        self.assertFalse(next(row for row in cli.request("before", "instances")["result"]["instances"]
                              if row["id"] == "helper")["mapped"])
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        self.assertEqual(self.direct_command(direct, "run")["reason"], "stage")
        left = cli.request("lookup", "lookup", instance_id="helper", symbol="SupportProbe")["result"]
        right = self.direct_command(direct, "lookup", instance_id="helper", symbol="SupportProbe")
        self.assertEqual(left, right)
        request = {"entry": left["address"], "abi": "win64", "arguments": [{"value": 1}]}
        self.assertTrue(cli.request("invoke", "invoke", invocation=request)["ok"])
        self.direct_command(direct, "invoke", invocation=request)
        left = cli.request("call", "run")["result"]
        right = self.direct_command(direct, "run")
        for event in (left, right):
            self.assertEqual(event["reason"], "invocation_complete")
            self.assertEqual(event["invocation"]["value"], "0x2B")
        self.assertEqual(left["invocation"]["instructions"], right["invocation"]["instructions"])
        instances = cli.request("instances", "instances")["result"]["instances"]
        self.assertEqual(len(instances), 2)
        helper = next(row for row in instances if row["id"] == "helper")
        self.assertTrue(helper["mapped"])
        self.assertEqual(helper["base"], "0x1B0000000")
        cli.request("quit", "quit")
        cli.finish()

    def test_bootstrap_instance_uses_the_same_public_identity_and_symbol_query(self):
        config = self.config()
        config["environment"].pop("image")
        config["environment"]["image_module"] = "driver"
        modules = self.root / "bootstrap-modules.json"
        modules.write_text(json.dumps({"modules": [{"id": "driver", "load": "lazy",
            "image": str(self.fixtures / "driver_invocation.sys"),
            "debug": {"kind": "pdb", "path": str(self.fixtures / "driver_invocation.pdb")}}]}), encoding="utf-8")
        cli = self.live(config, modules)
        rows = cli.request("instances", "instances")["result"]["instances"]
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual((row["id"], row["module_id"], row["role"]), ("$driver", "driver", "driver"))
        self.assertTrue(row["environment_owned"] and row["mapped"])
        self.assertEqual(row["load"], "lazy")
        self.assertEqual(cli.request("state", "inspect")["result"]["instructions"], "0x0")
        symbol = cli.request("lookup", "lookup", instance_id="$driver", symbol="ProbeSum")["result"]
        self.assertTrue(symbol["found"])
        self.assertEqual(int(symbol["address"], 16), self.address("ProbeSum"))
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        cli.request("quit", "quit")
        cli.finish()

    def test_primary_ordinal_import_waits_for_public_provider_supply(self):
        config = self.config()
        config["environment"].pop("image")
        config["environment"]["image_module"] = "primary"
        config["run"]["enable_resource_pauses"] = False
        modules = self.root / "primary-modules.json"
        modules.write_text(json.dumps({"modules": [{"id": "primary",
            "image": str(self.fixtures / "driver_module_imports_ordinal.sys"),
            "debug": {"kind": "pdb", "path": str(self.fixtures / "driver_module_imports_ordinal.pdb")}}]}), encoding="utf-8")
        cli = self.live(config, modules)
        event = cli.request("first", "run")["result"]
        self.assertEqual(event["state"], "awaiting_resource")
        self.assertEqual(event["request"]["dependency"], "provider.sys")
        before = cli.request("before", "inspect")["result"]
        self.assertEqual(before["instructions"], "0x0")
        self.assertFalse(before["environment_report"]["primary_imports_linked"])
        self.assertTrue(cli.request("add", "module_add", module={"id": "provider",
            "image": str(self.fixtures / "kernel_module_provider.sys"), "load": "lazy"})["ok"])
        binding = {"id": "provider", "module_id": "provider", "guest_name": "provider.sys",
                   "address_space": "kernel", "base": "0x1c0000000"}
        self.assertTrue(cli.request("supply", "supply", supply={"token": event["request"]["token"],
                                                               "bindings": [binding]})["ok"])
        self.assertEqual(cli.request("linked", "inspect")["result"]["instructions"], "0x0")
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        for name, expected in (("PrimaryEntryCount", 1), ("PrimaryObserved", 199)):
            symbol = cli.request("lookup-" + name, "lookup", instance_id="$driver", symbol=name)["result"]
            value = cli.request("read-" + name, "read", address=symbol["address"], size=8)["result"]["bytes_hex"]
            self.assertEqual(int.from_bytes(bytes.fromhex(value), "little"), expected)
        cli.request("quit", "quit")
        cli.finish()

    def test_published_native_module_catches_its_exception_through_the_public_cli(self):
        modules = self.root / "seh-modules.json"
        modules.write_text(json.dumps({"modules": [{"id": "seh",
            "image": str(self.fixtures / "kernel_module_seh.sys"),
            "debug": {"kind": "pdb", "path": str(self.fixtures / "kernel_module_seh.pdb")}}]}), encoding="utf-8")
        config = self.config()
        config["bindings"] = [{"id": "seh", "module_id": "seh", "guest_name": "seh.sys",
                               "address_space": "kernel", "base": "0x1b0000000"}]
        cli = self.live(config, modules)
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        address = cli.request("lookup", "lookup", instance_id="seh", symbol="ModuleSEHCatch")["result"]["address"]
        for n in range(2):
            self.assertTrue(cli.request("invoke" + str(n), "invoke", invocation={"entry": address,
                "abi": "win64", "arguments": [{"value": 7}]})["ok"])
            result = cli.request("call" + str(n), "run")["result"]
            self.assertEqual(result["reason"], "invocation_complete")
            self.assertEqual(result["invocation"]["value"], "0x4A")
        self.assertTrue(cli.request("inspect", "inspect")["result"]["environment_report"]["capabilities"]["module_x64_c_seh"])
        cli.request("quit", "quit")
        cli.finish()

    def test_repaired_kernel_module_reads_its_new_pte_through_cli_and_dll(self):
        declaration = {"id": "paging", "image": str(self.fixtures / "kernel_module_paging.sys")}
        result, error = self.call("neverd_module_set_add_json", self.source,
                                  json.dumps(declaration).encode())
        self.assertEqual(result, 1, error)
        modules = self.root / "paging-modules.json"
        modules.write_text(json.dumps({"modules": [declaration]}), encoding="utf-8")
        config = self.config()
        config["environment"]["scenario"].update(page_tables=True,
            windows_version={"major": 10, "minor": 0, "build": 19041})
        config["run"].update(enable_resource_pauses=True, instruction_quantum=100000)
        config["bindings"] = [{"id": "paging", "module_id": "paging", "guest_name": "paging.sys",
                               "address_space": "kernel", "base": "0x1b0000000"}]
        cli = self.live(config, modules)
        direct = self.direct(config)
        transports = (("cli", lambda op, **fields: cli.request(op, op, **fields)["result"]),
                      ("dll", lambda op, **fields: self.direct_command(direct, op, **fields)))
        for label, command in transports:
            with self.subTest(transport=label):
                self.assertEqual(command("run")["reason"], "stage")
                def lookup(name):
                    return command("lookup", instance_id="paging", symbol=name)["address"]
                function = lookup("ModulePagingAfterRead")
                counter = lookup("PagingCalls")
                high_base = 0xfffff88010000000
                target = int(lookup("PagingValue"), 16) + high_base - 0x1b0000000
                command("invoke", invocation={"entry": function, "abi": "win64",
                    "arguments": [{"value": hex(target)}, {"value": "0xffffff0000000000"},
                                  {"value": "0x8000000000000003"}]})
                event = command("run")
                self.assertEqual(event["state"], "awaiting_resource")
                self.assertEqual(int(event["request"]["address"], 16), target)
                self.assertEqual(int.from_bytes(bytes.fromhex(
                    command("read", address=counter, size=8)["bytes_hex"]), "little"), 1)
                before = command("inspect")["instructions"]
                command("module_add", module={"id": "late-paging-source",
                        "image": str(self.fixtures / "kernel_module_paging.sys"), "load": "lazy"})
                command("supply", supply={"token": event["request"]["token"], "bindings": [
                    {"id": "late-paging", "module_id": "late-paging-source",
                     "guest_name": "late-paging.sys", "address_space": "kernel", "base": hex(high_base)}]})
                self.assertEqual(command("inspect")["instructions"], before)
                result = command("run")
                self.assertEqual(result["reason"], "invocation_complete")
                self.assertEqual(result["invocation"]["value"], "0x2A")
                self.assertEqual(int.from_bytes(bytes.fromhex(
                    command("read", address=counter, size=8)["bytes_hex"]), "little"), 1)
        cli.request("quit", "quit")
        cli.finish()

    def test_active_native_cfg_works_after_late_publication_in_cli_and_dll(self):
        declaration = {"id": "guard", "image": str(self.fixtures / "kernel_module_guard_active.sys")}
        modules = self.root / "cfg-modules.json"
        modules.write_text(json.dumps({"modules": [declaration]}), encoding="utf-8")
        value, error = self.call("neverd_module_set_add_json", self.source, json.dumps(declaration).encode())
        self.assertEqual(value, 1, error)
        cli, direct = self.live(self.config(), modules), self.direct()
        for label, command in (("cli", lambda op, **fields: cli.request(op, op, **fields)["result"]),
                               ("dll", lambda op, **fields: self.direct_command(direct, op, **fields))):
            with self.subTest(transport=label):
                self.assertEqual(command("run")["reason"], "stage")
                command("declare", binding={"id": "guard", "module_id": "guard", "guest_name": "guard.sys",
                        "address_space": "kernel", "base": "0x1b0000000"})
                # The next CPU action maps the declared eager image before entry.
                command("invoke", invocation={"entry": hex(self.address("ProbeSum")), "abi": "win64",
                        "arguments": [{"value": n} for n in range(1, 8)]})
                self.assertEqual(command("run")["reason"], "invocation_complete")
                address = command("lookup", instance_id="guard", symbol="ModuleGuardSelf")["address"]
                for _ in range(2):
                    command("invoke", invocation={"entry": address, "abi": "win64"})
                    result = command("run")
                    self.assertEqual(result["reason"], "invocation_complete")
                    self.assertEqual(result["invocation"]["value"], "0x134")
                self.assertTrue(command("inspect")["environment_report"]["capabilities"]["module_cfg"])
        cli.request("quit", "quit")
        cli.finish()

    def test_bounded_reports_keep_full_counts_and_do_not_change_execution(self):
        config = self.config()
        config["environment"]["report"] = {"call_tail": 1, "write_tail": 1}
        full, bounded = self.direct(), self.direct(config)
        for run in (full, bounded):
            self.assertEqual(self.direct_command(run, "run")["reason"], "stage")
            for _ in range(2):
                self.direct_command(run, "invoke", invocation={"entry": hex(self.address("ProbeNotify")),
                                    "abi": "win64", "arguments": [{"value": 5}]})
                self.assertEqual(self.direct_command(run, "run")["reason"], "invocation_complete")
        a, b = (self.direct_command(run, "inspect") for run in (full, bounded))
        self.assertEqual(a["instructions"], b["instructions"])
        self.assertEqual(a["events"], b["events"])
        left, right = a["environment_report"], b["environment_report"]
        self.assertNotIn("trace_view", left)
        self.assertEqual(right["calls"], left["calls"][-1:])
        self.assertEqual(right["writes"], left["writes"][-1:])
        self.assertEqual(right["trace_view"]["total_calls"], len(left["calls"]))
        self.assertEqual(right["trace_view"]["first_call_index"], len(left["calls"]) - 1)
        again = self.direct_command(bounded, "inspect")
        self.assertEqual(again["environment_report"], right)

    def test_public_replay_runner_retains_buffers_globals_and_module_lifetime(self):
        scenario = self.root / "replay.scenario.json"
        scenario.write_text(json.dumps({"load_address": "0x190000000"}), encoding="utf-8")
        report = self.root / "replay.report.json"
        runner = Path(__file__).resolve().parents[1] / "labs/run_module_driver_replay.py"
        command = [sys.executable, str(runner), "--library", str(Path(OPTIONS.library).resolve()),
                   "--image", str(self.fixtures / "driver_invocation.sys"),
                   "--pdb", str(self.fixtures / "driver_invocation.pdb"), "--scenario", str(scenario),
                   "--entry", hex(self.address("ProbeBuffers")), "--capacity", "4", "--capacity", "4",
                   "--buffer-bytes", "16", "--output", str(report), "--timeout-seconds", "30"]
        process = subprocess.run(command, capture_output=True, timeout=45)
        self.assertEqual(process.returncode, 0, process.stderr.decode("utf-8", "replace"))
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertTrue(data["completed"] and data["image_unchanged"])
        self.assertTrue(data["source_handle_released_before_execution"])
        self.assertEqual([row["event"]["invocation"]["value"] for row in data["calls"]], ["0x2A", "0x2B"])
        for row in data["calls"]:
            output = bytes.fromhex(row["event"]["invocation"]["buffers"][0]["bytes_hex"])
            self.assertTrue(row["output_modified"] and row["api_calls_complete"])
            self.assertEqual(output[4:], bytes([0xa5]) * 12)

    def test_report_and_primary_import_policy_reject_invalid_configuration(self):
        for patch in ({"primary_import_policy": "guess"}, {"report": []},
                      {"report": {"call_tail": 0}}, {"report": {"write_tail": 1000001}},
                      {"report": {"unexpected": 1}}):
            config = self.config()
            config["environment"].update(patch)
            text = json.dumps(config).encode()
            options = NeverDExecutionOptionsV1(ctypes.sizeof(NeverDExecutionOptionsV1), self.source, text)
            run, error = self.call("neverd_execution_create_v1", ctypes.byref(options))
            self.assertFalse(run)
            self.assertTrue(error)

    def resource_cli(self, lazy=False):
        support = {"id": "support", "image": str(self.fixtures / "kernel_module_support.sys"),
                   "debug": {"kind": "pdb", "path": str(self.fixtures / "kernel_module_support.pdb")}}
        declarations = [support]
        if lazy:
            declarations.append({"id": "late", "image": support["image"], "load": "lazy"})
        modules = self.root / "resource-modules.json"
        modules.write_text(json.dumps({"modules": declarations}), encoding="utf-8")
        config = self.config()
        config["bindings"] = [{"id": "helper", "module_id": "support", "guest_name": "helper.sys",
                               "scope": "kernel", "address_space": "kernel", "base": "0x1b0000000"}]
        cli = self.live(config, modules)
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        return cli, support

    def test_missing_module_is_supplied_inside_the_same_driver_call(self):
        cli, source = self.resource_cli()
        caller = cli.request("caller", "lookup", instance_id="helper", symbol="SupportIndirect")["result"]["address"]
        target = cli.request("target", "lookup", instance_id="helper", symbol="SupportProbe")["result"]["address"]
        invocation = {"entry": caller, "abi": "win64",
                      "arguments": [{"value": hex(int(target, 16) + 0x10000000)}, {"value": 7}]}
        self.assertTrue(cli.request("invoke", "invoke", invocation=invocation)["ok"])
        event = cli.request("fault", "run")["result"]
        self.assertEqual(event["state"], "awaiting_resource")
        before = cli.request("before", "inspect")["result"]
        self.assertTrue(before["environment_report"]["capabilities"]["resource_pause"])
        self.assertTrue(cli.request("add", "module_add", module={"id": "late", "image": source["image"]})["ok"])
        binding = {"id": "late-helper", "module_id": "late", "guest_name": "late.sys", "scope": "kernel",
                   "address_space": "kernel", "base": "0x1c0000000"}
        self.assertTrue(cli.request("supply", "supply", supply={"token": event["request"]["token"],
                                                               "bindings": [binding]})["ok"])
        after = cli.request("after", "inspect")["result"]
        self.assertEqual(after["instructions"], before["instructions"])
        event = cli.request("continue", "run")["result"]
        self.assertEqual(event["reason"], "invocation_complete")
        self.assertEqual(event["invocation"]["value"], "0x5C")
        cli.request("quit", "quit")
        cli.finish()

    def test_configured_lazy_module_maps_on_the_first_indirect_call(self):
        cli, _ = self.resource_cli(lazy=True)
        before = cli.request("sources", "modules")["result"]["modules"]
        self.assertFalse(next(row for row in before if row["id"] == "late")["prepared"])
        size = next(row for row in cli.request("size", "instances")["result"]["instances"]
                    if row["id"] == "helper")["size"]
        binding = {"id": "late-helper", "module_id": "late", "guest_name": "late.sys", "scope": "kernel",
                   "address_space": "kernel", "base": "0x1c0000000", "size": size}
        self.assertTrue(cli.request("declare", "declare", binding=binding)["ok"])
        caller = cli.request("caller", "lookup", instance_id="helper", symbol="SupportIndirect")["result"]["address"]
        target = cli.request("target", "lookup", instance_id="helper", symbol="SupportProbe")["result"]["address"]
        invocation = {"entry": caller, "abi": "win64",
                      "arguments": [{"value": hex(int(target, 16) + 0x10000000)}, {"value": 7}]}
        self.assertTrue(cli.request("invoke", "invoke", invocation=invocation)["ok"])
        event = cli.request("first", "run")["result"]
        self.assertEqual(event["reason"], "invocation_complete")
        self.assertEqual(event["invocation"]["value"], "0x5C")
        rows = cli.request("instances", "instances")["result"]["instances"]
        self.assertTrue(next(row for row in rows if row["id"] == "late-helper")["mapped"])
        cli.request("quit", "quit")
        cli.finish()


    def test_missing_dependency_is_supplied_with_the_unpublished_importer(self):
        modules = self.root / "dependency-modules.json"
        modules.write_text(json.dumps({"modules": [{"id": "dependent",
            "image": str(self.fixtures / "kernel_module_dependent.sys"),
            "debug": {"kind": "pdb", "path": str(self.fixtures / "kernel_module_dependent.pdb")}}]}), encoding="utf-8")
        config = self.config()
        config["bindings"] = [{"id": "helper", "module_id": "dependent", "guest_name": "dependent.sys",
                               "address_space": "kernel", "base": "0x1b0000000"}]
        cli = self.live(config, modules)
        event = cli.request("first", "run")["result"]
        self.assertEqual(event["state"], "awaiting_resource")
        self.assertEqual(event["request"]["kind"], "missing_dependency")
        self.assertEqual(event["request"]["dependency"], "provider.sys")
        self.assertFalse(next(row for row in cli.request("before", "instances")["result"]["instances"]
                              if row["id"] == "helper")["mapped"])
        self.assertTrue(cli.request("add", "module_add", module={"id": "supplier",
            "image": str(self.fixtures / "kernel_module_provider.sys")})["ok"])
        binding = {"id": "supplier", "module_id": "supplier", "guest_name": "PROVIDER.SYS",
                   "address_space": "kernel", "base": "0x1c0000000"}
        self.assertTrue(cli.request("supply", "supply", supply={"token": event["request"]["token"],
                                                               "bindings": [binding]})["ok"])
        rows = cli.request("after", "instances")["result"]["instances"]
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(row["mapped"] for row in rows))
        self.assertEqual(cli.request("inspect", "inspect")["result"]["instructions"], "0x0")
        self.assertEqual(cli.request("entry", "run")["result"]["reason"], "stage")
        address = cli.request("lookup", "lookup", instance_id="helper", symbol="DependentProbe")["result"]["address"]
        invocation = {"entry": address, "abi": "win64", "arguments": [{"value": 1}]}
        self.assertTrue(cli.request("invoke", "invoke", invocation=invocation)["ok"])
        event = cli.request("call", "run")["result"]
        self.assertEqual(event["reason"], "invocation_complete")
        self.assertEqual(event["invocation"]["value"], "0x5A")
        cli.request("quit", "quit")
        cli.finish()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--fixtures", required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
