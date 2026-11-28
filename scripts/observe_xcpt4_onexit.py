"""Observe the first UCRT exit registration in the pinned, unmodified PE32 corpus."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


FIXTURE_SHA256 = "57edc0c2dff4ea80b4a2f0a85acbd31be23238d03219d3448fec5470915a4ada"
UCRT_SHA256 = "f4d768583fd3fc020d30fd3a2d9d2f39dd2a5a49c99cc5e2040dfb61424a4778"
FIXTURE_NAME = "xcpt4-msvc-x86-native-no-gs-o0.exe"


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def marker(name, expressions):
    fields = " ".join(f"{field}=%u" for field in expressions)
    arguments = ", ".join(f"({value})" for value in expressions.values())
    return f'.printf "XCPT4_{name} {fields}\\n", {arguments}'


def observation():
    # PE entry is RVA 0x6e36. Resolve the actual provider from its eager
    # import and cross-check two separate exports before using the table.
    # Only boolean predicates on these fixed application/runtime images
    # leave CDB. The workflow separately runs the file outside the debugger.
    entry = {
        "pc": "@eip == @$exentry-0x272",
        "app_type_export": "poi(@$exentry+0x128e) == @$t0+0x85a00",
        "commode_export": "poi(@$exentry+0x12ca) == @$t0+0x879d0",
        "callback": "poi(@esp) == @$exentry+0x6eb",
        "uses_default": "poi(@$exentry+0x43ca) == 0xffffffff",
        "mode0_zero": "poi(@$t0+0x1043c4) == 0",
        "mode1_zero": "poi(@$t0+0x1043c8) == 0",
        "handler0_null": "poi(@$t0+0x1043bc) == poi(@$t0+0x103300)",
        "handler1_null": "poi(@$t0+0x1043c0) == poi(@$t0+0x103300)",
    }
    table = {
        "pc": "@eip == @$t0+0x616e0",
        "callback": "poi(@esp+8) == @$exentry+0x6eb",
        "ucrt_table": "@$t1 == @$t0+0x10475c",
        "program_table": "@$t1 == @$exentry+0x43ca",
        "first_null": "poi(@$t1) == poi(@$t0+0x103300)",
        "last_null": "poi(@$t1+4) == poi(@$t0+0x103300)",
        "end_null": "poi(@$t1+8) == poi(@$t0+0x103300)",
        "full": "poi(@$t1+4) == poi(@$t1+8)",
    }
    comparison = {
        "pc": "@eip == @$t0+0x617d2",
        "first_null": "@esi == 0",
        "equal": "@edi == @ebx",
        "take_growth_branch": "(@efl & 0x40) != 0",
    }
    returned = {
        "pc": "@eip == @$exentry-0x26d",
        "success": "@eax == 0",
        "first_null": "poi(@$t1) == poi(@$t0+0x103300)",
        "nonempty": "poi(@$t1) != poi(@$t1+4)",
        "room": "poi(@$t1+4) != poi(@$t1+8)",
    }
    def stop_at(address):
        # CDB also stops on first-chance exceptions. A sample at that stop
        # carries no evidence about the requested instruction.
        return ["bp /1 " + address, "g", ".lastevent",
                '.if (@eip != (' + address + ')) { .echo XCPT4_UNEXPECTED_STOP; qd }']

    steps = [
        ".expr /s masm", *stop_at("@$exentry"), ".echo XCPT4_PROCESS_ENTRY",
        *stop_at("@$exentry-0x272"),
        "r $t0 = poi(@$exentry+0x127a)-0x61640", marker("ENTRY", entry),
        *stop_at("@$t0+0x616e0"), "r $t1 = poi(@esp+4)",
        '.if ((@$t1 != (@$t0+0x10475c)) and (@$t1 != (@$exentry+0x43ca))) { .echo XCPT4_UNKNOWN_TABLE; qd }',
        marker("TABLE", table),
        *stop_at("@$t0+0x617d2"), marker("COMPARE", comparison),
        *stop_at("@$exentry-0x26d"), marker("RETURN", returned),
        "qd",
    ]
    return steps, {"ENTRY": entry, "TABLE": table, "COMPARE": comparison, "RETURN": returned}


def parse_markers(output, groups):
    results = {}
    for name, fields in groups.items():
        pattern = " ".join(f"{key}=([01])" for key in fields)
        matches = re.findall(rf"\bXCPT4_{name} {pattern}[ \t]*$", output, re.MULTILINE)
        if len(matches) != 1:
            errors = [line for line in output.splitlines()
                      if any(word in line.lower() for word in ("error", "syntax", "couldn't", "unable", "breakpoint", "operand", "unknown", "xcpt4_"))]
            raise ValueError(f"Expected one {name} observation, got {len(matches)}. {errors[-16:]}")
        results[name] = dict(zip(fields, (bool(int(n)) for n in matches[0])))
    return results


def main():
    original = json.loads(Path("xcpt4.json").read_text(encoding="utf-8-sig"))
    trace = json.loads(Path("xcpt4-modules.json").read_text(encoding="utf-8-sig"))
    fixture = Path(trace["modules"][FIXTURE_NAME]["file"]).resolve()
    provider = Path(trace["modules"]["ucrtbase.dll"]["file"])
    if (fixture.name != FIXTURE_NAME or digest(fixture) != FIXTURE_SHA256
            or original["sha256"] != FIXTURE_SHA256 or trace["image_sha256"] != FIXTURE_SHA256
            or digest(provider) != UCRT_SHA256 or trace["modules"]["ucrtbase.dll"]["sha256"] != UCRT_SHA256):
        raise ValueError("The application or UCRT differs from the images used to identify the call")
    debugger = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10/Debuggers/x86/cdb.exe"
    steps, groups = observation()
    with tempfile.TemporaryDirectory() as folder:
        commands = Path(folder) / "observe.txt"
        commands.write_text("\n".join(steps) + "\n", encoding="ascii")
        command = [str(debugger), "-hd", "-y", folder, "-cf", str(commands), str(fixture)]
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   errors="replace", timeout=120, check=True)
    if re.search(r"(?:^|> )XCPT4_(?:UNEXPECTED_STOP|UNKNOWN_TABLE)[ \t]*$", completed.stdout, re.MULTILINE):
        events = [line for line in completed.stdout.splitlines()
                  if re.search(r"last event:|xcpt4.*\+0x[0-9a-f]+:$|\be\s+[0-9a-f]{8}\b|XCPT4_PROCESS_ENTRY$|XCPT4_UNEXPECTED_STOP$", line, re.I)]
        raise ValueError(f"CDB stopped outside the fixed startup observations: {events[-20:]}")
    results = parse_markers(completed.stdout, groups)
    before, table, branch, after = (results[n] for n in ("ENTRY", "TABLE", "COMPARE", "RETURN"))
    if not (all(before[key] for key in ("pc", "app_type_export", "commode_export", "callback"))
            and table["pc"] and table["callback"] and table["ucrt_table"] != table["program_table"]
            and table["ucrt_table"] == before["uses_default"]
            and branch["pc"] and after["pc"] and branch["first_null"] == table["first_null"]
            and branch["take_growth_branch"] == branch["equal"] == table["full"]):
        events = [line for line in completed.stdout.splitlines() if "Last event:" in line]
        raise ValueError(f"The register and table observations disagree: {results}; {events}")
    if after["success"] and not after["nonempty"]:
        raise ValueError("The registrar returned success with an empty table")
    if digest(fixture) != FIXTURE_SHA256 or digest(provider) != UCRT_SHA256:
        raise ValueError("An observed image changed")
    record = dict(fixture=FIXTURE_SHA256, provider=UCRT_SHA256, debugger=digest(debugger),
                  ordinary_run_cases=original["cases"], observations=results)
    Path("xcpt4-onexit.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results))


if __name__ == "__main__":
    main()
