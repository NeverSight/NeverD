//===- KernelSEHTests.cpp - Pure x64 exception transfer tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelSEH.h"

#include <algorithm>
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
  std::map<uint64_t, std::vector<uint8_t>> Instructions;
  uint64_t DeniedPC = 0;
  uint64_t ActualBase = Base;
  uint64_t SecurityCookie = 0x123456789abc;
  unsigned CookieReads = 0;

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
  ExceptionFunction &gsHandler(uint64_t RVA = 0x1000) {
    auto &F = handler(RVA);
    F.Personality = ExceptionPersonality::GSHandlerCheckSEH;
    auto &GS = F.GSCookie.emplace();
    GS.ParseStatus = ExceptionParseStatus::Complete;
    GS.CookieOffset = 32;
    GS.HasExceptionHandler = GS.HasUnwindHandler = true;
    return F;
  }
  ExceptionFunction &standaloneGS(uint64_t RVA = 0x2000) {
    auto &F = gsHandler(RVA);
    F.Personality = ExceptionPersonality::GSHandlerCheck;
    F.SEH.reset();
    F.GSCookie->HasExceptionHandler = false;
    F.GSCookie->HasUnwindHandler = false;
    return F;
  }
  static SEHScopeRecord scope(uint64_t Begin, uint64_t End, uint64_t Handler) {
    SEHScopeRecord S;
    S.Kind = SEHScopeKind::CatchAll;
    S.GuardedRange = {Base + Begin, Base + End};
    S.HandlerVA = S.ContinuationVA = Base + Handler;
    return S;
  }
  KernelSEH planner() {
    return KernelSEH(
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
        },
        [&](uint64_t PC, llvm::MutableArrayRef<uint8_t> Bytes) {
          std::fill(Bytes.begin(), Bytes.end(), 0xCC);
          for (const auto &[Start, Code] : Instructions)
            for (size_t I = 0; I < Code.size(); ++I)
              if (Start + I >= PC && Start + I - PC < Bytes.size())
                Bytes[Start + I - PC] = Code[I];
          return llvm::Error::success();
        },
        [&]() -> llvm::Expected<uint64_t> {
          ++CookieReads;
          return SecurityCookie;
        });
  }
  llvm::Expected<std::optional<KernelSEH::Transfer>> plan() {
    return planner().plan(Code, Caller, {StackBase, StackSize});
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
    EXPECT_EQ(Caller.Xmm, Before.Xmm);
    EXPECT_EQ(Caller.PC, Before.PC);
    EXPECT_EQ(Words, MemoryBefore);
  }
};

TEST_F(DriverKernelSEH, GSCookieIsCheckedDuringSearchAndTargetUnwind) {
  gsHandler();
  const uint64_t SP = Caller.GPR[4];
  Words[SP + 32] = SP ^ SecurityCookie;
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
  EXPECT_EQ(Reads, (std::vector<uint64_t>{SP + 32, SP + 32}));
  EXPECT_EQ(CookieReads, 2u);
  Words[SP + 32] ^= 1;
  rejected("cookie check failed");
}

TEST_F(DriverKernelSEH, StandaloneGSChecksWithoutInventingLanguageScopes) {
  handler();
  auto &Inner = standaloneGS();
  Inner.UnwindOperations = {op(UnwindOperationKind::AllocateSmall, 8, 64)};
  Caller.PC = Base + 0x2040;
  const auto SP = Caller.GPR[seh::StackRegister];
  Words = {{SP + 32, SP ^ SecurityCookie}, {SP + 64, Base + 0x1041}};
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
  EXPECT_EQ(CookieReads, 2u);
  EXPECT_EQ(Reads, (std::vector<uint64_t>{SP + 32, SP + 64, SP + 32}));
  Words[SP + 32] ^= 1;
  rejected("cookie check failed");
}

TEST_F(DriverKernelSEH, StandaloneGSUnwindOnlyChecksAfterOuterFilterSelection) {
  auto &Outer = handler();
  Outer.SEH->Scopes[0].Kind = SEHScopeKind::Filter;
  Outer.SEH->Scopes[0].FilterOrFinallyVA = Base + 0x3000;
  auto &Inner = standaloneGS();
  Inner.UnwindFlags = seh::UnwindHandlerFlag;
  Inner.UnwindOperations = {op(UnwindOperationKind::AllocateSmall, 8, 64)};
  Caller.PC = Base + 0x2040;
  const auto SP = Caller.GPR[seh::StackRegister];
  Words = {{SP + 32, SP ^ SecurityCookie}, {SP + 64, Base + 0x1041}};
  auto Planner = planner();
  auto State = Planner.begin(Code, Caller, {StackBase, StackSize});
  auto Filter = Planner.advance(State);
  ASSERT_TRUE(bool(Filter)) << llvm::toString(Filter.takeError());
  EXPECT_EQ(Filter->Kind, KernelSEH::ActionKind::Filter);
  EXPECT_EQ(CookieReads, 0u);
  Words[SP + 32] ^= 1;
  auto Corrupt = Planner.advance(State, 1);
  ASSERT_FALSE(bool(Corrupt));
  EXPECT_NE(llvm::toString(Corrupt.takeError()).find("cookie check failed"),
            std::string::npos);
  Words[SP + 32] ^= 1;
  auto Handler = Planner.advance(State, 1);
  ASSERT_TRUE(bool(Handler)) << llvm::toString(Handler.takeError());
  EXPECT_EQ(Handler->Kind, KernelSEH::ActionKind::Handler);
}

TEST_F(DriverKernelSEH,
       StandaloneGSRejectsMissingCookieAndExtraLanguageTables) {
  auto &F = standaloneGS();
  Caller.PC = Base + 0x2040;
  const auto Cookie = F.GSCookie;
  F.GSCookie.reset();
  rejected("inconsistent C exception-handler metadata");
  F.GSCookie = Cookie;
  F.SEH.emplace();
  rejected("inconsistent C exception-handler metadata");
  F.SEH.reset();
  F.GSCookie->ParseStatus = ExceptionParseStatus::Malformed;
  rejected("GS cookie metadata");
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, GSAlignedSlotUsesOriginalFramePointerForEncoding) {
  auto &F = gsHandler();
  F.FrameRegister = 5;
  F.FrameOffset = 48;
  F.UnwindOperations = {op(UnwindOperationKind::SetFramePointer, 8)};
  F.GSCookie->HasAlignment = true;
  F.GSCookie->Alignment = 64;
  F.GSCookie->AlignmentBaseOffset = -8;
  F.GSCookie->CookieOffset = -16;
  Caller.GPR[5] = Caller.GPR[4] + F.FrameOffset;
  const uint64_t Slot = ((Caller.GPR[4] - 8) & ~uint64_t(63)) - 16;
  Words[Slot] = Caller.GPR[5] ^ SecurityCookie;
  ActualBase = 0xfffff80000000000;
  Caller.PC = ActualBase + 0x1040;
  EXPECT_EQ(selected().HandlerPC, ActualBase + 0x1080);
  EXPECT_EQ(Reads, (std::vector<uint64_t>{Slot, Slot}));
}

TEST_F(DriverKernelSEH, GSRejectsTruncatedOrMalformedCookieBeforeStackReads) {
  auto &F = gsHandler();
  F.GSCookie->ParseStatus = ExceptionParseStatus::Partial;
  rejected("GS cookie metadata");
  F.GSCookie->ParseStatus = ExceptionParseStatus::Complete;
  F.GSCookie->CookieOffset = 7;
  rejected("GS cookie metadata");
  F.GSCookie->CookieOffset = 32;
  F.GSCookie->HasAlignment = true;
  for (uint32_t Alignment : {0u, 3u}) {
    F.GSCookie->Alignment = Alignment;
    rejected("GS cookie metadata");
  }
  F.GSCookie->Alignment = 32;
  F.UnwindFlags = seh::ExceptionHandlerFlag;
  rejected("GS cookie metadata");
  EXPECT_TRUE(Reads.empty());
  EXPECT_EQ(CookieReads, 0u);
}

TEST_F(DriverKernelSEH, GSBoundsDoNotPermitReadsOutsideCurrentStack) {
  auto &F = gsHandler();
  for (int32_t Offset : {int32_t(StackSize), -int32_t(StackSize)}) {
    F.GSCookie->CookieOffset = Offset;
    rejected("cookie exceeds");
  }
  EXPECT_TRUE(Reads.empty());
  EXPECT_EQ(CookieReads, 0u);
}

TEST_F(DriverKernelSEH, GSRejectsNonzeroUpperCookieBitsEvenWhenValuesMatch) {
  gsHandler();
  SecurityCookie |= uint64_t(1) << 48;
  Words[Caller.GPR[4] + 32] = Caller.GPR[4] ^ SecurityCookie;
  rejected("cookie check failed");
}

TEST_F(DriverKernelSEH, GSSignedSlotArithmeticCannotWrapIntoTheStack) {
  auto &F = gsHandler();
  auto Planner = planner();
  for (bool Overflow : {false, true}) {
    const uint64_t SP = Overflow ? UINT64_MAX - 15 : 8;
    F.GSCookie->CookieOffset = Overflow ? 32 : INT32_MIN;
    Caller.GPR[4] = SP;
    auto Result = Planner.plan(Code, Caller, {SP, 8});
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError())
                  .find(Overflow ? "overflows" : "underflows"),
              std::string::npos);
  }
  EXPECT_TRUE(Reads.empty());
  EXPECT_EQ(CookieReads, 0u);
}

TEST_F(DriverKernelSEH, GSRequiresAnExplicitImageCookieReader) {
  gsHandler();
  KernelSEH Planner(
      Metadata, Base, Base, ImageSize,
      [&](uint64_t) -> llvm::Expected<uint64_t> {
        ADD_FAILURE() << "missing cookie authority must fail before reads";
        return 0;
      },
      [](uint64_t) { return true; });
  auto Result = Planner.plan(Code, Caller, {StackBase, StackSize});
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("image security cookie"),
            std::string::npos);
}

TEST_F(DriverKernelSEH, GSRechecksMutationByFilterBeforeTransferringControl) {
  auto &F = gsHandler();
  F.SEH->Scopes[0].Kind = SEHScopeKind::Filter;
  F.SEH->Scopes[0].FilterOrFinallyVA = Base + 0x2000;
  const auto Slot = Caller.GPR[4] + 32;
  Words[Slot] = Caller.GPR[4] ^ SecurityCookie;
  auto Planner = planner();
  for (bool ChangeImageCookie : {false, true}) {
    auto State = Planner.begin(Code, Caller, {StackBase, StackSize});
    auto Filter = Planner.advance(State);
    ASSERT_TRUE(bool(Filter)) << llvm::toString(Filter.takeError());
    ASSERT_EQ(Filter->Kind, KernelSEH::ActionKind::Filter);
    auto &Changed = ChangeImageCookie ? SecurityCookie : Words[Slot];
    Changed ^= 1;
    auto Invalid = Planner.advance(State, 1);
    ASSERT_FALSE(bool(Invalid));
    EXPECT_NE(llvm::toString(Invalid.takeError()).find("cookie check failed"),
              std::string::npos);
    Changed ^= 1;
    auto Retried = Planner.advance(State, 1);
    ASSERT_TRUE(bool(Retried)) << llvm::toString(Retried.takeError());
    EXPECT_EQ(Retried->Kind, KernelSEH::ActionKind::Handler);
  }
}

TEST_F(DriverKernelSEH, GSChecksBeforeFinallyAndAgainInEachOuterFrame) {
  gsHandler();
  auto &Inner = gsHandler(0x2000);
  Inner.UnwindOperations = {op(UnwindOperationKind::AllocateSmall, 8, 64)};
  auto &Cleanup = Inner.SEH->Scopes[0];
  Cleanup.Kind = SEHScopeKind::Finally;
  Cleanup.HandlerVA = Cleanup.FilterOrFinallyVA = Base + 0x3000;
  Cleanup.ContinuationVA = 0;
  Caller.PC = Base + 0x2040;
  const auto SP = Caller.GPR[4];
  const auto OuterSP = SP + 72;
  Words = {{SP + 32, SP ^ SecurityCookie},
           {SP + 64, Base + 0x1041},
           {OuterSP + 32, OuterSP ^ SecurityCookie}};
  auto Planner = planner();
  auto State = Planner.begin(Code, Caller, {StackBase, StackSize});
  auto Finally = Planner.advance(State);
  ASSERT_TRUE(bool(Finally)) << llvm::toString(Finally.takeError());
  EXPECT_EQ(Finally->Kind, KernelSEH::ActionKind::Finally);
  EXPECT_EQ(CookieReads, 3u); // Both search frames, then inner unwind.
  Words[OuterSP + 32] ^= 1;
  auto Invalid = Planner.advance(State);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("cookie check failed"),
            std::string::npos);
  Words[OuterSP + 32] ^= 1;
  auto Handler = Planner.advance(State);
  ASSERT_TRUE(bool(Handler)) << llvm::toString(Handler.takeError());
  EXPECT_EQ(Handler->Kind, KernelSEH::ActionKind::Handler);
}

TEST_F(DriverKernelSEH, GSLanguageFlagsDoNotDisableTheCookieCheck) {
  handler();
  auto &Inner = gsHandler(0x2000);
  Inner.UnwindOperations = {op(UnwindOperationKind::AllocateSmall, 8, 64)};
  Inner.GSCookie->HasExceptionHandler = false;
  Inner.GSCookie->HasUnwindHandler = false;
  Caller.PC = Base + 0x2040;
  const auto SP = Caller.GPR[4];
  Words = {{SP + 32, SP ^ SecurityCookie}, {SP + 64, Base + 0x1041}};
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
  EXPECT_EQ(CookieReads, 2u);
  Words[SP + 32] ^= 1;
  rejected("cookie check failed");
}

TEST_F(DriverKernelSEH, GSUnwindOnlyWrapperDoesNotCheckCookieDuringSearch) {
  handler();
  auto &Inner = gsHandler(0x2000);
  Inner.UnwindFlags = seh::UnwindHandlerFlag;
  Inner.GSCookie->HasExceptionHandler = false;
  Inner.GSCookie->HasUnwindHandler = false;
  Inner.UnwindOperations = {op(UnwindOperationKind::AllocateSmall, 8, 64)};
  Caller.PC = Base + 0x2040;
  const auto SP = Caller.GPR[4];
  Words = {{SP + 32, SP ^ SecurityCookie}, {SP + 64, Base + 0x1041}};
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
  EXPECT_EQ(Reads, (std::vector<uint64_t>{SP + 64, SP + 32}));
  EXPECT_EQ(CookieReads, 1u);
}

TEST_F(DriverKernelSEH, GSPrologueAndEpilogueDoNotReadUnestablishedCookie) {
  handler();
  gsHandler(0x2000);
  Words[Caller.GPR[4]] = Base + 0x1041;
  Caller.PC = Base + 0x2001;
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
  Caller.PC = Base + 0x2040;
  Instructions[Caller.PC] = {0xc3};
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
  EXPECT_EQ(CookieReads, 0u);
}

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

TEST_F(DriverKernelSEH, EpilogueRestoresOnlyInstructionsAfterControlPC) {
  handler();
  auto &Helper = function(0x2000);
  Helper.UnwindOperations = {op(UnwindOperationKind::AllocateSmall, 6, 32),
                             op(UnwindOperationKind::PushNonVolatile, 2, 0, 12),
                             op(UnwindOperationKind::PushNonVolatile, 1, 0, 3)};
  // ADD rsp, 32; POP r12; POP rbx; REP RET.
  Instructions[Base + 0x2040] = {0x48, 0x83, 0xc4, 0x20, 0x41,
                                 0x5c, 0x5b, 0xf3, 0xc3};
  const auto Initial = Caller;
  const auto SP = Caller.GPR[4];
  Words = {{SP + 32, 0x1234}, {SP + 40, 0x5678}, {SP + 48, Base + 0x1041}};
  for (unsigned Offset : {0u, 4u, 6u, 7u}) {
    SCOPED_TRACE(Offset);
    Caller = Initial;
    Caller.PC = Base + 0x2040 + Offset;
    Caller.GPR[4] =
        SP + (Offset ? 32 : 0) + (Offset >= 6 ? 8 : 0) + (Offset >= 7 ? 8 : 0);
    if (Offset >= 6)
      Caller.GPR[12] = 0x1234;
    if (Offset >= 7)
      Caller.GPR[3] = 0x5678;
    const auto Result = selected();
    EXPECT_EQ(Result.Registers.GPR[12], 0x1234u);
    EXPECT_EQ(Result.Registers.GPR[3], 0x5678u);
    EXPECT_EQ(Result.Registers.GPR[4], SP + 56);
    EXPECT_EQ(Result.HandlerPC, Base + 0x1080);
  }
}

TEST_F(DriverKernelSEH, EpilogueUsesFrameRegisterAndDoesNotReadTailTarget) {
  handler();
  auto &Helper = function(0x2000);
  Helper.FrameRegister = 5;
  Helper.FrameOffset = 32;
  Helper.UnwindOperations = {op(UnwindOperationKind::SetFramePointer, 8),
                             op(UnwindOperationKind::AllocateSmall, 5, 32),
                             op(UnwindOperationKind::PushNonVolatile, 1, 0, 5)};
  Caller.PC = Base + 0x2040;
  Caller.GPR[5] = Caller.GPR[4] + 96;
  // LEA rsp, [rbp]; POP rbp; JMP [rax]. RAX is deliberately not mapped.
  Instructions[Caller.PC] = {0x48, 0x8d, 0x65, 0x00, 0x5d, 0xff, 0x20};
  Words = {{Caller.GPR[5], 0x12345678}, {Caller.GPR[5] + 8, Base + 0x1041}};
  const auto Result = selected();
  EXPECT_EQ(Result.Registers.GPR[5], 0x12345678u);
  EXPECT_EQ(Result.Registers.GPR[4], Caller.GPR[5] + 16);
  EXPECT_EQ(Reads.size(), 2u);
}

TEST_F(DriverKernelSEH, EpilogueSkipsCurrentFrameScopesAtRebasedAddress) {
  handler();
  handler(0x2000);
  ActualBase += 0x100000;
  Caller.PC = ActualBase + 0x2040;
  Instructions[Caller.PC] = {0xc3};
  Words[Caller.GPR[4]] = ActualBase + 0x1041;
  EXPECT_EQ(selected().HandlerPC, ActualBase + 0x1080);
}

TEST_F(DriverKernelSEH, ReturnAddressBiasIsNotDecodedAsAnInstruction) {
  handler();
  function(0x2000).UnwindOperations = {
      op(UnwindOperationKind::AllocateSmall, 4, 32)};
  Caller.PC = Base + 0x2040;
  Caller.FromReturnAddress = true;
  // The final byte of a call displacement looks like RET, but the saved
  // return address points at MOV eax, eax in the ordinary function body.
  Instructions[Caller.PC] = {0xc3, 0x89, 0xc0, 0xc3};
  Words[Caller.GPR[4] + 32] = Base + 0x1041;
  EXPECT_EQ(selected().Registers.GPR[4], Caller.GPR[4] + 40);
}

TEST_F(DriverKernelSEH,
       IncompleteEpilogueUsesOrdinaryUnwindWithoutPartialReads) {
  handler();
  function(0x2000).UnwindOperations = {
      op(UnwindOperationKind::AllocateSmall, 4, 32)};
  Caller.PC = Base + 0x2040;
  Instructions[Caller.PC] = {0x5b, 0x90, 0xc3}; // POP rbx; NOP; RET.
  Words[Caller.GPR[4] + 32] = Base + 0x1041;
  EXPECT_EQ(selected().Registers.GPR[3], Caller.GPR[3]);
  EXPECT_EQ(Reads, (std::vector<uint64_t>{Caller.GPR[4] + 32}));
}

TEST_F(DriverKernelSEH, EpilogueRejectsOutOfBoundsRestoresAtomically) {
  function();
  Instructions[Caller.PC] = {0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};
  Caller.GPR[4] = StackBase + StackSize - 16;
  rejected("stack adjustment exceeds");
  EXPECT_TRUE(Reads.empty());
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
    if (Kind == SEHScopeKind::Finally) {
      Unsupported.HandlerVA = Unsupported.FilterOrFinallyVA;
      Unsupported.ContinuationVA = 0;
    }
    F.SEH->Scopes.insert(F.SEH->Scopes.begin(), Unsupported);
    rejected("filter or finally");
    EXPECT_TRUE(Reads.empty());
  }
}

TEST_F(DriverKernelSEH, FiltersSearchBeforeAnyFinallyAndRetainNativeOrder) {
  auto &F = handler();
  auto Filter = scope(0x1030, 0x1060, 0x1070);
  Filter.Kind = SEHScopeKind::Filter;
  Filter.FilterOrFinallyVA = Base + 0x2000;
  auto Cleanup = scope(0x1030, 0x1060, 0);
  Cleanup.Kind = SEHScopeKind::Finally;
  Cleanup.HandlerVA = Cleanup.FilterOrFinallyVA = Base + 0x2100;
  Cleanup.ContinuationVA = 0;
  F.SEH->Scopes.insert(F.SEH->Scopes.begin(), {Cleanup, Filter, Filter});
  auto Planner = planner();
  auto State = Planner.begin(Code, Caller, {StackBase, StackSize});
  auto First = Planner.advance(State);
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  EXPECT_EQ(First->Kind, KernelSEH::ActionKind::Filter);
  EXPECT_EQ(First->State.HandlerPC, Filter.FilterOrFinallyVA);
  auto MissingResult = Planner.advance(State);
  ASSERT_FALSE(bool(MissingResult));
  llvm::consumeError(MissingResult.takeError());
  auto Second = Planner.advance(State, 0);
  ASSERT_TRUE(bool(Second)) << llvm::toString(Second.takeError());
  EXPECT_EQ(Second->Kind, KernelSEH::ActionKind::Filter);
  auto Finally = Planner.advance(State, 9);
  ASSERT_TRUE(bool(Finally)) << llvm::toString(Finally.takeError());
  EXPECT_EQ(Finally->Kind, KernelSEH::ActionKind::Finally);
  EXPECT_EQ(Finally->State.HandlerPC, Cleanup.HandlerVA);
  auto Handler = Planner.advance(State);
  ASSERT_TRUE(bool(Handler)) << llvm::toString(Handler.takeError());
  EXPECT_EQ(Handler->Kind, KernelSEH::ActionKind::Handler);
  EXPECT_EQ(Handler->State.HandlerPC, Filter.ContinuationVA);
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, NegativeFilterRestoresOriginalContextWithoutFinally) {
  auto &F = handler();
  F.SEH->Scopes[0].Kind = SEHScopeKind::Filter;
  F.SEH->Scopes[0].FilterOrFinallyVA = Base + 0x2000;
  auto Planner = planner();
  auto State = Planner.begin(Code, Caller, {StackBase, StackSize});
  auto First = Planner.advance(State);
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  auto Continued = Planner.advance(State, -9);
  ASSERT_TRUE(bool(Continued)) << llvm::toString(Continued.takeError());
  EXPECT_EQ(Continued->Kind, KernelSEH::ActionKind::ContinueExecution);
  EXPECT_EQ(Continued->State.Registers.GPR, Caller.GPR);
  EXPECT_EQ(Continued->State.Registers.PC, Caller.PC);
}

TEST_F(DriverKernelSEH,
       SearchAndHandlerRejectImmutableRecordEditsBeforeAdvancing) {
  auto &F = handler();
  F.SEH->Scopes.front().Kind = SEHScopeKind::Filter;
  F.SEH->Scopes.front().FilterOrFinallyVA = Base + 0x2000;
  F.SEH->Scopes.push_back(F.SEH->Scopes.front());
  constexpr uint64_t Storage = 0x80000000;
  KernelSEH::Exception Raised{Caller, Code, 0, Caller.PC, {}};
  auto Planner = planner();
  for (int32_t Disposition : {0, 1}) {
    for (uint64_t Offset :
         {uint64_t(0), seh::ContextOffset + seh::ContextCSOffset}) {
      SCOPED_TRACE(Disposition);
      SCOPED_TRACE(Offset);
      auto State = Planner.begin(Code, Caller, {StackBase, StackSize});
      auto First = Planner.advance(State);
      ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
      ASSERT_EQ(First->Kind, KernelSEH::ActionKind::Filter);
      auto Bytes = KernelSEH::encodeRecords(Raised, Storage);
      ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
      (*Bytes)[Offset] ^= 1;
      const auto Before = *Bytes;
      auto Invalid =
          Planner.finishFilter(State, Disposition, *Bytes, Raised, Storage);
      ASSERT_FALSE(bool(Invalid));
      EXPECT_NE(llvm::toString(Invalid.takeError())
                    .find("unsupported exception context fields"),
                std::string::npos);
      EXPECT_EQ(*Bytes, Before);
      (*Bytes)[Offset] ^= 1;
      auto Retried =
          Planner.finishFilter(State, Disposition, *Bytes, Raised, Storage);
      ASSERT_TRUE(bool(Retried)) << llvm::toString(Retried.takeError());
      EXPECT_EQ(Retried->Kind, Disposition ? KernelSEH::ActionKind::Handler
                                           : KernelSEH::ActionKind::Filter);
      EXPECT_EQ(Retried->State.ExceptionCode, Code);
    }
  }
}

TEST_F(DriverKernelSEH,
       SearchPreservesIntegerEditsForContinuationWithoutChangingUnwind) {
  auto &F = handler();
  F.SEH->Scopes.front().Kind = SEHScopeKind::Filter;
  F.SEH->Scopes.front().FilterOrFinallyVA = Base + 0x2000;
  F.SEH->Scopes.push_back(F.SEH->Scopes.front());
  constexpr uint64_t Storage = 0x80000000;
  KernelSEH::Exception Raised{Caller, Code, 0, Caller.PC, {}};
  auto Planner = planner();
  for (int32_t Disposition : {-1, 1}) {
    auto State = Planner.begin(Code, Caller, {StackBase, StackSize});
    auto First = Planner.advance(State);
    ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
    auto Bytes = KernelSEH::encodeRecords(Raised, Storage);
    ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
    (*Bytes)[seh::ContextOffset + seh::ContextGPROffset] ^= 1;
    const auto Before = *Bytes;
    auto Second = Planner.finishFilter(State, 0, *Bytes, Raised, Storage);
    ASSERT_TRUE(bool(Second)) << llvm::toString(Second.takeError());
    EXPECT_EQ(Second->Kind, KernelSEH::ActionKind::Filter);
    EXPECT_EQ(Second->State.Registers.GPR, Caller.GPR);
    EXPECT_EQ(*Bytes, Before);
    auto Final =
        Planner.finishFilter(State, Disposition, *Bytes, Raised, Storage);
    ASSERT_TRUE(bool(Final)) << llvm::toString(Final.takeError());
    if (Disposition < 0) {
      EXPECT_EQ(Final->Kind, KernelSEH::ActionKind::ContinueExecution);
      auto Restored =
          Planner.continuation(*Bytes, Raised, Storage, {StackBase, StackSize});
      ASSERT_TRUE(bool(Restored)) << llvm::toString(Restored.takeError());
      EXPECT_EQ(Restored->GPR[seh::ReturnRegister],
                Caller.GPR[seh::ReturnRegister] ^ 1);
    } else {
      EXPECT_EQ(Final->Kind, KernelSEH::ActionKind::Handler);
      EXPECT_EQ(Final->State.Registers.GPR[seh::ReturnRegister], Code);
    }
    EXPECT_EQ(*Bytes, Before);
  }
}

TEST_F(DriverKernelSEH,
       NestedSearchRevisitsProtectedScopesAndClearsBoundaryFlag) {
  auto &Outer = handler();
  Outer.SEH->Scopes.front().Kind = SEHScopeKind::Filter;
  Outer.SEH->Scopes.front().FilterOrFinallyVA = Base + 0x3100;
  auto &Inner = handler(0x2000);
  Inner.SEH->Scopes.front().Kind = SEHScopeKind::Filter;
  Inner.SEH->Scopes.front().FilterOrFinallyVA = Base + 0x3200;
  Caller.PC = Base + 0x2040;
  Words[Caller.GPR[4]] = Base + 0x1041;
  auto Planner = planner();
  auto Parent = Planner.begin(Code, Caller, {StackBase, StackSize});
  auto Filter = Planner.advance(Parent);
  ASSERT_TRUE(bool(Filter)) << llvm::toString(Filter.takeError());
  ASSERT_EQ(Filter->Kind, KernelSEH::ActionKind::Filter);
  auto Child = Caller;
  constexpr uint64_t ChildStack = StackBase + StackSize;
  Child.GPR[4] = ChildStack + 0x100;
  Child.PC = Base + 0x3040;
  Words[Child.GPR[4]] = Base + ImageSize + 1;
  auto Nested = Planner.beginNested(Code + 1, Child, {ChildStack, StackSize},
                                    Parent, *Filter);
  ASSERT_TRUE(bool(Nested)) << llvm::toString(Nested.takeError());
  auto First = Planner.advance(*Nested);
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  EXPECT_EQ(First->State.HandlerPC, Filter->State.HandlerPC);
  EXPECT_EQ(First->ExceptionFlags, seh::ExceptionNestedCallFlag);
  EXPECT_EQ(First->Bounds.Base, StackBase);
  auto Second = Planner.advance(*Nested, 0);
  ASSERT_TRUE(bool(Second)) << llvm::toString(Second.takeError());
  EXPECT_EQ(Second->State.HandlerPC, Base + 0x3100);
  EXPECT_EQ(Second->ExceptionFlags, 0u);
  auto Handled = Planner.advance(*Nested, 1);
  ASSERT_TRUE(bool(Handled)) << llvm::toString(Handled.takeError());
  EXPECT_EQ(Handled->Kind, KernelSEH::ActionKind::Handler);
  EXPECT_EQ(Handled->State.ExceptionCode, Code + 1);
  // Building a nested path never consumes the suspended filter disposition.
  auto Original = Planner.advance(Parent, 1);
  ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
  EXPECT_EQ(Original->State.ExceptionCode, Code);
  EXPECT_EQ(Original->State.HandlerPC, Base + 0x2080);
}

TEST_F(DriverKernelSEH,
       CollidedUnwindSkipsEnteredFinallyButRetainsOuterCleanup) {
  auto &F = handler();
  auto Cleanup = scope(0x1030, 0x1060, 0x2000);
  Cleanup.Kind = SEHScopeKind::Finally;
  Cleanup.FilterOrFinallyVA = Cleanup.HandlerVA;
  Cleanup.ContinuationVA = 0;
  auto OuterCleanup = Cleanup;
  OuterCleanup.HandlerVA = OuterCleanup.FilterOrFinallyVA = Base + 0x2100;
  F.SEH->Scopes.insert(F.SEH->Scopes.begin(), {Cleanup, OuterCleanup});
  auto Planner = planner();
  auto Parent = Planner.begin(Code, Caller, {StackBase, StackSize});
  auto Finally = Planner.advance(Parent);
  ASSERT_TRUE(bool(Finally)) << llvm::toString(Finally.takeError());
  ASSERT_EQ(Finally->Kind, KernelSEH::ActionKind::Finally);
  EXPECT_EQ(Finally->ScopeIndex, 1u);
  auto Child = Caller;
  constexpr uint64_t ChildStack = StackBase + StackSize;
  Child.GPR[4] = ChildStack + 0x100;
  Child.PC = Base + 0x3040;
  Words[Child.GPR[4]] = Base + ImageSize + 1;
  auto Nested = Planner.beginNested(Code + 1, Child, {ChildStack, StackSize},
                                    Parent, *Finally);
  ASSERT_TRUE(bool(Nested)) << llvm::toString(Nested.takeError());
  auto Next = Planner.advance(*Nested);
  ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
  ASSERT_EQ(Next->Kind, KernelSEH::ActionKind::Finally);
  EXPECT_EQ(Next->State.HandlerPC, Base + 0x2100);
  EXPECT_EQ(Next->Bounds.Base, StackBase);
  EXPECT_EQ(Next->State.ExceptionCode, Code + 1);
  auto Handled = Planner.advance(*Nested);
  ASSERT_TRUE(bool(Handled)) << llvm::toString(Handled.takeError());
  EXPECT_EQ(Handled->Kind, KernelSEH::ActionKind::Handler);
  EXPECT_EQ(Handled->State.HandlerPC, Base + 0x1080);
  EXPECT_EQ(Handled->State.ExceptionCode, Code + 1);
}

TEST_F(DriverKernelSEH, ContextRecordsAllowOnlyBoundedIntegerControlChanges) {
  KernelSEH::Exception Raised{Caller,    Code,        0,
                              Caller.PC, {0, 0x1234}, 0x81000000};
  constexpr uint64_t Storage = 0x80000000;
  auto Bytes = KernelSEH::encodeRecords(Raised, Storage);
  ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
  ASSERT_EQ(Bytes->size(), seh::RecordsSize);
  auto Planner = planner();
  auto Decode = [&] {
    return Planner.continuation(*Bytes, Raised, Storage,
                                {StackBase, StackSize});
  };
  auto Original = Decode();
  ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
  EXPECT_EQ(Original->GPR, Caller.GPR);
  (*Bytes)[seh::ContextOffset + seh::ContextGPROffset] ^= 1;
  auto Changed = Decode();
  ASSERT_TRUE(bool(Changed)) << llvm::toString(Changed.takeError());
  EXPECT_EQ(Changed->GPR[0], Caller.GPR[0] ^ 1);
  for (uint64_t Offset :
       {seh::ExceptionFlagsOffset, seh::ExceptionLinkOffset,
        seh::ExceptionPointersOffset, seh::ContextOffset + seh::ContextCSOffset,
        seh::ContextOffset + seh::ContextFlagsOffset,
        seh::ContextOffset + seh::ContextSize - 1}) {
    (*Bytes)[Offset] ^= 1;
    auto Invalid = Decode();
    ASSERT_FALSE(bool(Invalid)) << Offset;
    llvm::consumeError(Invalid.takeError());
    (*Bytes)[Offset] ^= 1;
  }
  (*Bytes)[seh::ContextOffset + seh::ContextEFlagsOffset + 1] ^= 2;
  auto BadFlags = Decode();
  ASSERT_FALSE(bool(BadFlags));
  llvm::consumeError(BadFlags.takeError());
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
  rejected("metadata");
  F.Personality = ExceptionPersonality::CxxFrameHandler3;
  rejected("personality");
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, RestoresBothHalvesOfSavedNonvolatileXmmRegisters) {
  handler();
  auto &F = function(0x2000);
  F.UnwindOperations = {op(UnwindOperationKind::SaveXMM128Far, 12, 32, 15),
                        op(UnwindOperationKind::SaveXMM128, 8, 16, 6),
                        op(UnwindOperationKind::AllocateSmall, 4, 56)};
  Caller.PC = Base + 0x2040;
  const uint64_t SP = Caller.GPR[4];
  Words = {{SP + 16, 0x123456789abcdef0},
           {SP + 24, 0xfedcba9876543210},
           {SP + 32, 0x5a5a112233445566},
           {SP + 40, 0xa5a5887766554433},
           {SP + 56, Base + 0x1041}};
  const auto Result = selected();
  EXPECT_EQ(Result.Registers.Xmm[0],
            (std::array<uint64_t, 2>{0x123456789abcdef0, 0xfedcba9876543210}));
  EXPECT_EQ(Result.Registers.Xmm[9],
            (std::array<uint64_t, 2>{0x5a5a112233445566, 0xa5a5887766554433}));
  EXPECT_EQ(Result.Registers.GPR[4], SP + 64);
}

TEST_F(DriverKernelSEH, XmmBoundsAndPartialReadFailurePreserveInputState) {
  auto &F = function();
  F.UnwindOperations = {op(UnwindOperationKind::SaveXMM128, 8, 0, 6)};
  Caller.GPR[4] = StackBase + StackSize - 8;
  rejected("XMM unwind read exceeds");
  EXPECT_TRUE(Reads.empty());
  Caller.GPR[4] = StackBase + 0x100;
  Words[Caller.GPR[4]] = 0x12345678;
  rejected("unavailable stack word");
  EXPECT_EQ(Reads.size(), 2u);
}

TEST_F(DriverKernelSEH, MalformedXmmRegisterAndOffsetAreRejectedBeforeRead) {
  auto &F = handler();
  for (uint16_t Register : {uint16_t(5), uint16_t(16)}) {
    F.UnwindOperations = {op(UnwindOperationKind::SaveXMM128, 8, 16, Register)};
    rejected("nonvolatile XMM register");
  }
  F.UnwindOperations = {op(UnwindOperationKind::SaveXMM128, 8, 8, 6)};
  rejected("unaligned XMM");
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, ChainedSavesRestoreBeforeThePrimaryFrame) {
  handler();
  auto &Primary = function(0x2000);
  Primary.UnwindInfoRVA = 0x3000;
  Primary.UnwindOperations = {
      op(UnwindOperationKind::AllocateSmall, 6, 56),
      op(UnwindOperationKind::PushNonVolatile, 1, 0, 3)};
  auto &Middle = function(0x2200);
  Middle.Kind = RuntimeFunctionKind::Chained;
  Middle.UnwindFlags = seh::ChainFlag;
  Middle.UnwindInfoRVA = 0x3100;
  Middle.PrimaryFunctionIndex = 1;
  Middle.ChainedPrimaryRange = Metadata.Functions[1].CodeRange;
  Middle.ChainedUnwindInfoRVA = 0x3000;
  Middle.UnwindOperations = {
      op(UnwindOperationKind::SaveNonVolatile, 8, 40, 12)};
  auto &Last = function(0x2400);
  Last.Kind = RuntimeFunctionKind::Chained;
  Last.UnwindFlags = seh::ChainFlag;
  Last.PrimaryFunctionIndex = 2;
  Last.ChainedPrimaryRange = Metadata.Functions[2].CodeRange;
  Last.ChainedUnwindInfoRVA = 0x3100;
  Last.UnwindOperations = {op(UnwindOperationKind::SaveXMM128, 8, 16, 15)};
  Caller.PC = Base + 0x2440;
  const uint64_t SP = Caller.GPR[4];
  Words = {{SP + 16, 0x123456789abcdef0},
           {SP + 24, 0xfedcba9876543210},
           {SP + 40, 0x12121212},
           {SP + 56, 0x33333333},
           {SP + 64, Base + 0x1041}};
  const auto Result = selected();
  EXPECT_EQ(Result.Registers.GPR[3], 0x33333333u);
  EXPECT_EQ(Result.Registers.GPR[12], 0x12121212u);
  EXPECT_EQ(Result.Registers.Xmm[9],
            (std::array<uint64_t, 2>{0x123456789abcdef0, 0xfedcba9876543210}));
  EXPECT_EQ(Result.Registers.GPR[4], SP + 72);
  EXPECT_EQ(Reads, (std::vector<uint64_t>{SP + 16, SP + 24, SP + 40, SP + 56,
                                          SP + 64}));
}

TEST_F(DriverKernelSEH, ChainedPartialPrologueUsesTheEstablishedPrimaryFrame) {
  handler();
  auto &Primary = function(0x2000);
  Primary.UnwindInfoRVA = 0x3000;
  Primary.FrameRegister = 5;
  Primary.FrameOffset = 16;
  Primary.UnwindOperations = {
      op(UnwindOperationKind::SetFramePointer, 9),
      op(UnwindOperationKind::AllocateSmall, 6, 32),
      op(UnwindOperationKind::PushNonVolatile, 1, 0, 5)};
  auto &Secondary = function(0x2200);
  Secondary.Kind = RuntimeFunctionKind::Chained;
  Secondary.UnwindFlags = seh::ChainFlag;
  Secondary.FrameRegister = 5;
  Secondary.FrameOffset = 16;
  Secondary.PrimaryFunctionIndex = 1;
  Secondary.ChainedPrimaryRange = Metadata.Functions[1].CodeRange;
  Secondary.ChainedUnwindInfoRVA = 0x3000;
  Secondary.UnwindOperations = {
      op(UnwindOperationKind::SaveNonVolatile, 8, 16, 12)};
  Caller.PC = Base + 0x2202;
  const uint64_t SP = Caller.GPR[4];
  Caller.GPR[5] = SP + 80;
  Words = {{SP + 96, 0x55555555}, {SP + 104, Base + 0x1041}};
  const auto Result = selected();
  EXPECT_EQ(Result.Registers.GPR[12], Caller.GPR[12]);
  EXPECT_EQ(Result.Registers.GPR[5], 0x55555555u);
  EXPECT_EQ(Result.Registers.GPR[4], SP + 112);
  EXPECT_EQ(Reads.size(), 2u);
}

TEST_F(DriverKernelSEH,
       ChainedFragmentUsesPrimaryScopesAndAllowsProvenOverlap) {
  auto &Primary = handler();
  Primary.CodeRange.End = Base + 0x1300;
  Primary.UnwindInfoRVA = 0x3000;
  Primary.SEH->Scopes.front().GuardedRange = {Base + 0x1220, Base + 0x1260};
  auto &Secondary = function(0x1200);
  Secondary.Kind = RuntimeFunctionKind::Chained;
  Secondary.UnwindFlags = seh::ChainFlag;
  Secondary.PrimaryFunctionIndex = 0;
  Secondary.ChainedPrimaryRange = Metadata.Functions[0].CodeRange;
  Secondary.ChainedUnwindInfoRVA = 0x3000;
  Caller.PC = Base + 0x1240;
  EXPECT_EQ(selected().HandlerPC, Base + 0x1080);
  EXPECT_TRUE(Reads.empty());
}

TEST_F(DriverKernelSEH, BadChainLinksAndStackChangesFailBeforeReadingStack) {
  auto &Primary = function(0x2000);
  Primary.UnwindInfoRVA = 0x3000;
  auto &Secondary = function();
  Secondary.Kind = RuntimeFunctionKind::Chained;
  Secondary.UnwindFlags = seh::ChainFlag;
  Secondary.PrimaryFunctionIndex = 0;
  Secondary.ChainedPrimaryRange = Metadata.Functions[0].CodeRange;
  Secondary.ChainedUnwindInfoRVA = 0x3000;
  Secondary.UnwindOperations = {op(UnwindOperationKind::AllocateSmall, 8, 32)};
  rejected("primary stack allocation");
  Secondary.UnwindOperations.clear();
  Secondary.ChainedUnwindInfoRVA = 0x3100;
  rejected("does not match");
  Secondary.ChainedUnwindInfoRVA = 0x3000;
  Secondary.FrameRegister = 5;
  rejected("different frame register");
  Secondary.FrameRegister = 0;
  Secondary.PrimaryFunctionIndex = 2;
  rejected("index is out of range");
  Secondary.PrimaryFunctionIndex = 1;
  Secondary.UnwindInfoRVA = Secondary.ChainedUnwindInfoRVA;
  Secondary.ChainedPrimaryRange = Secondary.CodeRange;
  rejected("cyclic");
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

TEST_F(DriverKernelSEH, PartialPrologueUndoesOnlyCompletedOperations) {
  for (uint32_t Offset : {0, 2, 6, 9}) {
    SCOPED_TRACE(Offset);
    Metadata.Functions.clear();
    Words.clear();
    Reads.clear();
    handler();
    auto &F = handler(0x2000);
    F.FrameRegister = 5;
    F.FrameOffset = 16;
    F.UnwindOperations = {op(UnwindOperationKind::SetFramePointer, 9),
                          op(UnwindOperationKind::AllocateSmall, 6, 32),
                          op(UnwindOperationKind::PushNonVolatile, 2, 0, 5)};
    // A protected range covering a prologue must not run its language handler.
    F.SEH->Scopes.front().GuardedRange.Begin = Base + 0x2000;
    const uint64_t EntrySP = StackBase + 0x200;
    Caller.PC = Base + 0x2000 + Offset;
    Caller.GPR[4] = EntrySP - (Offset >= 2 ? 8 : 0) - (Offset >= 6 ? 32 : 0);
    Caller.GPR[5] = Offset >= 9 ? Caller.GPR[4] + 16 : UINT64_MAX;
    Words[EntrySP] = Base + 0x1041;
    if (Offset >= 2)
      Words[EntrySP - 8] = 0x13579bdf2468ace0;
    const auto Result = selected();
    EXPECT_EQ(Result.HandlerPC, Base + 0x1080);
    EXPECT_EQ(Result.Registers.GPR[4], EntrySP + 8);
    EXPECT_EQ(Result.Registers.GPR[5],
              Offset >= 2 ? 0x13579bdf2468ace0ULL : UINT64_MAX);
    EXPECT_EQ(Reads.size(), Offset >= 2 ? 2u : 1u);
  }
}

TEST_F(DriverKernelSEH, AmbiguousRuntimeRangesFailExplicitly) {
  handler();
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
