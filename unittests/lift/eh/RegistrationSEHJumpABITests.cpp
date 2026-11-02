//===- RegistrationSEHJumpABITests.cpp - Checked jump arguments -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep incidental register and private-stack values out of jump-call ABIs.

#include "../../../lib/ir/med/X86/RegistrationSEHJumpPrivate.h"
#include "gtest/gtest.h"

#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHJump.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

namespace {
using namespace neverd;

MedVar reg(int Id, int Version, uint64_t Offset) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.Id = Id;
  Value.SSAVer = Version;
  Value.RegOff = Offset;
  Value.Size = 4;
  Value.TheArch = Arch::X86;
  return Value;
}

MedOp operation(NdOp Opcode, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

struct JumpCallFixture {
  BinaryImage Image;
  MedFunc Function;
  MedVar Buffer = reg(2, 1, x86reg::RAX);
  MedVar ResumeBuffer = reg(2, 3, x86reg::RAX);

  JumpCallFixture() {
    Image.Arch = Arch::X86;
    Image.Format = BinaryFormat::COFF;
    Function.Entry = 0x1000;
    auto &EH = Function.ExceptionMetadata.emplace();
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    EH.CodeRange = {0x1000, 0x1025};
    EH.Registration.emplace();
    EH.Registration->SeededTryLevel = -1;
    EH.Registration->Scopes = {{-1, 0x4000, 0x4100, false}};
    auto &States = Function.RegistrationStates.emplace();
    States.Complete = States.CallbackStatesComplete = true;
    States.RegistrationLifetimeComplete = true;
    States.HasSEHJumpCaptures = true;
    States.SEHJumpCaptures.push_back(
        {0x1010, 0x1015, 10, 0x2000, 0x3000, -96, -128, {0}});
    RegistrationSEHJumpResume Resume;
    Resume.Address = 0x1020;
    Resume.OpSeq = 20;
    Resume.CaptureCall = 0x1010;
    Resume.TargetVA = 0x1015;
    Resume.EntryVA = 0;
    Resume.BufferOffset = -96;
    Resume.StackOffset = -128;
    Resume.ValueOffset = -132;
    Resume.ResumedLevels = {0};
    States.SEHJumpResumptions.push_back(Resume);
    Function.Blocks.resize(2);
    for (unsigned I = 0; I != 2; ++I) {
      auto &Block = Function.Blocks[I];
      Block.Id = I;
      Block.StartAddr = I ? 0x1015 : 0x1000;
      Block.EndAddr = I ? 0x1025 : 0x1015;
      if (!I)
        Block.Succs = {1};
      States.Blocks.push_back(
          {Block.Id, {Block.StartAddr, Block.EndAddr}, {0}});
      // All ABI register banks contain incidental values. The buffer remains
      // in EAX after its explicit stack store, just like the real LEA/PUSH.
      const auto CurrentBuffer = I ? ResumeBuffer : Buffer;
      Block.Ops.push_back(
          operation(NdOp::INT_SUB, CurrentBuffer,
                    {reg(3, 0, x86reg::RBP), MedVar::makeConst(96, 4)}));
      Block.Ops.push_back(operation(NdOp::COPY, reg(4, I + 1, x86reg::RCX),
                                    {MedVar::makeConst(88, 4)}));
      Block.Ops.push_back(operation(NdOp::COPY, reg(5, I + 1, x86reg::RDX),
                                    {MedVar::makeConst(99, 4)}));
      Block.Ops.push_back(operation(NdOp::COPY, reg(6, I + 1, x86reg::XMM0),
                                    {MedVar::makeConst(0x3f800000, 4)}));
      const auto SP = reg(1, I + 1, x86reg::RSP);
      Block.Ops.push_back(operation(
          NdOp::INT_SUB, SP,
          {reg(1, 0, x86reg::RSP), MedVar::makeConst(I ? 136 : 128, 4)}));
      const std::vector<MedVar> Arguments =
          I ? std::vector<MedVar>{CurrentBuffer, MedVar::makeConst(42, 4)}
            : std::vector<MedVar>{CurrentBuffer, MedVar::makeConst(2, 4),
                                  MedVar::makeConst(0x3000, 4),
                                  MedVar::makeConst(0, 4)};
      for (unsigned J = 0; J <= Arguments.size(); ++J) {
        // One extra private spill must not extend the protocol's argument run.
        MedVar Address = reg(10 + J, I + 1, 0);
        Address.Kind = MedVar::Temp;
        Block.Ops.push_back(operation(NdOp::INT_ADD, Address,
                                      {SP, MedVar::makeConst(J * 4, 4)}));
        Block.Ops.push_back(operation(
            NdOp::STORE, {},
            {Address, J == Arguments.size() ? CurrentBuffer : Arguments[J]}));
      }
      MedOp Call = operation(NdOp::CALL, reg(2, I ? 4 : 2, x86reg::RAX),
                             {MedVar::makeConst(I ? 0x2010 : 0x2000, 4)});
      Call.Addr = I ? 0x1020 : 0x1010;
      Call.OriginSeq = I ? 20 : 10;
      Call.DoesNotReturn = I != 0;
      Block.Ops.push_back(Call);
    }
  }

  void recover() {
    // Contradictory inferred prototypes cannot override the source protocol.
    std::map<va_t, int> RegisterArity{{0x2000, 1}, {0x2010, 1}};
    std::map<va_t, int> TotalArity = RegisterArity;
    std::map<va_t, int> FPArity = RegisterArity;
    recoverCallAbi(Function, Arch::X86,
                   {{0x2000, "unnamed_capture"}, {0x2010, "unnamed_resume"}},
                   &Image, &RegisterArity, &TotalArity, &FPArity);
  }
};

TEST(RegistrationSEHJumpABI, SourceProtocolOwnsStackOrderAndExactArity) {
  JumpCallFixture F;
  ASSERT_TRUE(getX86RegistrationSEHJumpCalls(F.Function));
  for (unsigned Pass = 0; Pass != 2; ++Pass) {
    SCOPED_TRACE(Pass);
    F.recover();
    ASSERT_EQ(F.Function.CallInfos.size(), 2u);
    const auto &Capture = F.Function.CallInfos[0];
    const auto &Resume = F.Function.CallInfos[1];
    ASSERT_EQ(Capture.Args.size(), 4u);
    ASSERT_EQ(Resume.Args.size(), 2u);
    EXPECT_EQ(Capture.Args[0], F.Buffer);
    EXPECT_EQ(Capture.Args[1].ConstVal, 2u);
    EXPECT_EQ(Capture.Args[2].ConstVal, 0x3000u);
    EXPECT_EQ(Capture.Args[3].ConstVal, 0u);
    EXPECT_EQ(Resume.Args[0], F.ResumeBuffer);
    EXPECT_EQ(Resume.Args[1].ConstVal, 42u);
    for (const auto &Call : F.Function.CallInfos) {
      EXPECT_EQ(Call.IntegerRegisterArgCount, 0);
      EXPECT_EQ(Call.FloatingRegisterArgCount, 0);
      EXPECT_EQ(Call.StackArgumentSlotBytes, 4u);
    }
    EXPECT_EQ(Capture.StackArgumentOffsets,
              (std::vector<int64_t>{0, 4, 8, 12}));
    EXPECT_EQ(Resume.StackArgumentOffsets, (std::vector<int64_t>{0, 4}));
  }
}

TEST(RegistrationSEHJumpABI, SelectedHandlersDoNotInventCaptureResumption) {
  for (bool AlsoResume : {false, true})
    for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
      SCOPED_TRACE(AlsoResume);
      SCOPED_TRACE(Mutation);
      JumpCallFixture F;
      auto &States = *F.Function.RegistrationStates;
      auto &Resume = States.SEHJumpResumptions.front();
      auto &Scope = F.Function.ExceptionMetadata->Registration->Scopes.front();
      Resume.SelectedScopes = {0};
      Resume.ResumedLevels =
          AlsoResume ? std::vector<int32_t>{-1} : std::vector<int32_t>{};
      Resume.StateRestores =
          AlsoResume ? std::vector<RegistrationSEHJumpState>{{0, -1, 0}}
                     : std::vector<RegistrationSEHJumpState>{};
      MedBlock Handler;
      Handler.Id = 2;
      Handler.StartAddr = Scope.HandlerVA;
      Handler.EndAddr = Scope.HandlerVA + 1;
      F.Function.Blocks.push_back(std::move(Handler));
      States.Blocks.push_back(
          {2, {Scope.HandlerVA, Scope.HandlerVA + 1}, {-1}});
      auto &State = States.Blocks.back();
      State.Reached = true;
      switch (Mutation) {
      case 1:
        Resume.SelectedScopes = {1};
        break;
      case 2:
        Resume.SelectedScopes = {0, 0};
        break;
      case 3:
        Resume.SelectedScopes = {1, 0};
        break;
      case 4:
        Scope.IsFinally = true;
        break;
      case 5:
        Scope.FilterVA = 0;
        break;
      case 6:
        Scope.HandlerVA += 1;
        break;
      case 7:
        State.Reached = false;
        break;
      case 8:
        State.Unknown = true;
        break;
      case 9:
        State.CallbackOnly = true;
        break;
      case 10:
        State.ExplicitLevels = {0};
        break;
      case 11:
        Resume.StateRestores = {{0, 0, 0}};
        break;
      }
      const auto Calls = getX86RegistrationSEHJumpCalls(F.Function);
      ASSERT_EQ(bool(Calls), Mutation == 0);
      if (Calls) {
        ASSERT_EQ(Calls->size(), 2u);
        EXPECT_EQ(Calls->at(0x1010).ResumeLevel, AlsoResume ? -1 : 0);
        EXPECT_TRUE(Calls->at(0x1010).ResumeStateMap.empty());
      }
    }
}

TEST(RegistrationSEHJumpABI, RuntimeArityDoesNotInventMissingStackArguments) {
  JumpCallFixture F;
  for (auto &Block : F.Function.Blocks)
    std::erase_if(Block.Ops, [](const auto &Op) {
      return Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
             Op.Inputs[0].Kind == MedVar::Temp && Op.Inputs[0].Id == 11;
    });
  // The retained source receipt proves the ABI, not newly edited SSA values.
  // A gap in current stores keeps its position as an explicit unspecified
  // word rather than a synthesized zero, and the later words keep theirs.
  F.recover();
  ASSERT_EQ(F.Function.CallInfos.size(), 2u);
  const auto &Capture = F.Function.CallInfos[0];
  const auto &Resume = F.Function.CallInfos[1];
  ASSERT_EQ(Capture.Args.size(), 4u);
  ASSERT_EQ(Resume.Args.size(), 2u);
  for (const auto &Call : F.Function.CallInfos)
    EXPECT_EQ(Call.Args[1].Kind, MedVar::Unspecified);
  EXPECT_EQ(Capture.Args[0], F.Buffer);
  EXPECT_EQ(Capture.Args[2].ConstVal, 0x3000u);
  EXPECT_EQ(Capture.Args[3].ConstVal, 0u);
  EXPECT_EQ(Resume.Args[0], F.ResumeBuffer);
}

TEST(RegistrationSEHJumpABI,
     IncompleteOrChangedReceiptsCannotSelectRuntimeABI) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &States = *F.Function.RegistrationStates;
    auto &Capture = States.SEHJumpCaptures.front();
    auto &Resume = States.SEHJumpResumptions.front();
    auto &CaptureOp = F.Function.Blocks[0].Ops.back();
    switch (Mutation) {
    case 0:
      States.Complete = false;
      break;
    case 1:
      States.CallbackStatesComplete = false;
      break;
    case 2:
      States.RegistrationLifetimeComplete = false;
      break;
    case 3:
      States.SEHJumpCapturesComplete = false;
      break;
    case 4:
      States.SEHJumpResumptionsComplete = false;
      break;
    case 5:
      Capture.OpSeq += 1;
      break;
    case 6:
      Capture.TargetVA += 1;
      break;
    case 7:
      Capture.EndAddress += 1;
      break;
    case 8:
      Resume.CaptureCall += 1;
      break;
    case 9:
      Resume.ValueOffset.reset();
      break;
    case 10:
      CaptureOp.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 11:
      CaptureOp.ExactArguments = true;
      break;
    case 12:
      CaptureOp.Dead = true;
      break;
    case 13:
      F.Function.Blocks[1].Ops.back().DoesNotReturn = false;
      break;
    case 14:
      States.Blocks[0].Unknown = true;
      break;
    }
    EXPECT_FALSE(getX86RegistrationSEHJumpCalls(F.Function));
  }
  JumpCallFixture F;
  F.Function.RegistrationStates->Complete = false;
  F.recover();
  ASSERT_EQ(F.Function.CallInfos.size(), 2u);
  // Merely retaining the runtime name/target must not select stack-only ABI.
  EXPECT_NE(F.Function.CallInfos[0].IntegerRegisterArgCount, 0);
}

TEST(RegistrationSEHJumpABI,
     MultipleStatesRequireACompleteFrameStateProjection) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &Chain = *F.Function.ExceptionMetadata->Registration;
    Chain.SeededTryLevel = -1;
    Chain.Scopes = {{-1, 0x4000, 0x4100, false}};
    auto &Resume = F.Function.RegistrationStates->SEHJumpResumptions.front();
    Resume.ResumedLevels = {-1, 0};
    Resume.StateRestores = {{-1, -1, 0}, {0, 0, 0}};
    switch (Mutation) {
    case 1:
      Resume.StateRestores.clear();
      break;
    case 2:
      Resume.ResumedLevels = {-2, 0};
      break;
    case 3:
      Resume.ResumedLevels = {-1, 1};
      break;
    case 4:
      Resume.ResumedLevels = {0, 0};
      break;
    case 5:
      Resume.ResumedLevels = {0, -1};
      break;
    case 6:
      Resume.ResumedLevels.clear();
      break;
    case 7:
      Chain.SeededTryLevel.reset();
      break;
    }
    const auto Calls = getX86RegistrationSEHJumpCalls(F.Function);
    if (Mutation) {
      EXPECT_FALSE(Calls);
      continue;
    }
    ASSERT_TRUE(Calls);
    ASSERT_EQ(Calls->size(), 2u);
    for (const auto &[Address, Call] : *Calls)
      EXPECT_FALSE(Call.ResumeLevel);
    F.recover();
    ASSERT_EQ(F.Function.CallInfos.size(), 2u);
    EXPECT_EQ(F.Function.CallInfos[0].Args.size(), 4u);
    EXPECT_EQ(F.Function.CallInfos[1].Args.size(), 2u);
  }
}

TEST(RegistrationSEHJumpABI, ProjectsSkippedScopesAndRejectsAmbiguousMappings) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &Chain = *F.Function.ExceptionMetadata->Registration;
    Chain.SeededTryLevel = -1;
    Chain.Scopes = {{-1, 0x4000, 0x4100, false},
                    {0, 0x4200, 0x4300, false},
                    {-1, 0x4400, 0x4500, false}};
    auto &Resume = F.Function.RegistrationStates->SEHJumpResumptions.front();
    Resume.ResumedLevels = {-1, 0};
    Resume.StateRestores = {{-1, -1, 0}, {1, 0, 0}, {2, -1, 0}};
    switch (Mutation) {
    case 1:
      Resume.StateRestores.push_back({2, 0, 0});
      break;
    case 2:
      Resume.StateRestores.push_back({2, -1, 0});
      break;
    case 3:
      std::reverse(Resume.StateRestores.begin(), Resume.StateRestores.end());
      break;
    case 4:
      Resume.StateRestores.back().FrameLevel = 3;
      break;
    case 5:
      Resume.StateRestores.front().FrameLevel = -2;
      break;
    case 6:
      Resume.StateRestores.back().ResumeLevel = -2;
      break;
    case 7:
      Resume.StateRestores.back().ResumeLevel = 3;
      break;
    case 8:
      Resume.StateRestores = {{1, 0, 0}};
      break;
    case 9:
      Resume.ResumedLevels = {0};
      break;
    }
    const auto Calls = getX86RegistrationSEHJumpCalls(F.Function);
    if (Mutation) {
      EXPECT_FALSE(Calls);
      continue;
    }
    ASSERT_TRUE(Calls);
    for (const auto &[Address, Call] : *Calls) {
      EXPECT_FALSE(Call.ResumeLevel);
      EXPECT_EQ(Call.ResumeStateMap,
                (std::map<std::pair<int32_t, int32_t>, int32_t>{{{0, 1}, 0},
                                                                {{0, 2}, -1}}));
    }
  }
}

TEST(RegistrationSEHJumpABI, EH4RequiresTheCookieProtocolAndDisjointStorage) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &EH = *F.Function.ExceptionMetadata;
    EH.Personality = ExceptionPersonality::ExceptHandler4;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
    EH.Registration->RegistrationOffset = -16;
    EH.Registration->TryLevelOffset = -4;
    EH.Registration->EHCookieOffset = -28;
    EH.Registration->GSCookieOffset = -32;
    auto &States = *F.Function.RegistrationStates;
    States.SecurityCookiesComplete = true;
    States.SecurityCookieVA = 0x405000;
    auto &Capture = States.SEHJumpCaptures.front();
    Capture.Protocol = RegistrationSEHJumpCapture::Kind::EH4;
    Capture.SecurityCookieVA = States.SecurityCookieVA;
    if (Mutation == 1)
      Capture.Protocol = RegistrationSEHJumpCapture::Kind::EH3;
    if (Mutation == 2)
      Capture.SecurityCookieVA += 4;
    if (Mutation == 3)
      States.SecurityCookiesComplete = false;
    if (Mutation == 4)
      States.SecurityCookieVA = Capture.SecurityCookieVA = 0;
    if (Mutation == 5)
      EH.Registration->EHCookieOffset = -36;
    if (Mutation == 6)
      EH.Registration->GSCookieOffset = -36;
    if (Mutation == 7)
      Capture.StackOffset = Capture.BufferOffset - 16;
    if (Mutation == 8)
      EH.Personality = ExceptionPersonality::ExceptHandler3;
    EXPECT_EQ(bool(getX86RegistrationSEHJumpCalls(F.Function)), Mutation == 0);
  }
}

TEST(RegistrationSEHJumpABI, CapturedBoundsDisambiguateTheSameVisibleState) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &States = *F.Function.RegistrationStates;
    auto &Capture = States.SEHJumpCaptures.front();
    auto &Resume = States.SEHJumpResumptions.front();
    Capture.Levels = States.Blocks.front().ExplicitLevels = {-1, 0};
    Resume.ResumedLevels = {-1, 0};
    Resume.StateRestores = {{0, -1, -1}, {0, 0, 0}};
    if (Mutation == 1)
      Capture.Levels.clear();
    if (Mutation == 2)
      Capture.Levels = {0, -1};
    if (Mutation == 3)
      Capture.Levels.push_back(0);
    if (Mutation == 4)
      States.Blocks.front().ExplicitLevels = {0};
    if (Mutation == 5)
      Resume.StateRestores.front().CaptureLevel = -2;
    if (Mutation == 6)
      Resume.StateRestores.back().CaptureLevel = -1;
    if (Mutation == 7)
      Resume.StateRestores.clear();
    const auto Calls = getX86RegistrationSEHJumpCalls(F.Function);
    if (Mutation) {
      EXPECT_FALSE(Calls);
      continue;
    }
    ASSERT_TRUE(Calls);
    for (const auto &[Address, Call] : *Calls) {
      EXPECT_FALSE(Call.ResumeLevel);
      EXPECT_EQ(
          Call.ResumeStateMap,
          (std::map<std::pair<int32_t, int32_t>, int32_t>{{{-1, 0}, -1}}));
    }
  }
}

TEST(RegistrationSEHJumpABI, BindsNestedNonlocalCallsToTheirInvocation) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &Function = F.Function;
    auto &States = *Function.RegistrationStates;
    Function.ExceptionMetadata->Registration->Scopes = {{-1, 0, 0x1800, true}};
    auto Callback = Function.Blocks[1];
    Callback.Id = 2;
    Callback.StartAddr = 0x1800;
    Callback.EndAddr = 0x1825;
    Callback.Ops.back().Addr = 0x1820;
    auto &Caller = Function.Blocks[1];
    Caller.Ops.clear();
    auto Call = operation(NdOp::CALL, {}, {MedVar::makeConst(0x1800, 4)});
    Call.Addr = 0x1020;
    Call.OriginSeq = 30;
    Caller.Ops.push_back(Call);
    Caller.Succs = {3};
    Function.Blocks.push_back(std::move(Callback));
    MedBlock Continuation;
    Continuation.Id = 3;
    Continuation.StartAddr = 0x1025;
    Continuation.EndAddr = 0x1026;
    Function.Blocks.push_back(std::move(Continuation));
    RegistrationBlockState CallbackState;
    CallbackState.BlockId = 2;
    CallbackState.Range = {0x1800, 0x1825};
    CallbackState.ExplicitLevels = {0};
    CallbackState.CallbackOnly = CallbackState.Reached = true;
    States.Blocks.push_back(CallbackState);
    auto &Resume = States.SEHJumpResumptions.front();
    Resume.Address = 0x1820;
    Resume.EntryVA = 0x1800;
    Resume.OrdinaryCall = 0x1020;
    Resume.ValueOffset = -8;
    States.SEHControls.push_back(
        {RegistrationSEHControlContract::Kind::NearCall, 0x1020, 0x1025,
         0x1800});
    if (Mutation == 1)
      ++Resume.OrdinaryCall;
    if (Mutation == 2)
      ++Resume.EntryVA;
    if (Mutation == 3)
      States.SEHControls.clear();
    if (Mutation == 4)
      States.SEHJumpResumptionsComplete = false;
    if (Mutation == 5)
      States.SEHInvocationExitsComplete = false;
    if (Mutation == 6)
      Function.Blocks[1].Ops.back().Dead = true;
    if (Mutation == 7)
      Function.Blocks[1].Ops.back().OriginSeq = -1;
    if (Mutation == 8)
      Function.Blocks[1].Ops.back().Inputs[0] = MedVar::makeConst(0x1900, 4);
    if (Mutation == 9) {
      States.SEHJumpResumptions.clear();
      const auto Calls = getRegistrationSEHNonlocalCalls(Function);
      ASSERT_TRUE(Calls);
      EXPECT_TRUE(Calls->empty());
      continue;
    }
    if (Mutation == 10) {
      auto Runtime = Resume;
      Runtime.OrdinaryCall = 0;
      States.SEHJumpResumptions.push_back(Runtime);
    }
    const auto Calls = getRegistrationSEHNonlocalCalls(Function);
    if (Mutation && Mutation != 10) {
      EXPECT_FALSE(Calls);
      continue;
    }
    ASSERT_TRUE(Calls);
    ASSERT_EQ(Calls->size(), 1u);
    EXPECT_EQ(Calls->at({1, 0x1020, 30}), 0x1800u);
    F.recover();
    const auto Finally = llvm::find_if(Function.CallInfos, [](const auto &CI) {
      return CI.TargetAddr == 0x1800;
    });
    ASSERT_NE(Finally, Function.CallInfos.end());
    EXPECT_TRUE(Finally->Args.empty());
    EXPECT_EQ(Finally->IntegerRegisterArgCount, 0);
  }
}

TEST(RegistrationSEHJumpABI, ReplacementsBindTheActualCallbackAndCapture) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &Function = F.Function;
    auto &States = *Function.RegistrationStates;
    Function.ExceptionMetadata->CodeRange.End = 0x1045;
    Function.ExceptionMetadata->Registration->Scopes.push_back(
        {0, 0, 0x1800, true});
    auto Callback = Function.Blocks[1];
    Callback.Id = 3;
    Callback.StartAddr = 0x1800;
    Callback.EndAddr = 0x1825;
    Callback.Ops.back().Addr = 0x1820;
    auto Caller = Function.Blocks[1];
    Caller.Id = 2;
    Caller.StartAddr = 0x1035;
    Caller.EndAddr = 0x1045;
    Caller.Ops.front().Inputs[1] = MedVar::makeConst(176, 4);
    Caller.Ops.back().Addr = 0x1040;
    Caller.Ops.back().OriginSeq = 40;
    auto Capture = Function.Blocks[0];
    Capture.Id = 1;
    Capture.StartAddr = 0x1015;
    Capture.EndAddr = 0x1035;
    Capture.Succs = {2};
    Capture.Ops.front().Inputs[1] = MedVar::makeConst(176, 4);
    Capture.Ops.back().Addr = 0x1030;
    Capture.Ops.back().OriginSeq = 30;
    Function.Blocks[1] = std::move(Capture);
    Function.Blocks.push_back(std::move(Caller));
    Function.Blocks.push_back(std::move(Callback));
    States.Blocks.clear();
    for (const auto &Block : Function.Blocks) {
      RegistrationBlockState State;
      State.BlockId = Block.Id;
      State.Range = {Block.StartAddr, Block.EndAddr};
      State.ExplicitLevels = {0};
      State.Reached = true;
      State.CallbackOnly = Block.Id == 3;
      States.Blocks.push_back(std::move(State));
    }
    States.SEHJumpCaptures.push_back(
        {0x1030, 0x1035, 30, 0x2000, 0x3000, -176, -208, {0}});
    auto CallbackResume = States.SEHJumpResumptions.front();
    CallbackResume.Address = 0x1820;
    CallbackResume.EntryVA = 0x1800;
    CallbackResume.ValueOffset = -8;
    auto &CallerResume = States.SEHJumpResumptions.front();
    CallerResume.Address = 0x1040;
    CallerResume.OpSeq = 40;
    CallerResume.CaptureCall = 0x1030;
    CallerResume.TargetVA = 0x1035;
    CallerResume.BufferOffset = -176;
    CallerResume.StackOffset = -208;
    CallerResume.ValueOffset = -212;
    CallerResume.ResumedLevels.clear();
    CallerResume.Replacements = {{0x1820, 20, 0x1800, 0x1010}};
    auto &Replacement = CallerResume.Replacements.front();
    if (Mutation == 1)
      ++Replacement.Address;
    if (Mutation == 2)
      ++Replacement.OpSeq;
    if (Mutation == 3)
      ++Replacement.EntryVA;
    if (Mutation == 4)
      ++Replacement.CaptureCall;
    if (Mutation == 5)
      Replacement.CaptureCall = CallerResume.CaptureCall;
    if (Mutation == 6)
      CallerResume.Replacements.push_back(Replacement);
    if (Mutation == 7)
      CallbackResume.ResumedLevels.clear();
    if (Mutation == 8)
      Replacement.EntryVA = CallbackResume.EntryVA = 0;
    if (Mutation == 9)
      Function.Blocks.back().Ops.back().DoesNotReturn = false;
    if (Mutation == 10)
      CallbackResume.BufferOffset = -176;
    if (Mutation == 11)
      CallerResume.Replacements.clear();
    if (Mutation == 12)
      CallerResume.ResumedLevels = {0};
    States.SEHJumpResumptions.push_back(std::move(CallbackResume));
    if (Mutation == 13)
      std::reverse(States.SEHJumpResumptions.begin(),
                   States.SEHJumpResumptions.end());
    const auto Calls = getX86RegistrationSEHJumpCalls(Function);
    EXPECT_EQ(bool(Calls), Mutation == 0 || Mutation >= 12);
    if (Calls) {
      EXPECT_EQ(Calls->at(0x1040).Capture->Address, 0x1030u);
      EXPECT_EQ(Calls->at(0x1820).Capture->Address, 0x1010u);
    }
  }
}

TEST(RegistrationSEHJumpABI, ReplacementsMustCloseEveryCallbackPath) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    RegistrationStateAnalysis States;
    RegistrationSEHJumpResume First, Second;
    First.Address = First.EntryVA = 0x1800;
    First.OpSeq = 1;
    First.CaptureCall = 0x1010;
    First.Replacements = {{0x1900, 2, 0x1900, 0x1020}};
    Second.Address = Second.EntryVA = 0x1900;
    Second.OpSeq = 2;
    Second.CaptureCall = 0x1020;
    Second.ResumedLevels = {0};
    if (Mutation == 1 || Mutation == 2) {
      Second.Replacements = {{0x1800, 1, 0x1800, 0x1010}};
      if (Mutation == 1)
        Second.ResumedLevels.clear();
    }
    if (Mutation == 3) {
      Second.ResumedLevels.clear();
      Second.SelectedScopes = {0};
    }
    States.SEHJumpResumptions = {First, Second};
    if (Mutation == 4)
      std::reverse(States.SEHJumpResumptions.begin(),
                   States.SEHJumpResumptions.end());
    size_t Work = 0;
    EXPECT_EQ(hasCompleteSEHJumpReplacements(States, Work),
              Mutation == 0 || Mutation >= 3);
  }
}

} // namespace
