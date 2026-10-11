"""Second-generation trivial PE32 objects, reference writes and rethrows."""
from __future__ import annotations

from types import SimpleNamespace

if __package__:
    from .check_windows_registration_multiple_catch import observe as observe_catches
    from .check_windows_registration_nested_try import search_context, validate_search_context
    from .replay_windows_registration_nested_try import validate_capture
    from .windows_registration_image import validate_generation
    from .windows_registration_objects import CASES
else:
    from check_windows_registration_multiple_catch import observe as observe_catches
    from check_windows_registration_nested_try import search_context, validate_search_context
    from replay_windows_registration_nested_try import validate_capture
    from windows_registration_image import validate_generation
    from windows_registration_objects import CASES


def entry_context(case):
    if case not in CASES:
        raise ValueError("unknown generated object profile")
    return 0, 0, b"callback_parent"


def validate_first(root, capture):
    if capture.get("profile") != "objects":
        raise ValueError("generated object evidence requires the original object proofs")
    return validate_capture(root, capture)


def validate_installation(original, product, receipt, first, case):
    _, _, export = entry_context(case)
    validate_search_context(first, case)
    validate_generation(original, product, receipt, first, export)
    fields = ("entry_registers", "entry_pop", "cleanup_actions", "cleanup_calls",
              "incoming_reads", "incoming_writes")
    if any(type(receipt.get(key)) is not int or receipt[key] != 0 for key in fields):
        raise ValueError("generated trivial object changed its entry or lifetime contract")


def observe(path, case, route, receipt, launcher, env, timeout, *, source_section=".ndtext"):
    entry_context(case)
    expected = ((39, 28, 39, 39, 18, 39, 1, 12) if search_context(case)["rethrow_search"]
                else (17, 28, 39, 7, 18, 39, 1, 12))
    return observe_catches(path, case, route, receipt, launcher, env, timeout,
                           expected_values=expected, source_section=source_section)


def profile():
    return SimpleNamespace(cases=CASES, first_capture="nested-try-rewrite.json",
                           evidence="relifted-object-reconstruction", calls=12,
                           entry_context=entry_context, observe=observe,
                           validate_first=validate_first,
                           validate_installation=validate_installation)
