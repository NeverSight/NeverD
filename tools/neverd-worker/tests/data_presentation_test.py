#!/usr/bin/env python3
"""Arrays/alignment/enum rows: pagination, real bits, replay and atomic refusal;
SBF and EVM operand formats in their own spellings."""
import argparse
from contextlib import ExitStack
from pathlib import Path
import struct
import tempfile
from transport_test import Client
from source_variable_types_test import ok, global_elf, source
from source_synthesis_test import ENTRY, elf

DATA = 0x401000


def fixture():
    image, entry = global_elf()
    image = bytearray(image)
    struct.pack_into("<QQ", image, 152, 256, 256)
    image += bytes(224)
    # mov eax, 0xB; ret. Bytes after it are not an immediate match.
    image[176:182] = bytes.fromhex("b80b000000c3")
    for n in range(64):
        struct.pack_into("<I", image, 4096+4*n, n+1)
    return image, entry


def sbf_program(*instructions):
    """A legacy SBF ELF (EM_BPF, version 0) whose text is the instructions."""
    text = b"".join(struct.pack("<BBhi", op, (src << 4) | dst, offset, imm)
                    for op, dst, src, offset, imm in instructions)
    names = b"\0.text\0.rodata\0.shstrtab\0"
    text_at, names_at = 64, 64 + len(text)
    sections_at = (names_at + len(names) + 7) & ~7
    header = (b"\x7fELF\x02\x01\x01" + bytes(9) +
              struct.pack("<HHIQQQIHHHHHH", 3, 247, 1, 0, 0, sections_at, 0,
                          64, 56, 0, 64, 4, 3))
    image = bytearray(sections_at + 4 * 64)
    image[:64] = header
    image[text_at:names_at] = text
    image[names_at:names_at + len(names)] = names
    for index, (name, kind, flags, address, offset, size, align) in enumerate((
            (0, 0, 0, 0, 0, 0, 0),
            (1, 1, 6, 0, text_at, len(text), 8),
            (7, 1, 2, len(text), names_at, 0, 8),
            (15, 3, 0, 0, names_at, len(names), 1))):
        struct.pack_into("<IIQQQQIIQQ", image, sections_at + 64 * index, name,
                         kind, flags, address, offset, size, 0, 0, align, 0)
    return bytes(image)


def instructions(client, address):
    return [row["text"] for row in listing(client, address)["lines"]
            if row["kind"] == "insn"]


def listing(client, address=DATA, **options):
    return ok(client, "listing", {"address": hex(address), "before": 0, "after": 48, **options})


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-data-presentation-") as directory, ExitStack() as stack:
        path = Path(directory) / "data.elf"
        image, entry = fixture()
        path.write_bytes(image)
        client = stack.enter_context(Client(executable))
        ok(client, "open", {"path": str(path), "analysis": False})
        native = {r: source(client, r, entry)["text"] for r in ("low", "med", "llvm")}
        declarations = "struct QA_ROW { unsigned int A; unsigned int B; unsigned int C; }; enum QA_FLAGS { QA_READ = 1, QA_WRITE = 2, QA_READ_ALIAS = 1 }; enum QA_ALIAS { QA_ELEVEN = 11, QA_ELEVEN_ALIAS = 11 };"
        ok(client, "local_types_declare", {"source": declarations})
        ok(client, "item_define", {"address": hex(DATA), "action": "array", "element_size": 4, "count": 16})
        page = listing(client)
        assert "[0]" in str(page) and "[15]" in str(page), page
        assert "[7]" in str(listing(client, DATA+28)), page
        state = ok(client, "analysis_snapshot")
        for change in ({"count": 0}, {"count": 1048576}, {"element_size": 3}, {"element_size": 8, "type": "QA_ROW"}):
            reply = client.call("item_define", {"address": hex(DATA), "action": "array", "element_size": 4, "count": 16, **change})
            assert reply["status"] == "error", reply
            assert ok(client, "analysis_snapshot") == state
        ok(client, "undo")
        assert "array element" not in str(listing(client))
        ok(client, "redo")
        assert "[15]" in str(listing(client))
        ok(client, "item_define", {"address": hex(DATA), "action": "array", "element_size": 12, "count": 4, "type": "QA_ROW"})
        assert "QA_ROW" in str(listing(client)) and "[3]" in str(listing(client))
        ok(client, "item_define", {"address": hex(DATA+67), "action": "alignment", "alignment": 16})
        assert "align" in str(listing(client, DATA+67))
        state = ok(client, "analysis_snapshot")
        assert client.call("item_define", {"address": hex(DATA+255), "action": "alignment", "alignment": 1024})["status"] == "error"
        assert client.call("item_define", {"address": hex(entry), "action": "array", "element_size": 1, "count": 6})["status"] == "error"
        assert ok(client, "analysis_snapshot") == state
        # Exact decoded immediate, flags and residual unknown bits survive.
        ok(client, "operand_format", {"address": hex(entry), "operand": 1, "action": "enum", "type": "QA_FLAGS", "flags": True})
        shown = str(listing(client, entry))
        assert "QA_READ | QA_WRITE | 0x8" in shown, shown
        ok(client, "operand_format", {"address": hex(entry), "operand": 1, "action": "enum", "type": "QA_ALIAS"})
        assert "QA_ELEVEN" in str(listing(client, entry)) and "QA_ELEVEN_ALIAS" not in str(listing(client, entry))
        ok(client, "undo")
        state = ok(client, "analysis_snapshot")
        for request in ({"address": hex(entry+5)}, {"type": "QA_ROW"}, {"type": "MISSING"}):
            reply = client.call("operand_format", {"address": hex(entry), "operand": 1, "action": "enum", "type": "QA_FLAGS", **request})
            assert reply["status"] == "error", reply
            assert ok(client, "analysis_snapshot") == state
        ok(client, "undo")
        assert "QA_READ |" not in str(listing(client, entry))
        ok(client, "redo")
        # A changed type cannot resize the durable array span or keep a cached
        # enum spelling. Numeric storage remains available until the type returns.
        ok(client, "local_types_forget", {"names": ["QA_ROW", "QA_FLAGS"]})
        assert "stale element type" in str(listing(client))
        assert "QA_READ |" not in str(listing(client, entry))
        ok(client, "undo")
        assert "QA_READ | QA_WRITE | 0x8" in str(listing(client, entry))
        for rep, text in native.items(): assert source(client, rep, entry)["text"] == text
        replica = stack.enter_context(Client(executable))
        ok(replica, "analysis_restore", ok(client, "analysis_snapshot"))
        source(replica, "low", entry)
        assert "QA_ROW" in str(listing(replica))
        replica_page = listing(replica, entry)
        assert "QA_READ | QA_WRITE | 0x8" in str(replica_page), (replica_page, ok(client, "analysis_snapshot"))
        assert replica.call("item_define", {"address": hex(DATA), "action": "undefine"})["status"] == "error"
        replica.close(); client.close()
        client = stack.enter_context(Client(executable))
        ok(client, "open", {"path": str(path), "analysis": False})
        source(client, "low", entry)
        assert "QA_ROW" in str(listing(client))
        assert "QA_READ | QA_WRITE | 0x8" in str(listing(client, entry))
        ok(client, "item_define", {"address": hex(DATA+24), "action": "undefine"})
        assert "array element" not in str(listing(client))
        negative = Path(directory) / "negative.elf"
        image[177:181] = bytes.fromhex("ffffffff")
        negative.write_bytes(image)
        ok(client, "open", {"path": str(negative), "analysis": False})
        source(client, "low", entry)
        ok(client, "local_types_declare", {"source": "enum QA_SIGNED { QA_NEGATIVE = -1 };"})
        ok(client, "operand_format", {"address": hex(entry), "operand": 1, "action": "enum", "type": "QA_SIGNED"})
        assert "QA_NEGATIVE" in str(listing(client, entry))
        # A shift's byte immediate has its own logical width, independently
        # of its 32-bit destination. Folding must retain decoded indices too.
        for name, code, operand, declaration, expected in (
                ("shift", "c1e0ffc3", 1,
                 "enum QA_BYTE : signed char { QA_BYTE_NEGATIVE = -1 };",
                 "QA_BYTE_NEGATIVE"),
                ("imul", "6bc00bc3", 2,
                 "enum QA_IMUL { QA_FACTOR = 11 };", "QA_FACTOR")):
            small = Path(directory) / (name + ".elf")
            small.write_bytes(elf(bytes.fromhex(code)))
            ok(client, "open", {"path": str(small), "analysis": False})
            source(client, "low", ENTRY)
            ok(client, "local_types_declare",
               {"source": declaration, "language": "cpp"})
            enum_type = "QA_BYTE" if name == "shift" else "QA_IMUL"
            ok(client, "operand_format",
               {"address": hex(ENTRY), "operand": operand,
                "action": "enum", "type": enum_type})
            assert expected in str(listing(client, ENTRY))
        # SBF and EVM operands take the same formats in their own spellings.
        program = Path(directory) / "program.so"
        program.write_bytes(sbf_program(
            (0xb7, 1, 0, 0, 24),       # mov64 r1, 24
            (0x79, 3, 1, 24, 0),       # ldxdw r3, [r1 + 24]
            (0xb4, 0, 0, 0, -1),       # mov32 r0, -1
            (0x95, 0, 0, 0, 0)))       # exit
        ok(client, "open", {"path": str(program), "analysis": False})
        start = 0x100000000
        source(client, "low", start)
        ok(client, "local_types_declare", {"source": (
            "struct QA_PAIR { long long first; long long second; "
            "long long third; long long fourth; };"
            "enum QA_MASK : unsigned { QA_ALL = ~0u };")})
        assert "mov64   r1, 24" in instructions(client, start)[0], instructions(client, start)
        ok(client, "operand_format", {"address": hex(start), "operand": 1, "action": "hex"})
        ok(client, "operand_format", {"address": hex(start + 8), "operand": 1,
                                      "action": "struct", "type": "QA_PAIR"})
        ok(client, "operand_format", {"address": hex(start + 16), "operand": 1,
                                      "action": "enum", "type": "QA_MASK"})
        shown = instructions(client, start)
        assert "r1, 0x18" in shown[0], shown
        assert "r3, [r1 + QA_PAIR.fourth]" in shown[1], shown
        assert "r0, QA_ALL" in shown[2], shown
        # The type picker judges the operand's own access in SBF syntax.
        fit = ok(client, "local_types_fit", {"address": hex(start + 8), "operand": 1})
        assert fit["syntax"] == "sbf" and fit["target"]["base"] == "r1", fit
        assert fit["accesses"][0]["offset"] == 24, fit
        # A load's displacement takes a structure offset, but no number base.
        assert client.call("operand_format", {"address": hex(start + 8), "operand": 1,
                                              "action": "hex"})["status"] == "error"
        bytecode = Path(directory) / "push.evm"
        # PUSH2 0x0004; PUSH32 ff..ff; STOP
        bytecode.write_text("610004" + "7f" + "ff" * 32 + "00", encoding="ascii")
        ok(client, "open", {"path": str(bytecode), "analysis": False})
        source(client, "low", 0)
        ok(client, "local_types_declare",
           {"source": "enum QA_EVM : unsigned short { QA_EVM_FOUR = 4 };", "language": "cpp"})
        ok(client, "operand_format", {"address": "0x0", "operand": 0,
                                      "action": "enum", "type": "QA_EVM"})
        ok(client, "operand_format", {"address": "0x3", "operand": 0, "action": "decimal"})
        shown = instructions(client, 0)
        assert "PUSH2   QA_EVM_FOUR" in shown[0], shown
        assert ("PUSH32  1157920892373161954235709850086879078532699846656405640394"
                "57584007913129639935") in shown[1], shown
        reply = client.call("operand_format", {"address": "0x3", "operand": 0,
                                               "action": "enum", "type": "QA_EVM"})
        assert reply["status"] == "error" and "64 bits" in reply["error"]["message"], reply
        # Bytecode listings ask no native decoder about their functions.
        assert b"unsupported arch for decoder" not in client.logs, bytes(client.logs)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("worker")
    run(parser.parse_args().worker)
