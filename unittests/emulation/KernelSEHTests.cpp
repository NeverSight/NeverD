//===- KernelSEHTests.cpp - Pure x64 exception transfer tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelSEH.h"

#include <map>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::emulation;

namespace {
class DriverKernelSEH : public ::testing::Test {
protected:
  static constexpr uint64_t Base = 0x180000000;
  static constexpr uint64_t ImageSize = 0x4000;
  static constexpr uint64_t StackBase = 0x70000000;
  static constexpr uint64_t StackSize = 4096;
  static constexpr uint32_t Code = 0xC000009A;
  ExceptionInfo Metadata;
  KernelSEH::Context Caller;
  std::map<uint64_t, uint64_t> Words;
  std::vector<uint64_t> Reads;
  uint64_t DeniedPC = 0;
  uint64_t ActualBase = Base;

  void SetUp() override {
    for (size_t I = 0; I < Caller.GPR.size(); ++I)
      Caller.GPR[I] = 0x100 + I;
    Caller.GPR[seh::StackRegister] = StackBase + 0x100;
    Caller.PC = Base + 0x1040;
  }
  static UnwindOperation op(UnwindOperationKind Kind, uint32_t Position,
                            uint64_t Size = 0, uint16_t Register = 0) {
    UnwindOperation Result;
    Result.Kind = Kind;
    Result.CodeOffset = Position;
    Result.StackOffset = Size;
    Result.Register = Register;
    return Result;
  }
  ExceptionFunction &function(uint64_t RVA = 0x1000) {
    ExceptionFunction F;
    F.CodeRange = {Base + RVA, Base + RVA + 0x100};
    F.Encoding = ExceptionEncoding::X64UnwindV1;
    F.UnwindVersion = 1;
    F.PrologueSize = 16;
    Metadata.Functions.push_back(std::move(F));
    return Metadata.Functions.back();
  }
  ExceptionFunction &handler(uint64_t RVA = 0x1000) {
    auto &F = function(RVA);
    F.Personality = ExceptionPersonality::CSpecificHandler;
    F.UnwindFlags = seh::ExceptionHandlerFlag | seh::UnwindHandlerFlag;
    F.SEH.emplace();
    F.SEH->Scopes.push_back(scope(RVA + 0x30, RVA + 0x60, RVA + 0x80));
    return F;
  }
  static SEHScopeRecord scope(uint64_t Begin, uint64_t End, uint64_t Handler) {
    SEHScopeRecord S;
    S.Kind = SEHScopeKind::CatchAll;
    S.GuardedRange = {Base + Begin, Base + End};
    S.HandlerVA = S.ContinuationVA = Base + Handler;
    return S;
  }
  llvm::Expected<std::optional<KernelSEH::Transfer>> plan() {
    KernelSEH Planner(
        Metadata, Base, ActualBase, ImageSize,
        [&](uint64_t Address) -> llvm::Expected<uint64_t> {
          Reads.push_back(Address);
          auto I = Words.find(Address);
          if (I == Words.end())
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "unavailable stack word");
          return I->second;
        },
        [&](uint64_t PC) {
          return PC != DeniedPC && PC >= ActualBase &&
                 PC - ActualBase < ImageSize;
        });
    return Planner.plan(Code, Caller, {StackBase, StackSize});
  }
  KernelSEH::Transfer selected() {
    auto P = plan();
    EXPECT_TRUE(bool(P)) << (P ? "" : llvm::toString(P.takeError()));
    if (!P || !*P) {
      ADD_FAILURE() << "expected an exception handler";
      return {};
    }
    return **P;
  }
  void rejected(llvm::StringRef Fragment) {
    const auto Before = Caller;
    const auto MemoryBefore = Words;
    auto P = plan();
    ASSERT_FALSE(bool(P));
    EXPECT_NE(llvm::toString(P.takeError()).find(Fragment.str()),
              std::string::npos);
    EXPECT_EQ(Caller.GPR, Before.GPR);
    EXPECT_EQ(Caller.PC, Before.PC);
    EXPECT_EQ(Words, MemoryBefore);
  }
};

TEST_F(DriverKernelSEH, CatchAllTransfersInsideOriginalFrame) {
  handler();
  const auto Before = Caller;
  const auto Result = selected();
  EXPECT_EQ(Result.HandlerPC, Base + 0x1080);
  EXPECT_EQ(Result.Registers.PC, Result.HandlerPC);
  EXPECT_EQ(Result.EstablisherFrame, Before.GPR[4]);
  EXPECT_EQ(Result.ExceptionCode, Code);
  EXPECT_EQ(Result.Registers.GPR[0], Code);
  for (size_t I = 1; I < Before.GPR.size(); ++I)
    EXPECT_EQ(Result.Registers.GPR[I], Before.GPR[I]);
  EXPECT_EQ(Caller.GPR, Before.GPR);
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, HelpersRestoreSavedNonvolatileRegisters) {
  handler();
  auto &Helper = function(0x2000);
  Helper.UnwindOperations = {
      op(UnwindOperationKind::AllocateSmall, 6, 56),
      op(UnwindOperationKind::PushNonVolatile, 2, 0, 3),
      op(UnwindOperationKind::PushNonVolatile, 1, 0, 12)};
  Caller.PC = Base + 0x2040;
  const uint64_t SP = Caller.GPR[4];
  Words = {
      {SP + 56, 0xBADF00D}, {SP + 64, 0x12345678}, {SP + 72, Base + 0x1041}};
  const auto Result = selected();
  EXPECT_EQ(Result.Registers.GPR[3], 0xBADF00Du);
  EXPECT_EQ(Result.Registers.GPR[12], 0x12345678u);
  EXPECT_EQ(Result.Registers.GPR[4], SP + 80);
  EXPECT_EQ(Reads, (std::vector<uint64_t>{SP + 56, SP + 64, SP + 72}));
}

TEST_F(DriverKernelSEH, FramePointerRestoresDynamicStackAndCallerFrame) {
  handler();
  auto &Helper = function(0x2000);
  Helper.FrameRegister = 5;
  Helper.FrameOffset = 32;
  Helper.UnwindOperations = {op(UnwindOperationKind::SetFramePointer, 8),
                             op(UnwindOperationKind::AllocateSmall, 5, 32),
                             op(UnwindOperationKind::PushNonVolatile, 1, 0, 5)};
  const uint64_t SP = Caller.GPR[4];
  Caller.GPR[5] = SP + 96;
  Caller.PC = Base + 0x2040;
  Words = {{SP + 96, 0xABCD1234}, {SP + 104, Base + 0x1041}};
  const auto Result = selected();
  EXPECT_EQ(Result.Registers.GPR[5], 0xABCD1234u);
  EXPECT_EQ(Result.Registers.GPR[4], SP + 112);
  EXPECT_EQ(Result.EstablisherFrame, SP + 112);
}

TEST_F(DriverKernelSEH, SelectedDynamicFrameKeepsItsOwnLocals) {
  auto &F = handler();
  F.FrameRegister = 5;
  F.FrameOffset = 32;
  F.UnwindOperations = {op(UnwindOperationKind::SetFramePointer, 8),
                        op(UnwindOperationKind::AllocateSmall, 5, 64)};
  Caller.GPR[5] = Caller.GPR[4] + 96;
  const auto Result = selected();
  EXPECT_EQ(Result.Registers.GPR[4], Caller.GPR[4] + 64);
  EXPECT_EQ(Result.Registers.GPR[5], Caller.GPR[5]);
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, SaveOffsetsUseTheFixedAllocationBase) {
  handler();
  auto &Helper = function(0x2000);
  Helper.UnwindOperations = {
      op(UnwindOperationKind::SaveNonVolatileFar, 10, 40, 12),
      op(UnwindOperationKind::SaveNonVolatile, 8, 32, 13),
      op(UnwindOperationKind::AllocateLarge, 4, 1024)};
  Caller.PC = Base + 0x2040;
  const uint64_t SP = Caller.GPR[4];
  Words = {{SP + 40, 0x1212}, {SP + 32, 0x1313}, {SP + 1024, Base + 0x1041}};
  const auto Result = selected();
  EXPECT_EQ(Result.Registers.GPR[12], 0x1212u);
  EXPECT_EQ(Result.Registers.GPR[13], 0x1313u);
  EXPECT_EQ(Result.Registers.GPR[4], SP + 1032);
}

TEST_F(DriverKernelSEH, LeafHelperUsesTheActualSavedReturnAddress) {
  handler();
  Caller.PC = Base + 0x2040;
  Words[Caller.GPR[4]] = Base + 0x1041;
  EXPECT_EQ(selected().Registers.GPR[4], Caller.GPR[4] + 8);
}

TEST_F(DriverKernelSEH, EqualGuardedRangesRetainNativeTableOrder) {
  auto &F = handler();
  F.SEH->Scopes.insert(F.SEH->Scopes.begin(), scope(0x1030, 0x1060, 0x1070));
  EXPECT_EQ(selected().HandlerPC, Base + 0x1070);
  Caller.PC = Base + 0x1072;
  F.SEH->Scopes.push_back(scope(0x1070, 0x1078, 0x1090));
  EXPECT_EQ(selected().HandlerPC, Base + 0x1090);
}

TEST_F(DriverKernelSEH, ProtectedRangeEndRemainsExclusive) {
  handler();
  Caller.PC = Base + 0x1060;
  Words[Caller.GPR[4]] = Base + ImageSize + 0x100;
  auto Result = plan();
  ASSERT_TRUE(bool(Result));
  EXPECT_FALSE(*Result);
  EXPECT_EQ(Reads.size(), 1u);
}

TEST_F(DriverKernelSEH, RebasingDoesNotRewritePreferredMetadata) {
  handler();
  for (uint64_t Load : {uint64_t(0x100000), uint64_t(0xFFFFF80000000000)}) {
    ActualBase = Load;
    Caller.PC = ActualBase + 0x1040;
    EXPECT_EQ(selected().HandlerPC, ActualBase + 0x1080);
    EXPECT_EQ(Metadata.Functions.front().SEH->Scopes.front().HandlerVA,
              Base + 0x1080);
  }
}

TEST_F(DriverKernelSEH, ActiveFiltersAndFinallyNeverFallThrough) {
  for (auto Kind : {SEHScopeKind::Filter, SEHScopeKind::Finally}) {
    Metadata.Functions.clear();
    auto &F = handler();
    auto Unsupported = scope(0x1030, 0x1060, 0x1070);
    Unsupported.Kind = Kind;
    Unsupported.FilterOrFinallyVA = Base + 0x2000;
    F.SEH->Scopes.insert(F.SEH->Scopes.begin(), Unsupported);
    rejected("filter or finally");
    EXPECT_TRUE(Reads.empty());
  }
}

TEST_F(DriverKernelSEH, UnrelatedUnsupportedScopesAndFunctionsStayLazy) {
  auto &F = handler();
  auto Filter = scope(0x1020, 0x1030, 0x1090);
  Filter.Kind = SEHScopeKind::Filter;
  F.SEH->Scopes.insert(F.SEH->Scopes.begin(), Filter);
  function(0x2000).ParseStatus = ExceptionParseStatus::Malformed;
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
}

TEST_F(DriverKernelSEH, IncompleteAndUnsupportedFramesCannotActAsLeaves) {
  handler();
  auto &F = function(0x2000);
  Caller.PC = Base + 0x2040;
  Words[Caller.GPR[4]] = Base + 0x1041;
  F.ParseStatus = ExceptionParseStatus::Partial;
  rejected("incomplete");
  F.ParseStatus = ExceptionParseStatus::Complete;
  F.Kind = RuntimeFunctionKind::Chained;
  rejected("chained");
  F.Kind = RuntimeFunctionKind::Primary;
  F.UnwindVersion = 2;
  rejected("V1");
  F.UnwindVersion = 1;
  F.Personality = ExceptionPersonality::GSHandlerCheckSEH;
  rejected("personality");
  F.Personality = ExceptionPersonality::CxxFrameHandler3;
  rejected("personality");
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, UnsupportedXmmRestoreCannotBeSilentlyOmitted) {
  auto &F = handler();
  F.UnwindOperations = {op(UnwindOperationKind::SaveXMM128, 8, 16, 6)};
  rejected("XMM");
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, LateReadFailureLeavesEveryRegisterUnchanged) {
  handler();
  auto &F = function(0x2000);
  F.UnwindOperations = {op(UnwindOperationKind::PushNonVolatile, 2, 0, 3),
                        op(UnwindOperationKind::PushNonVolatile, 1, 0, 12)};
  Caller.PC = Base + 0x2040;
  Words[Caller.GPR[4]] = 0xFEED;
  rejected("unavailable stack word");
  EXPECT_EQ(Reads.size(), 2u);
}

TEST_F(DriverKernelSEH, StackBoundsAreCheckedBeforeAnyBackingRead) {
  auto &F = function();
  F.UnwindOperations = {
      op(UnwindOperationKind::AllocateLarge, 8, UINT64_MAX - 7)};
  rejected("exceeds the current stack");
  EXPECT_TRUE(Reads.empty());
  F.UnwindOperations = {
      op(UnwindOperationKind::SaveNonVolatileFar, 8, UINT64_MAX - 7, 3)};
  rejected("overflows");
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, BadFramePointerAndHandlerAreRejectedWithoutMutation) {
  auto &F = handler();
  F.FrameRegister = 5;
  F.FrameOffset = 32;
  F.UnwindOperations = {op(UnwindOperationKind::SetFramePointer, 8)};
  Caller.GPR[5] = 16;
  rejected("underflows");
  Caller.GPR[5] = Caller.GPR[4] + 32;
  DeniedPC = Base + 0x1080;
  rejected("not executable");
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, ProloguesAndAmbiguousRuntimeRangesFailExplicitly) {
  handler();
  Caller.PC = Base + 0x1008;
  rejected("prologue");
  Caller.PC = Base + 0x1040;
  function();
  rejected("overlapping");
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, FrameBudgetDoesNotInventAHandler) {
  Caller.PC = Base + 0x2040;
  for (uint64_t I = 0; I < seh::MaxFrames; ++I)
    Words[Caller.GPR[4] + I * 8] = Base + 0x2041;
  rejected("frame limit");
  EXPECT_EQ(Reads.size(), seh::MaxFrames);
}

TEST_F(DriverKernelSEH, MissingPartialDirectoryIsNotTreatedAsALeaf) {
  Words[Caller.GPR[4]] = Base + ImageSize + 0x100;
  Metadata.ParseStatus = ExceptionParseStatus::Partial;
  rejected("directory");
  EXPECT_TRUE(Reads.empty());
  Metadata.StructuralDecode.emplace();
  Metadata.StructuralDecode->ParseStatus = ExceptionParseStatus::Malformed;
  rejected("directory");
  EXPECT_TRUE(Reads.empty());
  Metadata.StructuralDecode->ParseStatus = ExceptionParseStatus::Complete;
  auto Result = plan();
  ASSERT_TRUE(bool(Result));
  EXPECT_FALSE(*Result);
  EXPECT_EQ(Reads.size(), 1u);
}

TEST_F(DriverKernelSEH, UnrelatedLanguageSummaryDoesNotRejectKnownFrame) {
  handler();
  Metadata.ParseStatus = ExceptionParseStatus::Partial;
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
}

} // namespace
