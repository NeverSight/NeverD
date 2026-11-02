//===- LegacyGSRecoveryTests.cpp - Explicit GS exchange contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent postconditions for legacy GS state and its recovery boundary.
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/RecoveryArchitecture.h"
#include "../../lib/lift/X86/X86NativeStateAudit.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExec.h"

#include <map>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {
SpecializationOptions options(bool MXCSR = false) {
  SpecializationOptions O;
  O.ExplicitMachineState = true;
  O.NormalNonfaultingExecution = true;
  O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
  O.X64LegacyGSState = true;
  if (MXCSR)
    O.X64MXCSRMask = 0xffbf;
  return O;
}

LowOp operation(NdOp Code, NdVar Output = {},
                std::initializer_list<NdVar> Inputs = {}) {
  LowOp Op;
  Op.Opcode = Code;
  Op.Output = Output;
  for (const auto Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowOp swapgs() {
  return operation(NdOp::INTRINSIC, {},
                   {NdVar::scalar(uint64_t(Intrinsic::Swapgs), 2)});
}

void exchange(SymState &S, RuntimeControlState &Controls,
              const SpecializationOptions &O) {
  uint64_t Next = 0x1000;
  const auto T = lowerRuntimeControlOperation(
      swapgs(), S, Controls, O, [&](uint16_t Size) {
        const auto Slot = NdVar::tmp(Next, Size);
        Next += Size;
        return Slot;
      });
  ASSERT_TRUE(T);
  ASSERT_TRUE(T->OutputState);
  EXPECT_FALSE(T->Rejected);
  SymExec E(S.context(), S);
  for (const auto &Op : T->Ops)
    ASSERT_EQ(E.step(Op), StepResult::Continue);
  auto Error =
      commitRuntimeControlState(S, Controls, T->OutputState->first,
                                E.operandValue(T->OutputState->second), O);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

void equal(SymContext &C, SymRef Left, SymRef Right) {
  solver::BitVectorSolver Solver(C);
  ASSERT_TRUE(Solver.assertDistinct(Left, Right));
  EXPECT_EQ(Solver.check(), solver::SatResult::Unsat);
}

TEST(LegacyGSRecovery, ExchangesBothCompleteBasesForEveryInput) {
  for (bool MXCSR : {false, true}) {
    auto O = options(MXCSR);
    SymContext C;
    SymState S(C);
    seedRuntimeMemoryState(C, S, O);
    const auto FS = S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS);
    const auto Active =
        S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS);
    auto Controls = seedRuntimeControlState(C, S, O);
    ASSERT_EQ(Controls.Values.size(), MXCSR ? 2U : 1U);
    const auto Original = Controls.Values;
    const auto Inactive = C.mkExtract(Original.back(), 64, 64);
    const auto Memory = S.load(C.mkConst(64, 0x8000), 8);
    const auto Register = S.read(SymSpace::Register, x86reg::RAX, 8);
    const auto Flags = S.read(SymSpace::Register, x86reg::CF, 1);
    exchange(S, Controls, O);
    equal(C, S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS),
          Inactive);
    equal(C, C.mkExtract(Controls.Values.back(), 64, 64), Active);
    // Equality of the bases is allowed; the exchanged value is not assumed
    // equal to its old value either. These are independent satisfiable
    // controls.
    solver::BitVectorSolver Aliases(C), Changed(C);
    ASSERT_TRUE(Aliases.assertEqual(Active, Inactive));
    EXPECT_EQ(Aliases.check(), solver::SatResult::Sat);
    ASSERT_TRUE(Changed.assertDistinct(Active, Inactive));
    EXPECT_EQ(Changed.check(), solver::SatResult::Sat);
    exchange(S, Controls, O);
    equal(C, Controls.Values.back(), Original.back());
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS), Active);
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS), FS);
    EXPECT_EQ(S.load(C.mkConst(64, 0x8000), 8), Memory);
    EXPECT_EQ(S.read(SymSpace::Register, x86reg::RAX, 8), Register);
    EXPECT_EQ(S.read(SymSpace::Register, x86reg::CF, 1), Flags);
    if (MXCSR)
      EXPECT_EQ(Controls.Values[0], Original[0]);
  }
}

TEST(LegacyGSRecovery, SegmentedMemoryUsesTheExchangedFullAddress) {
  constexpr uint64_t A = UINT64_C(0xffff800000010000);
  for (uint64_t B : {UINT64_C(0x0000000700002000), A}) {
    auto O = options();
    O.X64SegmentedReads = O.X64SegmentedWrites = true;
    SymContext C;
    SymState S(C);
    SymExec E(C, S);
    ASSERT_TRUE(S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                               C.mkConst(64, A)));
    ASSERT_TRUE(S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                               C.mkConst(64, 0x3000)));
    RuntimeControlState Controls{
        {C.mkConcat(C.mkConst(64, B), C.mkConst(64, A))}};
    ASSERT_TRUE(S.store(C.mkConst(64, A + 24), C.mkConst(64, 11)));
    ASSERT_TRUE(S.store(C.mkConst(64, B + 24), C.mkConst(64, 22)));
    auto Load = operation(NdOp::LOAD, NdVar::reg(x86reg::RAX, 8),
                          {NdVar::scalar(24, 8)});
    Load.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    ASSERT_TRUE(supportsRuntimeMemoryOperation(Load, O));
    ASSERT_EQ(E.step(Load), StepResult::Continue);
    equal(C, E.operandValue(Load.Output), C.mkConst(64, A == B ? 22 : 11));
    exchange(S, Controls, O);
    ASSERT_EQ(E.step(Load), StepResult::Continue);
    equal(C, E.operandValue(Load.Output), C.mkConst(64, 22));
    auto Store = operation(NdOp::STORE, {},
                           {NdVar::scalar(24, 8), NdVar::scalar(33, 8)});
    Store.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    ASSERT_TRUE(supportsRuntimeMemoryOperation(Store, O));
    ASSERT_EQ(E.step(Store), StepResult::Continue);
    equal(C, S.load(C.mkConst(64, B + 24), 8), C.mkConst(64, 33));
    exchange(S, Controls, O);
    ASSERT_EQ(E.step(Load), StepResult::Continue);
    equal(C, E.operandValue(Load.Output), C.mkConst(64, A == B ? 33 : 11));
    equal(C, S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS),
          C.mkConst(64, 0x3000));
    O.X64SegmentedReads = O.X64SegmentedWrites = false;
    EXPECT_FALSE(supportsRuntimeMemoryOperation(Load, O));
    EXPECT_FALSE(supportsRuntimeMemoryOperation(Store, O));
  }
}

TEST(LegacyGSRecovery, RefusesInvalidDomainsAndMalformedIntrinsics) {
  for (unsigned Case = 0; Case != 7; ++Case) {
    auto O = options();
    switch (Case) {
    case 0:
      O.ExplicitMachineState = false;
      break;
    case 1:
      O.NormalNonfaultingExecution = false;
      break;
    case 2:
      O.Architecture = Arch::AArch64;
      break;
    case 3:
      O.Architecture = Arch::X86;
      break;
    case 4:
      O.ByteOrder = llvm::endianness::big;
      break;
    case 5:
      O.X64FlagsProfile.reset();
      break;
    case 6:
      O.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
      break;
    }
    auto Error = validateRuntimeControlOptions(O);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_FALSE(supportsRuntimeControlOperation(swapgs(), O));
  }
  EXPECT_FALSE(supportsRuntimeControlOperation(swapgs(), {}));
  for (unsigned Case = 0; Case != 7; ++Case) {
    auto Op = swapgs();
    switch (Case) {
    case 0:
      Op.Output = NdVar::tmp(8, 1);
      break;
    case 1:
      Op.addInput(NdVar::scalar(0, 8));
      break;
    case 2:
      Op.Inputs[0].Size = 8;
      break;
    case 3:
      Op.Inputs[0] = NdVar::reg(0, 2);
      break;
    case 4:
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 5:
      Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 6:
      Op.Inputs[0].Offset = uint64_t(Intrinsic::Wrmsr);
      break;
    }
    EXPECT_FALSE(supportsRuntimeControlOperation(Op, options()));
  }
}

TEST(LegacyGSRecovery, RequiresTheCurrentActiveBaseAndValidTemporaryExtents) {
  const auto O = options(true);
  SymContext C;
  SymState S(C);
  EXPECT_TRUE(seedRuntimeControlState(C, O).Values.empty());
  EXPECT_TRUE(seedRuntimeControlState(C, S, O).Values.empty());
  seedRuntimeMemoryState(C, S, O);
  const auto Controls = seedRuntimeControlState(C, S, O);
  for (unsigned Case = 0; Case != 8; ++Case) {
    auto Broken = Controls;
    switch (Case) {
    case 0:
      Broken.Values.clear();
      break;
    case 1:
      Broken.Values.pop_back();
      break;
    case 2:
      Broken.Values.push_back(C.mkZero(32));
      break;
    case 3:
      Broken.Values[0] = C.mkZero(64);
      break;
    case 4:
      Broken.Values[1] = C.mkZero(64);
      break;
    case 5:
      Broken.Values[1] = {};
      break;
    case 6:
      Broken.Values[1] = SymRef(UINT32_MAX - 1);
      break;
    case 7:
      Broken.Values[1] = C.mkZero(128);
      break;
    }
    EXPECT_FALSE(
        lowerRuntimeControlOperation(swapgs(), S, Broken, O, [](uint16_t B) {
          return NdVar::tmp(0x1000, B);
        }));
  }
  for (unsigned Case = 0; Case != 4; ++Case) {
    auto T =
        lowerRuntimeControlOperation(swapgs(), S, Controls, O, [&](uint16_t B) {
          if (Case == 0)
            return NdVar::reg(0x1000, B);
          if (Case == 1)
            return NdVar::tmp(InvalidVA, B);
          if (Case == 2)
            return NdVar::tmp(0x1000, B - 1);
          return NdVar::tmp(0x1000, B); // Overlapping scratch slots.
        });
    EXPECT_FALSE(T);
  }
  S.clearX64MemoryAddressSpaceBases();
  EXPECT_FALSE(
      lowerRuntimeControlOperation(swapgs(), S, Controls, O, [](uint16_t B) {
        return NdVar::tmp(0x1000, B);
      }));
}

TEST(LegacyGSRecovery, FailedCommitPreservesControlAndMemoryState) {
  const auto O = options(true);
  SymContext C;
  SymState S(C);
  seedRuntimeMemoryState(C, S, O);
  auto Controls = seedRuntimeControlState(C, S, O);
  const auto Original = Controls.Values;
  const auto Base = S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS);
  const auto Memory = S.load(C.mkConst(64, 0x8000), 8);
  for (auto [Index, Value] : {std::pair<size_t, SymRef>{2, Original.back()},
                              {1, C.mkZero(64)},
                              {0, Original.back()},
                              {1, {}},
                              {1, SymRef(UINT32_MAX - 1)}}) {
    auto Error = commitRuntimeControlState(S, Controls, Index, Value, O);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Controls.Values, Original);
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS), Base);
    EXPECT_EQ(S.load(C.mkConst(64, 0x8000), 8), Memory);
  }
}

TEST(LegacyGSRecovery, MXCSRWritesAndGSExchangesUseIndependentControlSlots) {
  const auto O = options(true);
  SymContext C;
  SymState S(C);
  seedRuntimeMemoryState(C, S, O);
  auto Controls = seedRuntimeControlState(C, S, O);
  exchange(S, Controls, O);
  const auto GS = Controls.Values.back();
  auto Write = operation(NdOp::INTRINSIC, {},
                         {NdVar::scalar(uint64_t(Intrinsic::X86WriteMXCSR), 2),
                          NdVar::scalar(0x1f80, 4)});
  uint64_t Next = 0x1000;
  const auto T =
      lowerRuntimeControlOperation(Write, S, Controls, O, [&](uint16_t B) {
        const auto V = NdVar::tmp(Next, B);
        Next += B;
        return V;
      });
  ASSERT_TRUE(T);
  ASSERT_TRUE(T->OutputState);
  ASSERT_TRUE(T->Rejected);
  SymExec E(C, S);
  for (const auto &Op : T->Ops)
    ASSERT_EQ(E.step(Op), StepResult::Continue);
  equal(C, E.operandValue(*T->Rejected), C.mkZero(8));
  auto Error =
      commitRuntimeControlState(S, Controls, T->OutputState->first,
                                E.operandValue(T->OutputState->second), O);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  equal(C, Controls.Values[0], C.mkConst(32, 0x1f80));
  EXPECT_EQ(Controls.Values.back(), GS);
  exchange(S, Controls, O);
  equal(C, Controls.Values[0], C.mkConst(32, 0x1f80));
}

class Provider : public SpecializationProvider {
public:
  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    SpecializationInstruction I;
    I.Origin.Address = Cursor.Address;
    I.Origin.Size = 1;
    I.Fallthrough = {Cursor.Address + 1};
    if (Cursor.Address == 0x100 || Cursor.Address == 0x200) {
      I.Ops = {swapgs()};
    } else if (Cursor.Address == 0x101) {
      I.Ops = {operation(NdOp::BRANCH, {}, {NdVar::scalar(0x200, 8)})};
      I.Origin.Control = LowInstructionControl::Branch;
      I.Origin.ControlFlags = LowInstructionControlFlag::Branch;
      I.Origin.Immediate = 0x200;
    } else if (Cursor.Address == 0x201) {
      I.Ops = {operation(NdOp::RETURN)};
      I.Origin.Control = LowInstructionControl::Return;
      I.Origin.ControlFlags = LowInstructionControlFlag::Return;
    } else {
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "missing test instruction");
    }
    I.Origin.OpCount = I.Ops.size();
    for (auto &Op : I.Ops)
      Op.Addr = Cursor.Address;
    return I;
  }
};

TEST(LegacyGSRecovery, RawRecoveryRetainsEffectsAcrossNodeProjection) {
  Provider P;
  for (unsigned Transfers : {0U, 4U}) {
    auto O = options(true);
    O.MaxChainedTransfers = Transfers;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    unsigned Count = 0;
    for (const auto &Block : R.Residual.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::INTRINSIC) {
          EXPECT_EQ(Op.Inputs[0], swapgs().Inputs[0]);
          ++Count;
        }
    EXPECT_EQ(Count, 2U);
    if (!Transfers)
      EXPECT_GE(R.Residual.Blocks.size(), 2U);
    O.MaxOperations = R.EvaluatedOperations;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
    --O.MaxOperations;
    EXPECT_EQ(specializeInterpreter(P, {0x100}, O).Status,
              SpecializationStatus::BudgetExceeded);
  }
  EXPECT_EQ(specializeInterpreter(P, {0x100}).Status,
            SpecializationStatus::Unsupported);
}

TEST(LegacyGSRecovery, RawRecoveryUsesChangedBasesWithoutInventingSeparation) {
  class MemoryProvider : public SpecializationProvider {
  public:
    unsigned Exchanges = 1;
    llvm::Expected<SpecializationInstruction>
    instruction(SpecializationCursor Cursor) override {
      SpecializationInstruction I;
      I.Origin.Address = Cursor.Address;
      I.Origin.Size = 1;
      if (Cursor.Address == 0x100) {
        auto Load =
            operation(NdOp::LOAD, NdVar::reg(8, 8), {NdVar::scalar(24, 8)});
        Load.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
        I.Ops.push_back(Load);
        for (unsigned N = 0; N != Exchanges; ++N)
          I.Ops.push_back(swapgs());
        Load.Output = NdVar::reg(16, 8);
        I.Ops.push_back(Load);
        I.Ops.push_back(operation(NdOp::INT_NOTEQUAL, NdVar::reg(24, 1),
                                  {NdVar::reg(8, 8), NdVar::reg(16, 8)}));
        I.Ops.push_back(operation(
            NdOp::COND_BR, {}, {NdVar::scalar(0x200, 8), NdVar::reg(24, 1)}));
        I.Fallthrough = {0x300};
        I.Origin.Control = LowInstructionControl::Branch;
        I.Origin.ControlFlags = LowInstructionControlFlag::Branch |
                                LowInstructionControlFlag::Conditional;
        I.Origin.Immediate = 0x200;
      } else if (Cursor.Address == 0x200 || Cursor.Address == 0x300) {
        I.Ops = {operation(NdOp::RETURN)};
        I.Origin.Control = LowInstructionControl::Return;
        I.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else {
        return llvm::createStringError(llvm::errc::invalid_argument,
                                       "missing test instruction");
      }
      I.Origin.OpCount = I.Ops.size();
      for (unsigned N = 0; N != I.Ops.size(); ++N) {
        I.Ops[N].Addr = Cursor.Address;
        I.Ops[N].Seq = N;
      }
      return I;
    }
  } P;
  for (unsigned Exchanges : {1U, 2U}) {
    P.Exchanges = Exchanges;
    auto O = options();
    O.X64SegmentedReads = true;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    unsigned Returns = 0, Conditions = 0;
    for (const auto &Block : R.Residual.Blocks)
      for (const auto &Op : Block.Ops) {
        Returns += Op.Opcode == NdOp::RETURN;
        Conditions += Op.Opcode == NdOp::COND_BR;
      }
    // A single exchange permits equal OR different loaded words, even when
    // the bases alias. A double exchange reads the same unchanged cell.
    EXPECT_EQ(Returns, Exchanges == 1 ? 2U : 1U);
    EXPECT_EQ(Conditions, Exchanges == 1 ? 1U : 0U);
  }
}

TEST(LegacyGSRecovery, NativeEncodingHasSeparateCompleteAndOpaqueReceipts) {
  const uint8_t Bytes[] = {0x0f, 0x01, 0xf8};
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setStrict(true);
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), 0x1000, I), 3);
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  D.liftToLow(I, Ops, {}, {}, &Effects, &Preserved);
  ASSERT_EQ(Ops.size(), 1U);
  ASSERT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
  ASSERT_EQ(Ops.front().NumInputs, 1U);
  EXPECT_EQ(Ops.front().Inputs[0], NdVar::cst(uint64_t(Intrinsic::Swapgs), 2));
  EXPECT_TRUE(supportsRuntimeControlOperation(Ops.front(), options()));
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Preserved.Audit, LowPreservedStateAudit::LegacyIntegerV1);
  EXPECT_EQ(Preserved.StateSet, LowPreservedStateSet::LegacyIntegerOpaqueV1);
  EXPECT_EQ(Preserved.OperationDigest, Effects.OperationDigest);
}

TEST(LegacyGSRecovery,
     NativeAuditRejectsPrefixesAndInconsistentDecoderDetails) {
  csh Handle = 0;
  ASSERT_EQ(cs_open(CS_ARCH_X86, CS_MODE_64, &Handle), CS_ERR_OK);
  ASSERT_EQ(cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON), CS_ERR_OK);
  const uint8_t Bytes[] = {0x0f, 0x01, 0xf8};
  cs_insn *Decoded = nullptr;
  ASSERT_EQ(cs_disasm(Handle, Bytes, sizeof(Bytes), 0x1000, 1, &Decoded), 1U);
  const auto Check = [](const cs_insn *I) {
    return nativestateaudit::swapgsForm(I, Arch::X64);
  };
  EXPECT_TRUE(Check(Decoded));
  EXPECT_FALSE(nativestateaudit::swapgsForm(Decoded, Arch::X86));
  EXPECT_FALSE(nativestateaudit::swapgsForm(Decoded, Arch::AArch64));
  EXPECT_FALSE(Check(nullptr));
  for (unsigned Case = 0; Case != 16; ++Case) {
    auto I = *Decoded;
    auto Detail = *Decoded->detail;
    I.detail = &Detail;
    auto &X = Detail.x86;
    switch (Case) {
    case 0:
      I.size = 4;
      break;
    case 1:
      I.bytes[2] = 0xf9;
      break;
    case 2:
      X.prefix[0] = 0xf0;
      break;
    case 3:
      X.prefix[1] = 0x65;
      break;
    case 4:
      X.prefix[2] = 0x66;
      break;
    case 5:
      X.prefix[3] = 0x67;
      break;
    case 6:
      X.rex = 0x48;
      break;
    case 7:
      X.addr_size = 4;
      break;
    case 8:
      X.modrm = 0;
      break;
    case 9:
      X.op_count = 1;
      break;
    case 10:
      X.encoding.modrm_offset = 0;
      break;
    case 11:
      X.encoding.disp_size = 1;
      break;
    case 12:
      X.encoding.imm_size = 1;
      break;
    case 13:
      X.opcode[2] = 1;
      break;
    case 14:
      X.eflags = 1;
      break;
    case 15:
      I.detail = nullptr;
      break;
    }
    EXPECT_FALSE(Check(&I)) << Case;
  }
  cs_free(Decoded, 1);
  for (uint8_t Prefix : {0xf0, 0xf2, 0xf3, 0x65, 0x66, 0x67, 0x48}) {
    const uint8_t Prefixed[] = {Prefix, 0x0f, 0x01, 0xf8};
    Decoded = nullptr;
    if (cs_disasm(Handle, Prefixed, sizeof(Prefixed), 0x1000, 1, &Decoded)) {
      EXPECT_FALSE(Check(Decoded));
      cs_free(Decoded, 1);
    }
  }
  cs_close(&Handle);
}
} // namespace
