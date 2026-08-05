//===- ARM32_PopPushSingleRTTests.cpp - single-register pop/push --------===//
//
// Roundtrip probes for ARM32 single-register `pop {Rt}` / `push {Rt}`.
//
// capstone 6 aliases a single-register pop/push to ARM_INS_LDR / ARM_INS_STR
// (mnemonic "pop"/"push") with only the Rt operand — the [sp] base is implicit
// (the same way multi-register pop/push alias to LDM/STM).  The LDR/STR handler
// bailed out on `op_count < 2`, so the load/store was silently dropped: a
// `pop {r0}` left r0 unchanged and a `push {r0}` stored nothing / left sp.
//
// (This is what made an earlier `stlb`-into-a-stack-local probe wrongly return
// the argument: its epilogue `pop {r0}` — the return-value load — was dropped,
// not any store-to-load forwarding issue.)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32PopPushSingleRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PopPushSingleRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kArm32PopPush = {

  // push {Rt} then pop {Rt2}: round-trips the value through the stack.
  {"pushpop",
   "unsigned long pushpop(unsigned long a){ unsigned r;"
   " __asm__ volatile(\"push {%1}\\n pop {%0}\\n\":\"=&r\"(r):\"r\"((unsigned)a):\"memory\");"
   " return r; }\n",
   {0xDEADBEEF}, "PopPush", 1, ""},

  // Two single-register pushes, two pops in LIFO order: pop order reverses.
  {"push2pop2",
   "unsigned long push2pop2(unsigned long a){ unsigned x=(unsigned)a, y=(unsigned)a+0x1111; unsigned p,q;"
   " __asm__ volatile(\"push {%2}\\n push {%3}\\n pop {%0}\\n pop {%1}\\n\""
   " :\"=&r\"(p),\"=&r\"(q):\"r\"(x),\"r\"(y):\"memory\");"
   " return p*7+q*13; }\n",
   {100}, "PopPush", 1, ""},

  // Single-register pop into a value used afterwards (mirrors a return-value
  // load `pop {r0}` mid-function).
  {"pop_then_use",
   "unsigned long pop_then_use(unsigned long a){ unsigned r;"
   " __asm__ volatile(\"push {%1}\\n pop {%0}\\n\":\"=&r\"(r):\"r\"((unsigned)a+5):\"memory\");"
   " return r*3; }\n",
   {77}, "PopPush", 1, ""},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(PopPush, ARM32PopPushSingleRT,
                         ::testing::ValuesIn(kArm32PopPush), rtTCName);
