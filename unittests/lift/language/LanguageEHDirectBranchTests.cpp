//===- LanguageEHDirectBranchTests.cpp - Direct branch decoding tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "LanguageEHTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::dwarf_eh;
using namespace neverd::language_eh_test;

//===----------------------------------------------------------------------===//
// Direct branch decoding
//===----------------------------------------------------------------------===//

/// Decode the halfword pair \p Hw1, \p Hw2 as a Thumb branch sited at \p VA.
std::optional<va_t> decodeThumbAt(va_t VA, uint16_t Hw1, uint16_t Hw2) {
  const uint8_t Bytes[4] = {
      static_cast<uint8_t>(Hw1), static_cast<uint8_t>(Hw1 >> 8),
      static_cast<uint8_t>(Hw2), static_cast<uint8_t>(Hw2 >> 8)};
  size_t Length = 0;
  return decodeDirectBranchTarget(Arch::ARM, InstructionMode::Thumb, Bytes,
                                  sizeof(Bytes), VA, Length);
}

TEST(DirectBranch, DecodesThumbCallAndTailJump) {
  // `f000 f87e` is the `bl` a Thumb-2 assembler emits for a call 0x100 bytes
  // ahead, and `f000 b87e` the `b.w` for the same displacement.
  EXPECT_EQ(decodeThumbAt(0x1000, 0xF000, 0xF87E), va_t(0x1100));
  EXPECT_EQ(decodeThumbAt(0x1000, 0xF000, 0xB87E), va_t(0x1100));
}

TEST(DirectBranch, DecodesThumbBranchAcrossTheSignBoundary) {
  // `f7ff fffe` is the self-referential `bl` an unlinked object carries for a
  // call relocation; every J/I bit is set, so it exercises the whole
  // sign-recovery path.
  EXPECT_EQ(decodeThumbAt(0x1000, 0xF7FF, 0xFFFE), va_t(0x1000));
  EXPECT_EQ(decodeThumbAt(0x2000, 0xF7FE, 0xFFFE), va_t(0x1000));
}

TEST(DirectBranch, RoundsTheBaseDownForAnInterworkingCall) {
  // `blx` targets ARM state, so it is encoded against the word-aligned program
  // counter rather than the halfword-aligned one a `bl` uses.  Sited at an
  // address that is not word aligned, the two disagree.
  EXPECT_EQ(decodeThumbAt(0x1002, 0xF000, 0xE87E), va_t(0x1100));
}

TEST(DirectBranch, RejectsAConditionalThumbBranch) {
  // `f000 8000` is `beq.w`, which shares the leading halfword with `b.w` but
  // names a branch inside the function rather than a call edge.
  EXPECT_FALSE(decodeThumbAt(0x1000, 0xF000, 0x8000).has_value());
  // `f04f 0000` is `mov.w r0, #0` -- the same leading halfword again.
  EXPECT_FALSE(decodeThumbAt(0x1000, 0xF04F, 0x0000).has_value());
}

TEST(DirectBranch, KeepsDecodingARMStateBranches) {
  // `eb000000` is `bl` to the instruction eight bytes ahead, which is where
  // the ARM pipeline offset puts a zero displacement.
  const uint8_t Call[4] = {0x00, 0x00, 0x00, 0xEB};
  size_t Length = 0;
  EXPECT_EQ(decodeDirectBranchTarget(Arch::ARM, InstructionMode::ARM, Call,
                                     sizeof(Call), 0x1000, Length),
            va_t(0x1008));
  // A predicated branch is not a call edge, so it stays undecoded.
  const uint8_t Conditional[4] = {0x00, 0x00, 0x00, 0x0B};
  EXPECT_FALSE(decodeDirectBranchTarget(Arch::ARM, InstructionMode::ARM,
                                        Conditional, sizeof(Conditional),
                                        0x1000, Length)
                   .has_value());
}

TEST(DirectBranch, ScansThumbAtHalfwordGranularity) {
  // A 32-bit Thumb branch may begin at any halfword, so a word-granular scan
  // would step over half of them.
  EXPECT_EQ(getBranchScanStride(Arch::ARM, InstructionMode::Thumb), 2u);
  EXPECT_EQ(getBranchScanStride(Arch::ARM, InstructionMode::ARM), 4u);
  EXPECT_EQ(getBranchScanStride(Arch::X64, InstructionMode::Default), 1u);
}

} // namespace
