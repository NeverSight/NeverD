//===- COFFExceptionGSTests.cpp - GS cookie wrapper tests -------------===//
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

TEST(COFFExceptionParser, KeepsGSWrapperDistinctAndFailClosed) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1100, "__GSHandlerCheck_SEH");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);         // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x25);  // EH + aligned, cookie offset 0x20
  writeLE<int32_t>(X + 8, -0x10);  // alignment base
  writeLE<uint32_t>(X + 12, 0x10); // alignment

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  F.Personality = ExceptionPersonality::Unknown;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_FALSE(Decoded.GSCookie->Payload.empty());
  EXPECT_EQ(Decoded.GSCookie->CookieOffset, 0x20);
  EXPECT_TRUE(Decoded.GSCookie->HasAlignment);
  EXPECT_EQ(Decoded.GSCookie->Alignment, 0x10u);
  EXPECT_FALSE(Decoded.canRegenerateLanguageMetadata());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, InfersStrippedX64GSWrapperFromCheckedStructure) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe8;
  writeLE<int32_t>(Text + 0x101, 0x7b); // call 0x1180 from 0x1100

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);         // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x25);  // EH + aligned, cookie offset 0x20
  writeLE<int32_t>(X + 8, -0x10);  // alignment base
  writeLE<uint32_t>(X + 12, 0x10); // alignment

  ExceptionFunction Guarded;
  Guarded.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  Guarded.PersonalityVA = Img.Base + 0x1100;
  Guarded.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Guarded));

  ExceptionFunction Wrapper;
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x1160};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  EXPECT_EQ(Decoded.PersonalityName, "__GSHandlerCheck_SEH");
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

} // namespace
