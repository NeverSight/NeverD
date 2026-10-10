//===- WindowsRegistrationRealignedTests.cpp - PE32 callback recovery -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/X86RegistrationFrame.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/high/X86RegistrationFrame.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86RegistrationFrame.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>

using namespace neverd;

namespace neverd {
class MedLLVMEmitterTestPeer {
public:
  static bool rootHasIndependentIdentity(const MedFunc &Func, const MedOp &Op) {
    MedLLVMEmitter E;
    E.CurMedFunc = &Func;
    E.TargetArch = Arch::X86;
    E.TargetFormat = BinaryFormat::COFF;
    const auto Slot = E.canonicalFrameSlotKey(Op.Output);
    return !E.pointerPreservingInput(Op) && Slot &&
           Slot->first == std::make_pair(Op.Output.Id, Op.Output.SSAVer) &&
           Slot->second == 0 && E.addrSlotKey(Op.Output, 0, true) == Slot &&
           E.varIsFrameDerived(Op.Output) &&
           !E.canonicalFrameSlotKey(Op.Output, true);
  }
};
} // namespace neverd

namespace {

std::optional<uint32_t> evaluateRoot(const ExprPtr &Expr, uint32_t EntrySP) {
  if (!Expr)
    return std::nullopt;
  if (Expr->Kind == ExprKind::Var) {
    const auto &Var = Expr->Var;
    if (Var.Kind == MedVar::Reg && Var.TheArch == Arch::X86 && Var.Size == 4 &&
        Var.RegOff == getTargetRegInfo(Arch::X86).StackPointer &&
        Var.SSAVer == 0 && Var.RenameTag < 0)
      return EntrySP;
    return std::nullopt;
  }
  if (Expr->Kind == ExprKind::Const)
    return uint32_t(Expr->ConstVal);
  if (Expr->Kind != ExprKind::BinOp || Expr->Operands.size() != 2)
    return std::nullopt;
  const auto Left = evaluateRoot(Expr->Operands[0], EntrySP);
  const auto Right = evaluateRoot(Expr->Operands[1], EntrySP);
  if (!Left || !Right)
    return std::nullopt;
  switch (Expr->Op) {
  case NdOp::INT_ADD:
    return *Left + *Right;
  case NdOp::INT_SUB:
    return *Left - *Right;
  case NdOp::INT_AND:
    return *Left & *Right;
  default:
    return std::nullopt;
  }
}

#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
TEST(WindowsRegistrationRealigned, RuntimeRootsKeepInvocationIdentity) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (auto Kind :
       {MedOp::RegistrationRootKind::EstablishedFramePointer,
        MedOp::RegistrationRootKind::CallbackStackPointer,
        MedOp::RegistrationRootKind::RestoredStackPointer,
        MedOp::RegistrationRootKind::RealignedFramePointer,
        MedOp::RegistrationRootKind::RealignedRestoredStackPointer}) {
    const bool FP =
        Kind == MedOp::RegistrationRootKind::EstablishedFramePointer ||
        Kind == MedOp::RegistrationRootKind::RealignedFramePointer;
    const bool Restored =
        Kind == MedOp::RegistrationRootKind::RestoredStackPointer ||
        Kind == MedOp::RegistrationRootKind::RealignedRestoredStackPointer;
    MedFunc Func;
    Func.Blocks.resize(1);
    auto &Root = Func.Blocks[0].Ops.emplace_back();
    Root.Opcode = NdOp::COPY;
    Root.Output.Kind = MedVar::Reg;
    Root.Output.TheArch = Arch::X86;
    Root.Output.Id = 1;
    Root.Output.SSAVer = 1;
    Root.Output.RegOff = FP ? TRI.FramePointer : TRI.StackPointer;
    Root.Output.Size = 4;
    Root.addInput(Root.Output);
    Root.Inputs[0].SSAVer = 0;
    Root.RegistrationRoot = Kind;
    Root.RegistrationStackOffset = Restored ? -32 : 0;
    ASSERT_TRUE(hasValidRegistrationRootShape(Root));
    EXPECT_TRUE(MedLLVMEmitterTestPeer::rootHasIndependentIdentity(Func, Root));
    Root.RegistrationRoot = MedOp::RegistrationRootKind::None;
    Root.RegistrationStackOffset = 0;
    EXPECT_FALSE(
        MedLLVMEmitterTestPeer::rootHasIndependentIdentity(Func, Root));
  }
}

TEST(WindowsRegistrationRealigned, EmitsIndependentCallbackFrame) {
  llvm::LLVMContext Context;
  llvm::Module Module("callback-probe", Context);
  Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
  llvm::IRBuilder<> B(Context);
  auto *Parent = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(), false),
      llvm::GlobalValue::ExternalLinkage, "callback_parent", Module);
  Parent->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
  Parent->addFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
  Parent->addFnAttr("frame-pointer", "all");
  Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
      Module
          .getOrInsertFunction("__CxxFrameHandler3",
                               llvm::FunctionType::get(B.getInt32Ty(), true))
          .getCallee()));
  auto MakeBlock = [&](const char *Name) {
    return llvm::BasicBlock::Create(Context, Name, Parent);
  };
  auto *Entry = MakeBlock("entry"), *Normal = MakeBlock("normal"),
       *Dispatch = MakeBlock("dispatch"), *Catch = MakeBlock("catch"),
       *Resume = MakeBlock("resume");
  B.SetInsertPoint(Entry);
  auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 512));
  Frame->setAlignment(llvm::Align(64));
  B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                   &Module, llvm::Intrinsic::localescape),
               {Frame});
  B.CreateStore(B.getInt32(2), Frame)->setVolatile(true);
  auto Throw = Module.getOrInsertFunction(
      "callback_throw", llvm::FunctionType::get(B.getVoidTy(), false));
  B.CreateInvoke(Throw, Normal, Dispatch);
  B.SetInsertPoint(Normal);
  B.CreateUnreachable();
  B.SetInsertPoint(Dispatch);
  auto *Switch =
      B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Catch);
  B.SetInsertPoint(Catch);
  auto *Null = llvm::ConstantPointerNull::get(B.getPtrTy());
  auto *Pad = B.CreateCatchPad(Switch, {Null, B.getInt32(64), Null});
  auto Increment = Module.getOrInsertFunction(
      "callback_increment",
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false));
  llvm::cast<llvm::Function>(Increment.getCallee())
      ->setCallingConv(llvm::CallingConv::X86_ThisCall);
  auto *Call = B.CreateCall(Increment, {Frame},
                            {llvm::OperandBundleDef("funclet", Pad)});
  Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
  Call->setDoesNotThrow();
  B.CreateCatchRet(Pad, Resume);
  B.SetInsertPoint(Resume);
  // The first aligned local is at the captured parent ESP. Read through the
  // actual restored register so a correct ESI alone cannot make this pass.
  auto *Result = B.CreateCall(
      llvm::InlineAsm::get(llvm::FunctionType::get(B.getInt32Ty(), false),
                           "movl (%esp), $0", "=r,~{memory}", true));
  B.CreateRet(Result);
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  auto Object = Codegen().compile(Module, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(Object.Success);
  if (const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_OBJECT")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
              Object.ObjectData.size());
    Out.close();
    ASSERT_FALSE(Out.has_error());
  }
}

TEST(WindowsRegistrationRealigned, InputPE32RecoversTheCallbackContract) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_PE32");
  if (!Path)
    GTEST_SKIP()
        << "run check_windows_registration_realigned.py for the PE32 fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image));
  const auto It =
      llvm::find_if(Image->ExceptionMetadata.Functions, [](const auto &EH) {
        return EH.Registration && EH.Registration->RealignedFrame;
      });
  ASSERT_NE(It, Image->ExceptionMetadata.Functions.end());
  const auto EH = *It;
  ASSERT_TRUE(EH.Cxx);
  ASSERT_EQ(EH.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(EH.Cxx->TryBlocks[0].Handlers.size(), 1u);
  const auto Catch = EH.Cxx->TryBlocks[0].Handlers[0].HandlerVA;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto F =
      CFGBuilder().build(*Image, Decode, EH.CodeRange.Begin, "callback_parent");
  ASSERT_TRUE(F.RegistrationStates);
  const auto &State = *F.RegistrationStates;
  EXPECT_TRUE(State.Complete);
  EXPECT_TRUE(State.CallbackStatesComplete);
  EXPECT_TRUE(State.RegistrationLifetimeComplete);
  EXPECT_TRUE(State.ChainOperationsComplete);
  EXPECT_TRUE(State.CallFrameEffectsComplete);
  EXPECT_TRUE(State.ImageReadsComplete);
  ASSERT_TRUE(State.CxxContinuationsComplete);
  ASSERT_EQ(State.CxxContinuations.size(), 1u);
  EXPECT_EQ(State.CxxContinuations[0].SavedStackOffset,
            EH.Registration->RealignedFrame->BaseOffset);
  EXPECT_TRUE(llvm::any_of(State.Blocks, [&](const auto &B) {
    return B.Range.Begin == State.CxxContinuations[0].TargetVA && B.Reached &&
           !B.Unknown && !B.CallbackOnly;
  }));
  const auto Med =
      LowToMedConverter().convert(F, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "realigned-registration-roots"));
  llvm::LLVMContext RootContext;
  llvm::IRBuilder<> RootBuilder(RootContext);
  bool CatchFrame = false, ResumeFrame = false, ResumeStack = false;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None) {
        SCOPED_TRACE(Block.StartAddr);
        if (Op.RegistrationRoot ==
            MedOp::RegistrationRootKind::RealignedFramePointer) {
          CatchFrame |= Block.StartAddr == Catch;
          ResumeFrame |= Block.StartAddr == State.CxxContinuations[0].TargetVA;
        }
        if (Op.RegistrationRoot ==
            MedOp::RegistrationRootKind::RealignedRestoredStackPointer) {
          EXPECT_EQ(Block.StartAddr, State.CxxContinuations[0].TargetVA);
          EXPECT_EQ(Op.RegistrationStackOffset,
                    State.CxxContinuations[0].SavedStackOffset);
          ResumeStack = true;
        }
        EXPECT_TRUE(hasValidRegistrationRootShape(Op));
        EXPECT_FALSE(
            entryStackOffset(Med, Op.Output, Arch::X86, BinaryFormat::COFF));
        EXPECT_TRUE(
            MedLLVMEmitterTestPeer::rootHasIndependentIdentity(Med, Op));
        if (Op.RegistrationRoot ==
            MedOp::RegistrationRootKind::CallbackStackPointer) {
          EXPECT_FALSE(registrationRootFrameCoordinate(Med, Op));
          continue;
        }
        ASSERT_TRUE(registrationRootFrameCoordinate(Med, Op));
        const auto &Layout = *EH.Registration->RealignedFrame;
        // Exercise every ABI-compatible residue, including PE32 wraparound.
        for (uint32_t Base : {0x10000u, 0xfffffff0u})
          for (uint32_t Residue = 0; Residue < Layout.Alignment; Residue += 4) {
            const uint32_t SP = Base + Residue;
            const uint32_t Expected = ((SP - 16) & -Layout.Alignment) -
                                      Layout.AllocationBytes -
                                      uint32_t(Layout.BaseOffset) +
                                      uint32_t(Op.RegistrationStackOffset);
            auto *Value = emitX86RegistrationRoot(
                Med, Op, RootBuilder.getInt32(SP), RootBuilder);
            const auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Value);
            ASSERT_NE(Constant, nullptr);
            EXPECT_EQ(Constant->getZExtValue(), Expected);
            EXPECT_EQ(evaluateRoot(lowerX86RegistrationRoot(Med, Op), SP),
                      Expected);
          }
      }
  EXPECT_TRUE(CatchFrame);
  EXPECT_TRUE(ResumeFrame);
  EXPECT_TRUE(ResumeStack);
  const auto Throw =
      llvm::find_if(State.CallFrameEffects,
                    [](const auto &Call) { return Call.DoesNotReturn; });
  ASSERT_NE(Throw, State.CallFrameEffects.end());
  const auto ThrowBlock = llvm::find_if(Med.Blocks, [&](const auto &Block) {
    return Block.StartAddr <= Throw->Address && Throw->Address < Block.EndAddr;
  });
  ASSERT_NE(ThrowBlock, Med.Blocks.end());
  EXPECT_TRUE(ThrowBlock->Succs.empty());
  EXPECT_TRUE(llvm::any_of(ThrowBlock->ExceptionalSuccs, [](const auto &Edge) {
    return Edge.Kind == ExceptionalEdgeKind::CxxCatch;
  }));
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Unproved = F;
    auto &Proof = *Unproved.RegistrationStates;
    auto &Call = Proof.CallFrameEffects[Throw - State.CallFrameEffects.begin()];
    if (Mutation == 0)
      Proof.CallFrameEffectsComplete = false;
    if (Mutation == 1)
      Call.DoesNotReturn = false;
    if (Mutation == 2)
      Call.Target += 4;
    if (Mutation == 3)
      ++Call.EndAddress;
    if (Mutation == 4)
      Proof.CallFrameEffects.clear();
    if (Mutation == 5)
      Unproved.OrdinaryModuleAnalysisRoots.insert(
          State.CxxContinuations[0].TargetVA);
    if (Mutation == 6)
      Proof.CxxContinuationsComplete = false;
    if (Mutation == 7)
      Unproved.RegistrationStates.reset();
    const auto Other =
        LowToMedConverter().convert(Unproved, Arch::X86, BinaryFormat::COFF);
    for (const auto &Block : Other.Blocks)
      if (Block.StartAddr == State.CxxContinuations[0].TargetVA)
        for (const auto &Op : Block.Ops)
          EXPECT_NE(Op.RegistrationRoot,
                    MedOp::RegistrationRootKind::RealignedFramePointer);
  }
  const auto High = MedToHighConverter().convert(Med, Arch::X86);
  EXPECT_EQ(High.StructuredExceptionRegions, 0u);
  EXPECT_GT(High.UnstructuredExceptionRegions, 0u);
  ASSERT_EQ(High.Body.size(), 1u);
  EXPECT_FALSE(High.Body.front().EHIsReducible);
  ASSERT_EQ(High.Body.front().EHClauses.size(), 1u);
  EXPECT_EQ(High.Body.front().EHClauses[0].ContinuationVAs,
            (std::vector<va_t>{State.CxxContinuations[0].TargetVA}));
  const auto &Resume = State.CxxContinuations[0];
  unsigned Restores = 0;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Addr != Resume.Address)
      return;
    EXPECT_NE(Stmt.Kind, StmtKind::Return);
    if (Stmt.Kind == StmtKind::Store) {
      ++Restores;
      const auto &Layout = *EH.Registration->RealignedFrame;
      for (uint32_t Base : {0x10000u, 0xfffffff0u})
        for (uint32_t Residue = 0; Residue < Layout.Alignment; Residue += 4) {
          const uint32_t SP = Base + Residue;
          const uint32_t Frame = ((SP - 16) & -Layout.Alignment) -
                                 Layout.AllocationBytes -
                                 uint32_t(Layout.BaseOffset);
          EXPECT_EQ(evaluateRoot(Stmt.StoreAddr, SP), Frame - 16);
          EXPECT_EQ(evaluateRoot(Stmt.StoreVal, SP),
                    Frame + uint32_t(Resume.SavedStackOffset));
        }
    }
  });
  EXPECT_EQ(Restores, 1u);
  const auto &Body = High.Body.front().Body;
  const auto Restore = llvm::find_if(Body, [&](const auto &Stmt) {
    return Stmt.Kind == StmtKind::Store && Stmt.Addr == Resume.Address;
  });
  ASSERT_NE(Restore, Body.end());
  ASSERT_NE(std::next(Restore), Body.end());
  // The continuation folder may place the target directly after its unique
  // catch. Either representation must restore SavedESP before resumption.
  if (std::next(Restore)->Kind == StmtKind::Goto)
    EXPECT_EQ(std::next(Restore)->GotoTarget, Resume.TargetVA);
  else
    EXPECT_EQ(std::next(Restore)->Addr, Resume.TargetVA);

  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Unproved = Med;
    auto &Proof = *Unproved.RegistrationStates;
    if (Mutation == 0)
      Proof.Complete = false;
    if (Mutation == 1)
      Proof.ChainOperationsComplete = false;
    if (Mutation == 2)
      Proof.CallbackStatesComplete = false;
    if (Mutation == 3)
      Proof.RegistrationLifetimeComplete = false;
    if (Mutation == 4)
      Proof.CxxContinuationsComplete = false;
    if (Mutation == 5)
      ++Proof.CxxContinuations[0].EndAddress;
    if (Mutation == 6)
      Proof.CxxContinuations[0].SavedStackOffset = -4;
    if (Mutation == 7)
      ++Proof.CxxContinuations[0].TargetVA;
    if (Mutation == 8)
      Unproved.ExceptionMetadata->Registration->RealignedFrame
          ->SavedParentFrameOffset = -24;
    if (Mutation == 9)
      ++Unproved.Entry;
    const auto Other = MedToHighConverter().convert(Unproved, Arch::X86);
    walkStmts(Other.Body, [&](const HighStmt &Stmt) {
      EXPECT_FALSE(Stmt.Kind == StmtKind::Store && Stmt.Addr == Resume.Address);
    });
  }

  // Source callback analysis does not grant a new native frame/ABI model.
  EXPECT_FALSE(classifyWindowsEHNativeSource(EH, Arch::X86, BinaryFormat::COFF)
                   .canPatchOutput());

  auto Change = [](BinaryImage &Img, va_t Address, uint8_t Byte) {
    for (auto &Segment : Img.Segments)
      if (Address >= Segment.VA && Address - Segment.VA < Segment.Data.size()) {
        Segment.Data[Address - Segment.VA] = Byte;
        return true;
      }
    return false;
  };
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BinaryImage Bad = *Image;
    if (Mutation == 0)
      ASSERT_TRUE(Change(Bad, Catch, 0x50)); // Save EAX instead of runtime EBP.
    if (Mutation == 1)
      ASSERT_TRUE(Change(Bad, State.CxxContinuations[0].Address - 1, 0x58));
    if (Mutation == 2)
      ASSERT_TRUE(Change(Bad, Catch + 3, Bad.readVA(Catch + 3, 1)[0] + 4));
    if (Mutation == 3) {
      // A metadata snapshot alone cannot authenticate a changed allocation.
      const auto Address = EH.CodeRange.Begin + 11;
      ASSERT_TRUE(Change(Bad, Address, Bad.readVA(Address, 1)[0] ^ 4));
    }
    Decoder BadDecode;
    ASSERT_TRUE(BadDecode.init(Bad));
    const auto Broken = CFGBuilder().build(Bad, BadDecode, EH.CodeRange.Begin,
                                           "changed_callback_parent");
    ASSERT_TRUE(Broken.RegistrationStates);
    EXPECT_FALSE(Broken.RegistrationStates->CxxContinuationsComplete &&
                 Broken.RegistrationStates->ImageReadsComplete &&
                 Broken.RegistrationStates->ChainOperationsComplete);
  }

  auto Ordinary = F;
  Ordinary.OrdinaryModuleAnalysisRoots.insert(Catch);
  const auto OrdinaryStates = analyzeRegistrationStates(Ordinary);
  EXPECT_FALSE(OrdinaryStates.CxxContinuationsComplete);
  EXPECT_FALSE(OrdinaryStates.ChainOperationsComplete);

  // Both allocator choices must retain the exact chain-head read/store pair.
  const auto Read = EH.Registration->ChainInstallVA - 13;
  const auto ModRM = Image->readVA(Read, 3)[2];
  ASSERT_TRUE(ModRM == 0x0d || ModRM == 0x15);
  for (bool Matched : {false, true}) {
    BinaryImage Variant = *Image;
    ASSERT_TRUE(Change(Variant, Read + 2, ModRM == 0x0d ? 0x15 : 0x0d));
    if (Matched)
      ASSERT_TRUE(Change(Variant, Read + 8, ModRM == 0x0d ? 0x96 : 0x8e));
    Variant.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Variant);
    const auto *Graph =
        Variant.ExceptionMetadata.findFunction(EH.CodeRange.Begin);
    ASSERT_NE(Graph, nullptr);
    ASSERT_TRUE(Graph->Registration);
    EXPECT_EQ(Graph->Registration->RealignedFrame.has_value(), Matched);
  }
}

#else
TEST(WindowsRegistrationRealigned, RequiresCompilerFrameSupport) {
  GTEST_SKIP() << "LLVM does not provide the checked PE32 C++ frame ABI";
}
#endif

} // namespace
