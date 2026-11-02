//===- AllPlatform_SelfCompareFlagsRTTests.cpp - Self comparisons -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compare original machine code with lifted code and both C routes. A flag
/// consumer of A-A must retain both operands; TEST/TST and shifted operands
/// still depend on their inputs.
///
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"
#include "gtest/gtest.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

class X86SelfCompareFlagsRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86SelfCompareFlagsRT, Verify) { roundTripX86(GetParam()); }

class X64SelfCompareFlagsRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SelfCompareFlagsRT, Verify) { roundTripX64(GetParam()); }

class A64SelfCompareFlagsRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SelfCompareFlagsRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32SelfCompareFlagsRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SelfCompareFlagsRT, Verify) { roundTripARM32(GetParam()); }

struct FlagProbe {
  const char *Name;
  const char *X86Condition;
  const char *ArmCondition;
  bool Subtract;
  bool Branch;
  bool Logical;
};

constexpr FlagProbe kProbes[] = {
    {"cmp_eq", "e", "eq", false, false, false},
    {"cmp_ne_branch", "ne", "ne", false, true, false},
    {"cmp_borrow", "b", "lo", false, false, false},
    {"cmp_overflow", "o", "vs", false, false, false},
    {"cmp_signed_lt", "l", "lt", false, false, false},
    {"sub_ne", "ne", "ne", true, false, false},
    {"sub_borrow_branch", "b", "lo", true, true, false},
    {"sub_overflow", "o", "vs", true, false, false},
    {"sub_signed_lt", "l", "lt", true, false, false},
    {"logical_eq_control", "e", "eq", false, false, true},
    {"logical_ne_branch_control", "ne", "ne", false, true, true},
};

std::vector<RoundTripTC> makeCases(bool IsX86, bool Is64) {
  std::vector<RoundTripTC> Cases;
  for (unsigned Bits : {32u, 64u}) {
    if (Bits == 64 && !Is64)
      continue;
    const uint64_t Sign = uint64_t{1} << (Bits - 1);
    for (const auto &Probe : kProbes) {
      std::string Source;
      const std::string Type = Bits == 64 ? "unsigned long" : "unsigned";
      if (IsX86) {
        const std::string Op = Probe.Logical    ? "test"
                               : Probe.Subtract ? "sub"
                                                : "cmp";
        const std::string Insn = Op + (Bits == 64 ? "q" : "l") + " %1,%1\\n\\t";
        const std::string Condition = Probe.X86Condition;
        const std::string Consumer =
            Probe.Branch
                ? "movb $1,%0\\n\\tj" + Condition + " 1f\\n\\tmovb $0,%0\\n1:"
                : "set" + Condition + " %0";
        Source = "long f(long a){" + Type + " x=(" + Type +
                 ")a; unsigned char r; __asm__ volatile(\"" + Insn + Consumer +
                 "\":\"=&q\"(r),\"+r\"(x)::\"cc\"); return r;}\n";
      } else if (Is64) {
        const std::string Reg = Bits == 64 ? "%x1" : "%w1";
        const std::string Insn = Probe.Logical ? "tst " + Reg + "," + Reg
                                 : Probe.Subtract
                                     ? "subs " + Reg + "," + Reg + "," + Reg
                                     : "cmp " + Reg + "," + Reg;
        const std::string Condition = Probe.ArmCondition;
        const std::string Consumer =
            Probe.Branch
                ? "mov %w0,#1\\n\\tb." + Condition + " 1f\\n\\tmov %w0,#0\\n1:"
                : "cset %w0," + Condition;
        Source = "long f(long a){" + Type + " x=(" + Type +
                 ")a; unsigned r; __asm__ volatile(\"" + Insn + "\\n\\t" +
                 Consumer + "\":\"=&r\"(r),\"+r\"(x)::\"cc\"); return r;}\n";
      } else {
        const std::string Insn = Probe.Logical    ? "tst %1,%1"
                                 : Probe.Subtract ? "subs %1,%1,%1"
                                                  : "cmp %1,%1";
        const std::string Condition = Probe.ArmCondition;
        const std::string Consumer =
            Probe.Branch
                ? "mov %0,#1\\n\\tb" + Condition + " 1f\\n\\tmov %0,#0\\n1:"
                : "mov %0,#0\\n\\tmov" + Condition + " %0,#1";
        Source = "long f(long a){unsigned x=(unsigned)a,r; "
                 "__asm__ volatile(\"" +
                 Insn + "\\n\\t" + Consumer +
                 "\":\"=&r\"(r),\"+r\"(x)::\"cc\"); return r;}\n";
      }
      for (uint64_t Value : {uint64_t{0}, uint64_t{19}, Sign, Sign - 1}) {
        RoundTripTC TC;
        TC.Name = std::string(Probe.Name) + "_" + std::to_string(Bits) + "_" +
                  std::to_string(Value);
        TC.CSrc = Source;
        TC.Args = {Value};
        TC.Category = "SelfCompareFlags";
        TC.OptLevel = 2;
        Cases.push_back(TC);
      }
    }
  }
  if (!IsX86) {
    // Equal register identities do not imply equal operands after shifting.
    const std::string Source =
        Is64
            ? "long f(long a){long r; __asm__ volatile(\"cmp %1,%1,lsl #1\\n\\t"
              "cset %w0,eq\":\"=&r\"(r):\"r\"(a):\"cc\"); return r;}\n"
            : "long f(long a){long r; __asm__ volatile(\"cmp %1,%1,lsl #1\\n\\t"
              "mov %0,#0\\n\\tmoveq %0,#1\":\"=&r\"(r):\"r\"(a):\"cc\");"
              "return r;}\n";
    for (uint64_t Value : {uint64_t{0}, uint64_t{19}}) {
      RoundTripTC TC;
      TC.Name = "shifted_compare_control_" + std::to_string(Value);
      TC.CSrc = Source;
      TC.Args = {Value};
      TC.Category = "SelfCompareFlags";
      TC.OptLevel = 2;
      Cases.push_back(TC);
    }
  }
  // No source argument supplies this register. Its bits cannot affect a
  // self-comparison, so reconstruction must not introduce a machine-state
  // guard merely to evaluate a condition whose answer is already fixed.
  const std::string UnobservedAsm =
      IsX86  ? std::string(Is64 ? "cmpq %%r11,%%r11" : "cmpl %%esi,%%esi") +
                   "\\n\\tjne 1f\\n\\tmov $42,%0\\n\\tjmp 2f\\n1: "
                   "mov $7,%0\\n2:"
      : Is64 ? "cmp x19,x19\\n\\tb.ne 1f\\n\\tmov %0,#42\\n\\tb 2f\\n1: "
               "mov %0,#7\\n2:"
             : "cmp r8,r8\\n\\tbne 1f\\n\\tmov %0,#42\\n\\tb 2f\\n1: "
               "mov %0,#7\\n2:";
  RoundTripTC Unobserved;
  Unobserved.Name = "unobserved_self_cmp_branch";
  Unobserved.CSrc = "long f(void){long r; __asm__ volatile(\"" + UnobservedAsm +
                    "\":\"=&r\"(r)::\"cc\"); return r;}\n";
  Unobserved.Category = "SelfCompareFlags";
  Unobserved.OptLevel = 2;
  Cases.push_back(Unobserved);
  const std::string OverflowAsm =
      IsX86  ? std::string(Is64 ? "subq %%r11,%%r11" : "subl %%ecx,%%ecx") +
                   "\\n\\tjo 1f\\n\\tmov $42,%0\\n\\tjmp 2f\\n1: "
                   "mov $7,%0\\n2:"
      : Is64 ? "subs %0,x19,x19\\n\\tb.vs 1f\\n\\tmov %0,#42\\n\\tb 2f\\n1: "
               "mov %0,#7\\n2:"
             : "subs %0,r8,r8\\n\\tbvs 1f\\n\\tmov %0,#42\\n\\tb 2f\\n1: "
               "mov %0,#7\\n2:";
  const std::string Clobber = IsX86 ? (Is64 ? ",\"r11\"" : ",\"ecx\"") : "";
  Unobserved.Name = "unobserved_self_sub_overflow_branch";
  Unobserved.CSrc = "long f(void){long r; __asm__ volatile(\"" + OverflowAsm +
                    "\":\"=&r\"(r)::\"cc\"" + Clobber + "); return r;}\n";
  Cases.push_back(Unobserved);
  return Cases;
}

INSTANTIATE_TEST_SUITE_P(SelfCompareFlags, X86SelfCompareFlagsRT,
                         ::testing::ValuesIn(makeCases(true, false)), rtTCName);
INSTANTIATE_TEST_SUITE_P(SelfCompareFlags, X64SelfCompareFlagsRT,
                         ::testing::ValuesIn(makeCases(true, true)), rtTCName);
INSTANTIATE_TEST_SUITE_P(SelfCompareFlags, A64SelfCompareFlagsRT,
                         ::testing::ValuesIn(makeCases(false, true)), rtTCName);
INSTANTIATE_TEST_SUITE_P(SelfCompareFlags, ARM32SelfCompareFlagsRT,
                         ::testing::ValuesIn(makeCases(false, false)),
                         rtTCName);

} // namespace
