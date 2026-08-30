//===- LowToMedStack.cpp - LowIR stack analysis --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stack-slot analysis for the LowIR to MedIR conversion.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/LowToMed.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace neverd {

void LowToMedConverter::analyzeStack(const LowFunc &Low) {
  std::set<std::pair<int64_t, uint16_t>> SeenSlots;

  const auto &TRI = getTargetRegInfo(TargetArch);
  auto IsFrameReg = [&TRI](const NdVar &VN) -> bool {
    if (!VN.isReg())
      return false;
    return TRI.isFrameReg(VN.Offset);
  };

  auto AddSlot = [&](int64_t Disp, uint16_t Sz) {
    if (SeenSlots.insert({Disp, Sz}).second) {
      StackSlot Slot;
      Slot.Offset = Disp;
      Slot.Size = Sz;
      Slot.VarId = allocVarId();
      StackSlots.push_back(Slot);
    }
  };

  // Pass 0: collect the instruction address and operand pairs of flag-setting
  // add/sub (adds/subs/cmp/cmn).  The lifter models these as an
  // INT_ADD/INT_SUB for the value plus carry/overflow flag ops
  // (INT_CARRY/INT_SOVF/INT_SBOR).  Operand storage is reused between machine
  // instructions, so both the address and pair must match.  A matching op is a
  // comparison whose result is never a frame address, so it must not seed a
  // stack slot — otherwise a register that once held sp and was reused for
  // data (flow-insensitive AddrMap never
  // invalidates) makes `cmp rN,#imm` (subs tmp, rN, #imm) look like a frame
  // access at offset -imm and inflates FrameSize past the real stack (#387:
  // ipcksum's `while(sum>>16)`
  // -> subs tmp, sum, #0x10000 -> a bogus 0x10090-byte frame ->
  // WRITE_UNMAPPED).
  auto VnKey = [](const NdVar &VN) {
    return std::make_pair(VN.Space, VN.Offset);
  };
  using VnStorage = std::pair<VnodeSpace, uint64_t>;
  std::set<std::tuple<va_t, VnStorage, VnStorage>> FlagArithPairs;
  for (const auto &Blk : Low.Blocks)
    for (const auto &Op : Blk.Ops)
      if ((Op.Opcode == NdOp::INT_CARRY || Op.Opcode == NdOp::INT_SOVF ||
           Op.Opcode == NdOp::INT_SBOR) &&
          Op.NumInputs >= 2) {
        auto A = VnKey(Op.Inputs[0]);
        auto B = VnKey(Op.Inputs[1]);
        FlagArithPairs.insert({Op.Addr, A, B});
        FlagArithPairs.insert({Op.Addr, B, A});
      }

  // Pass 1: track which temp/register operands are frame_reg +/- const.  A
  // definition kills an old association unless its opcode is one of the
  // address-preserving forms handled below.  Operand storage is routinely
  // reused for unrelated data, so retaining an association across an
  // arbitrary redef is unsound even when the resulting phantom displacement
  // happens to be below kMaxFrameSize.
  std::map<std::pair<VnodeSpace, uint64_t>, int64_t> AddrMap;
  std::set<std::pair<VnodeSpace, uint64_t>> FrameDefsInBlock;
  auto IsTrackableOutput = [](const LowOp &Op) {
    return Op.Output.Size > 0 && (Op.Output.isTemp() || Op.Output.isReg());
  };
  auto ClearOutput = [&](const LowOp &Op) {
    if (IsTrackableOutput(Op)) {
      AddrMap.erase(VnKey(Op.Output));
      FrameDefsInBlock.erase(VnKey(Op.Output));
    }
  };
  auto FrameOffset = [&](const NdVar &VN, int64_t &Offset) {
    auto It = AddrMap.find(VnKey(VN));
    if (It != AddrMap.end()) {
      Offset = It->second;
      return true;
    }
    if (IsFrameReg(VN)) {
      Offset = 0;
      return true;
    }
    return false;
  };

  for (const auto &Blk : Low.Blocks) {
    FrameDefsInBlock.clear();
    for (const auto &Op : Blk.Ops) {
      // Refine a frame-derived address at its actual use site.  Address
      // operands are scratch storage and may be redefined later in the same
      // function, so a post-pass lookup in the final AddrMap state loses the
      // data width (or, worse, attributes it to the wrong displacement).
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.NumInputs >= 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        int64_t Offset = 0;
        if (FrameOffset(Op.Inputs[0], Offset)) {
          uint16_t DataSz = 0;
          if (Op.Opcode == NdOp::LOAD)
            DataSz = Op.Output.Size;
          else if (Op.NumInputs >= 2)
            DataSz = Op.Inputs[1].Size;
          if (DataSz > 0)
            AddSlot(Offset, DataSz);
        }
      }

      if (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) {
        // A flag-setting compare (matching carry/overflow flag ops on the same
        // operands) is a comparison value, never a stack address, so it must
        // not seed a slot.  Clearing its output is essential when the
        // comparison reuses an operand slot that previously held a
        // frame-derived value.
        if (Op.NumInputs >= 2 &&
            FlagArithPairs.count(
                {Op.Addr, VnKey(Op.Inputs[0]), VnKey(Op.Inputs[1])})) {
          ClearOutput(Op);
          continue;
        }

        bool Propagated = false;
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          // `constant - frame` is not an address derived from the frame.
          if (Op.Opcode == NdOp::INT_SUB && I != 0)
            continue;

          uint8_t Other = 1 - I;
          if (Other >= Op.NumInputs || !Op.Inputs[Other].isConst())
            continue;

          int64_t Base = 0;
          if (!FrameOffset(Op.Inputs[I], Base))
            continue;

          int64_t Disp = static_cast<int64_t>(Op.Inputs[Other].Offset);
          if (Op.Opcode == NdOp::INT_SUB && I == 0) {
            if (Disp == std::numeric_limits<int64_t>::min())
              continue;
            Disp = -Disp;
          }

          if ((Disp > 0 && Base > std::numeric_limits<int64_t>::max() - Disp) ||
              (Disp < 0 && Base < std::numeric_limits<int64_t>::min() - Disp))
            continue;
          int64_t Total = Base + Disp;

          // A genuine frame-slot offset is bounded by the maximum frame size.
          // A |Total| beyond it means the "frame" input was a stale
          // (space,offset)-keyed association on a register that once held sp
          // but is now reused for data -- e.g. a TEA/hash kernel's `sum +=
          // 0x9E3779B9` on such a register looks like frame arithmetic at a
          // ~1.5 GB offset.  Don't seed that bogus slot (computeFrameSize would
          // only drop it later with a warning), and erase the stale association
          // so the data value doesn't cascade into further phantom frame
          // arithmetic.  The bound is far above any real frame (even large
          // vectorized kernels stay in the KB range), so legitimate frame
          // associations are untouched (#387a residual: the COPY-only clear
          // missed non-COPY data redefinitions of a reused EA-scratch temp).
          if (Total > limits::kMaxFrameSize || Total < -limits::kMaxFrameSize)
            continue;

          // Snapshotting Base above must precede this clear: address updates
          // are commonly in-place (`sp = sp - imm`), so output and frame input
          // may be the same operand.
          ClearOutput(Op);
          if (IsTrackableOutput(Op)) {
            AddrMap[VnKey(Op.Output)] = Total;
            FrameDefsInBlock.insert(VnKey(Op.Output));
          }

          AddSlot(Total, Op.Output.Size > 0 ? Op.Output.Size : 8);
          Propagated = true;
          break;
        }

        // A frame address may carry a runtime index before a later constant
        // displacement, as in x86-64 red-zone accesses `rsp + index - size`.
        // The dynamic term cannot define a concrete slot yet, but the result
        // is still frame-derived.  Preserve its known base so the following
        // constant adjustment can establish a conservative stack bound.
        if (!Propagated && Op.NumInputs >= 2) {
          for (uint8_t I = 0; I < Op.NumInputs; ++I) {
            if (Op.Opcode == NdOp::INT_SUB && I != 0)
              continue;
            uint8_t Other = 1 - I;
            if (Other >= Op.NumInputs || Op.Inputs[Other].isConst())
              continue;
            if (!IsFrameReg(Op.Inputs[I]) &&
                !FrameDefsInBlock.count(VnKey(Op.Inputs[I])))
              continue;
            int64_t Base = 0;
            if (!FrameOffset(Op.Inputs[I], Base))
              continue;
            ClearOutput(Op);
            if (IsTrackableOutput(Op)) {
              AddrMap[VnKey(Op.Output)] = Base;
              FrameDefsInBlock.insert(VnKey(Op.Output));
            }
            Propagated = true;
            break;
          }
        }
        if (!Propagated)
          ClearOutput(Op);
      }
      // These operations retain the numeric address while changing storage or
      // width.  SUBBYTES only preserves an address when selecting byte zero.
      else if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
                Op.Opcode == NdOp::INT_SEXT || Op.Opcode == NdOp::SUBBYTES) &&
               Op.NumInputs >= 1) {
        bool ZeroSubpiece = Op.Opcode != NdOp::SUBBYTES ||
                            (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                             Op.Inputs[1].Offset == 0);
        int64_t Offset = 0;
        bool Propagated = ZeroSubpiece && IsTrackableOutput(Op) &&
                          FrameOffset(Op.Inputs[0], Offset);
        ClearOutput(Op);
        if (Propagated) {
          AddrMap[VnKey(Op.Output)] = Offset;
          FrameDefsInBlock.insert(VnKey(Op.Output));
        }
      } else
        ClearOutput(Op);
    }
  }

  std::sort(StackSlots.begin(), StackSlots.end(),
            [](const auto &A, const auto &B) { return A.Offset < B.Offset; });
}

} // namespace neverd
