"""Second-generation catch-local cleanup profiles for the shared PE32 runner."""
from __future__ import annotations

from types import SimpleNamespace

if __package__:
    from .check_windows_registration_multiple_catch import observe as observe_catches
    from .check_windows_registration_nested_try import validate_search_context
    from .replay_windows_registration_nested_try import validate_capture
    from .windows_registration_image import validate_generation
else:
    from check_windows_registration_multiple_catch import observe as observe_catches
    from check_windows_registration_nested_try import validate_search_context
    from replay_windows_registration_nested_try import validate_capture
    from windows_registration_image import validate_generation

CASES = tuple("catch-cleanup-" + mode + "-llvm-fixed" + suffix
              for mode in ("o0", "o1") for suffix in ("", "-control"))


def entry_context(case):
    if case not in CASES:
        raise ValueError("unknown generated catch cleanup profile")
    return 0, 0, b"callback_parent"


def validate_installation(original, product, receipt, first, case):
    _, _, export = entry_context(case)
    validate_search_context(first, case)
    validate_generation(original, product, receipt, first, export)
    actions = 2 if "-o0-" in case else 1
    if (receipt.get("entry_registers"), receipt.get("entry_pop")) != (0, 0) or \
            (receipt.get("cleanup_actions"), receipt.get("cleanup_calls")) != (actions, 2) or \
            (receipt.get("incoming_reads"), receipt.get("incoming_writes")) != (0, 0):
        raise ValueError("generated catch cleanup lost its ordered action or entry proof")


def observe(path, case, route, receipt, launcher, env, timeout, *, source_section=".ndtext"):
    entry_context(case)
    return observe_catches(path, case, route, receipt, launcher, env, timeout,
                           expected_cleanup=(0, 53, 0), source_section=source_section)


def profile():
    return SimpleNamespace(cases=CASES, first_capture="nested-try-rewrite.json",
                           evidence="relifted-catch-cleanup-reconstruction", calls=12,
                           entry_context=entry_context, observe=observe,
                           validate_first=validate_capture,
                           validate_installation=validate_installation)
