//===- MedNoReturnTests.cpp - Internal no-return propagation tests -------===//

#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedNoReturn.h"

#include <initializer_list>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

MedOp trapOp() {
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Brk), 2));
  return Op;
}

MedOp callOp(va_t Target) {
  MedOp Op;
  Op.Opcode = NdOp::CALL;
  Op.addInput(MedVar::makeConst(Target, 8));
  return Op;
}

MedOp returnOp() {
  MedOp Op;
  Op.Opcode = NdOp::RETURN;
  return Op;
}

MedBlock block(int Id, std::initializer_list<MedOp> Ops,
               std::initializer_list<int> Succs = {}) {
  MedBlock Block;
  Block.Id = Id;
  Block.Ops.assign(Ops.begin(), Ops.end());
  Block.Succs.assign(Succs.begin(), Succs.end());
  return Block;
}

MedFunc function(va_t Entry, std::initializer_list<MedBlock> Blocks) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "sub_" + std::to_string(Entry);
  Func.Blocks.assign(Blocks.begin(), Blocks.end());
  return Func;
}

} // namespace

TEST(MedNoReturn, PropagatesOnlyFromExplicitTerminatingFacts) {
  constexpr va_t TrapEntry = 0x1000;
  constexpr va_t WrapperEntry = 0x2000;
  constexpr va_t MixedEntry = 0x3000;
  constexpr va_t UnknownEntry = 0x4000;

  std::vector<MedFunc> Funcs;
  // Put the wrapper first so proving it requires a second fixed-point round.
  Funcs.push_back(
      function(WrapperEntry, {block(0, {callOp(TrapEntry), returnOp()})}));
  Funcs.push_back(function(MixedEntry, {block(0, {}, {1, 2}),
                                        block(1, {callOp(TrapEntry)}, {2}),
                                        block(2, {returnOp()})}));
  Funcs.push_back(function(UnknownEntry, {block(0, {MedOp{}})}));
  Funcs.push_back(function(TrapEntry, {block(0, {trapOp()})}));

  propagateInternalNoReturn(Funcs, Arch::AArch64);

  EXPECT_TRUE(Funcs[0].DoesNotReturn);
  EXPECT_FALSE(Funcs[1].DoesNotReturn);
  EXPECT_FALSE(Funcs[2].DoesNotReturn);
  EXPECT_TRUE(Funcs[3].DoesNotReturn);
  EXPECT_TRUE(Funcs[0].Blocks[0].Ops[0].DoesNotReturn);
  EXPECT_TRUE(Funcs[1].Blocks[1].Ops[0].DoesNotReturn);

  // The late pipeline refresh is intentionally idempotent.
  propagateInternalNoReturn(Funcs, Arch::AArch64);
  EXPECT_TRUE(Funcs[0].DoesNotReturn);
  EXPECT_TRUE(Funcs[1].Blocks[1].Ops[0].DoesNotReturn);
}

TEST(MedNoReturn, NativeSourceEffectsFollowCurrentProofWithoutSharedMutation) {
  auto Call = callOp(0x2000);
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->CallKind = SourceCallTypeHint::Kind::Native;
  Hint->TargetAddress = 0x2000;
  Call.SourceCallHint = Hint;
  std::vector<MedFunc> Functions = {
      function(0x1000, {block(0, {Call, returnOp()})}),
      function(0x2000, {block(0, {trapOp()})})};
  propagateInternalNoReturn(Functions, Arch::AArch64);
  EXPECT_TRUE(Functions[0].Blocks[0].Ops[0].SourceCallHint->DoesNotReturn);
  EXPECT_FALSE(Hint->DoesNotReturn);
  EXPECT_TRUE(hasProvenNoReturnExit(Functions[1], Arch::AArch64));
  Functions[1].Blocks[0].Ops = {returnOp()};
  EXPECT_FALSE(hasProvenNoReturnExit(Functions[1], Arch::AArch64));
  propagateInternalNoReturn(Functions, Arch::AArch64);
  EXPECT_FALSE(Functions[0].DoesNotReturn);
  EXPECT_FALSE(Functions[0].Blocks[0].Ops[0].SourceCallHint->DoesNotReturn);
}

TEST(MedNoReturn, RuntimeImportEffectSurvivesAnInventoriedVeneer) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const auto Kind : {SourceCallTypeHint::Kind::ObjCRuntimeCall,
                            SourceCallTypeHint::Kind::DarwinRuntimeCall,
                            SourceCallTypeHint::Kind::SwiftRuntimeCall}) {
      SCOPED_TRACE(static_cast<int>(Kind));
      auto Call = callOp(0x2000);
      Call.DoesNotReturn = true;
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->CallKind = Kind;
      Hint->TargetAddress = 0x4000; // The import slot, not the veneer entry.
      Hint->DoesNotReturn = true;
      Hint->Signature.ReturnType = NdType::makeVoid();
      std::string Diagnostic;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Hint->Signature, Architecture,
                                              Diagnostic))
          << Diagnostic;
      Call.SourceCallHint = Hint;
      // The imported entry has no local machine termination proof. A generic
      // inventory can still contain its veneer alongside callers and wrappers.
      std::vector<MedFunc> Functions = {
          function(0x1000, {block(0, {Call, returnOp()})}),
          function(0x2000, {block(0, {MedOp{}})}),
          function(0x3000, {block(0, {callOp(0x1000), returnOp()})})};
      for (unsigned Iteration = 0; Iteration < 2; ++Iteration) {
        propagateInternalNoReturn(Functions, Architecture);
        EXPECT_TRUE(Functions[0].DoesNotReturn);
        EXPECT_TRUE(Functions[0].Blocks[0].Ops[0].DoesNotReturn);
        EXPECT_FALSE(Functions[1].DoesNotReturn);
        EXPECT_TRUE(Functions[2].DoesNotReturn);
      }

      // A source declaration is not an independent machine termination fact.
      Functions[0].Blocks[0].Ops[0].DoesNotReturn = false;
      propagateInternalNoReturn(Functions, Architecture);
      EXPECT_FALSE(Functions[0].DoesNotReturn);
      EXPECT_FALSE(Functions[2].DoesNotReturn);

      // Revoking the external source effect must also revoke the old marker.
      Functions[0].Blocks[0].Ops[0].DoesNotReturn = true;
      Hint->DoesNotReturn = false;
      propagateInternalNoReturn(Functions, Architecture);
      EXPECT_FALSE(Functions[0].DoesNotReturn);
      EXPECT_FALSE(Functions[2].DoesNotReturn);

      // A malformed binding cannot protect a stale internal marker either.
      Functions[0].Blocks[0].Ops[0].DoesNotReturn = true;
      Hint->DoesNotReturn = true;
      Hint->Signature.Parameters = {{"missing", NdType::makeInt(8, false)}};
      ASSERT_TRUE(assignDarwinScalarSourceABI(Hint->Signature, Architecture,
                                              Diagnostic))
          << Diagnostic;
      propagateInternalNoReturn(Functions, Architecture);
      EXPECT_FALSE(Functions[0].DoesNotReturn);
      EXPECT_FALSE(Functions[2].DoesNotReturn);
    }
  }
}

TEST(MedNoReturn, CallFollowedByTrapDoesNotInventCalleeNoReturn) {
  constexpr va_t Entry = 0x1000;
  constexpr va_t Helper = 0x2000;
  constexpr va_t OrdinaryCaller = 0x3000;
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (Intrinsic Kind : {Intrinsic::Int3, Intrinsic::Int1, Intrinsic::Ud2}) {
      SCOPED_TRACE(static_cast<int>(Kind));
      MedOp Trap;
      Trap.Opcode = NdOp::INTRINSIC;
      Trap.addInput(MedVar::makeConst(static_cast<uint64_t>(Kind), 2));
      auto Call = callOp(Helper);
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->CallKind = SourceCallTypeHint::Kind::Native;
      Hint->TargetAddress = Helper;
      Call.SourceCallHint = Hint;
      std::vector<MedFunc> Functions = {
          function(Entry, {block(0, {Call, Trap, returnOp()})}),
          function(Helper, {block(0, {returnOp()})}),
          function(OrdinaryCaller, {block(0, {callOp(Helper), returnOp()})})};
      for (unsigned Iteration = 0; Iteration < 2; ++Iteration) {
        propagateInternalNoReturn(Functions, Architecture);
        EXPECT_FALSE(Functions[0].Blocks[0].Ops[0].DoesNotReturn);
        ASSERT_TRUE(Functions[0].Blocks[0].Ops[0].SourceCallHint);
        EXPECT_FALSE(
            Functions[0].Blocks[0].Ops[0].SourceCallHint->DoesNotReturn);
        EXPECT_FALSE(Hint->DoesNotReturn);
        EXPECT_FALSE(Functions[1].DoesNotReturn);
        EXPECT_FALSE(Functions[2].DoesNotReturn);
        EXPECT_FALSE(Functions[2].Blocks[0].Ops[0].DoesNotReturn);
      }
    }
  }
}
