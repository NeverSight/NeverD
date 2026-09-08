"""Independent smali reader semantics and rejection boundaries."""
from __future__ import annotations

from pathlib import Path
import sys
import textwrap
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/neverd"))

from mobile.common import Limits, MobileError
from mobile.dalvik_model import Budget, FieldRef, MethodRef, TryRegion, link_classes
from mobile.dalvik_smali import parse_smali


def parse(text, budget=None):
    return parse_smali(textwrap.dedent(text), input_id="fixture.smali", budget=budget or Budget(Limits()))


def method(body, declaration="public static run()V"):
    return parse(".class public LTest;\n.super Ljava/lang/Object;\n.method " + declaration + "\n" + textwrap.dedent(body) + "\n.end method\n").methods[0]


class SmaliReaderTests(unittest.TestCase):
    def test_existing_corpus_has_exact_references_and_control_flow(self):
        classes = [parse_smali(path.read_text(), input_id=path.name, budget=Budget(Limits()))
                   for path in sorted((ROOT / "scripts/tests/fixtures/mobile").glob("*.smali"))]
        linked = link_classes(classes, Budget(Limits()))
        self.assertEqual(set(linked), {"Lfixture/Calculator;", "Lfixture/Calculator$Nested;", "Lfixture/Peer;"})
        calculator = linked["Lfixture/Calculator;"]
        compute, loop, divide = calculator.methods
        self.assertEqual(compute.instructions[0].registers, (1,))
        self.assertEqual(compute.instructions[0].reference, MethodRef("Lfixture/Peer;", "twice", ("I",), "I"))
        self.assertEqual([i.target for i in loop.instructions if i.target is not None], [10, 7, 3])
        self.assertEqual(divide.tries, [TryRegion(0, 1, (("Ljava/lang/ArithmeticException;", 2),))])
        self.assertEqual(sum(len(c.methods) for c in classes), 6)
        self.assertEqual(sum(len(m.instructions) for c in classes for m in c.methods), 27)
        nested = linked["Lfixture/Calculator$Nested;"]
        self.assertEqual((nested.enclosing, nested.inner_name, nested.inner_access),
                         ("Lfixture/Calculator;", "Nested", frozenset({"public", "static"})))

    def test_locals_and_registers_use_absolute_word_aliases(self):
        code = """
            REGISTER_DIRECTIVE
            move-wide v0, p2
            invoke-static {p1, p2, p3, p4, p5}, LOther;->accept(IJD)V
            return-void
        """
        first = method(code.replace("REGISTER_DIRECTIVE", ".locals 2"), "public run(IJD)V")
        second = method(code.replace("REGISTER_DIRECTIVE", ".registers 8"), "public run(IJD)V")
        self.assertEqual(first.registers, 8)
        self.assertEqual(first.incoming_words, 6)
        self.assertEqual(first.instructions, second.instructions)
        self.assertEqual(first.instructions[0].registers, (0, 4))
        self.assertEqual(first.instructions[1].registers, (3, 4, 5, 6, 7))

    def test_zero_frame_constructor_and_bodyless_declarations(self):
        self.assertEqual(method(".registers 0\nreturn-void").registers, 0)
        constructor = method(".locals 0\ninvoke-direct {p0}, Ljava/lang/Object;-><init>()V\nreturn-void",
                             "public constructor <init>()V")
        self.assertEqual(constructor.instructions[0].reference.name, "<init>")
        for flags in ("public abstract", "public native"):
            abstract = method("", flags + " run()I")
            self.assertEqual((abstract.registers, abstract.instructions, abstract.code_end), (0, [], 0))

    def test_common_instruction_formats_preserve_typed_operands(self):
        rows = [
            ("move/from16 v1, v299", (1, 299), None),
            ("move-wide/16 v298, v296", (298, 296), None),
            ("neg-long v0, v2", (0, 2), None),
            ("int-to-double v0, v2", (0, 2), None),
            ("double-to-int v0, v2", (0, 2), None),
            ("cmp-long v0, v2, v4", (0, 2, 4), None),
            ("aget-wide v0, v2, v3", (0, 2, 3), None),
            ("iput-object v0, v1, LTest;->name:Ljava/lang/String;", (0, 1), FieldRef("LTest;", "name", "Ljava/lang/String;")),
            ("sget-wide v0, LTest;->count:J", (0,), FieldRef("LTest;", "count", "J")),
            ("new-array v0, v1, [[I", (0, 1), "[[I"),
            ("instance-of v0, v1, [I", (0, 1), "[I"),
            ("check-cast v0, LTest;", (0,), "LTest;"),
            ("const-method-type v0, (IJ)D", (0,), "(IJ)D"),
            ("add-long/2addr v0, v2", (0, 2), None),
            ("ushr-long v0, v2, v299", (0, 2, 299), None),
        ]
        for text, registers, reference in rows:
            # The 23x shift count is itself limited to eight register bits.
            text = text.replace("v299", "v200") if text.startswith("ushr-") else text
            registers = (0, 2, 200) if text.startswith("ushr-") else registers
            with self.subTest(text=text):
                instruction = method(".registers 300\n" + text + "\nreturn-void").instructions[0]
                self.assertEqual(instruction.opcode, text.split()[0])
                self.assertEqual(instruction.registers, registers)
                self.assertEqual(instruction.reference, reference)

    def test_constants_keep_word_bits_strings_characters_and_suffix_signs(self):
        rows = [
            ("const/4 v0, -0x1", -1), ("const/16 v0, 0xffffs", -1),
            ("const v0, 0xffffffff", -1), ("const-wide v0, 0xffffffffffffffffL", -1),
            ("const/high16 v0, 0x3f800000", 0x3f800000),
            ("const-wide/high16 v0, 0x3ff0000000000000L", 0x3ff0000000000000),
            ("const v0, 1.0f", 0x3f800000), ("const v0, 1f", 0x3f800000),
            ("const-wide v0, -1.0", -4616189618054758400),
            (r"const/16 v0, '\u0041'", 65),
            (r'const-string/jumbo v0, "hash# comma, quote\" newline\n" # comment', 'hash# comma, quote" newline\n'),
        ]
        for text, expected in rows:
            with self.subTest(text=text):
                self.assertEqual(method(".registers 2\n" + text + "\nreturn-void").instructions[0].literal, expected)

    def test_field_constants_and_interfaces_are_explicit(self):
        cls = parse(r'''
            .class public LTest;
            .super Ljava/lang/Object;
            .implements LRunnable;
            .field public static final SMALL:B = 0xfft
            .field public static final YES:Z = true
            .field public static final LETTER:C = '\u0041'
            .field public static final BIG:J = 0xffffffffffffffffL
            .field public static final RATE:F = 1.5f
            .field public static final TEXT:Ljava/lang/String; = "a,#\n"
            .field public instance:I
        ''')
        self.assertEqual(cls.interfaces, ["LRunnable;"])
        self.assertEqual([f.value for f in cls.fields], [-1, True, 65, -1, {"kind": "float-bits", "bits": 0x3fc00000}, "a,#\n", None])
        self.assertEqual(cls.source_id, "fixture.smali")

    def test_floating_field_values_keep_negative_zero_and_nan_bits(self):
        cls = parse("""
            .class LTest;
            .super Ljava/lang/Object;
            .field static ZERO:F = -0.0f
            .field static WIDEZERO:D = -0.0
            .field static NOTNUMBER:F = NaNf
            .field static INFINITE:D = Infinity
        """)
        self.assertEqual([f.value for f in cls.fields], [
            {"kind": "float-bits", "bits": 0x80000000},
            {"kind": "double-bits", "bits": 0x8000000000000000},
            {"kind": "float-bits", "bits": 0x7fc00000},
            {"kind": "double-bits", "bits": 0x7ff0000000000000},
        ])

    def test_range_arguments_are_words_and_wide_pairs_are_adjacent(self):
        result = method(".registers 8\ninvoke-static/range {v0 .. v7}, LOther;->accept(JJJJ)V\nreturn-void")
        self.assertEqual(result.instructions[0].registers, tuple(range(8)))
        result = method(".registers 0\ninvoke-static/range {}, LOther;->accept()V\nreturn-void")
        self.assertEqual(result.instructions[0].registers, ())
        for statement in ("invoke-static {v0, v2}, LOther;->accept(J)V",
                          "invoke-direct {v0}, LOther;->accept(I)V",
                          "invoke-static/range {v3 .. v1}, LOther;->accept(I)V",
                          "invoke-static {v0, v1, v2, v3, v4, v5}, LOther;->accept(IIIIII)V"):
            with self.subTest(statement=statement), self.assertRaises(MobileError):
                method(".registers 8\n" + statement + "\nreturn-void")

    def test_switch_payloads_are_attached_without_executable_pseudo_ops(self):
        packed = method("""
            .registers 1
            packed-switch v0, :table
            return-void
            :one
            return-void
            :two
            return-void
            :table
            .packed-switch -0x1
                :one
                :two
            .end packed-switch
            :end
        """)
        self.assertEqual(packed.code_end, 5)
        self.assertEqual([i.pc for i in packed.instructions], [0, 1, 2, 3])
        self.assertEqual((packed.instructions[0].target, packed.instructions[0].keys, packed.instructions[0].targets), (4, (-1, 0), (2, 3)))
        sparse = method("""
            .registers 1
            sparse-switch v0, :table
            :done
            return-void
            :table
            .sparse-switch
                -0x80000000 -> :done
                0x7fffffff -> :done
            .end sparse-switch
        """)
        self.assertEqual((sparse.instructions[0].keys, sparse.instructions[0].targets), ((-2147483648, 2147483647), (1, 1)))

    def test_array_payload_is_typed_raw_data(self):
        for size, values, expected in ((1, "0xfft, 0x7ft", (-1, 127)), (2, "0xffffs\n0x7fffs", (-1, 32767)),
                                       (4, "1.0f, 0xffffffff", (0x3f800000, -1)), (8, "0xffffffffffffffffL", (-1,))):
            with self.subTest(size=size):
                result = method(f".registers 1\nfill-array-data v0, :data\nreturn-void\n:data\n.array-data {size}\n{values}\n.end array-data")
                data = result.instructions[0]
                self.assertEqual((data.element_width, data.data, data.target, result.code_end), (size, expected, 2, 3))

    def test_try_end_can_be_code_end_and_handler_order_is_preserved(self):
        result = method("""
            .registers 1
            goto :start
            :handler
            move-exception v0
            return-void
            :start
            throw v0
            :end
            .catch LFirst; {:start .. :end} :handler
            .catch LSecond; {:start .. :end} :handler
            .catchall {:start .. :end} :handler
        """)
        self.assertEqual(result.code_end, 4)
        self.assertEqual(result.tries, [TryRegion(3, 4, (("LFirst;", 1), ("LSecond;", 1), (None, 1)))])

    def test_debug_directives_are_validated_and_do_not_consume_pcs(self):
        result = method(r'''
            .locals 1
            .param p0, "arg"
            .end param
            .prologue
            .line 10
            .local v0, "value":I
            .source "file#name.java"
            const/4 v0, 0x1
            .end local v0
            .restart local v0
            .epilogue
            return v0
        ''', "public static run(I)I")
        self.assertEqual((len(result.instructions), result.code_end), (2, 2))

    def test_unknown_syntax_annotations_and_duplicate_definitions_are_rejected(self):
        fragments = [".unknown thing", ".annotation runtime LUnknown;\n.end annotation",
                     ".annotation system Ldalvik/annotation/MemberClasses;\nvalue = { LOther;, LOther; }\n.end annotation",
                     ".super LOther;\n.super LAgain;", ".implements LOther;\n.implements LOther;",
                     ".field static x:I\n.field static x:I", ".class public LAgain;"]
        for fragment in fragments:
            with self.subTest(fragment=fragment), self.assertRaises(MobileError):
                parse(".class public LTest;\n.super Ljava/lang/Object;\n" + fragment)
        with self.assertRaises(MobileError):
            parse(".class public LTest;\n.super Ljava/lang/Object;\n" + ".method static same()V\n.registers 0\nreturn-void\n.end method\n" * 2)

    def test_malformed_instruction_frames_targets_and_payloads_fail(self):
        bodies = [
            ".registers 1\n.registers 1\nreturn-void", ".registers 1\nmove-wide v0, v0\nreturn-void",
            ".registers 17\nmove v0, v16\nreturn-void", ".registers 1\nreturn p0",
            ".registers 1\nconst/4 v0, 0x8\nreturn-void", ".registers 1\nconst/high16 v0, 0x10001\nreturn-void",
            ".registers 1\nconst/4 v0, 0x10000000000000000\nreturn-void",
            ".registers 1\nconst v0, 1.0d\nreturn-void", ".registers 2\nconst-wide v0, 1.0f\nreturn-void",
            ".registers 1\nconst/4 v0, 010\nreturn-void",
            ".registers 1\nsget-object v0, LTest;->x:I\nreturn-void",
            ".registers 1\ninvoke-static {}, LTest;-><init>()V\nreturn-void",
            ".registers 1\nreturn-object v0",
            ".registers 0\ngoto :missing", ".registers 0\n:x\n:x\nreturn-void",
            ".registers 0\ngoto :end\n:end", ".registers 0\nmade-up-op\nreturn-void",
            ".registers 1\ninvoke-custom {}, site\nreturn-void",
            ".registers 1\npacked-switch v0, :data\nreturn-void\n:data\n.array-data 1\n0\n.end array-data",
            ".registers 1\nfill-array-data v0, :data\n:data\n.array-data 1\n0\n.end array-data",
            ".registers 1\nreturn-void\n:data\n.array-data 3\n0\n.end array-data",
            ".registers 1\nreturn-void\n:data\n.array-data 1\n0\n.end array-data",
            ".registers 1\n:data\n.array-data 1\n0\n.end array-data\nfill-array-data v0, :data\nreturn-void",
            ".registers 1\nsparse-switch v0, :data\n:done\nreturn-void\n:data\n.sparse-switch\n1 -> :done\n1 -> :done\n.end sparse-switch",
        ]
        for body in bodies:
            with self.subTest(body=body), self.assertRaises(MobileError):
                method(body)

    def test_missing_closers_invalid_constructors_and_catches_fail(self):
        with self.assertRaises(MobileError):
            parse(".class public LTest;\n.super Ljava/lang/Object;\n.method static run()V\n.registers 0\nreturn-void")
        for declaration in ("static constructor <init>()V", "constructor <init>()I", "constructor ordinary()V", "abstract static run()V"):
            with self.subTest(declaration=declaration), self.assertRaises(MobileError):
                method(".locals 0\nreturn-void", declaration)
        for catches in (".catch LEx; {:missing .. :end} :start", ".catchall {:start .. :end} :start\n.catch LEx; {:start .. :end} :start"):
            with self.subTest(catches=catches), self.assertRaises(MobileError):
                method(".registers 0\n:start\nreturn-void\n:end\n" + catches)

    def test_work_deadline_and_byte_budgets_fail_before_expansion(self):
        text = ".class LTest;\n.super Ljava/lang/Object;"
        budget = Budget(Limits()); budget.remaining = 4
        with self.assertRaisesRegex(MobileError, "budget"):
            parse(text, budget)
        budget = Budget(Limits()); budget.deadline = time.monotonic() - 1
        with self.assertRaisesRegex(MobileError, "budget"):
            parse(text, budget)
        with self.assertRaisesRegex(MobileError, "byte budget"):
            parse(text, Budget(Limits(max_bytes=10)))


if __name__ == "__main__":
    unittest.main()
