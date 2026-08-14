//===- SignatureMatcherTests.cpp - Signature matcher tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/SignatureMatcher.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

using namespace neverd::sigs;

TEST(SignatureMatcherCRC16, MatchesPublishedCheckVectors) {
  EXPECT_EQ(SignatureMatcher::computeCRC16(nullptr, 0), 0x0000u);

  constexpr std::array<uint8_t, 9> Check = {'1', '2', '3', '4', '5',
                                            '6', '7', '8', '9'};
  EXPECT_EQ(SignatureMatcher::computeCRC16(Check.data(), Check.size()),
            0x6E90u);
}

TEST(SignatureMatcherRange, TruncatesCoverageAtDeclaredFunctionEnd) {
  constexpr std::array<uint8_t, 4> Data = {0xAA, 0x42, 0x43, 0xFF};

  PatternModule LeadingPastEnd;
  LeadingPastEnd.LeadingBytes = {{0xAA, false}, {0xFE, false}};
  LeadingPastEnd.TotalLen = 1;
  EXPECT_TRUE(
      SignatureMatcher::matchPattern(LeadingPastEnd, Data.data(), Data.size()));
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(LeadingPastEnd));
  EXPECT_EQ(SignatureMatcher::fixedByteCount(LeadingPastEnd), 1u);

  PatternModule CRCPastEnd;
  CRCPastEnd.LeadingBytes = {{0xAA, false}};
  CRCPastEnd.CRCLen = 2;
  CRCPastEnd.CRC16 = 0x0E0A;
  CRCPastEnd.TotalLen = 2;
  EXPECT_FALSE(
      SignatureMatcher::matchPattern(CRCPastEnd, Data.data(), Data.size()));

  PatternModule TailPastEnd;
  TailPastEnd.LeadingBytes = {{0xAA, false}};
  TailPastEnd.CRCLen = 1;
  TailPastEnd.CRC16 = 0x6E91;
  TailPastEnd.TailBytes = {{0x43, false}};
  TailPastEnd.TotalLen = 2;
  EXPECT_TRUE(
      SignatureMatcher::matchPattern(TailPastEnd, Data.data(), Data.size()));
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(TailPastEnd));
  EXPECT_EQ(SignatureMatcher::fixedByteCount(TailPastEnd), 1u);

  PatternModule Exact = TailPastEnd;
  Exact.TotalLen = 3;
  EXPECT_TRUE(SignatureMatcher::matchPattern(Exact, Data.data(), Data.size()));
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(Exact));
}

TEST(SignatureMatcherRange, MatchesWildcardPaddedShortFunctionPrefix) {
  // Text signature records use one fixed-width leading field.  A function
  // shorter than that field is padded with wildcards; bytes after TotalLen
  // belong to the next function and must not participate in this match.
  constexpr std::array<uint8_t, 4> Data = {0xAA, 0xBB, 0x11, 0x22};
  PatternModule Short;
  Short.LeadingBytes = {
      {0xAA, false}, {0xBB, false}, {0x00, true}, {0x00, true}};
  Short.TotalLen = 2;

  EXPECT_TRUE(SignatureMatcher::matchPattern(Short, Data.data(), Data.size()));
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(Short));
  EXPECT_EQ(SignatureMatcher::fixedByteCount(Short), 2u);
}

TEST(SignatureMatcherRange, IgnoresFixedPrefixBytesPastDeclaredFunctionEnd) {
  constexpr std::array<uint8_t, 3> Data = {0xAA, 0xBB, 0xCC};
  PatternModule EscapesFunction;
  EscapesFunction.LeadingBytes = {{0xAA, false}, {0xBB, false}, {0x00, false}};
  EscapesFunction.TotalLen = 2;

  EXPECT_TRUE(SignatureMatcher::matchPattern(EscapesFunction, Data.data(),
                                             Data.size()));
  EXPECT_TRUE(SignatureMatcher::isFullyVerified(EscapesFunction));
  EXPECT_EQ(SignatureMatcher::fixedByteCount(EscapesFunction), 2u);
}

TEST(SignatureMatcherRange, HashIndexIgnoresBytesPastDeclaredFunctionEnd) {
  constexpr std::array<uint8_t, 2> Data = {0xAA, 0x42};
  std::vector<PatternModule> Modules;
  for (unsigned I = 0; I != SignatureMatcher::HashIndex::kLeafCandidates + 1;
       ++I) {
    PatternModule Short;
    Short.LeadingBytes = {{0xAA, false}, {static_cast<uint8_t>(I), false}};
    Short.TotalLen = 1;
    Modules.push_back(std::move(Short));
  }

  SignatureMatcher::HashIndex Index;
  Index.build(Modules);
  EXPECT_EQ(Index.candidateCount(Data.data(), 1), Modules.size());

  unsigned Matches = 0;
  SignatureMatcher::scanRegion(Data.data(), Data.size(), Modules,
                               [&](size_t Offset, const PatternModule &) {
                                 EXPECT_EQ(Offset, 0u);
                                 ++Matches;
                               });

  EXPECT_EQ(Matches, Modules.size());
}

TEST(SignatureMatcherRange, UnboundedModulesRequireTheirWholeNominalSpan) {
  constexpr std::array<uint8_t, 3> Data = {0xAA, 0xBB, 0xCC};
  PatternModule Legacy;
  Legacy.LeadingBytes = {{0xAA, false}, {0xBB, false}};
  Legacy.TailBytes = {{0xCC, false}};

  EXPECT_FALSE(SignatureMatcher::matchPattern(Legacy, Data.data(), 2));
  EXPECT_TRUE(SignatureMatcher::matchPattern(Legacy, Data.data(), Data.size()));
}

TEST(SignatureMatcherIndex, RefinesCollidingPrefixesWithoutDuplicates) {
  std::vector<PatternModule> Modules;
  for (unsigned Value = 0; Value != 256; ++Value) {
    PatternModule Module;
    Module.LeadingBytes = {
        {0xAA, false}, {0xBB, false}, {static_cast<uint8_t>(Value), false}};
    Module.TotalLen = 3;
    Modules.push_back(std::move(Module));
  }
  for (unsigned I = 0; I != 8; ++I) {
    PatternModule Wildcard;
    Wildcard.LeadingBytes = {{0xAA, false}, {0xBB, false}, {0, true}};
    Wildcard.TotalLen = 3;
    Modules.push_back(std::move(Wildcard));
  }

  SignatureMatcher::HashIndex Index;
  Index.build(Modules);

  constexpr std::array<uint8_t, 3> Data = {0xAA, 0xBB, 0x42};
  EXPECT_EQ(Index.candidateCount(Data.data(), Data.size()), 9u);

  unsigned Matches = 0;
  SignatureMatcher::scanAtAddresses(
      Data.data(), Data.size(), 0, {0}, Modules, Index,
      [&](uint64_t Address, const PatternModule &) {
        EXPECT_EQ(Address, 0u);
        ++Matches;
      });
  EXPECT_EQ(Matches, 9u);

  constexpr std::array<uint8_t, 3> WrongPrefix = {0xAA, 0xBC, 0x42};
  EXPECT_EQ(Index.candidateCount(WrongPrefix.data(), WrongPrefix.size()), 0u);
}
