from __future__ import annotations

import json
import sys
import types
import unittest
from dataclasses import FrozenInstanceError
from unittest import mock


class PluginDecoratorTests(unittest.TestCase):
    def test_plugin_decorator_exports_validated_spec(self) -> None:
        module_name = "_neverd_plugin_test_valid_spec"
        module = types.ModuleType(module_name)
        sys.modules[module_name] = module
        try:
            exec(
                """
from neverd_plugin import Plugin

@Plugin(name="Example", version="1.2.3", author="Team")
class Example:
    pass
""",
                module.__dict__,
            )
            spec = module.__neverd_plugin__
            self.assertIs(spec.plugin_class, module.Example)
            self.assertEqual(spec.name, "Example")
            self.assertEqual(spec.version, "1.2.3")
            self.assertEqual(spec.version_info, (1, 2, 3))
            self.assertEqual(spec.author, "Team")
        finally:
            sys.modules.pop(module_name, None)

    def test_plugin_decorator_rejects_numeric_prerelease_with_leading_zero(
        self,
    ) -> None:
        from neverd_plugin import Plugin

        with self.assertRaisesRegex(ValueError, "strict SemVer"):
            Plugin(name="Example", version="1.2.3-01")

    def test_plugin_decorator_rejects_metadata_that_is_not_valid_utf8(self) -> None:
        from neverd_plugin import Plugin

        with self.assertRaisesRegex(ValueError, "UTF-8"):
            Plugin(name="bad\ud800name", version="1.0.0")

    def test_plugin_decorator_rejects_invalid_contract_values(self) -> None:
        from neverd_plugin import Plugin

        for version in ("1", "1.2", "01.2.3", "1.2.3-", "1.2.3+"):
            with self.subTest(version=version):
                with self.assertRaisesRegex(ValueError, "strict SemVer"):
                    Plugin(name="Example", version=version)
        with self.assertRaisesRegex(ValueError, "non-empty"):
            Plugin(name="", version="1.0.0")
        with self.assertRaisesRegex(ValueError, "NUL"):
            Plugin(name="bad\0name", version="1.0.0")
        with self.assertRaisesRegex(TypeError, "boolean"):
            Plugin(name="Example", version="1.0.0", type=True)
        with self.assertRaisesRegex(ValueError, "unknown plugin type"):
            Plugin(name="Example", version="1.0.0", type="unknown")

    def test_plugin_spec_is_immutable_and_module_allows_only_one(self) -> None:
        module_name = "_neverd_plugin_test_duplicate"
        module = types.ModuleType(module_name)
        sys.modules[module_name] = module
        try:
            exec(
                """
from neverd_plugin import Plugin

@Plugin(name="First", version="1.0.0")
class First:
    pass
""",
                module.__dict__,
            )
            with self.assertRaises(FrozenInstanceError):
                module.__neverd_plugin__.name = "Changed"
            with self.assertRaisesRegex(RuntimeError, "only one"):
                exec(
                    """
@Plugin(name="Second", version="1.0.0")
class Second:
    pass
""",
                    module.__dict__,
                )
        finally:
            sys.modules.pop(module_name, None)


class _FakeNativeBridge:
    def __init__(self) -> None:
        self.active = True

    def session_address(self, _handle: object) -> int:
        if not self.active:
            raise RuntimeError("NeverD session context is no longer active")
        return 0x1234


class _FailIfCalledHost:
    def owned_string(self, *_arguments: object) -> str:
        raise AssertionError("FFI must not run for a stale session")


class _RecordingHost:
    def __init__(self) -> None:
        self.calls: list[tuple[str, tuple[object, ...]]] = []

    def call(self, name: str, *arguments: object) -> object:
        self.calls.append((name, arguments))
        values = {
            "neverd_session_is_loaded": 1,
            "neverd_session_bitness": 64,
            "neverd_func_count": 3,
        }
        return values[name]

    def owned_string(self, name: str, *arguments: object) -> str:
        self.calls.append((name, arguments))
        values = {
            "neverd_session_file_path": "/tmp/input.bin",
            "neverd_session_arch_name": "x86_64",
            "neverd_project_name": "NeverD",
            "neverd_version_number": "3389.0.1",
        }
        return values[name]


class _StatusHost:
    def __init__(self) -> None:
        self.results = {
            "neverd_session_analyze": 1,
            "neverd_annotations_save": 0,
            "neverd_annotations_load": 0,
            "neverd_rename_func": 0,
        }

    def call(self, name: str, *_arguments: object) -> object:
        return self.results[name]

    def owned_string(self, name: str, *_arguments: object) -> str:
        if name == "neverd_last_error":
            return "host failure"
        raise AssertionError(name)


class _BorrowingHost:
    def __init__(self) -> None:
        self.calls: list[tuple[str, tuple[object, ...]]] = []

    def borrowed_bytes(self, name: str, *arguments: object) -> bytes:
        self.calls.append((name, arguments))
        return b"\x01\x02"


class _SymbolicHost:
    def __init__(self) -> None:
        self.options: object | None = None

    def owned_string(self, name: str, *arguments: object) -> str:
        if name != "neverd_symbolic_explore_json":
            raise AssertionError(name)
        self.options = arguments[2]._obj
        return json.dumps(
            {
                "ok": True,
                "function": "dispatch",
                "entry": "0x401000",
                "liftComplete": True,
                "complete": True,
                "exact": True,
                "reachablePaths": 1,
                "reportedPaths": 1,
                "executedSteps": 7,
                "unmodelledOps": 0,
                "paths": [
                    {
                        "outcome": "returned",
                        "block": 2,
                        "blocks": [0, 2],
                        "constraints": 1,
                        "unmodelledOps": 0,
                        "predicate": "x == 1",
                    }
                ],
            }
        )


class SessionTests(unittest.TestCase):
    def test_default_host_is_lazy_and_cached(self) -> None:
        import neverd_plugin.api as api_module
        from neverd_plugin import Session

        host = _RecordingHost()
        with mock.patch.object(api_module, "HostAPI", return_value=host) as factory:
            session = Session(object(), _native=_FakeNativeBridge())

            factory.assert_not_called()
            self.assertTrue(session.loaded)
            self.assertEqual(session.project_name, "NeverD")
            factory.assert_called_once_with()

    def test_stale_default_session_is_rejected_before_host_loading(self) -> None:
        import neverd_plugin.api as api_module
        from neverd_plugin import Session

        native = _FakeNativeBridge()
        with mock.patch.object(api_module, "HostAPI") as factory:
            session = Session(object(), _native=native)
            native.active = False

            with self.assertRaisesRegex(RuntimeError, "no longer active"):
                _ = session.file_path
            factory.assert_not_called()

    def test_raw_borrowed_buffers_offer_global_and_lifetime_checked_calls(
        self,
    ) -> None:
        from neverd_plugin import Session

        host = _BorrowingHost()
        session = Session(object(), _native=_FakeNativeBridge(), _host=host)

        self.assertEqual(session.raw.borrowed_bytes("global", 7), b"\x01\x02")
        self.assertEqual(
            session.raw.session_borrowed_bytes("neverd_roundtrip_obj"),
            b"\x01\x02",
        )
        self.assertEqual(host.calls[0], ("global", (7,)))
        self.assertEqual(host.calls[1][0], "neverd_roundtrip_obj")
        self.assertEqual(host.calls[1][1][0].value, 0x1234)

    def test_stale_session_is_rejected_before_ffi_access(self) -> None:
        from neverd_plugin import Session

        native = _FakeNativeBridge()
        session = Session(object(), _native=native, _host=_FailIfCalledHost())
        native.active = False

        with self.assertRaisesRegex(RuntimeError, "no longer active"):
            _ = session.file_path

    def test_typed_session_queries_revalidate_the_native_handle(self) -> None:
        from neverd_plugin import Session

        native = _FakeNativeBridge()
        host = _RecordingHost()
        session = Session(object(), _native=native, _host=host)

        self.assertTrue(session.loaded)
        self.assertEqual(session.file_path, "/tmp/input.bin")
        self.assertEqual(session.architecture, "x86_64")
        self.assertEqual(session.bitness, 64)
        self.assertEqual(session.function_count, 3)
        self.assertEqual(session.project_name, "NeverD")
        self.assertEqual(session.version, "3389.0.1")
        pointer_arguments = [
            arguments[0]
            for name, arguments in host.calls
            if name not in {"neverd_project_name", "neverd_version_number"}
        ]
        self.assertEqual(len(pointer_arguments), 5)
        self.assertTrue(all(argument.value == 0x1234 for argument in pointer_arguments))

    def test_session_respects_boolean_and_zero_success_conventions(self) -> None:
        from neverd_plugin import NeverDError, Session

        host = _StatusHost()
        session = Session(object(), _native=_FakeNativeBridge(), _host=host)
        session.analyze()
        session.save_annotations()
        session.load_annotations()
        session.rename_function("old", "new")

        host.results["neverd_annotations_save"] = 1
        with self.assertRaisesRegex(NeverDError, "host failure"):
            session.save_annotations()
        host.results["neverd_session_analyze"] = 0
        with self.assertRaisesRegex(NeverDError, "host failure"):
            session.analyze()

    def test_symbolic_exploration_is_typed_and_passes_resource_limits(self) -> None:
        from neverd_plugin import Session, SymbolicExploration

        host = _SymbolicHost()
        session = Session(object(), _native=_FakeNativeBridge(), _host=host)
        result = session.symbolic_explore(
            0x401000,
            max_paths=9,
            max_steps=100,
            max_block_visits=4,
            include_expressions=True,
        )

        self.assertIsInstance(result, SymbolicExploration)
        self.assertEqual(result.entry, 0x401000)
        self.assertTrue(result.exact)
        self.assertEqual(result.executed_steps, 7)
        self.assertEqual(result.paths[0].blocks, (0, 2))
        self.assertEqual(result.paths[0].predicate, "x == 1")
        self.assertEqual(host.options.max_paths, 9)
        self.assertEqual(host.options.max_steps, 100)
        self.assertEqual(host.options.max_block_visits, 4)
        self.assertEqual(host.options.include_expressions, 1)


class EventTests(unittest.TestCase):
    def test_all_event_variants_copy_only_their_relevant_payload(self) -> None:
        from neverd_plugin import Event, EventType, Session

        session = Session(
            object(), _native=_FakeNativeBridge(), _host=_FailIfCalledHost()
        )
        loaded = Event.from_host(session, EventType.BINARY_LOADED, path="/tmp/a.bin")
        selected = Event.from_host(
            session,
            EventType.FUNCTION_SELECTED,
            address=0x401000,
            name="main",
        )
        changed = Event.from_host(session, EventType.ADDRESS_CHANGED, address=0x402000)
        closing = Event.from_host(session, EventType.BINARY_CLOSING)
        analyzed = Event.from_host(session, EventType.ANALYSIS_DONE)
        patched = Event.from_host(
            session,
            EventType.PATCH_APPLIED,
            output_path="/tmp/patched.bin",
            code_size=17,
        )

        self.assertEqual(loaded.path, "/tmp/a.bin")
        self.assertEqual((selected.address, selected.name), (0x401000, "main"))
        self.assertEqual(changed.address, 0x402000)
        self.assertIsNone(closing.path)
        self.assertIsNone(analyzed.address)
        self.assertEqual(
            (patched.output_path, patched.code_size), ("/tmp/patched.bin", 17)
        )
        with self.assertRaisesRegex(ValueError, "requires path"):
            Event.from_host(session, EventType.BINARY_LOADED)


class _RecordingSimplifyHost:
    """A library that records the request and answers with fixed fields."""

    def __init__(self, status: int = 0, **fields: object) -> None:
        self.status = status
        self.fields = fields
        self.options: object | None = None
        self.expression: bytes | None = None
        self.disposals = 0

    def call(self, name: str, *arguments: object) -> object:
        if name == "neverd_simplify_expr":
            self.expression = arguments[0]
            # byref keeps the original struct reachable, which is what lets a
            # stand-in fill it the way the library would.
            self.options = arguments[1]._obj
            result = arguments[2]._obj
            for key, value in self.fields.items():
                setattr(result, key, value)
            return self.status
        if name == "neverd_simplify_result_dispose":
            self.disposals += 1
            return None
        raise AssertionError(f"unexpected call {name}")


class SimplifyExpressionTests(unittest.TestCase):
    def test_reports_every_field_of_a_rewrite(self) -> None:
        from neverd_plugin import SimplifyEvidence, SimplifyOutcome
        from neverd_plugin import simplify_expression

        host = _RecordingSimplifyHost(
            ok=1,
            input=b"(x ^ y) + 2 * (x & y)",
            output=b"x + y",
            changed=1,
            cost_before=11,
            cost_after=5,
            inputs=2,
            work=21,
            outcome=int(SimplifyOutcome.REWRITTEN),
            evidence=int(SimplifyEvidence.DERIVATION),
        )
        result = simplify_expression("(x ^ y) + 2 * (x & y)", host=host)

        self.assertEqual(result.output, "x + y")
        self.assertTrue(result.changed)
        self.assertEqual(result.saved, 6)
        self.assertEqual(result.work, 21)
        self.assertIs(result.outcome, SimplifyOutcome.REWRITTEN)
        self.assertIs(result.evidence, SimplifyEvidence.DERIVATION)
        self.assertEqual(host.disposals, 1)

    def test_carries_the_reason_an_expression_was_left_alone(self) -> None:
        from neverd_plugin import SimplifyOutcome, simplify_expression

        host = _RecordingSimplifyHost(
            ok=1,
            input=b"x + y + z",
            output=b"x + y + z",
            changed=0,
            outcome=int(SimplifyOutcome.TOO_MANY_INPUTS),
        )
        result = simplify_expression("x + y + z", host=host, max_atoms=2)

        self.assertFalse(result.changed)
        self.assertIs(result.outcome, SimplifyOutcome.TOO_MANY_INPUTS)
        self.assertEqual(host.options.max_atoms, 2)

    def test_translates_the_policy_the_engine_reads(self) -> None:
        import ctypes

        from neverd_plugin import simplify_expression
        from neverd_plugin.abi import NeverDSimplifyOptions

        host = _RecordingSimplifyHost(ok=1, input=b"x", output=b"x")
        simplify_expression(
            "x", host=host, width=256, deep=False, exhaustive=True
        )

        options = host.options
        self.assertEqual(options.struct_size, ctypes.sizeof(NeverDSimplifyOptions))
        self.assertEqual(options.width, 256)
        # `deep` is the useful default, so the flag crossing the ABI is its
        # negation: a zeroed struct has to mean the layered walk.
        self.assertEqual(options.shallow, 1)
        self.assertEqual(options.max_work, ctypes.c_size_t(-1).value)
        self.assertEqual(options.exhaustive, 1)

    def test_raises_with_the_offset_when_the_expression_will_not_read(self) -> None:
        from neverd_plugin import ExpressionSyntaxError, simplify_expression

        host = _RecordingSimplifyHost(ok=0, error=b"expected ')'", error_offset=6)
        with self.assertRaises(ExpressionSyntaxError) as raised:
            simplify_expression("(x + ", host=host)

        self.assertEqual(raised.exception.offset, 6)
        # The library owns the strings on the error path too, so releasing them
        # cannot be conditional on having got an answer.
        self.assertEqual(host.disposals, 1)

    def test_releases_the_result_even_when_the_call_is_refused(self) -> None:
        from neverd_plugin import NeverDError, simplify_expression

        host = _RecordingSimplifyHost(status=1)
        with self.assertRaises(NeverDError):
            simplify_expression("x", host=host)
        self.assertEqual(host.disposals, 1)


class _RecordingSynthesisHost:
    def __init__(self, status: int = 0, **fields: object) -> None:
        self.status = status
        self.fields = fields
        self.options: object | None = None
        self.expression: bytes | None = None
        self.disposals = 0

    def call(self, name: str, *arguments: object) -> object:
        if name == "neverd_synthesize_expr":
            self.expression = arguments[0]
            self.options = arguments[1]._obj
            result = arguments[2]._obj
            for key, value in self.fields.items():
                setattr(result, key, value)
            return self.status
        if name == "neverd_synthesize_result_dispose":
            self.disposals += 1
            return None
        raise AssertionError(f"unexpected call {name}")


class SynthesizeExpressionTests(unittest.TestCase):
    def test_reports_proof_and_search_telemetry(self) -> None:
        from neverd_plugin import ProofStatus, SynthesisOutcome
        from neverd_plugin import synthesize_expression

        host = _RecordingSynthesisHost(
            ok=1,
            input=b"(x >> 4) + ((x >> 2) >> 2)",
            output=b"x >> 3",
            changed=1,
            cost_before=9,
            cost_after=3,
            inputs=1,
            candidate_cost=3,
            outcome=int(SynthesisOutcome.REWRITTEN),
            proof_status=int(ProofStatus.EQUIVALENT),
            search_work=80,
            proof_queries=1,
            proof_conflicts=7,
            proof_propagations=11,
            proof_watch_visits=19,
        )
        result = synthesize_expression(
            "(x >> 4) + ((x >> 2) >> 2)",
            host=host,
            width=64,
            exhaustive=True,
            solver_max_conflicts=123,
        )

        self.assertEqual(result.output, "x >> 3")
        self.assertEqual(result.saved, 6)
        self.assertIs(result.outcome, SynthesisOutcome.REWRITTEN)
        self.assertIs(result.proof_status, ProofStatus.EQUIVALENT)
        self.assertEqual(result.proof_watch_visits, 19)
        self.assertEqual(host.options.width, 64)
        self.assertEqual(host.options.exhaustive, 1)
        self.assertEqual(host.options.solver_max_conflicts, 123)
        self.assertEqual(host.disposals, 1)

    def test_decodes_a_solver_counterexample(self) -> None:
        from neverd_plugin import ProofStatus, SynthesisOutcome
        from neverd_plugin import synthesize_expression

        host = _RecordingSynthesisHost(
            ok=1,
            input=b"x",
            output=b"x",
            outcome=int(SynthesisOutcome.COUNTEREXAMPLE),
            proof_status=int(ProofStatus.DIFFERENT),
            counterexample_json=b'{"x":"0x2a"}',
        )
        result = synthesize_expression("x", host=host)

        self.assertEqual(result.counterexample, {"x": "0x2a"})
        self.assertEqual(host.disposals, 1)

    def test_preserves_an_invalid_proof_disposition(self) -> None:
        from neverd_plugin import ProofStatus, SynthesisOutcome
        from neverd_plugin import synthesize_expression

        host = _RecordingSynthesisHost(
            ok=1,
            input=b"x + 0",
            output=b"x + 0",
            changed=0,
            outcome=int(SynthesisOutcome.PROOF_INCOMPLETE),
            proof_status=int(ProofStatus.INVALID),
        )
        result = synthesize_expression("x + 0", host=host)

        self.assertFalse(result.changed)
        self.assertIs(result.outcome, SynthesisOutcome.PROOF_INCOMPLETE)
        self.assertIs(result.proof_status, ProofStatus.INVALID)
        self.assertEqual(host.disposals, 1)

    def test_parse_errors_and_refusals_still_dispose(self) -> None:
        from neverd_plugin import ExpressionSyntaxError, NeverDError
        from neverd_plugin import synthesize_expression

        parse_host = _RecordingSynthesisHost(
            ok=0, error=b"expected expression", error_offset=4
        )
        with self.assertRaises(ExpressionSyntaxError) as raised:
            synthesize_expression("(x +", host=parse_host)
        self.assertEqual(raised.exception.offset, 4)
        self.assertEqual(parse_host.disposals, 1)

        refused_host = _RecordingSynthesisHost(status=1)
        with self.assertRaises(NeverDError):
            synthesize_expression("x", host=refused_host)
        self.assertEqual(refused_host.disposals, 1)


class _RecordingOptimizeHost:
    def __init__(self, status: int = 0, **fields: object) -> None:
        self.status = status
        self.fields = fields
        self.options: object | None = None
        self.ir: bytes | None = None
        self.disposals = 0

    def call(self, name: str, *arguments: object) -> object:
        if name == "neverd_optimize_llvm_ir":
            self.ir = arguments[0]
            self.options = arguments[1]._obj
            result = arguments[2]._obj
            for key, value in self.fields.items():
                setattr(result, key, value)
            return self.status
        if name == "neverd_optimize_llvm_ir_result_dispose":
            self.disposals += 1
            return None
        raise AssertionError(f"unexpected call {name}")


class OptimizeLLVMIRTests(unittest.TestCase):
    def test_rejects_conservative_synthesis_before_native_call(self) -> None:
        from neverd_plugin import OptimizationMode, optimize_llvm_ir

        host = _RecordingOptimizeHost()
        with self.assertRaisesRegex(ValueError, "conservative"):
            optimize_llvm_ir(
                "define void @f() { ret void }",
                host=host,
                mode=OptimizationMode.CONSERVATIVE,
                enable_synthesis=True,
            )
        self.assertIsNone(host.options)
        self.assertEqual(host.disposals, 0)

    def test_rejects_synthesis_policy_without_synthesis(self) -> None:
        from neverd_plugin import optimize_llvm_ir

        host = _RecordingOptimizeHost()
        with self.assertRaisesRegex(ValueError, "enable_synthesis"):
            optimize_llvm_ir(
                "define void @f() { ret void }",
                host=host,
                solver_max_conflicts=1,
            )
        self.assertIsNone(host.options)
        self.assertEqual(host.disposals, 0)

    def test_reports_transaction_and_semantic_telemetry(self) -> None:
        from neverd_plugin import LLVMOptimizationLevel, OptimizationMode
        from neverd_plugin import OptimizationStop, optimize_llvm_ir

        host = _RecordingOptimizeHost(
            ok=1,
            output_ir=b"define i32 @f() { ret i32 42 }\n",
            changed=1,
            stop=int(OptimizationStop.STABLE),
            functions_visited=2,
            rounds=4,
            semantic_rewrites=3,
            search_work=101,
            proof_queries=2,
            proof_conflicts=5,
            proof_propagations=8,
            proof_watch_visits=13,
        )
        result = optimize_llvm_ir(
            "define i32 @f() { ret i32 40 }",
            host=host,
            mode=OptimizationMode.DEEP,
            llvm_level=LLVMOptimizationLevel.O3,
            max_rounds=9,
            enable_synthesis=True,
            exhaustive=True,
        )

        self.assertIn("ret i32 42", result.output_ir)
        self.assertIs(result.stop, OptimizationStop.STABLE)
        self.assertEqual(result.semantic_rewrites, 3)
        self.assertEqual(result.proof_watch_visits, 13)
        self.assertEqual(host.options.mode, int(OptimizationMode.DEEP))
        self.assertEqual(host.options.llvm_level, int(LLVMOptimizationLevel.O3))
        self.assertEqual(host.options.max_rounds, 9)
        self.assertEqual(host.options.enable_synthesis, 1)
        self.assertEqual(host.options.exhaustive, 1)
        self.assertEqual(host.disposals, 1)

    def test_preserves_unsigned_64_bit_telemetry(self) -> None:
        from neverd_plugin import OptimizationStop, optimize_llvm_ir

        maximum = (1 << 64) - 1
        host = _RecordingOptimizeHost(
            ok=1,
            output_ir=b"define void @f() { ret void }\n",
            stop=int(OptimizationStop.STABLE),
            functions_visited=maximum,
            semantic_rewrites=maximum,
            search_work=maximum,
            proof_queries=maximum,
            proof_conflicts=maximum,
            proof_propagations=maximum,
            proof_watch_visits=maximum,
        )
        result = optimize_llvm_ir("define void @f() { ret void }", host=host)

        self.assertEqual(result.functions_visited, maximum)
        self.assertEqual(result.semantic_rewrites, maximum)
        self.assertEqual(result.search_work, maximum)
        self.assertEqual(result.proof_queries, maximum)
        self.assertEqual(result.proof_conflicts, maximum)
        self.assertEqual(result.proof_propagations, maximum)
        self.assertEqual(result.proof_watch_visits, maximum)
        self.assertEqual(host.disposals, 1)

    def test_parse_error_preserves_line_and_column(self) -> None:
        from neverd_plugin import LLVMIRSyntaxError, OptimizationStop
        from neverd_plugin import optimize_llvm_ir

        host = _RecordingOptimizeHost(
            ok=0,
            error=b"expected type",
            error_line=7,
            error_column=13,
            stop=int(OptimizationStop.INPUT_INVALID),
        )
        with self.assertRaises(LLVMIRSyntaxError) as raised:
            optimize_llvm_ir("broken", host=host)

        self.assertEqual(raised.exception.line, 7)
        self.assertEqual(raised.exception.column, 13)
        self.assertEqual(host.disposals, 1)

    def test_input_validation_is_not_misclassified_as_syntax(self) -> None:
        from neverd_plugin import LLVMIRSyntaxError, NeverDError
        from neverd_plugin import OptimizationStop, optimize_llvm_ir

        host = _RecordingOptimizeHost(
            ok=0,
            error=b"input module failed optimization validation",
            stop=int(OptimizationStop.INPUT_INVALID),
        )
        with self.assertRaises(NeverDError) as raised:
            optimize_llvm_ir("define void @f() { ret void }", host=host)

        self.assertNotIsInstance(raised.exception, LLVMIRSyntaxError)
        self.assertIn("validation", str(raised.exception))
        self.assertEqual(host.disposals, 1)

    def test_success_without_committed_ir_fails_closed(self) -> None:
        from neverd_plugin import NeverDError, OptimizationStop, optimize_llvm_ir

        host = _RecordingOptimizeHost(
            ok=1,
            stop=int(OptimizationStop.STABLE),
            output_ir=None,
        )
        with self.assertRaisesRegex(NeverDError, "committed LLVM IR"):
            optimize_llvm_ir("define void @f() { ret void }", host=host)
        self.assertEqual(host.disposals, 1)

    def test_rejected_transaction_and_call_refusal_still_dispose(self) -> None:
        from neverd_plugin import NeverDError, OptimizationStop
        from neverd_plugin import optimize_llvm_ir

        rejected = _RecordingOptimizeHost(
            ok=0,
            error=b"verification failed",
            stop=int(OptimizationStop.VERIFICATION_FAILED),
        )
        with self.assertRaisesRegex(NeverDError, "verification failed"):
            optimize_llvm_ir("define void @f() { ret void }", host=rejected)
        self.assertEqual(rejected.disposals, 1)

        refused = _RecordingOptimizeHost(status=1)
        with self.assertRaises(NeverDError):
            optimize_llvm_ir("define void @f() { ret void }", host=refused)
        self.assertEqual(refused.disposals, 1)

if __name__ == "__main__":
    unittest.main()
