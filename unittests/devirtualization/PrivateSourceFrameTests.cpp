//===- PrivateSourceFrameTests.cpp - Source frame certificates ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/pipeline/NativeSourcePreservation.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/NativeSourceHints.h"

using namespace neverd;

namespace {
NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar scalar(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
void append(LowBlock &Block, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Block.StartAddr + Block.Ops.size();
  for (NdVar Input : Inputs)
    Op.addInput(Input);
  Block.Ops.push_back(std::move(Op));
}
LowFunc frameFunction() {
  LowFunc Function;
  Function.Entry = 0x1000;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  append(Block, NdOp::INT_SUB, r(x86reg::RSP), {r(x86reg::RSP), scalar(64)});
  append(Block, NdOp::COPY, r(x86reg::R11), {r(x86reg::RSP)});
  Function.Blocks.push_back(std::move(Block));
  return Function;
}
void finish(LowBlock &Block) {
  append(Block, NdOp::INT_ADD, r(x86reg::RSP), {r(x86reg::RSP), scalar(64)});
  append(Block, NdOp::RETURN, {}, {r(x86reg::RAX)});
}
void store(LowBlock &Block, NdVar Address, NdVar Value) {
  append(Block, NdOp::STORE, {}, {Address, Value});
}
void load(LowBlock &Block, NdVar Output, NdVar Address) {
  append(Block, NdOp::LOAD, Output, {Address});
}
bool certified(const LowFunc &Function, bool Disjoint = false) {
  return certifiesPrivateNativeSourceFrame(Function, Arch::X64, Disjoint);
}
} // namespace

TEST(PrivateSourceFrame, InitializedPrivateContextReportsFullAllocation) {
  auto Function = frameFunction();
  auto &Block = Function.Blocks.front();
  store(Block, r(x86reg::R11), r(x86reg::RDI));
  load(Block, r(x86reg::RAX), r(x86reg::R11));
  finish(Block);
  int64_t Required = 0;
  EXPECT_TRUE(
      certifiesPrivateNativeSourceFrame(Function, Arch::X64, false, &Required));
  EXPECT_EQ(Required, 64);
  EXPECT_TRUE(restoresNativeSourceState(Function, Arch::X64, {}));
}

TEST(PrivateSourceFrame, RejectsReturnedFrameValueWithoutChangingLegacyProof) {
  auto Function = frameFunction();
  auto &Block = Function.Blocks.front();
  append(Block, NdOp::COPY, r(x86reg::RAX), {r(x86reg::R11)});
  finish(Block);
  EXPECT_FALSE(certified(Function));
  EXPECT_TRUE(restoresNativeSourceState(Function, Arch::X64, {}));
}

TEST(PrivateSourceFrame, RejectsFrameDerivedBranchCondition) {
  auto Function = frameFunction();
  auto &Entry = Function.Blocks.front();
  append(Entry, NdOp::INT_EQUAL, r(x86reg::ZF, 1),
         {r(x86reg::R11), r(x86reg::RDI)});
  append(Entry, NdOp::COND_BR, {}, {NdVar::cst(0x1100, 8), r(x86reg::ZF, 1)});
  Entry.Succs = {1, 2};
  for (int Id : {1, 2}) {
    LowBlock Exit;
    Exit.Id = Id;
    Exit.StartAddr = 0x1000 + Id * 0x100;
    Exit.Preds = {0};
    append(Exit, NdOp::COPY, r(x86reg::RAX), {scalar(Id)});
    finish(Exit);
    Function.Blocks.push_back(std::move(Exit));
  }
  EXPECT_FALSE(certified(Function, true));
  EXPECT_TRUE(restoresNativeSourceState(Function, Arch::X64, {}));
}

TEST(PrivateSourceFrame, RejectsStoredFrameValuesEvenWithDisjointMemory) {
  for (bool Private : {false, true}) {
    auto Function = frameFunction();
    auto &Block = Function.Blocks.front();
    store(Block, r(Private ? x86reg::R11 : x86reg::RDI), r(x86reg::RSP));
    append(Block, NdOp::COPY, r(x86reg::RAX), {scalar(0)});
    finish(Block);
    EXPECT_FALSE(certified(Function, true));
  }
}

TEST(PrivateSourceFrame, RejectsUninitializedAndPartiallyInitializedReads) {
  for (unsigned Bytes : {0, 1, 4, 7}) {
    auto Function = frameFunction();
    auto &Block = Function.Blocks.front();
    if (Bytes)
      store(Block, r(x86reg::R11), r(x86reg::RDI, Bytes));
    load(Block, r(x86reg::RAX), r(x86reg::R11));
    finish(Block);
    EXPECT_FALSE(certified(Function));
    EXPECT_TRUE(restoresNativeSourceState(Function, Arch::X64, {}));
  }
}

TEST(PrivateSourceFrame, PartialWritesCoverExactReadBytes) {
  auto Function = frameFunction();
  auto &Block = Function.Blocks.front();
  for (unsigned Byte = 0; Byte < 8; ++Byte) {
    append(Block, NdOp::INT_ADD, r(x86reg::R10),
           {r(x86reg::R11), scalar(Byte)});
    store(Block, r(x86reg::R10), r(x86reg::RDI, 1));
  }
  load(Block, r(x86reg::RAX), r(x86reg::R11));
  finish(Block);
  EXPECT_TRUE(certified(Function));
}

TEST(PrivateSourceFrame, InitializedBytesMeetAcrossAllPredecessors) {
  for (bool BothPaths : {false, true}) {
    auto Function = frameFunction();
    auto &Entry = Function.Blocks.front();
    append(Entry, NdOp::COND_BR, {},
           {NdVar::cst(0x1100, 8), r(x86reg::RDI, 1)});
    Entry.Succs = {1, 2};
    for (int Id : {1, 2}) {
      LowBlock Path;
      Path.Id = Id;
      Path.StartAddr = 0x1000 + Id * 0x100;
      Path.Preds = {0};
      Path.Succs = {3};
      if (Id == 1 || BothPaths)
        store(Path, r(x86reg::R11), r(x86reg::RSI));
      append(Path, NdOp::BRANCH, {}, {NdVar::cst(0x1300, 8)});
      Function.Blocks.push_back(std::move(Path));
    }
    LowBlock Exit;
    Exit.Id = 3;
    Exit.StartAddr = 0x1300;
    Exit.Preds = {1, 2};
    load(Exit, r(x86reg::RAX), r(x86reg::R11));
    finish(Exit);
    Function.Blocks.push_back(std::move(Exit));
    EXPECT_EQ(certified(Function), BothPaths);
  }
}

TEST(PrivateSourceFrame, ExternalAccessRequiresExplicitDisjointContract) {
  for (bool Write : {false, true}) {
    auto Function = frameFunction();
    auto &Block = Function.Blocks.front();
    store(Block, r(x86reg::R11), r(x86reg::RSI));
    if (Write)
      store(Block, r(x86reg::RDI), r(x86reg::RSI));
    else
      load(Block, r(x86reg::R10), r(x86reg::RDI));
    load(Block, r(x86reg::RAX), r(x86reg::R11));
    finish(Block);
    EXPECT_FALSE(certified(Function));
    EXPECT_TRUE(certified(Function, true));
  }
}

TEST(PrivateSourceFrame, DisjointContractDoesNotAuthorizeInexactFrameAddress) {
  for (bool Write : {false, true}) {
    auto Function = frameFunction();
    auto &Block = Function.Blocks.front();
    append(Block, NdOp::INT_XOR, r(x86reg::R10),
           {r(x86reg::R11), r(x86reg::RDI)});
    if (Write) {
      store(Block, r(x86reg::R10), r(x86reg::RSI));
      append(Block, NdOp::COPY, r(x86reg::RAX), {scalar(0)});
    } else {
      load(Block, r(x86reg::RAX), r(x86reg::R10));
    }
    finish(Block);
    EXPECT_FALSE(certified(Function, true));
  }
}

TEST(PrivateSourceFrame, RejectsFrameDerivedDivisionEvenWhenResultIsDead) {
  auto Function = frameFunction();
  auto &Block = Function.Blocks.front();
  append(Block, NdOp::INT_DIV, r(x86reg::R10),
         {r(x86reg::RSI), r(x86reg::R11)});
  append(Block, NdOp::COPY, r(x86reg::RAX), {scalar(0)});
  finish(Block);
  EXPECT_FALSE(certified(Function));
}

TEST(PrivateSourceFrame, InitializedSpillStillRestoresPreservedRegister) {
  auto Function = frameFunction();
  auto &Block = Function.Blocks.front();
  store(Block, r(x86reg::R11), r(x86reg::RBX));
  append(Block, NdOp::COPY, r(x86reg::RBX), {scalar(1)});
  load(Block, r(x86reg::RBX), r(x86reg::R11));
  append(Block, NdOp::COPY, r(x86reg::RAX), {r(x86reg::RDI)});
  finish(Block);
  EXPECT_TRUE(certified(Function));
}

TEST(PrivateSourceFrame, RejectsCallsAndUnsupportedArchitecture) {
  auto Function = frameFunction();
  auto &Block = Function.Blocks.front();
  append(Block, NdOp::CALL, r(x86reg::RAX), {NdVar::cst(0x2000, 8)});
  finish(Block);
  EXPECT_FALSE(certified(Function, true));
  EXPECT_FALSE(certifiesPrivateNativeSourceFrame(Function, Arch::AArch64));
}
