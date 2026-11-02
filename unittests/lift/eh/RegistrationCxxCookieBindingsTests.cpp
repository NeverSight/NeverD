//===- RegistrationCxxCookieBindingsTests.cpp - PE32 GS coverage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise exact source/current coverage independently of cookie values.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationIRProof.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/RegistrationCxxCookieBindings.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/backend/llvm/X86/X86RegistrationLayout.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace {
using namespace neverd;
using namespace neverd::registration_frame;

constexpr va_t ParentVA = 0x401000;
constexpr va_t HandlerVA = 0x401100;
constexpr va_t RuntimeVA = 0x401180;
constexpr va_t CheckVA = 0x401200;
constexpr va_t RelayVA = 0x401300;
constexpr va_t LeafVA = 0x401400;
constexpr va_t OrdinaryVA = 0x401440;
constexpr va_t TableVA = 0x403000;
constexpr va_t IATVA = 0x4030f0;
constexpr va_t CookieVA = 0x404000;

struct CookieBindingsFixture {
  BinaryImage Image;
  MedFunc Source;
  llvm::LLVMContext Context;
  llvm::Module Module{"cookie-coverage", Context};
  llvm::Function *Parent = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> B{llvm::BasicBlock::Create(Context, "entry", Parent)};
  llvm::AllocaInst *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(208));
  X86RegistrationFrameLayout Layout{208, 68, 64};
  llvm::Function *Checker =
      createX86RegistrationSecurityCheck(Module, "checker");
  llvm::Function *Ordinary = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), false),
      llvm::GlobalValue::ExternalLinkage, "ordinary", Module);
  llvm::Function *Cleanup = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "cleanup", Module);
  std::vector<CxxSourceCallBinding> Calls;
  std::vector<CxxCleanupCallBinding> Cleanups;
  std::map<X86RegistrationCatchIdentity, const llvm::CatchPadInst *> Catches;
  std::vector<llvm::Instruction *> Enters;
  std::vector<llvm::Instruction *> Exits;
  std::string Diagnostic;

  CookieBindingsFixture(bool TwoGuards = false) {
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    Frame->setAlignment(llvm::Align(16));
    rewrite_source::setOriginalVA(*Parent, ParentVA);
    rewrite_source::setOriginalVA(*Checker, CheckVA);
    rewrite_source::setOriginalVA(*Ordinary, OrdinaryVA);
    rewrite_source::setOriginalVA(*Cleanup, LeafVA);
    Cleanup->setCallingConv(llvm::CallingConv::X86_ThisCall);
    auto *Personality = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), true),
        llvm::GlobalValue::ExternalLinkage, "__CxxFrameHandler3", Module);
    Parent->setPersonalityFn(Personality);
    Source.Entry = ParentVA;
    auto &EH = Source.ExceptionMetadata.emplace();
    EH.CodeRange = {ParentVA, ParentVA + 0xa0};
    EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.PersonalityVA = HandlerVA;
    EH.HandlerDataVA = TableVA;
    EH.ParseStatus = ExceptionParseStatus::Complete;
    auto &Chain = EH.Registration.emplace();
    Chain.HandlerVA = HandlerVA;
    Chain.ScopeTableVA = TableVA;
    Chain.RegistrationOffset = -12;
    Chain.TryLevelOffset = -4;
    Chain.SeededTryLevel = -1;
    auto &Cxx = EH.Cxx.emplace();
    Cxx.NativeFuncInfoVA = TableVA;
    Cxx.Magic = 0x19930522;
    Cxx.Version = CxxFuncInfoVersion::WithEHFlags;
    Cxx.MaxState = 1;
    Cxx.Flags = 1;
    Cxx.IsSynchronous = true;
    Cxx.UnwindMap = {{-1, RelayVA, CxxUnwindAction::ActionKind::Direct}};
    auto &States = Source.RegistrationStates.emplace();
    States.Complete = States.CallbackStatesComplete = true;
    States.RegistrationLifetimeComplete = States.ChainOperationsComplete = true;
    States.IncomingFrameAccessesComplete = States.CallFrameEffectsComplete =
        true;
    States.CleanupFrameEffectsComplete = States.CxxContinuationsComplete = true;
    States.CxxCatchObjectsComplete = States.RuntimeObjectAccessesComplete =
        true;
    States.ImageReadsComplete = States.SecurityCookiesComplete = true;
    States.SecurityCookieVA = CookieVA;
    States.CookieCheckVA = CheckVA;
    States.CxxCookieConditions = {{-60, -56}};
    if (TwoGuards)
      States.CxxCookieConditions.push_back({-52, -56});
    States.CalleeContracts.emplace_back().Target = OrdinaryVA;

    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = ParentVA;
    Image.DynInfo.SecurityCookieRVA = CookieVA - Image.Base;
    Segment Text;
    Text.VA = ParentVA;
    Text.Size = Text.FileSz = 0x500;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size, 0xcc);
    Image.Segments.push_back(std::move(Text));
    Section Code;
    Code.VA = ParentVA;
    Code.Size = Code.FileSz = 0x500;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Sections.push_back(Code);
    for (va_t VA : {TableVA, CookieVA}) {
      Segment Data;
      Data.VA = VA;
      Data.Size = Data.FileSz = 0x100;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(Data.Size);
      Image.Segments.push_back(std::move(Data));
    }
    Import Import;
    Import.Name = "__CxxFrameHandler3";
    Import.Module = "MSVCR100.dll";
    Import.IATAddr = IATVA;
    Image.Imports.push_back(Import);
    writeCode(HandlerVA, {0x8b, 0x54, 0x24, 8, 0x8d, 0x42, 0xd4, 0x8b, 0x4a,
                          0xd0, 0x33, 0xc8, 0xe8, 0, 0, 0, 0});
    relativeCall(HandlerVA + 12, CheckVA);
    va_t Body = HandlerVA + 17;
    if (TwoGuards) {
      writeCode(Body, {0x8b, 0x4a, 0xd8, 0x33, 0xc8, 0xe8, 0, 0, 0, 0});
      relativeCall(Body + 5, CheckVA);
      Body += 10;
    }
    writeCode(Body, {0xb8, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0});
    codeWord(Body + 1, TableVA);
    codeWord(Body + 6, RuntimeVA - (Body + 10));
    writeCode(RuntimeVA, {0xff, 0x25, 0, 0, 0, 0});
    codeWord(RuntimeVA + 2, IATVA);
    writeCode(CheckVA, {0x3b, 0x0d, 0, 0, 0, 0, 0x75, 0x10, 0xc3});
    codeWord(CheckVA + 2, CookieVA);
    for (va_t VA : {Body + 1, RuntimeVA + 2, CheckVA + 2})
      Image.BaseRelocations.push_back(
          {VA, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    writeCode(RelayVA,
              {0x55, 0x83, 0xc5, 0,    0x8d, 0x4d, 0xf0, 0xe8, 0, 0,    0,
               0,    0x8d, 0x4d, 0xec, 0xe8, 0,    0,    0,    0, 0x5d, 0xc3});
    relativeCall(RelayVA + 7, LeafVA);
    relativeCall(RelayVA + 15, LeafVA);
    writeCode(LeafVA, {0x8b, 1, 0xc3});
    writeCode(OrdinaryVA, {0x31, 0xc0, 0xc3});
    writeMetadata();

    addSourceCall(false, false, -1); // Pre-install.
    addSourceCall(false, true, -1);  // Live caller-unwind dispatch.
    addSourceCall(false, true, 0);
    addSourceCall(true, true, 0);
    addSourceCall(false, false, -1); // Post-removal.
    B.CreateRetVoid();
    auto *CleanupBlock = llvm::BasicBlock::Create(Context, "unwind", Parent);
    B.SetInsertPoint(CleanupBlock);
    auto *Pad = B.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
    const std::array<llvm::Value *, 2> Objects{
        B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(48)),
        B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(44))};
    for (uint32_t I = 0; I != Objects.size(); ++I) {
      auto *Call = B.CreateCall(Cleanup, {Objects[I]},
                                {llvm::OperandBundleDef("funclet", Pad)});
      Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
      Call->setDoesNotThrow();
      Cleanups.push_back({Call, Pad, ParentVA, 0, RelayVA, I});
    }
    EXPECT_TRUE(refreshCleanupContracts());
    RegistrationCleanupFrameEffect Effect;
    Effect.BlockId = States.Blocks[2].BlockId;
    Effect.Range = States.Blocks[2].Range;
    Effect.DispatchLevel = 0;
    Effect.ActionState = Effect.CleanupIndex = 0;
    States.CleanupFrameEffects.push_back(Effect);
    B.CreateCleanupRet(Pad);
  }

  bool refreshCleanupContracts() {
    LowFunc Function;
    Function.ExceptionMetadata = Source.ExceptionMetadata;
    RegistrationCallCalleeIndex Index(Image);
    auto Contracts = Index.cleanupContracts(Function);
    if (!Contracts || Contracts->size() != 1 ||
        Contracts->front().Calls.size() != 2)
      return false;
    Source.RegistrationStates->CleanupContracts = std::move(*Contracts);
    return true;
  }

  void writeCode(va_t VA, llvm::ArrayRef<uint8_t> Bytes) {
    std::copy(Bytes.begin(), Bytes.end(),
              Image.Segments[0].Data.begin() + VA - ParentVA);
  }
  void codeWord(va_t VA, uint32_t Value) {
    writeLE<uint32_t>(Image.Segments[0].Data.data() + VA - ParentVA, Value);
  }
  void relativeCall(va_t VA, va_t Target) {
    codeWord(VA + 1, Target - (VA + 5));
  }
  void writeMetadata() {
    auto &Bytes = Image.Segments[1].Data;
    std::fill(Bytes.begin(), Bytes.end(), 0);
    auto Word = [&](size_t At, uint32_t Value) {
      writeLE<uint32_t>(Bytes.data() + At, Value);
    };
    const auto &Cxx = *Source.ExceptionMetadata->Cxx;
    Word(0, Cxx.Magic);
    Word(4, Cxx.MaxState);
    Word(8, TableVA + 0x30);
    Word(12, Cxx.TryBlocks.size());
    Word(16, Cxx.TryBlocks.empty() ? 0 : TableVA + 0x60);
    Word(32, Cxx.Flags);
    for (size_t I = 0; I != Cxx.UnwindMap.size(); ++I) {
      Word(0x30 + I * 8, Cxx.UnwindMap[I].ToState);
      Word(0x34 + I * 8, Cxx.UnwindMap[I].ActionVA);
    }
    if (!Cxx.TryBlocks.empty()) {
      Word(0x60, 0);
      Word(0x64, 0);
      Word(0x68, 1);
      Word(0x6c, 1);
      Word(0x70, TableVA + 0x80);
      Word(0x80, 64);
      Word(0x8c, Cxx.TryBlocks[0].Handlers[0].HandlerVA);
    }
  }
  llvm::Metadata *number(uint64_t Value, unsigned Width) {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(B.getIntNTy(Width), Value));
  }
  void identify(CxxSourceCallBinding &Binding) {
    const_cast<llvm::CallBase *>(Binding.Call)
        ->setMetadata(
            windows_eh_md::RegistrationOperationAttachment,
            llvm::MDNode::get(Context, {number(Binding.Owner, 64),
                                        number(Binding.Address, 64),
                                        number(Binding.OpSeq, 32),
                                        number(Binding.BlockId, 32),
                                        number(uint8_t(Binding.CallKind), 8)}));
  }
  llvm::Instruction *boundary(const RegistrationBlockState &State, bool Enter) {
    auto *Call = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::sideeffect));
    Call->setMetadata(
        windows_eh_md::RegistrationBlockAttachment,
        llvm::MDNode::get(Context,
                          {number(ParentVA, 64), number(State.Range.Begin, 64),
                           number(State.Range.End, 64),
                           number(State.BlockId, 32), number(Enter, 1)}));
    return Call;
  }
  llvm::CallInst *addSourceCall(bool Cookie, bool Live, int32_t Level,
                                llvm::CatchPadInst *Pad = nullptr) {
    const auto Index = Source.Blocks.size();
    auto &Block = Source.Blocks.emplace_back();
    Block.Id = 10 + Index; // Deliberately different from the source state ID.
    Block.StartAddr = ParentVA + Index * 16;
    Block.EndAddr = Block.StartAddr + 16;
    auto &State = Source.RegistrationStates->Blocks.emplace_back();
    State.BlockId = 100 + Index;
    State.Range = {Block.StartAddr, Block.EndAddr};
    State.Reached = true;
    State.CanDispatch = Live;
    if (Live)
      State.ExplicitLevels = {Level};
    State.CallbackOnly = Pad != nullptr;
    if (Pad) {
      State.CxxMinimumTryLevel = 1;
      State.CxxCatchStacks = {{{0, 0}}};
    }
    auto &Op = Block.Ops.emplace_back();
    Op.Opcode = NdOp::CALL;
    Op.Addr = Block.StartAddr + 2;
    Op.OriginSeq = 7;
    Op.NumInputs = 1;
    Op.Inputs[0] = MedVar::makeConst(Cookie ? CheckVA : OrdinaryVA, 4);
    if (Cookie)
      Source.RegistrationStates->CookieChecks.push_back(
          {Op.Addr, Op.Addr + 5, 7});
    else {
      RegistrationCallFrameEffect Effect;
      Effect.Address = Op.Addr;
      Effect.EndAddress = Op.Addr + 5;
      Effect.OpSeq = 7;
      Effect.Target = OrdinaryVA;
      Source.RegistrationStates->CallFrameEffects.push_back(Effect);
    }
    Enters.push_back(boundary(State, true));
    llvm::SmallVector<llvm::Value *, 1> Args;
    if (Cookie)
      Args.push_back(llvm::ConstantPointerNull::get(B.getPtrTy()));
    llvm::SmallVector<llvm::OperandBundleDef, 1> Bundles;
    if (Pad)
      Bundles.emplace_back("funclet", Pad);
    auto *Call = B.CreateCall(Cookie ? Checker : Ordinary, Args, Bundles);
    if (Cookie) {
      Call->setCallingConv(llvm::CallingConv::X86_FastCall);
      Call->addParamAttr(0, llvm::Attribute::InReg);
    }
    Calls.push_back({Call, ParentVA, State.BlockId, Op.Addr, Op.Addr + 5, 7,
                     Cookie ? CxxSourceCallBinding::Kind::Cookie
                            : CxxSourceCallBinding::Kind::Ordinary});
    identify(Calls.back());
    Exits.push_back(boundary(State, false));
    return Call;
  }
  llvm::CallInst *addCatchCall() {
    auto &Cxx = *Source.ExceptionMetadata->Cxx;
    Cxx.MaxState = 2;
    Cxx.UnwindMap.push_back({-1, 0, CxxUnwindAction::ActionKind::None});
    CxxTryBlock Try;
    Try.TryLow = Try.TryHigh = 0;
    Try.CatchHigh = 1;
    CxxCatchHandler Handler;
    Handler.HandlerVA = ParentVA + Source.Blocks.size() * 16;
    Handler.Adjectives = 64;
    Try.Handlers.push_back(Handler);
    Cxx.TryBlocks.push_back(Try);
    writeMetadata();
    auto *Search = llvm::BasicBlock::Create(Context, "search", Parent);
    auto *Body = llvm::BasicBlock::Create(Context, "catch", Parent);
    auto *Resume = llvm::BasicBlock::Create(Context, "resume", Parent);
    B.SetInsertPoint(Search);
    auto *Switch =
        B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 1);
    Switch->addHandler(Body);
    B.SetInsertPoint(Body);
    auto *Null = llvm::ConstantPointerNull::get(B.getPtrTy());
    auto *Pad = B.CreateCatchPad(Switch, {Null, B.getInt32(64), Null});
    Catches[{0, 0}] = Pad;
    auto *Call = addSourceCall(false, true, 1, Pad);
    B.CreateCatchRet(Pad, Resume);
    B.SetInsertPoint(Resume);
    B.CreateRetVoid();
    return Call;
  }
  llvm::Expected<CxxCookieCoverage> bind(size_t &Work) {
    return bindCxxCookieCoverage(Image, Source, *Parent, *Frame, Layout, Calls,
                                 Cleanups, Catches, Work);
  }
  bool accepts(size_t InitialWork = 0) {
    auto Result = bind(InitialWork);
    Diagnostic.clear();
    if (!Result) {
      Diagnostic = llvm::toString(Result.takeError());
      return false;
    }
    return true;
  }
  llvm::Error replay(std::vector<CxxSourceCallBinding> &Bindings,
                     coff_registration::SourceCookieCalls Mode =
                         coff_registration::SourceCookieCalls::RetainedCxx) {
    const std::array<const llvm::Function *, 1> Functions{Parent};
    auto Result = coff_registration::validateSourceSegments(
        *Parent, Source, {}, Functions, true, nullptr, Mode, &Bindings);
    return Result ? llvm::Error::success() : Result.takeError();
  }
};

TEST(RegistrationCxxCookieBindings, CoversLiveCallerUnwindAndEveryCleanup) {
  for (bool TwoGuards : {false, true}) {
    CookieBindingsFixture F(TwoGuards);
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    ASSERT_TRUE(coff_loader::getCheckedX86CxxHandlerABI(
        F.Image, *F.Source.ExceptionMetadata));
    EXPECT_FALSE(coff_loader::getCheckedX86CxxPersonalityABI(
        F.Image, *F.Source.ExceptionMetadata));
    size_t Work = 0;
    auto Result = F.bind(Work);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->SecurityCookieVA, CookieVA);
    ASSERT_EQ(Result->Calls.size(), 1u);
    EXPECT_EQ(Result->Calls[0].Call, F.Calls[3].Call);
    EXPECT_EQ(Result->Dispatches.size(), 4u);
    EXPECT_FALSE(Result->Dispatches.count(F.Calls[0].Call));
    EXPECT_TRUE(Result->Dispatches.count(F.Calls[1].Call));
    EXPECT_TRUE(llvm::isa<llvm::CallInst>(F.Calls[1].Call));
    EXPECT_FALSE(Result->Dispatches.count(F.Calls[3].Call));
    EXPECT_FALSE(Result->Dispatches.count(F.Calls[4].Call));
    for (const auto &[Call, Guards] : Result->Dispatches) {
      ASSERT_EQ(Guards.size(), TwoGuards ? 2u : 1u);
      EXPECT_EQ(Guards[0].Slot, (Cell{F.Frame, 4}));
      EXPECT_EQ(Guards[0].XORBase, (Cell{F.Frame, 8}));
      if (TwoGuards)
        EXPECT_EQ(Guards[1].Slot, (Cell{F.Frame, 12}));
    }
    for (const auto &Call : F.Cleanups)
      EXPECT_TRUE(Result->Dispatches.count(Call.Call));
  }
}

TEST(RegistrationCxxCookieBindings, CoversCallsInsideTheActiveCatch) {
  CookieBindingsFixture F;
  auto *Call = F.addCatchCall();
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  size_t Work = 0;
  auto Result = F.bind(Work);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(Result->Dispatches.count(Call));
  F.Calls.pop_back();
  EXPECT_FALSE(F.accepts());
}

TEST(RegistrationCxxCookieBindings, RejectsChangedRuntimeInvocationOwnership) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieBindingsFixture F;
    auto *Call = F.addCatchCall();
    ASSERT_TRUE(F.accepts()) << F.Diagnostic;
    if (Mutation == 0) {
      auto *Changed = llvm::CallBase::removeOperandBundle(
          Call, llvm::LLVMContext::OB_funclet, Call->getIterator());
      F.Calls.back().Call = Changed;
      Call->eraseFromParent();
    }
    if (Mutation == 1)
      const_cast<llvm::CleanupPadInst *>(F.Cleanups[0].Pad)
          ->setParentPad(
              const_cast<llvm::CatchPadInst *>(F.Catches.at({0, 0})));
    if (Mutation == 2)
      F.Catches.clear();
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    EXPECT_FALSE(F.accepts());
  }
}

TEST(RegistrationCxxCookieBindings,
     RequiresCurrentAllocationAlignmentAndBounds) {
  CookieBindingsFixture F;
  F.Source.ExceptionMetadata->Registration->RealignedFrame =
      RegistrationRealignedFrame{4, ParentVA + 3, 16, 64, -64, 0};
  F.Layout.EntrySP = 80;
  ASSERT_TRUE(F.accepts()) << F.Diagnostic;
  F.Frame->setAlignment(llvm::Align(8));
  EXPECT_FALSE(F.accepts());
  F.Frame->setAlignment(llvm::Align(16));
  ASSERT_TRUE(F.accepts()) << F.Diagnostic;
  F.Frame->setOperand(0, F.B.getInt32(80));
  F.Layout.Size = 80;
  F.writeCode(HandlerVA + 6, {0x7f});
  F.Source.RegistrationStates->CxxCookieConditions[0].XORFrameOffset = 115;
  EXPECT_FALSE(F.accepts());
  EXPECT_NE(F.Diagnostic.find("guard leaves"), std::string::npos);
}

TEST(RegistrationCxxCookieBindings, SourceIdentityDoesNotDependOnInputOrder) {
  CookieBindingsFixture F;
  std::reverse(F.Calls.begin(), F.Calls.end());
  std::reverse(F.Cleanups.begin(), F.Cleanups.end());
  EXPECT_TRUE(F.accepts()) << F.Diagnostic;
  rewrite_source::setOriginalVA(*F.Parent, ParentVA + 1);
  EXPECT_FALSE(F.accepts());
}

TEST(RegistrationCxxCookieBindings, HandlerOnlyChecksStillCoverEveryDispatch) {
  CookieBindingsFixture F;
  const_cast<llvm::CallBase *>(F.Calls[3].Call)->eraseFromParent();
  F.Calls.erase(F.Calls.begin() + 3);
  F.Source.Blocks[3].Ops.clear();
  F.Source.RegistrationStates->CookieChecks.clear();
  size_t Work = 0;
  auto Result = F.bind(Work);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(Result->Calls.empty());
  EXPECT_EQ(Result->Dispatches.size(), 4u);
}

TEST(RegistrationCxxCookieBindings, RechecksCleanupObjectAndImageEffects) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieBindingsFixture F;
    F.writeCode(LeafVA, {0x8b, 0x41, 0, 0x89, 0x41, 4, 0xa1, 0, 0, 0, 0, 0xa3,
                         0, 0, 0, 0, 0x31, 0xc0, 0xc3});
    F.codeWord(LeafVA + 7, TableVA + 0x80);
    F.codeWord(LeafVA + 12, TableVA + 0x84);
    ASSERT_TRUE(F.refreshCleanupContracts());
    // The destructor's scalar return is unused; source cleanup contracts do
    // not retain it. Its current object/image effects still need equality.
    ASSERT_FALSE(F.Source.RegistrationStates->CleanupContracts[0]
                     .Calls[0]
                     .Leaf.HasIndependentScalarReturn);
    ASSERT_TRUE(F.accepts());
    switch (Mutation) {
    case 0:
      F.writeCode(LeafVA + 2, {4});
      break;
    case 1:
      F.writeCode(LeafVA + 5, {8});
      break;
    case 2:
      F.codeWord(LeafVA + 7, TableVA + 0x88);
      break;
    case 3:
      F.codeWord(LeafVA + 12, TableVA + 0x88);
      break;
    case 4:
      F.writeCode(LeafVA + 18, {0x90, 0xc3});
      break;
    }
    EXPECT_FALSE(F.accepts());
    ASSERT_TRUE(F.refreshCleanupContracts());
    EXPECT_TRUE(F.accepts());
  }
}

TEST(RegistrationCxxCookieBindings, RechecksEveryOccurrenceField) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieBindingsFixture F;
    auto &Binding = F.Calls[3];
    switch (Mutation) {
    case 0:
      ++Binding.Owner;
      break;
    case 1:
      ++Binding.Address;
      break;
    case 2:
      ++Binding.EndAddress;
      break;
    case 3:
      ++Binding.OpSeq;
      break;
    case 4:
      Binding.BlockId = F.Source.Blocks[3].Id;
      break;
    case 5:
      Binding.CallKind = CxxSourceCallBinding::Kind::Ordinary;
      break;
    case 6:
      F.Calls.erase(F.Calls.begin() + 3);
      break;
    case 7:
      F.Calls.push_back(Binding);
      break;
    case 8:
      F.Calls[2].Call = Binding.Call;
      break;
    }
    if (Mutation < 6)
      F.identify(
          Binding); // A matching edited metadata receipt is insufficient.
    EXPECT_FALSE(F.accepts());
  }
}

TEST(RegistrationCxxCookieBindings, RejectsChangedHandlerAndFrameCoordinates) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieBindingsFixture F(true);
    auto &States = *F.Source.RegistrationStates;
    switch (Mutation) {
    case 0:
      States.SecurityCookiesComplete = false;
      break;
    case 1:
      States.SecurityCookieVA += 4;
      break;
    case 2:
      States.CookieCheckVA += 32;
      break;
    case 3:
      States.CxxCookieConditions.pop_back();
      break;
    case 4:
      States.CxxCookieConditions[0].FrameOffset = -48;
      break;
    case 5:
      ++States.CxxCookieConditions[0].XORFrameOffset;
      break;
    case 6:
      ++F.Layout.Establisher;
      break;
    case 7:
      --F.Layout.Size;
      break;
    case 8:
      F.writeCode(HandlerVA + 9, {0xd1});
      break;
    case 9:
      F.writeCode(CheckVA, {0x90});
      break;
    }
    EXPECT_FALSE(F.accepts());
  }
}

TEST(RegistrationCxxCookieBindings, RejectsExtraCallsAndUnretainedCookie) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieBindingsFixture F;
    auto *Call = const_cast<llvm::CallBase *>(F.Calls[3].Call);
    F.B.SetInsertPoint(Call);
    if (Mutation == 0)
      F.B.CreateCall(F.Ordinary);
    if (Mutation == 1)
      F.Calls[3].Call = F.B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          &F.Module, llvm::Intrinsic::sideeffect));
    if (Mutation == 2)
      Call->removeParamAttr(0, llvm::Attribute::InReg);
    if (Mutation == 3)
      rewrite_source::setOriginalVA(*F.Checker, CheckVA + 32);
    if (Mutation == 4)
      Call->addFnAttr(llvm::Attribute::NoUnwind);
    EXPECT_FALSE(F.accepts());
  }
}

TEST(RegistrationCxxCookieBindings,
     RejectsIncompleteOrConflictingSourceEffects) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieBindingsFixture F;
    auto &States = *F.Source.RegistrationStates;
    switch (Mutation) {
    case 0:
      States.CookieChecks.push_back(States.CookieChecks[0]);
      break;
    case 1:
      States.CallFrameEffects.push_back(States.CallFrameEffects[0]);
      break;
    case 2:
      States.CallFrameEffects[0].Address = States.CookieChecks[0].Address;
      break;
    case 3:
      States.Blocks[1].Unknown = true;
      break;
    case 4:
      F.Source.Blocks.erase(F.Source.Blocks.begin() + 1);
      break;
    case 5:
      States.CookieChecks[0].EndAddress = F.Source.Blocks[3].EndAddr + 1;
      break;
    case 6:
      F.Source.Blocks[1].Ops[0].Opcode = NdOp::INDIR_CALL;
      break;
    }
    EXPECT_FALSE(F.accepts());
  }
}

TEST(RegistrationCxxCookieBindings, ReauthenticatesOrderedCleanupTransfers) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieBindingsFixture F;
    auto *First = const_cast<llvm::CallBase *>(F.Cleanups[0].Call);
    auto *Second = const_cast<llvm::CallBase *>(F.Cleanups[1].Call);
    switch (Mutation) {
    case 0:
      F.Cleanups.pop_back();
      break;
    case 1:
      F.Cleanups.push_back(F.Cleanups[0]);
      break;
    case 2:
      ++F.Cleanups[0].ActionState;
      break;
    case 3:
      ++F.Cleanups[0].RelayTarget;
      break;
    case 4:
      std::swap(F.Cleanups[0].Ordinal, F.Cleanups[1].Ordinal);
      break;
    case 5:
      Second->moveBefore(First->getIterator());
      break;
    case 6:
      First->setArgOperand(0, Second->getArgOperand(0));
      break;
    case 7:
      F.writeCode(RelayVA + 6, {0xf1});
      break;
    case 8:
      F.writeCode(LeafVA, {0xcc});
      break;
    case 9:
      F.Source.RegistrationStates->CleanupContracts.clear();
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    EXPECT_FALSE(F.accepts());
  }
}

TEST(RegistrationCxxCookieBindings, RejectsForeignAndDetachedCurrentCalls) {
  CookieBindingsFixture F;
  auto *Call = const_cast<llvm::CallBase *>(F.Calls[3].Call);
  auto *Next = Call->getNextNode();
  Call->removeFromParent();
  EXPECT_FALSE(F.accepts());
  Call->insertBefore(Next->getIterator());
  auto *Other = llvm::Function::Create(F.Parent->getFunctionType(),
                                       llvm::GlobalValue::ExternalLinkage,
                                       "foreign", F.Module);
  auto *Block = llvm::BasicBlock::Create(F.Context, "entry", Other);
  F.B.SetInsertPoint(Block);
  auto *Return = F.B.CreateRetVoid();
  Call->moveBefore(Return->getIterator());
  EXPECT_FALSE(F.accepts());
  Call->moveBefore(Next->getIterator());
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
}

TEST(RegistrationCxxCookieBindings, HonorsTheSharedExhaustedBudget) {
  CookieBindingsFixture F;
  EXPECT_TRUE(F.accepts()) << F.Diagnostic;
  size_t Work = limits::kMaxRegistrationEHStateWork;
  auto Result = F.bind(Work);
  EXPECT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationCxxCookieBindings,
     SourceSegmentsBindRetainedChecksExplicitly) {
  CookieBindingsFixture F;
  std::vector<CxxSourceCallBinding> Bindings;
  auto Error = F.replay(Bindings);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_EQ(Bindings.size(), F.Calls.size());
  F.Calls = Bindings;
  EXPECT_TRUE(F.accepts()) << F.Diagnostic;
  Error = F.replay(Bindings, coff_registration::SourceCookieCalls::ElidedSEH);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST(RegistrationCxxCookieBindings, SourceSegmentsRejectMovedOrErasedChecks) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieBindingsFixture F;
    auto *Call = const_cast<llvm::CallBase *>(F.Calls[3].Call);
    if (Mutation == 0)
      Call->moveBefore(F.Enters[3]->getIterator());
    if (Mutation == 1)
      Call->moveBefore(F.Exits[2]->getIterator());
    if (Mutation == 2) {
      F.B.SetInsertPoint(Call);
      auto *Marker = F.B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          &F.Module, llvm::Intrinsic::sideeffect));
      Marker->setMetadata(
          windows_eh_md::RegistrationOperationAttachment,
          Call->getMetadata(windows_eh_md::RegistrationOperationAttachment));
      Call->eraseFromParent();
    }
    if (Mutation == 3)
      Call->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                        nullptr);
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    std::vector<CxxSourceCallBinding> Bindings{{}};
    auto Error = F.replay(Bindings);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    ASSERT_EQ(Bindings.size(), 1u); // Failed replay publishes nothing.
    EXPECT_EQ(Bindings[0].Call, nullptr);
  }
}
} // namespace
