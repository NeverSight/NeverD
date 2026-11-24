#!/usr/bin/env python3
"""Read the VBLANK and ROM strobes in the DTL-H2700 TUTO3.PAD recording."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


CAPTURE_SHA256 = "47ad71e3110126ee829e6ac168d1bf0e277b5fc6af0c4c206c1e9ceade225eca"
FRAME_BYTES = 16
BLOCK_BYTES = 1024 * FRAME_BYTES


def channel(frames, bit):
    levels = bytes((value >> (bit % 8)) & 1 for value in range(256))
    return frames[bit // 8::FRAME_BYTES].translate(levels)


def first_rom_read(frames, edge):
    # /CS2 and SBUS /RD are both active low. SBUS A23:A0 is in bits 60:37.
    for sample in range(edge, len(frames) // FRAME_BYTES):
        word, = struct.unpack_from("<Q", frames, sample * FRAME_BYTES)
        if word & ((1 << 24) | (1 << 15)) == 0:
            return {
                "vblank_rise_sample": edge,
                "rom_read_sample": sample,
                "elapsed_samples": sample - edge,
                "sbus_a23_a0": f"0x{(word >> 37) & 0xffffff:06x}",
            }
    raise ValueError(f"no ROM read following VBLANK at sample {edge}")


def measure(path):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != CAPTURE_SHA256:
        raise ValueError(f"{path}: unexpected TUTO3.PAD recording")

    # In this PAD2.02 recording, the count follows tag 0x50003, and tag
    # 0x50014 precedes the raw frames. Four bytes after the blocks end the file.
    count, = struct.unpack_from("<I", data, 13030)
    storage = data[13156:-4]
    end = len(storage)
    # The final stored block is all ones on all 128 channels. Exclude it
    # before looking for edges, even where it lies inside the declared count.
    while end >= BLOCK_BYTES and storage[end - BLOCK_BYTES:end] == b"\xff" * BLOCK_BYTES:
        end -= BLOCK_BYTES
    frames = storage[:min(count * FRAME_BYTES, end)]

    vblank = channel(frames, 36)
    return {
        "capture_sha256": CAPTURE_SHA256,
        "declared_frames": count,
        "stored_frames": len(storage) // FRAME_BYTES,
        "trailing_all_high_blocks": (len(storage) - end) // BLOCK_BYTES,
        "examined_frames": len(frames) // FRAME_BYTES,
        "irq_low_samples": {
            name: channel(frames, bit).count(b"\x00")
            for name, bit in (("/IRQ1", 35), ("/IRQ2", 71), ("/IRQ9", 67), ("/IRQ10", 65))
        },
        "vblank_fall_samples": [m.start() + 1 for m in re.finditer(b"\x01\x00", vblank)],
        "rom_reads_after_vblank_rises": [
            first_rom_read(frames, m.start() + 1) for m in re.finditer(b"\x00\x01", vblank)
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    print(json.dumps(measure(parser.parse_args().capture), indent=2))
