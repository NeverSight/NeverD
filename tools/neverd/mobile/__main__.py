"""Executable entry point, also usable with Python's isolated mode."""

import sys
from pathlib import Path

if sys.version_info < (3, 10):
    sys.exit("error: NeverD mobile requires Python 3.10 or newer")

sys.dont_write_bytecode = True

# The CLI supplies this installed file by absolute path. Never import helpers
# from the input application's directory or the caller's working directory.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from mobile.driver import main

sys.exit(main())
