#include "../../../lib/pipeline/NativeSourceFloatingReturn.h"
#include "gtest/gtest.h"

#include "neverd/lift/AArch64Regs.h"

#include <algorithm>
#include <array>
#include <functional>

using namespace neverd;

namespace {
MedVar fpReg(int Id, int Version, unsigned Number, uint16_t Size = 16) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.SSAVer = Version;
  V.RegOff = a64reg::V(Number);
  V.Size = Size;
  V.TheArch = Arch::AArch64;
  return V;
}

MedVar temp(int Id, uint16_t Size) {
  MedVar V;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = Size;
  return V;
}

MedOp operation(NdOp Opcode, MedVar Output,
                std::initializer_list<MedVar> Inputs = {}) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (auto V : Inputs)
    Op.addInput(V);
  return Op;
}

MedOp call(uint32_t Id, MedVar Output = {}, TypeRef Return = NdType::makeVoid(),
           bool NoReturn = false) {
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Hint.TargetName = "fixture_runtime";
  Hint.TargetAddress = 0x8000 + 8 * Id;
  Hint.DoesNotReturn = NoReturn;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  Hint.Signature.ReturnType = Return;
  std::string Error;
  EXPECT_TRUE(assignDarwinFixedSourceABI(Hint.Signature, Arch::AArch64, Error))
      << Error;
  auto Op =
      operation(NdOp::CALL, Output, {MedVar::makeConst(Hint.TargetAddress, 8)});
  Op.CallSiteId = Id;
  Op.DoesNotReturn = NoReturn;
  Op.SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Hint));
  return Op;
}

MedBlock block(int Id, std::vector<int> Preds, std::vector<int> Succs) {
  MedBlock B;
  B.Id = Id;
  B.StartAddr = 0x1000 + Id * 0x100;
  B.Preds = std::move(Preds);
  B.Succs = std::move(Succs);
  return B;
}

MedCallClobber preserved(MedVar Value, uint32_t Id, MedVar Input) {
  MedCallClobber C;
  C.Value = Value;
  C.CallSiteId = Id;
  C.PreservedInput = Input;
  C.PreservedPrefixSize = 8;
  return C;
}

// This represents a typed double result on one path and exact default bits on
// another. The full Q8 is clobbered by the runtime call; only its recorded low
// eight-byte prefix survives. The separate D8 PHI is deliberately stale after
// the typed Q8 write, as it is in the real SDImageScaleFactorForKey MedIR.
MedFunc diamond() {
  MedFunc F;
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8);
  F.Blocks = {block(0, {}, {1, 2}), block(1, {0}, {3}), block(2, {0}, {3}),
              block(3, {1, 2}, {})};
  const auto Default = MedVar::makeConst(0x3ff0000000000000, 8);
  F.Blocks[0].Ops = {operation(NdOp::COPY, fpReg(80, 1, 8, 8), {Default})};
  F.Blocks[1].Ops = {
      call(1, fpReg(100, 1, 0, 8), NdType::makeFloat(8)),
      operation(NdOp::INT_ZEXT, fpReg(0, 1, 0), {fpReg(100, 1, 0, 8)}),
      operation(NdOp::COPY, fpReg(8, 1, 8), {fpReg(0, 1, 0)}), call(2)};
  F.CallClobbers = {preserved(fpReg(8, 2, 8), 2, fpReg(8, 1, 8))};
  F.Blocks[2].Ops = {operation(NdOp::INT_ZEXT, fpReg(8, 3, 8), {Default})};
  F.Blocks[3].Phis = {
      {fpReg(8, 4, 8), {{1, fpReg(8, 2, 8)}, {2, fpReg(8, 3, 8)}}},
      {fpReg(80, 2, 8, 8), {{1, fpReg(80, 1, 8, 8)}, {2, fpReg(80, 1, 8, 8)}}}};
  F.Blocks[3].Ops = {operation(NdOp::COPY, fpReg(0, 2, 0), {fpReg(8, 4, 8)}),
                     operation(NdOp::RETURN, {})};
  return F;
}

bool proves(const MedFunc &F, Arch Architecture = Arch::AArch64) {
  return detail::hasProvenNativeSourceFloat64Return(F, Architecture);
}

void mutateHint(MedOp &Op,
                const std::function<void(SourceCallTypeHint &)> &Change) {
  auto Hint = std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
  Change(*Hint);
  Op.SourceCallHint = std::move(Hint);
}
} // namespace

TEST(NativeFloatingReturnProof, UsesTheRecordedQPrefixInsteadOfTheStaleDAlias) {
  auto F = diamond();
  EXPECT_TRUE(proves(F));
  EXPECT_EQ(F.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_FALSE(proves(F, Arch::X64));
  // An apparently good narrow alias cannot rescue an undefined Q8 arm.
  F.Blocks[3].Phis[0].Args[0].second = fpReg(8, 0, 8);
  EXPECT_FALSE(proves(F));
}

TEST(NativeFloatingReturnProof, RejectsEveryUnknownOrNonIdentityReturnArm) {
  const std::array<std::function<void(MedFunc &)>, 8> Mutations = {{
      [](auto &F) { F.Blocks[2].Ops[0].Inputs[0] = fpReg(9, 0, 9, 8); },
      [](auto &F) { F.Blocks[2].Ops[0].Opcode = NdOp::LOAD; },
      [](auto &F) {
        auto &Op = F.Blocks[2].Ops[0];
        Op.Opcode = NdOp::INT_XOR;
        Op.addInput(MedVar::makeConst(1, 8));
      },
      [](auto &F) { F.Blocks[2].Ops[0].Inputs[0].Size = 4; },
      [](auto &F) { F.Blocks[2].Ops[0].Inputs[0].Size = 16; },
      [](auto &F) { F.Blocks[3].Phis[0].Args[1].second.Size = 8; },
      [](auto &F) { F.Blocks[3].Phis[0].Args.pop_back(); },
      [](auto &F) {
        F.Blocks[3].Phis[0].Args[1].first = F.Blocks[3].Phis[0].Args[0].first;
      },
  }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    auto F = diamond();
    Mutations[I](F);
    EXPECT_FALSE(proves(F));
  }
}

TEST(NativeFloatingReturnProof, RequiresExactCurrentCallAndPreservedInput) {
  const std::array<std::function<void(MedFunc &)>, 13> Mutations = {{
      [](auto &F) { F.CallClobbers.clear(); },
      [](auto &F) { F.CallClobbers[0].CallSiteId = 50; },
      [](auto &F) { F.CallClobbers[0].PreservedInput.SSAVer = 0; },
      [](auto &F) { F.CallClobbers[0].PreservedInput = fpReg(80, 1, 8, 8); },
      [](auto &F) { F.CallClobbers[0].PreservedInput = fpReg(8, 3, 8); },
      [](auto &F) { F.CallClobbers[0].PreservedInput.RegOff = a64reg::V(9); },
      [](auto &F) { F.CallClobbers[0].PreservedInput.TheArch = Arch::X64; },
      [](auto &F) { F.CallClobbers[0].PreservedInput.Size = 8; },
      [](auto &F) { F.Blocks[1].Ops.back().PreservesCallerSaved = true; },
      [](auto &F) { F.Blocks[1].Ops.back().SourceCallHint.reset(); },
      [](auto &F) { F.Blocks[1].Ops.back().CallSiteId = 1; },
      [](auto &F) { std::swap(F.Blocks[1].Ops[2], F.Blocks[1].Ops[3]); },
      [](auto &F) {
        auto Owner = F.Blocks[1].Ops.back();
        F.Blocks[1].Ops.pop_back();
        F.Blocks.push_back(block(9, {}, {}));
        F.Blocks.back().Ops.push_back(std::move(Owner));
      },
  }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    auto F = diamond();
    Mutations[I](F);
    EXPECT_FALSE(proves(F));
  }
  for (uint16_t Prefix : {0, 4, 9, 16}) {
    SCOPED_TRACE(Prefix);
    auto F = diamond();
    F.CallClobbers[0].PreservedPrefixSize = Prefix;
    EXPECT_FALSE(proves(F));
  }
}

TEST(NativeFloatingReturnProof,
     RejectsNonCanonicalPreservedRegisterEvenIfNamedQ8) {
  auto F = diamond();
  // The SSA name is not authority for the target's preservation convention.
  for (auto &B : F.Blocks) {
    for (auto &P : B.Phis) {
      if (P.Output.Id == 8)
        P.Output.RegOff = a64reg::V(7);
      for (auto &[Pred, V] : P.Args)
        if (V.Id == 8)
          V.RegOff = a64reg::V(7);
    }
    for (auto &Op : B.Ops) {
      if (Op.Output.Id == 8)
        Op.Output.RegOff = a64reg::V(7);
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        if (Op.Inputs[I].Id == 8 && !Op.Inputs[I].isConst())
          Op.Inputs[I].RegOff = a64reg::V(7);
    }
  }
  F.CallClobbers[0].Value.RegOff = a64reg::V(7);
  F.CallClobbers[0].PreservedInput.RegOff = a64reg::V(7);
  EXPECT_FALSE(proves(F));
}

TEST(NativeFloatingReturnProof, ValidatesTypedCallWidthAndActualCarrier) {
  const std::array<std::function<void(MedFunc &)>, 7> Mutations = {{
      [](auto &F) {
        mutateHint(F.Blocks[1].Ops[0], [](auto &H) {
          H.Signature.ReturnType = NdType::makeFloat(4);
        });
      },
      [](auto &F) {
        mutateHint(F.Blocks[1].Ops[0], [](auto &H) {
          H.Signature.ReturnType = NdType::makeInt(8);
        });
      },
      [](auto &F) {
        mutateHint(F.Blocks[1].Ops[0], [](auto &H) {
          H.Signature.ReturnLocation.RegisterOffset = a64reg::V(1);
        });
      },
      [](auto &F) { F.Blocks[1].Ops[0].Output.Size = 4; },
      [](auto &F) { F.Blocks[1].Ops[0].Output.RegOff = a64reg::V(1); },
      [](auto &F) { F.Blocks[1].Ops[0].DoesNotReturn = true; },
      [](auto &F) { F.Blocks[1].Ops[0].addInput(MedVar::makeConst(42, 8)); },
  }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    auto F = diamond();
    Mutations[I](F);
    EXPECT_FALSE(proves(F));
  }

  auto Narrow = diamond();
  Narrow.Blocks[1].Ops[0] = call(1, fpReg(100, 1, 0, 4), NdType::makeFloat(4));
  Narrow.Blocks[1].Ops[1].Inputs[0].Size = 4;
  EXPECT_FALSE(proves(Narrow));

  auto Integer = diamond();
  auto X0 = fpReg(40, 1, 0, 8);
  X0.RegOff = a64reg::X0;
  Integer.Blocks[2].Ops.insert(Integer.Blocks[2].Ops.begin(),
                               call(3, X0, NdType::makeInt(8)));
  Integer.Blocks[2].Ops[1].Inputs[0] = X0;
  // Both source signatures are valid; an integer result on the other path
  // still prevents selecting a floating source signature for the function.
  EXPECT_FALSE(proves(Integer));
}

TEST(NativeFloatingReturnProof, RejectsStaleVolatileSSAAfterAnotherCall) {
  auto F = diamond();
  F.Blocks[1].Ops.insert(F.Blocks[1].Ops.begin() + 1, call(3));
  EXPECT_FALSE(proves(F));
  // An explicit temporary capture before the call retains the value, whereas
  // reusing the physical D0 SSA view after that call is stale machine state.
  F.Blocks[1].Ops.insert(
      F.Blocks[1].Ops.begin() + 1,
      operation(NdOp::COPY, temp(500, 8), {fpReg(100, 1, 0, 8)}));
  F.Blocks[1].Ops[3].Inputs[0] = temp(500, 8);
  EXPECT_TRUE(proves(F));
}

TEST(NativeFloatingReturnProof, AllowsOnlyZeroOffsetCompleteLowByteExtraction) {
  auto F = diamond();
  auto &Ops = F.Blocks[1].Ops;
  Ops.insert(Ops.begin() + 2,
             operation(NdOp::SUBBYTES, temp(200, 8),
                       {fpReg(0, 1, 0), MedVar::makeConst(0, 8)}));
  Ops.insert(Ops.begin() + 3, operation(NdOp::CONCAT, temp(201, 16),
                                        {fpReg(300, 0, 4, 8), temp(200, 8)}));
  Ops[4].Inputs[0] = temp(201, 16);
  // The high half is intentionally unknown and is never queried as a result.
  EXPECT_TRUE(proves(F));
  Ops[2].Inputs[1].ConstVal = 8;
  EXPECT_FALSE(proves(F));
  Ops[2].Inputs[1].ConstVal = 0;
  Ops[2].Output.Size = 4;
  Ops[3].Inputs[1].Size = 4;
  EXPECT_FALSE(proves(F));
}

TEST(NativeFloatingReturnProof, RequiresLocalCompleteWriteAtEveryNormalReturn) {
  const std::array<std::function<void(MedFunc &)>, 5> Mutations = {{
      [](auto &F) {
        F.Blocks[3].Ops.insert(F.Blocks[3].Ops.end() - 1,
                               operation(NdOp::COPY, fpReg(400, 1, 0, 4),
                                         {MedVar::makeConst(0, 4)}));
      },
      [](auto &F) {
        F.Blocks[3].Ops.insert(F.Blocks[3].Ops.end() - 1, call(3));
      },
      [](auto &F) { F.Blocks[3].Ops.erase(F.Blocks[3].Ops.begin()); },
      [](auto &F) {
        F.Blocks[3].Ops.insert(F.Blocks[3].Ops.end(), operation(NdOp::NOP, {}));
      },
      [](auto &F) {
        F.Blocks[0].Succs.push_back(4);
        F.Blocks.push_back(block(4, {0}, {}));
        F.Blocks.back().Ops = {operation(NdOp::RETURN, {})};
      },
  }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    auto F = diamond();
    Mutations[I](F);
    EXPECT_FALSE(proves(F));
  }
  auto F = diamond();
  F.Blocks[0].Succs.push_back(4);
  F.Blocks.push_back(block(4, {0}, {}));
  F.Blocks.back().Ops = {call(3, {}, NdType::makeVoid(), true)};
  EXPECT_TRUE(proves(F));
  F.Blocks.back().Ops[0].DoesNotReturn = false;
  EXPECT_FALSE(proves(F));

  F = diamond();
  auto Byte = fpReg(401, 1, 0, 1);
  Byte.RegOff += 3;
  F.Blocks[3].Ops.insert(
      F.Blocks[3].Ops.end() - 1,
      operation(NdOp::COPY, Byte, {MedVar::makeConst(0, 1)}));
  EXPECT_FALSE(proves(F));
}

TEST(NativeFloatingReturnProof,
     DemandsAReachableTypedAnchorForTheReturnedBytes) {
  auto F = diamond();
  F.Blocks[1].Ops[1].Inputs[0] = MedVar::makeConst(0x3ff0000000000000, 8);
  // Merely executing a double-returning call is not an anchor for other bits.
  EXPECT_FALSE(proves(F));
  F.Blocks.push_back(block(9, {}, {}));
  F.Blocks.back().Ops = {call(4, fpReg(100, 9, 0, 8), NdType::makeFloat(8))};
  F.Blocks[1].Ops[1].Inputs[0] = fpReg(100, 9, 0, 8);
  EXPECT_FALSE(proves(F));
  // A reachable definition in the other branch is also unavailable here.
  F.Blocks[2].Ops.insert(F.Blocks[2].Ops.begin(),
                         call(5, fpReg(100, 10, 0, 8), NdType::makeFloat(8)));
  F.Blocks[1].Ops[1].Inputs[0] = fpReg(100, 10, 0, 8);
  EXPECT_FALSE(proves(F));
}

TEST(NativeFloatingReturnProof, GroundsLoopPhisInAllExternalInputs) {
  auto F = diamond();
  F.Blocks[2].Succs = {4};
  F.Blocks[3].Preds = {1, 4};
  F.Blocks[3].Phis[0].Args[1] = {4, fpReg(8, 5, 8)};
  F.Blocks[3].Phis[1].Args[1].first = 4;
  F.Blocks.push_back(block(4, {2, 5}, {3, 5}));
  F.Blocks.push_back(block(5, {4}, {4}));
  F.Blocks[4].Phis = {
      {fpReg(8, 5, 8), {{2, fpReg(8, 3, 8)}, {5, fpReg(8, 6, 8)}}}};
  F.Blocks[5].Ops = {call(3)};
  F.CallClobbers.push_back(preserved(fpReg(8, 6, 8), 3, fpReg(8, 5, 8)));
  EXPECT_TRUE(proves(F));

  // Neither a self SCC nor an uninitialized external arm can establish bits.
  auto Unknown = F;
  Unknown.Blocks[4].Phis[0].Args[0].second = fpReg(8, 0, 8);
  EXPECT_FALSE(proves(Unknown));
  auto Self = F;
  Self.Blocks[4].Phis[0].Args[0].second = fpReg(8, 5, 8);
  EXPECT_FALSE(proves(Self));

  // Container order, edge order and PHI argument order are not evidence.
  std::array<int, 6> Order{{0, 1, 2, 3, 4, 5}};
  unsigned Count = 0;
  do {
    auto Permuted = F;
    Permuted.Blocks.clear();
    for (int I : Order) {
      auto B = F.Blocks[I];
      std::reverse(B.Preds.begin(), B.Preds.end());
      std::reverse(B.Succs.begin(), B.Succs.end());
      for (auto &P : B.Phis)
        std::reverse(P.Args.begin(), P.Args.end());
      Permuted.Blocks.push_back(std::move(B));
    }
    SCOPED_TRACE(Count++);
    EXPECT_TRUE(proves(Permuted));
  } while (std::next_permutation(Order.begin(), Order.end()));
  EXPECT_EQ(Count, 720u);
}

TEST(NativeFloatingReturnProof,
     RejectsUnseededEntryCycleDespiteSeparateTypedExit) {
  auto F = diamond();
  F.Blocks[0].Preds = {2};
  F.Blocks[2].Succs = {0};
  F.Blocks[3].Preds = {1};
  F.Blocks[3].Phis[0].Args.pop_back();
  F.Blocks[3].Phis[1].Args.pop_back();
  F.Blocks[0].Phis = {{fpReg(8, 7, 8), {{2, fpReg(8, 3, 8)}}}};
  F.Blocks[2].Ops[0] = operation(NdOp::COPY, fpReg(8, 3, 8), {fpReg(8, 7, 8)});
  // Make the actual returned value depend on this unseeded cycle while a
  // valid, reachable double result still exists elsewhere in the function.
  F.Blocks[1].Ops[2].Inputs[0] = fpReg(8, 7, 8);
  EXPECT_FALSE(proves(F));
}

TEST(NativeFloatingReturnProof,
     EntryBackedgeTypedSeedDoesNotDefineTheFirstInvocation) {
  auto F = diamond();
  F.Blocks[0].Preds = {2};
  F.Blocks[2].Succs = {0};
  F.Blocks[3].Preds = {1};
  F.Blocks[3].Phis[0].Args.pop_back();
  F.Blocks[3].Phis[1].Args.pop_back();
  F.Blocks[0].Phis = {{fpReg(8, 7, 8), {{2, fpReg(8, 3, 8)}}}};
  F.Blocks[2].Ops = {
      call(3, fpReg(100, 50, 0, 8), NdType::makeFloat(8)),
      operation(NdOp::INT_ZEXT, fpReg(8, 3, 8), {fpReg(100, 50, 0, 8)})};
  F.Blocks[1].Ops[2].Inputs[0] = fpReg(8, 7, 8);
  // This reachable typed seed executes only on the backedge. The very first
  // entry-to-return path still has no value for the PHI's omitted entry edge.
  EXPECT_FALSE(proves(F));

  F = diamond();
  F.Blocks[0].Preds = {0};
  F.Blocks[0].Succs.push_back(0);
  EXPECT_FALSE(proves(F));
}
