//===- AArch64_CompactJumpTableRTTests.cpp - compact table RT -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

#include <sstream>
#include <string>
#include <vector>

class AArch64CompactJumpTableRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};

TEST_P(AArch64CompactJumpTableRT, Verify) { roundTripAArch64(GetParam()); }

static std::string makeCompactGuarded32Source(const std::string &Name) {
  // Keep the address coordinate equal to the guarded W-register value.  A
  // plain X-register index would leave its upper half unconstrained.
  std::ostringstream Src;
  Src << "long " << Name << "(long selector) {\n"
      << "  long result;\n"
      << "  __asm__ volatile(\n"
      << "    \"cmp %w1, #31\\n\\t\"\n"
      << "    \"b.hi .Lcompact_default%=\\n\\t\"\n"
      << "    \"adrp x9, .Lcompact_table%=\\n\\t\"\n"
      << "    \"add x9, x9, :lo12:.Lcompact_table%=\\n\\t\"\n"
      << "    \"adr x10, .Lcompact_anchor%=\\n\\t\"\n"
      << "    \"ldrb w11, [x9, %w1, uxtw]\\n\\t\"\n"
      << "    \"add x10, x10, x11, lsl #2\\n\\t\"\n"
      << "    \"br x10\\n\\t\"\n"
      << "    \".Lcompact_anchor%=:\\n\\t\"\n";
  for (int I = 0; I < 32; ++I)
    Src << "    \"mov %w0, #" << 100 + I << "\\n\\t\"\n"
        << "    \"b .Lcompact_done%=\\n\\t\"\n";
  Src << "    \".Lcompact_default%=:\\n\\t\"\n"
      << "    \"mov %w0, #199\\n\\t\"\n"
      << "    \".Lcompact_done%=:\\n\\t\"\n"
      << "    \".pushsection .rodata\\n\\t\"\n"
      << "    \".Lcompact_table%=:\\n\\t\"\n"
      << "    \".byte 0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30\\n\\t\"\n"
      << "    \".byte 32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62\\n\\t\"\n"
      << "    \".popsection\\n\\t\"\n"
      << "    : \"=&r\"(result)\n"
      << "    : \"r\"(selector)\n"
      << "    : \"x9\", \"x10\", \"x11\", \"cc\", \"memory\");\n"
      << "  return result;\n"
      << "}\n";
  return Src.str();
}

// Compile the source at -O2 to keep the selector in the dispatch register,
// then disable NeverD optimization so this isolates LowIR CFG recovery.
static const std::vector<RoundTripTC> kCompactGuarded32 = {
    {"a64_compact_guarded32_case1",
     makeCompactGuarded32Source("a64_compact_guarded32_case1"),
     {1},
     "CompactByteGuard",
     2,
     "",
     true},
    {"a64_compact_guarded32_case31",
     makeCompactGuarded32Source("a64_compact_guarded32_case31"),
     {31},
     "CompactByteGuard",
     2,
     "",
     true},
    {"a64_compact_guarded32_default",
     makeCompactGuarded32Source("a64_compact_guarded32_default"),
     {32},
     "CompactByteGuard",
     2,
     "",
     true},
};

INSTANTIATE_TEST_SUITE_P(CompactByteGuard, AArch64CompactJumpTableRT,
                         ::testing::ValuesIn(kCompactGuarded32), rtTCName);
