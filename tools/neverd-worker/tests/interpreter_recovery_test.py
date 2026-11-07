#!/usr/bin/env python3
"""Recovery views retain SDK results, page once and reject malformed requests."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from transport_test import BASE, Client
from source_synthesis_test import elf, ENTRY


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def request(address, options, representation="source", offset=0, limit=2):
    return dict(address=address, representation=representation,
                source_options={"interpreter_recovery": options},
                offset=offset, limit=limit)


def run(args):
    with tempfile.TemporaryDirectory(prefix="neverd-interpreter-view-") as directory:
        root = Path(directory)
        binary = root / "recovery-source.bin"
        if args.real:
            # Whole XMM copy/add, then independently selected CPUID and carry.
            binary.write_bytes(elf(bytes.fromhex(
                "f30f6fc2 660ffcc1 b807000000 b903000000 f9 0fa2 c3")))
            address = hex(ENTRY)
        else:
            binary.write_bytes(b"recovery view fixture")
            address = BASE
        with Client(args.worker) as client:
            assert "interpreter_recovery" in client.hello["capabilities"]
            ok(client, "open", {"path": str(binary), "analysis": False})
            for representation in ("source", "llvmc"):
                options = {"source_abi": "xmm"}
                first = ok(client, "decompile", request(address, options, representation))
                assert first["source_available"] and first["recovery_report"]["complete"], first
                assert first["rows"] == [] and first["code_names"] == []
                assert first["mapping_status"] == "unmapped_recovery_source"
                if not args.real:
                    assert first["recovery_report"]["fixture_prepares"] == 0
                pieces = [first["text"]]
                page = first
                while not page["complete"]:
                    page = ok(client, "decompile", request(address, options, representation,
                                                            page["next_offset"]))
                    assert "recovery_report" not in page
                    pieces.append(page["text"])
                source = "".join(pieces)
                repeated = ok(client, "decompile", request(address, options, representation))
                assert repeated["recovery_report"] == first["recovery_report"]
                if args.cli:
                    output = root / (representation + ".c")
                    cmd = [args.cli, "decompile", str(binary), "--func", address,
                           "--devirtualize", "--vm-machine-state", "--vm-source-abi=xmm",
                           "-o", str(output)]
                    if representation == "llvmc":
                        cmd.append("--llvm")
                    subprocess.run(cmd, check=True, capture_output=True, text=True)
                    assert source == output.read_text(), representation
            invalid = [{"unknown": 1}, {"source_abi": "unknown"},
                       {"max_nodes": -1}, {"max_nodes": True},
                       {"max_nodes": 0.5}, {"max_nodes": 1 << 32},
                       {"entry_frame_begin": -(1 << 63) - 1, "entry_frame_end": 1},
                       {"entry_frame_begin": 0, "entry_frame_end": 1 << 63},
                       {"entry_frame_begin": 0}, {"external_stores_disjoint_frame": 1}]
            for options in invalid:
                reply = client.call("decompile", request(address, options))
                assert reply["status"] == "error", (options, reply)
            for representation in ("cpp", "rust", "low"):
                reply = client.call("decompile", request(address, {}, representation))
                assert reply["status"] == "error", reply
            mixed = request(address, {})
            mixed["source_options"]["symbolic"] = True
            assert client.call("decompile", mixed)["status"] == "error"
            if not args.real:
                options = dict(source_abi="queries", entry_frame_begin=-8192,
                               entry_frame_end=4096, entry_frame_alignment=16,
                               entry_frame_residue=8, external_stores_disjoint_frame=True,
                               prove_store_return_separation=True, max_nodes=765,
                               max_operations=(1 << 63) + 17)
                page = ok(client, "decompile", request(address, options))
                fields = page["recovery_report"]["fixture_options"]
                assert fields["abi"] == 1 and fields["frame_begin"] == -8192
                assert fields["frame_end"] == 4096 and fields["frame_flags"] == 2
                assert fields["alignment"] == 16 and fields["residue"] == 8
                assert fields["separation_flags"] == fields["proof_flags"] == 1
                assert fields["max_nodes"] == 765 and fields["max_operations"] == (1 << 63) + 17
        refused = root / "recovery-refused.bin"
        refused.write_bytes(elf(bytes.fromhex("ffe0")) if args.real else b"fixture")
        with Client(args.worker) as client:
            ok(client, "open", {"path": str(refused), "analysis": False})
            page = ok(client, "decompile", request(address, {"source_abi": "xmm"}))
            assert page["text"] == "" and not page["source_available"], page
            assert "error" in page["recovery_report"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--real", action="store_true")
    parser.add_argument("--cli")
    run(parser.parse_args())
