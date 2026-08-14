from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ROOT_CMAKE = ROOT / "CMakeLists.txt"
SYMBOLIC_CMAKE = ROOT / "lib" / "symbolic" / "CMakeLists.txt"
SOLVER_CMAKE = ROOT / "lib" / "solver" / "CMakeLists.txt"
PASS_IR_CMAKE = ROOT / "lib" / "pass" / "ir" / "CMakeLists.txt"
SDK_CMAKE = ROOT / "lib" / "sdk" / "CMakeLists.txt"


def _without_comments(source: str) -> str:
    lines: list[str] = []
    for line in source.splitlines():
        quoted = False
        escaped = False
        kept: list[str] = []
        for character in line:
            if escaped:
                kept.append(character)
                escaped = False
                continue
            if character == "\\":
                kept.append(character)
                escaped = True
                continue
            if character == '"':
                quoted = not quoted
                kept.append(character)
                continue
            if character == "#" and not quoted:
                break
            kept.append(character)
        lines.append("".join(kept))
    return "\n".join(lines)


def _calls(path: Path, command: str, target: str) -> list[list[str]]:
    source = _without_comments(path.read_text(encoding="utf-8"))
    pattern = re.compile(
        rf"\b{re.escape(command)}\s*\(\s*{re.escape(target)}\b"
    )
    calls: list[list[str]] = []
    for match in pattern.finditer(source):
        opening = source.find("(", match.start())
        depth = 0
        quoted = False
        escaped = False
        end = None
        for index in range(opening, len(source)):
            character = source[index]
            if escaped:
                escaped = False
                continue
            if character == "\\":
                escaped = True
                continue
            if character == '"':
                quoted = not quoted
                continue
            if quoted:
                continue
            if character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    end = index
                    break
        if end is None:
            raise AssertionError(f"unterminated {command} call in {path}")
        calls.append(re.findall(r'"(?:\\.|[^"\\])*"|\S+', source[opening + 1 : end]))
    return calls


def component_links(path: Path, target: str) -> set[str]:
    calls = _calls(path, "add_neverd_component_library", target)
    if len(calls) != 1:
        raise AssertionError(f"expected one component declaration for {target}")
    tokens = calls[0]
    if "LINK_COMPONENTS" not in tokens:
        return set()
    begin = tokens.index("LINK_COMPONENTS") + 1
    end = (
        tokens.index("LINK_LIBS", begin)
        if "LINK_LIBS" in tokens[begin:]
        else len(tokens)
    )
    return set(tokens[begin:end])


def component_link_libraries(path: Path, target: str) -> set[str]:
    calls = _calls(path, "add_neverd_component_library", target)
    if len(calls) != 1:
        raise AssertionError(f"expected one component declaration for {target}")
    tokens = calls[0]
    if "LINK_LIBS" not in tokens:
        return set()
    return set(tokens[tokens.index("LINK_LIBS") + 1 :])


def target_link_visibility(path: Path, target: str) -> set[tuple[str, str]]:
    links: set[tuple[str, str]] = set()
    for tokens in _calls(path, "target_link_libraries", target):
        visibility = ""
        for token in tokens[1:]:
            if token in {"PRIVATE", "PUBLIC", "INTERFACE"}:
                visibility = token
            elif visibility:
                links.add((token.strip('"'), visibility))
    return links


class ComponentDependencyTests(unittest.TestCase):
    def test_parent_owns_integrated_llvm_generated_header_order(self) -> None:
        calls = _calls(ROOT_CMAKE, "add_dependencies", "LLVMMC")
        self.assertEqual(len(calls), 1)
        self.assertIn("llvm_vcsrevision_h", calls[0][1:])

    def test_solver_owns_the_symbolic_proof_adapter_dependency(self) -> None:
        symbolic = component_links(SYMBOLIC_CMAKE, "NeverDSymbolic")
        symbolic_libraries = component_link_libraries(
            SYMBOLIC_CMAKE, "NeverDSymbolic"
        )
        solver = component_links(SOLVER_CMAKE, "NeverDSolver")

        self.assertNotIn("Solver", symbolic)
        self.assertNotIn("NeverDSolver", symbolic_libraries)
        self.assertIn("Symbolic", solver)

    def test_ir_pass_depends_on_both_semantic_components(self) -> None:
        links = component_links(PASS_IR_CMAKE, "NeverDPassIR")
        self.assertIn("Symbolic", links)
        self.assertIn("Solver", links)

    def test_shared_sdk_keeps_solver_as_a_private_dependency(self) -> None:
        links = target_link_visibility(SDK_CMAKE, "neverd_shared")
        self.assertIn(("NeverDSolver", "PRIVATE"), links)
        self.assertNotIn(("NeverDSolver", "PUBLIC"), links)
        self.assertNotIn(("NeverDSolver", "INTERFACE"), links)


if __name__ == "__main__":
    unittest.main()
