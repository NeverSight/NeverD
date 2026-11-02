//===- X86ReferenceBoundaryTests.cpp - Address boundary regressions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prefix-looking operands, genuine prefixes and competing instruction paths.
///
//===----------------------------------------------------------------------===//

#include "../../lib/sigs/X86/X86ImportReference.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sigs;

namespace {
#define NEVERD_SIGS_REFERENCE_LIMIT(Name, Value)                               \
  inline constexpr auto Name = Value;
#include "../../lib/sigs/ReferenceCheckLimits.def"
} // namespace

TEST(X86ReferenceBoundary, OperandBytesDoNotBecomeAddressPrefixes) {
  for (const auto &Prefix : {std::vector<uint8_t>{0x48, 0x8d, 0x0c, 0xf0},
                             std::vector<uint8_t>{0xb8, 0, 0, 0, 0x67},
                             std::vector<uint8_t>{0x48, 0x83, 0xc0, 0x48}}) {
    auto Code = Prefix;
    const uint8_t Lea[] = {0x4c, 0x8d, 0x0d, 0, 0, 0, 0};
    Code.insert(Code.end(), std::begin(Lea), std::end(Lea));
    EXPECT_TRUE(isX64AddressReferenceField(Code, Prefix.size() + 3));
  }
}

TEST(X86ReferenceBoundary, RealPrefixesAndEmbeddedInstructionsStayUnknown) {
  for (uint8_t Prefix : {0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65,
                         0x66, 0x67, 0x40, 0x48}) {
    const uint8_t Code[] = {Prefix, 0x4c, 0x8d, 0x0d, 0, 0, 0, 0};
    EXPECT_FALSE(isX64AddressReferenceField(Code, 4)) << unsigned(Prefix);
  }
  // The apparent LEA is inside MOVABS's eight-byte immediate.
  const uint8_t Immediate[] = {0x48, 0xb8, 0xf0, 0x4c, 0x8d, 0x0d, 0, 0, 0, 0};
  EXPECT_FALSE(isX64AddressReferenceField(Immediate, 6));
}

TEST(X86ReferenceBoundary, ReachabilityAndOverlappingPathsAreChecked) {
  // The fallthrough path treats F0 as MOV's immediate; the taken path enters
  // it as LOCK before LEA. Neither traversal order may authorize this field.
  const uint8_t Overlap[] = {0x74, 0x04, 0xb8, 0, 0, 0, 0xf0,
                             0x4c, 0x8d, 0x0d, 0, 0, 0, 0};
  EXPECT_FALSE(isX64AddressReferenceField(Overlap, 10));
  auto Unreachable =
      std::vector<uint8_t>(std::begin(Overlap), std::end(Overlap));
  Unreachable[0] = 0xeb;
  Unreachable[1] = 0x0c;
  EXPECT_FALSE(isX64AddressReferenceField(Unreachable, 10));
  auto Reachable = std::vector<uint8_t>(std::begin(Overlap), std::end(Overlap));
  Reachable[1] = 0x05; // Both paths reach the complete LEA at offset seven.
  EXPECT_TRUE(isX64AddressReferenceField(Reachable, 10));
  Reachable.pop_back();
  EXPECT_FALSE(isX64AddressReferenceField(Reachable, 10));
}

TEST(X86ReferenceBoundary, DecodingHasABoundedPrefixAndNoLoopGuess) {
  for (bool Exhausted : {false, true}) {
    const size_t Start =
        MaxReferenceBoundaryBytes - x86::kAddressInstructionSize + Exhausted;
    std::vector<uint8_t> Code(Start - 5, 0x90);
    const uint8_t Tail[] = {0xb8, 0, 0, 0, 0xf0, 0x4c, 0x8d, 0x0d, 0, 0, 0, 0};
    Code.insert(Code.end(), std::begin(Tail), std::end(Tail));
    EXPECT_EQ(isX64AddressReferenceField(Code, Start + 3), !Exhausted);
  }
  // LOOP has its own counter/control semantics; it is outside this fallback.
  const uint8_t Loop[] = {0xe2, 0x04, 0xb8, 0, 0, 0, 0xf0,
                          0x4c, 0x8d, 0x0d, 0, 0, 0, 0};
  EXPECT_FALSE(isX64AddressReferenceField(Loop, 10));
  // One path reaches LEA; the other's unknown target might enter its middle.
  const uint8_t Indirect[] = {0x74, 0x02, 0xff, 0xe0, 0xb8, 0, 0, 0,
                              0xf0, 0x4c, 0x8d, 0x0d, 0,    0, 0, 0};
  EXPECT_FALSE(isX64AddressReferenceField(Indirect, 12));
}

TEST(X86ReferenceBoundary, ReusedBuffersCannotInheritAStaleBoundary) {
  std::vector<uint8_t> Code = {0x74, 0x05, 0xb8, 0, 0, 0, 0xf0,
                               0x4c, 0x8d, 0x0d, 0, 0, 0, 0};
  for (bool Overlap : {false, false, true, true, false}) {
    Code[1] = Overlap ? 0x04 : 0x05;
    EXPECT_EQ(isX64AddressReferenceField(Code, 10), !Overlap);
    // Equal bytes at a different allocation have the same boundary; there is
    // no image-address identity in this instruction-only check.
    auto Copy = Code;
    EXPECT_EQ(isX64AddressReferenceField(Copy, 10), !Overlap);
  }
  Code.resize(10);
  EXPECT_FALSE(isX64AddressReferenceField(Code, 10));
}

TEST(X86ReferenceBoundary, IncomingRequestsUseTheSameDecodedAddressBoundary) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = Code.FileSz = 0x200;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(0x200, 0xcc);
  std::fill_n(Code.Data.begin(), 16, 0x90);
  const uint8_t Body[] = {0x48, 0x8d, 0x0c, 0xf0, 0x4c, 0x8d,
                          0x0d, 0,    0,    0,    0,    0xc3};
  std::copy(std::begin(Body), std::end(Body), Code.Data.begin() + 16);
  writeLE<uint32_t>(Code.Data.data() + 23, 0x1100 - 0x101b);
  Code.Data[0x100] = 0x33;
  Code.Data[0x101] = 0xc0;
  Code.Data[0x102] = 0xc3;
  Img.Segments.push_back(std::move(Code));
  SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      "90909090909090909090909090909090488D0CF04C8D0D........C3 00 0000 001C "
      ":0000 parent ^0017 child\n"
      "33C0C3 00 0000 0003 !caller :0000 child\n",
      "boundaries"));
  DB.apply(Img, {0x1000, 0x1100});
  ASSERT_TRUE(DB.buildCandidateNameMap().contains(0x1100));
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "child");
  EXPECT_TRUE(DB.buildNameMap().empty());
}
