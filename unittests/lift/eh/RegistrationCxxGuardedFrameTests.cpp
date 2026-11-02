//===- RegistrationCxxGuardedFrameTests.cpp - Final GS frames -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the final native candidate and mutations of its actual LLVM frame.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/RegistrationCxxCookieBindings.h"
#include "neverd/backend/llvm/RegistrationCxxGuardedFrame.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86/X86RegistrationLayout.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86/X86RegistrationFrame.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/object/PELayout.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::registration_frame;

class RegistrationCxxGuardedFrame : public ::testing::Test {
protected:
  std::unique_ptr<BinaryImage> Image;
  MedFunc Source;
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module;
  llvm::Function *Parent = nullptr;
  llvm::AllocaInst *Frame = nullptr;
  X86RegistrationFrameLayout Layout;
  std::vector<CxxSourceCallBinding> Calls;
  std::vector<CxxCleanupCallBinding> Cleanups;
  std::set<const llvm::Instruction *> ChainReads;
  std::vector<uint8_t> Binary;
  CompiledImage Native;
  va_t CodeVA = 0;

  void SetUp() override {
    const char *Path = std::getenv("NEVERD_REGISTRATION_GUARDED_PE32");
    if (!Path)
      GTEST_SKIP()
          << "NEVERD_REGISTRATION_GUARDED_PE32 authored GS input absent";
    auto Loaded = COFFLoader().load(Path);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    Image = std::make_unique<BinaryImage>(std::move(*Loaded));
    const auto Export = llvm::find_if(Image->Exports, [](const auto &E) {
      return E.Name == "cleanup_only_parent";
    });
    ASSERT_NE(Export, Image->Exports.end());
    Decoder Decode;
    ASSERT_TRUE(Decode.init(*Image));
    auto Low = CFGBuilder().build(*Image, Decode, Export->Addr, "guarded");
    ASSERT_TRUE(Low.RegistrationStates);
    ASSERT_TRUE(Low.RegistrationStates->SecurityCookiesComplete);
    LowToMedConverter Converter;
    Converter.setBinaryImage(Image.get());
    Source = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
    recoverCallAbi(Source, Arch::X86, {}, Image.get());
    inferMedTypes(Source, Arch::X86);
    ASSERT_TRUE(Source.ExceptionMetadata && Source.ExceptionMetadata->Cxx);
    ASSERT_TRUE(Source.ExceptionMetadata->Cxx->TryBlocks.empty());
    ASSERT_EQ(Source.RegistrationStates->CleanupContracts.size(), 1u);
    MedLLVMEmitter Emitter;
    Module = Emitter.emit({Source}, Context, "guarded", Arch::X86, {},
                          Image.get(), BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    Parent = Module->getFunction(Source.Name);
    ASSERT_TRUE(Parent);
    ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    uint64_t EntrySP = 0;
    for (auto &Block : *Parent)
      for (auto &I : Block) {
        if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I))
          if (const auto *MD = Slot->getMetadata(
                  windows_eh_md::RegistrationFrameAttachment)) {
            ASSERT_FALSE(Frame);
            ASSERT_EQ(MD->getNumOperands(), 2u);
            const auto *Value = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                MD->getOperand(1));
            ASSERT_TRUE(Value);
            Frame = Slot;
            EntrySP = Value->getZExtValue();
          }
        if (const auto *MD =
                I.getMetadata(windows_eh_md::RegistrationOperationAttachment)) {
          ASSERT_EQ(MD->getNumOperands(), 5u);
          uint64_t Values[5];
          for (unsigned N = 0; N != 5; ++N) {
            const auto *Value = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                MD->getOperand(N));
            ASSERT_TRUE(Value);
            Values[N] = Value->getZExtValue();
          }
          if (Values[4] < 2)
            continue;
          const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
          ASSERT_TRUE(Call);
          const auto &States = *Source.RegistrationStates;
          const auto *Check = States.cookieCheck(Values[1], Values[2]);
          const auto *Effect = States.callFrameEffect(Values[1], Values[2]);
          ASSERT_TRUE(bool(Check) != bool(Effect));
          Calls.push_back({Call, Values[0], int(Values[3]), Values[1],
                           Check ? Check->EndAddress : Effect->EndAddress,
                           int(Values[2]),
                           Check ? CxxSourceCallBinding::Kind::Cookie
                                 : CxxSourceCallBinding::Kind::Ordinary});
        }
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
          if (Load->getPointerAddressSpace() == 257) {
            ChainReads.insert(Load);
            for (const auto *Use : Load->users())
              if (const auto *Cast = llvm::dyn_cast<llvm::IntToPtrInst>(Use))
                for (const auto *Read : Cast->users())
                  if (const auto *Head = llvm::dyn_cast<llvm::LoadInst>(Read))
                    ChainReads.insert(Head);
          }
      }
    const auto &Cleanup = Source.RegistrationStates->CleanupContracts.front();
    for (auto &Block : *Parent)
      if (const auto *Pad = llvm::dyn_cast<llvm::CleanupPadInst>(
              &*Block.getFirstNonPHIIt())) {
        uint32_t Ordinal = 0;
        for (const auto &I : Block)
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
              Call && !llvm::isa<llvm::IntrinsicInst>(Call))
            Cleanups.push_back({Call, Pad, Source.Entry, Cleanup.ActionState,
                                Cleanup.RelayTarget, Ordinal++});
      }
    ASSERT_TRUE(Frame);
    ASSERT_EQ(Cleanups.size(), Cleanup.Calls.size());
    const auto Coordinate = cxxRegistrationFrameCoordinate(Source);
    ASSERT_TRUE(Coordinate);
    const auto Size = Frame->getAllocationSize(Module->getDataLayout());
    ASSERT_TRUE(Size && !Size->isScalable());
    const auto Projected = projectX86RegistrationFrame(
        *Coordinate, Size->getFixedValue(), EntrySP, Frame->getAlign().value());
    ASSERT_TRUE(Projected);
    Layout = *Projected;
    auto Error = validate();
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }

  llvm::Error validate() {
    return validateCxxGuardedFrame(*Image, Source, *Parent, *Frame, Layout,
                                   Calls, Cleanups, ChainReads);
  }
  void compile() {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
    auto Input = llvm::MemoryBuffer::getFile(
        std::getenv("NEVERD_REGISTRATION_GUARDED_PE32"));
    ASSERT_TRUE(bool(Input));
    const auto Bytes = (*Input)->getBuffer();
    Binary.assign(Bytes.bytes_begin(), Bytes.bytes_end());
    CodeVA = COFFPatcher().plannedExecSegmentVA(Binary, Arch::X86);
    ASSERT_NE(CodeVA, 0u);
    auto Resolve = [&](llvm::StringRef Symbol,
                       uint32_t) -> std::optional<uint64_t> {
      if (auto VA = parseNdDataSymbol(Symbol))
        return *VA;
      if (auto VA = parseNdCodePtrSymbol(Symbol))
        return *VA;
      for (const auto &Function : *Module) {
        llvm::SmallString<64> Name;
        llvm::Mangler Mangler;
        Mangler.getNameWithPrefix(Name, &Function, false);
        if (Name != Symbol)
          continue;
        auto VA = rewrite_source::getOriginalVA(Function);
        if (!VA) {
          llvm::consumeError(VA.takeError());
          return std::nullopt;
        }
        return *VA;
      }
      return std::nullopt;
    };
    Native = compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                                  CodeVA, Resolve, Image->Base);
    ASSERT_TRUE(Native.Success);
    ASSERT_TRUE(Native.Unresolved.empty());
  }
  llvm::CallBase *call(bool Cookie) {
    for (const auto &Binding : Calls)
      if ((Binding.CallKind == CxxSourceCallBinding::Kind::Cookie) == Cookie)
        return const_cast<llvm::CallBase *>(Binding.Call);
    return nullptr;
  }
  void rejects() {
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validate();
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
};

TEST_F(RegistrationCxxGuardedFrame, NativeCandidateHasIndependentFrameProof) {
  auto ObjectModule = llvm::CloneModule(*Module);
  const auto Compiled =
      Codegen().compile(*ObjectModule, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_FALSE(Compiled.ObjectData.empty());
  EXPECT_FALSE(coff_loader::getCheckedX86CxxPersonalityABI(
      *Image, *Source.ExceptionMetadata));
  // The caller's unguarded catch can independently identify this same CRT.
  // Restrict the lookup to the guarded parent when checking this boundary.
  auto GuardedImage = *Image;
  GuardedImage.ExceptionMetadata.Functions = {*Source.ExceptionMetadata};
  EXPECT_FALSE(
      findCOFFExceptionPersonalityVA(GuardedImage, "__CxxFrameHandler3"));
  // The image-only query stays closed; current source/control/frame replay
  // must independently establish the guarded dispatch preconditions.
  auto Error =
      validateCOFFRegistrationCxxIR(*Parent, *Source.ExceptionMetadata, *Image);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

TEST_F(RegistrationCxxGuardedFrame, InstallsOnlyWithIndependentCompilerProof) {
  ASSERT_NO_FATAL_FAILURE(compile());
  auto Tables = getCheckedCOFFRegistrationCxxTableReceipt(
      *Parent, *Source.ExceptionMetadata, *Image, Native);
  ASSERT_TRUE(bool(Tables)) << llvm::toString(Tables.takeError());
  auto Handler = getCheckedCOFFRegistrationCxxHandlerReceipt(
      *Parent, *Source.ExceptionMetadata, *Image, Native);
  ASSERT_TRUE(bool(Handler)) << llvm::toString(Handler.takeError());
  const std::pair<va_t, va_t> Mapping{Source.Entry, Tables->OwnerVA};
  auto Update = prepareCOFFRegistrationPatch(Binary, *Image, Native, {Mapping},
                                             CodeVA, *Module);
  ASSERT_TRUE(bool(Update)) << llvm::toString(Update.takeError());
  ASSERT_TRUE(Update->Apply);
  ASSERT_EQ(Update->GeneratedCxxGraphs.size(), 1u);
  ASSERT_EQ(COFFPatcher().appendExecSegment(Binary, Native.Bytes,
                                            kNdTextSection, Arch::X86),
            CodeVA);
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  std::optional<size_t> Entry;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    const uint64_t RVA = Source.Entry - Image->Base;
    if (RVA >= Section.VirtualAddress &&
        rangeInBounds(RVA - Section.VirtualAddress, 5, Section.SizeOfRawData))
      Entry = Section.PointerToRawData + RVA - Section.VirtualAddress;
  });
  ASSERT_TRUE(Entry);
  ASSERT_TRUE(rangeInBounds(*Entry, 5, Binary.size()));
  Binary[*Entry] = 0xe9;
  writeLE<uint32_t>(Binary.data() + *Entry + 1,
                    uint32_t(Tables->OwnerVA - Source.Entry - 5));
  auto Applied = applyCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Applied)) << llvm::toString(std::move(Applied));
  auto Checked = validateCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Checked)) << llvm::toString(std::move(Checked));
  if (const char *Output =
          std::getenv("NEVERD_REGISTRATION_GUARDED_OUTPUT_PE32")) {
    std::error_code Error;
    llvm::raw_fd_ostream Stream(Output, Error, llvm::sys::fs::OF_None);
    ASSERT_FALSE(bool(Error)) << Error.message();
    Stream.write(reinterpret_cast<const char *>(Binary.data()), Binary.size());
    Stream.close();
    ASSERT_FALSE(Stream.has_error());
  }
  auto Changed = Native;
  Changed.Bytes[Handler->CodeRange.Begin - Changed.BaseVA] = 0x90;
  auto Rejected = getCheckedCOFFRegistrationCxxHandlerReceipt(
      *Parent, *Source.ExceptionMetadata, *Image, Changed);
  EXPECT_FALSE(bool(Rejected));
  if (!Rejected)
    llvm::consumeError(Rejected.takeError());
}

TEST_F(RegistrationCxxGuardedFrame, CurrentEditsRejectAtEveryInstallationGate) {
  ASSERT_NO_FATAL_FAILURE(compile());
  auto Tables = getCheckedCOFFRegistrationCxxTableReceipt(
      *Parent, *Source.ExceptionMetadata, *Image, Native);
  ASSERT_TRUE(bool(Tables)) << llvm::toString(Tables.takeError());
  const std::pair<va_t, va_t> Mapping{Source.Entry, Tables->OwnerVA};
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Edited = llvm::CloneModule(*Module);
    auto *Function = Edited->getFunction(Parent->getName());
    llvm::CallBase *Cookie = nullptr, *Dispatch = nullptr;
    llvm::AllocaInst *Slot = nullptr;
    std::vector<llvm::CallBase *> CleanupCalls;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
          Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
        auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call || llvm::isa<llvm::IntrinsicInst>(Call))
          continue;
        if (Call->getCallingConv() == llvm::CallingConv::X86_FastCall)
          Cookie = Call;
        else if (Call->getOperandBundle("funclet"))
          CleanupCalls.push_back(Call);
        else if (!Dispatch)
          Dispatch = Call;
      }
    ASSERT_TRUE(Cookie && Dispatch && Slot);
    ASSERT_EQ(CleanupCalls.size(), 2u);
    llvm::IRBuilder<> B(Dispatch);
    switch (Mutation) {
    case 0:
      Cookie->setArgOperand(0, llvm::ConstantPointerNull::get(
                                   llvm::PointerType::get(Context, 0)));
      break;
    case 1:
      Cookie->eraseFromParent();
      break;
    case 2: {
      auto *Alias = new llvm::GlobalVariable(
          *Edited, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
          nullptr,
          makeNdDataSymbol(Source.RegistrationStates->SecurityCookieVA));
      B.CreateStore(B.getInt32(0), Alias);
      break;
    }
    case 3: {
      const auto Offset = Layout.runtimeOffset(
          Source.RegistrationStates->CxxCookieConditions.front().FrameOffset,
          4);
      ASSERT_TRUE(Offset);
      B.CreateStore(B.getInt32(0),
                    B.CreateGEP(B.getInt8Ty(), Slot, B.getInt32(*Offset)));
      break;
    }
    case 4: {
      auto *Argument = llvm::dyn_cast<llvm::GetElementPtrInst>(
          CleanupCalls[1]->getArgOperand(0));
      ASSERT_TRUE(Argument);
      Argument->moveBefore(CleanupCalls[0]->getIterator());
      CleanupCalls[1]->moveBefore(CleanupCalls[0]->getIterator());
      break;
    }
    case 5:
      Cookie->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                          nullptr);
      break;
    case 6:
      B.CreateCall(Edited->getOrInsertFunction(
          "unknown", llvm::FunctionType::get(B.getVoidTy(), false)));
      break;
    case 7:
      Cookie->setCallingConv(llvm::CallingConv::C);
      break;
    case 8:
      CleanupCalls.front()->setArgOperand(
          0,
          llvm::ConstantPointerNull::get(llvm::PointerType::get(Context, 0)));
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(*Edited, &llvm::errs()));
    auto Error = validateCOFFRegistrationCxxIR(
        *Function, *Source.ExceptionMetadata, *Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    auto Table = getCheckedCOFFRegistrationCxxTableReceipt(
        *Function, *Source.ExceptionMetadata, *Image, Native);
    EXPECT_FALSE(bool(Table));
    if (!Table)
      llvm::consumeError(Table.takeError());
    auto Handler = getCheckedCOFFRegistrationCxxHandlerReceipt(
        *Function, *Source.ExceptionMetadata, *Image, Native);
    EXPECT_FALSE(bool(Handler));
    if (!Handler)
      llvm::consumeError(Handler.takeError());
    auto Copy = Native;
    auto Update = prepareCOFFRegistrationPatch(Binary, *Image, Copy, {Mapping},
                                               CodeVA, *Edited);
    EXPECT_FALSE(bool(Update));
    if (!Update)
      llvm::consumeError(Update.takeError());
  }
}

TEST_F(RegistrationCxxGuardedFrame, RejectsCurrentCookieWordAtDispatch) {
  auto *Dispatch = call(false);
  ASSERT_TRUE(Dispatch);
  const auto Slot = Layout.runtimeOffset(
      Source.RegistrationStates->CxxCookieConditions.front().FrameOffset, 4);
  ASSERT_TRUE(Slot);
  llvm::IRBuilder<> B(Dispatch);
  B.CreateStore(B.getInt32(0),
                B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(*Slot)));
  rejects();
}

TEST_F(RegistrationCxxGuardedFrame, RejectsAliasedGlobalCookieWrite) {
  auto *Dispatch = call(false);
  ASSERT_TRUE(Dispatch);
  llvm::IRBuilder<> B(Dispatch);
  auto *Cookie = new llvm::GlobalVariable(
      *Module, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
      nullptr, makeNdDataSymbol(Source.RegistrationStates->SecurityCookieVA));
  B.CreateStore(B.getInt32(0), Cookie);
  rejects();
}

TEST_F(RegistrationCxxGuardedFrame, RejectsChangedRetainedCheckArgument) {
  auto *Cookie = call(true);
  ASSERT_TRUE(Cookie);
  Cookie->setArgOperand(
      0, llvm::ConstantPointerNull::get(llvm::PointerType::get(Context, 0)));
  rejects();
}

TEST_F(RegistrationCxxGuardedFrame, RejectsDeletedOccurrenceBeforeDereference) {
  auto *Cookie = call(true);
  ASSERT_TRUE(Cookie);
  Cookie->eraseFromParent();
  rejects();
}

TEST_F(RegistrationCxxGuardedFrame, RejectsMissingCleanupCoverage) {
  ASSERT_FALSE(Cleanups.empty());
  Cleanups.pop_back();
  rejects();
}

TEST_F(RegistrationCxxGuardedFrame, RejectsExtraCallAndStaleEffects) {
  auto *Dispatch = call(false);
  ASSERT_TRUE(Dispatch);
  llvm::IRBuilder<> B(Dispatch);
  auto *Extra = B.CreateCall(Module->getOrInsertFunction(
      "unknown", llvm::FunctionType::get(B.getVoidTy(), false)));
  rejects();
  Extra->eraseFromParent();
  auto &Callee = Source.RegistrationStates->CalleeContracts.front();
  ASSERT_FALSE(Callee.CodeRanges.empty());
  ++Callee.CodeRanges.front().End;
  rejects();
}

TEST_F(RegistrationCxxGuardedFrame, RejectsBadCandidateBeforePublishingModule) {
  auto Changed = Source;
  bool Altered = false;
  for (auto &Block : Changed.Blocks)
    for (auto &Op : Block.Ops)
      if (!Altered && !Op.Dead && Op.Opcode == NdOp::INT_XOR &&
          Op.NumInputs == 2) {
        Op.Inputs[1] = MedVar::makeConst(0, Op.Inputs[1].Size);
        Altered = true;
      }
  ASSERT_TRUE(Altered);
  llvm::LLVMContext OtherContext;
  MedLLVMEmitter Emitter;
  auto Rejected = Emitter.emit({Changed}, OtherContext, "bad-guard", Arch::X86,
                               {}, Image.get(), BinaryFormat::COFF);
  EXPECT_FALSE(Rejected);
  // A failed candidate must not leave a deferred proof or stale pointer in a
  // reused emitter instance.
  auto Valid = Emitter.emit({Source}, OtherContext, "valid-guard", Arch::X86,
                            {}, Image.get(), BinaryFormat::COFF);
  ASSERT_TRUE(Valid);
  const auto *Function = Valid->getFunction(Source.Name);
  ASSERT_TRUE(Function);
  EXPECT_TRUE(Function->getMetadata(windows_eh_md::NativeAttachment));
}
} // namespace
