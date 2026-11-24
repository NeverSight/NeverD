#!/usr/bin/env python3
"""Measure reset release to the first external boot-read request on two TLA waveforms."""

import argparse
import hashlib
import json
from pathlib import Path

import pymupdf


def first_edge(pix, samples, columns, high_y, low_y, initial_high):
    """Bracket a transition with the last old and first new logic levels."""
    last_old = None
    # The PDF's image transform flips the waveform vertically. The original
    # digital levels are white; the colored cursors can obscure both rails.
    def white(x, y):
        offset = (pix.height - 1 - y) * pix.stride + x * pix.n
        return samples[offset : offset + 3] == b"\xff\xff\xff"

    for x in columns:
        high, low = white(x, high_y), white(x, low_y)
        if high == low:
            continue
        if high == initial_high:
            last_old = x
        elif last_old is not None:
            return last_old, x
        else:
            raise ValueError("initial level missing before the first edge")
    raise ValueError("no transition in the acquisition window")


def measure(path, sha256, page, columns, ns_per_pixel, reset_rows, request_rows, request_signal):
    source = path.read_bytes()
    if hashlib.sha256(source).hexdigest() != sha256:
        raise ValueError(f"{path}: unexpected probe manual edition")
    with pymupdf.open(stream=source, filetype="pdf") as doc:
        (waveform,) = doc[page - 1].get_images()
        pix = pymupdf.Pixmap(doc, waveform[0])
        samples = pix.samples
        release = first_edge(pix, samples, columns, *reset_rows, initial_high=False)
        request = first_edge(pix, samples, columns, *request_rows, initial_high=True)
    if request[0] <= release[1]:
        raise ValueError("cannot resolve the first request after reset release")
    bounds = [(request[0] - release[1]) * ns_per_pixel,
              (request[1] - release[0]) * ns_per_pixel]
    return {
        "pdf_page": page,
        "ns_per_pixel": ns_per_pixel,
        "start_event": {"kind": "reset_release", "signal": "ResetB", "edge": "rising"},
        "end_event": {
            "kind": "external_boot_read_request", "signal": request_signal, "edge": "falling",
        },
        "reset_release_x_px": release,
        "first_valid_request_x_px": request,
        "interval_ns": sum(bounds) / 2,
        "raster_bounds_ns": bounds,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("r4300_manual", type=Path)
    parser.add_argument("r4xxx_manual", type=Path)
    args = parser.parse_args()
    # Native 640 x 480 figure 5.1 in each manual. In the R4300 figure,
    # C1 and C2 are 180 pixels and 360 ns apart. The R4600 axis spans
    # 512 pixels from -72 ns to +440 ns. Rows are (high, low) per channel.
    print(json.dumps({
        "R4300": measure(
            args.r4300_manual,
            "95c7ae1667dbd3722d0320df394293047fb3f2130ed3c47d50a7d8ce75fcfd97",
            47, range(110, 621), 2, (376, 388), (250, 262), "PValidB",
        ),
        "R4600": measure(
            args.r4xxx_manual,
            "85c1024fa15c3c4b994c4558c235303cff6df640ed1e60c8864e7004b4a234c7",
            58, range(108, 621), 1, (328, 340), (184, 196), "ValidOutB",
        ),
    }, indent=2))


if __name__ == "__main__":
    main()
