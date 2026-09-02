import concurrent.futures
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import scripts.audit_evm_opcode_metadata as opcode_audit
from scripts.audit_evm_opcode_metadata import (
    DEFAULT_GETH_PROBE,
    DEFAULT_GETH_REF,
    DEFAULT_GETH_REMOTE,
    DEFAULT_NEVERD_CONSTANTS,
    DEFAULT_NEVERD_HARDFORKS,
    DEFAULT_NEVERD_OPCODES,
    DEFAULT_POLICY,
    DEFAULT_SEMANTICS_POLICY,
    GethOpcodeSource,
    OpcodeAuditError,
    audit_geth_opcode_semantics,
    audit_geth_rule_probes,
    audit_geth_mainnet_forks,
    audit_geth_eip8024_immediates,
    audit_opcodes,
    build_geth_probe_request,
    checkout_geth_revision,
    fetch_geth_opcode_source,
    main,
    parse_args,
    parse_geth_opcodes,
    parse_geth_fork_aliases,
    parse_eip8024_immediate_policy,
    parse_neverd_opcode_metadata,
    parse_neverd_hardforks,
    parse_neverd_latest_hardfork,
    parse_neverd_opcodes,
    parse_neverd_stack_limit,
    parse_policy,
    parse_semantics_policy,
    run_geth_opcode_probe,
)


requires_bounded_process = unittest.skipUnless(
    os.name == "posix"
    and (sys.platform == "darwin" or sys.platform.startswith("linux")),
    "bounded process execution is unavailable on this platform",
)


class EVMOpcodeAuditTests(unittest.TestCase):
    @staticmethod
    def _git(repository: Path, *arguments: str) -> str:
        result = subprocess.run(
            ("git", "-C", str(repository), *arguments),
            check=True,
            capture_output=True,
            encoding="utf-8",
        )
        return result.stdout.strip()

    @staticmethod
    def _stop_contract():
        opcodes = parse_neverd_opcode_metadata(
            """EVM_OPCODE(STOP, 0x00, 0, 0, 0, None, Control, Frontier,
                          Halt, None, None, None, true)
"""
        )
        policy = parse_semantics_policy(
            """EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_ACTIVE_WITHOUT_COST(STOP, Frontier)
"""
        )
        return opcodes, policy

    @staticmethod
    def _stop_manifest(revision: str, go_version: str, stack_limit: int):
        return {
            "schema_version": opcode_audit.GETH_PROBE_SCHEMA_VERSION,
            "geth_revision": revision,
            "go_version": go_version,
            "stack_limit": stack_limit,
            "forks": [
                {
                    "name": "Frontier",
                    "rules": [],
                    "opcodes": [
                        {
                            "byte": 0,
                            "name": "STOP",
                            "base_min_stack": 0,
                            "net_stack_delta": 0,
                        }
                    ],
                }
            ],
        }

    @staticmethod
    def _minimal_probe_files(root: Path) -> tuple[Path, Path, Path]:
        geth = root / "go-ethereum"
        geth.mkdir()
        (geth / "core/vm").mkdir(parents=True)
        (geth / "go.mod").write_text(
            "module github.com/ethereum/go-ethereum\n\ngo 1.24\n",
            encoding="utf-8",
        )
        (geth / "go.sum").write_text("", encoding="utf-8")
        helper = root / "probe.go"
        helper.write_text("package main\n", encoding="utf-8")
        go_executable = (
            root / "test-go-root/bin" / ("go.exe" if os.name == "nt" else "go")
        )
        go_executable.parent.mkdir(parents=True)
        go_executable.write_bytes(b"test executable")
        go_executable.chmod(0o700)
        return geth, helper, go_executable

    @staticmethod
    def _empty_probe_request():
        return {
            "rule_fields": [],
            "rule_probes": [],
            "forks": [],
            "opcodes": [],
            "eip8024_specs": [],
        }

    def test_default_source_fetches_official_remote_head(self):
        arguments = parse_args([])
        self.assertEqual(vars(arguments), {"manifest_output": None})
        self.assertEqual(opcode_audit.DEFAULT_GO_TOOLCHAIN, "local")
        self.assertEqual(opcode_audit.GETH_PROBE_SCHEMA_VERSION, 3)

    def test_cli_has_no_existing_checkout_bypass(self):
        for bypass in (
            "--git-executable",
            "--go-executable",
            "--geth-root",
            "--geth-cache",
            "--geth-remote",
            "--geth-ref",
            "--geth-probe",
            "--geth-eip8024-overlay",
            "--go-toolchain",
            "--neverd-opcodes",
            "--neverd-hardforks",
            "--neverd-constants",
            "--policy",
            "--semantics-policy",
            "--geth-fork-aliases",
            "--eip8024-policy",
        ):
            with self.subTest(bypass=bypass):
                error_output = io.StringIO()
                with redirect_stderr(error_output), self.assertRaises(SystemExit):
                    parse_args([bypass, "/tmp/local-docs"])
                self.assertIn("unrecognized arguments", error_output.getvalue())

    def test_probe_function_rejects_trusted_identity_overrides(self):
        request = {**self._empty_probe_request(), "authority": "forged"}
        with self.assertRaisesRegex(OpcodeAuditError, "override trusted fields"):
            run_geth_opcode_probe(
                geth_root=Path("/does/not/matter"),
                geth_revision="a" * 40,
                request=request,
            )

    def test_default_neverd_metadata_paths_exist(self):
        self.assertTrue(DEFAULT_NEVERD_OPCODES.is_file())
        self.assertTrue(DEFAULT_NEVERD_CONSTANTS.is_file())
        self.assertTrue(DEFAULT_NEVERD_HARDFORKS.is_file())
        self.assertTrue(DEFAULT_POLICY.is_file())
        self.assertTrue(DEFAULT_SEMANTICS_POLICY.is_file())
        self.assertTrue(DEFAULT_GETH_PROBE.is_file())
        self.assertTrue(opcode_audit.DEFAULT_GETH_EIP8024_OVERLAY.is_file())
        self.assertTrue(opcode_audit.DEFAULT_GETH_FORK_ALIASES.is_file())
        self.assertTrue(opcode_audit.DEFAULT_EIP8024_POLICY.is_file())

    def test_ci_audits_live_head_and_records_the_exact_revision(self):
        workflow = (
            Path(__file__).resolve().parents[2]
            / ".github/workflows/evm-upstream-audit.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("actions/setup-go@", workflow)
        self.assertIn("go-version: stable", workflow)
        self.assertIn("check-latest: true", workflow)
        self.assertIn("GOTOOLCHAIN: local", workflow)
        self.assertIn("permissions:\n  contents: read", workflow)
        self.assertIn(
            "apt-get install --yes apparmor-profiles apparmor-utils bubblewrap",
            workflow,
        )
        self.assertIn("bwrap --version", workflow)
        self.assertIn("python scripts/audit_evm_opcode_metadata.py", workflow)
        self.assertNotIn("--geth-cache", workflow)
        self.assertNotIn("--go-toolchain", workflow)
        self.assertIn("--manifest-output", workflow)
        self.assertIn("GITHUB_STEP_SUMMARY", workflow)
        self.assertIn("PIPESTATUS[0]", workflow)
        self.assertIn("if: ${{ always() }}", workflow)
        self.assertIn("geth-revision.txt", workflow)
        self.assertIn("manifest.json", workflow)
        self.assertIn("audit.log", workflow)
        self.assertIn(
            "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
            workflow,
        )
        self.assertIn("if: ${{ failure() }}", workflow)
        self.assertNotIn("--geth-root", workflow)
        self.assertNotIn("local_docs", workflow)
        self.assertNotIn("submodules:", workflow)
        job_timeout = re.search(r"(?m)^\s*timeout-minutes:\s*([0-9]+)\s*$", workflow)
        self.assertIsNotNone(job_timeout)
        job_timeout_seconds = int(job_timeout.group(1)) * 60
        subprocess_deadlines = (
            opcode_audit.GIT_FETCH_TIMEOUT_SECONDS
            + opcode_audit.GO_PROBE_TOTAL_TIMEOUT_SECONDS
        )
        self.assertGreater(job_timeout_seconds, subprocess_deadlines)
        self.assertGreaterEqual(
            job_timeout_seconds - subprocess_deadlines,
            2 * opcode_audit.GIT_FETCH_TIMEOUT_SECONDS,
        )

    def test_ci_loads_and_exercises_the_bubblewrap_apparmor_profile(self):
        workflow = (
            Path(__file__).resolve().parents[2]
            / ".github/workflows/evm-upstream-audit.yml"
        ).read_text(encoding="utf-8")
        for profile_contract in (
            "BWRAP_APPARMOR_PROFILE_SOURCE: "
            "/usr/share/apparmor/extra-profiles/bwrap-userns-restrict",
            "BWRAP_APPARMOR_PROFILE_DESTINATION: /etc/apparmor.d/bwrap-userns-restrict",
        ):
            with self.subTest(profile_contract=profile_contract):
                self.assertIn(profile_contract, workflow)
        step_marker = "      - name: Install fail-closed upstream sandbox\n"
        self.assertEqual(workflow.count(step_marker), 1)
        sandbox_step = workflow.split(step_marker, 1)[1].split("\n      - name:", 1)[0]

        for required_contract in (
            "apparmor-profiles",
            "BWRAP_APPARMOR_PROFILE_SOURCE",
            "BWRAP_APPARMOR_PROFILE_DESTINATION",
            "sudo install --mode=0644",
            'sudo apparmor_parser --replace "$BWRAP_APPARMOR_PROFILE_DESTINATION"',
            "bwrap --cap-drop ALL --unshare-net --ro-bind / / -- /bin/true",
        ):
            with self.subTest(required_contract=required_contract):
                self.assertIn(required_contract, sandbox_step)
        self.assertNotIn("apparmor_restrict_unprivileged_userns=0", sandbox_step)
        self.assertNotIn("--share-net", sandbox_step)

    @requires_bounded_process
    def test_fetches_and_refreshes_remote_head(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            upstream = root / "upstream"
            cache = root / "cache.git"
            upstream.mkdir()
            self._git(upstream, "init", "--initial-branch=master")
            self._git(upstream, "config", "user.name", "NeverD Test")
            self._git(upstream, "config", "user.email", "neverd@example.invalid")
            self._git(upstream, "config", "commit.gpgsign", "false")

            opcode_path = upstream / "core/vm/opcodes.go"
            opcode_path.parent.mkdir(parents=True)
            opcode_path.write_text(
                "const (\n    STOP OpCode = 0x00\n)\n", encoding="utf-8"
            )
            self._git(upstream, "add", "core/vm/opcodes.go")
            self._git(upstream, "commit", "-m", "add stop")
            first_revision = self._git(upstream, "rev-parse", "HEAD")

            first = fetch_geth_opcode_source(
                remote=str(upstream), ref="HEAD", cache=cache
            )
            self.assertEqual(first.revision, first_revision)
            self.assertIn("STOP OpCode", first.text)

            opcode_path.write_text(
                "const (\n    STOP OpCode = 0x00\n    ADD\n)\n",
                encoding="utf-8",
            )
            self._git(upstream, "add", "core/vm/opcodes.go")
            self._git(upstream, "commit", "-m", "add arithmetic")
            second_revision = self._git(upstream, "rev-parse", "HEAD")

            second = fetch_geth_opcode_source(
                remote=str(upstream), ref="HEAD", cache=cache
            )
            self.assertEqual(second.revision, second_revision)
            self.assertNotEqual(second.revision, first.revision)
            self.assertIsNotNone(first.authority_ref)
            self.assertIsNotNone(second.authority_ref)
            self.assertNotEqual(first.authority_ref, second.authority_ref)
            self.assertEqual(
                self._git(cache, "rev-parse", first.authority_ref), first.revision
            )
            self.assertEqual(
                self._git(cache, "rev-parse", second.authority_ref), second.revision
            )
            self.assertIn("ADD", second.text)

    def test_git_commands_ignore_inherited_repository_and_config_injection(self):
        inherited = {
            "GIT_ALLOW_PROTOCOL": "file",
            "GIT_ALTERNATE_OBJECT_DIRECTORIES": "/tmp/untrusted-objects",
            "GIT_ATTR_SOURCE": "untrusted-tree",
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_GLOBAL": "/tmp/untrusted-config",
            "GIT_CONFIG_KEY_0": "url.file:///tmp/untrusted.insteadOf",
            "GIT_CONFIG_VALUE_0": "https://github.com/",
            "GIT_DIR": "/tmp/untrusted.git",
            "GIT_EXEC_PATH": "/tmp/untrusted-exec-path",
            "GIT_IMPLICIT_WORK_TREE": "1",
            "GIT_OBJECT_DIRECTORY": "/tmp/untrusted-object-directory",
            "GIT_PREFIX": "/tmp/untrusted-prefix",
            "GIT_PROTOCOL_FROM_USER": "0",
            "GIT_SSL_CAPATH": "/tmp/untrusted-certificates",
            "GIT_WORK_TREE": "/tmp/untrusted-work-tree",
        }
        completed = opcode_audit.BoundedProcessResult(
            returncode=0,
            stdout="git version test\n",
            stderr="",
            stdout_sha256="a",
            stderr_sha256="b",
        )
        with (
            mock.patch.dict(os.environ, inherited),
            mock.patch.object(
                opcode_audit, "_run_bounded_process", return_value=completed
            ) as run,
        ):
            self.assertEqual(opcode_audit._run_git(("version",)), "git version test\n")

        environment = run.call_args.kwargs["environment"]
        for name in inherited:
            if name.startswith("GIT_CONFIG_"):
                continue
            self.assertNotEqual(environment.get(name), inherited[name])
        self.assertEqual(environment["GIT_CONFIG_NOSYSTEM"], "1")
        self.assertEqual(environment["GIT_CONFIG_GLOBAL"], os.devnull)
        self.assertEqual(environment["GIT_ATTR_NOSYSTEM"], "1")
        self.assertEqual(environment["GIT_CONFIG_COUNT"], "3")
        self.assertEqual(environment["GIT_CONFIG_KEY_0"], "core.hooksPath")
        self.assertEqual(environment["GIT_CONFIG_VALUE_0"], os.devnull)
        self.assertEqual(environment["GIT_CONFIG_KEY_1"], "core.attributesFile")
        self.assertEqual(environment["GIT_CONFIG_VALUE_1"], os.devnull)
        self.assertEqual(environment["GIT_CONFIG_KEY_2"], "protocol.ext.allow")
        self.assertEqual(environment["GIT_CONFIG_VALUE_2"], "never")
        self.assertEqual(environment["GIT_NO_REPLACE_OBJECTS"], "1")

    def test_go_commands_ignore_ambient_build_and_source_overrides(self):
        inherited = {
            "GOENV": "/tmp/untrusted-go-env",
            "GOFLAGS": "-overlay=/tmp/untrusted-overlay.json -toolexec=false",
            "GOMODCACHE": "/tmp/untrusted-module-cache",
            "GONOSUMDB": "*",
            "GOPROXY": "https://untrusted.invalid",
            "GOROOT": "/tmp/untrusted-go-root",
            "GOTOOLCHAIN": "path",
            "SSH_AUTH_SOCK": "/tmp/untrusted-agent",
            "GITHUB_TOKEN": "untrusted-token",
        }
        completed = opcode_audit.BoundedProcessResult(
            returncode=0,
            stdout="go version test\n",
            stderr="",
            stdout_sha256="a",
            stderr_sha256="b",
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            with (
                mock.patch.dict(os.environ, inherited),
                mock.patch.object(
                    opcode_audit, "_run_bounded_process", return_value=completed
                ) as run,
            ):
                self.assertEqual(
                    opcode_audit._run_go(
                        ("version",),
                        cwd=root,
                        go_executable="go",
                        environment_root=root / "environment",
                        go_toolchain="local",
                        timeout_seconds=1,
                    ),
                    "go version test\n",
                )
                opcode_audit._run_go(
                    ("version",),
                    cwd=root,
                    go_executable="go",
                    environment_root=root / "offline-environment",
                    go_toolchain="local",
                    timeout_seconds=1,
                    network_allowed=False,
                )

        environment = run.call_args.kwargs["environment"]
        self.assertEqual(environment["GOENV"], "off")
        self.assertEqual(environment["GOFLAGS"], "")
        self.assertEqual(environment["GONOSUMDB"], "")
        self.assertEqual(environment["GOPROXY"], "off")
        self.assertEqual(environment["GOSUMDB"], "off")
        self.assertEqual(environment["GOVCS"], "*:off")
        self.assertEqual(environment["GOTOOLCHAIN"], "local")
        self.assertEqual(environment["GOWORK"], "off")
        self.assertNotIn("GOROOT", environment)
        self.assertNotIn("SSH_AUTH_SOCK", environment)
        self.assertNotIn("GITHUB_TOKEN", environment)
        self.assertEqual(Path(environment["HOME"]).name, "home")
        self.assertEqual(Path(environment["XDG_CONFIG_HOME"]).name, "xdg-config")
        for path_name in ("GOCACHE", "GOMODCACHE", "GOPATH", "GOTMPDIR"):
            self.assertTrue(Path(environment[path_name]).is_absolute())
            self.assertNotEqual(environment[path_name], inherited.get(path_name))

    def test_go_filesystem_sandbox_wraps_online_and_offline_commands(self):
        completed = opcode_audit.BoundedProcessResult(
            returncode=0,
            stdout="",
            stderr="",
            stdout_sha256="a",
            stderr_sha256="b",
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory).resolve()
            module = root / "module"
            module.mkdir()
            environment_root = root / "environment"
            wrapped = ("sandbox", "wrapped")
            with (
                mock.patch.object(
                    opcode_audit, "_sandboxed_command", return_value=wrapped
                ) as sandbox,
                mock.patch.object(
                    opcode_audit, "_run_bounded_process", return_value=completed
                ) as run,
            ):
                for network_allowed in (True, False):
                    opcode_audit._run_go(
                        ("mod", "download"),
                        cwd=module,
                        go_executable=sys.executable,
                        environment_root=environment_root,
                        go_toolchain="local",
                        timeout_seconds=1,
                        network_allowed=network_allowed,
                        sandbox_required=True,
                        sandbox_readable_roots=(root,),
                        sandbox_writable_roots=(module,),
                    )

        self.assertEqual(sandbox.call_count, 2)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(
            [call.kwargs["network_allowed"] for call in sandbox.call_args_list],
            [True, False],
        )
        self.assertTrue(all(call.args[0] == wrapped for call in run.call_args_list))
        offline_environment = run.call_args_list[-1].kwargs["environment"]
        self.assertEqual(offline_environment["GOPROXY"], "off")
        self.assertEqual(offline_environment["GOVCS"], "*:off")

    @requires_bounded_process
    def test_process_output_limit_accepts_exact_capacity_and_rejects_one_more(self):
        command = (
            sys.executable,
            "-c",
            "import os,sys; os.write(sys.stdout.fileno(), b'x' * int(sys.argv[1]))",
        )
        exact = opcode_audit._run_bounded_process(
            (*command, "16"),
            cwd=Path.cwd(),
            environment={"PATH": os.environ.get("PATH", "")},
            timeout_seconds=5,
            stdout_limit=16,
            stderr_limit=16,
        )
        self.assertEqual(exact.stdout, "x" * 16)
        self.assertEqual(
            exact.stdout_sha256,
            __import__("hashlib").sha256(b"x" * 16).hexdigest(),
        )

        with self.assertRaisesRegex(
            OpcodeAuditError, "stdout exceeded 16-byte limit.*sha256=.*bounded tail"
        ):
            opcode_audit._run_bounded_process(
                (*command, "17"),
                cwd=Path.cwd(),
                environment={"PATH": os.environ.get("PATH", "")},
                timeout_seconds=5,
                stdout_limit=16,
                stderr_limit=16,
            )

    def test_fixed_input_bytes_are_bounded_and_strict_utf8(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            source = Path(temporary_directory) / "policy.def"
            source.write_bytes(b"abcd")
            budget = opcode_audit._FixedInputBudget(limit=4)
            self.assertEqual(
                opcode_audit._read_bounded_utf8(
                    source, "test policy", budget=budget, per_file_limit=4
                ),
                "abcd",
            )
            self.assertEqual(budget.used, 4)

            source.write_bytes(b"abcde")
            with self.assertRaisesRegex(OpcodeAuditError, "4-byte file limit"):
                opcode_audit._read_bounded_utf8(source, "test policy", per_file_limit=4)

            source.write_bytes(b"\xff")
            with self.assertRaisesRegex(OpcodeAuditError, "not valid UTF-8"):
                opcode_audit._read_bounded_utf8(source, "test policy", per_file_limit=4)

            source.write_bytes(b"a")
            exhausted = opcode_audit._FixedInputBudget(limit=0)
            with self.assertRaisesRegex(OpcodeAuditError, "aggregate fixed-input"):
                opcode_audit._read_bounded_utf8(
                    source, "test policy", budget=exhausted, per_file_limit=4
                )

    def test_probe_request_byte_limit_accepts_exact_and_rejects_one_more(self):
        opcode_audit._require_probe_request_bytes("x" * 4, limit=4)
        with self.assertRaisesRegex(OpcodeAuditError, "4-byte limit"):
            opcode_audit._require_probe_request_bytes("x" * 5, limit=4)
        with self.assertRaisesRegex(OpcodeAuditError, "4-byte limit"):
            opcode_audit._require_probe_request_bytes("ééé", limit=4)

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups")
    def test_process_timeout_kills_descendants_holding_output_pipes(self):
        command = (
            sys.executable,
            "-c",
            "import os,time; child=os.fork(); "
            "time.sleep(2) if child == 0 else time.sleep(60)",
        )
        started = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            opcode_audit._run_bounded_process(
                command,
                cwd=Path.cwd(),
                environment={"PATH": os.environ.get("PATH", "")},
                timeout_seconds=0.1,
            )
        self.assertLess(time.monotonic() - started, 1.0)

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups")
    def test_process_success_kills_descendants_holding_output_pipes(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            sentinel = Path(temporary_directory) / "orphan-survived"
            command = (
                sys.executable,
                "-c",
                "import os,sys,time; child=os.fork(); "
                "sys.exit(0) if child else "
                "(time.sleep(0.75), open(sys.argv[1], 'w').close())",
                str(sentinel),
            )
            started = time.monotonic()
            result = opcode_audit._run_bounded_process(
                command,
                cwd=Path.cwd(),
                environment={"PATH": os.environ.get("PATH", "")},
                timeout_seconds=5,
            )
            self.assertEqual(result.returncode, 0)
            self.assertLess(time.monotonic() - started, 1.0)
            time.sleep(0.8)
            self.assertFalse(sentinel.exists())

    def test_upstream_execution_sandbox_is_fail_closed_per_platform(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            probe_root = root / "probe"
            geth_root = root / "geth"
            go_root = root / "go"
            writable_root = probe_root / "environment"
            cwd = probe_root / "module"
            go_executable = go_root / "bin/go"
            for directory in (geth_root, writable_root, cwd, go_executable.parent):
                directory.mkdir(parents=True, exist_ok=True)
            go_executable.write_bytes(b"test executable")
            go_executable.chmod(0o700)
            readable_roots = (probe_root, geth_root, go_root)

            with (
                mock.patch.object(opcode_audit.sys, "platform", "darwin"),
                mock.patch.object(opcode_audit.shutil, "which", return_value=None),
                self.assertRaisesRegex(OpcodeAuditError, "requires sandbox-exec"),
            ):
                opcode_audit._sandboxed_command(
                    (str(go_executable), "run"),
                    cwd=cwd,
                    readable_roots=readable_roots,
                    writable_roots=(writable_root,),
                    profile_root=probe_root,
                    network_allowed=False,
                )

            with (
                mock.patch.object(opcode_audit.sys, "platform", "darwin"),
                mock.patch.object(
                    opcode_audit.shutil,
                    "which",
                    return_value="/usr/bin/sandbox-exec",
                ),
            ):
                darwin_offline = opcode_audit._sandboxed_command(
                    (str(go_executable), "run"),
                    cwd=cwd,
                    readable_roots=readable_roots,
                    writable_roots=(writable_root,),
                    profile_root=probe_root,
                    network_allowed=False,
                )
                darwin_online = opcode_audit._sandboxed_command(
                    (str(go_executable), "mod", "download"),
                    cwd=cwd,
                    readable_roots=readable_roots,
                    writable_roots=(writable_root,),
                    profile_root=probe_root,
                    network_allowed=True,
                )
            darwin_offline_profile = Path(darwin_offline[2]).read_text(encoding="utf-8")
            darwin_online_profile = Path(darwin_online[2]).read_text(encoding="utf-8")
            self.assertNotIn("(allow file-read*)", darwin_offline_profile)
            self.assertNotIn("(allow network*)", darwin_offline_profile)
            self.assertIn("(allow network*)", darwin_online_profile)

            with (
                mock.patch.object(opcode_audit.sys, "platform", "linux"),
                mock.patch.object(
                    opcode_audit.shutil, "which", return_value="/usr/bin/true"
                ),
            ):
                offline_command = opcode_audit._sandboxed_command(
                    (str(go_executable), "run"),
                    cwd=cwd,
                    readable_roots=readable_roots,
                    writable_roots=(writable_root,),
                    profile_root=probe_root,
                    network_allowed=False,
                )
                online_command = opcode_audit._sandboxed_command(
                    (str(go_executable), "mod", "download"),
                    cwd=cwd,
                    readable_roots=readable_roots,
                    writable_roots=(writable_root,),
                    profile_root=probe_root,
                    network_allowed=True,
                )
            self.assertIn("--unshare-net", offline_command)
            self.assertNotIn("--unshare-net", online_command)
            self.assertIn("--ro-bind", offline_command)
            self.assertEqual(
                offline_command[-2:], (str(go_executable.resolve()), "run")
            )

            forbidden_roots = {
                str(Path(Path.home().anchor)),
                str(Path.home().resolve()),
                str(opcode_audit.REPO_ROOT.resolve()),
            }
            for command in (offline_command, online_command):
                for option in ("--ro-bind", "--bind"):
                    for index, argument in enumerate(command):
                        if argument != option:
                            continue
                        source, destination = command[index + 1 : index + 3]
                        self.assertNotIn(source, forbidden_roots)
                        self.assertNotIn(destination, forbidden_roots)

    def test_darwin_rosetta_runtime_root_is_read_only_in_generated_profile(self):
        declared_rosetta_root = Path("/Library/Apple/usr/libexec/oah")
        self.assertIn(
            declared_rosetta_root, opcode_audit.DARWIN_SANDBOX_SYSTEM_READ_PATHS
        )
        self.assertNotIn(
            declared_rosetta_root, opcode_audit.DARWIN_SANDBOX_SYSTEM_WRITE_PATHS
        )

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            rosetta_root = root / "oah"
            readable_root = root / "readable"
            writable_root = readable_root / "writable"
            for directory in (rosetta_root, writable_root):
                directory.mkdir(parents=True)
            executable = readable_root / "go"
            executable.write_bytes(b"test executable")
            executable.chmod(0o700)

            with (
                mock.patch.object(
                    opcode_audit,
                    "DARWIN_SANDBOX_SYSTEM_READ_PATHS",
                    (rosetta_root,),
                ),
                mock.patch.object(
                    opcode_audit, "DARWIN_SANDBOX_SYSTEM_WRITE_PATHS", ()
                ),
            ):
                profile = opcode_audit._write_darwin_sandbox_profile(
                    executable=executable,
                    readable_roots=(readable_root,),
                    writable_roots=(writable_root,),
                    profile_root=root,
                    network_allowed=False,
                )

            profile_source = profile.read_text(encoding="utf-8")
            rosetta = opcode_audit._sandbox_profile_string(rosetta_root.resolve())
            self.assertIn(f'(allow file-read* (subpath "{rosetta}"))', profile_source)
            self.assertNotIn(
                f'(allow file-write* (subpath "{rosetta}"))', profile_source
            )
            self.assertNotIn(
                f'(allow file-write* (literal "{rosetta}"))', profile_source
            )

    def test_go_executable_must_resolve_inside_its_reported_goroot(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            go_root = root / "go"
            go_bin = go_root / "bin"
            go_bin.mkdir(parents=True)
            go_executable = go_bin / "go"
            go_executable.write_bytes(b"test executable")
            go_executable.chmod(0o700)

            resolved = opcode_audit._validate_go_toolchain_paths(
                go_executable, str(go_root)
            )
            self.assertEqual(resolved, (go_executable.resolve(), go_root.resolve()))

            outside = root / "outside/go"
            outside.parent.mkdir()
            outside.write_bytes(b"test executable")
            outside.chmod(0o700)
            linked_go = go_bin / "linked-go"
            linked_go.symlink_to(outside)
            with self.assertRaisesRegex(OpcodeAuditError, "outside GOROOT/bin"):
                opcode_audit._validate_go_toolchain_paths(linked_go, str(go_root))

    def test_probe_rejects_reported_goroot_before_loading_upstream_modules(self):
        revision = "e" * 40
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            geth, helper, go_executable = self._minimal_probe_files(root)
            false_go_root = root / "false-go-root"
            (false_go_root / "bin").mkdir(parents=True)
            completed = opcode_audit.BoundedProcessResult(
                returncode=0,
                stdout=json.dumps(
                    {"GOROOT": str(false_go_root), "GOVERSION": "go1.24.0"}
                ),
                stderr="",
                stdout_sha256="a",
                stderr_sha256="b",
            )
            with mock.patch.object(
                opcode_audit, "_run_bounded_process", return_value=completed
            ) as run:
                with self.assertRaisesRegex(OpcodeAuditError, "outside GOROOT/bin"):
                    run_geth_opcode_probe(
                        geth_root=geth,
                        geth_revision=revision,
                        request=self._empty_probe_request(),
                        helper=helper,
                        go_executable=str(go_executable),
                        sandbox_required=False,
                    )
            self.assertEqual(run.call_count, 1)

    @unittest.skipUnless(
        sys.platform == "darwin" and shutil.which("sandbox-exec"),
        "Darwin sandbox-exec is unavailable",
    )
    def test_darwin_sandbox_denies_host_sentinel_outside_capability_roots(self):
        secret = "neverd-host-sentinel-8f80b5df"
        with (
            tempfile.TemporaryDirectory(prefix="neverd-sandbox-allowed-") as allowed,
            tempfile.TemporaryDirectory(prefix="neverd-sandbox-host-") as host,
        ):
            allowed_root = Path(allowed)
            allowed_file = allowed_root / "allowed.txt"
            allowed_file.write_text("allowed\n", encoding="utf-8")
            sentinel = Path(host) / "sentinel.txt"
            sentinel.write_text(secret, encoding="utf-8")

            for network_allowed in (False, True):
                with self.subTest(network_allowed=network_allowed):
                    allowed_command = opcode_audit._sandboxed_command(
                        ("/bin/cat", str(allowed_file.resolve())),
                        cwd=allowed_root,
                        readable_roots=(allowed_root,),
                        writable_roots=(allowed_root,),
                        profile_root=allowed_root,
                        network_allowed=network_allowed,
                    )
                    allowed_result = opcode_audit._run_bounded_process(
                        allowed_command,
                        cwd=allowed_root,
                        environment={"PATH": os.environ.get("PATH", "")},
                        timeout_seconds=5,
                    )
                    self.assertEqual(allowed_result.returncode, 0)
                    self.assertEqual(allowed_result.stdout, "allowed\n")

                    denied_command = opcode_audit._sandboxed_command(
                        ("/bin/cat", str(sentinel.resolve())),
                        cwd=allowed_root,
                        readable_roots=(allowed_root,),
                        writable_roots=(allowed_root,),
                        profile_root=allowed_root,
                        network_allowed=network_allowed,
                    )
                    denied_result = opcode_audit._run_bounded_process(
                        denied_command,
                        cwd=allowed_root,
                        environment={"PATH": os.environ.get("PATH", "")},
                        timeout_seconds=5,
                    )
                    combined_output = denied_result.stdout + denied_result.stderr
                    self.assertNotEqual(denied_result.returncode, 0)
                    self.assertNotIn(secret, combined_output)

    @requires_bounded_process
    def test_fetch_ignores_global_url_rewrites(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            upstream = root / "upstream"
            substitute = root / "substitute"
            cache = root / "cache.git"
            for repository, opcode in ((upstream, "STOP"), (substitute, "ADD")):
                repository.mkdir()
                self._git(repository, "init", "--initial-branch=master")
                self._git(repository, "config", "user.name", "NeverD Test")
                self._git(repository, "config", "user.email", "neverd@example.invalid")
                opcode_path = repository / "core/vm/opcodes.go"
                opcode_path.parent.mkdir(parents=True)
                opcode_path.write_text(
                    f"const (\n    {opcode} OpCode = 0x00\n)\n", encoding="utf-8"
                )
                self._git(repository, "add", "core/vm/opcodes.go")
                self._git(repository, "commit", "-m", "add opcode")

            global_config = root / "global.gitconfig"
            global_config.write_text(
                f'[url "{substitute}"]\n\tinsteadOf = {upstream}\n',
                encoding="utf-8",
            )
            with mock.patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(global_config)}):
                source = fetch_geth_opcode_source(
                    remote=str(upstream), ref="HEAD", cache=cache
                )

            self.assertIn("STOP", source.text)
            self.assertNotIn("ADD", source.text)

    @requires_bounded_process
    def test_concurrent_fetches_keep_independent_authority_refs(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            cache = root / "cache.git"
            upstreams = []
            for name, opcode in (("upstream-a", "STOP"), ("upstream-b", "ADD")):
                repository = root / name
                repository.mkdir()
                self._git(repository, "init", "--initial-branch=master")
                self._git(repository, "config", "user.name", "NeverD Test")
                self._git(repository, "config", "user.email", "neverd@example.invalid")
                opcode_path = repository / "core/vm/opcodes.go"
                opcode_path.parent.mkdir(parents=True)
                opcode_path.write_text(
                    f"const (\n    {opcode} OpCode = 0x00\n)\n", encoding="utf-8"
                )
                self._git(repository, "add", "core/vm/opcodes.go")
                self._git(repository, "commit", "-m", "add opcode")
                upstreams.append(repository)

            opcode_audit._prepare_bare_cache(cache, "git")
            first_fetch_done = threading.Event()
            second_fetch_done = threading.Event()
            original_run_git = opcode_audit._run_git

            def interleaved_run_git(arguments, **kwargs):
                is_fetch = "fetch" in arguments
                if is_fetch and str(upstreams[0]) in arguments:
                    result = original_run_git(arguments, **kwargs)
                    first_fetch_done.set()
                    if not second_fetch_done.wait(timeout=10):
                        raise AssertionError("second fetch did not complete")
                    return result
                if is_fetch and str(upstreams[1]) in arguments:
                    if not first_fetch_done.wait(timeout=10):
                        raise AssertionError("first fetch did not complete")
                    result = original_run_git(arguments, **kwargs)
                    second_fetch_done.set()
                    return result
                return original_run_git(arguments, **kwargs)

            with (
                mock.patch.object(
                    opcode_audit, "_run_git", side_effect=interleaved_run_git
                ),
                concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor,
            ):
                futures = [
                    executor.submit(
                        fetch_geth_opcode_source,
                        remote=str(repository),
                        ref="HEAD",
                        cache=cache,
                    )
                    for repository in upstreams
                ]
                first, second = (future.result(timeout=15) for future in futures)

            self.assertIn("STOP", first.text)
            self.assertNotIn("ADD", first.text)
            self.assertIn("ADD", second.text)
            self.assertNotIn("STOP", second.text)
            self.assertNotEqual(first.authority_ref, second.authority_ref)

    @requires_bounded_process
    def test_checks_out_the_exact_fetched_revision_temporarily(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            upstream = root / "upstream"
            cache = root / "cache.git"
            upstream.mkdir()
            self._git(upstream, "init", "--initial-branch=master")
            self._git(upstream, "config", "user.name", "NeverD Test")
            self._git(upstream, "config", "user.email", "neverd@example.invalid")
            self._git(upstream, "config", "commit.gpgsign", "false")
            opcode_path = upstream / "core/vm/opcodes.go"
            opcode_path.parent.mkdir(parents=True)
            opcode_path.write_text(
                "const (\n    STOP OpCode = 0x00\n)\n", encoding="utf-8"
            )
            self._git(upstream, "add", "core/vm/opcodes.go")
            self._git(upstream, "commit", "-m", "add opcode source")

            xdg_root = root / "untrusted-xdg"
            attributes = xdg_root / "git/attributes"
            attributes.parent.mkdir(parents=True)
            attributes.write_text(
                "*.go working-tree-encoding=UTF-16LE\n", encoding="utf-8"
            )
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": str(xdg_root)}):
                source = fetch_geth_opcode_source(
                    remote=str(upstream), ref="HEAD", cache=cache
                )
                with checkout_geth_revision(
                    cache=cache,
                    revision=source.revision,
                    authority_ref=source.authority_ref,
                ) as checkout:
                    checkout_path = checkout
                    self.assertEqual(
                        self._git(checkout, "rev-parse", "HEAD"), source.revision
                    )
                    self.assertEqual(
                        (checkout / "core/vm/opcodes.go").read_text(encoding="utf-8"),
                        source.text,
                    )
            self.assertFalse(checkout_path.exists())
            authority = subprocess.run(
                (
                    "git",
                    f"--git-dir={cache}",
                    "show-ref",
                    "--verify",
                    source.authority_ref,
                ),
                check=False,
                capture_output=True,
            )
            self.assertNotEqual(authority.returncode, 0)

    def test_probe_go_commands_share_one_total_timeout(self):
        revision = "a" * 40
        audit_time = 123
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            geth, helper, go_executable = self._minimal_probe_files(root)
            response = {
                **self._stop_manifest(revision, "go1.24.0", 1024),
                "authority": opcode_audit.GETH_AUDIT_AUTHORITY,
                "geth_remote": DEFAULT_GETH_REMOTE,
                "geth_ref": DEFAULT_GETH_REF,
                "audit_unix_time": audit_time,
                "rule_probes": [],
                "mainnet": {},
                "eip8024": {},
            }
            observed_timeouts = []

            def run_go(command, **kwargs):
                observed_timeouts.append(kwargs["timeout_seconds"])
                stdout = ""
                if command[1:3] == ("env", "-json"):
                    go_root = str(Path(command[0]).parent.parent)
                    stdout = json.dumps({"GOROOT": go_root, "GOVERSION": "go1.24.0"})
                elif command[1] == "run":
                    stdout = json.dumps(response)
                return opcode_audit.BoundedProcessResult(
                    returncode=0,
                    stdout=stdout,
                    stderr="",
                    stdout_sha256="a",
                    stderr_sha256="b",
                )

            clock = mock.Mock(
                side_effect=(
                    100.0,
                    101.0,
                    103.0,
                    106.0,
                    110.0,
                    115.0,
                    121.0,
                    128.0,
                    136.0,
                )
            )
            with (
                mock.patch.object(opcode_audit.time, "monotonic", clock),
                mock.patch.object(
                    opcode_audit, "_run_bounded_process", side_effect=run_go
                ),
            ):
                result = run_geth_opcode_probe(
                    geth_root=geth,
                    geth_revision=revision,
                    request=self._empty_probe_request(),
                    helper=helper,
                    go_executable=str(go_executable),
                    audit_unix_time=audit_time,
                    sandbox_required=False,
                )

            self.assertEqual(result, response)
            self.assertEqual(len(observed_timeouts), 8)
            self.assertTrue(all(timeout > 0 for timeout in observed_timeouts))
            self.assertEqual(observed_timeouts, sorted(observed_timeouts, reverse=True))
            self.assertEqual(
                observed_timeouts[0],
                opcode_audit.GO_PROBE_TOTAL_TIMEOUT_SECONDS - 1,
            )
            self.assertEqual(
                observed_timeouts[-1],
                opcode_audit.GO_PROBE_TOTAL_TIMEOUT_SECONDS - 36,
            )

    def test_probe_stops_before_starting_a_go_command_after_deadline(self):
        revision = "b" * 40
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            geth, helper, go_executable = self._minimal_probe_files(root)

            def run_process(command, **_kwargs):
                stdout = ""
                if command[1:3] == ("env", "-json"):
                    stdout = json.dumps(
                        {
                            "GOROOT": str(Path(command[0]).parent.parent),
                            "GOVERSION": "go1.24.0",
                        }
                    )
                return opcode_audit.BoundedProcessResult(
                    returncode=0,
                    stdout=stdout,
                    stderr="",
                    stdout_sha256="a",
                    stderr_sha256="b",
                )

            run = mock.Mock(side_effect=run_process)
            clock = mock.Mock(
                side_effect=(
                    100.0,
                    101.0,
                    100.0 + opcode_audit.GO_PROBE_TOTAL_TIMEOUT_SECONDS,
                )
            )

            with (
                mock.patch.object(opcode_audit.time, "monotonic", clock),
                mock.patch.object(opcode_audit, "_run_bounded_process", run),
            ):
                with self.assertRaisesRegex(
                    OpcodeAuditError,
                    rf"shared {opcode_audit.GO_PROBE_TOTAL_TIMEOUT_SECONDS}-second timeout",
                ):
                    run_geth_opcode_probe(
                        geth_root=geth,
                        geth_revision=revision,
                        request=self._empty_probe_request(),
                        helper=helper,
                        go_executable=str(go_executable),
                        sandbox_required=False,
                    )

            self.assertEqual(run.call_count, 1)
            self.assertGreater(run.call_args.kwargs["timeout_seconds"], 0)

    def test_probe_subprocess_timeout_uses_total_deadline_diagnostic(self):
        revision = "d" * 40
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            geth, helper, go_executable = self._minimal_probe_files(root)
            run = mock.Mock(
                side_effect=subprocess.TimeoutExpired(("go", "mod"), timeout=899)
            )
            clock = mock.Mock(side_effect=(100.0, 101.0))

            with (
                mock.patch.object(opcode_audit.time, "monotonic", clock),
                mock.patch.object(opcode_audit, "_run_bounded_process", run),
            ):
                with self.assertRaisesRegex(
                    OpcodeAuditError,
                    re.escape(opcode_audit.GO_PROBE_TIMEOUT_DIAGNOSTIC),
                ):
                    run_geth_opcode_probe(
                        geth_root=geth,
                        geth_revision=revision,
                        request=self._empty_probe_request(),
                        helper=helper,
                        go_executable=str(go_executable),
                        sandbox_required=False,
                    )

            self.assertEqual(run.call_count, 1)

    @requires_bounded_process
    @unittest.skipUnless(shutil.which("go"), "Go is unavailable")
    def test_probe_uses_exported_geth_api_from_an_external_module(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            revision = "a" * 40
            geth = Path(temporary_directory) / "go-ethereum"
            params = geth / "params"
            vm = geth / "core/vm"
            params.mkdir(parents=True)
            vm.mkdir(parents=True)
            (geth / "go.mod").write_text(
                "module github.com/ethereum/go-ethereum\n\ngo 1.20\n",
                encoding="utf-8",
            )
            (geth / "go.sum").write_text("", encoding="utf-8")
            (params / "params.go").write_text(
                """package params

import "math/big"

const StackLimit = 16

type Rules struct {
    IsFuture bool
}

type Fork string

func (fork Fork) String() string { return string(fork) }

type ChainConfig struct{}

var MainnetChainConfig = &ChainConfig{}

func (*ChainConfig) Rules(*big.Int, bool, uint64) Rules {
    return Rules{IsFuture: true}
}

func (*ChainConfig) LatestFork(uint64) Fork { return Fork("Future") }
""",
                encoding="utf-8",
            )

            eip8024_overlay = Path(temporary_directory) / "eip8024-overlay.go"
            eip8024_overlay.write_text(
                """package vm

type NeverDAuditEIP8024Observation struct{}
type NeverDAuditEIP8024MissingOperand struct{}
type NeverDAuditEIP8024Spec struct {
    Name string `json:"name"`
    Byte uint8 `json:"byte"`
    Family string `json:"family"`
    OperationKind string `json:"operation_kind"`
    ValidStackDelta int `json:"valid_stack_delta"`
}
type NeverDAuditEIP8024Handler struct{}
type NeverDAuditEIP8024Table struct {
    Target string `json:"target"`
    ActiveOpcodes []string `json:"active_opcodes"`
    Handlers []NeverDAuditEIP8024Handler `json:"handlers"`
    Observations []NeverDAuditEIP8024Observation `json:"observations"`
    MissingOperand []NeverDAuditEIP8024MissingOperand `json:"missing_operand"`
}

func NeverDAuditEIP8024(
    target string, _ JumpTable, _ []NeverDAuditEIP8024Spec,
) (NeverDAuditEIP8024Table, error) {
    return NeverDAuditEIP8024Table{
        Target: target,
        ActiveOpcodes: []string{},
        Handlers: []NeverDAuditEIP8024Handler{},
        Observations: []NeverDAuditEIP8024Observation{},
        MissingOperand: []NeverDAuditEIP8024MissingOperand{},
    }, nil
}
""",
                encoding="utf-8",
            )

            def run_geth_opcode_probe(**kwargs):
                request = kwargs["request"]
                request.setdefault(
                    "rule_probes",
                    [
                        {
                            "name": "IsFuture",
                            "category": "MappedForkSelector",
                            "expected_fork": request["forks"][0]["name"],
                        }
                    ],
                )
                kwargs["eip8024_overlay"] = eip8024_overlay
                kwargs["audit_unix_time"] = 123
                kwargs["sandbox_required"] = False
                return opcode_audit.run_geth_opcode_probe(**kwargs)

            (vm / "vm.go").write_text(
                """package vm

import "github.com/ethereum/go-ethereum/params"

type operation struct {
    minimum int
    maximum int
    cost bool
    undefined bool
}

func (op *operation) Stack() (int, int) { return op.minimum, op.maximum }
func (op *operation) HasCost() bool { return op.cost }

type JumpTable [256]*operation

func LookupInstructionSet(rules params.Rules) (JumpTable, error) {
    var table JumpTable
    for index := range table {
        table[index] = &operation{maximum: params.StackLimit, undefined: true}
    }
    table[0x00] = &operation{maximum: params.StackLimit}
    table[0x01] = &operation{minimum: 2, maximum: params.StackLimit + 1, cost: true}
    table[0xfe] = &operation{maximum: params.StackLimit}
    if rules.IsFuture {
        table[0x02] = &operation{minimum: 1, maximum: params.StackLimit, cost: true}
    }
    return table, nil
}
""",
                encoding="utf-8",
            )

            probe_request = {
                "rule_fields": ["IsFuture"],
                "rule_probes": [
                    {
                        "name": "IsFuture",
                        "category": "MappedForkSelector",
                        "expected_fork": "Future",
                    }
                ],
                "forks": [
                    {"name": "Frontier", "rules": []},
                    {"name": "Future", "rules": ["IsFuture"]},
                ],
                "opcodes": [
                    {
                        "name": "STOP",
                        "byte": 0x00,
                        "active_without_cost_from": 0,
                    },
                    {"name": "ADD", "byte": 0x01},
                    {"name": "FUTURE", "byte": 0x02},
                    {
                        "name": "INVALID",
                        "byte": 0xFE,
                        "active_without_cost_from": 0,
                    },
                ],
                "eip8024_specs": [],
            }
            result = run_geth_opcode_probe(
                geth_root=geth,
                geth_revision=revision,
                request=probe_request,
                eip8024_overlay=eip8024_overlay,
                audit_unix_time=123,
                sandbox_required=False,
            )

            self.assertEqual(
                result["schema_version"], opcode_audit.GETH_PROBE_SCHEMA_VERSION
            )
            self.assertEqual(result["geth_revision"], revision)
            self.assertRegex(result["go_version"], r"^go[0-9]+\.[0-9]+")
            self.assertEqual(result["stack_limit"], 16)
            self.assertEqual(
                [table["target"] for table in result["eip8024"]["tables"]],
                ["Frontier", "Future", "mainnet.active", "mainnet.scheduled"],
            )
            self.assertTrue(
                all(
                    table["observations"] == [] for table in result["eip8024"]["tables"]
                )
            )
            self.assertEqual(
                result["forks"],
                [
                    {
                        "name": "Frontier",
                        "rules": [],
                        "opcodes": [
                            {
                                "name": "STOP",
                                "byte": 0x00,
                                "base_min_stack": 0,
                                "net_stack_delta": 0,
                            },
                            {
                                "name": "ADD",
                                "byte": 0x01,
                                "base_min_stack": 2,
                                "net_stack_delta": -1,
                            },
                            {
                                "name": "INVALID",
                                "byte": 0xFE,
                                "base_min_stack": 0,
                                "net_stack_delta": 0,
                            },
                        ],
                    },
                    {
                        "name": "Future",
                        "rules": ["IsFuture"],
                        "opcodes": [
                            {
                                "name": "STOP",
                                "byte": 0x00,
                                "base_min_stack": 0,
                                "net_stack_delta": 0,
                            },
                            {
                                "name": "ADD",
                                "byte": 0x01,
                                "base_min_stack": 2,
                                "net_stack_delta": -1,
                            },
                            {
                                "name": "FUTURE",
                                "byte": 0x02,
                                "base_min_stack": 1,
                                "net_stack_delta": 0,
                            },
                            {
                                "name": "INVALID",
                                "byte": 0xFE,
                                "base_min_stack": 0,
                                "net_stack_delta": 0,
                            },
                        ],
                    },
                ],
            )

            original_vm = (vm / "vm.go").read_text(encoding="utf-8")
            overlaid_vm = Path(temporary_directory) / "overlaid-vm.go"
            overlaid_source = original_vm.replace(
                "    return table, nil\n",
                "    table[0x03] = &operation{cost: true}\n    return table, nil\n",
            )
            self.assertNotEqual(overlaid_source, original_vm)
            overlaid_vm.write_text(overlaid_source, encoding="utf-8")
            overlay = Path(temporary_directory) / "overlay.json"
            overlay.write_text(
                json.dumps(
                    {
                        "Replace": {
                            str((vm / "vm.go").resolve()): str(overlaid_vm.resolve())
                        }
                    }
                ),
                encoding="utf-8",
            )
            go_env = Path(temporary_directory) / "go-env"
            go_env.write_text(f"GOFLAGS=-overlay={overlay}\n", encoding="utf-8")
            with mock.patch.dict(
                os.environ,
                {"GOENV": str(go_env), "GOFLAGS": f"-overlay={overlay}"},
            ):
                isolated_result = run_geth_opcode_probe(
                    geth_root=geth,
                    geth_revision=revision,
                    request=probe_request,
                )
            self.assertEqual(isolated_result["forks"], result["forks"])

            with self.assertRaisesRegex(
                OpcodeAuditError, "activates unreviewed opcode byte 0x02"
            ):
                run_geth_opcode_probe(
                    geth_root=geth,
                    geth_revision=revision,
                    request={
                        "rule_fields": ["IsFuture"],
                        "rule_probes": [
                            {
                                "name": "IsFuture",
                                "category": "MappedForkSelector",
                                "expected_fork": "Future",
                            }
                        ],
                        "forks": [{"name": "Future", "rules": ["IsFuture"]}],
                        "opcodes": [
                            {
                                "name": "STOP",
                                "byte": 0x00,
                                "active_without_cost_from": 0,
                            },
                            {"name": "ADD", "byte": 0x01},
                            {
                                "name": "INVALID",
                                "byte": 0xFE,
                                "active_without_cost_from": 0,
                            },
                        ],
                        "eip8024_specs": [],
                    },
                )

            with self.assertRaisesRegex(
                OpcodeAuditError, "zero-cost activation policy mismatch at byte 0x00"
            ):
                run_geth_opcode_probe(
                    geth_root=geth,
                    geth_revision=revision,
                    request={
                        "rule_fields": ["IsFuture"],
                        "rule_probes": [
                            {
                                "name": "IsFuture",
                                "category": "MappedForkSelector",
                                "expected_fork": "Frontier",
                            }
                        ],
                        "forks": [{"name": "Frontier", "rules": []}],
                        "opcodes": [
                            {"name": "STOP", "byte": 0x00},
                            {"name": "ADD", "byte": 0x01},
                            {
                                "name": "INVALID",
                                "byte": 0xFE,
                                "active_without_cost_from": 0,
                            },
                        ],
                        "eip8024_specs": [],
                    },
                )

            with self.assertRaisesRegex(
                OpcodeAuditError, "unexpected field unexpected"
            ):
                run_geth_opcode_probe(
                    geth_root=geth,
                    geth_revision=revision,
                    request={
                        "rule_fields": ["IsFuture"],
                        "rule_probes": [
                            {
                                "name": "IsFuture",
                                "category": "MappedForkSelector",
                                "expected_fork": "Frontier",
                            }
                        ],
                        "forks": [{"name": "Frontier", "rules": []}],
                        "opcodes": [
                            {
                                "name": "STOP",
                                "byte": 0x00,
                                "active_without_cost_from": 0,
                            }
                        ],
                        "eip8024_specs": [],
                        "unexpected": True,
                    },
                )

            vm_source = (vm / "vm.go").read_text(encoding="utf-8")
            zero_cost_future = vm_source.replace(
                "table[0x02] = &operation{minimum: 1, maximum: params.StackLimit, cost: true}",
                "table[0x02] = &operation{minimum: 1, maximum: params.StackLimit}",
            )
            self.assertNotEqual(zero_cost_future, vm_source)
            (vm / "vm.go").write_text(zero_cost_future, encoding="utf-8")
            with self.assertRaisesRegex(
                OpcodeAuditError, "activates unreviewed opcode byte 0x02"
            ):
                run_geth_opcode_probe(
                    geth_root=geth,
                    geth_revision=revision,
                    request={
                        "rule_fields": ["IsFuture"],
                        "rule_probes": [
                            {
                                "name": "IsFuture",
                                "category": "MappedForkSelector",
                                "expected_fork": "Future",
                            }
                        ],
                        "forks": [{"name": "Future", "rules": ["IsFuture"]}],
                        "opcodes": [
                            {
                                "name": "STOP",
                                "byte": 0x00,
                                "active_without_cost_from": 0,
                            },
                            {"name": "ADD", "byte": 0x01},
                            {
                                "name": "INVALID",
                                "byte": 0xFE,
                                "active_without_cost_from": 0,
                            },
                        ],
                        "eip8024_specs": [],
                    },
                )

            missing_undefined_marker = vm_source.replace(
                "    undefined bool\n", ""
            ).replace(", undefined: true", "")
            self.assertNotEqual(missing_undefined_marker, vm_source)
            (vm / "vm.go").write_text(missing_undefined_marker, encoding="utf-8")
            with self.assertRaisesRegex(
                OpcodeAuditError, "operation.undefined metadata is unavailable"
            ):
                run_geth_opcode_probe(
                    geth_root=geth,
                    geth_revision=revision,
                    request={
                        "rule_fields": ["IsFuture"],
                        "rule_probes": [
                            {
                                "name": "IsFuture",
                                "category": "MappedForkSelector",
                                "expected_fork": "Frontier",
                            }
                        ],
                        "forks": [{"name": "Frontier", "rules": []}],
                        "opcodes": [
                            {
                                "name": "STOP",
                                "byte": 0x00,
                                "active_without_cost_from": 0,
                            },
                            {"name": "ADD", "byte": 0x01},
                            {
                                "name": "INVALID",
                                "byte": 0xFE,
                                "active_without_cost_from": 0,
                            },
                        ],
                        "eip8024_specs": [],
                    },
                )

    def test_main_audits_exported_api_at_the_exact_fetched_revision(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            geth = root / "go-ethereum"
            params = geth / "params"
            vm = geth / "core/vm"
            params.mkdir(parents=True)
            vm.mkdir(parents=True)
            (geth / "go.mod").write_text(
                "module github.com/ethereum/go-ethereum\n\ngo 1.20\n",
                encoding="utf-8",
            )
            (geth / "go.sum").write_text("", encoding="utf-8")
            (params / "params.go").write_text(
                """package params

const StackLimit = 16

type Rules struct{}
""",
                encoding="utf-8",
            )
            (vm / "opcodes.go").write_text(
                """package vm

type OpCode byte

const (
    STOP OpCode = 0x00
    ADD OpCode = 0x01
)
""",
                encoding="utf-8",
            )
            (vm / "vm.go").write_text(
                """package vm

import "github.com/ethereum/go-ethereum/params"

type operation struct {
    minimum int
    maximum int
    cost bool
    undefined bool
}

func (op *operation) Stack() (int, int) { return op.minimum, op.maximum }
func (op *operation) HasCost() bool { return op.cost }

type JumpTable [256]*operation

func LookupInstructionSet(params.Rules) (JumpTable, error) {
    var table JumpTable
    for index := range table {
        table[index] = &operation{maximum: params.StackLimit, undefined: true}
    }
    table[0x00] = &operation{maximum: params.StackLimit}
    table[0x01] = &operation{
        minimum: 2, maximum: params.StackLimit + 1, cost: true,
    }
    return table, nil
}
""",
                encoding="utf-8",
            )
            self._git(geth, "init", "--initial-branch=master")
            self._git(geth, "config", "user.name", "NeverD Test")
            self._git(geth, "config", "user.email", "neverd@example.invalid")
            self._git(geth, "config", "commit.gpgsign", "false")
            self._git(geth, "add", ".")
            self._git(geth, "commit", "-m", "add exported EVM API")
            revision = self._git(geth, "rev-parse", "HEAD")

            opcodes = root / "EVMOpcodes.def"
            opcodes.write_text(
                """EVM_OPCODE(STOP, 0x00, 0, 0, 0, None, Control, Frontier,
                           Halt, None, None, None, true)
EVM_OPCODE(ADD, 0x01, 2, 1, 0, None, Arithmetic, Frontier,
           None, None, None, None, false)
""",
                encoding="utf-8",
                newline="\n",
            )
            hardforks = root / "EVMHardforks.def"
            hardforks.write_text(
                'EVM_HARDFORK(Frontier, "frontier")\n'
                'EVM_HARDFORK_LATEST(Frontier, "latest")\n',
                encoding="utf-8",
                newline="\n",
            )
            fork_aliases = root / "EVMUpstreamForkAliases.def"
            fork_aliases.write_text(
                "EVM_GETH_FORK_ALIAS(Paris, Frontier)\n",
                encoding="utf-8",
                newline="\n",
            )
            opcode_policy = root / "EVMUpstreamOpcodePolicy.def"
            opcode_policy.write_text("", encoding="utf-8", newline="\n")
            semantics_policy = root / "EVMUpstreamSemanticsPolicy.def"
            semantics_policy.write_text(
                """EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_ACTIVE_WITHOUT_COST(STOP, Frontier)
""",
                encoding="utf-8",
                newline="\n",
            )
            constants = root / "EVMConstants.h"
            constants.write_text(
                "inline constexpr std::size_t kStackLimit = 16;\n",
                encoding="utf-8",
                newline="\n",
            )
            manifest_path = root / "manifest.json"

            checkout = mock.MagicMock()
            checkout.__enter__.return_value = geth
            checkout.__exit__.return_value = False

            def probe_response(**kwargs):
                return {
                    "schema_version": opcode_audit.GETH_PROBE_SCHEMA_VERSION,
                    "authority": opcode_audit.GETH_AUDIT_AUTHORITY,
                    "geth_remote": DEFAULT_GETH_REMOTE,
                    "geth_ref": DEFAULT_GETH_REF,
                    "geth_revision": revision,
                    "audit_unix_time": kwargs["audit_unix_time"],
                    "go_version": "go1.24.0",
                    "stack_limit": 16,
                    "forks": [
                        {
                            "name": "Frontier",
                            "rules": [],
                            "opcodes": [
                                {
                                    "name": "STOP",
                                    "byte": 0,
                                    "base_min_stack": 0,
                                    "net_stack_delta": 0,
                                },
                                {
                                    "name": "ADD",
                                    "byte": 1,
                                    "base_min_stack": 2,
                                    "net_stack_delta": -1,
                                },
                            ],
                        }
                    ],
                    "rule_probes": [],
                    "mainnet": {},
                    "eip8024": {},
                }

            output = io.StringIO()
            with (
                mock.patch.object(
                    opcode_audit,
                    "fetch_geth_opcode_source",
                    return_value=GethOpcodeSource(
                        text=(vm / "opcodes.go").read_text(encoding="utf-8"),
                        revision=revision,
                    ),
                ) as fetch,
                mock.patch.object(
                    opcode_audit, "checkout_geth_revision", return_value=checkout
                ),
                mock.patch.object(
                    opcode_audit,
                    "run_geth_opcode_probe",
                    side_effect=probe_response,
                ),
                mock.patch.object(opcode_audit, "audit_geth_rule_probes"),
                mock.patch.object(opcode_audit, "audit_geth_mainnet_forks"),
                mock.patch.object(opcode_audit, "audit_geth_eip8024_immediates"),
                redirect_stdout(output),
            ):
                result = main(
                    [
                        "--manifest-output",
                        str(manifest_path),
                    ],
                    input_paths=opcode_audit.AuditInputPaths(
                        neverd_opcodes=opcodes,
                        neverd_hardforks=hardforks,
                        neverd_constants=constants,
                        opcode_policy=opcode_policy,
                        semantics_policy=semantics_policy,
                        geth_fork_aliases=fork_aliases,
                    ),
                )

            fetch.assert_called_once_with(
                remote=DEFAULT_GETH_REMOTE,
                ref=DEFAULT_GETH_REF,
                cache=mock.ANY,
                git_executable="git",
            )

            self.assertEqual(result, 0)
            self.assertIn(revision, output.getvalue())
            self.assertIn("Fetched go-ethereum revision", output.getvalue())
            self.assertIn("exported instruction sets match", output.getvalue())
            self.assertIn("2 opcodes", output.getvalue())
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(
                set(manifest),
                {
                    "schema_version",
                    "authority",
                    "geth_remote",
                    "geth_ref",
                    "geth_revision",
                    "audit_unix_time",
                    "go_version",
                    "stack_limit",
                    "forks",
                    "rule_probes",
                    "mainnet",
                    "eip8024",
                    "diagnostics",
                },
            )
            self.assertEqual(manifest["geth_revision"], revision)
            self.assertEqual(manifest["stack_limit"], 16)
            self.assertEqual(manifest["diagnostics"], [])
            self.assertEqual(manifest["forks"][0]["name"], "Frontier")

    def test_main_records_the_fetched_revision_when_the_audit_fails(self):
        revision = "a" * 40
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            manifest_path = root / "manifest.json"
            manifest_path.write_text("incomplete", encoding="utf-8")
            output = io.StringIO()
            error_output = io.StringIO()
            with mock.patch(
                "scripts.audit_evm_opcode_metadata.fetch_geth_opcode_source",
                return_value=GethOpcodeSource(text="package vm\n", revision=revision),
            ):
                with redirect_stdout(output), redirect_stderr(error_output):
                    result = main(["--manifest-output", str(manifest_path)])

            self.assertEqual(result, 1)
            self.assertIn(revision, output.getvalue() + error_output.getvalue())
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(
                set(manifest),
                {
                    "schema_version",
                    "authority",
                    "geth_remote",
                    "geth_ref",
                    "geth_revision",
                    "audit_unix_time",
                    "go_version",
                    "stack_limit",
                    "forks",
                    "rule_probes",
                    "mainnet",
                    "eip8024",
                    "diagnostics",
                },
            )
            self.assertEqual(manifest["geth_revision"], revision)
            self.assertTrue(manifest["diagnostics"])
            self.assertEqual(list(root.glob(".manifest.json.*.tmp")), [])

    def test_main_records_fetched_revision_when_probe_deadline_expires(self):
        revision = "c" * 40
        timeout_message = opcode_audit.GO_PROBE_TIMEOUT_DIAGNOSTIC
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            manifest_path = root / "manifest.json"
            checkout = mock.MagicMock()
            checkout.__enter__.return_value = root / "go-ethereum"
            checkout.__exit__.return_value = False

            with (
                mock.patch.object(
                    opcode_audit,
                    "fetch_geth_opcode_source",
                    return_value=GethOpcodeSource(text="", revision=revision),
                ),
                mock.patch.object(opcode_audit, "parse_geth_opcodes", return_value={}),
                mock.patch.object(
                    opcode_audit, "audit_opcodes", return_value=mock.Mock()
                ),
                mock.patch.object(
                    opcode_audit, "checkout_geth_revision", return_value=checkout
                ),
                mock.patch.object(
                    opcode_audit,
                    "run_geth_opcode_probe",
                    side_effect=OpcodeAuditError(timeout_message),
                ),
                redirect_stdout(io.StringIO()),
                redirect_stderr(io.StringIO()),
            ):
                result = main(["--manifest-output", str(manifest_path)])

            self.assertEqual(result, 1)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["geth_revision"], revision)
            self.assertEqual(manifest["diagnostics"], [timeout_message])

    def test_fetch_rejects_a_non_bare_cache(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            cache = Path(temporary_directory) / "cache.git"
            cache.mkdir()
            with self.assertRaisesRegex(OpcodeAuditError, "bare Git repository"):
                fetch_geth_opcode_source(remote="unused", ref="HEAD", cache=cache)

    @requires_bounded_process
    def test_fetch_rejects_cache_config_and_object_indirection(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            cache = root / "cache.git"
            subprocess.run(("git", "init", "--bare", "--quiet", str(cache)), check=True)
            subprocess.run(
                (
                    "git",
                    f"--git-dir={cache}",
                    "config",
                    "url.file:///tmp/untrusted.insteadOf",
                    "https://github.com/ethereum/go-ethereum.git",
                ),
                check=True,
            )
            with self.assertRaisesRegex(
                OpcodeAuditError, "unexpected local Git config"
            ):
                fetch_geth_opcode_source(remote="unused", ref="HEAD", cache=cache)

            subprocess.run(
                (
                    "git",
                    f"--git-dir={cache}",
                    "config",
                    "--unset-all",
                    "url.file:///tmp/untrusted.insteadof",
                ),
                check=True,
            )
            alternates = cache / "objects/info/alternates"
            alternates.write_text("/tmp/untrusted-objects\n", encoding="utf-8")
            with self.assertRaisesRegex(
                OpcodeAuditError, "forbidden object indirection"
            ):
                fetch_geth_opcode_source(remote="unused", ref="HEAD", cache=cache)

    def test_fetch_rejects_ambiguous_or_option_like_refs(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            cache = Path(temporary_directory) / "cache.git"
            for ref in ("master", "--upload-pack=malicious", "refs/heads/a..b"):
                with self.subTest(ref=ref):
                    with self.assertRaisesRegex(OpcodeAuditError, "full refs"):
                        fetch_geth_opcode_source(remote="unused", ref=ref, cache=cache)

    def test_parses_go_iota_and_aliases(self):
        opcodes = parse_geth_opcodes(
            """
            const (
                PUSH1 OpCode = 0x60 + iota
                PUSH2
                PUSH3
            )
            const (
                DIFFICULTY OpCode = 0x44
                PREVRANDAO OpCode = 0x44
            )
            """
        )
        self.assertEqual(opcodes["PUSH1"], 0x60)
        self.assertEqual(opcodes["PUSH3"], 0x62)
        self.assertEqual(opcodes["PREVRANDAO"], 0x44)

    def test_parses_neverd_activation_and_stack_mutation_metadata(self):
        metadata = parse_neverd_opcode_metadata(
            """
            EVM_OPCODE(ADD, 0x01, 2, 1, 0, None, Arithmetic, Frontier,
                       None, None, None, None, false)
            EVM_OPCODE(PUSH0, 0x5f, 0, 1, 0, None, Stack, Shanghai,
                       None, None, None, None, false)
            """
        )
        self.assertEqual(metadata["ADD"].byte, 0x01)
        self.assertEqual(metadata["ADD"].stack_pops, 2)
        self.assertEqual(metadata["ADD"].stack_pushes, 1)
        self.assertEqual(metadata["ADD"].introduced, "Frontier")
        self.assertEqual(metadata["PUSH0"].introduced, "Shanghai")

    def test_parses_neverd_stack_limit_from_shared_constants(self):
        self.assertEqual(
            parse_neverd_stack_limit(
                "inline constexpr std::size_t kStackLimit = 1024;\n"
            ),
            1024,
        )

    def test_latest_marker_and_geth_aliases_are_closed_declarative_data(self):
        hardfork_text = """EVM_HARDFORK(Frontier, "frontier")
EVM_HARDFORK(Fusaka, "fusaka")
EVM_HARDFORK_LATEST(Fusaka, "latest")
"""
        hardforks = parse_neverd_hardforks(hardfork_text)
        self.assertEqual(
            parse_neverd_latest_hardfork(hardfork_text, hardforks), "Fusaka"
        )
        self.assertEqual(
            parse_geth_fork_aliases(
                """EVM_GETH_FORK_ALIAS(Osaka, Fusaka)
EVM_GETH_FORK_ALIAS(BPO1, Fusaka)
""",
                hardforks,
            ),
            {"Osaka": "Fusaka", "BPO1": "Fusaka"},
        )

        with self.assertRaisesRegex(OpcodeAuditError, "exactly one"):
            parse_neverd_latest_hardfork(
                hardfork_text + 'EVM_HARDFORK_LATEST(Frontier, "latest2")\n',
                hardforks,
            )
        with self.assertRaisesRegex(OpcodeAuditError, "unknown canonical"):
            parse_geth_fork_aliases(
                "EVM_GETH_FORK_ALIAS(NewFork, Missing)\n", hardforks
            )
        with self.assertRaisesRegex(
            OpcodeAuditError, "unparsed EVM_HARDFORK_LATEST record"
        ):
            parse_neverd_latest_hardfork(
                hardfork_text + 'EVM_HARDFORK_LATEST(Frontier, "other", Unexpected)\n',
                hardforks,
            )
        with self.assertRaisesRegex(
            OpcodeAuditError, "unparsed EVM_GETH_FORK_ALIAS record"
        ):
            parse_geth_fork_aliases(
                "EVM_GETH_FORK_ALIAS(Osaka, Fusaka, Unexpected)\n", hardforks
            )

    def test_eip8024_policy_requires_one_declarative_row_per_byte(self):
        lines = []
        for encoded in range(256):
            lines.append(f"EVM_EIP8024_SINGLE_VALID(0x{encoded:02x}, {encoded + 1})")
            lines.append(f"EVM_EIP8024_PAIR_VALID(0x{encoded:02x}, 1, {encoded + 2})")
        policy = parse_eip8024_immediate_policy("\n".join(lines))
        self.assertEqual(policy.single[0], (1,))
        self.assertEqual(policy.pair[255], (1, 257))

        with self.assertRaisesRegex(OpcodeAuditError, "single byte inventory"):
            parse_eip8024_immediate_policy("\n".join(lines[:-2]))

        duplicate = lines + ["EVM_EIP8024_SINGLE_INVALID(0x00)"]
        with self.assertRaisesRegex(OpcodeAuditError, "duplicate single byte"):
            parse_eip8024_immediate_policy("\n".join(duplicate))

        malformed = lines + [
            "EVM_EIP8024_SINGLE_VALID(0x00, 1, Unexpected)",
            "EVM_EIP8024_PAIR_VALID(0x00, 1, 2, Unexpected)",
        ]
        with self.assertRaisesRegex(OpcodeAuditError, "unparsed single record"):
            parse_eip8024_immediate_policy("\n".join(malformed))

    def test_semantics_policy_parses_symbolic_fork_and_stack_overrides(self):
        policy = parse_semantics_policy(
            """
            EVM_GETH_RULE_FIELD(IsHomestead, MappedForkSelector, Homestead)
            EVM_GETH_RULE_FIELD(IsEIP155, NoOpcodeAllocation, Frontier)
            EVM_GETH_RULE_FIELD(IsUBT, ExcludedSelectorExpectedError, Cancun)
            EVM_GETH_FORK_RULE(Frontier, None)
            EVM_GETH_FORK_RULE(Homestead, IsHomestead)
            EVM_GETH_BASE_MIN_STACK(DUP1, 1)
            EVM_GETH_ACTIVE_WITHOUT_COST(STOP, Frontier)
            EVM_GETH_DYNAMIC_STACK_IMMEDIATE(DUPN, Single, Dup, 1)
            """
        )
        self.assertEqual(
            policy.rule_fields,
            ("IsHomestead", "IsEIP155", "IsUBT"),
        )
        self.assertEqual(
            policy.rule_classifications["IsHomestead"].category,
            "MappedForkSelector",
        )
        self.assertEqual(
            policy.rule_classifications["IsEIP155"].expected_fork,
            "Frontier",
        )
        self.assertEqual(
            policy.rule_classifications["IsUBT"].category,
            "ExcludedSelectorExpectedError",
        )
        self.assertEqual(
            policy.fork_rules,
            (("Frontier", None), ("Homestead", "IsHomestead")),
        )
        self.assertEqual(policy.base_min_stack, {"DUP1": 1})
        self.assertEqual(policy.active_without_cost, {"STOP": "Frontier"})
        self.assertEqual(
            policy.dynamic_stack_immediates["DUPN"],
            opcode_audit.DynamicStackImmediateSpec(
                family="single", operation_kind="dup", valid_stack_delta=1
            ),
        )

    def test_semantics_policy_rejects_malformed_dynamic_stack_record(self):
        with self.assertRaisesRegex(
            OpcodeAuditError, "unparsed EVM_GETH_DYNAMIC_STACK_IMMEDIATE record"
        ):
            parse_semantics_policy(
                """EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_DYNAMIC_STACK_IMMEDIATE(DUPN, Single, Dup, 1, Unexpected)
"""
            )

    def test_every_upstream_policy_macro_rejects_unparsed_records(self):
        opcode_policy_cases = (
            (
                "EVM_UPSTREAM_OPCODE_ALIAS(SHA3, KECCAK256, Unexpected)\n",
                "EVM_UPSTREAM_OPCODE_ALIAS",
            ),
            (
                "EVM_UPSTREAM_OPCODE_EXCLUSION_KIND(Bad, true, false)\n",
                "EVM_UPSTREAM_OPCODE_EXCLUSION_KIND",
            ),
            (
                "EVM_UPSTREAM_OPCODE_IGNORE(BAD, 0xaa, Reason, Unexpected)\n",
                "EVM_UPSTREAM_OPCODE_IGNORE",
            ),
        )
        for text, macro in opcode_policy_cases:
            with (
                self.subTest(macro=macro),
                self.assertRaisesRegex(OpcodeAuditError, f"unparsed {macro} record"),
            ):
                parse_policy(text)

        semantics_policy_cases = (
            (
                "EVM_GETH_FORK_RULE(Frontier, None, Unexpected)\n",
                "EVM_GETH_FORK_RULE",
            ),
            (
                "EVM_GETH_FORK_RULE(Frontier, None)\n"
                "EVM_GETH_BASE_MIN_STACK(DUP1, 1, Unexpected)\n",
                "EVM_GETH_BASE_MIN_STACK",
            ),
            (
                "EVM_GETH_FORK_RULE(Frontier, None)\n"
                "EVM_GETH_ACTIVE_WITHOUT_COST(STOP, Frontier) trailing\n",
                "EVM_GETH_ACTIVE_WITHOUT_COST",
            ),
        )
        for text, macro in semantics_policy_cases:
            with (
                self.subTest(macro=macro),
                self.assertRaisesRegex(OpcodeAuditError, f"unparsed {macro} record"),
            ):
                parse_semantics_policy(text)

    def test_canonical_opcode_and_hardfork_macros_are_fully_consumed(self):
        with self.assertRaisesRegex(OpcodeAuditError, "unparsed EVM_OPCODE record"):
            parse_neverd_opcode_metadata(
                """EVM_OPCODE(STOP, 0x00, 0, 0, 0, None, Control, Frontier,
                              Halt, None, None, None, true, Unexpected)
"""
            )
        with self.assertRaisesRegex(OpcodeAuditError, "unparsed EVM_HARDFORK record"):
            parse_neverd_hardforks('EVM_HARDFORK(Frontier, "frontier", Unexpected)\n')

    def test_semantics_policy_classifies_every_rule_exactly_once(self):
        with self.assertRaisesRegex(OpcodeAuditError, "duplicate geth rule field"):
            parse_semantics_policy(
                """EVM_GETH_RULE_FIELD(IsHomestead, MappedForkSelector, Homestead)
EVM_GETH_RULE_FIELD(IsHomestead, NoOpcodeAllocation, Frontier)
EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_FORK_RULE(Homestead, IsHomestead)
"""
            )

        with self.assertRaisesRegex(OpcodeAuditError, "unknown rule category"):
            parse_semantics_policy(
                """EVM_GETH_RULE_FIELD(IsHomestead, Unreviewed, Homestead)
EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_FORK_RULE(Homestead, IsHomestead)
"""
            )

        with self.assertRaisesRegex(
            OpcodeAuditError, "unparsed EVM_GETH_RULE_FIELD record"
        ):
            parse_semantics_policy(
                """EVM_GETH_RULE_FIELD(IsHomestead, MappedForkSelector,
                                        Homestead, Unexpected)
EVM_GETH_FORK_RULE(Frontier, None)
"""
            )

    def test_probe_request_is_driven_by_closed_def_inventories(self):
        hardforks = parse_neverd_hardforks(
            """
            EVM_HARDFORK(Frontier, "frontier")
            EVM_HARDFORK(Homestead, "homestead")
            EVM_HARDFORK_ALIAS("latest", Homestead)
            """
        )
        opcodes = parse_neverd_opcode_metadata(
            """
            EVM_OPCODE(STOP, 0x00, 0, 0, 0, None, Control, Frontier,
                       Halt, None, None, None, true)
            EVM_OPCODE(DUP1, 0x80, 0, 1, 0, None, Stack, Frontier,
                       None, None, None, None, false)
            """
        )
        semantics = parse_semantics_policy(
            """
            EVM_GETH_RULE_FIELD(IsHomestead, MappedForkSelector, Homestead)
            EVM_GETH_FORK_RULE(Frontier, None)
            EVM_GETH_FORK_RULE(Homestead, IsHomestead)
            EVM_GETH_BASE_MIN_STACK(DUP1, 1)
            EVM_GETH_ACTIVE_WITHOUT_COST(STOP, Frontier)
            """
        )

        request = build_geth_probe_request(opcodes, hardforks, semantics)
        self.assertEqual(request["rule_fields"], ["IsHomestead"])
        self.assertEqual(
            request["rule_probes"],
            [
                {
                    "name": "IsHomestead",
                    "category": "MappedForkSelector",
                    "expected_fork": "Homestead",
                }
            ],
        )
        self.assertEqual(
            request["forks"],
            [
                {"name": "Frontier", "rules": []},
                {"name": "Homestead", "rules": ["IsHomestead"]},
            ],
        )
        self.assertEqual(
            request["opcodes"],
            [
                {
                    "name": "STOP",
                    "byte": 0x00,
                    "active_without_cost_from": 0,
                },
                {"name": "DUP1", "byte": 0x80},
            ],
        )
        self.assertEqual(request["eip8024_specs"], [])

    def test_rule_probes_compare_every_single_field_table_and_error_contract(self):
        policy = parse_semantics_policy(
            """EVM_GETH_RULE_FIELD(IsHomestead, MappedForkSelector, Homestead)
EVM_GETH_RULE_FIELD(IsEIP155, NoOpcodeAllocation, Frontier)
EVM_GETH_RULE_FIELD(IsUBT, ExcludedSelectorExpectedError, Frontier)
EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_FORK_RULE(Homestead, IsHomestead)
"""
        )
        frontier = [
            {"name": "STOP", "byte": 0, "base_min_stack": 0, "net_stack_delta": 0}
        ]
        homestead = frontier + [
            {"name": "NEW", "byte": 1, "base_min_stack": 0, "net_stack_delta": 0}
        ]
        manifest = [
            {
                "name": "IsEIP155",
                "category": "NoOpcodeAllocation",
                "expected_fork": "Frontier",
                "lookup_error": False,
                "opcodes": frontier,
            },
            {
                "name": "IsHomestead",
                "category": "MappedForkSelector",
                "expected_fork": "Homestead",
                "lookup_error": False,
                "opcodes": homestead,
            },
            {
                "name": "IsUBT",
                "category": "ExcludedSelectorExpectedError",
                "expected_fork": "Frontier",
                "lookup_error": True,
                "opcodes": frontier,
            },
        ]
        audit_geth_rule_probes(
            policy,
            {"Frontier": frontier, "Homestead": homestead},
            manifest,
        )

        manifest[-1] = {**manifest[-1], "lookup_error": False}
        with self.assertRaisesRegex(OpcodeAuditError, "lookup error contract"):
            audit_geth_rule_probes(
                policy,
                {"Frontier": frontier, "Homestead": homestead},
                manifest,
            )

    def test_mainnet_latest_and_scheduled_forks_map_to_audited_tables(self):
        fusaka = [
            {"name": "CLZ", "byte": 0x1E, "base_min_stack": 1, "net_stack_delta": 0}
        ]
        amsterdam = fusaka + [
            {"name": "DUPN", "byte": 0xE6, "base_min_stack": 1, "net_stack_delta": 1}
        ]
        mainnet = {
            "active": {
                "upstream_fork": "BPO2",
                "rules": ["IsOsaka"],
                "opcodes": fusaka,
            },
            "scheduled": {
                "upstream_fork": "Amsterdam",
                "rules": ["IsAmsterdam"],
                "opcodes": amsterdam,
            },
        }
        audit_geth_mainnet_forks(
            mainnet,
            latest_hardfork="Fusaka",
            geth_fork_aliases={"BPO2": "Fusaka", "Amsterdam": "Amsterdam"},
            fork_opcode_records={"Fusaka": fusaka, "Amsterdam": amsterdam},
            rule_fields=("IsOsaka", "IsAmsterdam"),
        )

        with self.assertRaisesRegex(OpcodeAuditError, "unknown upstream fork"):
            audit_geth_mainnet_forks(
                {
                    **mainnet,
                    "scheduled": {**mainnet["scheduled"], "upstream_fork": "NewFork"},
                },
                latest_hardfork="Fusaka",
                geth_fork_aliases={"BPO2": "Fusaka"},
                fork_opcode_records={"Fusaka": fusaka},
                rule_fields=("IsOsaka", "IsAmsterdam"),
            )

    def test_eip8024_audit_differentials_all_three_by_256_executions(self):
        lines = []
        for encoded in range(256):
            lines.append(f"EVM_EIP8024_SINGLE_VALID(0x{encoded:02x}, {encoded + 1})")
            lines.append(f"EVM_EIP8024_PAIR_VALID(0x{encoded:02x}, 1, {encoded + 2})")
        policy = parse_eip8024_immediate_policy("\n".join(lines))
        opcodes = parse_neverd_opcode_metadata(
            """EVM_OPCODE(DUPN, 0xe6, 0, 1, 1, EIP8024Single, Stack, Amsterdam,
                          None, None, None, None, false)
EVM_OPCODE(SWAPN, 0xe7, 0, 0, 1, EIP8024Single, Stack, Amsterdam,
           None, None, None, None, false)
EVM_OPCODE(EXCHANGE, 0xe8, 0, 0, 1, EIP8024Pair, Stack, Amsterdam,
           None, None, None, None, false)
"""
        )
        semantics = parse_semantics_policy(
            """EVM_GETH_RULE_FIELD(IsAmsterdam, MappedForkSelector, Amsterdam)
EVM_GETH_RULE_FIELD(IsBogota, MappedForkSelector, Bogota)
EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_FORK_RULE(Amsterdam, IsAmsterdam)
EVM_GETH_FORK_RULE(Bogota, IsBogota)
EVM_GETH_DYNAMIC_STACK_IMMEDIATE(DUPN, Single, Dup, 1)
EVM_GETH_DYNAMIC_STACK_IMMEDIATE(SWAPN, Single, Swap, 0)
EVM_GETH_DYNAMIC_STACK_IMMEDIATE(EXCHANGE, Pair, Exchange, 0)
"""
        )
        observations = []
        for opcode, family in (
            ("DUPN", "single"),
            ("SWAPN", "single"),
            ("EXCHANGE", "pair"),
        ):
            for encoded in range(256):
                operands = list(getattr(policy, family)[encoded])
                observations.append(
                    {
                        "opcode": opcode,
                        "encoded": encoded,
                        "accepted": True,
                        "operands": operands,
                        "pc_delta": 1,
                        "error_class": "none",
                        "stack_delta": 1 if opcode == "DUPN" else 0,
                        "marker_transition_verified": True,
                        "underflow_error_class": "stack_underflow",
                        "underflow_pc_delta": 0,
                        "underflow_stack_unchanged": True,
                    }
                )
        missing = [
            {
                "opcode": opcode,
                "matches_zero_immediate": True,
                "marker_transition_verified": True,
            }
            for opcode in ("DUPN", "SWAPN", "EXCHANGE")
        ]
        handlers = [
            {"opcode": opcode, "symbol": f"core/vm.op{opcode.title()}"}
            for opcode in ("DUPN", "SWAPN", "EXCHANGE")
        ]
        active_opcodes = [{"name": opcode} for opcode in ("DUPN", "SWAPN", "EXCHANGE")]
        fork_records = {
            "Frontier": [],
            "Amsterdam": active_opcodes,
            "Bogota": active_opcodes,
        }
        mainnet = {
            "active": {"opcodes": active_opcodes},
            "scheduled": {"opcodes": active_opcodes},
        }

        def table(target, active):
            return {
                "target": target,
                "active_opcodes": ["DUPN", "SWAPN", "EXCHANGE"] if active else [],
                "handlers": handlers if active else [],
                "observations": observations if active else [],
                "missing_operand": missing if active else [],
            }

        raw = {
            "tables": [
                table("Frontier", False),
                table("Amsterdam", True),
                table("Bogota", True),
                table("mainnet.active", True),
                table("mainnet.scheduled", True),
            ]
        }
        arguments = (
            policy,
            semantics,
            opcodes,
            ("Frontier", "Amsterdam", "Bogota"),
            fork_records,
            mainnet,
        )
        audit_geth_eip8024_immediates(*arguments, raw)

        raw["tables"][2]["observations"] = list(observations)
        raw["tables"][2]["observations"][0] = {
            **observations[0],
            "operands": [999],
        }
        with self.assertRaisesRegex(OpcodeAuditError, "Bogota.*DUPN.*0x00"):
            audit_geth_eip8024_immediates(*arguments, raw)

        raw["tables"][2]["observations"] = observations
        raw["tables"][2]["handlers"] = [dict(handler) for handler in handlers]
        raw["tables"][2]["handlers"][0]["symbol"] = "core/vm.replacedDup"
        with self.assertRaisesRegex(OpcodeAuditError, "post-activation handler drift"):
            audit_geth_eip8024_immediates(*arguments, raw)

        raw["tables"][2]["handlers"] = handlers
        partial_records = {**fork_records, "Amsterdam": active_opcodes[:1]}
        with self.assertRaisesRegex(OpcodeAuditError, "partially activates"):
            audit_geth_eip8024_immediates(
                policy,
                semantics,
                opcodes,
                ("Frontier", "Amsterdam", "Bogota"),
                partial_records,
                mainnet,
                raw,
            )

    def test_semantics_audit_compares_activation_base_minimum_and_net_delta(self):
        revision = "b" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes = parse_neverd_opcode_metadata(
            """
            EVM_OPCODE(STOP, 0x00, 0, 0, 0, None, Control, Frontier,
                       Halt, None, None, None, true)
            EVM_OPCODE(DUP1, 0x80, 0, 1, 0, None, Stack, Frontier,
                       None, None, None, None, false)
            EVM_OPCODE(DUPN, 0xe6, 0, 1, 1, EIP8024Single, Stack, Frontier,
                       None, None, None, None, false)
            """
        )
        policy = parse_semantics_policy(
            """
            EVM_GETH_FORK_RULE(Frontier, None)
            EVM_GETH_BASE_MIN_STACK(DUP1, 1)
            EVM_GETH_BASE_MIN_STACK(DUPN, 1)
            EVM_GETH_ACTIVE_WITHOUT_COST(STOP, Frontier)
            EVM_GETH_DYNAMIC_STACK_IMMEDIATE(DUPN, Single, Dup, 1)
            """
        )
        upstream = {
            "schema_version": opcode_audit.GETH_PROBE_SCHEMA_VERSION,
            "geth_revision": revision,
            "go_version": go_version,
            "stack_limit": stack_limit,
            "forks": [
                {
                    "name": "Frontier",
                    "rules": [],
                    "opcodes": [
                        {
                            "name": "STOP",
                            "byte": 0x00,
                            "base_min_stack": 0,
                            "net_stack_delta": 0,
                        },
                        {
                            "name": "DUP1",
                            "byte": 0x80,
                            "base_min_stack": 1,
                            "net_stack_delta": 1,
                        },
                        {
                            "name": "DUPN",
                            "byte": 0xE6,
                            "base_min_stack": 1,
                            "net_stack_delta": 1,
                        },
                    ],
                }
            ],
        }

        result = audit_geth_opcode_semantics(
            opcodes,
            ("Frontier",),
            policy,
            upstream,
            expected_revision=revision,
            expected_go_version=go_version,
            expected_stack_limit=stack_limit,
        )
        self.assertEqual(result.opcode_count, 3)
        self.assertEqual(result.base_min_stack_override_count, 2)
        self.assertEqual(result.dynamic_stack_immediate_count, 1)

    def test_dynamic_stack_policy_requires_an_explicit_base_precheck(self):
        opcodes = parse_neverd_opcode_metadata(
            """
            EVM_OPCODE(DUPN, 0xe6, 0, 1, 1, EIP8024Single, Stack, Frontier,
                       None, None, None, None, false)
            """
        )
        policy = parse_semantics_policy(
            """
            EVM_GETH_FORK_RULE(Frontier, None)
            EVM_GETH_DYNAMIC_STACK_IMMEDIATE(DUPN, Single, Dup, 1)
            """
        )
        upstream = {
            "schema_version": opcode_audit.GETH_PROBE_SCHEMA_VERSION,
            "geth_revision": "a" * 40,
            "go_version": "go1.24.6",
            "stack_limit": 1024,
            "forks": [
                {
                    "name": "Frontier",
                    "rules": [],
                    "opcodes": [
                        {
                            "name": "DUPN",
                            "byte": 0xE6,
                            "base_min_stack": 1,
                            "net_stack_delta": 1,
                        }
                    ],
                }
            ],
        }

        with self.assertRaisesRegex(OpcodeAuditError, "explicit base minimum"):
            audit_geth_opcode_semantics(
                opcodes,
                ("Frontier",),
                policy,
                upstream,
                expected_revision="a" * 40,
                expected_go_version="go1.24.6",
                expected_stack_limit=1024,
            )

    def test_semantics_manifest_rejects_unknown_root_fields(self):
        revision = "c" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        manifest = self._stop_manifest(revision, go_version, stack_limit)
        manifest["unexpected"] = True

        with self.assertRaisesRegex(OpcodeAuditError, "unexpected field"):
            audit_geth_opcode_semantics(
                opcodes,
                ("Frontier",),
                policy,
                manifest,
                expected_revision=revision,
                expected_go_version=go_version,
                expected_stack_limit=stack_limit,
            )

    def test_semantics_manifest_rejects_unknown_nested_fields(self):
        revision = "d" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        for level in ("fork", "opcode"):
            with self.subTest(level=level):
                manifest = self._stop_manifest(revision, go_version, stack_limit)
                target = manifest["forks"][0]
                if level == "opcode":
                    target = target["opcodes"][0]
                target["unexpected"] = True
                with self.assertRaisesRegex(OpcodeAuditError, "unexpected field"):
                    audit_geth_opcode_semantics(
                        opcodes,
                        ("Frontier",),
                        policy,
                        manifest,
                        expected_revision=revision,
                        expected_go_version=go_version,
                        expected_stack_limit=stack_limit,
                    )

    def test_semantics_manifest_rejects_duplicate_hardforks(self):
        revision = "e" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        manifest = self._stop_manifest(revision, go_version, stack_limit)
        duplicate = self._stop_manifest(revision, go_version, stack_limit)["forks"][0]
        manifest["forks"].append(duplicate)

        with self.assertRaisesRegex(OpcodeAuditError, "duplicate hardfork"):
            audit_geth_opcode_semantics(
                opcodes,
                ("Frontier",),
                policy,
                manifest,
                expected_revision=revision,
                expected_go_version=go_version,
                expected_stack_limit=stack_limit,
            )

    def test_semantics_manifest_rejects_duplicate_opcode_records(self):
        revision = "f" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        manifest = self._stop_manifest(revision, go_version, stack_limit)
        manifest["forks"][0]["opcodes"].append(dict(manifest["forks"][0]["opcodes"][0]))

        with self.assertRaisesRegex(OpcodeAuditError, "duplicate opcode record"):
            audit_geth_opcode_semantics(
                opcodes,
                ("Frontier",),
                policy,
                manifest,
                expected_revision=revision,
                expected_go_version=go_version,
                expected_stack_limit=stack_limit,
            )

    def test_semantics_manifest_rejects_duplicate_opcode_bytes_per_fork(self):
        revision = "1" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes = parse_neverd_opcode_metadata(
            """EVM_OPCODE(STOP, 0x00, 0, 0, 0, None, Control, Frontier,
                          Halt, None, None, None, true)
EVM_OPCODE(ADD, 0x01, 2, 1, 0, None, Arithmetic, Frontier,
           None, None, None, None, false)
"""
        )
        policy = parse_semantics_policy(
            """EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_ACTIVE_WITHOUT_COST(STOP, Frontier)
"""
        )
        manifest = self._stop_manifest(revision, go_version, stack_limit)
        manifest["forks"][0]["opcodes"].append(
            {
                "byte": 0,
                "name": "ADD",
                "base_min_stack": 2,
                "net_stack_delta": -1,
            }
        )

        with self.assertRaisesRegex(OpcodeAuditError, "duplicate opcode byte"):
            audit_geth_opcode_semantics(
                opcodes,
                ("Frontier",),
                policy,
                manifest,
                expected_revision=revision,
                expected_go_version=go_version,
                expected_stack_limit=stack_limit,
            )

    def test_semantics_manifest_rejects_boolean_schema_version(self):
        revision = "2" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        manifest = self._stop_manifest(revision, go_version, stack_limit)
        manifest["schema_version"] = True

        with self.assertRaisesRegex(
            OpcodeAuditError, "schema_version must be an integer"
        ):
            audit_geth_opcode_semantics(
                opcodes,
                ("Frontier",),
                policy,
                manifest,
                expected_revision=revision,
                expected_go_version=go_version,
                expected_stack_limit=stack_limit,
            )

    def test_semantics_manifest_rejects_boolean_opcode_numbers(self):
        revision = "3" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        for field in ("byte", "base_min_stack", "net_stack_delta"):
            with self.subTest(field=field):
                manifest = self._stop_manifest(revision, go_version, stack_limit)
                manifest["forks"][0]["opcodes"][0][field] = False
                with self.assertRaisesRegex(
                    OpcodeAuditError, rf"{field} must be an integer"
                ):
                    audit_geth_opcode_semantics(
                        opcodes,
                        ("Frontier",),
                        policy,
                        manifest,
                        expected_revision=revision,
                        expected_go_version=go_version,
                        expected_stack_limit=stack_limit,
                    )

    def test_semantics_manifest_requires_typed_identity_fields(self):
        revision = "4" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        for field, malformed, message in (
            ("geth_revision", [], "geth_revision must be a string"),
            ("go_version", [], "go_version must be a string"),
            ("stack_limit", False, "stack_limit must be an integer"),
        ):
            with self.subTest(field=field):
                manifest = self._stop_manifest(revision, go_version, stack_limit)
                manifest[field] = malformed
                with self.assertRaisesRegex(OpcodeAuditError, message):
                    audit_geth_opcode_semantics(
                        opcodes,
                        ("Frontier",),
                        policy,
                        manifest,
                        expected_revision=revision,
                        expected_go_version=go_version,
                        expected_stack_limit=stack_limit,
                    )

    def test_semantics_manifest_requires_typed_nested_fields(self):
        revision = "5" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        for field, malformed, message in (
            ("rules", "", "rules must be an array of strings"),
            ("name", 0, "name must be a string"),
        ):
            with self.subTest(field=field):
                manifest = self._stop_manifest(revision, go_version, stack_limit)
                target = manifest["forks"][0]
                if field == "name":
                    target = target["opcodes"][0]
                target[field] = malformed
                with self.assertRaisesRegex(OpcodeAuditError, message):
                    audit_geth_opcode_semantics(
                        opcodes,
                        ("Frontier",),
                        policy,
                        manifest,
                        expected_revision=revision,
                        expected_go_version=go_version,
                        expected_stack_limit=stack_limit,
                    )

    def test_semantics_manifest_rejects_identity_drift(self):
        revision = "6" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        for field, argument, expected, message in (
            (
                "geth_revision",
                "expected_revision",
                "7" * 40,
                "revision.*fetched SHA",
            ),
            (
                "go_version",
                "expected_go_version",
                "go1.25.0",
                "Go version.*toolchain",
            ),
            (
                "stack_limit",
                "expected_stack_limit",
                2048,
                "stack limit drift",
            ),
        ):
            with self.subTest(field=field):
                manifest = self._stop_manifest(revision, go_version, stack_limit)
                arguments = {
                    "expected_revision": revision,
                    "expected_go_version": go_version,
                    "expected_stack_limit": stack_limit,
                }
                arguments[argument] = expected
                with self.assertRaisesRegex(OpcodeAuditError, message):
                    audit_geth_opcode_semantics(
                        opcodes,
                        ("Frontier",),
                        policy,
                        manifest,
                        **arguments,
                    )

    def test_semantics_manifest_rejects_activation_drift(self):
        revision = "8" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes = parse_neverd_opcode_metadata(
            """EVM_OPCODE(STOP, 0x00, 0, 0, 0, None, Control, Frontier,
                          Halt, None, None, None, true)
EVM_OPCODE(ADD, 0x01, 2, 1, 0, None, Arithmetic, Homestead,
           None, None, None, None, false)
"""
        )
        policy = parse_semantics_policy(
            """EVM_GETH_RULE_FIELD(IsHomestead, MappedForkSelector, Homestead)
EVM_GETH_FORK_RULE(Frontier, None)
EVM_GETH_FORK_RULE(Homestead, IsHomestead)
EVM_GETH_ACTIVE_WITHOUT_COST(STOP, Frontier)
"""
        )
        manifest = self._stop_manifest(revision, go_version, stack_limit)
        homestead = dict(manifest["forks"][0])
        homestead["name"] = "Homestead"
        homestead["rules"] = ["IsHomestead"]
        homestead["opcodes"] = [dict(homestead["opcodes"][0])]
        manifest["forks"].append(homestead)

        with self.assertRaisesRegex(
            OpcodeAuditError, "Homestead activation drift: missing ADD"
        ):
            audit_geth_opcode_semantics(
                opcodes,
                ("Frontier", "Homestead"),
                policy,
                manifest,
                expected_revision=revision,
                expected_go_version=go_version,
                expected_stack_limit=stack_limit,
            )

    def test_semantics_manifest_rejects_stack_drift(self):
        revision = "9" * 40
        go_version = "go1.24.6"
        stack_limit = 1024
        opcodes, policy = self._stop_contract()
        for field, value in (("base_min_stack", 1), ("net_stack_delta", 1)):
            with self.subTest(field=field):
                manifest = self._stop_manifest(revision, go_version, stack_limit)
                manifest["forks"][0]["opcodes"][0][field] = value
                with self.assertRaisesRegex(OpcodeAuditError, field):
                    audit_geth_opcode_semantics(
                        opcodes,
                        ("Frontier",),
                        policy,
                        manifest,
                        expected_revision=revision,
                        expected_go_version=go_version,
                        expected_stack_limit=stack_limit,
                    )

    def test_alias_and_explicit_ignore_form_a_closed_inventory(self):
        neverd = parse_neverd_opcodes(
            """
            EVM_OPCODE(SHA3, 0x20, 2, 1, 0, None, Crypto, Frontier,
                       None, None, None, None, false)
            EVM_OPCODE(STOP, 0x00, 0, 0, 0, None, Control, Frontier,
                       Halt, None, None, None, true)
            """
        )
        upstream = parse_geth_opcodes(
            """
            const (
                STOP OpCode = 0x00
                KECCAK256 OpCode = 0x20
                EOFONLY OpCode = 0xe0
            )
            """
        )
        policy = parse_policy(
            """
            EVM_UPSTREAM_OPCODE_EXCLUSION_KIND(UnscheduledEOF, false, true,
                                               "not active")
            EVM_UPSTREAM_OPCODE_ALIAS(SHA3, KECCAK256)
            EVM_UPSTREAM_OPCODE_IGNORE(EOFONLY, 0xe0, UnscheduledEOF)
            """
        )
        result = audit_opcodes(neverd, upstream, policy)
        self.assertEqual(result.neverd_count, 2)
        self.assertEqual(result.ignored_count, 1)

        with self.assertRaisesRegex(OpcodeAuditError, "policy=0xe1.*0xe0"):
            audit_opcodes(
                neverd,
                upstream,
                parse_policy(
                    """
                    EVM_UPSTREAM_OPCODE_EXCLUSION_KIND(UnscheduledEOF,
                                                       false, true,
                                                       "not active")
                    EVM_UPSTREAM_OPCODE_ALIAS(SHA3, KECCAK256)
                    EVM_UPSTREAM_OPCODE_IGNORE(EOFONLY, 0xe1,
                                               UnscheduledEOF)
                    """
                ),
            )

        with self.assertRaisesRegex(OpcodeAuditError, "unknown reason"):
            parse_policy("EVM_UPSTREAM_OPCODE_IGNORE(EOFONLY, 0xe0, TypoReason)")

    def test_exclusion_kind_controls_byte_ownership(self):
        upstream = {"STOP": 0x00, "OLD_STOP": 0x00, "EOFONLY": 0xE0}
        historical = """
            EVM_UPSTREAM_OPCODE_EXCLUSION_KIND(HistoricalAlias, true, false,
                                               "shares a canonical byte")
            EVM_UPSTREAM_OPCODE_IGNORE(OLD_STOP, 0x00, HistoricalAlias)
        """
        result = audit_opcodes(
            {"STOP": 0x00},
            {"STOP": 0x00, "OLD_STOP": 0x00},
            parse_policy(historical),
        )
        self.assertEqual(result.ignored_count, 1)

        with self.assertRaisesRegex(OpcodeAuditError, "overlaps.*forbids"):
            audit_opcodes(
                {"STOP": 0x00},
                upstream,
                parse_policy(
                    """
                    EVM_UPSTREAM_OPCODE_EXCLUSION_KIND(Unscheduled, false,
                                                       true, "inactive")
                    EVM_UPSTREAM_OPCODE_IGNORE(OLD_STOP, 0x00, Unscheduled)
                    EVM_UPSTREAM_OPCODE_IGNORE(EOFONLY, 0xe0, Unscheduled)
                    """
                ),
            )

        with self.assertRaisesRegex(OpcodeAuditError, "does not require"):
            audit_opcodes(
                {"STOP": 0x00},
                upstream,
                parse_policy(
                    """
                    EVM_UPSTREAM_OPCODE_EXCLUSION_KIND(HistoricalAlias, true,
                                                       false, "alias")
                    EVM_UPSTREAM_OPCODE_IGNORE(OLD_STOP, 0x00,
                                               HistoricalAlias)
                    EVM_UPSTREAM_OPCODE_IGNORE(EOFONLY, 0xe0,
                                               HistoricalAlias)
                    """
                ),
            )

        for flags in (("false", "false"), ("true", "true")):
            with (
                self.subTest(flags=flags),
                self.assertRaisesRegex(OpcodeAuditError, "select exactly one"),
            ):
                parse_policy(
                    "EVM_UPSTREAM_OPCODE_EXCLUSION_KIND(Broken, "
                    f'{flags[0]}, {flags[1]}, "broken")'
                )

    def test_byte_drift_fails_with_both_values(self):
        with self.assertRaisesRegex(OpcodeAuditError, "NeverD=0x01.*0x02"):
            audit_opcodes(
                {"ADD": 0x01},
                {"ADD": 0x02},
                parse_policy(""),
            )

    def test_unreviewed_upstream_opcode_fails(self):
        with self.assertRaisesRegex(OpcodeAuditError, "unreviewed.*NEWOP"):
            audit_opcodes(
                {"STOP": 0x00},
                {"STOP": 0x00, "NEWOP": 0x01},
                parse_policy(""),
            )

    def test_unsupported_upstream_expression_fails_closed(self):
        with self.assertRaisesRegex(OpcodeAuditError, "unsupported.*expression"):
            parse_geth_opcodes(
                """
                const (
                    STOP OpCode = byte(0x00)
                )
                """
            )

        with self.assertRaisesRegex(
            OpcodeAuditError, "unparsed EVM_GETH_RULE_FIELD record"
        ):
            parse_semantics_policy(
                """EVM_GETH_RULE_FIELD(IsHomestead, MappedForkSelector,
                                        Homestead, Unexpected)
EVM_GETH_FORK_RULE(Frontier, None)
"""
            )

    def test_non_opcode_const_blocks_are_ignored(self):
        opcodes = parse_geth_opcodes(
            """
            const (
                WORD_BYTES = 32
            )
            const (
                STOP OpCode = 0x00
            )
            """
        )
        self.assertEqual(opcodes, {"STOP": 0x00})

    def test_duplicate_upstream_declaration_fails(self):
        with self.assertRaisesRegex(OpcodeAuditError, "duplicate.*STOP"):
            parse_geth_opcodes(
                """
                const (
                    STOP OpCode = 0x00
                    STOP OpCode = 0x00
                )
                """
            )

    def test_unparsed_standalone_opcode_declaration_fails_closed(self):
        with self.assertRaisesRegex(OpcodeAuditError, "unparsed.*NEWOP"):
            parse_geth_opcodes(
                """
                const (
                    STOP OpCode = 0x00
                )
                const NEWOP OpCode = 0x01
                """
            )


if __name__ == "__main__":
    unittest.main()
