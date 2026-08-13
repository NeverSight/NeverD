//===- COFFExceptionSEHTests.cpp - SEH scope table tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFExceptionTestsDetail.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/support/BinaryEncoding.h"

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
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CSpecificHandler);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_EQ(Decoded.SEH->Scopes.size(), 1u);
  EXPECT_EQ(Decoded.SEH->Scopes[0].Kind, SEHScopeKind::CatchAll);
  EXPECT_EQ(Decoded.SEH->Scopes[0].GuardedRange.Begin, Img.Base + 0x1000);
  EXPECT_EQ(Decoded.SEH->Scopes[0].HandlerVA, Img.Base + 0x1080);
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
