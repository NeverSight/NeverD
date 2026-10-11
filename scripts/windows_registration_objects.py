"""Trivial object source profile for the shared PE32 nested-search matrix."""
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_object_driver.cpp"
TYPES = ROOT / "unittests/lift/eh/fixtures/registration_object_types.h"
PROOF = ROOT / "unittests/lift/eh/RegistrationObjectTests.cpp"
FORMS = {prefix + "-" + mode + "-llvm-fixed": "-" + mode.upper()
         for prefix in ("nested", "rethrow", "direct-nested", "direct-rethrow")
         for mode in ("o0", "o1")}
CASES = tuple(name + suffix for name in FORMS for suffix in ("", "-control"))
TEST = "RegistrationObject.InputPE32BindsEveryObjectByte"


def profile(validate_nested):
    def validate(text, language, inline=False, direct=False, catch_try=False, cleanup_actions=0):
        validate_nested(text, language, inline, False, catch_try, cleanup_actions)
        # RTTI identifies the records; it does not disclose source fields or
        # constructors. The fixture's actual declarations supply syntax types.
        if language == "cpp" and ("ReferenceObject" not in text or
                                   ".Head" in text or ".Tail" in text or ".Tag" in text):
            raise ValueError("object decompilation lost RTTI or invented source fields")
        if direct and language == "cpp" and "throw (float)" not in text:
            raise ValueError("object profile lost its distinct scalar throw")
    return SimpleNamespace(source=SOURCE, forms=FORMS, cases=CASES,
                           types=TYPES, proof=PROOF, test=TEST,
                           validate_decompilation=validate)
