//===- HighFlagReadTests.cpp - Unused flag snapshots ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A dead architectural snapshot is removable, while a live read keeps its
/// occurrence and ordering across state writes and calls.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"
#include "neverd/backend/llvm/X86/LLVMX86StackEffects.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/IntrinsicValueEffects.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <string>

namespace neverd {
void eliminateUnusedValues(std::vector<HighStmt> &Stmts);
bool containsNonMovableEffect(const ExprPtr &E);
} // namespace neverd

namespace {
using namespace neverd;

ExprPtr temporary(int Id, uint16_t Bytes = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = Bytes;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(Bytes, false));
}

ExprPtr flagRead(uint16_t Bytes = 8) {
  auto E = HighExpr::makeCall("__readeflags", 0, {});
  E->IntrinsicId = Intrinsic::Pushf;
  E->Type = NdType::makeInt(Bytes, false);
  return E;
}

HighStmt assign(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}

HighStmt returning(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

TEST(HighFlagReads, DeadSnapshotsAndTheirDeadConsumersDisappear) {
  for (uint16_t Bytes : {2, 4, 8}) {
    auto Captured = temporary(1, Bytes), Copy = temporary(2, Bytes);
    auto Read = flagRead(Bytes);
    std::vector<HighStmt> Body{
        assign(Captured, Read),
        assign(Copy, HighExpr::makeBinop(NdOp::INT_AND, Captured,
                                         HighExpr::makeConst(0x200, Bytes))),
        returning(HighExpr::makeConst(7, 4))};
    eliminateUnusedValues(Body);
    ASSERT_EQ(Body.size(), 1U);
    EXPECT_EQ(Body.front().Kind, StmtKind::Return);
  }
}

TEST(HighFlagReads, LiveSnapshotsRemainAtTheirOriginalStateBoundaries) {
  auto Before = temporary(1), After = temporary(2);
  auto First = flagRead(), Second = flagRead();
  auto Restore = HighExpr::makeCall("__writeeflags", 0, {Before});
  Restore->IntrinsicId = Intrinsic::Popf;
  Restore->Type = NdType::makeVoid();
  HighStmt Write;
  Write.Kind = StmtKind::Call;
  Write.CallExpr = Restore;
  std::vector<HighStmt> Body{
      assign(Before, First), Write, assign(After, Second),
      returning(HighExpr::makeBinop(NdOp::INT_XOR, Before, After))};
  eliminateUnusedValues(Body);
  ASSERT_EQ(Body.size(), 4U);
  EXPECT_EQ(Body[0].Val, First);
  EXPECT_EQ(Body[1].CallExpr, Restore);
  EXPECT_EQ(Body[2].Val, Second);
  EXPECT_TRUE(containsNonMovableEffect(First));
  EXPECT_TRUE(containsNonMovableEffect(Second));
}

TEST(HighFlagReads, NamesAndMalformedShapesDoNotAuthorizeDeletion) {
  for (unsigned Variant = 0; Variant != 11; ++Variant) {
    SCOPED_TRACE(Variant);
    auto Read = flagRead();
    switch (Variant) {
    case 0:
      Read->IntrinsicId = Intrinsic::None;
      break;
    case 1:
      Read->IntrinsicId = Intrinsic::Popf;
      break;
    case 2:
      Read->Operands.push_back(HighExpr::makeConst(0, 8));
      break;
    case 3:
      Read->IntrinsicOutputs.push_back(temporary(9)->Var);
      break;
    case 4:
      Read->Type = NdType::makeInt(1, false);
      break;
    case 5:
      Read->Type = NdType::makeInt(16, false);
      break;
    case 6:
      Read->Type = NdType::makePtr();
      break;
    case 7:
      Read->Type = NdType::makeVoid();
      break;
    case 8:
      Read->MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 9:
      Read->IndirectTarget = temporary(3);
      break;
    case 10:
      Read->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    }
    std::vector<HighStmt> Body{assign(temporary(1), Read),
                               returning(HighExpr::makeConst(7, 4))};
    eliminateUnusedValues(Body);
    ASSERT_EQ(Body.size(), 2U);
    EXPECT_EQ(Body.front().Val, Read);
  }
}

TEST(HighFlagReads, RemovingADeadSnapshotKeepsItsBranchEntry) {
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x401010;
  auto Read = assign(temporary(1), flagRead());
  Read.Addr = Jump.GotoTarget;
  std::vector<HighStmt> Body{Jump, Read, returning(HighExpr::makeConst(7, 4))};
  eliminateUnusedValues(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[1].Kind, StmtKind::Block);
  EXPECT_EQ(Body[1].Addr, Jump.GotoTarget);
  EXPECT_FALSE(Body[1].Val);
}

struct LLVMFlagFixture {
  llvm::LLVMContext Context;
  llvm::Module Module;
  llvm::IRBuilder<> Builder;
  llvm::Function *Function;
  llvm::CallInst *Read;

  LLVMFlagFixture(Arch Architecture, bool Live = false)
      : Module("flag_read", Context), Builder(Context) {
    const bool Wide = Architecture == Arch::X64;
    Module.setDataLayout(Wide ? "e-p:64:64" : "e-p:32:32");
    Module.setTargetTriple(
        llvm::Triple(Wide ? "x86_64-pc-windows-msvc" : "i386-pc-windows-msvc"));
    auto *Word = Builder.getIntNTy(Wide ? 64 : 32);
    auto *Type = llvm::FunctionType::get(Word, false);
    Function = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                      "flag_read", Module);
    Function->addFnAttr(llvm::Attribute::NoRedZone);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Assembly = llvm::InlineAsm::get(
        Type, Wide ? "pushfq\n\tpopq $0" : "pushfl\n\tpopl $0", "=r,~{memory}",
        true);
    Read = Builder.CreateCall(Assembly, {});
    markX86StackTemporary(*Read);
    Builder.CreateRet(Live ? static_cast<llvm::Value *>(Read)
                           : llvm::ConstantInt::get(Word, 7));
  }

  std::string source(Arch Architecture) {
    std::string C;
    llvm::raw_string_ostream Stream(C);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    Options.Format = BinaryFormat::COFF;
    EXPECT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
    return C;
  }

  std::string ir() {
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    Module.print(Stream, nullptr);
    return Text;
  }
};

} // namespace

TEST(LLVMFlagReads, DeadOwnedSnapshotsDisappearOnlyFromTheSourceClone) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    LLVMFlagFixture F(Architecture);
    F.Builder.SetInsertPoint(F.Function->getEntryBlock().getTerminator());
    F.Builder.CreateAdd(F.Read, llvm::ConstantInt::get(F.Read->getType(), 1));
    EXPECT_FALSE(F.Read->use_empty());
    const auto Before = F.ir();
    const auto Source = F.source(Architecture);
    EXPECT_EQ(Source.find("__readeflags"), std::string::npos) << Source;
    EXPECT_EQ(F.ir(), Before);
    EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::Pushf));
    EXPECT_FALSE(intrinsicHasOnlyValueEffects(Intrinsic::Pushf, 1, 8, 0));
  }
}

TEST(LLVMFlagReads, LiveSnapshotsKeepBothReadsAndTheirInterveningWrite) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    const bool Wide = Architecture == Arch::X64;
    LLVMFlagFixture F(Architecture, true);
    auto *Return = llvm::cast<llvm::ReturnInst>(
        F.Function->getEntryBlock().getTerminator());
    F.Builder.SetInsertPoint(Return);
    auto *Type = llvm::FunctionType::get(F.Builder.getVoidTy(),
                                         {F.Read->getType()}, false);
    auto *Assembly = llvm::InlineAsm::get(
        Type, Wide ? "pushq $0\n\tpopfq" : "pushl $0\n\tpopfl",
        "r,~{memory},~{dirflag},~{flags}", true);
    markX86StackTemporary(*F.Builder.CreateCall(Assembly, {F.Read}));
    auto *Second = F.Builder.CreateCall(F.Read->getFunctionType(),
                                        F.Read->getCalledOperand(), {});
    markX86StackTemporary(*Second);
    Return->setOperand(0, F.Builder.CreateXor(F.Read, Second));
    const auto Before = F.ir();
    const auto Source = F.source(Architecture);
    const auto FirstAt = Source.find("__readeflags()");
    const auto WriteAt = Source.find("__writeeflags(");
    const auto SecondAt = Source.find("__readeflags()", FirstAt + 1);
    ASSERT_NE(FirstAt, std::string::npos) << Source;
    ASSERT_NE(WriteAt, std::string::npos) << Source;
    ASSERT_NE(SecondAt, std::string::npos) << Source;
    EXPECT_LT(FirstAt, WriteAt);
    EXPECT_LT(WriteAt, SecondAt);
    EXPECT_EQ(F.ir(), Before);
  }
}

TEST(LLVMFlagReads, UnownedOrDifferentAssemblyRetainsItsEffectContract) {
  for (unsigned Variant = 0; Variant != 8; ++Variant) {
    SCOPED_TRACE(Variant);
    LLVMFlagFixture F(Arch::X64);
    std::string Text = "pushfq\n\tpopq $0", Constraints = "=r,~{memory}";
    switch (Variant) {
    case 0:
      F.Read->setMetadata(X86StackTemporaryMetadata, nullptr);
      break;
    case 1:
      F.Function->removeFnAttr(llvm::Attribute::NoRedZone);
      break;
    case 2:
      Text += "\n\tnop";
      break;
    case 3:
      Constraints += ",~{flags}";
      break;
    default:
      break;
    }
    F.Read->setCalledOperand(llvm::InlineAsm::get(
        F.Read->getFunctionType(), Text, Constraints, Variant != 4,
        Variant == 5,
        Variant == 6 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT,
        Variant == 7));
    EXPECT_FALSE(isDiscardableX86FlagRead(Arch::X64, *F.Read));
    if (Variant < 2)
      EXPECT_NE(F.source(Arch::X64).find("__readeflags"), std::string::npos);
  }
  LLVMFlagFixture F(Arch::X64);
  EXPECT_FALSE(isDiscardableX86FlagRead(Arch::X86, *F.Read));
  EXPECT_FALSE(isDiscardableX86FlagRead(Arch::AArch64, *F.Read));
}
