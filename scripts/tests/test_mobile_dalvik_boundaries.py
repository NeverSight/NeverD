"""Publication and declaration boundaries shared by builtin Android readers."""
from __future__ import annotations

import copy
from pathlib import Path
import struct
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/neverd"))

from mobile.android import decompile_android
from mobile.common import Limits, MobileError
from mobile.dalvik_dex import parse_dex
from mobile.dalvik_java import _constructor_throws, recover_java
from mobile.dalvik_model import Budget, Class, Field, FieldRef, Instruction, Method, MethodRef, link_classes
from mobile.dalvik_smali import parse_smali
from scripts.tests.test_mobile_dalvik_dex import fixture, seal


def read_classes(*sources, limits=None):
    budget = Budget(limits or Limits(timeout=30))
    classes = [parse_smali(textwrap.dedent(source), input_id=f"owned-{index}.smali", budget=budget)
               for index, source in enumerate(sources)]
    return classes, budget


def declaration(name, *, flags="public", parent="Ljava/lang/Object;", body=""):
    return f".class {flags} {name}\n.super {parent}\n" + body


class DalvikBoundaryTests(unittest.TestCase):
    def test_output_budget_is_cumulative_and_rejects_invalid_charges(self):
        budget = Budget(Limits(max_bytes=9))
        budget.output(4)
        budget.output(5)
        with self.assertRaisesRegex(MobileError, "output"):
            budget.output(1)
        for charge in (-1, 1.5, True):
            with self.subTest(charge=charge):
                with self.assertRaises(MobileError):
                    Budget(Limits()).output(charge)

    def assert_unpublished(self, sources, *, limits=None, coverage=None):
        with tempfile.TemporaryDirectory(prefix="neverd-android-boundaries-") as temporary:
            root = Path(temporary)
            inputs = root / "input"
            inputs.mkdir()
            for index, source in enumerate(sources):
                (inputs / f"Input{index}.smali").write_text(source, encoding="utf-8")
            output = root / "staged"
            with self.assertRaises(MobileError):
                if coverage is None:
                    decompile_android(inputs, output, limits=limits or Limits(timeout=30))
                else:
                    with patch("mobile.dalvik_java.recover_java", return_value=copy.deepcopy(coverage)):
                        decompile_android(inputs, output, limits=limits or Limits(timeout=30))
            self.assertFalse((output / "sources").exists(), "invalid output must fail before writing any source")
            self.assertFalse((output / "metadata").exists(), "invalid output must fail before writing metadata")

    def test_windows_device_names_from_class_descriptors_are_not_output_paths(self):
        for name in ("Lcon;", "Laux/Child;", "Lpkg/LPT1;"):
            with self.subTest(name=name):
                self.assert_unpublished([declaration("LA;"), declaration(name)])

    def test_parent_directory_case_collisions_are_rejected_before_any_write(self):
        self.assert_unpublished([declaration("Lpkg/First;"), declaration("LPkg/Second;")])

    def test_metadata_size_is_preflighted_with_sources_before_any_write(self):
        coverage = {"schema_version": 1, "status": "recovered", "class_count": 1,
                    "method_count": 0, "methods": [], "reason": "x" * 1100,
                    "source_units": [{"path": "A.java", "class": "LA;", "source": "class A {}\n"}]}
        self.assert_unpublished([declaration("LA;")], limits=Limits(max_bytes=1024), coverage=coverage)

    def test_implicit_output_directories_count_toward_the_file_budget(self):
        self.assert_unpublished([declaration("La/b/c/d/Leaf;")], limits=Limits(max_files=4))

    def test_source_growth_is_rejected_inside_generation(self):
        classes, budget = read_classes(declaration("LFrame;", body="""
            .method public static run()I
                .registers 1024
                const/4 v0, 7
                return v0
            .end method
        """), limits=Limits(max_bytes=1024))
        with self.assertRaisesRegex(MobileError, "output"):
            recover_java(link_classes(classes, budget), budget=budget)

    def test_class_and_interface_inheritance_cycles_are_rejected(self):
        cases = [
            [declaration("LA;", parent="LB;"), declaration("LB;", parent="LA;")],
            [declaration("LA;", flags="public abstract interface", body=".implements LB;\n"),
             declaration("LB;", flags="public abstract interface", body=".implements LA;\n")],
        ]
        for sources in cases:
            with self.subTest(sources=sources):
                classes, budget = read_classes(*sources)
                with self.assertRaisesRegex(MobileError, "cycl"):
                    link_classes(classes, budget)

    def test_known_parent_kinds_and_final_superclass_are_checked(self):
        cases = [
            [declaration("LA;", flags="public final"), declaration("LB;", parent="LA;")],
            [declaration("LA;", flags="public abstract interface"), declaration("LB;", parent="LA;")],
            [declaration("LA;"), declaration("LB;", body=".implements LA;\n")],
        ]
        for sources in cases:
            with self.subTest(sources=sources):
                classes, budget = read_classes(*sources)
                with self.assertRaises(MobileError):
                    link_classes(classes, budget)

    def test_concrete_class_cannot_declare_abstract_methods(self):
        classes, budget = read_classes(declaration("LA;", body="""
            .method public abstract run()I
            .end method
        """))
        with self.assertRaisesRegex(MobileError, "abstract"):
            link_classes(classes, budget)

    def test_invalid_class_field_and_method_flag_combinations_are_rejected(self):
        sources = [declaration("LA;", flags="public final abstract"),
                   declaration("LA;", flags="public final abstract interface"),
                   declaration("LA;", body=".field public final volatile count:I\n")]
        for source in sources:
            with self.subTest(source=source):
                classes, budget = read_classes(source)
                with self.assertRaises(MobileError):
                    link_classes(classes, budget)
        for flag in ("static", "private", "final", "native", "synchronized", "strictfp"):
            with self.subTest(method_flag=flag):
                classes, budget = read_classes(declaration("LA;", flags="public abstract", body=
                                                          ".method abstract run()I\n.end method\n"))
                classes[0].methods[0].access |= {flag}
                with self.assertRaises(MobileError):
                    link_classes(classes, budget)

    def test_dex_visibility_conflicts_use_the_shared_declaration_validator(self):
        data, offsets = fixture()
        malformed = bytearray(data)
        struct.pack_into("<I", malformed, offsets["class"] + 4, 0x3)  # public | private
        budget = Budget(Limits())
        classes = parse_dex(seal(malformed), input_id="owned.dex", budget=budget)
        with self.assertRaisesRegex(MobileError, "visibility"):
            link_classes(classes, budget)

    def test_compatible_abstract_interface_and_class_declarations_remain_usable(self):
        classes, budget = read_classes(
            declaration("LContract;", flags="public abstract interface", body="""
                .method public abstract run()I
                .end method
            """),
            declaration("LBase;", flags="public abstract", body="""
                .implements LContract;
                .method public abstract run()I
                .end method
            """),
            declaration("LChild;", parent="LBase;", body="""
                .method public run()I
                    .registers 2
                    const/4 v0, 7
                    return v0
                .end method
            """))
        linked = link_classes(classes, budget)
        self.assertEqual(set(linked), {"LContract;", "LBase;", "LChild;"})

    def test_nested_ownership_cycle_referenced_by_an_unrelated_field_is_rejected(self):
        visibility = frozenset({"public"})
        nested = frozenset({"public", "static"})
        first = Class("LFirst;", "Ljava/lang/Object;", visibility, "owned-first",
                      enclosing="LSecond;", inner_name="First", inner_access=nested)
        second = Class("LSecond;", "Ljava/lang/Object;", visibility, "owned-second",
                       enclosing="LFirst;", inner_name="Second", inner_access=nested)
        consumer = Class("LAConsumer;", "Ljava/lang/Object;", visibility, "owned-consumer",
                         fields=[Field(FieldRef("LAConsumer;", "value", first.name), visibility)])
        budget = Budget(Limits(timeout=30))
        with self.assertRaisesRegex(MobileError, "nested|ownership|cycl"):
            recover_java(link_classes([consumer, first, second], budget), budget=budget)

    def test_deep_nested_type_reference_uses_a_declared_depth_bound(self):
        visibility = frozenset({"public"})
        classes = []
        for index in range(1100):
            classes.append(Class(f"LLevel{index};", "Ljava/lang/Object;", visibility, "owned-nested",
                                 enclosing=f"LLevel{index - 1};" if index else None,
                                 inner_name=f"Level{index}" if index else None,
                                 inner_access=frozenset({"public", "static"}) if index else frozenset()))
        classes.append(Class("LAConsumer;", "Ljava/lang/Object;", visibility, "owned-consumer",
                             fields=[Field(FieldRef("LAConsumer;", "value", classes[-1].name), visibility)]))
        budget = Budget(Limits(timeout=30))
        with self.assertRaisesRegex(MobileError, "nested|depth"):
            recover_java(link_classes(classes, budget), budget=budget)

    def test_long_constructor_chain_preserves_external_checked_exception_dependency(self):
        visibility = frozenset({"public"})
        classes = []
        for index in range(1100):
            owner = f"LChain{index};"
            parent = f"LChain{index + 1};" if index < 1099 else "Ljava/io/FileInputStream;"
            ref = MethodRef(owner, "<init>", ("Ljava/lang/String;",), "V")
            target = MethodRef(parent, "<init>", ref.parameters, "V")
            constructor = Method(ref, visibility | {"constructor"}, 2,
                                 [Instruction(0, "invoke-direct", (0, 1), reference=target),
                                  Instruction(1, "return-void")], code_end=2)
            classes.append(Class(owner, parent, visibility, "owned-constructors", methods=[constructor]))
        budget = Budget(Limits(timeout=30))
        linked = link_classes(classes, budget)
        self.assertTrue(_constructor_throws(classes[0].methods[0], linked, budget),
                        "the chain must retain the external FileInputStream constructor dependency")


if __name__ == "__main__":
    unittest.main()
