//===- RegistrationNestedTryTestUtils.cpp - PE32 nested search checks ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationNestedTryTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Verifier.h"
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/Endian.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

namespace neverd::registration_test {
void checkNestedTryHigh(const MedFunc &Med, const HighFunc &High) {
  const auto &EH = *Med.ExceptionMetadata;
  const HighStmt *Outer = nullptr;
  for (const auto &Stmt : High.Body)
    if (Stmt.Kind == StmtKind::CxxTry && Stmt.EHClauses.size() == 2)
      Outer = &Stmt;
  ASSERT_TRUE(Outer);
  const HighStmt *Inner = nullptr;
  walkStmts(Outer->Body, [&](const auto &Stmt) {
    if (Stmt.Kind == StmtKind::CxxTry)
      Inner = &Stmt;
  });
  ASSERT_TRUE(Inner);
  const HighStmt *Tries[] = {Inner, Outer};
  for (unsigned I = 0; I != 2; ++I) {
    const auto &Try = *Tries[I];
    ASSERT_EQ(Try.EHClauses.size(), EH.Cxx->TryBlocks[I].Handlers.size());
    ASSERT_EQ(Try.EHClauseBodies.size(), Try.EHClauses.size());
    for (unsigned J = 0; J != Try.EHClauses.size(); ++J) {
      const auto &Clause = Try.EHClauses[J];
      EXPECT_EQ(Clause.HandlerVA, EH.Cxx->TryBlocks[I].Handlers[J].HandlerVA);
      ASSERT_EQ(Clause.ContinuationVAs.size(), 1u);
      ASSERT_FALSE(Try.EHClauseBodies[J].empty());
      bool Resumes = false;
      walkStmts(Try.EHClauseBodies[J], [&](const auto &Stmt) {
        Resumes |= Stmt.Kind == StmtKind::Goto &&
                   Stmt.GotoTarget == Clause.ContinuationVAs[0];
        EXPECT_NE(Stmt.Kind, StmtKind::Return);
      });
      EXPECT_TRUE(Resumes);
    }
  }
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Med;
    if (Mutation == 0)
      Changed.RegistrationStates->CxxContinuationsComplete = false;
    if (Mutation == 1)
      Changed.RegistrationStates->CallFrameEffectsComplete = false;
    if (Mutation == 2)
      Changed.ExceptionMetadata->Cxx->IsSynchronous = false;
    if (Mutation == 3)
      for (auto &Block : Changed.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.RegistrationRoot ==
              MedOp::RegistrationRootKind::CallbackStackPointer)
            Op.RegistrationRoot = MedOp::RegistrationRootKind::None;
    const auto Rejected = MedToHighConverter().convert(Changed, Arch::X86);
    EXPECT_LT(Rejected.StructuredExceptionRegions, 2u);
    EXPECT_GT(Rejected.UnstructuredExceptionRegions, 0u);
  }
}

void checkNestedTrySource(const BinaryImage &Image,
                          const ExceptionFunction &Source) {
  auto Edit = [](BinaryImage &Changed, va_t Address, uint8_t Byte) {
    for (auto &Segment : Changed.Segments)
      if (Address >= Segment.VA && Address - Segment.VA < Segment.Data.size()) {
        Segment.Data[Address - Segment.VA] = Byte;
        return true;
      }
    return false;
  };
  ASSERT_TRUE(coff_loader::getCheckedX86CxxPersonalityABI(Image, Source));
  const auto *Thunk = Image.readVA(Source.PersonalityVA, 24);
  ASSERT_TRUE(Thunk);
  unsigned Prefix = 0;
  while (Prefix < 24 && Thunk[Prefix] != 0xb8)
    ++Prefix;
  ASSERT_LT(Prefix, 24u);
  // O0's exact argument reads may not modify another register, read a
  // different stack slot, or become a store to the dispatcher's arguments.
  for (unsigned I = 0; I < Prefix; ++I) {
    SCOPED_TRACE(I);
    auto Changed = Image;
    ASSERT_TRUE(Edit(Changed, Source.PersonalityVA + I, Thunk[I] ^ 1));
    EXPECT_FALSE(coff_loader::getCheckedX86CxxPersonalityABI(Changed, Source));
  }
  const auto Begin = Source.CodeRange.Begin;
  const auto *Code = Image.readVA(Begin, 20);
  ASSERT_TRUE(Code);
  const unsigned Copy = Code[6] == 0x83 ? 9 : 12;
  const unsigned Saved = Code[Copy + 1] == 0xe0 ? 5 : 3;
  for (unsigned I = 0; I < Saved; ++I) {
    SCOPED_TRACE(I);
    auto Changed = Image;
    ASSERT_TRUE(Edit(Changed, Begin + Copy + I, Code[Copy + I] ^ 1));
    Changed.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Changed);
    const auto *Graph = Changed.ExceptionMetadata.findFunction(Begin);
    EXPECT_TRUE(!Graph || !Graph->Registration ||
                !Graph->Registration->RegistrationOffset);
  }
}

void checkNestedTryEdits(const llvm::Function &Parent,
                         const ExceptionFunction &Source,
                         const BinaryImage &Image, bool Secondary) {
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  for (unsigned Mutation = 0; Mutation != (Secondary ? 7u : 5u); ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CatchSwitchInst *Inner = nullptr, *Outer = nullptr;
    std::vector<llvm::CatchReturnInst *> Returns;
    std::vector<llvm::InvokeInst *> Invokes;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Switch = llvm::dyn_cast<llvm::CatchSwitchInst>(&I))
          (Switch->getNumHandlers() == 1 ? Inner : Outer) = Switch;
        if (auto *Return = llvm::dyn_cast<llvm::CatchReturnInst>(&I))
          Returns.push_back(Return);
        if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&I))
          Invokes.push_back(Invoke);
      }
    ASSERT_TRUE(Inner && Outer);
    ASSERT_EQ(Returns.size(), 3u);
    ASSERT_EQ(Invokes.size(), Secondary ? 4u : 3u);
    llvm::InvokeInst *NestedCall = nullptr;
    for (auto *Invoke : Invokes)
      if (Invoke->getOperandBundle(llvm::LLVMContext::OB_funclet))
        NestedCall = Invoke;
    ASSERT_EQ(bool(NestedCall), Secondary);
    if (NestedCall) {
      EXPECT_EQ(NestedCall->getUnwindDest(), Outer->getParent());
      EXPECT_EQ(NestedCall->getOperandBundle(llvm::LLVMContext::OB_funclet)
                    ->Inputs[0],
                &*(*Inner->handler_begin())->getFirstNonPHIIt());
    }
    if (Mutation == 0) {
      if (NestedCall) {
        llvm::changeToCall(NestedCall);
        NestedCall = nullptr;
      }
      auto *Replacement =
          llvm::CatchSwitchInst::Create(Inner->getParentPad(), nullptr, 1,
                                        "changed.search", Inner->getIterator());
      Replacement->addHandler(*Inner->handler_begin());
      Replacement->copyMetadata(*Inner);
      Inner->replaceAllUsesWith(Replacement);
      Inner->eraseFromParent();
    } else if (Mutation == 1) {
      Invokes[0]->setUnwindDest(Outer->getParent());
    } else if (Mutation == 2) {
      auto *First = Outer->getSuccessor(0);
      Outer->setSuccessor(0, Outer->getSuccessor(1));
      Outer->setSuccessor(1, First);
    } else if (Mutation == 3) {
      auto From = (*Inner->handler_begin())->getFirstNonPHIIt();
      auto To = (*Outer->handler_begin())->getFirstNonPHIIt();
      From->setMetadata(
          llvm::mc_rewrite::RewriteWinEHSemanticAttachment,
          To->getMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment));
    } else if (Mutation == 4) {
      Returns[0]->setSuccessor(Returns[2]->getSuccessor());
    } else if (Mutation == 5) {
      // A plain call is verifier-clean inside this catch but omits the
      // source's secondary search. It must still fail semantic admission.
      llvm::changeToCall(NestedCall);
    } else {
      auto *Replacement = llvm::CallBase::removeOperandBundle(
          NestedCall, llvm::LLVMContext::OB_funclet, NestedCall->getIterator());
      Replacement->copyMetadata(*NestedCall);
      NestedCall->replaceAllUsesWith(Replacement);
      NestedCall->eraseFromParent();
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationCxxIR(*Function, Source, Image);
    EXPECT_TRUE(bool(Error));
    if (Error)
      llvm::consumeError(std::move(Error));
  }

  auto Module = llvm::CloneModule(*Parent.getParent());
  auto *Function = Module->getFunction(Parent.getName());
  auto Resolve = [&](llvm::StringRef Symbol,
                     uint32_t) -> std::optional<uint64_t> {
    if (auto Address = parseNdDataSymbol(Symbol))
      return *Address;
    if (auto Address = parseNdCodePtrSymbol(Symbol))
      return *Address;
    for (const auto &Candidate : *Module) {
      llvm::SmallString<64> Name;
      llvm::Mangler().getNameWithPrefix(Name, &Candidate, false);
      if (Name != Symbol)
        continue;
      auto Address = rewrite_source::getOriginalVA(Candidate);
      if (Address)
        return *Address;
      llvm::consumeError(Address.takeError());
      return std::nullopt;
    }
    return std::nullopt;
  };
  va_t Base = Image.Base;
  for (const auto &Segment : Image.Segments)
    Base = std::max(Base, Segment.VA + Segment.Size);
  auto Compiled =
      compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                           llvm::alignTo(Base, 0x1000), Resolve, Image.Base);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.Unresolved.empty());
  auto Check = [&](const CompiledImage &Output) {
    return getCheckedCOFFRegistrationCxxTableReceipt(*Function, Source, Image,
                                                     Output);
  };
  auto Valid = Check(Compiled);
  ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  size_t Inner = SIZE_MAX, Outer = SIZE_MAX;
  for (size_t I = 0; I < Compiled.WinEHSemanticRecords.size(); ++I) {
    const auto &Token = Compiled.WinEHSemanticRecords[I].Token;
    if (Token.Kind == llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch &&
        Token.Clause == 0)
      (Token.Region == 0 ? Inner : Outer) = I;
  }
  ASSERT_NE(Inner, SIZE_MAX);
  ASSERT_NE(Outer, SIZE_MAX);
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Compiled;
    auto &Row = Changed.WinEHSemanticRecords[Inner];
    const auto &Other = Changed.WinEHSemanticRecords[Outer];
    auto Word = [&](va_t VA, uint32_t Value) {
      llvm::support::endian::write32le(
          Changed.Bytes.data() + VA - Changed.BaseVA, Value);
    };
    switch (Mutation) {
    case 0:
      Row.ContainerVA = Other.ContainerVA;
      break;
    case 1:
      Row.GeneratedState = Other.GeneratedState;
      break;
    case 2:
      Row.X86CxxLayout->Tables[3] = Other.X86CxxLayout->Tables[3];
      break;
    case 3:
      Word(Row.ContainerVA, 0);
      break;
    case 4:
      Word(Other.ContainerVA + 4, 0);
      break;
    case 5:
      Word(Row.ContainerVA + 8, 3);
      break;
    case 6:
      Word(Row.X86CxxLayout->Tables[1].BeginVA + 8, UINT32_MAX);
      break;
    case 7:
      Changed.WinEHSemanticRecords.erase(Changed.WinEHSemanticRecords.begin() +
                                         Inner);
      break;
    }
    auto Receipt = Check(Changed);
    EXPECT_FALSE(bool(Receipt));
    if (!Receipt)
      llvm::consumeError(Receipt.takeError());
  }
#endif
}
} // namespace neverd::registration_test
