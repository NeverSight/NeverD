//===- MedCallingConvFP.cpp - Floating/vector parameter byte demand -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover observed floating/vector input bytes across target conventions.

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/X86/X86VectorLaneReads.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallingConvDetail.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>

namespace neverd {

void detectFPParams(
    MedFunc &Func, const MedBlock &Entry, const TargetRegInfo &TRI,
    const std::map<std::pair<uint64_t, uint16_t>, int> &RegVarMap,
    Arch TargetArch, BinaryFormat TargetFormat) {
  // FP/vector arguments arrive in a dedicated vector register class under
  // clang's -O2 conventions: x86-64 XMM0-7, AArch64 V0-7, ARM VFP D0-7 — all
  // observed as live-in self-copies.
  if (TRI.VecRegCount == 0)
    return;
  // Only a leading entry self-copy of a floating argument register can become
  // a parameter (the loop at the end); without one, nothing is recovered.
  auto isFPArgSelfCopy = [&](const MedOp &Op) {
    return Op.Output.Kind == MedVar::Reg && Op.NumInputs >= 1 &&
           Op.Inputs[0].Id == Op.Output.Id &&
           TRI.isFPArgReg(Op.Output.RegOff, TargetFormat);
  };
  {
    bool AnyCandidate = false;
    for (const auto &Op : Entry.Ops) {
      if (Op.Opcode != NdOp::COPY)
        break;
      if (isFPArgSelfCopy(Op)) {
        AnyCandidate = true;
        break;
      }
    }
    if (!AnyCandidate)
      return;
  }

  // An entry self-copy alone does not prove a parameter: the lifter emits one
  // for every vector register the body touches, including those used purely as
  // scratch for an FP computation (e.g. an i386 `int` function whose body
  // builds a determinant in XMM0-7, each register first zeroed by `xorps x,x`).
  // A real FP parameter's incoming value reaches a genuine consumer.  A
  // scratch register's flows -- possibly carried through PHIs -- only into
  // self-cancelling idioms (`x^x`, `x-x` = 0) that discard it, or into lanes
  // that nothing reads: a scalar write such as `cvtsi2ss xmm2, eax` keeps the
  // upper lanes of xmm2, so the incoming value lives on there unobserved.  On
  // i386 every argument is stack-passed, so a phantom XMM parameter shifts
  // every real stack argument to a wrong offset; recover the register only
  // when an incoming byte is truly used.
  using med_calling_conv_detail::ValueKey;
  using med_calling_conv_detail::valueKey;
  struct ValueUse {
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
    const MedCallClobber *Clobber = nullptr;
  };
  llvm::DenseMap<ValueKey, llvm::SmallVector<ValueUse, 2>> Uses;
  llvm::DenseMap<ValueKey, const MedOp *> Defs;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis) {
      Defs[valueKey(Phi.Output)] = nullptr;
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        if (!Arg.isConst())
          Uses[valueKey(Arg)].push_back({nullptr, &Phi});
      }
    }
    for (const MedOp &Op : Block.Ops) {
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst())
          Uses[valueKey(Op.Inputs[I])].push_back({&Op, nullptr});
      const bool EntrySeed = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                             Op.Output.Kind == MedVar::Reg &&
                             Op.Output == Op.Inputs[0] &&
                             Op.Output.Size == Op.Inputs[0].Size &&
                             Op.Output.RegOff == Op.Inputs[0].RegOff;
      if (EntrySeed && &Block == &Entry)
        Defs[valueKey(Op.Output)] = nullptr;
      if (!EntrySeed &&
          (Op.Output.Kind == MedVar::Temp || Op.Output.Kind == MedVar::Reg)) {
        auto [It, Added] = Defs.try_emplace(valueKey(Op.Output), &Op);
        if (!Added)
          It->second = nullptr;
      }
      for (const MedVar &Output : Op.IntrinsicOutputs)
        Defs[valueKey(Output)] = nullptr;
    }
  }
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    Defs[valueKey(Clobber.Value)] = nullptr;
  // The amounts a shift count can take: a constant, a byte view of one, or
  // a selection of them (the lifter's count-at-least-the-width rule for
  // vector shifts).
  std::function<bool(const MedVar &, llvm::SmallVectorImpl<uint64_t> &,
                     unsigned)>
      shiftAmounts = [&](const MedVar &Count,
                         llvm::SmallVectorImpl<uint64_t> &Out, unsigned Depth) {
        const uint64_t Keep = Count.Size == 0 || Count.Size >= 8
                                  ? ~uint64_t{0}
                                  : (uint64_t{1} << (8 * Count.Size)) - 1;
        if (Count.isConst()) {
          Out.push_back(Count.ConstVal & Keep);
          return true;
        }
        const auto It = Defs.find(valueKey(Count));
        if (It == Defs.end() || !It->second ||
            Depth > ::neverd::limits::ir::
                        kMedCallingConvFPDetectFPParamsShiftAmountsDepthLimit)
          return false;
        const MedOp &Def = *It->second;
        if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT) &&
            Def.NumInputs == 1)
          return shiftAmounts(Def.Inputs[0], Out, Depth + 1);
        if (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs == 2 &&
            Def.Inputs[1].isConst() && Def.Inputs[1].ConstVal < 8) {
          llvm::SmallVector<uint64_t, 2> Whole;
          if (!shiftAmounts(Def.Inputs[0], Whole, Depth + 1))
            return false;
          for (const uint64_t Value : Whole)
            Out.push_back((Value >> (8 * Def.Inputs[1].ConstVal)) & Keep);
          return true;
        }
        return Def.Opcode == NdOp::SELECT && Def.NumInputs == 3 &&
               shiftAmounts(Def.Inputs[1], Out, Depth + 1) &&
               shiftAmounts(Def.Inputs[2], Out, Depth + 1);
      };

  // A vector mask can be a CONCAT of two constant halves, then stored in a
  // register whose first definition is version zero. Neither a missing nor an
  // ambiguous definition supplies a mask. Keep this query local and bounded.
  size_t ConstantWork = ::neverd::limits::ir::kMedCallingConvFPConstantWork;
  std::function<std::optional<llvm::APInt>(const MedVar &, unsigned)>
      constantBits =
          [&](const MedVar &V, unsigned Depth) -> std::optional<llvm::APInt> {
    if (!ConstantWork ||
        Depth > ::neverd::limits::ir::
                    kMedCallingConvFPDetectFPParamsConstantBitsDepthLimit ||
        !V.Size || V.Size > 64)
      return std::nullopt;
    --ConstantWork;
    const unsigned Width = V.Size * 8;
    if (V.isConst())
      return llvm::APInt(Width, V.ConstVal, /*isSigned=*/false,
                         /*implicitTrunc=*/true);
    const auto It = Defs.find(valueKey(V));
    if (It == Defs.end() || !It->second)
      return std::nullopt;
    const MedOp &Op = *It->second;
    if (Op.Output.Size != V.Size || Op.NumInputs > std::size(Op.Inputs) ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (V.Kind == MedVar::Reg && Op.Output.RegOff != V.RegOff))
      return std::nullopt;
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs == 1) {
      const auto Input = constantBits(Op.Inputs[0], Depth + 1);
      if (!Input || Input->getBitWidth() > Width ||
          (Op.Opcode == NdOp::COPY && Input->getBitWidth() != Width))
        return std::nullopt;
      return Op.Opcode == NdOp::INT_SEXT ? Input->sext(Width)
                                         : Input->zext(Width);
    }
    if (Op.Opcode == NdOp::CONCAT && Op.NumInputs == 2 &&
        Op.Inputs[0].Size + Op.Inputs[1].Size == V.Size) {
      const auto High = constantBits(Op.Inputs[0], Depth + 1);
      const auto Low = constantBits(Op.Inputs[1], Depth + 1);
      if (High && Low)
        return High->zext(Width).shl(Low->getBitWidth()) | Low->zext(Width);
    }
    if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal <= Op.Inputs[0].Size &&
        V.Size <= Op.Inputs[0].Size - Op.Inputs[1].ConstVal) {
      const auto Input = constantBits(Op.Inputs[0], Depth + 1);
      if (Input)
        return Input->lshr(Op.Inputs[1].ConstVal * 8).trunc(Width);
    }
    if ((Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR ||
         Op.Opcode == NdOp::INT_XOR) &&
        Op.NumInputs == 2 && Op.Inputs[0].Size == V.Size &&
        Op.Inputs[1].Size == V.Size) {
      const auto Left = constantBits(Op.Inputs[0], Depth + 1);
      const auto Right = constantBits(Op.Inputs[1], Depth + 1);
      if (Left && Right) {
        if (Op.Opcode == NdOp::INT_AND)
          return *Left & *Right;
        return Op.Opcode == NdOp::INT_OR ? *Left | *Right : *Left ^ *Right;
      }
    }
    return std::nullopt;
  };
  // SSA records the low prefix that the call ABI preserves as an implicit
  // value definition. Connect that exact prefix to its pre-call value, just
  // like an explicit byte copy; the volatile upper lanes supply no input use.
  std::map<uint32_t, const MedOp *> Calls;
  std::set<uint32_t> AmbiguousCalls;
  for (const auto &Block : Func.Blocks)
    for (const auto &Op : Block.Ops)
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.CallSiteId && !Calls.emplace(Op.CallSiteId, &Op).second)
        AmbiguousCalls.insert(Op.CallSiteId);
  for (const auto &Clobber : Func.CallClobbers) {
    const auto Owner = Calls.find(Clobber.CallSiteId);
    const auto &Output = Clobber.Value;
    const auto &Input = Clobber.PreservedInput;
    if (Owner == Calls.end() || AmbiguousCalls.count(Clobber.CallSiteId) ||
        Owner->second->DoesNotReturn || Owner->second->PreservesCallerSaved ||
        !Clobber.PreservedPrefixSize || Output.Kind != MedVar::Reg ||
        Input.Kind != MedVar::Reg || Output.Id != Input.Id ||
        Output.RegOff != Input.RegOff || Output.Size != Input.Size ||
        valueKey(Output) == valueKey(Input) || Output.Size > 64 ||
        Clobber.PreservedPrefixSize >= Output.Size ||
        Clobber.PreservedPrefixSize !=
            TRI.callPreservedPrefixSize(Output.RegOff, Output.Size,
                                        TargetFormat))
      continue;
    Uses[valueKey(Input)].push_back({nullptr, nullptr, &Clobber});
  }
  // A RETURN may read an FP return register.  i386 returns floating point in
  // x87 st0 and records through memory; XMM0 carries only a vector result
  // there, so a scalar lane merge that reaches XMM0 is not returned.
  auto isFPReturnReg = [&](uint64_t RegOff) {
    if (TRI.ReturnsFPInX87)
      return false;
    return (TRI.hasFPReturnReg() && RegOff == TRI.FPReturnReg) ||
           llvm::is_contained(TRI.FPReturnRegs, RegOff);
  };

  // Writing a return register is transport, too: NEON may interleave scratch
  // bytes into D0 and mask them before producing the result. Only a definition
  // that reaches RETURN can be an implicit return read. A value forwarded in
  // that register may also be read by CALL before ABI recovery adds operands.
  // Keep explicit body consumers in Uses, and retain conservative demand if
  // this query cannot establish the reaching definitions within its budget.
  llvm::DenseSet<ValueKey> PossibleFPReads;
  bool UnknownFPReads = false;
  size_t ReadWork = ::neverd::limits::ir::kMedCallingConvFPReadWork;
  llvm::SmallVector<uint64_t, 4> ReturnRegs;
  if (!TRI.ReturnsFPInX87) {
    ReturnRegs.append(TRI.FPReturnRegs.begin(), TRI.FPReturnRegs.end());
    if (TRI.hasFPReturnReg() &&
        !llvm::is_contained(ReturnRegs, TRI.FPReturnReg))
      ReturnRegs.push_back(TRI.FPReturnReg);
  }
  for (const MedBlock &Block : Func.Blocks) {
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto Opcode = Block.Ops[I].Opcode;
      const bool Call = Opcode == NdOp::CALL || Opcode == NdOp::INDIR_CALL;
      if (Opcode != NdOp::RETURN && !Call)
        continue;
      for (uint64_t RegOff : ReturnRegs) {
        if (Call && !TRI.isFPArgReg(RegOff, TargetFormat))
          continue;
        llvm::SmallVector<std::pair<int, size_t>, 8> Pending{{Block.Id, I}};
        std::set<std::pair<int, size_t>> Seen;
        while (!Pending.empty()) {
          const auto [Id, Before] = Pending.pop_back_val();
          if (!Seen.emplace(Id, Before).second)
            continue;
          if (!ReadWork || Id < 0 ||
              static_cast<size_t>(Id) >= Func.Blocks.size() ||
              Func.Blocks[Id].Id != Id || Before > Func.Blocks[Id].Ops.size()) {
            UnknownFPReads = true;
            break;
          }
          const auto &Predecessor = Func.Blocks[Id];
          bool Defined = false;
          for (size_t J = Before; J > 0; --J) {
            if (!ReadWork) {
              UnknownFPReads = true;
              break;
            }
            --ReadWork;
            const MedOp &Op = Predecessor.Ops[J - 1];
            if (Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
                Op.Output.RegOff == RegOff) {
              PossibleFPReads.insert(valueKey(Op.Output));
              Defined = true;
              break;
            }
            if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
                !Op.PreservesCallerSaved) {
              // FP result registers are volatile; a call ends this incoming
              // value's path to an implicit return read.
              Defined = true;
              break;
            }
          }
          if (Defined || UnknownFPReads)
            continue;
          for (const PhiNode &Phi : Predecessor.Phis)
            if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size &&
                Phi.Output.RegOff == RegOff) {
              PossibleFPReads.insert(valueKey(Phi.Output));
              Defined = true;
              break;
            }
          if (Defined)
            continue;
          if (!Predecessor.ExceptionalPreds.empty())
            UnknownFPReads = true;
          for (int Pred : Predecessor.Preds) {
            if (Pred < 0 || static_cast<size_t>(Pred) >= Func.Blocks.size()) {
              UnknownFPReads = true;
              continue;
            }
            Pending.emplace_back(Pred, Func.Blocks[Pred].Ops.size());
          }
        }
      }
    }
  }

  // The incoming bytes each value carries, one mask bit per byte.
  constexpr uint16_t MaskBytes = 64;
  auto byteMask = [](uint16_t Size) {
    return Size >= MaskBytes ? ~uint64_t{0} : (uint64_t{1} << Size) - 1;
  };
  // Whether a genuine consumer reads one of the incoming bytes \p Initial
  // selects of \p LiveIn.
  auto liveInBytesUsed = [&](const MedVar &LiveIn, uint64_t Initial) {
    llvm::DenseMap<ValueKey, uint64_t> Carried;
    llvm::SmallVector<ValueKey, 16> Work;
    auto carried = [&](const MedVar &V) -> uint64_t {
      if (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp)
        return 0;
      auto It = Carried.find(valueKey(V));
      return It == Carried.end() ? 0 : It->second;
    };
    auto record = [&](const MedVar &Output, uint64_t Mask) {
      uint64_t &Known = Carried[valueKey(Output)];
      if ((Mask & ~Known) == 0)
        return;
      Known |= Mask;
      Work.push_back(valueKey(Output));
    };
    // Carry incoming bytes into \p Output.  Returns false when that already
    // observes one: a reinterpret into a general-purpose register (`fmov w0,
    // s0` / `movd eax, xmm0`) materializes the value's bits in the integer
    // domain, and a RETURN may read an FP return register.
    auto carry = [&](const MedVar &Output, uint64_t Mask) {
      if (Output.Kind != MedVar::Reg && Output.Kind != MedVar::Temp)
        return true;
      if (Output.Size == 0 || Output.Size > MaskBytes)
        return Mask == 0;
      Mask &= byteMask(Output.Size);
      if (Mask == 0)
        return true;
      if (Output.Kind == MedVar::Reg &&
          (!TRI.isVectorReg(Output.RegOff) ||
           PossibleFPReads.contains(valueKey(Output)) ||
           (UnknownFPReads && isFPReturnReg(Output.RegOff))))
        return false;
      record(Output, Mask);
      return true;
    };
    auto isSelfCopy = [](const MedOp &Op) {
      return Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
             Op.Output.Kind == MedVar::Reg && Op.Inputs[0].Id == Op.Output.Id;
    };
    // The values whose low lane a scalar operation wrote without incoming
    // bytes, and its width.  A scalar write into a return register keeps
    // the incoming upper lanes beside the value it computes; a scalar
    // return reads only that value.  Only a vector return would read the
    // kept lanes, and like other decompilers the scalar reading is the one
    // recovered.
    llvm::DenseMap<ValueKey, uint16_t> FreshLow;
    auto keepsLanesOnly = [&](const MedVar &Output, uint64_t Mask,
                              uint16_t Fresh) {
      if (!Fresh || (Mask & byteMask(Fresh)) || Output.Kind != MedVar::Reg ||
          !TRI.isVectorReg(Output.RegOff) || !isFPReturnReg(Output.RegOff))
        return false;
      FreshLow[valueKey(Output)] = Fresh;
      record(Output, Mask & byteMask(Output.Size));
      return true;
    };
    // Whether \p Op observes an incoming byte; otherwise carry them on.
    auto observes = [&](const MedOp &Op) {
      llvm::SmallVector<uint64_t, 4> In(Op.NumInputs, 0);
      uint64_t Any = 0;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        Any |= In[I] = carried(Op.Inputs[I]);
      if (Any == 0)
        return false;
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT: {
        if (Op.NumInputs != 1 ||
            (Op.Opcode == NdOp::INT_ZEXT && Op.Output.Size < Op.Inputs[0].Size))
          return true;
        if (isSelfCopy(Op)) {
          record(Op.Output, In[0] & byteMask(Op.Output.Size));
          return false;
        }
        // A vector register widened from its own low bytes (`vzeroupper`,
        // a VEX upper clear) keeps its value in place, which hands a caller
        // back nothing new, as a register the code leaves alone does not.
        // Only the incoming register's own bytes stay in place: bytes moved
        // in from another register (`fmov d0, d1`, which widens d0) are
        // handed back as any other write of a return register is.
        if (Op.Opcode == NdOp::INT_ZEXT && Op.Output.Kind == MedVar::Reg &&
            Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Output.RegOff == Op.Inputs[0].RegOff &&
            Op.Output.RegOff == LiveIn.RegOff &&
            TRI.isVectorReg(Op.Output.RegOff) && Op.Output.Size <= MaskBytes) {
          record(Op.Output, In[0] & byteMask(Op.Inputs[0].Size));
          return false;
        }
        const auto Fresh = FreshLow.find(valueKey(Op.Inputs[0]));
        if (Fresh != FreshLow.end() &&
            keepsLanesOnly(Op.Output, In[0], Fresh->second))
          return false;
        return !carry(Op.Output, In[0]);
      }
      case NdOp::INT_SEXT: {
        const uint16_t InSize = Op.Inputs[0].Size;
        if (Op.NumInputs != 1 || InSize == 0 || InSize > MaskBytes)
          return true;
        // The sign byte fills every widened byte.
        uint64_t Mask = In[0];
        if ((Mask >> (InSize - 1)) & 1)
          Mask |= ~byteMask(InSize);
        return !carry(Op.Output, Mask);
      }
      case NdOp::SUBBYTES: {
        if (Op.NumInputs != 2 || !Op.Inputs[1].isConst() || In[1] != 0)
          return true;
        const uint64_t Offset = Op.Inputs[1].ConstVal;
        return !carry(Op.Output, Offset >= MaskBytes ? 0 : In[0] >> Offset);
      }
      case NdOp::CONCAT: {
        const uint16_t LowSize = Op.NumInputs == 2 ? Op.Inputs[1].Size : 0;
        if (Op.NumInputs != 2 || LowSize == 0 ||
            (LowSize >= MaskBytes && In[0] != 0))
          return true;
        const uint64_t High = LowSize >= MaskBytes ? 0 : In[0] << LowSize;
        const uint16_t Fresh = In[1] == 0 ? LowSize : 0;
        if (Fresh)
          FreshLow[valueKey(Op.Output)] = Fresh;
        else
          FreshLow.erase(valueKey(Op.Output));
        if (keepsLanesOnly(Op.Output, High, Fresh))
          return false;
        return !carry(Op.Output, High | In[1]);
      }
      case NdOp::INT_XOR:
      case NdOp::INT_SUB:
        // `x ^ x` / `x - x`: the value is discarded, not consumed.
        return Op.NumInputs != 2 || !(Op.Inputs[0] == Op.Inputs[1]);
      case NdOp::INT_AND:
      case NdOp::INT_OR: {
        if (Op.NumInputs != 2 || !Op.Output.Size ||
            Op.Inputs[0].Size != Op.Output.Size ||
            Op.Inputs[1].Size != Op.Output.Size)
          return true;
        for (unsigned I = 0; I != 2; ++I) {
          if (In[I])
            continue;
          const auto Constant = constantBits(Op.Inputs[I], 0);
          if (!Constant)
            continue;
          uint64_t Kept = 0;
          for (unsigned Byte = 0; Byte < Op.Output.Size && Byte < MaskBytes;
               ++Byte) {
            const auto Bits = Constant->extractBitsAsZExtValue(8, Byte * 8);
            if (Op.Opcode == NdOp::INT_AND ? Bits != 0 : Bits != 0xff)
              Kept |= uint64_t{1} << Byte;
          }
          return !carry(Op.Output, In[1 - I] & Kept);
        }
        return true;
      }
      case NdOp::INT_LEFT:
      case NdOp::INT_RIGHT:
      case NdOp::INT_ASHR: {
        // A shift by known amounts only moves bytes, and drops those it
        // moves out: `psrad $16` after `punpckhwd` keeps none of the words
        // taken from the destination's incoming value.
        const uint16_t Size = Op.NumInputs == 2 ? Op.Inputs[0].Size : 0;
        llvm::SmallVector<uint64_t, 2> Amounts;
        if (Size == 0 || Size > 8 || In[1] != 0 ||
            !shiftAmounts(Op.Inputs[1], Amounts, 0))
          return true;
        const int64_t Bits = 8 * Size;
        uint64_t Mask = 0;
        for (const uint64_t Amount : Amounts)
          for (int64_t Byte = 0; Byte < Size; ++Byte) {
            // The input bits output byte Byte takes, and whether it also
            // takes the sign bit an arithmetic shift fills in.
            const int64_t Shift = static_cast<int64_t>(
                std::min<uint64_t>(Amount, static_cast<uint64_t>(Bits)));
            int64_t Low = 8 * Byte - Shift, High = 8 * Byte + 7 - Shift;
            bool Sign = false;
            if (Op.Opcode != NdOp::INT_LEFT) {
              Low = 8 * Byte + Shift;
              High = 8 * Byte + 7 + Shift;
              Sign = Op.Opcode == NdOp::INT_ASHR && High >= Bits;
            }
            Low = std::max<int64_t>(Low, 0);
            High = std::min<int64_t>(High, Bits - 1);
            uint64_t From = Sign ? uint64_t{1} << (Size - 1) : 0;
            for (int64_t Bit = Low; Bit <= High; Bit += 8)
              From |= uint64_t{1} << (Bit / 8);
            if (Low <= High)
              From |= uint64_t{1} << (High / 8);
            if (In[0] & From)
              Mask |= uint64_t{1} << Byte;
          }
        return !carry(Op.Output, Mask);
      }
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
        // A floating argument passes only the bytes the callee reads.
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          if (!In[I])
            continue;
          const uint16_t Width = Op.vectorArgumentWidth(I);
          if (!Width || (In[I] & byteMask(Width)))
            return true;
        }
        return false;
      case NdOp::INTRINSIC: {
        // An intrinsic that reads only some bytes of its vector operands
        // (`unpcklpd x, x` after `cvtsi2sd`, which kept x's incoming upper
        // lane) observes none of the rest, and its result holds none.
        if (Op.NumInputs < 2 || !Op.Inputs[0].isConst() || In[0] != 0)
          return true;
        const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
        for (uint8_t I = 1; I < Op.NumInputs; ++I) {
          if (!In[I])
            continue;
          const auto Read = x86VectorOperandReadBytes(Id, Op.Inputs[I].Size);
          if (!Read || (In[I] & *Read))
            return true;
        }
        return false;
      }
      default:
        return true; // a genuine consumer of an incoming byte
      }
    };

    record(LiveIn, Initial & byteMask(LiveIn.Size));
    while (!Work.empty()) {
      const ValueKey Key = Work.pop_back_val();
      auto It = Uses.find(Key);
      if (It == Uses.end())
        continue;
      for (const ValueUse &Use : It->second) {
        if (Use.Clobber) {
          const auto &Clobber = *Use.Clobber;
          if (!carry(Clobber.Value, carried(Clobber.PreservedInput) &
                                        byteMask(Clobber.PreservedPrefixSize)))
            return true;
          continue;
        }
        if (Use.Op) {
          if (observes(*Use.Op))
            return true;
          continue;
        }
        uint64_t Mask = 0;
        for (const auto &[Pred, Arg] : Use.Phi->Args) {
          (void)Pred;
          Mask |= carried(Arg);
        }
        if (!carry(Use.Phi->Output, Mask))
          return true;
      }
    }
    return false;
  };
  auto liveInValueUsed = [&](const MedVar &LiveIn) {
    return liveInBytesUsed(LiveIn, byteMask(LiveIn.Size));
  };
  auto isSelfCopyOf = [](const MedOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
           Op.Output.Kind == MedVar::Reg && Op.Inputs[0].Id == Op.Output.Id;
  };
  // The width of the scalar a used parameter register carries: its readers
  // take only the low 4 or 8 bytes, directly or through whole copies of the
  // register, and no incoming byte above them reaches a consumer.  A merge
  // or any other read leaves it a vector.  A live-in that is itself a 4- or
  // 8-byte view of its register (an AArch64 S or D register) has no other
  // bytes: whatever reads it, floating-point arithmetic included, reads a
  // scalar of that width, which only a narrower view at its start narrows.
  auto scalarLaneBytes = [&](const MedVar &LiveIn) -> uint16_t {
    const bool ScalarView = LiveIn.Size == 4 || LiveIn.Size == 8;
    uint16_t Width = 0;
    llvm::SmallVector<MedVar, 4> Aliases{LiveIn};
    std::set<ValueKey> Seen{valueKey(LiveIn)};
    while (!Aliases.empty()) {
      const MedVar Alias = Aliases.pop_back_val();
      const auto It = Uses.find(valueKey(Alias));
      if (It == Uses.end())
        continue;
      for (const ValueUse &Use : It->second) {
        if (!Use.Op) {
          if (!ScalarView)
            return 0;
          continue;
        }
        const MedOp &Op = *Use.Op;
        if (isSelfCopyOf(Op))
          continue;
        // A call passes the register on as a floating argument of the
        // width its callee reads.
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            if (Op.Inputs[I] == Alias) {
              const uint16_t Read = Op.vectorArgumentWidth(I);
              if (Read == 4 || Read == 8)
                Width = std::max(Width, Read);
              else if (!ScalarView)
                return 0;
            }
          continue;
        }
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Output.Size == LiveIn.Size &&
            (Op.Output.Kind == MedVar::Temp ||
             (Op.Output.Kind == MedVar::Reg &&
              TRI.isVectorReg(Op.Output.RegOff)))) {
          if (Seen.insert(valueKey(Op.Output)).second)
            Aliases.push_back(Op.Output);
          continue;
        }
        if (Op.Opcode != NdOp::SUBBYTES || Op.NumInputs != 2 ||
            !Op.Inputs[1].isConst() || !(Op.Inputs[0] == Alias)) {
          if (!ScalarView)
            return 0;
          continue;
        }
        if (Op.Inputs[1].ConstVal == 0) {
          if (Op.Output.Size != 4 && Op.Output.Size != 8)
            return 0;
          Width = std::max(Width, Op.Output.Size);
        }
      }
    }
    // A live-in read only as a scalar view (the argument a call passes
    // on) is a scalar of that view's width.
    if (ScalarView)
      return Width ? std::min<uint16_t>(Width, LiveIn.Size) : LiveIn.Size;
    if (!Width || Width >= LiveIn.Size ||
        liveInBytesUsed(LiveIn, byteMask(LiveIn.Size) & ~byteMask(Width)))
      return 0;
    return Width;
  };

  std::set<uint64_t> AlreadyParam;
  for (const auto &P : Func.Params)
    AlreadyParam.insert(P.RegOff);

  std::vector<MedVar> FPParams;
  for (const auto &Op : Entry.Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (!isFPArgSelfCopy(Op))
      continue;

    uint64_t ROff = Op.Output.RegOff;
    // A positional convention (Win64) finds its floating arguments among
    // the argument slots; each still carries a scalar or a vector.
    if (AlreadyParam.count(ROff)) {
      if (!Func.FPParamScalarBytes.count(ROff))
        if (const uint16_t Scalar = scalarLaneBytes(Op.Output))
          Func.FPParamScalarBytes[ROff] = Scalar;
      continue;
    }
    // Skip a scratch vector register whose incoming value is never read.
    if (!liveInValueUsed(Op.Output))
      continue;

    // Prefer the var recorded at the self-copy's own width: an ARM `float` arg
    // in s0 and a `double` arg in d0 share register offset 0x100, so the access
    // width (4 vs 8) selects the right one.
    const uint16_t WantSz = Op.Output.Size;
    int FoundId = -1;
    uint16_t FoundSz = 0;
    for (const auto &[RK, VId] : RegVarMap) {
      if (RK.first != ROff)
        continue;
      if (RK.second == WantSz) {
        FoundId = VId;
        FoundSz = RK.second;
        break;
      }
      if (FoundId < 0) {
        FoundId = VId;
        FoundSz = RK.second;
      }
    }
    if (FoundId >= 0) {
      MedVar Param;
      Param.Kind = MedVar::Param;
      Param.Id = FoundId;
      Param.Size = FoundSz;
      Param.RegOff = ROff;
      Param.TheArch = TargetArch;
      FPParams.push_back(Param);
      AlreadyParam.insert(ROff);
      if (const uint16_t Scalar = scalarLaneBytes(Op.Output))
        Func.FPParamScalarBytes[ROff] = Scalar;
    }
  }

  // FP arguments occupy the vector/FP registers in increasing register order
  // (XMM0,XMM1,.. / D0,D1,.. / S0,S1,..), so order the recovered FP parameters
  // by register offset to match the ABI sequence regardless of the order the
  // live-in self-copies happen to appear in the entry block.
  std::sort(
      FPParams.begin(), FPParams.end(),
      [](const MedVar &A, const MedVar &B) { return A.RegOff < B.RegOff; });
  for (auto &P : FPParams)
    Func.Params.push_back(P);
}

} // namespace neverd
