//===- RegistrationEH4WrapperTests.cpp - Retained EH4 runtime adapters
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Proves forwarding wrappers without losing their exception-code adjustment.

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

using namespace neverd;

namespace {

BinaryImage normalizedWrapper(uint32_t Base = 0x400000) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Base;
  Image.DynInfo.SecurityCookieRVA = 0x2000;
  for (bool Code : {true, false}) {
    Segment Segment;
    Segment.Name = Code ? ".text" : ".rdata";
    Segment.VA = Base + (Code ? 0x1000 : 0x2000);
    Segment.Size = Segment.FileSz = 0x200;
    Segment.Flags = SegmentFlags::Readable |
                    (Code ? SegmentFlags::Executable : SegmentFlags::None);
    Segment.Data.resize(Segment.Size, Code ? 0xcc : 0);
    Section Section;
    Section.Name = Segment.Name;
    Section.VA = Segment.VA;
    Section.Size = Section.FileSz = Segment.Size;
    Section.Flags = Segment.Flags;
    Image.Segments.push_back(std::move(Segment));
    Image.Sections.push_back(std::move(Section));
  }
  // Keep ESI across the scalar normalization, update only ExceptionCode,
  // then pass the four original arguments, checker and cookie to the CRT.
  const uint8_t Wrapper[] = {
      0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08, 0xff, 0x36, 0xe8, 0,    0,
      0,    0,    0xff, 0x75, 0x14, 0x89, 0x06, 0xff, 0x75, 0x10, 0xff, 0x75,
      0x0c, 0x56, 0x68, 0,    0,    0,    0,    0x68, 0,    0,    0,    0,
      0xe8, 0,    0,    0,    0,    0x83, 0xc4, 0x1c, 0x5e, 0x5d, 0xc3};
  auto &Text = Image.Segments.front().Data;
  std::copy(std::begin(Wrapper), std::end(Wrapper), Text.begin());
  llvm::support::endian::write32le(Text.data() + 10, 0x100 - 14);
  llvm::support::endian::write32le(Text.data() + 27, Base + 0x1080);
  llvm::support::endian::write32le(Text.data() + 32, Base + 0x2000);
  llvm::support::endian::write32le(Text.data() + 37, 0x180 - 41);
  Text[0x80] = 0xc3;
  const uint8_t Scalar[] = {0x55, 0x8b, 0xec, 0x51, 0x8b, 0x45,
                            0x08, 0x8b, 0xe5, 0x5d, 0xc3};
  std::copy(std::begin(Scalar), std::end(Scalar), Text.begin() + 0x100);
  Text[0x180] = 0xff;
  Text[0x181] = 0x25;
  llvm::support::endian::write32le(Text.data() + 0x182, Base + 0x2008);
  Image.Imports.push_back(
      {"vcruntime140.dll", "_except_handler4_common", 0, Base + 0x2008});
  EXPECT_TRUE(
      Image.recordImportStorageSlot(Base + 0x2008, "_except_handler4_common", 0,
                                    ImportStorageEvidence::ImportDirectory));
  return Image;
}

} // namespace

TEST(RegistrationEH4Wrapper, RetainsScalarNormalizationBeforeRuntimeDispatch) {
  for (uint32_t Base : {0x400000u, 0x18000000u}) {
    auto Image = normalizedWrapper(Base);
    EXPECT_EQ(coff_loader::getCheckedX86EH4CookieCheck(Image, Base + 0x1000),
              Base + 0x1080);
    const auto ABI =
        coff_loader::getCheckedX86EH4PersonalityABI(Image, Base + 0x1000);
    ASSERT_TRUE(ABI);
    EXPECT_EQ(ABI->IATVA, Base + 0x2008);
    EXPECT_EQ(ABI->CookieCheckVA, Base + 0x1080);
    EXPECT_EQ(ABI->SecurityCookieVA, Base + 0x2000);
    EXPECT_TRUE(ABI->ImageReads.empty());
    ASSERT_EQ(ABI->CodeRanges.size(), 3u);
    EXPECT_EQ(ABI->CodeRanges[0].Begin, Base + 0x1000);
    EXPECT_EQ(ABI->CodeRanges[0].End, Base + 0x102f);
    EXPECT_EQ(ABI->CodeRanges[1].Begin, Base + 0x1100);
    EXPECT_EQ(ABI->CodeRanges[1].End, Base + 0x110b);
    EXPECT_EQ(ABI->CodeRanges[2].Begin, Base + 0x1180);
    EXPECT_EQ(ABI->CodeRanges[2].End, Base + 0x1186);
    for (unsigned Mode = 0; Mode != 5; ++Mode) {
      auto Bad = Image;
      if (Mode == 0)
        Bad.ImportStorageSlots.erase(Base + 0x2008);
      if (Mode == 1)
        Bad.ImportStorageSlots.at(Base + 0x2008).Addend = 1;
      if (Mode == 2)
        Bad.ConflictingImportStorageSlots.insert(Base + 0x2008);
      if (Mode == 3)
        Bad.Segments.back().FileSz = 10;
      if (Mode == 4)
        Bad.Segments.push_back(Bad.Segments.back());
      EXPECT_FALSE(
          coff_loader::getCheckedX86EH4PersonalityABI(Bad, Base + 0x1000));
    }
  }
}

TEST(RegistrationEH4Wrapper, RejectsChangedArgumentsStackAndRuntimeIdentity) {
  for (unsigned Offset : {3,  6,  7,  8,  16, 17, 18, 21, 24, 25,
                          26, 31, 32, 36, 41, 42, 43, 44, 45, 46}) {
    auto Image = normalizedWrapper();
    Image.Segments.front().Data[Offset] ^= 0x80;
    EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Image, 0x401000))
        << Offset;
  }
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Image = normalizedWrapper();
    if (Mutation == 0)
      Image.Imports.front().Module = "other.dll";
    if (Mutation == 1)
      Image.Imports.front().Name = "different_handler";
    if (Mutation == 2)
      Image.Segments.front().Data[0x180] = 0xc3;
    if (Mutation == 3)
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
    if (Mutation == 4)
      llvm::support::endian::write32le(Image.Segments.front().Data.data() + 10,
                                       uint32_t(-5));
    EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Image, 0x401000))
        << Mutation;
  }
}

TEST(RegistrationEH4Wrapper, NormalizerCannotObserveOrEscapeItsCallerFrame) {
  for (const std::vector<uint8_t> Body :
       {std::vector<uint8_t>{0x8b, 0x45, 0x04}, // Return PC.
        {0x8b, 0x45, 0x00},                     // Saved caller EBP.
        {0x8b, 0x45, 0xfc},                     // Uninitialized scratch.
        {0x89, 0xe8},                           // Escape frame address.
        {0x89, 0xf0},                           // Incoming ESI.
        {0xeb, 0xfe},                           // A cycle.
        {0xe8, 0, 0, 0, 0},                     // Another call.
        {0xc2, 4, 0},                           // Callee cleanup.
        {0x89, 0x45, 0x08},                     // Caller argument write.
        {0x64, 0xa1, 0, 0, 0, 0}}) {            // FS chain observation.
    auto Image = normalizedWrapper();
    auto &Text = Image.Segments.front().Data;
    std::fill(Text.begin() + 0x104, Text.begin() + 0x180, 0xcc);
    std::copy(Body.begin(), Body.end(), Text.begin() + 0x104);
    const uint8_t Return[] = {0x8b, 0xe5, 0x5d, 0xc3};
    std::copy(std::begin(Return), std::end(Return),
              Text.begin() + 0x104 + Body.size());
    EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Image, 0x401000));
  }
}

TEST(RegistrationEH4Wrapper, RetainsRuntimeDataAndMXCSRBranches) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool Writable : {false, true}) {
      auto Image = normalizedWrapper(Base);
      if (Writable) {
        Image.Segments.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        Image.Sections.back().Flags = Image.Segments.back().Flags;
      }
      // Read the runtime ISA level and preserve both paths of a normalizer:
      // return the input, or select an exception code using the current MXCSR.
      std::vector<uint8_t> Scalar = {
          0x55, 0x8b, 0xec, 0x51, 0x83, 0x3d, 0,    0,    0,    0,    1,
          0x8b, 0x4d, 0x08, 0x7c, 20,   0x0f, 0xae, 0x5d, 0xfc, 0x8b, 0x45,
          0xfc, 0x83, 0xf0, 0x3f, 0xa8, 0x81, 0xb8, 0x8e, 0,    0,    0xc0,
          0x0f, 0x44, 0xc8, 0x8b, 0xc1, 0x8b, 0xe5, 0x5d, 0xc3};
      llvm::support::endian::write32le(Scalar.data() + 6, Base + 0x2010);
      std::copy(Scalar.begin(), Scalar.end(),
                Image.Segments.front().Data.begin() + 0x100);
      EXPECT_EQ(coff_loader::getCheckedX86EH4CookieCheck(Image, Base + 0x1000),
                Base + 0x1080);
      const auto ABI =
          coff_loader::getCheckedX86EH4PersonalityABI(Image, Base + 0x1000);
      ASSERT_TRUE(ABI);
      ASSERT_EQ(ABI->CodeRanges.size(), 3u);
      EXPECT_EQ(ABI->CodeRanges[1].Begin, Base + 0x1100);
      EXPECT_EQ(ABI->CodeRanges[1].End, Base + 0x1100 + Scalar.size());
      ASSERT_EQ(ABI->ImageReads.size(), 1u);
      EXPECT_EQ(ABI->ImageReads[0].Begin, Base + 0x2010);
      EXPECT_EQ(ABI->ImageReads[0].End, Base + 0x2014);
      for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
        auto Bad = Image;
        if (Mutation == 0)
          Bad.Segments.front().Data[0x113] = 8; // MXCSR into caller argument.
        if (Mutation == 1)
          Bad.Segments.front().Data[0x10f] = 21; // Enter a MOV operand.
        if (Mutation == 2)
          Bad.Sections.back().Size = 0x12; // Incomplete runtime data cell.
        if (Mutation == 3)
          Bad.Segments.push_back(Bad.Segments.back()); // Ambiguous storage.
        EXPECT_FALSE(
            coff_loader::getCheckedX86EH4CookieCheck(Bad, Base + 0x1000))
            << Mutation;
      }
    }
}

TEST(RegistrationEH4Wrapper, DirectDependenciesIncludeHotpatchAndEveryVeneer) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool Hotpatch : {false, true})
      for (bool Indirect : {false, true}) {
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Hotpatch);
        SCOPED_TRACE(Indirect);
        auto Image = normalizedWrapper(Base);
        auto &Text = Image.Segments.front().Data;
        std::vector<uint8_t> Code;
        if (Hotpatch)
          Code.insert(Code.end(), {0x8b, 0xff});
        const size_t Begin = Code.size();
        const uint8_t Prefix[] = {0x55, 0x89, 0xe5, 0xff, 0x75, 20,
                                  0xff, 0x75, 16,   0xff, 0x75, 12,
                                  0xff, 0x75, 8,    0x68};
        Code.insert(Code.end(), std::begin(Prefix), std::end(Prefix));
        auto Word = [&](uint32_t Value) {
          const auto At = Code.size();
          Code.resize(At + 4);
          llvm::support::endian::write32le(Code.data() + At, Value);
        };
        Word(Base + 0x1080);
        Code.push_back(0x68);
        Word(Base + 0x2000);
        if (Indirect) {
          Code.insert(Code.end(), {0xff, 0x15});
          Word(Base + 0x2008);
        } else {
          Code.push_back(0xe8);
          Word(uint32_t(0x170 - Begin - 30));
          Text[0x170] = 0xe9;
          llvm::support::endian::write32le(Text.data() + 0x171, 11);
        }
        Code.insert(Code.end(), {0x83, 0xc4, 24, 0x5d, 0xc3});
        std::copy(Code.begin(), Code.end(), Text.begin());
        const auto ABI =
            coff_loader::getCheckedX86EH4PersonalityABI(Image, Base + 0x1000);
        ASSERT_TRUE(ABI);
        EXPECT_EQ(ABI->IATVA, Base + 0x2008);
        EXPECT_EQ(ABI->CookieCheckVA, Base + 0x1080);
        EXPECT_EQ(ABI->SecurityCookieVA, Base + 0x2000);
        EXPECT_TRUE(ABI->ImageReads.empty());
        ASSERT_EQ(ABI->CodeRanges.size(), Indirect ? 1u : 3u);
        EXPECT_EQ(ABI->CodeRanges[0].Begin, Base + 0x1000);
        EXPECT_EQ(ABI->CodeRanges[0].End, Base + 0x1000 + Code.size());
        if (!Indirect) {
          EXPECT_EQ(ABI->CodeRanges[1].Begin, Base + 0x1170);
          EXPECT_EQ(ABI->CodeRanges[1].End, Base + 0x1175);
          EXPECT_EQ(ABI->CodeRanges[2].Begin, Base + 0x1180);
          EXPECT_EQ(ABI->CodeRanges[2].End, Base + 0x1186);
        }
      }
}

TEST(RegistrationEH4Wrapper, EveryReturnRequiresAnInitializedScalar) {
  auto Image = normalizedWrapper();
  // The taken branch reaches the epilogue without ever defining EAX. A
  // definition on the other path must not be used to authorize that return.
  const uint8_t Body[] = {0x8b, 0x4d, 8, 0x85, 0xc9, 0x74, 5,    0xb8,
                          1,    0,    0, 0,    0x8b, 0xe5, 0x5d, 0xc3};
  std::copy(std::begin(Body), std::end(Body),
            Image.Segments.front().Data.begin() + 0x104);
  EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Image, 0x401000));
}

TEST(RegistrationEH4Wrapper, RelocationsMustMatchTheDecodedAddressOperands) {
  auto Image = normalizedWrapper();
  for (va_t Address : {0x40101bu, 0x401020u, 0x401182u})
    Image.BaseRelocations.push_back(
        {Address, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
  ASSERT_EQ(coff_loader::getCheckedX86EH4CookieCheck(Image, 0x401000),
            0x401080u);
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Bad = Image;
    if (Mutation == 0)
      Bad.BaseRelocations.push_back(Bad.BaseRelocations.front());
    if (Mutation == 1)
      Bad.BaseRelocations.front().Type = llvm::COFF::IMAGE_REL_BASED_DIR64;
    if (Mutation == 2)
      ++Bad.BaseRelocations.front().Address;
    if (Mutation == 3)
      Bad.BaseRelocations.push_back(
          {0x401100u, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    if (Mutation == 4)
      Bad.Imports.push_back(Bad.Imports.front());
    EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Bad, 0x401000))
        << Mutation;
  }
}

TEST(RegistrationEH4Wrapper, DependenciesCoverBothPathsWithoutOwningGaps) {
  auto Image = normalizedWrapper();
  const uint8_t Scalar[] = {
      0x55, 0x8b, 0xec, 0x51, 0x8b, 0x45, 8,    0x85, 0xc0, 0x74, 5,    0xeb, 8,
      0xcc, 0xcc, 0xcc, 0x83, 0xf0, 1,    0xeb, 0,    0x8b, 0xe5, 0x5d, 0xc3};
  std::copy(std::begin(Scalar), std::end(Scalar),
            Image.Segments.front().Data.begin() + 0x100);
  const auto ABI = coff_loader::getCheckedX86EH4PersonalityABI(Image, 0x401000);
  ASSERT_TRUE(ABI);
  ASSERT_EQ(ABI->CodeRanges.size(), 4u);
  EXPECT_EQ(ABI->CodeRanges[1].Begin, 0x401100u);
  EXPECT_EQ(ABI->CodeRanges[1].End, 0x40110du);
  EXPECT_EQ(ABI->CodeRanges[2].Begin, 0x401110u);
  EXPECT_EQ(ABI->CodeRanges[2].End, 0x401119u);
  for (unsigned Offset : {0x104, 0x110, 0x115}) {
    auto Bad = Image;
    Bad.Segments.front().Data[Offset] = 0xcc;
    EXPECT_FALSE(coff_loader::getCheckedX86EH4PersonalityABI(Bad, 0x401000));
  }
  Image.Segments.front().Data[0x10d] = 0xc3;
  EXPECT_TRUE(coff_loader::getCheckedX86EH4PersonalityABI(Image, 0x401000));
}
