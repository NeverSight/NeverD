//===- WindowsRegistrationHighCallbackTests.cpp - Callback HighIR --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/high/X86RegistrationFrame.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86RegistrationCall.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>

using namespace neverd;

namespace {

TEST(WindowsRegistrationHighCallback, EntryIdentityIncludesTheInvocation) {
  HighExpr Entry;
  Entry.Kind = ExprKind::EntryRegister;
  Entry.Var.Kind = MedVar::Reg;
  Entry.Var.TheArch = Arch::X86;
  Entry.Var.RegOff = getTargetRegInfo(Arch::X86).StackPointer;
  Entry.Var.Size = 4;
  Entry.Type = NdType::makeInt(4, false);
  Entry.EntryFunctionVA = 0x401000;
  Entry.EntryVA = 0x401080;
  EXPECT_TRUE(Entry.structuralEq(Entry));
  auto Other = Entry;
  ++Other.EntryFunctionVA;
  EXPECT_FALSE(Entry.structuralEq(Other));
  Other = Entry;
  ++Other.EntryVA;
  EXPECT_FALSE(Entry.structuralEq(Other));
  Other = Entry;
  Other.Var.RegOff = getTargetRegInfo(Arch::X86).FramePointer;
  EXPECT_FALSE(Entry.structuralEq(Other));
  Other = Entry;
  Other.Kind = ExprKind::Var;
  EXPECT_FALSE(Entry.structuralEq(Other));
}

TEST(WindowsRegistrationHighCallback, InputPE32BindsCurrentCallbackRoots) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_PE32");
  if (!Path)
    GTEST_SKIP() << "requires the realigned PE32 callback fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image));
  const auto It =
      llvm::find_if(Image->ExceptionMetadata.Functions, [](const auto &EH) {
        return EH.Registration && EH.Registration->hasCxxCallbackStack();
      });
  ASSERT_NE(It, Image->ExceptionMetadata.Functions.end());
  ASSERT_TRUE(It->Cxx);
  ASSERT_EQ(It->Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(It->Cxx->TryBlocks[0].Handlers.size(), 1u);
  const va_t Catch = It->Cxx->TryBlocks[0].Handlers[0].HandlerVA;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low = CFGBuilder().build(*Image, Decode, It->CodeRange.Begin,
                                      "callback_parent");
  const auto Med =
      LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "high-callback"));
  const auto Region = registrationCallbackRegion(Med, Catch);
  ASSERT_TRUE(Region);
  ASSERT_FALSE(Region->Ranges.empty());
  unsigned CheckedCalls = 0;
  for (size_t B = 0; B < Med.Blocks.size(); ++B)
    for (size_t O = 0; O < Med.Blocks[B].Ops.size(); ++O) {
      const auto &Call = Med.Blocks[B].Ops[O];
      if (Call.Opcode != NdOp::CALL)
        continue;
      const auto ABI = registrationCallABI(Med, Med.Blocks[B], Call);
      ASSERT_TRUE(ABI);
      ++CheckedCalls;
      if (ABI->BorrowsECX) {
        auto Changed = Med;
        auto &Ops = Changed.Blocks[B].Ops;
        const auto ECX = getTargetRegInfo(Arch::X86).IntParamRegs.front();
        bool Replaced = false;
        for (size_t K = O; K != 0; --K) {
          auto &Def = Ops[K - 1];
          if (Def.Dead || Def.Output.Kind != MedVar::Reg ||
              Def.Output.RegOff != ECX || Def.Output.Size != 4)
            continue;
          Def.Opcode = NdOp::COPY;
          Def.NumInputs = 1;
          Def.Inputs[0] = MedVar::makeConst(0x12345678, 4);
          Replaced = true;
          break;
        }
        ASSERT_TRUE(Replaced);
        const auto ChangedHigh =
            MedToHighConverter().convert(Changed, Arch::X86);
        unsigned Found = 0;
        walkStmts(ChangedHigh.Body, [&](const HighStmt &S) {
          const auto E = S.CallExpr ? S.CallExpr : S.Val;
          if (!E || E->Kind != ExprKind::Call ||
              E->CallAddr != Call.Inputs[0].ConstVal)
            return;
          ++Found;
          ASSERT_EQ(E->Operands.size(), 1u);
          EXPECT_EQ(E->Operands[0]->Kind, ExprKind::Const);
          EXPECT_EQ(E->Operands[0]->ConstVal, 0x12345678u);
        });
        EXPECT_EQ(Found, 1u);
      }
      for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
        SCOPED_TRACE(Mutation);
        auto Changed = Med;
        auto &Block = Changed.Blocks[B];
        auto &Op = Block.Ops[O];
        auto &State = *Changed.RegistrationStates;
        auto Effect = llvm::find_if(State.CallFrameEffects, [&](const auto &E) {
          return E.Address == Op.Addr && E.OpSeq == Op.OriginSeq;
        });
        ASSERT_NE(Effect, State.CallFrameEffects.end());
        auto &Callee = State.CalleeContracts[Effect->CalleeIndex];
        if (Mutation == 0)
          State.CallFrameEffectsComplete = false;
        if (Mutation == 1)
          ++Op.Inputs[0].ConstVal;
        if (Mutation == 2)
          ++Op.OriginSeq;
        if (Mutation == 3)
          Effect->CalleeIndex = State.CalleeContracts.size();
        if (Mutation == 4)
          Effect->StackPopBytes = 4;
        if (Mutation == 5)
          Effect->EndAddress = Block.EndAddr + 1;
        if (Mutation == 6)
          ++Callee.Target;
        if (Mutation == 7)
          Callee.StackPopBytes = 4;
        if (Mutation == 8)
          Callee.DoesNotReturn = !Callee.DoesNotReturn;
        if (Mutation == 9)
          Op.DoesNotReturn = !Op.DoesNotReturn;
        if (Mutation == 10) {
          const auto Duplicate = *Effect;
          State.CallFrameEffects.push_back(Duplicate);
        }
        if (Mutation == 11) {
          const auto Duplicate = Block;
          Changed.Blocks.push_back(Duplicate);
        }
        EXPECT_FALSE(registrationCallABI(Changed, Changed.Blocks[B],
                                         Changed.Blocks[B].Ops[O]));
      }
    }
  EXPECT_EQ(CheckedCalls, Med.RegistrationStates->CallFrameEffects.size());
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Med;
    auto &State = *Changed.RegistrationStates;
    auto Fact = llvm::find_if(
        State.Blocks, [&](const auto &B) { return B.Range.Begin == Catch; });
    ASSERT_NE(Fact, State.Blocks.end());
    ASSERT_FALSE(State.CxxContinuations.empty());
    auto &Resume = State.CxxContinuations.front();
    auto ReturnBlock = llvm::find_if(Changed.Blocks, [&](const auto &B) {
      return B.StartAddr <= Resume.Address && Resume.Address < B.EndAddr;
    });
    ASSERT_NE(ReturnBlock, Changed.Blocks.end());
    auto Return = llvm::find_if(ReturnBlock->Ops, [&](const auto &Op) {
      return Op.Opcode == NdOp::RETURN && Op.Addr == Resume.Address;
    });
    ASSERT_NE(Return, ReturnBlock->Ops.end());
    if (Mutation == 0)
      Fact->Reached = false;
    if (Mutation == 1)
      Fact->CallbackOnly = false;
    if (Mutation == 2)
      Fact->Unknown = true;
    if (Mutation == 3)
      ++Fact->Range.End;
    if (Mutation == 4) {
      const auto Duplicate = *Fact;
      State.Blocks.push_back(Duplicate);
    }
    if (Mutation == 5)
      ++Return->Inputs[0].ConstVal;
    if (Mutation == 6)
      ++Return->OriginSeq;
    if (Mutation == 7)
      ++Resume.EndAddress;
    if (Mutation == 8)
      ++Resume.CatchIndex;
    if (Mutation == 9)
      ReturnBlock->Succs.push_back(Changed.Blocks.front().Id);
    EXPECT_FALSE(registrationCallbackRegion(Changed, Catch));
  }
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Med;
    auto Block = llvm::find_if(
        Changed.Blocks, [&](const auto &B) { return B.StartAddr == Catch; });
    ASSERT_NE(Block, Changed.Blocks.end());
    auto Root = llvm::find_if(Block->Ops, [](const auto &Op) {
      return Op.RegistrationRoot ==
             MedOp::RegistrationRootKind::CallbackStackPointer;
    });
    ASSERT_NE(Root, Block->Ops.end());
    if (Mutation == 0)
      Changed.RegistrationStates->Complete = false;
    if (Mutation == 1)
      Changed.RegistrationStates->CallbackStatesComplete = false;
    if (Mutation == 2)
      Changed.RegistrationStates->RegistrationLifetimeComplete = false;
    if (Mutation == 3)
      Changed.RegistrationStates->ChainOperationsComplete = false;
    if (Mutation == 4)
      Changed.RegistrationStates->CxxContinuationsComplete = false;
    if (Mutation == 5)
      ++Changed.Entry;
    if (Mutation == 6)
      Block->Preds.push_back(Changed.Blocks.front().Id);
    if (Mutation == 7)
      Block->ExceptionalPreds.clear();
    if (Mutation == 8)
      ++Root->Addr;
    if (Mutation == 9)
      Root->RegistrationStackOffset = -4;
    if (Mutation == 10)
      Root->Inputs[0].SSAVer = Root->Output.SSAVer;
    if (Mutation == 11)
      Root->Output.RegOff = getTargetRegInfo(Arch::X86).FramePointer;
    EXPECT_FALSE(isRegistrationCallbackStackRoot(Changed, *Root));
    EXPECT_EQ(lowerX86RegistrationRoot(Changed, *Root)->Kind, ExprKind::Undef);
    EXPECT_FALSE(registrationCallbackRegion(Changed, Catch));
    const auto High = MedToHighConverter().convert(Changed, Arch::X86);
    EXPECT_EQ(High.StructuredExceptionRegions, 0u);
  }

  const auto High = MedToHighConverter().convert(Med, Arch::X86);
  EXPECT_EQ(High.StructuredExceptionRegions, 1u);
  EXPECT_EQ(High.UnstructuredExceptionRegions, 0u);
  unsigned Captures = 0;
  unsigned Calls = 0;
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    forEachExpr(Stmt, [&](const ExprPtr &Root) {
      HighExprSet Seen;
      std::vector<ExprPtr> Work{Root};
      while (!Work.empty()) {
        const auto E = Work.back();
        Work.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (E->Kind == ExprKind::EntryRegister) {
          ++Captures;
          EXPECT_EQ(Stmt.Addr, Catch);
          EXPECT_EQ(Stmt.Kind, StmtKind::Assign);
          EXPECT_EQ(Stmt.Val, E);
          EXPECT_EQ(E->EntryFunctionVA, Med.Entry);
          EXPECT_EQ(E->EntryVA, Catch);
        }
        if (E->Kind == ExprKind::Call) {
          const auto Callee = llvm::find_if(
              Med.RegistrationStates->CalleeContracts,
              [&](const auto &C) { return C.Target == E->CallAddr; });
          if (Callee != Med.RegistrationStates->CalleeContracts.end()) {
            ++Calls;
            const bool Borrow =
                !Callee->ECXReads.empty() || !Callee->ECXWrites.empty();
            EXPECT_EQ(E->Operands.size(), Borrow ? 1u : 0u);
            if (Borrow && !E->Operands.empty())
              EXPECT_NE(E->Operands.front()->Kind, ExprKind::Undef);
          }
        }
        E->forEachChildExpr(
            [&](const ExprPtr &Child) { Work.push_back(Child); });
      }
    });
  });
  EXPECT_EQ(Captures, 1u);
  EXPECT_EQ(Calls, CheckedCalls);

  CEmitterOptions Options;
  Options.TheArch = Arch::X86;
  Options.Format = BinaryFormat::COFF;
  for (bool Structured : {false, true}) {
    Options.StructuredExceptionSyntax = Structured;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    EXPECT_NE(Text.find("__neverd_x86_callback_esp(0x"), std::string::npos);
    EXPECT_NE(Text.find("Each invocation has its own stack"),
              std::string::npos);
    if (!Structured) {
      EXPECT_NE(Text.find("Native x86 callback @"), std::string::npos);
      EXPECT_NE(Text.find("goto L_x86_eh_after_"), std::string::npos);
    }
  }
}

TEST(WindowsRegistrationHighCallback, InputPE32CollectsTheWholeCallbackCFG) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_PE32");
  if (!Path)
    GTEST_SKIP() << "requires the realigned PE32 callback fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image));
  const auto It =
      llvm::find_if(Image->ExceptionMetadata.Functions, [](const auto &EH) {
        return EH.Registration && EH.Registration->hasCxxCallbackStack();
      });
  ASSERT_NE(It, Image->ExceptionMetadata.Functions.end());
  const va_t Catch = It->Cxx->TryBlocks[0].Handlers[0].HandlerVA;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low = CFGBuilder().build(*Image, Decode, It->CodeRange.Begin,
                                      "callback_parent");
  auto Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
  const auto Before = registrationCallbackRegion(Med, Catch);
  ASSERT_TRUE(Before);
  const auto Index =
      llvm::find_if(Med.Blocks,
                    [&](const auto &B) { return B.StartAddr == Catch; }) -
      Med.Blocks.begin();
  ASSERT_LT(size_t(Index), Med.Blocks.size());
  auto &Block = Med.Blocks[Index];
  const auto Split =
      llvm::find_if(Block.Ops, [&](const auto &Op) { return Op.Addr > Catch; });
  ASSERT_NE(Split, Block.Ops.end());
  const va_t Boundary = Split->Addr;
  auto Tail = Block;
  for (const auto &B : Med.Blocks)
    Tail.Id = std::max(Tail.Id, B.Id + 1);
  Tail.StartAddr = Boundary;
  Tail.Preds = {Block.Id};
  Tail.ExceptionalPreds.clear();
  Tail.Phis.clear();
  Tail.Ops.assign(Split, Block.Ops.end());
  Block.Ops.erase(Split, Block.Ops.end());
  Block.EndAddr = Boundary;
  Block.Succs = {Tail.Id};
  for (auto &B : Med.Blocks)
    if (llvm::is_contained(Tail.Succs, B.Id))
      for (auto &Pred : B.Preds)
        if (Pred == Block.Id)
          Pred = Tail.Id;
  auto &Facts = Med.RegistrationStates->Blocks;
  const auto Fact = llvm::find_if(
      Facts, [&](const auto &B) { return B.Range.Begin == Catch; });
  ASSERT_NE(Fact, Facts.end());
  auto TailFact = *Fact;
  TailFact.BlockId = Tail.Id;
  TailFact.Range.Begin = Boundary;
  Fact->Range.End = Boundary;
  Facts.push_back(TailFact);
  Med.Blocks.push_back(std::move(Tail));
  const auto After = registrationCallbackRegion(Med, Catch);
  ASSERT_TRUE(After);
  EXPECT_EQ(After->Ranges.size(), Before->Ranges.size() + 1);

  // A second ordinary entry invalidates the whole body, including the part
  // whose runtime root remains well formed.
  Med.Blocks.back().Preds.push_back(Med.Blocks.front().Id);
  EXPECT_FALSE(registrationCallbackRegion(Med, Catch));
}

} // namespace
