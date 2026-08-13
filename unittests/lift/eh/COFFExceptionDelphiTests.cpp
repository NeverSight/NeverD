//===- COFFExceptionDelphiTests.cpp - Delphi x64 scope table tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFExceptionTestsDetail.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstring>

namespace {

using namespace neverd;
using namespace neverd::coff_eh_test;

// Delphi's x86-64 compiler drops the `FS:[0]` chain its 32-bit one uses and
// describes a frame through the ordinary table mechanism, with a `TExcData`
// scope array in the handler data.  Handler data that does not read as one is
// reported rather than guessed at: reported as complete it would describe a
// Delphi `try` as a function that installs a handler and then handles nothing.
TEST(COFFExceptionParser, ReportsHandlerDataThatIsNotADelphiScopeTable) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__DelphiExceptionHandler");

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  F.Personality = ExceptionPersonality::Unknown;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality,
            ExceptionPersonality::DelphiExceptionHandler);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.Diagnostics.empty());
  // Nothing may be invented from the undecoded table.
  EXPECT_FALSE(Decoded.SEH.has_value());
  EXPECT_FALSE(Decoded.Cxx.has_value());
  EXPECT_FALSE(Decoded.DelphiScopes.has_value());
  EXPECT_FALSE(Decoded.canRegenerateLanguageMetadata());
}

/// Set up a Delphi x86-64 frame whose handler data holds one `TExcScope`.
ExceptionFunction addDelphiScope(BinaryImage &Img, uint32_t BeginRVA,
                                 uint32_t EndRVA, uint32_t TableOffset,
                                 uint32_t TargetRVA) {
  addPersonalityImport(Img, Img.Base + 0x1100, "@DelphiExceptionHandler");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 1); // TExcData.ScopeCount
  writeLE<uint32_t>(X + 4, BeginRVA);
  writeLE<uint32_t>(X + 8, EndRVA);
  writeLE<uint32_t>(X + 12, TableOffset);
  writeLE<uint32_t>(X + 16, TargetRVA);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  F.Personality = ExceptionPersonality::Unknown;
  return F;
}

TEST(COFFExceptionParser, DecodesADelphiX64FinallyScope) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  Img.ExceptionMetadata.Functions.push_back(
      addDelphiScope(Img, 0x1000, 0x1040, 0, 0x1080));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  // The `@` prefix is how Delphi's own RTL spells the routine, and must not
  // keep it from being recognized.
  EXPECT_EQ(Decoded.Personality,
            ExceptionPersonality::DelphiExceptionHandler);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.DelphiScopes.has_value());
  ASSERT_EQ(Decoded.DelphiScopes->Scopes.size(), 1u);
  const DelphiScopeRecord &Scope = Decoded.DelphiScopes->Scopes[0];
  EXPECT_EQ(Scope.Kind, DelphiScopeKind::Finally);
  EXPECT_EQ(Scope.GuardedRange.Begin, Img.Base + 0x1000);
  EXPECT_EQ(Scope.GuardedRange.End, Img.Base + 0x1040);
  EXPECT_EQ(Scope.TargetVA, Img.Base + 0x1080);
}

TEST(COFFExceptionParser, TellsDelphiX64CatchAllFromSafecall) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  Img.ExceptionMetadata.Functions.push_back(
      addDelphiScope(Img, 0x1000, 0x1040, 2, 0x1080));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(Decoded.DelphiScopes.has_value());
  ASSERT_EQ(Decoded.DelphiScopes->Scopes.size(), 1u);
  EXPECT_EQ(Decoded.DelphiScopes->Scopes[0].Kind, DelphiScopeKind::CatchAll);

  BinaryImage Safecall = makeX64ExceptionImage(0x200);
  Safecall.ExceptionMetadata.Functions.push_back(
      addDelphiScope(Safecall, 0x1000, 0x1040, 1, 0x1080));
  coff_loader::resolveExceptionHandlers(Safecall);
  ASSERT_TRUE(Safecall.ExceptionMetadata.Functions[0].DelphiScopes.has_value());
  EXPECT_EQ(
      Safecall.ExceptionMetadata.Functions[0].DelphiScopes->Scopes[0].Kind,
      DelphiScopeKind::SafecallCatch);
}

TEST(COFFExceptionParser, ReadsADelphiX64OnExceptionArmTable) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  // `TableOffset` above 2 is the RVA of a `TExcDesc` rather than a
  // discriminant, and `TargetOffset` carries nothing in that case.
  Img.ExceptionMetadata.Functions.push_back(
      addDelphiScope(Img, 0x1000, 0x1040, 0x3040, 0));

  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<int32_t>(X + 0x40, 1);      // TExcDesc.DescCount
  writeLE<uint32_t>(X + 0x44, 0x3100); // TExcDescEntry.VTable
  writeLE<uint32_t>(X + 0x48, 0x1080); // TExcDescEntry.Handler
  // A 64-bit VMT whose class reference is at RVA 0x3100: `vmtSelfPtr` holds
  // the reference's own address, which is what makes the identification
  // decisive, and the fields sit twice as far back as on x86-32.
  writeLE<uint64_t>(X + 0x3100 - 176 - 0x3000, Img.Base + 0x3100);
  writeLE<uint64_t>(X + 0x3100 - 112 - 0x3000, Img.Base + 0x3180);
  writeLE<uint32_t>(X + 0x3100 - 104 - 0x3000, 0x20);
  X[0x180] = 8;
  std::memcpy(X + 0x181, "EMyError", 8);

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.DelphiScopes.has_value());
  ASSERT_EQ(Decoded.DelphiScopes->Scopes.size(), 1u);
  const DelphiScopeRecord &Scope = Decoded.DelphiScopes->Scopes[0];
  EXPECT_EQ(Scope.Kind, DelphiScopeKind::OnException);
  EXPECT_EQ(Scope.DescriptorVA, Img.Base + 0x3040);
  ASSERT_EQ(Scope.OnExceptions.size(), 1u);
  EXPECT_EQ(Scope.OnExceptions[0].ClassName, "EMyError");
  EXPECT_EQ(Scope.OnExceptions[0].HandlerVA, Img.Base + 0x1080);
}

TEST(COFFExceptionParser, RejectsADelphiX64ScopeThatEscapesItsFunction) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  // A scope guards a stretch of the function it belongs to.  A range outside
  // it means these sixteen bytes were something other than a `TExcScope`.
  Img.ExceptionMetadata.Functions.push_back(
      addDelphiScope(Img, 0x1000, 0x1400, 0, 0x1080));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.DelphiScopes.has_value());
}

TEST(COFFExceptionParser, RejectsADelphiX64ScopeWhoseHandlerIsNotCode) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  // 0x3080 is in `.xdata`, which is readable but not executable.
  Img.ExceptionMetadata.Functions.push_back(
      addDelphiScope(Img, 0x1000, 0x1040, 0, 0x3080));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.DelphiScopes.has_value());
}

TEST(COFFExceptionParser, RejectsADelphiX64ArmTableWithNoRealClass) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  Img.ExceptionMetadata.Functions.push_back(
      addDelphiScope(Img, 0x1000, 0x1040, 0x3040, 0));
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<int32_t>(X + 0x40, 1);
  writeLE<uint32_t>(X + 0x44, 0x3100); // points at zeroes, not a VMT
  writeLE<uint32_t>(X + 0x48, 0x1080);

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.DelphiScopes.has_value());
}

} // namespace
