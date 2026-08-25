//===- COFFExceptionSEHTests.cpp - SEH scope table tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace {

using namespace neverd;
using namespace neverd::coff_eh_test;

TEST(COFFExceptionParser, ReconstructsCSpecificScopeTable) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__C_specific_handler");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 1);
  writeLE<uint32_t>(X + 4, 0x1000);
  writeLE<uint32_t>(X + 8, 0x1040);
  writeLE<uint32_t>(X + 12, 1); // catch-all filter
  writeLE<uint32_t>(X + 16, 0x1080);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  F.Personality = ExceptionPersonality::Unknown;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.PersonalityVA, Img.Base + 0x1100);
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CSpecificHandler);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_EQ(Decoded.SEH->Scopes.size(), 1u);
  EXPECT_EQ(Decoded.SEH->Scopes[0].Kind, SEHScopeKind::CatchAll);
  EXPECT_EQ(Decoded.SEH->Scopes[0].GuardedRange.Begin, Img.Base + 0x1000);
  EXPECT_EQ(Decoded.SEH->Scopes[0].HandlerVA, Img.Base + 0x1080);
}

TEST(COFFExceptionParser, MarksFunctionPartialForEmptySEHScope) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__C_specific_handler");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 1);
  writeLE<uint32_t>(X + 4, 0x1020);
  writeLE<uint32_t>(X + 8, 0x1020);
  writeLE<uint32_t>(X + 12, 1); // catch-all filter
  writeLE<uint32_t>(X + 16, 0x1080);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.Encoding = ExceptionEncoding::X64UnwindV1;
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.canRegenerateLanguageMetadata());
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_EQ(Decoded.SEH->Scopes.size(), 1u);
  EXPECT_EQ(Decoded.SEH->Scopes[0].ParseStatus,
            ExceptionParseStatus::Partial);
  EXPECT_EQ(Decoded.SEH->Scopes[0].GuardedRange.Begin,
            Img.Base + 0x1020);
  EXPECT_EQ(Decoded.SEH->Scopes[0].GuardedRange.End, Img.Base + 0x1020);
}

TEST(COFFExceptionParser, ResolvesAArch64PersonalityBranchVeneer) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  uint8_t *Text = Img.Segments[0].Data.data();
  writeLE<uint32_t>(Text + 0x100, 0x14000008); // b +0x20

  Symbol Personality;
  Personality.Name = "__C_specific_handler";
  Personality.Addr = Img.Base + 0x1120;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));
  writeLE<uint32_t>(Img.Segments[1].Data.data(), 0); // empty scope table

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CSpecificHandler);
  EXPECT_EQ(Decoded.PersonalityName, "__C_specific_handler");
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.SEH.has_value());
  EXPECT_TRUE(Decoded.SEH->Scopes.empty());
}

TEST(COFFExceptionParser, NormalizesOnlyExactAArch64ConstantTrueFilterThunks) {
  constexpr uint32_t Prologue[] = {
      0xa9bf7bfdu, // stp x29, x30, [sp, #-16]!
      0xaa0103fdu, // mov x29, x1
      0x52800020u, // mov w0, #1
      0xa8c17bfdu, // ldp x29, x30, [sp], #16
      0xd65f03c0u, // ret
  };
  constexpr uint32_t Leaf[] = {
      0x52800020u, // mov w0, #1
      0xd65f03c0u, // ret
  };

  auto Decode = [&](Arch TargetArch, llvm::ArrayRef<uint32_t> FilterCode) {
    BinaryImage Img = makeX64ExceptionImage(0x200);
    Img.Arch = TargetArch;
    Img.Bits = Bitness::Bits64;
    addPersonalityImport(Img, Img.Base + 0x1100, "__C_specific_handler");

    uint8_t *Text = Img.Segments[0].Data.data();
    for (size_t I = 0; I < FilterCode.size(); ++I)
      writeLE<uint32_t>(Text + 0x90 + I * sizeof(uint32_t), FilterCode[I]);
    uint8_t *XData = Img.Segments[1].Data.data();
    writeLE<uint32_t>(XData, 1);
    writeLE<uint32_t>(XData + 4, 0x1000);
    writeLE<uint32_t>(XData + 8, 0x1040);
    writeLE<uint32_t>(XData + 12, 0x1090);
    writeLE<uint32_t>(XData + 16, 0x1060);

    ExceptionFunction F;
    F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
    F.Encoding = TargetArch == Arch::AArch64 ? ExceptionEncoding::ARM64Unpacked
                                             : ExceptionEncoding::X64UnwindV1;
    F.PersonalityVA = Img.Base + 0x1100;
    F.HandlerDataVA = Img.Base + 0x3000;
    Img.ExceptionMetadata.Functions.push_back(std::move(F));
    coff_loader::resolveExceptionHandlers(Img);
    return Img;
  };

  for (llvm::ArrayRef<uint32_t> FilterCode :
       {llvm::ArrayRef<uint32_t>(Prologue), llvm::ArrayRef<uint32_t>(Leaf)}) {
    BinaryImage Img = Decode(Arch::AArch64, FilterCode);
    const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions.front();
    ASSERT_TRUE(Decoded.SEH.has_value());
    ASSERT_EQ(Decoded.SEH->Scopes.size(), 1u);
    EXPECT_EQ(Decoded.SEH->Scopes.front().Kind, SEHScopeKind::CatchAll);
    EXPECT_EQ(Decoded.SEH->Scopes.front().FilterOrFinallyVA, 0u);
    EXPECT_EQ(Decoded.SEH->Scopes.front().NormalizedFilterVA,
              Img.Base + 0x1090);
    EXPECT_FALSE(Decoded.canRegenerateLanguageMetadata());

    const WindowsEHNativeSourceClassification IRSource =
        classifyWindowsEHNativeSource(
            Decoded, Arch::AArch64, BinaryFormat::COFF,
            WindowsEHNativeCapability::IRLowering);
    EXPECT_TRUE(IRSource.canLowerNativeIR());
    const WindowsEHNativeSourceClassification PatchSource =
        classifyWindowsEHNativeSource(
            Decoded, Arch::AArch64, BinaryFormat::COFF,
            WindowsEHNativeCapability::OutputPatch);
    EXPECT_FALSE(PatchSource.canPatchOutput());
    EXPECT_EQ(PatchSource.Reason,
              WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI);
  }

  std::array<uint32_t, 5> ReturnsFalse;
  std::copy(std::begin(Prologue), std::end(Prologue), ReturnsFalse.begin());
  ReturnsFalse[2] = 0x52800000u; // mov w0, #0
  for (auto [TargetArch, FilterCode] :
       {std::pair{Arch::AArch64, llvm::ArrayRef<uint32_t>(ReturnsFalse)},
        std::pair{Arch::X64, llvm::ArrayRef<uint32_t>(Prologue)}}) {
    BinaryImage Img = Decode(TargetArch, FilterCode);
    const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions.front();
    ASSERT_TRUE(Decoded.SEH.has_value());
    ASSERT_EQ(Decoded.SEH->Scopes.size(), 1u);
    EXPECT_EQ(Decoded.SEH->Scopes.front().Kind, SEHScopeKind::Filter);
    EXPECT_EQ(Decoded.SEH->Scopes.front().FilterOrFinallyVA, Img.Base + 0x1090);
    EXPECT_EQ(Decoded.SEH->Scopes.front().NormalizedFilterVA, 0u);
  }
}

TEST(COFFExceptionParser, DoesNotTreatAArch64CallAsBranchVeneer) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  writeLE<uint32_t>(Img.Segments[0].Data.data() + 0x100,
                    0x94000008); // bl +0x20

  Symbol Personality;
  Personality.Name = "__C_specific_handler";
  Personality.Addr = Img.Base + 0x1120;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));
  // Language data the unresolved personality leaves uninterpreted, which is
  // what keeps the record short of complete.
  writeLE<uint32_t>(Img.Segments[1].Data.data(), 1);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.SEH.has_value());
}

} // namespace
