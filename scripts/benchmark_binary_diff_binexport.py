"""Read the native inventory actually consumed by BinDiff, without protobuf deps.

This is a bounded wire projection of BinExport2's Meta and CallGraph vertices,
not an instruction decoder. Field numbers follow the upstream public schema:
https://github.com/google/binexport/blob/main/binexport2.proto
Unknown length-delimited payloads (notably instructions) are skipped in place.
"""
import hashlib
from pathlib import Path


UINT64_MAX = (1 << 64) - 1
FIELD_MAX = (1 << 29) - 1
VERTEX_TYPES = ("NORMAL", "LIBRARY", "IMPORTED", "THUNK", "INVALID")
READER_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def inventory(path: Path) -> dict:
    with path.open("rb") as stream:
        end = stream.seek(0, 2)

        def varint(limit):
            value = 0
            for shift in range(0, 70, 7):
                if stream.tell() >= limit:
                    raise ValueError("truncated BinExport varint")
                byte = stream.read(1)[0]
                value |= (byte & 127) << shift
                if value > UINT64_MAX:
                    raise ValueError("overflowing BinExport varint")
                if not byte & 128:
                    return value
            raise ValueError("unterminated BinExport varint")

        def fields(begin, limit):
            stream.seek(begin)
            while stream.tell() < limit:
                tag = varint(limit)
                field, wire = tag >> 3, tag & 7
                if not 0 < field <= FIELD_MAX:
                    raise ValueError("invalid BinExport field number")
                if wire == 0:
                    yield field, wire, varint(limit)
                    continue
                if wire not in (1, 2, 5):
                    raise ValueError("unsupported BinExport wire type")
                size = varint(limit) if wire == 2 else (8 if wire == 1 else 4)
                start = stream.tell()
                if size > limit - start:
                    raise ValueError("BinExport field exceeds its message")
                yield field, wire, (start, start + size)
                stream.seek(start + size)

        def require_wire(actual, expected):
            if actual != expected:
                raise ValueError("wrong BinExport inventory field type")

        graph_seen, meta_seen = False, False
        executable_id, entries, types = None, [], dict.fromkeys(VERTEX_TYPES, 0)
        for field, wire, value in fields(0, end):
            if field == 1:  # Meta
                require_wire(wire, 2)
                if meta_seen:
                    raise ValueError("duplicate BinExport metadata")
                meta_seen = True
                for key, kind, span in fields(*value):
                    if key == 2:  # executable_id
                        require_wire(kind, 2)
                        if executable_id is not None:
                            raise ValueError("duplicate BinExport executable identity")
                        if span[1] - span[0] not in (40, 64):
                            raise ValueError("unsupported BinExport executable identity")
                        stream.seek(span[0])
                        executable_id = stream.read(span[1] - span[0]).decode("ascii")
            elif field == 8:  # CallGraph
                require_wire(wire, 2)
                if graph_seen:
                    raise ValueError("duplicate BinExport call graph")
                graph_seen = True
                for key, kind, span in fields(*value):
                    if key != 1:  # vertex
                        continue
                    require_wire(kind, 2)
                    address, vertex_type, type_seen = None, 0, False
                    for item, encoding, number in fields(*span):
                        if item in (1, 2):
                            require_wire(encoding, 0)
                            if item == 1:
                                if address is not None:
                                    raise ValueError("duplicate BinExport vertex address field")
                                address = number
                            else:
                                if type_seen:
                                    raise ValueError("duplicate BinExport vertex type")
                                vertex_type, type_seen = number, True
                    if address is None or vertex_type >= len(VERTEX_TYPES):
                        raise ValueError("incomplete or unknown BinExport vertex")
                    if entries and address <= entries[-1]:
                        raise ValueError("BinExport vertices are not unique and ascending")
                    entries.append(address)
                    types[VERTEX_TYPES[vertex_type]] += 1
        if not graph_seen or not meta_seen or executable_id is None:
            raise ValueError("BinExport lacks native graph or input identity")
        return {"entries": entries, "vertex_types": types,
                "executable_id": executable_id, "inventory_source": "BinExport2.CallGraph.vertex"}
