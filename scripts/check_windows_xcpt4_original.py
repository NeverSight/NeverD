#!/usr/bin/env python3
"""Run the original MSVC PE32 xcpt4 and check each numbered outcome."""

import argparse
import json
import os
from pathlib import Path

from check_windows_registration_eh import image_digest, run_image
from check_windows_xcpt4_source import check_runtime


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    before = image_digest(args.image)
    result = run_image(args.image, [], os.environ.copy(), 120)
    try:
        cases = check_runtime(result)
        if image_digest(args.image) != before:
            raise ValueError("PE32 fixture changed during execution")
    except ValueError as error:
        print(json.dumps({"error": str(error), "sha256": before, **result}))
        return 1
    print(json.dumps({"sha256": before, "cases": cases, **result},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
