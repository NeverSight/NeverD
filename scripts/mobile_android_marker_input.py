"""CI-only SDK evidence for MarkerBase's four empty class applications.

The SDK verifies and reads the actual post-D8 input. This helper consumes its
``-a -f -h -l plain`` output; it is not a DEX parser or a recovery certificate.
AOSP dumpClass emits the indexed annotation block before the class descriptor:
https://android.googlesource.com/platform/art/+/310cca0/dexdump/dexdump.cc
"""
from __future__ import annotations

import hashlib
import re

if __package__:
    from .mobile_real_apps_android import dex_header, parse_dexdump, require
else:
    from mobile_real_apps_android import dex_header, parse_dexdump, require


OWNER = "Lfixture/MarkerBase;"
EXPECTED = {
    "Lfixture/MarkerDefault;": "VISIBILITY_BUILD",
    "Lfixture/MarkerClass;": "VISIBILITY_BUILD",
    "Lfixture/MarkerInherited;": "VISIBILITY_RUNTIME",
    "Lfixture/MarkerRuntime;": "VISIBILITY_RUNTIME",
}


def inspect_marker_dex(data: bytes, dump: str) -> dict:
    require(type(data) is bytes and len(data) <= 1024 * 1024,
            "Marker input DEX exceeds the owned fixture byte budget")
    require(isinstance(dump, str) and len(dump) <= 4 * 1024 * 1024,
            "Marker input SDK dump exceeds the owned fixture text budget")
    header = dex_header(data)
    inventory = parse_dexdump(dump, "classes.dex", header)
    owners = [row for row in inventory["classes"] if row["descriptor"] == OWNER]
    require(len(owners) == 1, "Marker input SDK inventory lacks its unique owner")
    owner_index = owners[0]["index"]

    # Complete class headers and bodies are validated by parse_dexdump. Keep
    # annotation ownership separate: an annotation's type is not its owner.
    header_index = annotation_index = None
    seen = set()
    owner_lines = None
    for raw in dump.splitlines():
        line = raw.strip()
        if not line:
            continue
        match = re.fullmatch(r"Class #(\d+) header:", line)
        if match:
            header_index, annotation_index = int(match[1]), None
            continue
        match = re.fullmatch(r"Class #(\d+)\s*-", line)
        if match:
            require(header_index == int(match[1]), "Marker input SDK class binding changed")
            header_index = annotation_index = None
            continue
        match = re.fullmatch(r"Class #(\d+) annotations:", line)
        if match:
            index = int(match[1])
            require(header_index == index and index not in seen,
                    "Marker input SDK annotation block is detached, repeated or misbound")
            seen.add(index)
            annotation_index = index
            if index == owner_index:
                owner_lines = []
            continue
        require(not line.startswith("Class #"), "Marker input SDK class marker is malformed")
        if annotation_index is not None:
            if annotation_index == owner_index:
                owner_lines.append(line)
        elif line.startswith(("Annotations on", "VISIBILITY_", "empty-annotation-set")):
            require(False, "Marker input SDK annotation is outside its indexed block")

    require(owner_lines is not None and len(owner_lines) == 5
            and owner_lines[0] == "Annotations on class",
            "Marker input requires exactly four empty applications on the owner class")
    observed = {}
    for line in owner_lines[1:]:
        match = re.fullmatch(r"(VISIBILITY_[A-Z]+)\s+(\S+)", line)
        require(match is not None and match[2] in EXPECTED
                and EXPECTED[match[2]] == match[1],
                "Marker input application has values, an unexpected type or wrong visibility")
        require(match[2] not in observed, "Marker input repeats an annotation application")
        observed[match[2]] = match[1]
    require(observed == EXPECTED, "Marker input annotation applications are incomplete")
    return {"scope": "owned-markerbase-class-annotations", "provider": "android-sdk-dexdump",
            "input_sha256": hashlib.sha256(data).hexdigest(), "header": header,
            "class_descriptors": [row["descriptor"] for row in inventory["classes"]],
            "owner": OWNER, "class_index": owner_index,
            "annotations": [{"type": name, "visibility": observed[name]} for name in sorted(observed)]}
