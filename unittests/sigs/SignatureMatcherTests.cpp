//===- SignatureMatcherTests.cpp - Signature matcher tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"
#include "neverd/support/Parallel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
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

namespace {

std::string hexByte(unsigned Value) {
  static const char Digits[] = "0123456789ABCDEF";
  return {Digits[(Value >> 4) & 0xF], Digits[Value & 0xF]};
}

std::string hexWord(unsigned Value) {
  return hexByte(Value >> 8) + hexByte(Value & 0xFF);
}

/// The packed modules of \p Text, and the memory they point into.
struct PackedText {
  std::vector<PatternChunk> Chunks;
  std::unique_ptr<uint8_t[]> Bytes;
  std::vector<StoredModule> Modules;

  explicit PackedText(llvm::StringRef Text, size_t ChunkBytes = 4096) {
    Chunks = PatternParser::splitChunks(Text, ChunkBytes);
    Bytes = PatternParser::reserveBytes(Chunks);
    PatternParser::parseChunks(Chunks);
    for (const PatternChunk &Chunk : Chunks) {
      EXPECT_FALSE(Chunk.ErrorLine) << Chunk.Error;
      Modules.insert(Modules.end(), Chunk.Modules.begin(), Chunk.Modules.end());
    }
  }
};

} // namespace

TEST(SignatureMatcherForms, PackedModulesMatchExactlyAsTheirValues) {
  std::mt19937 Random(7);
  unsigned Matched = 0;
  for (unsigned Case = 0; Case < 3000; ++Case) {
    // Code, and a module describing it: a leading pattern, a CRC span, and a
    // tail, with wildcards, sometimes a wrong CRC or a changed byte.
    const size_t Leading = 1 + Random() % 40, CRCLen = Random() % 8;
    const size_t Tail = Random() % 90;
    std::vector<uint8_t> Code(Leading + CRCLen + Tail + 8);
    for (uint8_t &Byte : Code)
      Byte = static_cast<uint8_t>(Random());
    size_t Total = 1 + Random() % (Leading + CRCLen + Tail + 2);
    if (CRCLen != 0)
      Total = std::max(Total, Leading + CRCLen);
    const uint16_t CRC =
        SignatureMatcher::computeCRC16(Code.data() + Leading, CRCLen);
    std::string Line;
    for (size_t I = 0; I < Leading; ++I)
      Line += Random() % 5 == 0 ? ".." : hexByte(Code[I]);
    Line += " " + hexByte(static_cast<unsigned>(CRCLen)) + " " +
            hexWord(Random() % 4 == 0 ? CRC ^ 1 : CRC) + " " +
            hexWord(static_cast<unsigned>(Total)) + " :0000 routine";
    if (Tail != 0) {
      Line += ' ';
      for (size_t I = 0; I < Tail; ++I)
        Line += Random() % 5 == 0 ? ".." : hexByte(Code[Leading + CRCLen + I]);
    }
    if (Random() % 3 == 0)
      Code[Random() % Code.size()] ^= 0x10;
    SCOPED_TRACE(Line);

    auto Value = PatternParser::parseLine(Line);
    ASSERT_TRUE(static_cast<bool>(Value)) << llvm::toString(Value.takeError());
    PackedText Packed(Line);
    ASSERT_EQ(Packed.Modules.size(), 1u);
    const StoredModule &Stored = Packed.Modules.front();

    for (size_t Available : {Code.size(), Total, Total - 1, Leading}) {
      const bool Expected =
          SignatureMatcher::matchPattern(*Value, Code.data(), Available);
      EXPECT_EQ(SignatureMatcher::matchPattern(Stored, Code.data(), Available),
                Expected)
          << Available;
      Matched += Expected;
    }
    EXPECT_EQ(SignatureMatcher::fixedByteCount(Stored),
              SignatureMatcher::fixedByteCount(*Value));
    EXPECT_EQ(SignatureMatcher::isFullyVerified(Stored),
              SignatureMatcher::isFullyVerified(*Value));
  }
  EXPECT_GT(Matched, 1000u) << "the cases must include matches";
}

TEST(SignatureMatcherIndex, ParallelBuildMatchesTheSingleThreadedOne) {
  // Enough modules for the subtrees to be built on several threads, spread
  // over many first bytes, with shared prologues and wildcards.
  std::mt19937 Random(11);
  std::string Text;
  for (unsigned I = 0; I < 40000; ++I) {
    std::string Leading;
    const unsigned Family = Random() % 6;
    for (unsigned Byte = 0; Byte < 32; ++Byte) {
      if (Family == 0 && Byte < 4)
        Leading += hexByte(0xF30F1EFAu >> (24 - 8 * Byte));
      else if (Random() % 9 == 0)
        Leading += "..";
      else
        Leading += hexByte(Byte < 3 ? Random() % (Family + 2) + Family * 40
                                    : Random());
    }
    Text += Leading + " 00 0000 0040 :0000 routine_" + std::to_string(I) + "\n";
  }
  PackedText Packed(Text);
  ASSERT_EQ(Packed.Modules.size(), 40000u);

  auto BuildWith = [&](unsigned Threads) {
    neverd::setWorkerThreadCount(Threads);
    SignatureMatcher::HashIndex Index;
    Index.build(Packed.Modules);
    neverd::setWorkerThreadCount(0);
    return Index;
  };
  const SignatureMatcher::HashIndex One = BuildWith(1);
  const SignatureMatcher::HashIndex Many = BuildWith(8);

  ASSERT_EQ(Many.Root, One.Root);
  ASSERT_EQ(Many.Nodes.size(), One.Nodes.size());
  for (size_t I = 0; I < One.Nodes.size(); ++I) {
    const auto &A = One.Nodes[I], &B = Many.Nodes[I];
    ASSERT_EQ(B.Offset, A.Offset) << I;
    ASSERT_EQ(B.WildcardChild, A.WildcardChild) << I;
    ASSERT_EQ(B.Candidates, A.Candidates) << I;
    ASSERT_EQ(B.ExactChildren.size(), A.ExactChildren.size()) << I;
    for (size_t E = 0; E < A.ExactChildren.size(); ++E) {
      ASSERT_EQ(B.ExactChildren[E].Value, A.ExactChildren[E].Value) << I;
      ASSERT_EQ(B.ExactChildren[E].Child, A.ExactChildren[E].Child) << I;
    }
  }
}

TEST(SignatureMatcherIndex, SharedProloguesRetainExactAndWildcardDecisions) {
  // Two versions of each suffix share a long prologue, but only one states
  // byte 15. Uniform wildcard columns occur on both sides of that decision.
  std::string Text;
  for (unsigned Suffix = 0; Suffix < 256; ++Suffix) {
    for (bool Wild : {false, true}) {
      for (unsigned Byte = 0; Byte < 32; ++Byte)
        Text += Byte == 3 || Byte == 20 || (Wild && Byte == 15)
                    ? ".."
                    : hexByte(Byte == 31 ? Suffix : 0x90);
      Text += " 00 0000 0020 :0000 routine_" + std::to_string(Suffix) +
              (Wild ? "_wild\n" : "_exact\n");
    }
  }
  PackedText Packed(Text);
  ASSERT_EQ(Packed.Modules.size(), 512u);
  SignatureMatcher::HashIndex Index;
  Index.build(Packed.Modules);
  std::array<uint8_t, 32> Code;
  Code.fill(0x90);
  const auto Matched = [&](size_t Available) {
    std::vector<size_t> Result;
    for (const auto &Hit : SignatureMatcher::findAtAddresses(
             Code.data(), Available, 0, {0}, Packed.Modules, Index))
      Result.push_back(Hit.Module);
    return Result;
  };
  for (unsigned Suffix = 0; Suffix < 256; ++Suffix) {
    Code[31] = Suffix;
    // Unstated bytes never become common-prefix requirements.
    Code[3] = Suffix;
    Code[20] = Suffix ^ 0xff;
    Code[15] = 0x90;
    EXPECT_EQ(Matched(Code.size()),
              (std::vector<size_t>{2 * Suffix + 1, 2 * Suffix}));
    EXPECT_EQ(Index.candidateCount(Code.data(), Code.size()), 2u);
    Code[15] = 0xcc;
    EXPECT_EQ(Matched(Code.size()), (std::vector<size_t>{2 * Suffix + 1}));
    EXPECT_EQ(Index.candidateCount(Code.data(), Code.size()), 1u);
  }
  for (size_t Available = 0; Available < Code.size(); ++Available)
    EXPECT_TRUE(Matched(Available).empty()) << Available;
  Code[2] = 0xcc;
  EXPECT_TRUE(Matched(Code.size()).empty());
  EXPECT_EQ(Index.candidateCount(Code.data(), Code.size()), 0u);
}

TEST(SignatureMatcherIndex, CopiesRetainCandidatesAcrossRebuildAndMove) {
  std::string Text;
  for (unsigned I = 0; I < 128; ++I)
    Text += hexByte(I) + "0102030405 00 0000 0006 :0000 routine_" +
            std::to_string(I) + "\n";
  PackedText Packed(Text);
  SignatureMatcher::HashIndex Copy;
  {
    SignatureMatcher::HashIndex Original;
    Original.build(Packed.Modules);
    Copy = Original;
    Original.build(llvm::ArrayRef<StoredModule>{});
  }
  SignatureMatcher::HashIndex Moved(std::move(Copy));
  Copy.build(llvm::ArrayRef<StoredModule>{});
  const SignatureMatcher::HashIndex Retained(Moved);
  Moved.build(llvm::ArrayRef<StoredModule>{});
  for (unsigned I = 0; I < 128; ++I) {
    const uint8_t Code[] = {static_cast<uint8_t>(I), 1, 2, 3, 4, 5};
    const auto Hits = SignatureMatcher::findAtAddresses(
        Code, sizeof(Code), 0, {0}, Packed.Modules, Retained);
    ASSERT_EQ(Hits.size(), 1u) << I;
    EXPECT_EQ(Hits.front().Module, I);
    EXPECT_EQ(Retained.candidateCount(Code, sizeof(Code)), 1u);
  }
}

namespace {

using HitPair = std::pair<uint64_t, size_t>;

std::vector<HitPair> hitPairs(llvm::ArrayRef<SignatureMatcher::Hit> Hits) {
  std::vector<HitPair> Result;
  for (const auto &Hit : Hits)
    Result.emplace_back(Hit.Address, Hit.Module);
  return Result;
}

std::string crcPattern(llvm::ArrayRef<uint8_t> Code, size_t Leading,
                       uint8_t Length, unsigned Variant = 0) {
  std::string Line;
  for (size_t I = 0; I < Leading; ++I)
    Line += hexByte(Code[I]);
  const uint16_t CRC =
      SignatureMatcher::computeCRC16(Code.data() + Leading, Length);
  Line += " " + hexByte(Length) + " " + hexWord(CRC ^ (Variant == 1)) + " " +
          hexWord(Leading + Length + 1) + " :0000 routine ";
  return Line + hexByte(Code[Leading + Length] ^ (Variant == 2)) + "\n";
}

} // namespace

TEST(SignatureMatcherCRCReuse, EveryLengthKeepsCRCAndTailChecks) {
  std::array<uint8_t, 320> Code;
  for (size_t I = 0; I < Code.size(); ++I)
    Code[I] = static_cast<uint8_t>(I * 17 + 3);
  std::string Text;
  for (unsigned Variant = 0; Variant != 3; ++Variant)
    for (unsigned Length = 1; Length != 256; ++Length)
      Text += crcPattern(Code, 32, Length, Variant);
  PackedText Packed(Text);
  SignatureMatcher::HashIndex Index;
  Index.build(Packed.Modules);

  // Repeated addresses cross worker-block boundaries. An out-of-range entry,
  // a wrong leading byte, and a truncated input must not reuse valid results.
  constexpr uint64_t Base = 0x1000;
  std::vector<uint64_t> Entries(40, Base);
  Entries.insert(Entries.end(), {Base - 1, Base + 1, Base + Code.size() - 1});
  std::vector<HitPair> Expected;
  for (uint64_t Address : Entries) {
    if (Address < Base || Address - Base >= Code.size())
      continue;
    for (size_t M = 0; M < Packed.Modules.size(); ++M)
      if (SignatureMatcher::matchPattern(Packed.Modules[M],
                                         Code.data() + (Address - Base),
                                         Code.size() - (Address - Base)))
        Expected.emplace_back(Address, M);
  }
  ASSERT_EQ(Expected.size(), 40u * 255u);

  SignatureMatcher::ScanStatistics One, Many;
  neverd::setWorkerThreadCount(1);
  const auto Sequential = SignatureMatcher::findAtAddresses(
      Code.data(), Code.size(), Base, Entries, Packed.Modules, Index, One);
  neverd::setWorkerThreadCount(8);
  const auto Parallel = SignatureMatcher::findAtAddresses(
      Code.data(), Code.size(), Base, Entries, Packed.Modules, Index, Many);
  const auto Untracked = SignatureMatcher::findAtAddresses(
      Code.data(), Code.size(), Base, Entries, Packed.Modules, Index);
  neverd::setWorkerThreadCount(0);

  EXPECT_EQ(hitPairs(Sequential), Expected);
  EXPECT_EQ(hitPairs(Parallel), Expected);
  EXPECT_EQ(hitPairs(Untracked), Expected);
  EXPECT_EQ(One.Entries, 42u);
  EXPECT_EQ(One.CRCRequests, 40u * 3u * 255u);
  EXPECT_EQ(One.CRCComputations, 40u * 255u);
  EXPECT_EQ(One.CRCBytes, 40u * (255u * 256u / 2u));
  EXPECT_EQ(Many.Entries, One.Entries);
  EXPECT_EQ(Many.Candidates, One.Candidates);
  EXPECT_EQ(Many.CRCRequests, One.CRCRequests);
  EXPECT_EQ(Many.CRCComputations, One.CRCComputations);
  EXPECT_EQ(Many.CRCBytes, One.CRCBytes);
}

TEST(SignatureMatcherCRCReuse, SharedWindowWorkDoesNotGrowWithLibraryCount) {
  std::array<uint8_t, 96> Code{};
  std::string Text;
  for (unsigned Library = 0; Library != 1024; ++Library)
    Text += crcPattern(Code, 32, 63, 1);
  PackedText Packed(Text);
  SignatureMatcher::HashIndex Index;
  Index.build(Packed.Modules);
  SignatureMatcher::ScanStatistics Statistics;
  EXPECT_TRUE(SignatureMatcher::findAtAddresses(Code.data(), Code.size(), 0,
                                                {0}, Packed.Modules, Index,
                                                Statistics)
                  .empty());
  EXPECT_EQ(Statistics.Candidates, 1024u);
  EXPECT_EQ(Statistics.CRCRequests, 1024u);
  EXPECT_EQ(Statistics.CRCComputations, 1u);
  EXPECT_EQ(Statistics.CRCBytes, 63u);

  // The call replaces counters even when it has no input to scan.
  EXPECT_TRUE(SignatureMatcher::findAtAddresses(nullptr, Code.size(), 0, {0},
                                                Packed.Modules, Index,
                                                Statistics)
                  .empty());
  EXPECT_EQ(Statistics.Entries, 0u);
  EXPECT_EQ(Statistics.Candidates, 0u);
  EXPECT_EQ(Statistics.CRCRequests, 0u);
  EXPECT_EQ(Statistics.CRCComputations, 0u);
  EXPECT_EQ(Statistics.CRCBytes, 0u);
}

TEST(SignatureMatcherCRCReuse, LeadingOffsetsAndInputChangesStayIndependent) {
  std::array<uint8_t, 80> Code;
  for (size_t I = 0; I < Code.size(); ++I)
    Code[I] = static_cast<uint8_t>(I * I + 1);
  std::string Text;
  // The same length describes distinct windows, including nonstandard
  // leading widths greater than the byte index's width.
  for (size_t Leading : {16u, 32u, 40u, 16u, 40u})
    Text += crcPattern(Code, Leading, 8);
  PackedText Packed(Text);
  SignatureMatcher::HashIndex Index;
  Index.build(Packed.Modules);
  auto Check = [&](size_t Available) {
    std::vector<HitPair> Expected;
    for (size_t M = 0; M < Packed.Modules.size(); ++M)
      if (SignatureMatcher::matchPattern(Packed.Modules[M], Code.data(),
                                         Available))
        Expected.emplace_back(0, M);
    SignatureMatcher::ScanStatistics Statistics;
    EXPECT_EQ(hitPairs(SignatureMatcher::findAtAddresses(Code.data(), Available,
                                                         0, {0}, Packed.Modules,
                                                         Index, Statistics)),
              Expected);
    return Statistics;
  };
  EXPECT_EQ(Check(Code.size()).CRCRequests, 5u);
  EXPECT_EQ(Check(20).CRCRequests, 0u);
  Code[47] ^= 1;
  Check(Code.size());
  Code[47] ^= 1;
  Code[24] ^= 1;
  Check(Code.size());
}

TEST(SignatureMatcherCRCReuse, ValueScansAgreeWithIndependentPatternChecks) {
  std::array<uint8_t, 80> Code;
  for (size_t I = 0; I < Code.size(); ++I)
    Code[I] = static_cast<uint8_t>((I / 8) * 7);
  std::string Text;
  for (size_t Offset : {0u, 3u, 16u})
    for (size_t Leading : {4u, 16u, 40u})
      for (unsigned Variant = 0; Variant != 3; ++Variant)
        Text += crcPattern(llvm::ArrayRef(Code).drop_front(Offset), Leading, 8,
                           Variant);
  auto Modules = PatternParser::parseText(Text);
  ASSERT_TRUE(static_cast<bool>(Modules))
      << llvm::toString(Modules.takeError());
  std::vector<HitPair> Expected, Region, Entries;
  std::vector<uint64_t> Addresses;
  for (size_t Offset = 0; Offset < Code.size(); ++Offset) {
    Addresses.push_back(Offset);
    for (size_t M = 0; M < Modules->size(); ++M)
      if (SignatureMatcher::matchPattern((*Modules)[M], Code.data() + Offset,
                                         Code.size() - Offset))
        Expected.emplace_back(Offset, M);
  }
  ASSERT_FALSE(Expected.empty());
  SignatureMatcher::scanRegion(
      Code.data(), Code.size(), *Modules,
      [&](uint64_t Offset, const PatternModule &Module) {
        Region.emplace_back(Offset, &Module - Modules->data());
      });
  SignatureMatcher::scanAtAddresses(
      Code.data(), Code.size(), 0, Addresses, *Modules,
      [&](uint64_t Offset, const PatternModule &Module) {
        Entries.emplace_back(Offset, &Module - Modules->data());
      });
  EXPECT_EQ(Region, Expected);
  EXPECT_EQ(Entries, Expected);
}

TEST(SignatureMatcherCRCReuse, ExternalCallbacksCanChangeInputBytes) {
  std::array<uint8_t, 41> Code{};
  const std::string Line = crcPattern(Code, 32, 8);
  auto Modules = PatternParser::parseText(Line + Line);
  ASSERT_TRUE(static_cast<bool>(Modules))
      << llvm::toString(Modules.takeError());
  unsigned Matches = 0;
  auto Mutate = [&](uint64_t, const PatternModule &) {
    ++Matches;
    Code[32] ^= 1;
  };
  SignatureMatcher::scanAtAddresses(Code.data(), Code.size(), 0, {0}, *Modules,
                                    Mutate);
  EXPECT_EQ(Matches, 1u);
  Code.fill(0);
  Matches = 0;
  SignatureMatcher::scanRegion(Code.data(), Code.size(), *Modules, Mutate);
  EXPECT_EQ(Matches, 1u);
}

TEST(SignatureMatcherIndex, CallerRecordsDoNotIncreaseByteScanWork) {
  std::string Text = "33C0C3 00 0000 0003 :0000 ordinary\n";
  for (unsigned I = 0; I < 4096; ++I)
    Text += "33C0C3 00 0000 0003 !caller :0000 leaf" + std::to_string(I) + "\n";
  auto Chunks = PatternParser::splitChunks(Text);
  const auto Bytes = PatternParser::reserveBytes(Chunks);
  PatternParser::parseChunks(Chunks);
  ASSERT_FALSE(PatternParser::firstError(Chunks));
  std::vector<StoredModule> Modules;
  std::vector<PatternModule> Values;
  for (const auto &Chunk : Chunks)
    for (const auto &Mod : Chunk.Modules) {
      Modules.push_back(Mod);
      Values.push_back(PatternParser::toPatternModule(Mod));
    }
  const uint8_t Code[] = {0x33, 0xc0, 0xc3};
  SignatureMatcher::HashIndex Index;
  Index.build(Modules);
  EXPECT_EQ(Index.candidateCount(Code, sizeof(Code)), 1u);
  ASSERT_EQ(Index.callerModules(Modules, "leaf1234").size(), 1u);
  EXPECT_TRUE(Index.callerModules(Modules, "missing").empty());
  auto Copy = Index;
  Index.build(std::vector<PatternModule>{});
  auto Moved = std::move(Copy);
  ASSERT_EQ(Moved.callerModules(Modules, "leaf1234").size(), 1u);
  EXPECT_EQ(Moved.candidateCount(Code, sizeof(Code)), 1u);
  SignatureMatcher::ScanStatistics Work;
  const auto Hits = SignatureMatcher::findAtAddresses(
      Code, sizeof(Code), 0x1000, {0x1000}, Modules, Moved, Work);
  EXPECT_EQ(Hits.size(), 1u);
  EXPECT_EQ(Work.Candidates, 1u);
  Index.build(Values);
  EXPECT_EQ(Index.candidateCount(Code, sizeof(Code)), 1u);
}
