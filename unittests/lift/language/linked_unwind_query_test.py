#!/usr/bin/env python3
"""Apple-linked unwind metadata through real public consumers; no execution."""

from __future__ import annotations

import argparse
import ctypes
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import traceback


def texts(path, macro):
    source = path.read_text()
    pattern = (
        rf"^{macro}\(\s*(\w+),\s*"
        r'(?:R"(?P<tag>\w*)\((?P<raw>.*?)\)(?P=tag)"|'
        r'(?P<quoted>(?:"(?:[^"\\]|\\.)*"\s*)+))\s*\)'
    )
    rows = list(re.finditer(pattern, source, re.MULTILINE | re.DOTALL))
    result = {
        row.group(1): row.group("raw")
        if row.group("raw") is not None else "".join(
            json.loads(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', row.group("quoted"))
        )
        for row in rows
    }
    assert len(result) == len(rows) == len(re.findall(rf"^{macro}\(", source, re.MULTILINE))
    return result


def numbers(path, macro, typed=False):
    source = path.read_text()
    type_field = r"\w+\s*,\s*" if typed else ""
    rows = re.findall(
        rf"^{macro}\(\s*{type_field}(\w+),\s*(-?(?:0x[0-9a-fA-F]+|[0-9]+))\s*\)",
        source, re.MULTILINE,
    )
    result = {name: int(value, 0) for name, value in rows}
    assert len(result) == len(rows) == len(re.findall(rf"^{macro}\(", source, re.MULTILINE))
    return result


FIXTURES = Path(__file__).with_name("LinkedUnwindQueryFixtures.def")
WORDS = texts(FIXTURES, "NEVERD_LINKED_QUERY_TEXT")
LIMITS = numbers(FIXTURES, "NEVERD_LINKED_QUERY_LIMIT")


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


class Commands:
    def __init__(self, directory, timeout):
        self.directory = directory
        self.timeout = timeout
        self.index = 0

    def run(self, arguments, baseline=False):
        self.index += 1
        paths = {
            name: self.directory / WORDS[name].format(index=self.index)
            for name in ("kCommandLog", "kStdoutLog", "kStderrLog")
        }
        record = {
            WORDS["kCommandArguments"]: arguments,
            WORDS["kExitCode"]: None,
            WORDS["kTimedOut"]: False,
        }
        save_json(paths["kCommandLog"], record)
        with paths["kStdoutLog"].open("wb") as stdout, paths["kStderrLog"].open("wb") as stderr:
            try:
                result = subprocess.run(arguments, stdout=stdout, stderr=stderr, timeout=self.timeout)
                record[WORDS["kExitCode"]] = result.returncode
                success = result.returncode == 0
            except subprocess.TimeoutExpired:
                record[WORDS["kTimedOut"]] = True
                success = False
            finally:
                save_json(paths["kCommandLog"], record)
        if not success and not baseline:
            raise RuntimeError(WORDS["kCommandFailed"].format(path=paths["kCommandLog"]))
        return success, paths["kStdoutLog"].read_text(errors="replace")


class AddressBridge:
    """The test owns this real session until all consumers finish querying."""

    def session_address(self, handle):
        return handle


def address_fields(value):
    """Normalize only the four address fields in this public frame schema."""
    if value is None:
        return None
    result = {}
    for key, item in value.items():
        if key in {WORDS[name] for name in ("kBegin", "kEnd", "kQueryPC", "kSigningPC")}:
            assert isinstance(item, str) and item.lower().startswith(WORDS["kAddressPrefix"]), (key, item)
            result[key] = hex(int(item, 16))
        elif isinstance(item, dict):
            result[key] = address_fields(item)
        else:
            result[key] = item
    return result


def expected_state(entry, pc, signing_offset, state_offset, compact):
    signed = {
        WORDS["kStatus"]: WORDS["kKnown"],
        WORDS["kKind"]: WORDS["kSPAndPC"],
        WORDS["kKey"]: WORDS["kKeyB"],
        WORDS["kRecipe"]: WORDS["kNonHint"] if compact else WORDS["kHint"],
        WORDS["kSigningPC"]: hex(entry + signing_offset),
    }
    dwarf = None if compact else signed if pc >= entry + state_offset else {
        WORDS["kStatus"]: WORDS["kKnown"], WORDS["kKind"]: WORDS["kUnsigned"],
    }
    return {
        WORDS["kQueryPC"]: hex(pc),
        WORDS["kDwarf"]: dwarf,
        WORDS["kCompact"]: signed if compact else {WORDS["kStatus"]: WORDS["kUnknown"]},
    }


def check_expected(frame, expected, entry, end):
    assert frame is not None
    assert int(frame[WORDS["kBegin"]], 16) == entry, frame
    assert int(frame[WORDS["kEnd"]], 16) == end, frame
    actual = dict(frame[WORDS["kReturnAddress"]])
    compact = actual[WORDS["kCompact"]]
    if compact is not None and compact[WORDS["kStatus"]] == WORDS["kUnknown"]:
        compact = dict(compact)
        assert compact.pop(WORDS["kDiagnostic"]), frame
        actual[WORDS["kCompact"]] = compact
    assert actual == expected, (actual, expected)


def check_image(args, directory, image, entry, code_bytes, signing_offset, state_offset, compact, commands):
    from neverd_plugin import Session
    from neverd_plugin.abi import UNWIND_FRAME_QUERY
    from neverd_plugin.ffi import HostAPI
    from transport_test import Client

    host = HostAPI(ctypes.CDLL(str(Path(args.library).resolve(strict=True))))
    handle = int(host.call(WORDS["kCreate"]) or 0)
    assert handle
    pointer = ctypes.c_void_p(handle)
    records = []
    worker = None
    original = image.read_bytes()
    try:
        host.call(WORDS["kDebug"], pointer, 0)
        session = Session(handle, _native=AddressBridge(), _host=host)
        session.load(image)
        with Client(args.worker) as worker:
            assert WORDS["kOperation"] in worker.hello[WORDS["kCapabilities"]], worker.hello
            opened = worker.call(WORDS["kOpen"], {
                WORDS["kPath"]: str(image), WORDS["kReadOnly"]: True, WORDS["kAnalysis"]: False,
            })
            assert opened[WORDS["kStatus"]] == WORDS["kOK"], opened
            before = worker.call(WORDS["kMetadata"])
            end = entry + code_bytes
            offsets = sorted({0, signing_offset - 1, signing_offset, signing_offset + 1,
                              state_offset - 1, state_offset, state_offset + 1,
                              code_bytes - 1, code_bytes})
            for offset in offsets:
                pc = entry + offset
                raw = host.owned_string(UNWIND_FRAME_QUERY, pointer, pc)
                assert raw is not None
                native = json.loads(raw)
                python = session.unwind_frame(pc)
                _, output = commands.run([
                    args.neverd, WORDS["kCLIInfo"], str(image), WORDS["kCLINoDebug"],
                    WORDS["kCLIUnwind"] + hex(pc), WORDS["kCLIJSON"],
                ])
                cli = json.loads(output)
                reply = worker.call(WORDS["kOperation"], {WORDS["kAddress"]: hex(pc)})
                expected = None if pc == end else expected_state(
                    entry, pc, signing_offset, state_offset, compact,
                )
                records.append({
                    WORDS["kAddress"]: hex(pc), WORDS["kCAPIResult"]: native,
                    WORDS["kPythonResult"]: python, WORDS["kCLIResult"]: cli,
                    WORDS["kWorkerResult"]: reply, WORDS["kExpectedResult"]: expected,
                })
                save_json(directory / WORDS["kQueriesLog"], records)
                assert python == native == cli, records[-1]
                assert reply[WORDS["kStatus"]] == WORDS["kOK"], reply
                canonical = address_fields(native)
                assert reply[WORDS["kPayload"]] == canonical, records[-1]
                for name in ("kRevision", "kProject", "kAnalysisState"):
                    assert reply[WORDS[name]] == before[WORDS[name]], reply
                if expected is None:
                    assert canonical is None, records[-1]
                else:
                    check_expected(canonical, expected, entry, end)
            after = worker.call(WORDS["kMetadata"])
            assert after[WORDS["kPayload"]] == before[WORDS["kPayload"]], (before, after)
            assert image.read_bytes() == original
    finally:
        host.call(WORDS["kDestroy"], pointer)
        if worker is not None:
            (directory / WORDS["kWorkerLog"]).write_bytes(worker.logs)
        save_json(directory / WORDS["kQueriesLog"], records)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option, destination in (("kSourceRootArgument", "kSourceRoot"), ("kLibraryArgument", "kLibrary"),
                                ("kCLIArgument", "kCLI"), ("kWorkerArgument", "kWorker")):
        parser.add_argument(WORDS[option], dest=WORDS[destination], required=True)
    parser.add_argument(WORDS["kArtifactsArgument"], dest=WORDS["kArtifacts"])
    args = parser.parse_args()
    source_root = Path(args.source_root).resolve(strict=True)
    if args.artifacts:
        Path(args.artifacts).mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=WORDS["kTemporaryPrefix"], dir=args.artifacts)).resolve()
    try:
        linked_path = source_root / WORDS["kLinkedFixtures"]
        linked = texts(linked_path, "NEVERD_A64_LINKED_TEXT")
        values = numbers(linked_path, "NEVERD_A64_LINKED_VALUE", typed=True)
        commands = Commands(root, values["kToolTimeoutSeconds"])
        xcrun = shutil.which(linked["kXcrun"])
        if sys.platform != WORDS["kApplePlatform"] or not xcrun:
            (root / WORDS["kSkipLog"]).write_text(linked["kHostUnavailable"])
            print(WORDS["kBaselineUnavailable"].format(path=root))
            return LIMITS["kSkipReturnCode"]
        probe = root / linked["kProbeSourceFile"]
        probe.write_text(linked["kProbeSource"])
        success, _ = commands.run([
            xcrun, linked["kClang"], linked["kTarget"], linked["kTriple"],
            linked["kDynamicLibrary"], str(probe), linked["kOutput"], str(root / linked["kProbeImageFile"]),
        ], baseline=True)
        if not success:
            (root / WORDS["kSkipLog"]).write_text(linked["kToolchainUnavailable"])
            print(WORDS["kBaselineUnavailable"].format(path=root))
            return LIMITS["kSkipReturnCode"]
        sys.path.insert(0, str(source_root / WORDS["kPythonSDK"]))
        sys.path.insert(0, str(source_root / WORDS["kWorkerTests"]))
        cases = [
            (WORDS["kExplicitCase"], linked["kCompactSource"], values["kCompactCodeBytes"], values["kSigningOffset"], True),
            (WORDS["kLegacyCase"], linked["kDwarfPrefix"] + linked["kLegacySigning"] + linked["kDwarfSuffix"],
             values["kDwarfCodeBytes"], values["kSigningOffset"], False),
            (WORDS["kAbsoluteCase"], linked["kDwarfPrefix"] + linked["kAbsoluteSigning"] + linked["kDwarfSuffix"],
             values["kDwarfCodeBytes"], values["kAbsoluteStateOffset"], False),
        ]
        summary = {}
        for name, source, size, state_offset, compact in cases:
            directory = root / name
            directory.mkdir()
            path = directory / linked["kSourceFile"]
            path.write_text(source)
            object_path = directory / linked["kObjectFile"]
            image = directory / linked["kImageFile"]
            commands.run([xcrun, linked["kClang"], linked["kTarget"], linked["kTriple"],
                          linked["kCompile"], str(path), linked["kOutput"], str(object_path)])
            commands.run([xcrun, linked["kClang"], linked["kTarget"], linked["kTriple"],
                          linked["kDynamicLibrary"], str(object_path), linked["kOutput"], str(image)])
            _, symbols = commands.run([xcrun, WORDS["kNM"], *WORDS["kNMArguments"].split(), str(image)])
            entries = [int(fields[0], 16) for line in symbols.splitlines()
                       if len(fields := line.split()) == 3 and fields[-1] == linked["kSymbol"]]
            assert len(entries) == 1, symbols
            summary[name] = check_image(args, directory, image, entries[0], size,
                                        values["kSigningOffset"], state_offset, compact, commands)
        save_json(root / WORDS["kSummaryLog"], summary)
        print(WORDS["kCompleted"].format(path=root))
        print(WORDS["kNoExecution"])
        return 0
    except BaseException:
        (root / WORDS["kFailureLog"]).write_text(traceback.format_exc())
        print(WORDS["kFailed"].format(path=root), file=sys.stderr)
        raise


if __name__ == "__main__":
    raise SystemExit(main())
