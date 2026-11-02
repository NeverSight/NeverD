//===- X86PackedSourceConstantsTests.cpp - Complete C scalar storage ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute real rendered packed inputs with checked recorders and sanitizers.
/// The recorder replaces only the ISA operation; this does not execute AVX-512.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86CompressExpand.h"
#include "../../../lib/backend/c/render/X86/X86FourSource.h"
#include "../x86_64/X86FPStateAccuracyFixture.h"

namespace {
class X86PackedSourceConstants : public X86FPStateFixture {};

TEST_F(X86PackedSourceConstants, NarrowLiteralsFillTheCompletePackedInput) {
  std::string Source = R"(
#include <stdint.h>
#include <stdlib.h>
#define NEVERD_X86_COMPACT(native, ...) native
#define NEVERD_X86_FOUR_SOURCE(native, ...) native
static unsigned expected, calls;
static void check(const void *data, unsigned bytes, unsigned first) {
 const unsigned char *p = data;
 for (unsigned i = 0; i < bytes; ++i)
  if (p[i] != (first && i < 4 ? (unsigned char)(expected >> (i * 8)) : 0)) abort();
}
)";
  for (unsigned Bytes : {16u, 32u, 64u})
    Source += "typedef unsigned int v" + std::to_string(Bytes) +
              " __attribute__((vector_size(" + std::to_string(Bytes) + ")));\n";
  std::string Main = "int main(void) {\n";
  unsigned Cases = 0;
  for (unsigned Bytes : {16u, 32u, 64u})
    for (unsigned Kind = 0; Kind < 6; ++Kind)
      for (bool Expand : {false, true}) {
        const auto P = *x86CompactPlan(Expand, Bytes, Kind, false, 0);
        const auto V = "v" + std::to_string(Bytes);
        const auto Name = "record_" + P.mnemonic() + std::to_string(Bytes);
        Source += "#define " + P.builtin() + " " + Name + "\nstatic " +
                  (Expand ? V : "void") + " " + Name +
                  "(void *address, unsigned long long mask, " + V + " data, " +
                  (Expand ? "unsigned zero, " : "") + "unsigned prefix) {\n";
        Source +=
            "if ((uintptr_t)address != 0x12345678 || mask != 5 || prefix" +
            std::string(Expand ? " || zero" : "") + ") abort();\n";
        Source += "check(&data, " + std::to_string(Bytes) + ", 1); ++calls;\n";
        if (Expand)
          Source += "return data;\n";
        Source += "}\n";
        for (unsigned Value : {0u, 1u, 0x11223344u}) {
          const auto Id =
              Expand ? Intrinsic::EVEXExpandLoad : Intrinsic::EVEXCompressStore;
          auto E = HighExpr::makeCall("compact", 0, {});
          E->IntrinsicId = Id;
          E->Type = Expand ? NdType::makeInt(Bytes, false) : NdType::makeVoid();
          const auto Control =
              P.elementBytes() | (Kind >= 4 ? X86CompactFloat : 0);
          E->Operands = {HighExpr::makeConst(0x12345678, 8),
                         HighExpr::makeConst(5, 8),
                         HighExpr::makeConst(Value, Bytes),
                         HighExpr::makeConst(Control, 1)};
          const auto Function = "case_" + std::to_string(Cases++);
          const auto Type =
              "unsigned _BitInt(" + std::to_string(Bytes * 8) + ")";
          Source += "static void " + Function + "(void) { (void)" +
                    renderX86CompactExpression(Arch::X64, *E,
                                               {"0x12345678", "5",
                                                std::to_string(Value),
                                                std::to_string(Control)},
                                               Type) +
                    "; }\n";
          Main +=
              "expected=" + std::to_string(Value) + "; " + Function + "();\n";
        }
      }
  for (unsigned Kind = 0; Kind < 6; ++Kind) {
    const auto P = *x86FourSourcePlan(Kind, false, 0);
    const auto Bytes = P.Control.bytes();
    const auto V = "v" + std::to_string(Bytes);
    const auto Name = "record_four_" + std::to_string(Kind);
    Source += "#define " + P.builtin() + " " + Name + "\nstatic " + V + " " +
              Name + "(void *address, " + V + " acc, " + V + " a, " + V +
              " b, " + V + " c, " + V +
              " d, unsigned short mask, unsigned zero, unsigned prefix) {\n";
    Source += "if ((uintptr_t)address != 0x12345678 || mask != 1 || zero || "
              "prefix) abort();\n";
    Source += "check(&acc, " + std::to_string(Bytes) + ", 1); check(&a, " +
              std::to_string(Bytes) + ", 1); check(&b, " +
              std::to_string(Bytes) + ", 0); check(&c, " +
              std::to_string(Bytes) + ", 0); check(&d, " +
              std::to_string(Bytes) + ", 0); ++calls; return acc; }\n";
    for (unsigned Value : {0u, 1u, 0x11223344u}) {
      auto E = HighExpr::makeCall("four", 0, {});
      E->IntrinsicId = Intrinsic::X86FourSourceMemory;
      E->Type = NdType::makeInt(Bytes, false);
      E->Operands = {HighExpr::makeConst(0x12345678, 8),
                     HighExpr::makeConst(Value, Bytes),
                     HighExpr::makeConst(Value, P.Control.sourceBytes() *
                                                    X86FourSourceCount),
                     HighExpr::makeConst(1, P.Control.maskBytes()),
                     HighExpr::makeConst(Kind, 1)};
      const auto Function = "case_" + std::to_string(Cases++);
      const auto Type = "unsigned _BitInt(" + std::to_string(Bytes * 8) + ")";
      Source += "static void " + Function + "(void) { (void)" +
                renderX86FourSourceExpression(
                    Arch::X64, *E,
                    {"0x12345678", std::to_string(Value), std::to_string(Value),
                     "1", std::to_string(Kind)},
                    Type) +
                "; }\n";
      Main += "expected=" + std::to_string(Value) + "; " + Function + "();\n";
    }
  }
  Source += Main + "if (calls != " + std::to_string(Cases) +
            ") abort(); return 0; }\n";
  EXPECT_EQ(Cases, 126u);
  const auto Input = file("packed-constants.c");
  write(Input, Source);
  retain("packed-constants.c", Source);
  for (const auto *Opt : {"-O0", "-O2"}) {
    const auto Output = file(std::string("constants") + Opt);
    auto Built = command(Compiler,
                         {Opt, "-std=gnu23", "-Wno-psabi",
                          "-fsanitize=address,undefined", Input, "-o", Output});
    ASSERT_EQ(Built.Status, 0) << Built.Error;
    auto Ran = command(Output, {});
    ASSERT_EQ(Ran.Status, 0) << Ran.Out << Ran.Error;
  }
  RecordProperty("sanitized_operand_comparisons", 2 * Cases);
}
} // namespace
