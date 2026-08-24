//===- COFFExceptionGSTests.cpp - GS cookie wrapper tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace {

using namespace neverd;
using namespace neverd::coff_eh_test;

void addX64SEHGSPayload(BinaryImage &Img) {
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);         // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x25);  // EH + aligned, cookie offset 0x20
  writeLE<int32_t>(X + 8, -0x10);  // alignment base
  writeLE<uint32_t>(X + 12, 0x10); // alignment
}

void addStrippedGSFrame(BinaryImage &Img,
                        const ExceptionAddressRange &WrapperRange,
                        va_t PersonalityVA = 0) {
  ExceptionFunction Guarded;
  Guarded.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  Guarded.PersonalityVA =
      PersonalityVA != 0 ? PersonalityVA : WrapperRange.Begin;
  Guarded.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Guarded));

  ExceptionFunction Wrapper;
  Wrapper.CodeRange = WrapperRange;
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));
}

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
  Text[0x105] = 0xc3;                   // ret

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
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x1106};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  EXPECT_EQ(Decoded.PersonalityName, "__GSHandlerCheck_SEH");
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser,
     InfersStrippedX64GSWrapperFromInstructionBoundaryTailJump) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe8;
  writeLE<int32_t>(Text + 0x101, 0x6b); // call cookie check at 0x1170
  Text[0x105] = 0xe9;
  writeLE<int32_t>(Text + 0x106, 0x76); // tail-jump to 0x1180

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
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x110a};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser,
     InfersStrippedX64GSWrapperThroughReachableInternalBlock) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe9;
  writeLE<int32_t>(Text + 0x101, 0x0b); // jump to wrapper block at 0x1110
  Text[0x110] = 0xe8;
  writeLE<int32_t>(Text + 0x111, 0x6b); // call 0x1180
  Text[0x115] = 0xc3;                   // ret

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
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x1116};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, InfersStrippedX64GSWrapperWhenEveryReturnDelegates) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0x75;
  Text[0x101] = 0x07; // jne 0x1109
  Text[0x102] = 0xe8;
  writeLE<int32_t>(Text + 0x103, 0x79); // call 0x1180
  Text[0x107] = 0xeb;
  Text[0x108] = 0x05; // jmp 0x110e
  Text[0x109] = 0xe8;
  writeLE<int32_t>(Text + 0x10a, 0x72); // call 0x1180
  Text[0x10e] = 0xc3;                   // ret

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x110f});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedX64GSWrapperWhenAPathBypassesDelegation) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0x75;
  Text[0x101] = 0x05; // jne 0x1107
  Text[0x102] = 0xe8;
  writeLE<int32_t>(Text + 0x103, 0x79); // call 0x1180
  Text[0x107] = 0xc3;                   // ret

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1108});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     InfersStrippedX64GSWrapperWithUD0UD1OrUD2ExitAndIgnoresDeadCall) {
  struct TrapCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
  };
  const TrapCase Cases[] = {{"ud0", {0x0f, 0xff, 0xc0}},
                            {"ud1", {0x0f, 0xb9, 0xc0}},
                            {"ud2", {0x0f, 0x0b}}};

  for (const TrapCase &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    BinaryImage Img = makeX64ExceptionImage();
    addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

    Symbol DeadPersonality;
    DeadPersonality.Name = "__CxxFrameHandler3";
    DeadPersonality.Addr = Img.Base + 0x1190;
    DeadPersonality.IsFunc = true;
    Img.Symbols.push_back(std::move(DeadPersonality));

    uint8_t *Text = Img.Segments[0].Data.data();
    Text[0x100] = 0x75;
    Text[0x101] = 0x06; // jne 0x1108
    Text[0x102] = 0xe8;
    writeLE<int32_t>(Text + 0x103, 0x79); // call 0x1180
    Text[0x107] = 0xc3;                   // ret
    std::copy(Case.Bytes.begin(), Case.Bytes.end(), Text + 0x108);
    const size_t DeadCall = 0x108 + Case.Bytes.size();
    Text[DeadCall] = 0xe8;
    writeLE<int32_t>(Text + DeadCall + 1,
                     static_cast<int32_t>(0x190 - (DeadCall + 5)));

    addX64SEHGSPayload(Img);
    addStrippedGSFrame(Img,
                       {Img.Base + 0x1100, Img.Base + 0x1000 + DeadCall + 5});

    coff_loader::resolveExceptionHandlers(Img);
    const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
    EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
    ASSERT_TRUE(Decoded.SEH.has_value());
    ASSERT_TRUE(Decoded.GSCookie.has_value());
    EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  }
}

TEST(COFFExceptionParser, InfersStrippedX64GSWrapperThroughExactIATCall) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xff;
  Text[0x101] = 0x15;
  writeLE<int32_t>(Text + 0x102, 0x1fea); // call [rip + IAT@0x30f0]
  Text[0x106] = 0xc3;                     // ret

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1107});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, InfersStrippedX86GSWrapperThroughExactIATCall) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  const va_t OldBase = Img.Base;
  Img.Base = 0x400000;
  for (Segment &Seg : Img.Segments)
    Seg.VA = Img.Base + (Seg.VA - OldBase);
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xff;
  Text[0x101] = 0x15;
  writeLE<uint32_t>(Text + 0x102, static_cast<uint32_t>(Img.Base + 0x30f0));
  Text[0x106] = 0xc3; // ret

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1107});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser,
     InfersStrippedX86GSWrapperThroughReachableInternalBlock) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  const va_t OldBase = Img.Base;
  Img.Base = 0x400000;
  for (Segment &Seg : Img.Segments)
    Seg.VA = Img.Base + (Seg.VA - OldBase);
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe9;
  writeLE<int32_t>(Text + 0x101, 0x0b); // jmp 0x1110
  Text[0x110] = 0xe8;
  writeLE<int32_t>(Text + 0x111, 0x6b); // call 0x1180
  Text[0x115] = 0xc3;                   // ret

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1116});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, DoesNotInferStrippedX64GSWrapperFromStubUsedAsIAT) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xff;
  Text[0x101] = 0x15;
  writeLE<int32_t>(Text + 0x102, 0x7a); // call [rip + stub@0x1180]
  Text[0x106] = 0xc3;                   // ret

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1107});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedX64GSWrapperFromDirectCallToIATData) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe8;
  writeLE<int32_t>(Text + 0x101, 0x1feb); // call IAT data at 0x30f0
  Text[0x105] = 0xc3;                     // ret

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1106});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedX64GSWrapperFromBranchToInstructionInterior) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe8;
  writeLE<int32_t>(Text + 0x101, 0x7b); // call 0x1180
  Text[0x105] = 0x75;
  Text[0x106] = 0x01; // jne 0x1108, into the following mov immediate
  Text[0x107] = 0xb8;
  writeLE<uint32_t>(Text + 0x108, 0); // mov eax, 0
  Text[0x10c] = 0xc3;                 // ret

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x110d});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser, DoesNotInferStrippedX64GSWrapperBeyondByteBudget) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Segments[0].Size = 0x301;
  Img.Segments[0].Data.resize(0x301, 0x90);
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe8;
  writeLE<int32_t>(Text + 0x101, 0x7b); // call 0x1180
  Text[0x105] = 0xc3;                   // ret

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1301});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedX64GSWrapperBeyondInstructionBudget) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe8;
  writeLE<int32_t>(Text + 0x101, 0x7b); // call 0x1180
  std::fill_n(Text + 0x105, 63, 0x90);
  Text[0x144] = 0xc3; // the 65th reachable instruction

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1145});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser, DoesNotInferStrippedX64GSWrapperBeyondBlockBudget) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe8;
  writeLE<int32_t>(Text + 0x101, 0x7b); // call 0x1180
  for (size_t I = 0; I < 32; ++I) {
    Text[0x105 + 2 * I] = 0x75;
    Text[0x106 + 2 * I] = 0x00; // jne to its own fallthrough block
  }
  Text[0x145] = 0xc3;

  addX64SEHGSPayload(Img);
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1146});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     InfersStrippedAArch64GSWrapperFromInstructionBoundaries) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::AArch64;
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  writeLE<uint32_t>(Text + 0x100, 0x9400001c); // bl 0x1170
  writeLE<uint32_t>(Text + 0x104, 0x1400001f); // b 0x1180

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset

  ExceptionFunction Guarded;
  Guarded.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  Guarded.PersonalityVA = Img.Base + 0x1100;
  Guarded.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Guarded));

  ExceptionFunction Wrapper;
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x1108};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.GSCookie->CookieOffset, 0x20);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser,
     InfersStrippedAArch64GSWrapperThroughReachableInternalBlock) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::AArch64;
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  writeLE<uint32_t>(Text + 0x100, 0x14000004); // b 0x1110
  writeLE<uint32_t>(Text + 0x110, 0x9400001c); // bl 0x1180
  writeLE<uint32_t>(Text + 0x114, 0xd65f03c0); // ret

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1118});

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, InfersStrippedAArch64GSWrapperWithTerminalTrapExit) {
  const uint32_t Traps[] = {0xd4200000,  // brk #0
                            0x00000000}; // udf #0
  for (uint32_t Trap : Traps) {
    SCOPED_TRACE(Trap);
    BinaryImage Img = makeX64ExceptionImage();
    Img.Arch = Arch::AArch64;
    addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

    uint8_t *Text = Img.Segments[0].Data.data();
    writeLE<uint32_t>(Text + 0x100, 0x54000060); // b.eq 0x110c
    writeLE<uint32_t>(Text + 0x104, 0x9400001f); // bl 0x1180
    writeLE<uint32_t>(Text + 0x108, 0xd65f03c0); // ret
    writeLE<uint32_t>(Text + 0x10c, Trap);
    writeLE<uint32_t>(Text + 0x110, 0x94000020); // dead bl 0x1190

    addX64SEHGSPayload(Img);
    addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1114});

    coff_loader::resolveExceptionHandlers(Img);
    const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
    EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
    ASSERT_TRUE(Decoded.SEH.has_value());
    ASSERT_TRUE(Decoded.GSCookie.has_value());
    EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  }
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedAArch64GSWrapperFromMisalignedRange) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::AArch64;
  addPersonalityImport(Img, Img.Base + 0x117e, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  writeLE<uint32_t>(Text + 0x102, 0x9400001f); // bl 0x117e
  writeLE<uint32_t>(Text + 0x106, 0xd65f03c0); // ret

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset
  addStrippedGSFrame(Img, {Img.Base + 0x1102, Img.Base + 0x110a},
                     Img.Base + 0x1102);

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedAArch64GSWrapperFromConditionalBranch) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::AArch64;
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  writeLE<uint32_t>(Text + 0x100, 0x9400001c); // bl 0x1170
  writeLE<uint32_t>(Text + 0x104, 0x540003e0); // b.eq 0x1180
  writeLE<uint32_t>(Text + 0x108, 0xd65f03c0); // ret

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset

  ExceptionFunction Guarded;
  Guarded.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  Guarded.PersonalityVA = Img.Base + 0x1100;
  Guarded.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Guarded));

  ExceptionFunction Wrapper;
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x110c};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     InfersStrippedARM32GSWrapperFromThumbInstructionBoundaries) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  const va_t OldBase = Img.Base;
  Img.Base = 0x400000;
  for (Segment &Seg : Img.Segments)
    Seg.VA = Img.Base + (Seg.VA - OldBase);
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  const uint8_t WrapperCode[] = {0x00, 0xf0, 0x36, 0xf8,  // bl 0x1170
                                 0x00, 0xf0, 0x3c, 0xb8}; // b.w 0x1180
  std::copy(std::begin(WrapperCode), std::end(WrapperCode), Text + 0x100);

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset

  ExceptionFunction Guarded;
  Guarded.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  Guarded.PersonalityVA = Img.Base + 0x1101; // Thumb interworking bit
  Guarded.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Guarded));

  ExceptionFunction Wrapper;
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x1108};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.GSCookie->CookieOffset, 0x20);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser,
     InfersStrippedARM32GSWrapperThroughReachableThumbBlock) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  const va_t OldBase = Img.Base;
  Img.Base = 0x400000;
  for (Segment &Seg : Img.Segments)
    Seg.VA = Img.Base + (Seg.VA - OldBase);
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  const uint8_t Entry[] = {0x06, 0xe0};             // b.n 0x1110
  const uint8_t Target[] = {0x00, 0xf0, 0x36, 0xf8, // bl 0x1180
                            0x70, 0x47};            // bx lr
  std::copy(std::begin(Entry), std::end(Entry), Text + 0x100);
  std::copy(std::begin(Target), std::end(Target), Text + 0x110);

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1116},
                     Img.Base + 0x1101);

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, InfersStrippedARM32GSWrapperWithTerminalTrapExit) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  const va_t OldBase = Img.Base;
  Img.Base = 0x400000;
  for (Segment &Seg : Img.Segments)
    Seg.VA = Img.Base + (Seg.VA - OldBase);
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  const uint8_t WrapperCode[] = {
      0x02, 0xd1,             // bne 0x1108
      0x00, 0xf0, 0x3d, 0xf8, // bl 0x1180
      0x70, 0x47,             // bx lr
      0x00, 0xde,             // udf #0
      0x00, 0xf0, 0x41, 0xf8, // dead bl 0x1190
  };
  std::copy(std::begin(WrapperCode), std::end(WrapperCode), Text + 0x100);

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x110e},
                     Img.Base + 0x1101);

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, DoesNotInferStrippedARM32GSWrapperThroughThumbIT) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  const va_t OldBase = Img.Base;
  Img.Base = 0x400000;
  for (Segment &Seg : Img.Segments)
    Seg.VA = Img.Base + (Seg.VA - OldBase);
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  const uint8_t WrapperCode[] = {0x08, 0xbf,             // it eq
                                 0x00, 0xf0, 0x3d, 0xf8, // bl 0x1180
                                 0x70, 0x47};            // bx lr
  std::copy(std::begin(WrapperCode), std::end(WrapperCode), Text + 0x100);

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);        // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x20); // cookie offset
  addStrippedGSFrame(Img, {Img.Base + 0x1100, Img.Base + 0x1108},
                     Img.Base + 0x1101);

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedX64GSWrapperFromCallBytesInsideImmediate) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  // movabs rax, 0x00000000000079e8; ret.  A byte-wise scanner mistakes the
  // first immediate byte for `call rel32` and lands exactly on 0x1180.
  const uint8_t WrapperCode[] = {0x48, 0xb8, 0xe8, 0x79, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0xc3};
  std::copy(std::begin(WrapperCode), std::end(WrapperCode), Text + 0x100);

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
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x110b};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedX64GSWrapperFromUnreachableCallAfterReturn) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xc3; // ret
  Text[0x101] = 0xe8;
  writeLE<int32_t>(Text + 0x102, 0x7a); // unreachable call to 0x1180

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
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x1106};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedX64GSWrapperPastExternalTailJump) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe9;
  writeLE<int32_t>(Text + 0x101, 0x6b); // tail-jump to unnamed 0x1170
  Text[0x105] = 0xe8;
  writeLE<int32_t>(Text + 0x106, 0x76); // unreachable call to 0x1180

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
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x110a};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

TEST(COFFExceptionParser,
     DoesNotInferStrippedX64GSWrapperFromPartialSEHPayload) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1180, "__C_specific_handler");

  uint8_t *Text = Img.Segments[0].Data.data();
  Text[0x100] = 0xe8;
  writeLE<int32_t>(Text + 0x101, 0x7b); // call 0x1180 from 0x1100
  Text[0x105] = 0xc3;                   // ret

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 1);
  writeLE<uint32_t>(X + 4, 0x1020);
  writeLE<uint32_t>(X + 8, 0x1020); // empty scope is only partially decoded
  writeLE<uint32_t>(X + 12, 1);     // catch-all filter
  writeLE<uint32_t>(X + 16, 0x1060);
  writeLE<uint32_t>(X + 20, 0x20); // GS cookie offset

  ExceptionFunction Guarded;
  Guarded.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  Guarded.PersonalityVA = Img.Base + 0x1100;
  Guarded.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Guarded));

  ExceptionFunction Wrapper;
  Wrapper.CodeRange = {Img.Base + 0x1100, Img.Base + 0x1106};
  Img.ExceptionMetadata.Functions.push_back(std::move(Wrapper));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.GSCookie.has_value());
}

} // namespace
