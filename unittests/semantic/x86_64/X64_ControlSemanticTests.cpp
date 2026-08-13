//===- X64_ControlSemanticTests.cpp - x64 control/string/rep tests -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Migrated from scripts/lift_verifier.py — StringRep, FlagMisc, Fence, etc.
// Also from scripts/pipeline_verifier.py — x64 pipeline test cases.
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

TEST_P(X64Semantic, ControlVerify) { runX64(GetParam()); }

// clang-format off

// ============================================================================
// StringRep: REP-prefixed string ops (single iteration with ECX=1)
// ============================================================================
static const std::vector<SemTC> kX64StringRep = {
  {"rep_stosb",     "cld; mov al, 0x42; mov ecx, 1; rep stosb",
                     {{"rdi", DATA_BASE}, {"rcx", 0}},                                     {"rdi", "rcx"},    "StringRep", {}},
  {"rep_stosd",     "cld; mov eax, 0xDEADBEEF; mov ecx, 1; rep stosd",
                     {{"rdi", DATA_BASE}, {"rcx", 0}},                                     {"rdi", "rcx"},    "StringRep", {}},
  {"rep_stosq",     "cld; mov rax, 0x0102030405060708; mov ecx, 1; rep stosq",
                     {{"rdi", DATA_BASE}, {"rcx", 0}},                                     {"rdi", "rcx"},    "StringRep", {}},
  {"rep_movsb",     "cld; mov ecx, 1; rep movsb",
                     {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 0x100}, {"rcx", 0}},         {"rsi", "rdi", "rcx"}, "StringRep",
   {{DATA_BASE, {0x42}}}},
  {"rep_movsq",     "cld; mov ecx, 1; rep movsq",
                     {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 0x100}, {"rcx", 0}},         {"rsi", "rdi", "rcx"}, "StringRep",
   {{DATA_BASE, packU64(0xDEADBEEF12345678ULL)}}},
  {"std_stosb",     "std; mov al, 0x42; stosb; cld",
                     {{"rdi", DATA_BASE + 8}},                                              {"rdi"},           "StringRep", {}},
  {"std_lodsb",     "std; lodsb; cld",
                     {{"rsi", DATA_BASE + 8}},                                              {"rax", "rsi"},    "StringRep",
   {{DATA_BASE + 8, {0xAA}}}},
  {"scasb",         "cld; mov al, 0x42; scasb",
                     {{"rdi", DATA_BASE}},                                                   {"rdi"},           "StringRep",
   {{DATA_BASE, {0x42}}}},
  {"cmpsb",         "cld; cmpsb",
                     {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}},                         {"rsi", "rdi"},    "StringRep",
   {{DATA_BASE, cat({{0x42}, zeros(15), {0x42}})}}},
};

// ============================================================================
// FlagMisc: flag-related edge cases
// ============================================================================
static const std::vector<SemTC> kX64FlagMisc = {
  {"pushfq_popfq",  "pushfq; popfq",                               {},                                         {},                "FlagMisc", {}},
  {"cld",           "cld",                                         {},                                         {},                "FlagMisc", {}},
  {"std_cld",       "std; cld",                                    {},                                         {},                "FlagMisc", {}},
};

// ============================================================================
// Fence: LFENCE, SFENCE, MFENCE
// ============================================================================
static const std::vector<SemTC> kX64Fence = {
  {"lfence",        "lfence; mov rax, 42",                         {},                                          {"rax"},           "Fence", {}},
  {"sfence",        "sfence; mov rax, 42",                         {},                                          {"rax"},           "Fence", {}},
  {"mfence",        "mfence; mov rax, 42",                         {},                                          {"rax"},           "Fence", {}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(StringRep, X64Semantic, ::testing::ValuesIn(kX64StringRep), semTCName);
INSTANTIATE_TEST_SUITE_P(FlagMisc, X64Semantic, ::testing::ValuesIn(kX64FlagMisc), semTCName);
INSTANTIATE_TEST_SUITE_P(Fence, X64Semantic, ::testing::ValuesIn(kX64Fence), semTCName);
