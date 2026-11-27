"""Small owned class files for wire formats not emitted by the current compiler."""

import base64
import hashlib
import json
from pathlib import Path
import struct

U2 = struct.Struct(">H").pack
U4 = struct.Struct(">I").pack


def program(
    code,
    *,
    descriptor="(I)I",
    major=49,
    minor=0,
    stack=2,
    locals_=1,
    frames=None,
    handlers=(),
    opaque=False,
    dynamic=0,
):
    pool = []

    def add(tag, data):
        pool.append(bytes([tag]) + data)
        return len(pool)

    def utf(s):
        b = s.encode("ascii")
        return add(1, U2(len(b)) + b)

    cls = add(7, U2(utf("Wire")))
    obj = add(7, U2(utf("java/lang/Object")))
    method = utf("method")
    desc = utf(descriptor)
    code_name = utf("Code")
    ctor_nt = add(12, U2(utf("<init>")) + U2(utf("()V")))
    ctor = add(10, U2(obj) + U2(ctor_nt))
    bootstrap_attrs = []
    site = 0
    if dynamic:
        owner = add(7, U2(utf("example/Bootstrap")))
        bdesc = utf(
            "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;"
        )
        nt = add(12, U2(utf("resolve")) + U2(bdesc))
        bootstrap = add(15, b"\x06" + U2(add(10, U2(owner) + U2(nt))))
        name_type = add(12, U2(utf("value")) + U2(utf("I" if dynamic == 17 else "()I")))
        site = add(dynamic, U2(0) + U2(name_type))
        data = U2(1) + U2(bootstrap) + U2(0)
        bootstrap_attrs.append(U2(utf("BootstrapMethods")) + U4(len(data)) + data)
    if callable(code):
        code = code(obj, ctor, site)
    attr = []
    if frames is not None:
        attr.append(U2(utf("StackMapTable")) + U4(len(frames)) + frames)
    if opaque:
        attr.append(U2(utf("OwnerPrivateMetadata")) + U4(3) + b"\x07\x00\x10")
    if major == 45 and minor <= 2:
        body = bytes([stack, locals_]) + U2(len(code))
    else:
        body = U2(stack) + U2(locals_) + U4(len(code))
    body += (
        code
        + U2(len(handlers))
        + b"".join(struct.pack(">4H", *h) for h in handlers)
        + U2(len(attr))
        + b"".join(attr)
    )
    return (
        b"\xca\xfe\xba\xbe"
        + U2(minor)
        + U2(major)
        + U2(len(pool) + 1)
        + b"".join(pool)
        + U2(0x21)
        + U2(cls)
        + U2(obj)
        + U2(0)
        + U2(0)
        + U2(1)
        + U2(9)
        + U2(method)
        + U2(desc)
        + U2(1)
        + U2(code_name)
        + U4(len(body))
        + body
        + U2(len(bootstrap_attrs))
        + b"".join(bootstrap_attrs)
    )


def table():
    # PC 1 is the switch opcode. Its targets address the three return arms.
    return (
        bytes.fromhex("1aaa0000")
        + struct.pack(">iiiii", 27, 0, 1, 23, 25)
        + bytes.fromhex("04ac05ac06ac")
    )


def fixtures():
    rows = []

    def row(name, status, data):
        rows.append(
            {
                "id": name,
                "status": status,
                "bytes": base64.b64encode(data).decode(),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )

    for minor in range(3):
        row(
            f"early-45.{minor}",
            "reconstructed_source",
            program(bytes.fromhex("1a0660ac"), major=45, minor=minor),
        )
    row("opaque", "reconstructed_source", program(bytes.fromhex("1aac"), opaque=True))
    row(
        "wide",
        "reconstructed_source",
        program(bytes.fromhex("06c4360100c4150100ac"), locals_=257),
    )
    row("tableswitch", "reconstructed_source", program(table()))
    bad = bytearray(table())
    bad[2] = 1
    row("switch-padding", "malformed", program(bad))
    bad = bytearray(table())
    bad[7] = 4
    row("switch-target", "malformed", program(bad))
    row(
        "wide-boundary",
        "malformed",
        program(bytes.fromhex("a70004c4150100ac"), locals_=257),
    )
    row("wide-opcode", "malformed", program(bytes.fromhex("c46000ac")))
    row(
        "wide-local",
        "malformed",
        program(bytes.fromhex("c4160000ad"), descriptor="()J"),
    )
    row(
        "bad-stack-join",
        "malformed",
        program(bytes.fromhex("1a99000704a70004095803ac")),
    )
    row("wrong-return", "malformed", program(bytes.fromhex("09ac")))
    row(
        "map-primitive-type",
        "malformed",
        program(bytes.fromhex("1aac"), major=52, frames=bytes.fromhex("00014102")),
    )
    row(
        "map-operand-boundary",
        "malformed",
        program(bytes.fromhex("1003ac"), major=52, frames=bytes.fromhex("000101")),
    )
    row(
        "handler-boundary",
        "malformed",
        program(bytes.fromhex("1003ac"), handlers=[(0, 1, 1, 0)]),
    )
    row(
        "uninitialized-return",
        "malformed",
        program(
            lambda obj, ctor, site: b"\xbb" + U2(obj) + b"\xb0",
            descriptor="()Ljava/lang/Object;",
        ),
    )
    row(
        "allocation",
        "reconstructed_source",
        program(
            lambda obj, ctor, site: b"\xbb"
            + U2(obj)
            + b"\x59\xb7"
            + U2(ctor)
            + b"\xb0",
            descriptor="()Ljava/lang/Object;",
        ),
    )
    row(
        "monitor",
        "disassembly_only",
        program(bytes.fromhex("2ac22ac3b1"), descriptor="(Ljava/lang/Object;)V"),
    )
    row(
        "unknown-indy",
        "disassembly_only",
        program(
            lambda o, c, k: b"\xba" + U2(k) + b"\0\0\xac",
            major=55,
            descriptor="()I",
            dynamic=18,
        ),
    )
    row(
        "unknown-condy",
        "disassembly_only",
        program(
            lambda o, c, k: b"\x13" + U2(k) + b"\xac",
            major=55,
            descriptor="()I",
            dynamic=17,
        ),
    )
    return rows


if __name__ == "__main__":
    (Path(__file__).resolve().parent / "fixtures" / "synthetic.json").write_text(
        json.dumps({"fixtures": fixtures()}, separators=(",", ":")) + "\n"
    )
