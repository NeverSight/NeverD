#include "../../../lib/ir/low/SourceBooleanResultProof.h"
#include "gtest/gtest.h"

#include "neverd/lift/AArch64Regs.h"

using namespace neverd;

namespace {
LowOp operation(NdOp Opcode, NdVar Output,
                std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Seq = 0;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowOp copy(uint64_t Target, uint64_t Source, unsigned Width = 8) {
  return operation(NdOp::COPY, NdVar::reg(Target, Width),
                   {NdVar::reg(Source, Width)});
}

LowOp constant(uint64_t Target, uint64_t Value, unsigned Width = 8) {
  return operation(NdOp::COPY, NdVar::reg(Target, Width),
                   {NdVar::cst(Value, Width)});
}

LowOp mask(uint64_t Target = a64reg::X0, uint64_t Source = a64reg::X8,
           uint64_t Value = 1, unsigned Width = 8) {
  return operation(NdOp::INT_AND, NdVar::reg(Target, Width),
                   {NdVar::reg(Source, Width), NdVar::cst(Value, Width)});
}

LowOp call(va_t Target = 0x2000) {
  return operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                   {NdVar::cst(Target, 8)});
}

LowOp ret() {
  return operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)});
}

LowBlock block(int Id, va_t Start, std::vector<LowOp> Ops,
               std::vector<int> Preds = {}, std::vector<int> Succs = {}) {
  LowBlock B;
  B.Id = Id;
  B.StartAddr = Start;
  B.EndAddr = Start + Ops.size() * 4;
  B.Preds = std::move(Preds);
  B.Succs = std::move(Succs);
  for (size_t I = 0; I < Ops.size(); ++I)
    Ops[I].Addr = Start + I * 4;
  B.Ops = std::move(Ops);
  return B;
}

struct Fixture {
  LowFunc Low;
  SourceFunctionTypeHint Entry;
  SourceFunctionTypeHint Compare;
  SourceFunctionTypeHint Release;
  SourceCallOccurrenceKey Selected;
  std::map<SourceCallOccurrenceKey, SourceBooleanOtherCallContract> Calls;
  SourceBooleanResultContract Raw;

  Fixture() {
    Low.Entry = 0x1000;
    Raw.Architecture = Arch::AArch64;
    Raw.ResultRegister = a64reg::X0;
    Raw.ResultCarrierBytes = 8;
    Raw.DefinedResultBits = 1;
    for (auto *Signature : {&Entry, &Compare, &Release}) {
      Signature->Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Signature->ReturnType = NdType::makeInt(8, false);
      std::string Error;
      EXPECT_TRUE(
          assignDarwinScalarSourceABI(*Signature, Arch::AArch64, Error));
    }
    Release.ReturnType = NdType::makeVoid();
    Release.Parameters = {{"object", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Release, Arch::AArch64, Error));
    linear({call(), copy(a64reg::X8, a64reg::X0), mask(), ret()});
  }

  void linear(std::vector<LowOp> Ops) {
    // Calls after normalization may leave incidental LR bytes changed. Model
    // the caller's actual reload before returning; no result ABI restores LR.
    if (!Ops.empty() && Ops.back().Opcode == NdOp::RETURN &&
        std::count_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::CALL;
        }) > 1)
      Ops.insert(Ops.end() - 1,
                 operation(NdOp::LOAD, NdVar::reg(a64reg::X30, 8),
                           {NdVar::reg(a64reg::SP, 8)}));
    Low.Blocks = {block(0, Low.Entry, std::move(Ops))};
    bindCalls();
  }

  void bindCalls() {
    Calls.clear();
    for (const auto &B : Low.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL) {
          const auto Key = sourceCallOccurrenceKey(Op);
          EXPECT_TRUE(Key);
          if (!Key)
            continue;
          if (Op.Inputs[0].Offset == 0x2000) {
            Selected = *Key;
            continue;
          }
          Calls.emplace(*Key, SourceBooleanOtherCallContract{&Release});
        }
  }

  std::optional<SourceBooleanResultCertificate> prove() const {
    return proveSourceBooleanResultNormalization(Low, Arch::AArch64, Selected,
                                                 Raw, Calls, Entry);
  }

  void diamond(bool BothMasks = true) {
    Low.Blocks = {block(0, 0x1000,
                        {call(), operation(NdOp::COND_BR, {},
                                           {NdVar::cst(0x1010, 8),
                                            NdVar::reg(a64reg::X2, 1)})},
                        {}, {1, 2}),
                  block(1, 0x1008,
                        {mask(a64reg::X0, a64reg::X0),
                         operation(NdOp::BRANCH, {}, {NdVar::cst(0x1018, 8)})},
                        {0}, {3}),
                  block(2, 0x1010,
                        {BothMasks ? mask(a64reg::X0, a64reg::X0)
                                   : operation(NdOp::NOP, {}, {}),
                         operation(NdOp::BRANCH, {}, {NdVar::cst(0x1018, 8)})},
                        {0}, {3}),
                  block(3, 0x1018, {ret()}, {1, 2})};
    bindCalls();
  }
};
} // namespace

TEST(NativeBooleanResultProof, ExactOccurrenceIdentityComesFromLowIR) {
  auto Direct = call();
  Direct.Addr = 0x1234;
  Direct.Seq = 3;
  const auto Key = sourceCallOccurrenceKey(Direct);
  ASSERT_TRUE(Key);
  EXPECT_EQ(Key->Instruction, 0x1234U);
  EXPECT_EQ(Key->Sequence, 3);
  EXPECT_EQ(Key->Opcode, NdOp::CALL);
  EXPECT_EQ(Key->StaticTarget, 0x2000U);
  std::set<SourceCallOccurrenceKey> Occurrences{*Key};
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Changed = Direct;
    if (Mutation == 0)
      ++Changed.Addr;
    else if (Mutation == 1)
      ++Changed.Seq;
    else if (Mutation == 2)
      Changed.Opcode = NdOp::INDIR_CALL;
    else
      ++Changed.Inputs[0].Offset;
    const auto Other = sourceCallOccurrenceKey(Changed);
    ASSERT_TRUE(Other);
    EXPECT_TRUE(Occurrences.insert(*Other).second);
  }
  auto Indirect = Direct;
  Indirect.Opcode = NdOp::INDIR_CALL;
  Indirect.Inputs[0] = NdVar::reg(a64reg::X3, 8);
  const auto Dynamic = sourceCallOccurrenceKey(Indirect);
  ASSERT_TRUE(Dynamic);
  EXPECT_FALSE(Dynamic->StaticTarget);
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto Changed = Direct;
    switch (Mutation) {
    case 0:
      Changed.Opcode = NdOp::BRANCH;
      break;
    case 1:
      Changed.Seq = -1;
      break;
    case 2:
      Changed.NumInputs = 0;
      break;
    case 3:
      Changed.addInput(NdVar::reg(a64reg::X3, 8));
      break;
    case 4:
      Changed.Inputs[0].Size = 4;
      break;
    case 5:
      Changed.Inputs[0] = NdVar::reg(a64reg::X3, 8);
      break;
    }
    EXPECT_FALSE(sourceCallOccurrenceKey(Changed));
  }
}

TEST(NativeBooleanResultProof, CompleteMasksKeepOnlyTheOriginalLowBit) {
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    SCOPED_TRACE(Variant);
    Fixture F;
    if (Variant == 1)
      F.linear({call(), copy(a64reg::X8, a64reg::X0, 4),
                mask(a64reg::X0, a64reg::X8, 1, 4), ret()});
    if (Variant == 2)
      F.linear({call(),
                operation(NdOp::INT_XOR, NdVar::reg(a64reg::X8, 4),
                          {NdVar::reg(a64reg::X0, 4), NdVar::cst(1, 4)}),
                mask(a64reg::X8, a64reg::X8, 1, 4),
                operation(NdOp::STORE, {},
                          {NdVar::cst(0x3000, 8), NdVar::reg(a64reg::X8, 1)}),
                constant(a64reg::X0, 0x4000), ret()});
    if (Variant == 3)
      F.linear(
          {call(),
           operation(NdOp::INT_OR, NdVar::reg(a64reg::X0, 8),
                     {NdVar::reg(a64reg::X0, 8), NdVar::cst(UINT64_MAX, 8)}),
           ret()});
    const auto Proof = F.prove();
    ASSERT_TRUE(Proof);
    EXPECT_EQ(Proof->Function, &F.Low);
    EXPECT_EQ(Proof->Site.Instruction, F.Selected.Instruction);
    EXPECT_EQ(F.Low.Blocks[0].Ops.front().Opcode, NdOp::CALL);
  }
}

TEST(NativeBooleanResultProof, RejectsObservableUpperBitsAndPartialWrites) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F;
    auto Ops = F.Low.Blocks[0].Ops;
    if (Mutation == 0)
      Ops[2] = mask(a64reg::X0, a64reg::X8, 3);
    if (Mutation == 1)
      Ops[2] = copy(a64reg::X0, a64reg::X8);
    if (Mutation == 2)
      Ops.insert(Ops.begin() + 2,
                 operation(NdOp::STORE, {},
                           {NdVar::cst(0x3000, 8), NdVar::reg(a64reg::X8, 8)}));
    if (Mutation == 3)
      Ops.insert(Ops.begin() + 2,
                 operation(NdOp::LOAD, NdVar::reg(a64reg::X9, 8),
                           {NdVar::reg(a64reg::X8, 8)}));
    if (Mutation == 4)
      Ops.insert(Ops.begin() + 2, call(0x2100));
    if (Mutation >= 5 && Mutation <= 8) {
      const uint64_t Registers[] = {a64reg::X19, a64reg::X29, a64reg::X30,
                                    a64reg::SP};
      Ops.insert(Ops.begin() + 2, copy(Registers[Mutation - 5], a64reg::X8));
    }
    if (Mutation == 9)
      Ops[2] = constant(a64reg::X0, 0, 1);
    if (Mutation == 10)
      Ops[2] = operation(NdOp::SUBBYTES, NdVar::reg(a64reg::X0, 1),
                         {NdVar::reg(a64reg::X8, 8), NdVar::cst(1, 4)});
    if (Mutation == 11)
      // Private spills are outside this proof; no aliasing exception hides
      // an observation of the discarded bits.
      Ops.insert(Ops.begin() + 2, operation(NdOp::STORE, {},
                                            {NdVar::reg(a64reg::SP, 8),
                                             NdVar::reg(a64reg::X8, 8)}));
    if (Mutation == 12) {
      F.Entry.ReturnType = NdType::makeInt(1, false);
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(F.Entry, Arch::AArch64, Error));
      // Zeroing just byte zero does not establish Darwin's complete W0
      // extension. Bits 31:8 still differ at the declared return boundary.
      Ops[2] = constant(a64reg::X0, 0, 1);
    }
    F.linear(std::move(Ops));
    EXPECT_FALSE(F.prove());
  }
}

TEST(NativeBooleanResultProof, OpaqueCallsRequireEveryPhysicalByteToAgree) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const std::vector<uint64_t> Registers = {
      a64reg::X1, a64reg::X8,         a64reg::X19,  a64reg::X30,
      a64reg::SP, TRI.VecRegBase + 8, a64reg::NFLAG};
  for (const auto Register : Registers) {
    for (bool Clear : {false, true}) {
      SCOPED_TRACE(Register);
      SCOPED_TRACE(Clear);
      Fixture F;
      const unsigned Width = Register == a64reg::NFLAG ? 1 : 8;
      F.linear(
          {call(), copy(Register, a64reg::X0, Width),
           mask(a64reg::X0, a64reg::X0),
           Clear ? constant(Register, 0, Width) : operation(NdOp::NOP, {}, {}),
           call(0x2100), constant(Register, 0, Width), constant(a64reg::X0, 0),
           ret()});
      F.Calls.begin()->second = {nullptr, false, true};
      EXPECT_EQ(bool(F.prove()), Clear);
    }
  }
  Fixture F;
  F.linear({call(0x2100), call(), mask(a64reg::X0, a64reg::X0), ret()});
  F.Calls.begin()->second = {nullptr, false, true};
  ASSERT_TRUE(F.prove());
  F.Calls.begin()->second.Signature = &F.Release;
  EXPECT_FALSE(F.prove());
  F.Calls.begin()->second.Signature = nullptr;
  F.Calls.begin()->second.DoesNotReturn = true;
  EXPECT_FALSE(F.prove());
  F.Calls.begin()->second = {};
  EXPECT_FALSE(F.prove());
}

TEST(NativeBooleanResultProof, OpaqueCallsRetainSameInstructionTemporaries) {
  Fixture F;
  F.linear(
      {call(),
       operation(NdOp::COPY, NdVar::tmp(0, 8), {NdVar::reg(a64reg::X0, 8)}),
       mask(a64reg::X0, a64reg::X0), call(0x2100),
       operation(NdOp::STORE, {}, {NdVar::cst(0x3000, 8), NdVar::tmp(0, 8)}),
       constant(a64reg::X0, 0), ret()});
  auto &B = F.Low.Blocks[0];
  // One machine instruction may expand into several operations. Its temporary
  // is private to LowIR, but remains observable after the opaque call.
  for (unsigned I = 1; I <= 4; ++I) {
    B.Ops[I].Addr = 0x1004;
    B.Ops[I].Seq = I - 1;
  }
  for (unsigned I = 5; I < B.Ops.size(); ++I)
    B.Ops[I].Addr -= 12;
  B.EndAddr -= 12;
  F.bindCalls();
  F.Calls.begin()->second = {nullptr, false, true};
  EXPECT_FALSE(F.prove());
  B.Ops[4].Inputs[1] = NdVar::reg(a64reg::X0, 8);
  EXPECT_TRUE(F.prove());
}

TEST(NativeBooleanResultProof, OpaquePrefixMustRemainIdenticalOnEveryBackedge) {
  for (bool Clear : {false, true}) {
    Fixture F;
    F.Low.Blocks = {
        block(0, 0x1000, {operation(NdOp::BRANCH, {}, {NdVar::cst(0x1004, 8)})},
              {}, {1}),
        block(1, 0x1004,
              {call(0x2100), call(), copy(a64reg::X19, a64reg::X0),
               mask(a64reg::X0, a64reg::X0),
               Clear ? constant(a64reg::X19, 0) : operation(NdOp::NOP, {}, {}),
               operation(NdOp::COND_BR, {},
                         {NdVar::cst(0x1004, 8), NdVar::reg(a64reg::X20, 1)})},
              {0, 1}, {1, 2}),
        block(2, 0x101c,
              {constant(a64reg::X19, 0), constant(a64reg::X30, 0), ret()},
              {1})};
    F.bindCalls();
    F.Calls.begin()->second = {nullptr, false, true};
    EXPECT_EQ(bool(F.prove()), Clear);
  }
}

TEST(NativeBooleanResultProof, ExactCallsKeepThePreservedRegisterDifference) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F;
    F.linear({call(), copy(a64reg::X19, a64reg::X0),
              constant(a64reg::X0, 0x3000), call(0x2100),
              mask(a64reg::X0, a64reg::X19, 1, 4),
              operation(NdOp::LOAD, NdVar::reg(a64reg::X19, 8),
                        {NdVar::reg(a64reg::SP, 8)}),
              ret()});
    if (Mutation == 1) {
      F.Low.Blocks[0].Ops[4] = copy(a64reg::X0, a64reg::X19);
      F.Low.Blocks[0].Ops[4].Addr = 0x1010;
    }
    if (Mutation == 2) {
      F.Low.Blocks[0].Ops[5] = operation(NdOp::NOP, {}, {});
      F.Low.Blocks[0].Ops[5].Addr = 0x1014;
    }
    if (Mutation == 3)
      F.Release.Parameters.push_back(
          {"context",
           NdType::makeInt(8),
           {SourceABICarrierKind::IntegerRegister, a64reg::X19, 0, 8}});
    if (Mutation == 4)
      F.Calls.emplace(F.Selected, SourceBooleanOtherCallContract{&F.Compare});
    if (Mutation == 5)
      ++F.Selected.Sequence;
    if (Mutation == 6)
      F.Low.Blocks[0].Ops[3].Inputs[0].Offset += 4;
    if (Mutation == 7)
      F.Raw.DefinedResultBits = 8;
    if (Mutation == 8)
      F.Calls.begin()->second.DoesNotReturn = true;
    if (Mutation == 9)
      F.Low.Blocks[0].Ops[0].Output.Size = 4;
    if (Mutation == 10)
      F.Low.Blocks[0].Ops[3].Output = NdVar::reg(a64reg::X19, 8);
    EXPECT_EQ(bool(F.prove()), Mutation == 0);
  }
}

TEST(NativeBooleanResultProof, CallsPreserveOnlyTheDeclaredVectorPrefix) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto V8 = TRI.VecRegBase + 8 * TRI.VecRegStride;
  for (bool LowPrefix : {false, true}) {
    Fixture F;
    const auto Zero = NdVar::cst(0, 8);
    const auto Result = NdVar::reg(a64reg::X0, 8);
    F.linear({call(),
              operation(NdOp::CONCAT, NdVar::reg(V8, 16),
                        {LowPrefix ? Zero : Result, LowPrefix ? Result : Zero}),
              constant(a64reg::X0, 0x3000), call(0x2100),
              constant(a64reg::X0, 0), ret()});
    EXPECT_EQ(bool(F.prove()), !LowPrefix);
  }
}

TEST(NativeBooleanResultProof, CallsCannotEraseUnspecifiedResultBytes) {
  // Even a zero-argument void callee may leave X8 untouched. Its permission
  // to clobber X8 cannot hide a later store of the original differing word.
  Fixture F;
  F.Release.Parameters.clear();
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(F.Release, Arch::AArch64, Error));
  F.linear({call(), copy(a64reg::X8, a64reg::X0), call(0x2100),
            operation(NdOp::STORE, {},
                      {NdVar::cst(0x3000, 8), NdVar::reg(a64reg::X8, 8)}),
            constant(a64reg::X0, 0), ret()});
  EXPECT_FALSE(F.prove());

  for (unsigned Width : {1, 2, 4, 8}) {
    SCOPED_TRACE(Width);
    F.Release.ReturnType = NdType::makeInt(Width, false);
    ASSERT_TRUE(assignDarwinScalarSourceABI(F.Release, Arch::AArch64, Error));
    F.linear({call(), call(0x2100),
              operation(NdOp::STORE, {},
                        {NdVar::cst(0x3000, 8), NdVar::reg(a64reg::X0, 8)}),
              constant(a64reg::X0, 0), ret()});
    EXPECT_EQ(bool(F.prove()), Width == 8);
    // Darwin narrow integer results establish the low W0 extension, never
    // the unspecified high word. A low-word-only observation is independent.
    F.Low.Blocks[0].Ops[2].Inputs[1].Size = 4;
    EXPECT_TRUE(F.prove());
  }

  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  F.Release.ReturnType = NdType::makeFloat(8);
  ASSERT_TRUE(assignDarwinScalarSourceABI(F.Release, Arch::AArch64, Error));
  for (bool LowPrefix : {false, true}) {
    SCOPED_TRACE(LowPrefix);
    const auto Zero = NdVar::cst(0, 8);
    const auto Result = NdVar::reg(a64reg::X0, 8);
    F.linear(
        {call(),
         operation(NdOp::CONCAT, NdVar::reg(TRI.VecRegBase, 16),
                   {LowPrefix ? Zero : Result, LowPrefix ? Result : Zero}),
         call(0x2100),
         operation(NdOp::STORE, {},
                   {NdVar::cst(0x3000, 8), NdVar::reg(TRI.VecRegBase, 16)}),
         constant(a64reg::X0, 0), ret()});
    // The full D0 result is independent, but the callee may copy an incoming
    // difference into Q0's unspecified high half regardless of its old value.
    EXPECT_FALSE(F.prove());
    F.Low.Blocks[0].Ops[3].Inputs[1].Size = 8;
    EXPECT_TRUE(F.prove());
  }
}

TEST(NativeBooleanResultProof, CallsPropagateAllIncidentalMachineOutputs) {
  for (const auto Output :
       {NdVar::reg(a64reg::X8, 8), NdVar::reg(a64reg::ZFLAG, 1),
        NdVar::reg(a64reg::V(8) + 8, 8), NdVar::reg(a64reg::X30, 8)}) {
    SCOPED_TRACE(Output.Offset);
    Fixture F;
    F.Release.Parameters.clear();
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(F.Release, Arch::AArch64, Error));
    F.linear({call(), copy(a64reg::X19, a64reg::X0), constant(a64reg::X0, 0),
              call(0x2100),
              operation(NdOp::STORE, {}, {NdVar::cst(0x3000, 8), Output}),
              operation(NdOp::LOAD, NdVar::reg(a64reg::X19, 8),
                        {NdVar::reg(a64reg::SP, 8)}),
              constant(a64reg::X0, 0), ret()});
    EXPECT_FALSE(F.prove());
    // A complete explicit overwrite after the call removes this observation.
    auto &Store = F.Low.Blocks[0].Ops[4];
    Store.Inputs[1] = NdVar::cst(0, Output.Size);
    EXPECT_TRUE(F.prove());
  }

  // A register absent from the function's explicit reads can carry a difference
  // through the first call and copy it into X8 at the second call. Clearing all
  // locally used volatile registers does not erase that hidden machine state.
  Fixture F;
  F.Release.Parameters.clear();
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(F.Release, Arch::AArch64, Error));
  F.linear({call(), copy(a64reg::X19, a64reg::X0), call(0x2100),
            operation(NdOp::LOAD, NdVar::reg(a64reg::X19, 8),
                      {NdVar::reg(a64reg::SP, 8)}),
            constant(a64reg::X0, 0), constant(a64reg::X8, 0), call(0x2100),
            operation(NdOp::STORE, {},
                      {NdVar::cst(0x3000, 8), NdVar::reg(a64reg::X8, 8)}),
            constant(a64reg::X0, 0), ret()});
  EXPECT_FALSE(F.prove());

  // An unmodelled register bank cannot hide a difference from call propagation.
  F.linear(
      {call(), copy(0x180, a64reg::X0), mask(a64reg::X0, a64reg::X0), ret()});
  EXPECT_FALSE(F.prove());
}

TEST(NativeBooleanResultProof, SelectedCallsPropagateLoopCarriedDifferences) {
  for (bool ClearBeforeBackedge : {false, true}) {
    SCOPED_TRACE(ClearBeforeBackedge);
    Fixture F;
    F.Low.Blocks = {
        block(0, 0x1000, {operation(NdOp::BRANCH, {}, {NdVar::cst(0x1004, 8)})},
              {}, {1}),
        block(1, 0x1004,
              {call(),
               operation(NdOp::STORE, {},
                         {NdVar::cst(0x3000, 8), NdVar::reg(a64reg::X8, 8)}),
               copy(a64reg::X19, a64reg::X0), mask(a64reg::X0, a64reg::X0),
               ClearBeforeBackedge ? constant(a64reg::X19, 0)
                                   : operation(NdOp::NOP, {}, {}),
               operation(NdOp::COND_BR, {},
                         {NdVar::cst(0x1004, 8), NdVar::reg(a64reg::X20, 1)})},
              {0, 1}, {1, 2}),
        block(2, 0x101c,
              {operation(NdOp::LOAD, NdVar::reg(a64reg::X19, 8),
                         {NdVar::reg(a64reg::SP, 8)}),
               operation(NdOp::LOAD, NdVar::reg(a64reg::X30, 8),
                         {NdVar::reg(a64reg::SP, 8)}),
               ret()},
              {1})};
    F.bindCalls();
    EXPECT_EQ(bool(F.prove()), ClearBeforeBackedge);
  }
}

TEST(NativeBooleanResultProof, EveryDeclaredResultComponentIsIndependent) {
  Fixture F;
  F.Release.Parameters.clear();
  F.Release.ReturnType = NdType::makeInt(16, false);
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(F.Release, Arch::AArch64, Error));
  ASSERT_EQ(F.Release.ReturnComponents.size(), 2U);
  F.linear({call(), copy(a64reg::X1, a64reg::X0), call(0x2100),
            operation(NdOp::STORE, {},
                      {NdVar::cst(0x3000, 8), NdVar::reg(a64reg::X1, 8)}),
            ret()});
  EXPECT_TRUE(F.prove());
  F.Release.ReturnType = NdType::makeInt(8, false);
  ASSERT_TRUE(assignDarwinScalarSourceABI(F.Release, Arch::AArch64, Error));
  EXPECT_FALSE(F.prove());

  // A complete Swift byte result describes eight result bits, not i1. Its
  // unspecified upper bytes cannot be discarded by this independent owner.
  F.Release.ReturnType = NdType::makeInt(1, false);
  ASSERT_TRUE(assignDarwinSwiftSourceABI(F.Release, Arch::AArch64, Error));
  F.linear({call(), call(0x2100),
            operation(NdOp::STORE, {},
                      {NdVar::cst(0x3000, 8), NdVar::reg(a64reg::X0, 4)}),
            constant(a64reg::X0, 0), ret()});
  EXPECT_FALSE(F.prove());
  F.Low.Blocks[0].Ops[2].Inputs[1].Size = 1;
  EXPECT_TRUE(F.prove());
}

TEST(NativeBooleanResultProof, EveryPhysicalJoinAndLoopPathMustBeSafe) {
  Fixture F;
  F.diamond();
  ASSERT_TRUE(F.prove());
  F.diamond(false);
  EXPECT_FALSE(F.prove());
  F.diamond();
  F.Low.Blocks[0].Ops.back().Inputs[1] = NdVar::reg(a64reg::X0, 1);
  EXPECT_FALSE(F.prove());

  F.Low.Blocks = {
      block(0, 0x1000,
            {call(), operation(NdOp::BRANCH, {}, {NdVar::cst(0x1008, 8)})}, {},
            {1}),
      block(1, 0x1008,
            {copy(a64reg::X8, a64reg::X0), mask(a64reg::X0, a64reg::X0),
             operation(NdOp::COND_BR, {},
                       {NdVar::cst(0x1008, 8), NdVar::reg(a64reg::X2, 1)})},
            {0, 1}, {1, 2}),
      block(2, 0x1014, {ret()}, {1})};
  F.bindCalls();
  ASSERT_TRUE(F.prove());
  // A loop's outgoing register state must meet every incoming physical path.
  F.Low.Blocks[1].Ops[1] = copy(a64reg::X0, a64reg::X8);
  F.Low.Blocks[1].Ops[1].Addr = 0x100c;
  EXPECT_FALSE(F.prove());
}

TEST(NativeBooleanResultProof, RejectsMalformedGraphsOperationsAndContracts) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F;
    F.diamond();
    auto &B = F.Low.Blocks[0];
    if (Mutation == 0)
      B.Succs.pop_back();
    if (Mutation == 1)
      B.Succs.push_back(1);
    if (Mutation == 2)
      F.Low.Blocks[2].Preds.clear();
    if (Mutation == 3)
      F.Low.Blocks[2].Id = 1;
    if (Mutation == 4)
      F.Low.Blocks[2].StartAddr = 0x1008;
    if (Mutation == 5)
      B.Ops.back().Inputs[0].Offset = 0x100c;
    if (Mutation == 6)
      F.Low.Blocks.back().EndAddr += 4;
    if (Mutation == 7)
      B.ExceptionalSuccs.emplace_back();
    if (Mutation == 8)
      B.Ops.front().Opcode = NdOp::INDIR_CALL;
    if (Mutation == 9)
      B.Ops.front().NumInputs = 7;
    if (Mutation == 10) {
      F.linear({call(), operation(NdOp::INTRINSIC, {}, {}),
                mask(a64reg::X0, a64reg::X0), ret()});
    }
    if (Mutation == 11) {
      F.linear({call(),
                operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                          {NdVar::tmp(65536, 8)}),
                mask(a64reg::X0, a64reg::X0), ret()});
    }
    if (Mutation == 12)
      B.Ops.front().Output = NdVar::reg(UINT64_MAX, 8);
    if (Mutation == 13)
      F.Entry.Architecture = Arch::X64;
    if (Mutation == 14) {
      const SourceCallOccurrenceKey Extra{0x1990, 0, NdOp::CALL, 0x3000};
      F.Calls.emplace(Extra, SourceBooleanOtherCallContract{&F.Compare});
    }
    if (Mutation == 15)
      F.Low.Blocks.resize(257);
    if (Mutation == 16) {
      auto Load = operation(NdOp::LOAD, NdVar::reg(a64reg::X8, 8),
                            {NdVar::cst(0x3000, 8)});
      Load.MemoryOrdering = NdMemoryOrdering::Acquire;
      F.linear({call(), Load, mask(a64reg::X0, a64reg::X0), ret()});
    }
    if (Mutation == 17) {
      auto Load = operation(NdOp::LOAD, NdVar::reg(a64reg::X8, 8),
                            {NdVar::cst(0x3000, 8)});
      Load.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      F.linear({call(), Load, mask(a64reg::X0, a64reg::X0), ret()});
    }
    EXPECT_FALSE(F.prove());
  }
}

TEST(NativeBooleanResultProof, ConstantShiftsAndTruncatedMasksTrackExactBits) {
  for (const auto Opcode : {NdOp::INT_RIGHT, NdOp::INT_ASHR}) {
    Fixture F;
    F.linear({call(),
              operation(Opcode, NdVar::reg(a64reg::X8, 4),
                        {NdVar::reg(a64reg::X0, 4), NdVar::cst(0, 4)}),
              operation(NdOp::INT_AND, NdVar::reg(a64reg::X9, 1),
                        {NdVar::reg(a64reg::X8, 4), NdVar::cst(1, 4)}),
              operation(NdOp::INT_ZEXT, NdVar::reg(a64reg::X0, 8),
                        {NdVar::reg(a64reg::X9, 1)}),
              ret()});
    ASSERT_TRUE(F.prove());
    F.Low.Blocks[0].Ops[1].Inputs[1].Offset = 1;
    EXPECT_FALSE(F.prove()); // Bit one reaches the observable result.
    F.Low.Blocks[0].Ops[1].Inputs[1].Offset = 32;
    EXPECT_FALSE(F.prove()); // Out-of-range shifts need separate semantics.
    F.Low.Blocks[0].Ops[1].Inputs[1] = NdVar::reg(a64reg::X2, 4);
    EXPECT_FALSE(F.prove());
    F.Low.Blocks[0].Ops[1].Inputs[1] = NdVar::cst(0, 4);
    F.Low.Blocks[0].Ops[2].Inputs[1].Offset = 3;
    EXPECT_FALSE(F.prove());
  }
}

TEST(NativeBooleanResultProof, VariableShiftsRequireIdenticalInputs) {
  for (const auto Opcode : {NdOp::INT_LEFT, NdOp::INT_RIGHT, NdOp::INT_ASHR}) {
    Fixture F;
    const auto DynamicShift =
        operation(Opcode, NdVar::reg(a64reg::X8, 8),
                  {NdVar::reg(a64reg::X1, 8), NdVar::reg(a64reg::X2, 8)});
    F.linear(
        {DynamicShift, call(), copy(a64reg::X8, a64reg::X0), mask(), ret()});
    EXPECT_TRUE(F.prove());

    F.linear({call(),
              operation(Opcode, NdVar::reg(a64reg::X8, 8),
                        {NdVar::reg(a64reg::X0, 8), NdVar::reg(a64reg::X2, 8)}),
              mask(), ret()});
    EXPECT_FALSE(F.prove());
  }
}

TEST(NativeBooleanResultProof, SelectRequiresIdenticalInputs) {
  Fixture F;
  const auto Select =
      operation(NdOp::SELECT, NdVar::reg(a64reg::X8, 1),
                {NdVar::reg(a64reg::X3, 1), NdVar::reg(a64reg::X4, 1),
                 NdVar::reg(a64reg::X5, 1)});
  F.linear({Select, call(), copy(a64reg::X8, a64reg::X0), mask(), ret()});
  EXPECT_TRUE(F.prove());

  F.linear({call(),
            operation(NdOp::SELECT, NdVar::reg(a64reg::X8, 8),
                      {NdVar::reg(a64reg::X0, 1), NdVar::reg(a64reg::X1, 8),
                       NdVar::reg(a64reg::X2, 8)}),
            constant(a64reg::X0, 0), ret()});
  EXPECT_FALSE(F.prove());
}

TEST(NativeBooleanResultProof, ConstantLeftShiftMovesTheDifferenceMask) {
  Fixture F;
  F.linear({call(),
            operation(NdOp::INT_LEFT, NdVar::reg(a64reg::X8, 8),
                      {NdVar::reg(a64reg::X0, 8), NdVar::cst(9, 8)}),
            mask(a64reg::X0, a64reg::X8, 512), ret()});
  ASSERT_TRUE(F.prove());
  F.Low.Blocks[0].Ops[2].Inputs[1].Offset = 1024;
  EXPECT_FALSE(F.prove());
  F.Low.Blocks[0].Ops[1].Inputs[1].Offset = 64;
  EXPECT_FALSE(F.prove());
}

TEST(NativeBooleanResultProof, EveryCallObservesItsImplicitStackPointer) {
  for (unsigned ArgumentCount : {0U, 1U, 9U}) {
    SCOPED_TRACE(ArgumentCount);
    Fixture F;
    F.Release.Parameters.clear();
    for (unsigned I = 0; I != ArgumentCount; ++I)
      F.Release.Parameters.push_back(
          {"argument", NdType::makePtr(NdType::makeVoid())});
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(F.Release, Arch::AArch64, Error));
    F.linear({call(), copy(a64reg::SP, a64reg::X0), constant(a64reg::X0, 0),
              call(0x3000), constant(a64reg::SP, 0), constant(a64reg::X0, 0),
              ret()});
    // Restoring SP after the call cannot undo accesses through the callee's
    // incoming stack, even when its declared arguments all use registers.
    EXPECT_FALSE(F.prove());
  }
}
