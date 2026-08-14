#!/usr/bin/env python3
"""Build exception-runtime .pat signatures and file them by target.

What this produces is the input to one narrow question the loader cannot
otherwise answer: in a stripped, statically linked image, which address is the
personality routine?  Everywhere else that question is a name lookup, and a
stripped image has no name to look up.

Three things have to line up for an answer to be usable, and this script is
where they meet.

  * The signature has to describe a routine NeverD's personality table knows.
    Anything else is a name at an address nothing will act on, so the emitted
    patterns are filtered down to the list in eh_runtime_symbols.py.

  * The signature has to agree with the *whole* routine, not its opening run.
    A personality decides the schema a frame's language data is decoded with,
    so a prefix match -- two routines sharing a prologue -- is a wrong answer
    waiting to happen.  That is why the signature maker is driven with a tail
    long enough to reach the end of each function, and why every emitted line
    is checked against the same rule the loader applies before it will act on
    a name (SignatureMatcher::isFullyVerified).

  * The signature has to be filed where the loader will look for it, which is
    <format>/<arch>/<bitness>/, decided by the archive's own object headers
    rather than by whoever ran the script.

Local use, against whatever the host has installed:

    cmake --build build --target neverd-sigmaker
    python3 scripts/signatures/build_eh_signatures.py \\
        --sigmaker build/bin/neverd-sigmaker \\
        --from-toolchain gcc --from-toolchain clang \\
        --output signatures

Or against archives named explicitly, which is what CI does after building the
unwinder from source:

    python3 scripts/signatures/build_eh_signatures.py \\
        --sigmaker build/bin/neverd-sigmaker \\
        --archive build-libunwind/lib/libunwind.a \\
        --name llvm-libunwind --output signatures
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from eh_runtime_symbols import is_eh_runtime_symbol  # noqa: E402
from eh_signature_inputs import LibraryInput, find_all  # noqa: E402


# Mirrors kMinFixedBytesForPersonality in lib/sigs/SignatureDB.cpp.  A line
# that states fewer bytes than this will never be allowed to name a
# personality routine, however well it matches, so reporting it here is the
# difference between shipping a database and shipping a database that works.
MIN_FIXED_BYTES = 16

# Larger than any personality routine, so the signature maker's tail reaches
# the end of every function it describes.  It clamps to the function size, so
# an oversized request costs nothing.
FULL_COVERAGE_TAIL = 65535

# Members an archive keeps for the linker rather than for a compiler.
ARCHIVE_BOOKKEEPING = frozenset({"/", "//", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED"})


# --- target identification -------------------------------------------------


@dataclass(frozen=True, slots=True)
class Target:
    """The directory triple a signature set is filed under."""

    binary_format: str
    arch: str
    bitness: str

    def as_path(self) -> Path:
        return Path(self.binary_format) / self.arch / self.bitness

    def __str__(self) -> str:
        return f"{self.binary_format}/{self.arch}/{self.bitness}"


# ELF e_machine, Mach-O cputype, and COFF machine values for the four
# architectures the signature layout distinguishes.  Everything else is a
# target NeverD does not file signatures for, and is reported rather than
# guessed at.
ELF_MACHINES = {3: ("x86", "32"), 62: ("x86", "64"), 40: ("arm", "32"), 183: ("arm", "64")}
MACHO_CPUTYPES = {
    7: ("x86", "32"),
    0x01000007: ("x86", "64"),
    12: ("arm", "32"),
    0x0100000C: ("arm", "64"),
}
COFF_MACHINES = {
    0x014C: ("x86", "32"),
    0x8664: ("x86", "64"),
    0x01C0: ("arm", "32"),
    0x01C4: ("arm", "32"),
    0xAA64: ("arm", "64"),
}


def first_object_member(archive: Path) -> bytes | None:
    """The first real object in an ar archive, or the file itself.

    Only the head of the member is needed, so the whole archive is not read
    into memory: an unwinder archive is small, but a C++ standard library is
    not, and this runs once per input.

    A BSD archive -- which is what Darwin and rustc produce -- keeps a member
    name longer than sixteen characters in the member's own data, behind a
    `#1/<length>` header field.  Reading past it is what tells the symbol
    index from the first object rather than sniffing the index as one.
    """

    with archive.open("rb") as handle:
        magic = handle.read(8)
        if magic not in (b"!<arch>\n", b"!<thin>\n"):
            handle.seek(0)
            return handle.read(64)

        while True:
            header = handle.read(60)
            if len(header) < 60:
                return None
            name = header[:16].decode("ascii", "replace").strip()
            try:
                size = int(header[48:58].decode("ascii", "replace").strip())
            except ValueError:
                return None
            body_start = handle.tell()

            name_length = 0
            if name.startswith("#1/"):
                try:
                    name_length = int(name[3:])
                except ValueError:
                    name_length = 0
                extended = handle.read(name_length)
                name = extended.split(b"\0")[0].decode("ascii", "replace").strip()

            if name.rstrip("/") and name not in ARCHIVE_BOOKKEEPING:
                data = handle.read(min(size - name_length, 64))
                if len(data) >= 4:
                    return data
            handle.seek(body_start + size + (size % 2))


def identify_target(archive: Path) -> Target | None:
    head = first_object_member(archive)
    if head is None or len(head) < 20:
        return None

    if head[:4] == b"\x7fELF":
        little = head[5] == 1
        machine = struct.unpack_from("<H" if little else ">H", head, 18)[0]
        entry = ELF_MACHINES.get(machine)
        return Target("elf", *entry) if entry else None

    magic = struct.unpack_from("<I", head, 0)[0]
    if magic in (0xFEEDFACE, 0xFEEDFACF):
        cputype = struct.unpack_from("<i", head, 4)[0] & 0xFFFFFFFF
        entry = MACHO_CPUTYPES.get(cputype)
        return Target("macho", *entry) if entry else None
    if magic in (0xCEFAEDFE, 0xCFFAEDFE):
        cputype = struct.unpack_from(">i", head, 4)[0] & 0xFFFFFFFF
        entry = MACHO_CPUTYPES.get(cputype)
        return Target("macho", *entry) if entry else None

    entry = COFF_MACHINES.get(struct.unpack_from("<H", head, 0)[0])
    return Target("pe", *entry) if entry else None


# --- pattern lines ---------------------------------------------------------


@dataclass(frozen=True, slots=True)
class PatternLine:
    """One .pat line, read the way lib/sigs/PatternParser.cpp reads it."""

    text: str
    leading: str
    crc_len: int
    crc16: int
    total_len: int
    names: tuple[str, ...]
    tail: str

    @staticmethod
    def _pairs(pattern: str) -> tuple[str, ...]:
        if len(pattern) % 2 != 0:
            return ()
        return tuple(pattern[index : index + 2] for index in range(0, len(pattern), 2))

    @staticmethod
    def _is_hex_pattern(pattern: str, *, allow_empty: bool) -> bool:
        if not pattern:
            return allow_empty
        pairs = PatternLine._pairs(pattern)
        if len(pairs) * 2 != len(pattern):
            return False
        return all(
            pair == ".."
            or all(
                character in "0123456789abcdefABCDEF" for character in pair
            )
            for pair in pairs
        )

    @property
    def is_well_formed(self) -> bool:
        if not (0 < self.total_len <= 0xFFFFFFFF):
            return False
        if not (0 <= self.crc_len <= 0xFF) or not (0 <= self.crc16 <= 0xFFFF):
            return False
        if not self._is_hex_pattern(self.leading, allow_empty=False):
            return False
        if not self._is_hex_pattern(self.tail, allow_empty=True):
            return False
        leading = self._pairs(self.leading)
        if len(leading) >= self.total_len:
            return self.crc_len == 0
        return len(leading) + self.crc_len <= self.total_len

    @property
    def fixed_bytes(self) -> int:
        if self.total_len <= 0:
            return 0
        leading = self._pairs(self.leading)
        tail = self._pairs(self.tail)
        leading_limit = min(len(leading), self.total_len)
        tail_limit = max(0, self.total_len - len(leading) - self.crc_len)
        covered = leading[:leading_limit] + tail[:tail_limit]
        return sum(1 for pair in covered if pair != "..")

    @property
    def is_fully_verified(self) -> bool:
        if not self.is_well_formed:
            return False
        verified = (
            len(self._pairs(self.leading))
            + self.crc_len
            + len(self._pairs(self.tail))
        )
        return verified >= self.total_len

    @property
    def can_name_a_personality(self) -> bool:
        return self.is_fully_verified and self.fixed_bytes >= MIN_FIXED_BYTES


def looks_like_pattern_bytes(token: str) -> bool:
    """True for a token lib/sigs/PatternParser.cpp would read as tail bytes.

    Mirrored rather than approximated: the counts this script reports are a
    prediction of what the loader will do with the database, and a rule that
    is merely close would make that prediction quietly wrong.
    """

    return token.startswith("..") or (
        len(token) >= 2 and all(c in "0123456789abcdefABCDEF" for c in token[:2])
    )


def parse_pattern_line(line: str) -> PatternLine | None:
    stripped = line.strip()
    if not stripped or stripped.startswith((";", "#")) or stripped == "---":
        return None
    tokens = stripped.split()
    if len(tokens) < 4:
        return None
    try:
        crc_len = int(tokens[1], 16)
        crc16 = int(tokens[2], 16)
        total_len = int(tokens[3], 16)
    except ValueError:
        return None

    names: list[str] = []
    tail = ""
    index = 4
    while index < len(tokens):
        token = tokens[index]
        if token.startswith(":"):
            if index + 1 < len(tokens) and not tokens[index + 1].startswith((":", "..")):
                names.append(tokens[index + 1])
                index += 1
        elif looks_like_pattern_bytes(token):
            tail = token
        index += 1

    if not names:
        return None
    return PatternLine(
        stripped, tokens[0], crc_len, crc16, total_len, tuple(names), tail
    )


# --- generation ------------------------------------------------------------


@dataclass
class Summary:
    archives: int = 0
    emitted: int = 0
    kept: int = 0
    actionable: int = 0

    def report(self) -> str:
        return (
            f"{self.archives} archives, {self.emitted} signatures generated, "
            f"{self.kept} exception-runtime signatures kept, "
            f"{self.actionable} of them strong enough to name a personality"
        )


def run_sigmaker(sigmaker: Path, archive: Path, leading: int, tail: int) -> list[str]:
    with tempfile.TemporaryDirectory(prefix="neverd-eh-sigs-") as scratch:
        output = Path(scratch) / "generated.pat"
        command = [
            str(sigmaker),
            str(archive),
            "-o",
            str(output),
            "--leading",
            str(leading),
            "--tail",
            str(tail),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            print(
                f"warning: {archive.name}: signature maker failed: "
                f"{result.stderr.strip()}",
                file=sys.stderr,
            )
            return []
        if not output.is_file():
            return []
        return output.read_text(encoding="utf-8", errors="replace").splitlines()


def collect(
    sigmaker: Path, inputs: list[LibraryInput], leading: int, tail: int, summary: Summary
) -> dict[Target, dict[str, PatternLine]]:
    """Generate, filter, and group by target.

    Keyed by the line text so that two archives carrying the same routine --
    which `libgcc_eh.a` and `libgcc.a` routinely do -- contribute it once.
    """

    by_target: dict[Target, dict[str, PatternLine]] = {}
    for entry in inputs:
        target = identify_target(entry.path)
        if target is None:
            print(
                f"warning: {entry.path}: unrecognized object target, skipped",
                file=sys.stderr,
            )
            continue
        summary.archives += 1

        for raw in run_sigmaker(sigmaker, entry.path, leading, tail):
            parsed = parse_pattern_line(raw)
            if parsed is None:
                continue
            summary.emitted += 1
            if not any(is_eh_runtime_symbol(name) for name in parsed.names):
                continue
            bucket = by_target.setdefault(target, {})
            if parsed.text not in bucket:
                bucket[parsed.text] = parsed
                summary.kept += 1
                if parsed.can_name_a_personality:
                    summary.actionable += 1
    return by_target


def write(
    by_target: dict[Target, dict[str, PatternLine]], output_root: Path, name: str
) -> list[Path]:
    written: list[Path] = []
    for target, lines in sorted(by_target.items(), key=lambda item: str(item[0])):
        directory = output_root / target.as_path()
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / f"{name}.pat"
        # Sorted so that a rebuild of unchanged inputs produces an identical
        # file: CI commits these, and a reordered database would be an empty
        # commit that still has to be reviewed.
        body = "\n".join(sorted(lines)) + "\n"
        path.write_text(body, encoding="utf-8")
        written.append(path)
    return written


def resolve_sigmaker(explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    found = shutil.which("neverd-sigmaker")
    if found:
        return Path(found)
    guess = Path.cwd() / "build" / "bin" / "neverd-sigmaker"
    return guess if guess.is_file() else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sigmaker", help="path to neverd-sigmaker")
    parser.add_argument(
        "--archive",
        action="append",
        default=[],
        help="static library or rlib to read; repeatable",
    )
    parser.add_argument(
        "--from-toolchain",
        action="append",
        default=[],
        help="compiler driver whose runtime libraries to locate; repeatable",
    )
    parser.add_argument(
        "--no-rust",
        action="store_true",
        help="skip the rustc sysroot when scanning toolchains",
    )
    parser.add_argument(
        "--output",
        default="signatures",
        help="root of the <format>/<arch>/<bitness> tree to write into",
    )
    parser.add_argument(
        "--name",
        default="eh-runtime",
        help="stem of the .pat file written in each target directory",
    )
    parser.add_argument(
        "--leading",
        type=int,
        default=32,
        help="leading pattern bytes to state exactly",
    )
    parser.add_argument(
        "--tail",
        type=int,
        default=FULL_COVERAGE_TAIL,
        help="trailing pattern bytes; the default covers every function to its end",
    )
    arguments = parser.parse_args()

    sigmaker = resolve_sigmaker(arguments.sigmaker)
    if sigmaker is None:
        print(
            "error: neverd-sigmaker not found; build it with "
            "`cmake --build <dir> --target neverd-sigmaker` and pass --sigmaker",
            file=sys.stderr,
        )
        return 1

    inputs = [LibraryInput(Path(a).resolve(), "--archive") for a in arguments.archive]
    if arguments.from_toolchain:
        inputs += find_all(arguments.from_toolchain, include_rust=not arguments.no_rust)
    missing = [entry for entry in inputs if not entry.path.is_file()]
    for entry in missing:
        print(f"error: {entry.path}: no such file", file=sys.stderr)
    if missing:
        return 1
    if not inputs:
        print(
            "error: nothing to read; pass --archive or --from-toolchain",
            file=sys.stderr,
        )
        return 1

    summary = Summary()
    by_target = collect(sigmaker, inputs, arguments.leading, arguments.tail, summary)
    if not by_target:
        print("error: no exception-runtime signatures were produced", file=sys.stderr)
        return 1

    for path in write(by_target, Path(arguments.output), arguments.name):
        print(f"wrote {path}")
    print(summary.report())

    if summary.actionable == 0:
        print(
            "error: no signature covers a whole function, so none of them may "
            "name a personality routine; raise --tail or check that the "
            "archives carry debug-free release objects",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
