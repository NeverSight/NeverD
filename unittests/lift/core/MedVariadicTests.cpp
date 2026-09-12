//===- MedVariadicTests.cpp - Variadic lookup boundaries ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include <initializer_list>

namespace neverd {

// Internal calling-convention recognizer, also used by detectCc.
void detectVariadic(MedFunc &Func, const TargetRegInfo &TRI, Arch TargetArch,
                    BinaryFormat Fmt);

} // namespace neverd

namespace {

using namespace neverd;

MedVar reg(int Id, int Version, uint16_t Size, uint64_t Offset) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.SSAVer = Version;
  V.Size = Size;
  V.RegOff = Offset;
  return V;
}

MedVar temp(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = Size;
  return V;
}

MedOp op(NdOp Opcode, MedVar Output, std::initializer_list<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const MedVar &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

MedFunc homeFunction(Arch TheArch) {
  const auto &TRI = getTargetRegInfo(TheArch);
  const uint16_t Size = TRI.PointerSize;
  const MedVar SP = reg(100, 0, Size, TRI.StackPointer);
  const MedVar Value = temp(1, Size);
  const MedVar Slot = temp(2, Size);
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Ops = {
      op(NdOp::INT_ADD, Value, {SP, MedVar::makeConst(2 * Size, Size)}),
      op(NdOp::INT_ADD, Slot,
         {SP, MedVar::makeConst(static_cast<uint64_t>(-16), Size)}),
      op(NdOp::STORE, {}, {Slot, Value}),
      op(NdOp::LOAD, temp(3, Size), {Slot}),
  };
  return F;
}

MedFunc walkFunction() {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const MedVar SP = reg(100, 0, 8, TRI.StackPointer);
  const MedVar Walk = reg(10, 1, 8, TRI.IntParamRegs[0]);
  const MedVar Next = reg(10, 2, 8, TRI.IntParamRegs[0]);
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Ops = {
      op(NdOp::INT_OR, Walk, {SP, MedVar::makeConst(8, 8)}),
      op(NdOp::INT_ADD, Next, {Walk, MedVar::makeConst(8, 8)}),
      op(NdOp::LOAD, temp(3), {Next}),
  };
  return F;
}

void classify(MedFunc &F, Arch TheArch = Arch::AArch64) {
  F.IsVariadic = false;
  F.VariadicOverflowBase = -1;
  detectVariadic(F, getTargetRegInfo(TheArch), TheArch,
                 TheArch == Arch::AArch64 ? BinaryFormat::MachO
                                          : BinaryFormat::ELF);
}

TEST(MedVariadic, HomeSlotProofKeepsIdentityPhiAndAddressSpaceBoundaries) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (int Mode = 0; Mode < 16; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc F = homeFunction(Arch::AArch64);
    auto &Ops = F.Blocks[0].Ops;
    const MedVar SP = Ops[0].Inputs[0];
    const MedVar Value = Ops[0].Output;
    PhiNode Phi;
    Phi.Output = Value;
    Phi.Args = {{0, SP}, {1, SP}};
    switch (Mode) {
    case 1:
      // A unique PHI has priority over an ordinary definition of its output.
      Ops[0] = op(NdOp::COPY, Value, {MedVar::makeConst(16, 8)});
      F.Blocks[0].Phis.push_back(Phi);
      break;
    case 2:
      Phi.Args[1].second = Ops[1].Output;
      F.Blocks[0].Phis.push_back(Phi);
      break;
    case 3:
      Ops.push_back(Ops[0]);
      break;
    case 4:
      F.Blocks[0].Phis = {Phi, Phi};
      break;
    case 5:
      Phi.Args.clear();
      F.Blocks[0].Phis.push_back(Phi);
      break;
    case 6:
      Ops[0].Inputs[1].Provenance = ConstantAddressProvenance::CodeAddress;
      break;
    case 7:
      Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 8:
      Ops[3].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 9:
      Ops[2].Inputs[1] = MedVar::makeConst(16, 8);
      break;
    case 10:
      Ops[2].Inputs[1].Size = 4;
      break;
    case 11:
      Ops[2].Inputs[1].SSAVer = 2;
      break;
    case 12: {
      MedVar R = reg(1, 1, 8, TRI.IntParamRegs[0]);
      Ops[0].Output = R;
      Ops[2].Inputs[1] = R;
      Ops[2].Inputs[1].RegOff = TRI.IntParamRegs[1];
      break;
    }
    case 13: {
      MedOp OtherWidth = Ops[0];
      OtherWidth.Output.Size = 4;
      Ops.push_back(OtherWidth);
      break;
    }
    case 14: {
      MedVar R = reg(1, 1, 8, TRI.IntParamRegs[0]);
      Ops[0].Output = R;
      Ops[2].Inputs[1] = R;
      MedOp OtherRegister = Ops[0];
      OtherRegister.Output.RegOff = TRI.IntParamRegs[1];
      Ops.push_back(OtherRegister);
      break;
    }
    case 15:
      // A bare entry SP is recognized before the definition ambiguity gate.
      Ops[2].Inputs[1] = SP;
      Ops.push_back(op(NdOp::COPY, SP, {MedVar::makeConst(7, 8)}));
      Ops.push_back(op(NdOp::COPY, SP, {MedVar::makeConst(9, 8)}));
      break;
    }
    // Unrelated later definitions must neither truncate nor poison the proof.
    for (int I = 0; I < 128; ++I)
      Ops.push_back(op(NdOp::LOAD, temp(1000 + I), {temp(2000 + I)}));
    classify(F);
    const bool Expected = Mode == 0 || Mode == 1 || Mode >= 13;
    EXPECT_EQ(F.IsVariadic, Expected);
    if (Expected)
      EXPECT_EQ(F.VariadicOverflowBase, Mode == 1 || Mode == 15 ? 0 : 16);
  }
}

TEST(MedVariadic, DefinitionLookupIsFreshForEachFunctionAndEdit) {
  for (Arch TheArch : {Arch::AArch64, Arch::X86, Arch::X64, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(TheArch));
    const auto &TRI = getTargetRegInfo(TheArch);
    const uint16_t Size = TRI.PointerSize;
    MedFunc Original = homeFunction(TheArch);
    auto &Ops = Original.Blocks[0].Ops;
    if (TheArch == Arch::X64) {
      Ops.push_back(op(NdOp::STORE, {},
                       {Ops[1].Output, reg(20, 0, Size, TRI.IntParamRegs[0])}));
      Ops.push_back(
          op(NdOp::STORE, {},
             {Ops[1].Output, MedVar::makeConst((uint64_t{48} << 32) | 8, 8)}));
    } else if (TheArch == Arch::ARM) {
      Ops[1].Inputs[1] = MedVar::makeConst(static_cast<uint64_t>(-Size), Size);
      Ops.push_back(
          op(NdOp::STORE, {},
             {Ops[1].Output, reg(20, 0, Size, TRI.IntParamRegs.back())}));
      Ops.push_back(
          op(NdOp::STORE, {},
             {Ops[1].Output, reg(21, 0, Size, TRI.IntParamRegs.front())}));
    }
    MedFunc F = Original;
    classify(F, TheArch);
    ASSERT_TRUE(F.IsVariadic);
    EXPECT_EQ(F.VariadicOverflowBase, 2 * Size);

    if (TheArch == Arch::X64)
      F.Blocks[0].Ops.back().Inputs[1] = MedVar::makeConst(0, 8);
    else if (TheArch == Arch::ARM)
      F.Blocks[0].Ops[4].Inputs[1] = F.Blocks[0].Ops[5].Inputs[1];
    else
      F.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(0, Size);
    classify(F, TheArch);
    EXPECT_FALSE(F.IsVariadic);

    // Reusing the same function object and interleaving a second one must
    // create fresh lookups over the current operation storage.
    classify(Original, TheArch);
    EXPECT_TRUE(Original.IsVariadic);
    F = Original;
    classify(F, TheArch);
    EXPECT_TRUE(F.IsVariadic);
    EXPECT_EQ(F.VariadicOverflowBase, 2 * Size);
  }
}

TEST(MedVariadic, DarwinWalkRetainsFirstCopyAndOrderedOrCandidates) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (int Mode = 0; Mode < 10; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc F = walkFunction();
    auto &Ops = F.Blocks[0].Ops;
    const MedVar SP = Ops[0].Inputs[0];
    const MedVar Walk = Ops[0].Output;
    const MedVar Next = Ops[1].Output;
    if (Mode >= 1 && Mode <= 3) {
      MedVar Address = reg(500, 1, 8, TRI.IntParamRegs[2]);
      MedVar CopyView = reg(500, 1, 4, TRI.IntParamRegs[3]);
      MedVar Other = reg(600, 1, 8, TRI.IntParamRegs[1]);
      Ops[2].Inputs[0] = Address;
      Ops.push_back(op(NdOp::COPY, CopyView, {})); // not an eligible COPY
      Ops.push_back(op(NdOp::COPY, CopyView, {Mode == 2 ? Other : Next}));
      if (Mode != 1)
        Ops.push_back(op(NdOp::COPY, CopyView, {Mode == 2 ? Next : Other}));
    } else if (Mode == 4 || Mode == 8) {
      // A failed earlier OR must not hide the next matching candidate.
      MedOp Valid = Ops[0];
      if (Mode == 4)
        Ops[0].Inputs[0] = temp(700);
      else
        Ops[0] = op(NdOp::INT_OR, Walk, {SP});
      Ops.push_back(Valid);
    } else if (Mode == 5 || Mode == 6) {
      MedVar Negative = temp(700);
      Ops.push_back(op(NdOp::INT_ADD, Negative,
                       {SP, MedVar::makeConst(static_cast<uint64_t>(-16), 8)}));
      MedOp NegativeOr =
          op(NdOp::INT_OR, Walk, {Negative, MedVar::makeConst(0, 8)});
      if (Mode == 5) {
        MedOp PositiveOr = Ops[0];
        Ops[0] = NegativeOr;
        Ops.push_back(PositiveOr);
      } else {
        Ops.push_back(NegativeOr);
      }
    } else if (Mode == 7) {
      Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    } else if (Mode == 9) {
      // An exact entry-SP definition wins over the coarser OR fallback.
      Ops[0] = op(NdOp::INT_ADD, Walk, {SP, MedVar::makeConst(16, 8)});
      MedVar Negative = temp(700);
      Ops.push_back(op(NdOp::INT_ADD, Negative,
                       {SP, MedVar::makeConst(static_cast<uint64_t>(-16), 8)}));
      MedVar NarrowWalk = Walk;
      NarrowWalk.Size = 4;
      Ops.push_back(
          op(NdOp::INT_OR, NarrowWalk, {Negative, MedVar::makeConst(0, 8)}));
    }
    classify(F);
    EXPECT_EQ(F.IsVariadic, Mode != 2 && Mode != 5 && Mode != 7);
  }
}

TEST(MedVariadic, ProofAndCopyDepthLimitsRemainPerRoot) {
  for (int Copies : {0, 62, 63}) {
    SCOPED_TRACE(Copies);
    MedFunc F = homeFunction(Arch::AArch64);
    auto &Ops = F.Blocks[0].Ops;
    MedVar Root = Ops[0].Output;
    for (int I = 0; I < Copies; ++I) {
      MedVar Next = temp(100 + I);
      Ops.push_back(op(NdOp::COPY, Next, {Root}));
      Root = Next;
    }
    Ops[2].Inputs[1] = Root;
    classify(F);
    EXPECT_EQ(F.IsVariadic, Copies < 63);
  }
  for (int Copies : {0, 33, 34}) {
    SCOPED_TRACE(Copies);
    MedFunc F = walkFunction();
    auto &Ops = F.Blocks[0].Ops;
    MedVar Root = Ops[1].Output;
    for (int I = 0; I < Copies; ++I) {
      MedVar Next = temp(100 + I);
      Ops.push_back(op(NdOp::COPY, Next, {Root}));
      Root = Next;
    }
    Ops[2].Inputs[0] = Root;
    classify(F);
    EXPECT_EQ(F.IsVariadic, Copies < 34);
  }
  MedFunc F = homeFunction(Arch::AArch64);
  auto &Ops = F.Blocks[0].Ops;
  const MedVar Cycle = temp(900);
  Ops.insert(Ops.begin(), op(NdOp::STORE, {}, {temp(901), Cycle}));
  Ops.push_back(op(NdOp::COPY, Cycle, {Cycle}));
  classify(F);
  EXPECT_TRUE(F.IsVariadic);
  EXPECT_EQ(F.VariadicOverflowBase, 16);
}

} // namespace
