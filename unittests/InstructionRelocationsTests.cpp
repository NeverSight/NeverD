//===- InstructionRelocationsTests.cpp - Field selection boundaries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare occurrence selection with independent linear range membership.
/// These tests inspect metadata and do not execute instructions.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/InstructionRelocations.h"
#include "neverd/loader/BinaryImage.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace neverd {
namespace {

BinaryImage relocationImage(Arch A, BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Format = Format;
  Image.Bits =
      A == Arch::X86 || A == Arch::ARM ? Bitness::Bits32 : Bitness::Bits64;
  for (va_t VA : {0x8000u, 0x9000u}) {
    Segment S;
    S.VA = VA;
    S.Size = S.FileSz = 64;
    S.Data.resize(64);
    S.Flags = SegmentFlags::Readable;
    if (VA == 0x9000)
      S.Flags = S.Flags | SegmentFlags::Executable;
    Image.Segments.push_back(std::move(S));
  }
  for (va_t VA : {0x1000u, 0x1008u, 0x1020u})
    Image.DataAddressRelocOperands[VA] = {0x8010, 0x8010, 8, 0x8000};
  for (va_t VA : {0x1004u, 0x1020u})
    Image.CodeAddressRelocOperands[VA] = {0x9010, 0x9010, 4, 0x9000};
  return Image;
}

void expectLinearSelection(const BinaryImage &Image, va_t Start,
                           uint16_t Size) {
  // Independently visit every record. Only its start must lie inside the
  // instruction; architecture consumers still authenticate the whole field.
  const va_t End = Start + Size;
  std::vector<RelocatedAddressOperand> Expected;
  for (bool Code : {false, true}) {
    const auto &Fields =
        Code ? Image.CodeAddressRelocOperands : Image.DataAddressRelocOperands;
    for (const auto &[VA, Field] : Fields)
      if (VA >= Start && VA < End)
        Expected.push_back({VA, Field.EncodedValue, Field.TargetVA, Field.Width,
                            Code ? ConstantAddressProvenance::CodeAddress
                                 : ConstantAddressProvenance::DataAddress,
                            Field.TargetOwnerVA, false});
  }
  const auto Actual =
      instructionRelocations(Image, DecodedInsn{Start, Size, 0, nullptr});
  ASSERT_TRUE(Actual.Scalars.empty());
  ASSERT_EQ(Actual.Addresses.size(), Expected.size());
  for (size_t I = 0; I != Expected.size(); ++I) {
    const auto &A = Actual.Addresses[I];
    const auto &E = Expected[I];
    EXPECT_EQ(A.FieldVA, E.FieldVA);
    EXPECT_EQ(A.EncodedValue, E.EncodedValue);
    EXPECT_EQ(A.TargetVA, E.TargetVA);
    EXPECT_EQ(A.Width, E.Width);
    EXPECT_EQ(A.Provenance, E.Provenance);
    EXPECT_EQ(A.TargetOwnerVA, E.TargetOwnerVA);
    EXPECT_EQ(A.PCRelativeFromInstructionEnd, E.PCRelativeFromInstructionEnd);
  }
}

TEST(InstructionRelocations, AllArchitecturesKeepHalfOpenFieldSelection) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(int(A));
      SCOPED_TRACE(int(Format));
      const auto Image = relocationImage(A, Format);
      for (va_t Start : {uint64_t(0), uint64_t(0xff8), uint64_t(0xfff),
                         uint64_t(0x1000), uint64_t(0x1001), uint64_t(0x1004),
                         uint64_t(0x1008), uint64_t(0x101f), uint64_t(0x1020),
                         uint64_t(0x1021), UINT64_MAX - 2, UINT64_MAX})
        for (uint16_t Size : {0, 1, 2, 4, 8, 16}) {
          SCOPED_TRACE(Start);
          SCOPED_TRACE(Size);
          expectLinearSelection(Image, Start, Size);
        }
    }
}

TEST(InstructionRelocations, ExtremaTrackMutationWithoutCaching) {
  auto Image = relocationImage(Arch::X64, BinaryFormat::COFF);
  Image.DataAddressRelocOperands.clear();
  Image.CodeAddressRelocOperands.clear();
  expectLinearSelection(Image, 0xfff, 2);
  Image.DataAddressRelocOperands[0x1000] = {0x8010, 0x8010, 8, 0x8000};
  expectLinearSelection(Image, 0xfff, 2);
  expectLinearSelection(Image, 0x1000, 1);
  expectLinearSelection(Image, 0x1001, 1);
  Image.DataAddressRelocOperands[0xff0] = {0x8010, 0x8010, 8, 0x8000};
  Image.DataAddressRelocOperands[0x1030] = {0x8010, 0x8010, 8, 0x8000};
  expectLinearSelection(Image, 0xfef, 2);
  expectLinearSelection(Image, 0x1030, 1);
  Image.DataAddressRelocOperands.erase(0xff0);
  Image.DataAddressRelocOperands.erase(0x1030);
  expectLinearSelection(Image, 0xfef, 2);
  expectLinearSelection(Image, 0x1030, 1);
}

TEST(InstructionRelocations, InRangeFieldsKeepOwnerAndPCRelativeChecks) {
  auto Image = relocationImage(Arch::X64, BinaryFormat::ELF);
  Image.DataAddressRelocOperands.clear();
  Image.CodeAddressRelocOperands.clear();
  // PC-relative target is recomputed from instruction end before ownership.
  Image.DataAddressRelocOperands[0x800d] = {0xf8, 0xdead, 1, 0x8000, true};
  Image.DataAddressRelocOperands[0x800e] = {0x8010, 0x8010, 8, InvalidVA};
  Image.DataAddressRelocOperands[0x800f] = {0, 0x8010, 0, 0x8000, true};
  const auto Selected =
      instructionRelocations(Image, DecodedInsn{0x800c, 4, 0, nullptr});
  ASSERT_EQ(Selected.Addresses.size(), 1u);
  EXPECT_EQ(Selected.Addresses[0].FieldVA, 0x800du);
  EXPECT_EQ(Selected.Addresses[0].EncodedValue, 0xf8u);
  EXPECT_EQ(Selected.Addresses[0].TargetVA, 0x8008u);
  EXPECT_EQ(Selected.Addresses[0].TargetOwnerVA, 0x8000u);
  EXPECT_TRUE(Selected.Addresses[0].PCRelativeFromInstructionEnd);
}

} // namespace
} // namespace neverd
