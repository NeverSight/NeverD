import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from scripts.replay_windows_registration import main, source_cases, source_records
from scripts.check_windows_registration_rewrite import PARENT_EXIT_CASES


class RegistrationSourceReplayTests(unittest.TestCase):
    def test_older_compiler_profiles_do_not_claim_callback_state_execution(self):
        current, r4, legacy = (source_cases(p) for p in ("all", "r4", "seh3-eh4"))
        self.assertEqual(len(r4), 48)
        self.assertEqual(len(legacy), 39)
        r5 = source_cases("r5")
        self.assertEqual(len(r5), 64)
        eh4_jumps = {name for name in current
                     if name.startswith("eh4-") and "longjmp-" in name}
        self.assertEqual(len(current), 243)
        parent_exits = {prefix + case for prefix in ("", "eh4-", "eh4-gs-")
                        for case in PARENT_EXIT_CASES}
        self.assertEqual(len(parent_exits), 27)
        self.assertTrue(parent_exits <= set(current))
        self.assertEqual(len(eh4_jumps), 118)
        for prefix in ("eh4-", "eh4-gs-"):
            for case in ("longjmp-filter-correlated-state", "longjmp-filter-shared-state",
                         "longjmp-filter-captured-cursor", "longjmp-filter-negative-disposition",
                         "longjmp-filter-skipped-scope", "longjmp-filter-two-resume-mappings",
                         "longjmp-filter-callback-activation"):
                self.assertEqual(current[prefix + case], current[case])
            for case in ("longjmp-callee", "longjmp-returning-callee",
                         "longjmp-scalar-callee"):
                self.assertEqual(current[prefix + case], current[case])
            for kind in ("callee", "returning-callee", "scalar-callee"):
                self.assertEqual(current[prefix + "longjmp-eh4-" + kind],
                                 current["longjmp-" + kind])
                self.assertEqual(current[prefix + "longjmp-eh4-" + kind + "-mixed-gs"],
                                 current["longjmp-" + kind])
        self.assertEqual(set(current) - set(r5) - eh4_jumps - parent_exits, {
            "longjmp-shared-parent-call", "shared-parent-throwing-call",
            "longjmp-private-parent-call", "longjmp-private-parent-graph",
            "longjmp-multistate-capture", "longjmp-handler-selected",
        "longjmp-handler-cleanup", "longjmp-handler-captured-cursor",
        "longjmp-handler-nested-finally", "longjmp-handler-mixed-resumption",
        "longjmp-handler-cleanup-jump", "longjmp-handler-nested-cleanup-jump",
        "longjmp-handler-cleanup-value", "longjmp-handler-nested-cleanup-value",
        "longjmp-handler-cleanup-zero", "longjmp-handler-nested-cleanup-zero",
        "longjmp-handler-cleanup-dynamic-value",
        "longjmp-handler-nested-cleanup-dynamic-value",
        "longjmp-handler-cleanup-other-capture",
        "longjmp-handler-nested-cleanup-other-capture",
        "longjmp-search-filter-jump",
        "longjmp-nested-search-filter-jump",
        "longjmp-search-filter-other-capture",
        "longjmp-nested-search-filter-other-capture",
        "longjmp-search-filter-mixed-jump",
        "longjmp-nested-search-filter-mixed-jump",
        "longjmp-search-filter-selected-jump",
        "longjmp-nested-search-filter-selected-jump",
        "longjmp-finally-register-loop",
        "longjmp-finally-frame-loop",
        "longjmp-finally-private-loop",
        "longjmp-filter-register-loop",
        "longjmp-filter-frame-loop",
        "longjmp-filter-private-loop"})
        self.assertEqual(set(current) - set(r4) - eh4_jumps - parent_exits, {
            "longjmp-state-activate-call", "longjmp-state-activate-unwind",
            "longjmp-state-retire", "longjmp-state-sentinel",
            "longjmp-state-byte", "longjmp-state-word",
            "longjmp-filter-correlated-state", "longjmp-filter-shared-state",
            "longjmp-filter-captured-cursor",
            "longjmp-filter-negative-disposition",
            "longjmp-filter-skipped-scope",
            "longjmp-filter-two-resume-mappings",
            "longjmp-filter-callback-activation", "longjmp-state-byte-high-bits",
            "longjmp-finally-incidental-result", "longjmp-deep-scope-chain",
            "longjmp-shared-parent-call", "shared-parent-throwing-call",
            "longjmp-private-parent-call", "longjmp-private-parent-graph",
            "longjmp-multistate-capture", "longjmp-handler-selected",
        "longjmp-handler-cleanup", "longjmp-handler-captured-cursor",
        "longjmp-handler-nested-finally", "longjmp-handler-mixed-resumption",
        "longjmp-handler-cleanup-jump", "longjmp-handler-nested-cleanup-jump",
        "longjmp-handler-cleanup-value", "longjmp-handler-nested-cleanup-value",
        "longjmp-handler-cleanup-zero", "longjmp-handler-nested-cleanup-zero",
        "longjmp-handler-cleanup-dynamic-value",
        "longjmp-handler-nested-cleanup-dynamic-value",
        "longjmp-handler-cleanup-other-capture",
        "longjmp-handler-nested-cleanup-other-capture",
        "longjmp-search-filter-jump",
        "longjmp-nested-search-filter-jump",
        "longjmp-search-filter-other-capture",
        "longjmp-nested-search-filter-other-capture",
        "longjmp-search-filter-mixed-jump",
        "longjmp-nested-search-filter-mixed-jump",
        "longjmp-search-filter-selected-jump",
        "longjmp-nested-search-filter-selected-jump",
        "longjmp-finally-register-loop",
        "longjmp-finally-frame-loop",
        "longjmp-finally-private-loop",
        "longjmp-filter-register-loop",
        "longjmp-filter-frame-loop",
        "longjmp-filter-private-loop"})
        self.assertTrue(all(name.startswith("eh4-gs-") for name in set(r4) - set(legacy)))
        with self.assertRaises(ValueError):
            source_cases("unknown")

    def evidence(self, schema=2):
        labels = ["original", "patched", "product-patched", "collision-patched"]
        if schema == 2:
            labels += ["cli-section-ir", "cli-section-exe-no-opt", "cli-section-exe-opt",
                       "cli-inplace-ir", "cli-inplace-exe-no-opt", "cli-inplace-exe-opt"]
        else:
            labels += ["cli-section", "cli-inplace"]
        return {"schema": schema, "case": "longjmp-retired-inner-scope",
                "passed": True, "expected_trace": 23,
                "observations": [
                    {"image": label + suffix + ".exe", "generated": label != "original",
                     "runtime_base": base}
                    for label in labels
                    for suffix, base in (("", 0x400000), ("-rebased", 0x18000000))]}

    def test_current_and_legacy_execution_sets_are_explicit(self):
        for schema, count in ((1, 12), (2, 20)):
            with self.subTest(schema=schema):
                self.assertEqual(len(source_records(self.evidence(schema),
                                                    "longjmp-retired-inner-scope", 23)), count)

    def test_control_fp_mode_requires_its_authored_source_case(self):
        source = self.evidence()
        source["control_fp_mode"] = "precision-53"
        with self.assertRaises(ValueError):
            source_records(source, "longjmp-retired-inner-scope", 23)
        source["case"] = "longjmp-control-fp"
        self.assertEqual(len(source_records(source, "longjmp-control-fp", 23)), 20)
        source["control_fp_mode"] = "precision-128"
        with self.assertRaises(ValueError):
            source_records(source, "longjmp-control-fp", 23)
        source = self.evidence()
        source["control_fp_veneer"] = True
        with self.assertRaises(ValueError):
            source_records(source, "longjmp-retired-inner-scope", 23)
        source["case"] = "longjmp-control-fp"
        self.assertEqual(len(source_records(source, "longjmp-control-fp", 23)), 20)
        source["control_fp_veneer"] = "true"
        with self.assertRaises(ValueError):
            source_records(source, "longjmp-control-fp", 23)

    def test_replay_forwards_fp_mode_to_every_native_observation(self):
        # Check dispatch only. Actual control-word effects belong to the
        # independent authored native fixture, not this mocked unit test.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            parent = root / "source-longjmp-control-fp"
            parent.mkdir()
            source = self.evidence()
            source.update(case="longjmp-control-fp", control_fp_mode="precision-53")
            for record in source["observations"]:
                image = parent / record["image"]
                image.write_bytes(b"authored replay harness input")
                record["sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
            (parent / "registration-rewrite.json").write_text(json.dumps(source))

            def observation(image, *args):
                return {"runtime_base": 0x18000000 if "-rebased" in image.stem
                        else 0x400000}

            with mock.patch("scripts.replay_windows_registration.source_cases",
                            return_value={"longjmp-control-fp": (24, 23)}), \
                    mock.patch("scripts.replay_windows_registration.observe",
                               side_effect=observation) as observe:
                self.assertEqual(main(["--evidence-root", str(root), "--output",
                                       str(root / "replay.json"), "--source-only",
                                       "--wine", "unused-test-launcher"]), 0)
                self.assertEqual(observe.call_count, 20)
                self.assertTrue(all(call.args[-1] == "precision-53"
                                    for call in observe.call_args_list))

    def test_changed_or_incomplete_evidence_is_not_native_coverage(self):
        for mutation in range(9):
            with self.subTest(mutation=mutation):
                evidence = copy.deepcopy(self.evidence())
                if mutation == 0:
                    evidence["observations"].pop()
                elif mutation == 1:
                    evidence["observations"][-1] = evidence["observations"][0]
                elif mutation == 2:
                    evidence["observations"][0]["generated"] = True
                elif mutation == 3:
                    evidence["observations"][0]["runtime_base"] = 0x18000000
                elif mutation == 4:
                    evidence["observations"][0]["image"] = "unknown.exe"
                elif mutation == 5:
                    evidence["passed"] = False
                elif mutation == 6:
                    evidence["expected_trace"] = 0
                elif mutation == 7:
                    evidence["case"] = "longjmp-finally"
                else:
                    evidence["schema"] = 3
                with self.assertRaises(ValueError):
                    source_records(evidence, "longjmp-retired-inner-scope", 23)


if __name__ == "__main__":
    unittest.main()
