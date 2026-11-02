//===- MedVariadicTests.cpp - Variadic lookup boundaries ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

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

// A System V va_start stores gp_offset, eight for each integer register that
// carries a named parameter, into its va_list's first field: that store
// alone counts them (MedFunc::VaStartNamedGPRs), the word whole or its two
// halves side by side.  The word in a register no store puts in the frame,
// or stored through an address that is no frame slot, counts nothing.
TEST(MedVariadic, X64NamedRegistersComeOnlyFromAStoredGpOffset) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const uint64_t Word = (uint64_t{48} << 32) | 16;
  const auto Variadic = [&](auto Edit) {
    MedFunc F = homeFunction(Arch::X64);
    auto &Ops = F.Blocks[0].Ops;
    Ops.push_back(op(NdOp::STORE, {},
                     {Ops[1].Output, reg(20, 0, 8, TRI.IntParamRegs[2])}));
    Edit(Ops);
    classify(F, Arch::X64);
    EXPECT_TRUE(F.IsVariadic);
    return F.VaStartNamedGPRs;
  };
  EXPECT_EQ(Variadic([&](std::vector<MedOp> &Ops) {
              Ops.push_back(op(NdOp::STORE, {},
                               {Ops[1].Output, MedVar::makeConst(Word, 8)}));
            }),
            2);
  EXPECT_EQ(
      Variadic([&](std::vector<MedOp> &Ops) {
        const MedVar FpField = temp(4);
        Ops.push_back(
            op(NdOp::STORE, {}, {Ops[1].Output, MedVar::makeConst(16, 4)}));
        Ops.push_back(op(NdOp::INT_ADD, FpField,
                         {Ops[1].Inputs[0],
                          MedVar::makeConst(static_cast<uint64_t>(-12), 8)}));
        Ops.push_back(op(NdOp::STORE, {}, {FpField, MedVar::makeConst(48, 4)}));
      }),
      2);
  EXPECT_EQ(Variadic([&](std::vector<MedOp> &Ops) {
              Ops.push_back(op(NdOp::COPY,
                               reg(21, 1, 8, TRI.IntReturnRegs.front()),
                               {MedVar::makeConst(Word, 8)}));
            }),
            -1);
  EXPECT_EQ(Variadic([&](std::vector<MedOp> &Ops) {
              Ops.push_back(op(NdOp::STORE, {},
                               {reg(22, 0, 8, TRI.IntParamRegs[0]),
                                MedVar::makeConst(Word, 8)}));
            }),
            -1);
}

TEST(MedVariadic, I386RequiresAHomedPointerAndAReloadOrAdvancedRead) {
  for (int Mode = 0; Mode < 10; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc F = homeFunction(Arch::X86);
    auto &Ops = F.Blocks[0].Ops;
    const MedVar Value = Ops[0].Output;
    const MedVar Advanced = temp(4, 4);
    if (Mode == 1) {
      Ops.erase(Ops.begin() + 2); // A reload alone supplies no va_list home.
    } else if (Mode == 2) {
      Ops.pop_back(); // A home alone supplies no va_arg use.
    } else if (Mode == 3) {
      Ops[3].Inputs[0] = Value; // A direct read must follow an advance.
    } else if (Mode >= 4) {
      Ops[3].Inputs[0] = Mode == 5 ? temp(99, 4) : Advanced;
      Ops.push_back(
          op(NdOp::INT_ADD, Advanced,
             {Value, Mode == 7 ? temp(98, 4) : MedVar::makeConst(4, 4)}));
      if (Mode == 6)
        Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mode == 8)
        Ops[3].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      if (Mode == 9) {
        const MedVar Joined = temp(5, 4);
        const MedVar Backedge = temp(6, 4);
        F.Blocks[0].Phis.push_back({Joined, {{0, Advanced}, {1, Backedge}}});
        Ops.push_back(op(NdOp::COPY, Backedge, {Joined}));
        Ops[3].Inputs[0] = Joined;
      }
    }
    classify(F, Arch::X86);
    EXPECT_EQ(F.IsVariadic, Mode == 0 || Mode == 4 || Mode == 9);
    if (F.IsVariadic)
      EXPECT_EQ(F.VariadicOverflowBase, 8);
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

// What clang -O2 emits for `int f(int n, ...)` whose va_arg reads ints only:
// x1-x7 and q0-q7 saved at entry SP - 0x58 and - 0xE0, and one va_list at
// entry SP - 0x20 whose walk loads __stack, __gr_top and __gr_offs.
struct AAPCS64VaStart {
  static constexpr int64_t Frame = 0xE0;
  static constexpr int64_t VaList = 0xC0; // from the lowered SP
  MedFunc F;
  MedVar SP, Lowered;
  int Next = 1000;
  bool Built = false;

  AAPCS64VaStart() {
    const auto &TRI = getTargetRegInfo(Arch::AArch64);
    SP = reg(100, 0, 8, TRI.StackPointer);
    Lowered = reg(100, 1, 8, TRI.StackPointer);
    F.Blocks.resize(1);
    F.Blocks[0].Id = 0;
    add(op(NdOp::INT_SUB, Lowered, {SP, MedVar::makeConst(Frame, 8)}));
    for (size_t K = 1; K < TRI.IntParamRegs.size(); ++K)
      add(op(NdOp::STORE, {},
             {at(0x88 + 8 * static_cast<int64_t>(K - 1)),
              reg(10 + static_cast<int>(K), 0, 8, TRI.IntParamRegs[K])}));
    for (size_t K = 0; K < TRI.FPParamRegs.size(); ++K)
      add(op(NdOp::STORE, {},
             {at(16 * static_cast<int64_t>(K)),
              reg(30 + static_cast<int>(K), 0, 16, TRI.FPParamRegs[K])}));
    add(op(NdOp::STORE, {}, {at(VaList), at(Frame)}));
    add(op(NdOp::STORE, {}, {at(VaList + 8), at(VaList)}));
    add(op(NdOp::STORE, {}, {at(VaList + 16), at(0x80)}));
    // MOVK builds both offsets, -56 and -128, from an AND and an OR.
    const MedVar Low = reg(8, 1, 8, TRI.indirectResultReg());
    const MedVar Masked = temp(Next++);
    const MedVar Offsets = reg(8, 2, 8, TRI.indirectResultReg());
    add(op(NdOp::COPY, Low, {MedVar::makeConst(0xFFFFFFFFFFFFFFC8, 8)}));
    add(op(NdOp::INT_AND, Masked,
           {Low, MedVar::makeConst(0xFFFF0000FFFFFFFF, 8)}));
    add(op(NdOp::INT_OR, Offsets,
           {Masked, MedVar::makeConst(0xFF8000000000, 8)}));
    add(op(NdOp::STORE, {}, {at(VaList + 24), Offsets}));
    // va_arg: __gr_top + __gr_offs, or __stack.
    const MedVar Top = temp(Next++), Offset = temp(Next++, 4);
    const MedVar Wide = temp(Next++), Arg = temp(Next++),
                 Value = temp(Next++, 4);
    add(op(NdOp::LOAD, temp(Next++), {at(VaList)}));
    add(op(NdOp::LOAD, Top, {at(VaList + 8)}));
    add(op(NdOp::LOAD, Offset, {at(VaList + 24)}));
    add(op(NdOp::INT_SEXT, Wide, {Offset}));
    add(op(NdOp::INT_ADD, Arg, {Top, Wide}));
    add(op(NdOp::LOAD, Value, {Arg}));
    add(op(NdOp::RETURN, {}, {}));
    Built = true;
  }

  /// Append \p Op, or insert it before the RETURN once that is in place.
  void add(MedOp Op) {
    auto &Ops = F.Blocks[0].Ops;
    Ops.insert(Built ? Ops.end() - 1 : Ops.end(), std::move(Op));
  }
  /// A new temporary holding the lowered SP plus \p Offset.
  MedVar at(int64_t Offset) {
    const MedVar Address = temp(Next++);
    add(op(NdOp::INT_ADD, Address,
           {Lowered, MedVar::makeConst(static_cast<uint64_t>(Offset), 8)}));
    return Address;
  }
  /// The STORE to the lowered SP plus \p Offset.
  MedOp &storeAt(int64_t Offset) {
    auto &Ops = F.Blocks[0].Ops;
    for (MedOp &Op : Ops)
      if (Op.Opcode == NdOp::STORE)
        for (const MedOp &Address : Ops)
          if (Address.Opcode == NdOp::INT_ADD &&
              Address.Output == Op.Inputs[0] && Address.Inputs[0] == Lowered &&
              Address.Inputs[1].ConstVal == static_cast<uint64_t>(Offset))
            return Op;
    ADD_FAILURE() << "no store at " << Offset;
    return Ops.front();
  }

  size_t vectorSpills() const {
    const auto &TRI = getTargetRegInfo(Arch::AArch64);
    size_t Count = 0;
    for (const MedOp &Op : F.Blocks[0].Ops)
      Count += Op.Opcode == NdOp::STORE && TRI.isFPArgReg(Op.Inputs[1].RegOff);
    return Count;
  }
};

TEST(MedVariadic, AAPCS64DropsTheVectorSaveAreaNoReadObserves) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const MedVar X0 = reg(1, 5, 8, TRI.IntParamRegs[0]);
  const MedVar Q1 = reg(51, 5, 16, TRI.FPParamRegs[1]);
  for (int Mode = 0; Mode < 22; ++Mode) {
    SCOPED_TRACE(Mode);
    AAPCS64VaStart V;
    BinaryFormat Format = BinaryFormat::ELF;
    switch (Mode) {
    case 1: // FP va_arg reads __vr_top and __vr_offs.
      V.add(op(NdOp::LOAD, temp(1), {V.at(V.VaList + 16)}));
      break;
    case 2:
      V.add(op(NdOp::LOAD, temp(1, 4), {V.at(V.VaList + 28)}));
      break;
    case 3: // A read of a saved vector register.
      V.add(op(NdOp::LOAD, temp(1), {V.at(0x18)}));
      break;
    case 4: // `vprintf(fmt, ap)` passes the va_list on.
      V.add(op(NdOp::COPY, X0, {V.at(V.VaList)}));
      V.add(op(NdOp::CALL, {}, {MedVar::makeConst(0x4000, 8)}));
      break;
    case 5: // A frame address stored where the proof does not follow it.
      V.add(op(NdOp::STORE, {}, {MedVar::makeConst(0x8000, 8), V.at(0)}));
      break;
    case 6: { // No __vr_top: nothing ties the area to a va_list.
      const MedVar Inside = V.at(0x70);
      V.storeAt(V.VaList + 16).Inputs[1] = Inside;
      break;
    }
    case 7: // __vr_offs for seven saved registers.
      for (MedOp &Op : V.F.Blocks[0].Ops)
        if (Op.Opcode == NdOp::INT_OR)
          Op.Inputs[1] = MedVar::makeConst(0xFF9000000000, 8);
      break;
    case 8: // q3 is missing from the area.
      V.storeAt(0x30).Inputs[1] = temp(1, 16);
      break;
    case 9: { // A load at a frame address with no fixed offset.
      const MedVar Indexed = temp(2);
      V.add(op(NdOp::INT_ADD, Indexed,
               {V.at(0x88), reg(12, 0, 8, TRI.IntParamRegs[2])}));
      V.add(op(NdOp::LOAD, temp(1, 4), {Indexed}));
      break;
    }
    case 10: // Apple arm64 passes every variadic argument on the stack.
      Format = BinaryFormat::MachO;
      break;
    case 11: // A frame address in a vector register.
      V.add(op(NdOp::COPY, Q1, {V.at(V.VaList)}));
      break;
    case 12: // The va_list's address reaches the return in x0.
      V.add(op(NdOp::COPY, X0, {V.at(V.VaList)}));
      break;
    case 13: // ... or is overwritten first.
      V.add(op(NdOp::COPY, X0, {V.at(V.VaList)}));
      V.add(op(NdOp::COPY, reg(1, 6, 8, TRI.IntParamRegs[0]),
               {MedVar::makeConst(0, 8)}));
      V.add(op(NdOp::CALL, {}, {MedVar::makeConst(0x4000, 8)}));
      break;
    case 14: // An intrinsic may access what it is given.
      V.add(op(NdOp::INTRINSIC, temp(1),
               {MedVar::makeConst(1, 4), V.at(V.VaList)}));
      break;
    case 15: // The exception dispatcher enters with what no edge shows.
      V.F.Blocks[0].ExceptionalPreds.emplace_back(1, 0x2000);
      break;
    case 16: // A narrow write leaves the address's upper bytes in x0.
      V.add(op(NdOp::COPY, X0, {V.at(V.VaList)}));
      V.add(op(NdOp::COPY, reg(2, 6, 4, TRI.IntParamRegs[0]),
               {MedVar::makeConst(0, 4)}));
      V.add(op(NdOp::CALL, {}, {MedVar::makeConst(0x4000, 8)}));
      break;
    case 17: // Windows ARM64: the save area decides, not the format.
      Format = BinaryFormat::COFF;
      break;
    case 18: // q7 saved twice.
      V.add(op(NdOp::STORE, {},
               {V.at(0x100), reg(37, 0, 16, TRI.FPParamRegs[7])}));
      break;
    case 19: // A load next to __vr_top reads __gr_top only.
      V.add(op(NdOp::LOAD, temp(1), {V.at(V.VaList + 8)}));
      break;
    case 20: // __stack later pointing into the area would bring it back.
      V.add(op(NdOp::STORE, {}, {V.at(V.VaList), V.at(0x10)}));
      break;
    case 21: // So would another __vr_top.
      V.add(op(NdOp::STORE, {}, {V.at(V.VaList + 16), V.at(0x20)}));
      break;
    }
    V.F.IsVariadic = false;
    detectVariadic(V.F, TRI, Arch::AArch64, Format);
    EXPECT_TRUE(V.F.IsVariadic);
    const bool Dropped = Mode == 0 || Mode == 13 || Mode == 17 || Mode == 19;
    EXPECT_EQ(V.vectorSpills(),
              Dropped ? 0u
                      : TRI.FPParamRegs.size() - (Mode == 8) + (Mode == 18));
    if (Mode != 10)
      EXPECT_EQ(V.F.VariadicFirstRegister, 1);
  }
}

} // namespace
