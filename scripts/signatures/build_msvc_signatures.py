#!/usr/bin/env python3
"""Build the pe/ and elf/ signature files from collected static libraries.

The inputs are the release assets the signatures repository's
`msvc-libraries.yml` workflow publishes: one zstd-compressed tar per
toolset, SDK or library and architecture, each with a JSON manifest naming
what it holds.  Every asset becomes lines in one file:

    toolset assets  ->  pe/<x86|arm>/<32|64>/vs<year>.pat
    winsdk assets   ->  pe/<x86|arm>/<32|64>/winsdk.pat
    library assets  ->  <pe|elf>/<x86|arm>/<32|64>/<library>.pat

A library asset holds static libraries that belong to no Visual Studio
release, such as the MASM32 SDK's or a Linux distribution's, and names its
file in its manifest; its `format` (`pe`, the default, or `elf`) says which
tree the file belongs to.

so a year's servicing toolsets (VS 2026's 14.50 and its current default, say)
land in the same file, and so do all Windows SDK versions.

Four rules decide what a file holds:

  * Lines come from `neverd-sigmaker` with `--machine` set to the asset's
    architecture, so an object for another target that shares a library
    directory can never be filed under the wrong one, and with a tail long
    enough to state every byte of every function.  A match is then agreement
    over the whole routine, not over its prologue.

  * Names are the linkage names the libraries' symbol tables spell.  Nothing
    here renames anything.

  * A file is rebuilt from its assets alone.  Lines it held before, from an
    earlier run or from another tool, are replaced, so give the script every
    asset of a directory at once.

  * When lines state the same bytes under different names, all of them are
    dropped.  The pattern cannot tell those routines apart -- MSVC folds
    identical template instantiations, for one -- and any single name chosen
    among them would be a guess presented as an identification.  The loader
    reads every file of a directory together, so this holds across the
    directory: bytes that one file's libraries give two names are dropped
    from every file of it, and so are bytes two files name differently.
    NeverD's matcher compares a line only as far as the line's own length
    and accepts any byte where the line has a wildcard, so a line is dropped
    as well when a routine of another name, at least as long, states every
    byte the line states: in a linked image it would name that routine too.
    The counts are reported.

  * Lines state the routines they branch to as `^offset name` references,
    which the matcher checks in the image (`neverd-sigmaker --references`).
    Several references at one offset name the routines one branch may reach,
    of which any confirms it: the alternate name a COFF link resolves an
    undefined symbol to, or the routine another build of the same bytes
    calls there.  Lines of one file with the same bytes and different names
    are kept when references tell every two of them apart, because at a
    match only one of them can be confirmed.  Across files the rule above
    stands: the Rich header loads one release's file, where such a line would
    have no rival.

Every file written is read back through `neverd-sigmaker --verify`, the
loader's own parser: the loader rejects a whole directory for one bad line.

Usage:

    python3 scripts/signatures/build_msvc_signatures.py \\
        --sigmaker build/bin/neverd-sigmaker \\
        --assets release-assets/ \\
        --output signatures
"""

from __future__ import annotations

from collections import defaultdict

import argparse
import bisect
import fnmatch
import json
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# Larger than any function, so the signature maker's tail reaches the end of
# every function it describes.  It clamps to the function size.
FULL_COVERAGE_TAIL = 65535

# The collector's architecture names, the directory the loader searches for
# that architecture, and the COFF machine neverd-sigmaker keeps.
ARCHITECTURES = {
    "x86": (Path("x86/32"), "x86"),
    "x64": (Path("x86/64"), "x64"),
    "arm": (Path("arm/32"), "arm"),
    "arm64": (Path("arm/64"), "arm64"),
}

# The object formats an asset's libraries hold, each a tree of its own.
FORMATS = ("pe", "elf", "macho")

# MSVC's archives and objects, and the GNU ar archives and objects MinGW
# builds: both hold COFF objects, which --machine sorts by architecture.
LIBRARY_SUFFIXES = (".lib", ".obj", ".a", ".o")

# What a library asset may name its file: a plain stem, and not one the
# toolset and SDK files already use.
LIBRARY_NAME = re.compile(r"[a-z0-9][a-z0-9._+-]*")
RESERVED_LIBRARY = re.compile(r"vs[0-9]{4}|winsdk")

# GitHub recommends no file above 50 MB and refuses one above 100 MB.  A file
# larger than this is written as parts, which the loader reads as one library:
# <name>.pat, then <name>.part2.pat, <name>.part3.pat, ...
MAX_FILE_BYTES = 50_000_000

# Native constraints use a separate extension so old directory readers skip
# them while retaining all ordinary patterns.
PATTERN_SUFFIXES = (".pat", ".npat")


def pattern_files(directory: Path) -> list[Path]:
    return sorted(path for path in directory.iterdir()
                  if path.is_file() and path.suffix in PATTERN_SUFFIXES)

# Lines whose first bytes are relocated are not searched for routines that
# state them; see settle_directory.
MIN_PREFIX_BYTES = 4

# neverd-sigmaker's leading pattern, and the size of a candidate range worth
# indexing rather than scanning.
LEAD_BYTES = 32
SMALL_RANGE = 64


class BuildError(RuntimeError):
    """An input is missing, malformed, or produced something unusable."""


# --- assets ------------------------------------------------------------------


@dataclass(frozen=True)
class Asset:
    name: str
    manifest: dict
    archive: Path

    @property
    def arch(self) -> str:
        return self.manifest["arch"]

    @property
    def format(self) -> str:
        fmt = str(self.manifest.get("format", "pe"))
        if fmt not in FORMATS:
            raise BuildError(f"{self.name}: unknown format {fmt!r}")
        if fmt != "pe" and self.manifest["kind"] != "library":
            raise BuildError(f"{self.name}: a {self.manifest['kind']} asset holds PE libraries")
        return fmt

    @property
    def directory(self) -> Path:
        return Path(self.format) / ARCHITECTURES[self.arch][0]

    @property
    def machine(self) -> str:
        return ARCHITECTURES[self.arch][1]

    @property
    def library(self) -> str:
        kind = self.manifest["kind"]
        if kind == "toolset":
            return f"vs{int(self.manifest['visual_studio']['year'])}"
        if kind == "winsdk":
            return "winsdk"
        if kind == "library":
            # A static library that belongs to no Visual Studio release, such
            # as the MASM32 SDK's, named by its manifest.
            name = str(self.manifest.get("library", ""))
            if not LIBRARY_NAME.fullmatch(name) or RESERVED_LIBRARY.fullmatch(name):
                raise BuildError(f"{self.name}: {name!r} cannot name a library file")
            return name
        raise BuildError(f"{self.name}: unknown asset kind {kind!r}")

    @property
    def output(self) -> Path:
        return self.directory / f"{self.library}.pat"

    def provenance(self) -> dict:
        entry = {
            "asset": self.name,
            "archive_sha256": self.manifest["archive"]["sha256"],
            "kind": self.manifest["kind"],
            "arch": self.arch,
        }
        # A Homebrew asset also names its formula and bottles: the registry
        # serves each bottle by the SHA-256 recorded here, so the file traces
        # back to them without the release.
        for key in ("toolset_version", "compiler_version", "windows_sdk_version",
                    "library_version", "formula", "bottles"):
            if self.manifest.get(key):
                entry[key] = self.manifest[key]
        return entry


def load_assets(directory: Path, patterns: list[str]) -> list[Asset]:
    assets = []
    for manifest_path in sorted(directory.glob("*.json")):
        if manifest_path.name == "assets.json":
            continue
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        name = manifest.get("asset")
        if name != manifest_path.stem:
            raise BuildError(f"{manifest_path.name}: manifest names asset {name!r}")
        if patterns and not any(fnmatch.fnmatchcase(name, p) for p in patterns):
            continue
        if manifest.get("arch") not in ARCHITECTURES:
            raise BuildError(f"{name}: unsupported architecture {manifest.get('arch')!r}")
        archive = directory / manifest["archive"]["name"]
        if not archive.is_file():
            raise BuildError(f"{name}: archive {archive.name} is missing")
        assets.append(Asset(name, manifest, archive))
    if not assets:
        raise BuildError(f"no assets in {directory} match {patterns or ['*']}")
    return assets


def extract(asset: Asset, destination: Path) -> list[Path]:
    """Unpack an asset and return its libraries in a stable order."""

    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["tar", "--zstd", "-xf", str(asset.archive), "-C", str(destination)],
        check=True,
    )
    recorded = {entry["path"] for entry in asset.manifest["files"]}
    found = {
        path.relative_to(destination).as_posix()
        for path in destination.rglob("*")
        if path.is_file()
    }
    if recorded != found:
        missing = sorted(recorded - found)[:5]
        extra = sorted(found - recorded)[:5]
        raise BuildError(f"{asset.name}: archive disagrees with manifest: "
                         f"missing {missing}, unexpected {extra}")
    return sorted(
        (destination / path for path in found if path.lower().endswith(LIBRARY_SUFFIXES)),
        key=lambda path: path.as_posix().lower(),
    )


# --- pattern lines -------------------------------------------------------------


@dataclass(frozen=True)
class PatternLine:
    """One .pat line split into what it asserts and what it names.

    `refs` are its `^offset name` references: the routines it branches to,
    which the matcher checks against the image.  They are not part of the
    key, because they state nothing about the line's own bytes.
    """

    text: str
    key: str
    names: tuple[str, ...]
    refs: tuple[tuple[int, str], ...] = ()
    require_references: bool = False
    require_caller: bool = False
    data_sites: frozenset[int] = frozenset()
    is_vtable: bool = False
    load_config_fields: tuple[tuple[int, str, str], ...] = ()
    local_sites: frozenset[int] = frozenset()
    is_code_layout: bool = False

    def reference_kind(self, offset: int) -> int:
        return 2 if offset in self.local_sites else int(offset in self.data_sites)

    @property
    def is_load_config(self) -> bool:
        return bool(self.load_config_fields)

    def with_refs(self, refs: tuple[tuple[int, str], ...],
                  require: bool | None = None,
                  data_sites: frozenset[int] | None = None,
                  local_sites: frozenset[int] | None = None) -> "PatternLine":
        """This line stating only `refs`."""

        if self.is_vtable or self.is_load_config or self.is_code_layout:
            if refs != self.refs or require not in (None, False) or data_sites or local_sites:
                raise BuildError("metadata fields cannot be refined as function references")
            return self
        tokens = self.key.split()
        head, tail = tokens[:4], tokens[4:]
        if data_sites is None:
            data_sites = self.data_sites.intersection(offset for offset, _ in refs)
        if local_sites is None:
            local_sites = self.local_sites.intersection(offset for offset, _ in refs)
        if not local_sites <= data_sites:
            raise BuildError("local references must retain their data kind")
        stated = reference_fields(refs, data_sites, local_sites)
        required = self.require_references if require is None else require
        constraint = ["!refs"] if required else ["!caller"] if self.require_caller else []
        text = " ".join([*head, *constraint, *self.names, *stated, *tail])
        return PatternLine(text, self.key, self.names, refs, required, self.require_caller,
                           data_sites, local_sites=local_sites)


def reference_fields(refs: tuple[tuple[int, str], ...], data_sites: frozenset[int],
                     local_sites: frozenset[int] = frozenset()) -> list[str]:
    """Keep function and external-data sites distinct across text round trips."""
    return [f"{'%' if offset in local_sites else '&' if offset in data_sites else '^'}{offset:04X} {name}"
            for offset, name in refs]


def alias_order(name: str) -> tuple:
    """Where a `:offset name` token sorts among one routine's names.

    NeverD shows, of the names a routine has at one offset, the one with the
    fewest leading underscores, then the shorter, then the smaller
    (preferredAliasOrder in include/neverd/sigs/Signature.h); lines list them
    in that order.
    """

    offset, _, bare = name.partition(" ")
    return (offset, len(bare) - len(bare.lstrip("_")), len(bare), bare)


def shared_names(name_sets: list[tuple[str, ...]]) -> set[str]:
    """The names every one of these claims gives its routine."""

    shared = set(name_sets[0])
    for names in name_sets[1:]:
        shared &= set(names)
    return shared


def aliases_of(lines: list[PatternLine]) -> PatternLine:
    """One line for claims that give the same bytes names they share.

    ELF libraries give one routine several symbols -- glibc's `puts` is also
    `_IO_puts` -- and builds of a library do not all define the same ones.
    Byte-identical claims that share a name are one routine, which has every
    name any of them gives it.
    """

    if any(line.is_vtable or line.is_load_config or line.is_code_layout for line in lines):
        raise BuildError("table declarations cannot be folded into function aliases")
    names = tuple(sorted({name for line in lines for name in line.names}, key=alias_order))
    merged = common_refs(lines)
    tokens = merged.key.split()
    stated = reference_fields(merged.refs, merged.data_sites, merged.local_sites)
    constraint = ["!refs"] if merged.require_references else ["!caller"] if merged.require_caller else []
    text = " ".join([*tokens[:4], *constraint, *names, *stated, *tokens[4:]])
    return PatternLine(text, merged.key, names, merged.refs, merged.require_references,
                       merged.require_caller, merged.data_sites, local_sites=merged.local_sites)


def ref_table(refs: tuple[tuple[int, str], ...]) -> dict[int, set[str]]:
    """The routines each branch of a line may reach: the names its references
    at that offset state, of which a COFF link reaches one."""

    table: dict[int, set[str]] = {}
    for offset, name in refs:
        table.setdefault(offset, set()).add(name)
    return table


def distinguished(lines: list[PatternLine]) -> bool:
    """Whether references tell apart every two of these same-byte lines.

    Two lines are told apart when one offset holds a reference in both and
    no routine the branch there may reach is one both name: at a match, only
    one of them can be confirmed.  Anything less leaves the bytes naming
    several routines.
    """

    tables = [ref_table(line.refs) for line in lines]
    for first in range(len(tables)):
        for second in range(first + 1, len(tables)):
            a, b = tables[first], tables[second]
            if not any(offset in b and
                       lines[first].reference_kind(offset) == lines[second].reference_kind(offset) and
                       not names & b[offset] for offset, names in a.items()):
                return False
    return True


def common_refs(lines: list[PatternLine]) -> PatternLine:
    """One line for copies that make the same claim under the same names.

    Builds of one library can call different routines from byte-identical
    code -- the debug CRT's `_free_dbg` where the release one calls `free`.
    A branch every copy states reaches one of the routines they name there,
    so the line names all of them; a branch some copy states no reference
    for is left unstated, or it would contradict the build that lacks it.
    """

    first = lines[0]
    required = any(line.require_references for line in lines)
    if all(line.refs == first.refs and line.data_sites == first.data_sites
           and line.local_sites == first.local_sites
           and line.require_references == required
           for line in lines):
        return first
    offsets = set.intersection(*(set(ref_table(line.refs)) for line in lines))
    offsets = {offset for offset in offsets
               if len({line.reference_kind(offset) for line in lines}) == 1}
    refs = {(offset, name) for line in lines for offset, name in line.refs if offset in offsets}
    return first.with_refs(tuple(sorted(refs)), required, first.data_sites & offsets,
                           first.local_sites & offsets)


@dataclass(frozen=True)
class Shape:
    """A line's byte assertions, in the parts NeverD's matcher reads."""

    lead: str
    crc_len: int
    crc: str
    total: int
    tail: str

    @classmethod
    def of(cls, key: str) -> "Shape":
        tokens = key.split()
        tail = tokens[4] if len(tokens) > 4 else ""
        return cls(tokens[0], int(tokens[1], 16), tokens[2], int(tokens[3], 16), tail)

    def stated_prefix(self) -> str:
        """The leading bytes up to the first one a relocation rewrites."""

        for index in range(0, len(self.lead), 2):
            if self.lead[index] == ".":
                return self.lead[:index]
        return self.lead

    def fully_verified(self) -> bool:
        """Whether the matcher verifies a complete nonempty function body."""
        if not self.total:
            return False
        leading = len(self.lead) // 2
        if leading >= self.total:
            return self.crc_len == 0
        end = leading + self.crc_len
        return end <= self.total and end + len(self.tail) // 2 >= self.total

    def covered_by(self, other: "Shape") -> bool:
        """Whether a routine at least as long states every byte this line states.

        NeverD's matcher compares a line only as far as its own length and
        accepts anything where the line has a wildcard, so such a routine
        matches this line wherever it is linked.  Only what the other line
        states can be compared. A wildcard also covers an opaque CRC byte;
        fixed bytes need explicit matching bytes, and a required CRC needs
        the same span and checksum. Different encodings of the leading/tail
        split do not change the offsets at which those assertions apply.
        """

        if other.total < self.total:
            return False
        if self.crc_len and (len(self.lead) != len(other.lead) or
                             self.crc_len != other.crc_len or self.crc != other.crc):
            return False
        if len(self.lead) == len(other.lead) and self.crc_len == other.crc_len:
            return (_states(self.lead[:2 * self.total], other.lead) and
                    _states(self.tail[:max(0, 2 * self.total - len(self.lead) - 2 * self.crc_len)],
                            other.tail))

        def states(pattern: str, start: int) -> bool:
            for index in range(0, min(len(pattern), 2 * self.total - start), 2):
                byte = pattern[index:index + 2]
                if byte == "..":
                    continue
                offset = start + index
                if offset < len(other.lead):
                    actual = other.lead[offset:offset + 2]
                else:
                    offset -= len(other.lead) + 2 * other.crc_len
                    actual = other.tail[offset:offset + 2] if offset >= 0 else ""
                if actual != byte:
                    return False
            return True

        return states(self.lead, 0) and states(self.tail, len(self.lead) + 2 * self.crc_len)

    def padding_collision_with(self, other: "Shape", *, machine: str = "x86") -> bool:
        """A shorter complete body can acquire this line's return padding.

        COFF section lengths can include x86 INT3/NOP alignment after RET,
        or halfword zero/NOP padding after a Thumb return.
        Such bytes cannot distinguish an otherwise identical shorter routine
        followed by that alignment in a linked image. This conservative
        collision check only handles these explicit machine/return forms;
        it refuses to use the common byte suffix as nominal identity.
        CRC assertions must agree at the same offsets; the return and added
        padding must remain literal bytes outside those opaque spans.
        """
        if other.total >= self.total or \
                not self.fully_verified() or not other.fully_verified():
            return False
        prefix = Shape(self.lead, self.crc_len, self.crc, other.total, self.tail)
        if not prefix.fully_verified() or not prefix.covered_by(other):
            return False
        # CRC bytes retain their own proof above. Mark them opaque here so
        # they cannot masquerade as the visible return or alignment suffix.
        own = (self.lead + ".." * self.crc_len + self.tail)[:2 * self.total].upper()
        short = (other.lead + ".." * other.crc_len + other.tail)[:2 * other.total].upper()
        if machine in {"x86", "x64"}:
            return short.endswith("C3") and all(
                own[index:index + 2] in {"90", "CC"}
                for index in range(len(short), len(own), 2))
        if machine == "arm" and other.total % 2 == 0 and self.total % 2 == 0:
            returns = short.endswith("7047") or re.search(r"BDE8[0-9A-F]{2}[89A-F][0-9A-F]$", short)
            return bool(returns) and all(
                own[index:index + 4] in {"0000", "00BF"}
                for index in range(len(short), len(own), 4))
        return False


def _states(pattern: str, other: str) -> bool:
    """Whether `other` states every byte `pattern` states, the same way."""

    if len(other) < len(pattern.rstrip(".")):
        return False
    for index in range(0, len(pattern), 2):
        if pattern[index] == ".":
            continue
        if other[index : index + 2] != pattern[index : index + 2]:
            return False
    return True


def parse_line(text: str, *, evidence: bool = False) -> PatternLine | None:
    """Split a line into its byte assertions and its names.

    The key is everything but the names, so two lines with equal keys make
    the same claim about a routine's bytes.
    """

    stripped = text.strip()
    if not stripped or stripped.startswith((";", "#")) or stripped == "---":
        return None
    tokens = stripped.split()
    raw_evidence = len(tokens) > 4 and tokens[4] == "!evidence"
    if raw_evidence and not evidence:
        raise BuildError("collision evidence is not a candidate pattern")
    if len(tokens) < 6:
        raise BuildError(f"malformed pattern line: {stripped[:120]}")
    names: list[str] = []
    refs: list[tuple[int, str]] = []
    kinds: dict[int, int] = {}
    rest = tokens[:4]
    index = 4
    required = tokens[index] == "!refs"
    caller = tokens[index] == "!caller"
    vtable = tokens[index] == "!vtable"
    load_config = tokens[index] == "!loadcfg"
    code_layout = tokens[index] == "!layout"
    config_pairs = defaultdict(dict)
    if required or caller or vtable or load_config or code_layout or raw_evidence:
        index += 1
    while index < len(tokens):
        token = tokens[index]
        if token.startswith(":") and index + 1 < len(tokens):
            names.append(f"{token} {tokens[index + 1]}")
            index += 2
            continue
        if token.startswith(("^", "&", "%")) and index + 1 < len(tokens):
            offset, kind = int(token[1:], 16), {"^": 0, "&": 1, "%": 2}[token[0]]
            if kind == 2 and (not (required or caller or evidence) or not re.fullmatch(
                    r"[0-9A-F]{8}(?:[+-](?!00000000:)[0-9A-F]{8})?:[^\s\x00-\x20\x7f]+",
                    tokens[index + 1])):
                raise BuildError("invalid source-local data anchor")
            if offset in kinds and kinds[offset] != kind and not load_config:
                raise BuildError("reference site mixes function and data kinds")
            if load_config:
                if kind in config_pairs[offset]:
                    raise BuildError("duplicate load-configuration field kind")
                config_pairs[offset][kind] = tokens[index + 1]
            kinds[offset] = kind
            refs.append((offset, tokens[index + 1]))
            index += 2
            continue
        rest.append(token)
        index += 1
    if not names:
        raise BuildError(f"pattern line names nothing: {stripped[:120]}")
    if caller:
        shape = Shape.of(" ".join(rest))
        if any(int(n.split()[0][1:], 16) != 0 for n in names) or \
                not shape.fully_verified():
            raise BuildError("!caller requires a complete body with public names at its start")
    if vtable:
        # A table is metadata, not an all-wildcard function. Every pointer
        # field must be represented, with alternatives only at the same slot.
        shape = Shape.of(" ".join(rest))
        if len(names) != 1 or not re.fullmatch(r":0+ \?\?_7.+@@6B@", names[0]) or \
                not shape.total or shape.total % 8 or shape.crc_len or int(shape.crc, 16) or \
                len(shape.lead + shape.tail) != 2 * shape.total or len(rest) > 5 or set(shape.lead + shape.tail) != {"."} or \
                any(kinds.values()) or set(kinds) != set(range(0, shape.total, 8)):
            raise BuildError("invalid complete primary x64 table declaration")
    if load_config:
        shape = Shape.of(" ".join(rest))
        if names != [":0000 _load_config_used"] or not config_pairs or \
                shape.crc_len or int(shape.crc, 16) or len(rest) > 5 or \
                len(shape.lead + shape.tail) != 2 * shape.total or \
                set(shape.lead + shape.tail) != {"."} or \
                any(offset < 0 or offset % 8 or offset + 8 > shape.total or
                    set(pair) != {False, True} for offset, pair in config_pairs.items()):
            raise BuildError("invalid load-configuration pointer/default pairs")
    if code_layout:
        shape = Shape.of(" ".join(rest))
        if not shape.fully_verified() or shape.crc_len or int(shape.crc, 16) or \
                len(shape.lead + shape.tail) != 2 * shape.total or \
                len(set(int(n.split()[0][1:], 16) for n in names)) < 2 or \
                len({n.split()[1] for n in names}) != len(names) or \
                any(kind == 2 for kind in kinds.values()):
            raise BuildError("invalid complete source code layout")
        # Canonical operand and budget validation belongs to the native reader.
        # Dedicated layout publication requires neverd-sigmaker --verify.
    normalized = " ".join([*tokens[:4], *tokens[5:]]) if raw_evidence else stripped
    return PatternLine(normalized, " ".join(rest), tuple(names), tuple(sorted(refs)), required, caller,
                       frozenset(offset for offset, is_data in kinds.items() if is_data), vtable,
                       tuple(sorted((offset, pair[True], pair[False])
                                    for offset, pair in config_pairs.items())),
                       frozenset(offset for offset, kind in kinds.items() if kind == 2), code_layout)


@dataclass
class FoldResult:
    """One file's lines once identical lines fold and ambiguous claims drop."""

    lines: list[PatternLine]
    ambiguous_keys: set[str] = field(default_factory=set)
    # Every distinct claim the libraries made, the dropped ones included.
    evidence: list[PatternLine] = field(default_factory=list)
    duplicates: int = 0
    # Groups of same-byte claims merged because their names are one routine's.
    merged_aliases: int = 0
    conflicting_lines: int = 0
    conflicting_groups: int = 0
    # Lines kept although others state the same bytes: references tell them
    # apart.
    distinguished_lines: int = 0
    conflict_examples: list[list[str]] = field(default_factory=list)
    dropped_across_files: int = 0
    dropped_covered: int = 0

    @property
    def texts(self) -> list[str]:
        return [line.text for line in self.lines]


def fold(lines: list[str], *, evidence: bool = False) -> FoldResult:
    """Fold identical lines and drop every ambiguous byte claim.

    Lines with the same key whose sets of names share a name are one
    routine's, under the aliases its builds define: they become one line with
    all of those names (see `aliases_of`).  A group of lines with the same
    key but sets of names with nothing in common is ambiguous: the bytes
    cannot say which routine they are.  All of its lines are dropped, and the
    key is kept so that `settle_directory` can drop the same bytes from the
    other files of the directory -- unless their references tell every two of
    them apart (see `distinguished`), when the matcher can: they are all
    kept.
    """

    copies: dict[str, dict[tuple[str, ...], list[PatternLine]]] = {}
    callers: dict[str, PatternLine] = {}
    seen = 0
    for raw in lines:
        parsed = parse_line(raw, evidence=evidence)
        if parsed is None:
            continue
        if parsed.is_vtable or parsed.is_load_config or parsed.is_code_layout:
            raise BuildError("metadata declarations require their dedicated selector")
        if parsed.require_caller:
            callers[parsed.text] = parsed
            continue
        seen += 1
        copies.setdefault(parsed.key, {}).setdefault(parsed.names, []).append(parsed)

    result = FoldResult(lines=[])
    for key, by_names in copies.items():
        variants = {names: common_refs(lines) for names, lines in by_names.items()}
        result.evidence.extend(variants.values())
        if any(line.require_references and not line.refs for line in variants.values()):
            result.ambiguous_keys.add(key)
            result.conflicting_groups += 1
            result.conflicting_lines += len(variants)
            continue
        if len(variants) > 1 and shared_names(list(variants)):
            result.lines.append(
                aliases_of([line for lines in by_names.values() for line in lines]))
            result.merged_aliases += 1
            continue
        if len(variants) > 1 and distinguished(list(variants.values())):
            result.lines.extend(variants.values())
            result.distinguished_lines += len(variants)
            continue
        if len(variants) > 1:
            result.ambiguous_keys.add(key)
            result.conflicting_groups += 1
            result.conflicting_lines += len(variants)
            if len(result.conflict_examples) < 20:
                result.conflict_examples.append(sorted(" ".join(n) for n in variants))
            continue
        (line,) = variants.values()
        result.lines.append(line)
    result.lines.extend(callers.values())
    result.evidence.extend(callers.values())
    result.lines.sort(key=lambda line: line.text)
    result.duplicates = seen - sum(len(v) for v in copies.values())
    return result


def another_routine(names: tuple[str, ...], other: tuple[str, ...]) -> bool:
    """Whether two claims name different routines: no name in common."""

    return not set(names) & set(other)


def settle_directory(results: dict[Path, FoldResult]) -> None:
    """Settle byte claims; named caller queries do not assert byte uniqueness."""

    callers = {path: [line for line in result.lines if line.require_caller]
               for path, result in results.items()}
    evidence = {path: result.evidence for path, result in results.items()}
    for result in results.values():
        result.lines = [line for line in result.lines if not line.require_caller]
        result.evidence = [line for line in result.evidence if not line.require_caller]
    _settle_byte_claims(results)
    for path, result in results.items():
        result.lines.extend(callers[path])
        result.lines.sort(key=lambda line: line.text)
        result.evidence = evidence[path]


def _settle_byte_claims(results: dict[Path, FoldResult]) -> None:
    """Drop lines whose bytes another file of the same directory contradicts.

    The loader applies every file of a directory together.  A line one file
    keeps is therefore only an identification if no library behind any file
    of the directory gives the same bytes another name: when another file
    states them under other names, or dropped them as ambiguous, the line
    is dropped too.  Lines that repeat a claim with the same names, such as
    a CRT routine that several toolsets share unchanged, are kept.
    """

    # The names each file gives each key.  A key stays only if no file found
    # it ambiguous and the files that state it agree: each names one routine
    # and a name is common to all of them (the routine's aliases differ from
    # library to library), or they keep the same lines that references tell
    # apart -- with the Rich header choosing one release's file, references
    # tell same-byte routines apart only among the lines of that one file.
    ambiguous: set[str] = set()
    for result in results.values():
        ambiguous |= result.ambiguous_keys
    claims: dict[str, list[frozenset[tuple[str, ...]]]] = {}
    for result in results.values():
        named: dict[str, set[tuple[str, ...]]] = {}
        for line in result.lines:
            named.setdefault(line.key, set()).add(line.names)
        for key, names in named.items():
            claims.setdefault(key, []).append(frozenset(names))

    def agreed(key: str) -> bool:
        if key in ambiguous:
            return False
        entries = claims[key]
        if all(len(entry) == 1 for entry in entries):
            return bool(shared_names([next(iter(entry)) for entry in entries]))
        return all(entry == entries[0] for entry in entries)

    settled = {key: agreed(key) for key in claims}
    for result in results.values():
        kept = [line for line in result.lines if settled[line.key]]
        result.dropped_across_files = len(result.lines) - len(kept)
        result.lines = kept

    # A routine that states every byte of a line begins with the bytes the
    # line states before its first wildcard, so sorting every claim by its
    # leading bytes puts the candidates in one range.
    claimed: list[tuple[str, Shape, tuple[str, ...]]] = []
    for result in results.values():
        for line in result.evidence:
            shape = Shape.of(line.key)
            claimed.append((shape.lead, shape, line.names))
    claimed.sort(key=lambda item: item[0])
    leads = [item[0] for item in claimed]
    # Within one range, which claims state which byte at which position of
    # the leading pattern: a covering routine is in every set a line's own
    # stated bytes pick out.
    stating: dict[tuple[int, int], list[dict[str, set[int]]]] = {}

    def index(start: int, end: int) -> list[dict[str, set[int]]]:
        if (start, end) not in stating:
            positions: list[dict[str, set[int]]] = [{} for _ in range(LEAD_BYTES)]
            for k in range(start, end):
                lead = claimed[k][0]
                for offset in range(0, min(len(lead), 2 * LEAD_BYTES), 2):
                    value = lead[offset : offset + 2]
                    if value != "..":
                        positions[offset // 2].setdefault(value, set()).add(k)
            stating[(start, end)] = positions
        return stating[(start, end)]

    # A routine that covers a line longer than the leading pattern has the
    # same CRC span with the same CRC.
    by_crc: dict[tuple[int, str], list[int]] = {}
    for k, (_, other, _) in enumerate(claimed):
        if other.crc_len:
            by_crc.setdefault((other.crc_len, other.crc), []).append(k)

    for result in results.values():
        kept = []
        for line in result.lines:
            shape = Shape.of(line.key)
            prefix = shape.stated_prefix()
            covered = False
            if shape.crc_len and len(shape.lead) < 2 * shape.total:
                for k in by_crc.get((shape.crc_len, shape.crc), ()):
                    _, other, names = claimed[k]
                    if another_routine(names, line.names) and other != shape \
                            and shape.covered_by(other):
                        covered = True
                        break
            # A line whose first bytes are relocated has no range to search;
            # it is kept.
            elif len(prefix) >= 2 * MIN_PREFIX_BYTES:
                start = bisect.bisect_left(leads, prefix)
                end = bisect.bisect_right(leads, prefix + "~")
                candidates: set[int] | None = None
                if end - start > SMALL_RANGE:
                    positions = index(start, end)
                    for offset in range(len(prefix), min(len(shape.lead), 2 * shape.total), 2):
                        value = shape.lead[offset : offset + 2]
                        if value == "..":
                            continue
                        found = positions[offset // 2].get(value, set())
                        candidates = found if candidates is None else candidates & found
                        if not candidates:
                            break
                for k in sorted(candidates) if candidates is not None else range(start, end):
                    _, other, names = claimed[k]
                    if another_routine(names, line.names) and other != shape \
                            and shape.covered_by(other):
                        covered = True
                        break
            if not covered:
                kept.append(line)
        result.dropped_covered = len(result.lines) - len(kept)
        result.lines = kept


# --- generation -------------------------------------------------------------------


def run_sigmaker(
    sigmaker: Path, libraries: list[Path], machine: str, tail: int, output: Path
) -> str:
    if not libraries:
        raise BuildError("no libraries to read")
    command = [
        str(sigmaker),
        *map(str, libraries),
        "-o",
        str(output),
        "--machine",
        machine,
        "--tail",
        str(tail),
        "--references",
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)
    if result.returncode != 0:
        raise BuildError(f"neverd-sigmaker failed ({result.returncode})")
    return result.stdout.strip()


def verify(sigmaker: Path, path: Path) -> None:
    result = subprocess.run(
        [str(sigmaker), "--verify", str(path)], capture_output=True, text=True
    )
    if result.returncode != 0:
        raise BuildError(
            f"{path} does not load: {(result.stderr or result.stdout).strip()}"
        )


def generate(
    args: argparse.Namespace, members: list[Asset], work_root: Path
) -> tuple[list[str], int, list[dict]]:
    """Every distinct line neverd-sigmaker writes for one file's assets, how
    many copies of those lines it wrote besides, and the assets' sources.

    The builds of a library an asset collects -- hundreds of glibc package
    revisions -- write most lines over and over, so each is kept once as it
    is read.
    """

    generated: dict[str, None] = {}
    repeated = 0
    sources = []
    for asset in members:
        libraries = extract(asset, work_root / asset.name)
        pat = work_root / f"{asset.name}.pat"
        summary = run_sigmaker(args.sigmaker, libraries, asset.machine, args.tail, pat)
        print(f"{asset.name}: {len(libraries)} libraries; {summary}", flush=True)
        written = 0
        with pat.open(encoding="utf-8") as lines:
            for line in lines:
                line = line.rstrip("\n")
                if not line.strip():
                    continue
                written += 1
                if line in generated:
                    repeated += 1
                else:
                    generated[line] = None
        if not written:
            raise BuildError(f"{asset.name} produced no signatures")
        entry = asset.provenance()
        # The summary names the scratch file it wrote; the record keeps only
        # the file's name, so that it reads the same on every machine and run.
        entry["sigmaker"] = summary.replace(str(pat), pat.name)
        sources.append(entry)
        shutil.rmtree(work_root / asset.name)
        pat.unlink()
    return list(generated), repeated, sources


def split_parts(texts: list[str], limit: int) -> list[list[str]]:
    """The lines in as few parts of at most `limit` bytes as hold them, of
    even size, in order."""

    total = sum(len(text) + 1 for text in texts)
    count = max(1, -(-total // limit))
    while True:
        target = -(-total // count)
        parts: list[list[str]] = [[]]
        size = 0
        for text in texts:
            if parts[-1] and size + len(text) + 1 > target and len(parts) < count:
                parts.append([])
                size = 0
            parts[-1].append(text)
            size += len(text) + 1
        if all(sum(len(text) + 1 for text in part) <= limit for part in parts):
            return parts
        count += 1


def part_path(destination: Path, index: int) -> Path:
    """Where part `index` (from 1) of a file goes."""

    if index == 1:
        return destination
    return destination.with_name(f"{destination.stem}.part{index}{destination.suffix}")


def write(
    args: argparse.Namespace, relative: Path, result: FoldResult, sources: list[dict]
) -> None:
    if not result.lines:
        raise BuildError(f"{relative}: nothing left to write")
    destination = args.output / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    parts = split_parts(result.texts, args.max_file_bytes)
    written = []
    for index, texts in enumerate(parts, start=1):
        path = part_path(destination, index)
        path.write_text("\n".join(texts) + "\n", encoding="utf-8")
        verify(args.sigmaker, path)
        written.append({"file": path.relative_to(args.output).as_posix(), "lines": len(texts)})
    # The parts an earlier, larger build left would still be loaded.
    for stale in destination.parent.glob(f"{destination.stem}.part*{destination.suffix}"):
        if stale.relative_to(args.output).as_posix() not in {w["file"] for w in written}:
            stale.unlink()

    provenance = {
        "file": relative.as_posix(),
        "generator": {
            "neverd_ref": args.neverd_ref,
            "release": args.release,
            "tail": args.tail,
        },
        "sources": sorted(
            sources, key=lambda entry: (entry["asset"], entry["archive_sha256"])
        ),
        "lines": len(result.lines),
    }
    if len(written) > 1:
        provenance["parts"] = written
    destination.with_suffix(".sources.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"{relative}: {len(result.lines)} lines"
        + (f" in {len(written)} parts" if len(written) > 1 else "")
        + f" ({result.duplicates} duplicates folded, "
        f"{result.conflicting_lines} lines in {result.conflicting_groups} "
        f"ambiguous groups dropped, "
        f"{result.dropped_across_files} dropped for bytes another file of "
        f"{relative.parent.as_posix()} names differently, "
        f"{result.dropped_covered} for bytes a routine of another name states "
        f"as well; {result.distinguished_lines} kept because references tell "
        f"same-byte routines apart"
        + (f", {result.merged_aliases} merged as one routine's aliases"
           if result.merged_aliases else "")
        + ")",
        flush=True,
    )
    for example in result.conflict_examples[:5]:
        print(f"  ambiguous: {', '.join(example)[:200]}", flush=True)


def build(args: argparse.Namespace) -> int:
    assets = load_assets(args.assets, args.asset)
    by_directory: dict[Path, dict[Path, list[Asset]]] = {}
    for asset in assets:
        by_directory.setdefault(asset.directory, {}).setdefault(asset.output, []).append(asset)

    work_root = Path(tempfile.mkdtemp(prefix="neverd-msvc-sigs-", dir=args.work))
    try:
        for _, outputs in sorted(by_directory.items()):
            results: dict[Path, FoldResult] = {}
            sources: dict[Path, list[dict]] = {}
            for relative, members in sorted(outputs.items()):
                generated, repeated, sources[relative] = generate(args, members, work_root)
                results[relative] = fold(generated)
                results[relative].duplicates += repeated
                del generated
            settle_directory(results)
            for relative, result in sorted(results.items()):
                write(args, relative, result, sources[relative])
    finally:
        shutil.rmtree(work_root, ignore_errors=True)
    return 0


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--sigmaker", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True,
                        help="directory holding <asset>.tar.zst and <asset>.json")
    parser.add_argument("--asset", action="append", default=[],
                        help="only assets whose name matches this glob (repeatable)")
    parser.add_argument("--output", type=Path, required=True,
                        help="signature tree root; files land under pe/ and elf/")
    parser.add_argument("--tail", type=int, default=FULL_COVERAGE_TAIL)
    parser.add_argument("--max-file-bytes", type=int, default=MAX_FILE_BYTES,
                        help="write a larger file as parts: <name>.pat, <name>.part2.pat, ...")
    parser.add_argument("--neverd-ref", default=None,
                        help="NeverD revision of the signature maker, for provenance")
    parser.add_argument("--release", default=None,
                        help="release tag the assets came from, for provenance")
    parser.add_argument("--work", type=Path, default=None,
                        help="scratch directory for unpacked libraries")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_arguments(argv)
    if not args.sigmaker.is_file():
        print(f"error: {args.sigmaker} does not exist", file=sys.stderr)
        return 1
    try:
        return build(args)
    except BuildError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
