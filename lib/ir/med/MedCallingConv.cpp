//===- MedCallingConv.cpp - Calling convention detection for MedIR ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Architecture-generic calling convention detection framework.
/// Detects calling conventions (SysV AMD64, Win64, CDECL, ARM AAPCS)
/// from live-in registers and stack patterns, recovers parameter lists
/// and stack frame layout.
///
/// Architecture-specific detection logic is split into separate files
/// following LLVM's target-dispatch pattern:
///   - MedCallingConvX86.cpp: XMM params, i386 CDECL stack params
///   - MedVariadic.cpp: per-ABI variadic (...) prologue detection
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/Support/Diagnostic.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallingConvDetail.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {

// ---- x86-specific helpers (MedCallingConvX86.cpp) ----
void detectXMMParams(
    MedFunc &Func, const MedBlock &Entry, const TargetRegInfo &TRI,
    const std::map<std::pair<uint64_t, uint16_t>, int> &RegVarMap,
    Arch TargetArch);
void detectCdeclStackParams(MedFunc &Func, Arch TargetArch);

// ---- variadic (...) prologue detection (MedVariadic.cpp) ----
void detectVariadic(MedFunc &Func, const TargetRegInfo &TRI, Arch TargetArch,
                    BinaryFormat Fmt);

namespace med_calling_conv_detail {
/// Scan all blocks to determine the widest data width at which the entry
/// live-in value of a parameter register is actually consumed. Returns 0 if
/// no use found. Later SSA values in the same physical register are clobbers,
/// not uses of the incoming parameter.
uint16_t findFirstUseSize(const MedFunc &Func, uint64_t ParamRegOff,
                          const TargetRegInfo &TRI) {
  if (Func.Blocks.empty())
    return 0;

  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  struct ValueEdge {
    ValueKey To;
    uint16_t WidthLimit = 0;
  };
  auto Key = [](const MedVar &V) { return ValueKey{V.Kind, V.Id, V.SSAVer}; };
  auto IsValue = [](const MedVar &V) { return V.Kind != MedVar::Const; };
  auto LimitWidth = [](uint16_t Width, uint16_t Limit) {
    if (Width == 0)
      return Limit;
    if (Limit == 0)
      return Width;
    return std::min(Width, Limit);
  };

  std::map<ValueKey, uint16_t> LiveInWidths;
  std::deque<ValueKey> Worklist;
  auto Seed = [&](const MedVar &V) {
    uint16_t Width = TRI.FullRegWidth;
    auto [It, Inserted] = LiveInWidths.emplace(Key(V), Width);
    if (Inserted || It->second < Width) {
      It->second = Width;
      Worklist.push_back(It->first);
    }
  };
  for (const MedOp &Op : Func.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != ParamRegOff ||
        Op.NumInputs < 1 || Op.Inputs[0].Kind != MedVar::Reg ||
        Op.Inputs[0].Id != Op.Output.Id ||
        Op.Inputs[0].SSAVer != Op.Output.SSAVer)
      continue;
    Seed(Op.Output);
  }
  if (LiveInWidths.empty())
    return 0;

  std::map<ValueKey, std::vector<ValueEdge>> Edges;
  auto AddEdge = [&](const MedVar &From, const MedVar &To,
                     uint16_t WidthLimit = 0) {
    if (IsValue(From) && IsValue(To))
      Edges[Key(From)].push_back({Key(To), WidthLimit});
  };
  for (const MedBlock &Blk : Func.Blocks) {
    for (const PhiNode &Phi : Blk.Phis)
      for (const auto &Arg : Phi.Args)
        AddEdge(Arg.second, Phi.Output);

    for (const MedOp &Op : Blk.Ops) {
      if (Op.NumInputs < 1)
        continue;
      switch (Op.Opcode) {
      case NdOp::COPY:
        if (Op.NumInputs == 1)
          AddEdge(Op.Inputs[0], Op.Output,
                  LimitWidth(Op.Inputs[0].Size, Op.Output.Size));
        break;
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        if (Op.NumInputs == 1)
          AddEdge(Op.Inputs[0], Op.Output, Op.Inputs[0].Size);
        break;
      case NdOp::SUBBYTES:
        if (Op.NumInputs >= 2 && Op.Inputs[1].Kind == MedVar::Const &&
            Op.Inputs[1].ConstVal == 0)
          AddEdge(Op.Inputs[0], Op.Output, Op.Output.Size);
        break;
      default:
        break;
      }
    }
  }

  while (!Worklist.empty()) {
    ValueKey From = Worklist.front();
    Worklist.pop_front();
    uint16_t FromWidth = LiveInWidths[From];
    auto EdgeIt = Edges.find(From);
    if (EdgeIt == Edges.end())
      continue;
    for (const ValueEdge &Edge : EdgeIt->second) {
      uint16_t Width = LimitWidth(FromWidth, Edge.WidthLimit);
      auto [It, Inserted] = LiveInWidths.emplace(Edge.To, Width);
      if (Inserted || It->second < Width) {
        It->second = Width;
        Worklist.push_back(It->first);
      }
    }
  }

  auto ConsumedWidth = [&](const MedVar &V) -> uint16_t {
    if (!IsValue(V) || V.Size == 0)
      return 0;
    auto It = LiveInWidths.find(Key(V));
    if (It == LiveInWidths.end())
      return 0;
    return LimitWidth(V.Size, It->second);
  };
  auto IsTransparent = [](const MedOp &Op) {
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs == 1)
      return true;
    return Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
           Op.Inputs[1].Kind == MedVar::Const && Op.Inputs[1].ConstVal == 0;
  };

  uint16_t MaxSz = 0;
  for (const auto &Blk : Func.Blocks) {
    for (const auto &Op : Blk.Ops) {
      if (IsTransparent(Op))
        continue;
      if (Op.Opcode == NdOp::STORE) {
        for (uint8_t K = 0; K < Op.NumInputs; ++K) {
          uint16_t Sz = ConsumedWidth(Op.Inputs[K]);
          if (Sz > MaxSz)
            MaxSz = Sz;
        }
        continue;
      }
      for (uint8_t K = 0; K < Op.NumInputs; ++K) {
        uint16_t Sz = ConsumedWidth(Op.Inputs[K]);
        if (Sz > MaxSz)
          MaxSz = Sz;
      }
    }
  }
  return MaxSz;
}

} // namespace med_calling_conv_detail

namespace {

/// An i386 parameter register can look "live-in" purely because the body uses
/// it as scratch in a way that reads its incoming bits — never because it
/// receives an argument.  Two idioms produce that false signal:
///   * a partial sub-register write (`setne %cl`, `mov %cx,..`) merges the
///     register's old upper bits (`(reg & 0xFFFFFF00) | new_low`);
///   * BSR/BSF model their architecturally-undefined zero-source destination as
///     the preserved old value (`SELECT(src==0, old_dst, computed)`), so the
///     first such instruction reads the register's incoming value.
/// Returns true when the register's live-in value (and everything transitively
/// copied from it) is consumed ONLY by those scratch idioms and never as a
/// genuine value, so it must not be recovered as a parameter (doing so injects
/// a phantom leading argument that shifts every real stack argument).
bool liveInOnlyFeedsScratch(const MedFunc &Func, uint64_t ParamRegOff) {
  if (Func.Blocks.empty())
    return false;
  auto key = [](const MedVar &V) { return std::make_pair(V.Id, V.SSAVer); };

  auto findDef = [&](const MedVar &V) -> const MedOp * {
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Output.Id == V.Id && Op.Output.SSAVer == V.SSAVer &&
            Op.Output.Kind == V.Kind)
          return &Op;
    return nullptr;
  };

  // Seed the taint set with the entry-block live-in self-copies of the
  // register.
  std::set<std::pair<int, int>> Taint;
  for (const auto &Op : Func.Blocks[0].Ops) {
    if (Op.Opcode != NdOp::COPY || Op.NumInputs < 1)
      continue;
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == ParamRegOff &&
        Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[0].Id == Op.Output.Id)
      Taint.insert(key(Op.Output));
  }
  if (Taint.empty())
    return false; // no identifiable live-in value: keep existing behavior

  auto tainted = [&](const MedVar &V) {
    return (V.Kind == MedVar::Reg || V.Kind == MedVar::Temp) &&
           Taint.count(key(V)) != 0;
  };
  // Value-preserving forwards through which taint must propagate.  A COPY only
  // keeps "this is still the live-in register" when it targets the *same*
  // register (an SSA version bump / the live-in self-copy); a COPY to a
  // different register or a temp lets the value escape — typically into a call
  // argument or a real computation — and so counts as a genuine use, not a
  // pass-through.  Width views (ZEXT/SEXT/SUBBYTES@0) keep the same value.
  auto isPassThrough = [](const MedOp &Op) {
    switch (Op.Opcode) {
    case NdOp::COPY:
      return Op.NumInputs == 1 && Op.Output.Kind == MedVar::Reg &&
             Op.Inputs[0].Kind == MedVar::Reg &&
             Op.Output.RegOff == Op.Inputs[0].RegOff;
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Op.NumInputs == 1;
    case NdOp::SUBBYTES:
      return Op.NumInputs >= 2 && Op.Inputs[1].Kind == MedVar::Const &&
             Op.Inputs[1].ConstVal == 0;
    default:
      return false;
    }
  };
  // Preserve masks emitted while reconstructing an i386 partial-register
  // write.  Besides low-byte/low-word writes, AH/CH/DH/BH clear bits 8..15 and
  // therefore use 0xFFFF00FF, whose low byte is deliberately nonzero.
  auto isPartialWritePreserveMask = [](const MedOp &Op, int TaintedIdx) {
    if (Op.Opcode != NdOp::INT_AND || Op.NumInputs < 2)
      return false;
    const MedVar &M = Op.Inputs[TaintedIdx == 0 ? 1 : 0];
    if (M.Kind != MedVar::Const)
      return false;
    switch (Op.Output.Size) {
    case 2:
      return M.ConstVal == 0xFF00ULL;
    case 4:
      return M.ConstVal == 0xFFFFFF00ULL || M.ConstVal == 0xFFFF00FFULL ||
             M.ConstVal == 0xFFFF0000ULL;
    case 8:
      return M.ConstVal == 0xFFFFFFFFFFFFFF00ULL ||
             M.ConstVal == 0xFFFFFFFFFFFF00FFULL ||
             M.ConstVal == 0xFFFFFFFFFFFF0000ULL ||
             M.ConstVal == 0xFFFFFFFF00000000ULL;
    default:
      return false;
    }
  };
  // The BSR/BSF zero-source preserve `SELECT(src==0, old_dst, computed)` reads
  // the register only as `old_dst` (input 1); `computed` is `(bits-1) -
  // LZCOUNT`. Matching that shape (not an arbitrary cmov) keeps a genuine
  // `cond?reg:y` parameter use out of the scratch bucket.
  auto isBsrBsfPreserve = [&](const MedOp &Op, int TaintedIdx) {
    if (Op.Opcode != NdOp::SELECT || Op.NumInputs != 3 || TaintedIdx != 1)
      return false;
    const MedOp *Comp = findDef(Op.Inputs[2]);
    if (!Comp || Comp->Opcode != NdOp::INT_SUB || Comp->NumInputs < 2)
      return false;
    const MedOp *Lz = findDef(Comp->Inputs[1]);
    return Lz && Lz->Opcode == NdOp::LZCOUNT;
  };

  bool Changed = true;
  int Guard = 0;
  while (Changed && Guard++ < 100000) {
    Changed = false;
    for (const auto &Blk : Func.Blocks) {
      for (const auto &Phi : Blk.Phis) {
        for (const auto &[Pred, AV] : Phi.Args)
          if (tainted(AV)) {
            if (Taint.insert(key(Phi.Output)).second)
              Changed = true;
            break;
          }
      }
      for (const auto &Op : Blk.Ops) {
        int TIdx = -1;
        for (uint8_t K = 0; K < Op.NumInputs; ++K)
          if (tainted(Op.Inputs[K])) {
            TIdx = K;
            break;
          }
        if (TIdx < 0)
          continue;
        if (isPassThrough(Op)) {
          if ((Op.Output.Kind == MedVar::Reg ||
               Op.Output.Kind == MedVar::Temp) &&
              Taint.insert(key(Op.Output)).second)
            Changed = true;
          continue;
        }
        if (isPartialWritePreserveMask(Op, TIdx))
          continue; // partial-write reconstruction, not a genuine use
        if (isBsrBsfPreserve(Op, TIdx))
          continue;   // BSR/BSF zero-source preserve, not a genuine use
        return false; // a genuine consumer of the live-in value: keep the param
      }
    }
  }
  return true; // every use is a sub-register merge: a scratch false positive
}

/// Find the set of self-copy parameter registers in the entry block.
/// In the LowIR->MedIR translation, self-copies (COPY reg, reg) at the
/// entry denote live-in registers from the caller.
std::set<uint64_t> findLiveInParamRegs(const MedBlock &Entry,
                                       llvm::ArrayRef<uint64_t> ParamRegs) {
  std::set<uint64_t> Used;
  for (const auto &Op : Entry.Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (Op.Output.Kind != MedVar::Reg)
      continue;
    if (Op.NumInputs < 1 || Op.Inputs[0].Id != Op.Output.Id)
      continue;
    for (uint64_t PR : ParamRegs) {
      if (Op.Output.RegOff == PR) {
        Used.insert(PR);
        break;
      }
    }
  }
  return Used;
}

//===----------------------------------------------------------------------===//
// Architecture-generic register-passed parameter detection
//===----------------------------------------------------------------------===//

void detectRegisterParams(
    MedFunc &Func, const TargetRegInfo &TRI, llvm::ArrayRef<uint64_t> ParamRegs,
    const std::set<uint64_t> &UsedParamRegs, Arch TargetArch) {
  // No parameter register is live-in: the function takes no register arguments
  // (a leaf with stack-only or no arguments).  Returning keeps Func.Params
  // empty so the cdecl/stack detector numbers arguments from arg0 rather than
  // past a spurious placeholder for the first parameter register.
  if (UsedParamRegs.empty())
    return;

  // Find the highest-index used parameter register so we can insert
  // placeholder params for gaps (e.g., if RDI and RDX are used but RSI
  // is not, we need a dummy RSI param to keep the ABI register mapping
  // correct after recompilation).
  size_t LastUsedIdx = 0;
  for (size_t I = 0; I < ParamRegs.size(); ++I)
    if (UsedParamRegs.count(ParamRegs[I]))
      LastUsedIdx = I;

  // i386's optional regparm convention fills ECX then EDX with no gaps, so the
  // used register arguments must be consecutive from arg0.  A parameter
  // register that is "live-in" only because a partial sub-register write merges
  // its old bits (e.g. `sete %dl` keeps EDX[31:8]) is not a real argument;
  // requiring an unbroken run from arg0 rejects that false positive — an unused
  // ECX proves a live EDX is not a regparm argument, so the function is plain
  // cdecl.
  if (TargetArch == Arch::X86) {
    size_t Consec = 0;
    while (Consec < ParamRegs.size() &&
           UsedParamRegs.count(ParamRegs[Consec]) &&
           !liveInOnlyFeedsScratch(Func, ParamRegs[Consec]))
      ++Consec;
    if (Consec == 0)
      return;
    LastUsedIdx = Consec - 1;
  }

  for (size_t Idx = 0; Idx < ParamRegs.size() && Idx <= LastUsedIdx; ++Idx) {
    uint64_t PR = ParamRegs[Idx];
    if (!UsedParamRegs.count(PR)) {
      // Insert a placeholder parameter to maintain correct ABI positioning.
      MedVar Placeholder;
      Placeholder.Kind = MedVar::Param;
      Placeholder.Id = -1;
      Placeholder.Size = TRI.FullRegWidth;
      Placeholder.RegOff = PR;
      Placeholder.TheArch = TargetArch;
      Func.Params.push_back(Placeholder);
      continue;
    }

    uint16_t FirstUse =
        med_calling_conv_detail::findFirstUseSize(Func, PR, TRI);
    uint16_t BestSize = FirstUse > 0 ? FirstUse : TRI.FullRegWidth;

    const MedVar *Marker = nullptr;
    for (const MedOp &Op : Func.Blocks.front().Ops) {
      if (Op.Opcode != NdOp::COPY)
        break;
      if (Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != PR ||
          Op.NumInputs < 1 || Op.Inputs[0].Kind != MedVar::Reg ||
          Op.Inputs[0].Id != Op.Output.Id ||
          Op.Inputs[0].SSAVer != Op.Output.SSAVer)
        continue;
      if (!Marker || Op.Output.Size == BestSize)
        Marker = &Op.Output;
      if (Op.Output.Size == BestSize)
        break;
    }
    if (!Marker)
      continue;

    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.Id = Marker->Id;
    Param.Size = BestSize;
    Param.RegOff = PR;
    Param.TheArch = TargetArch;
    Func.Params.push_back(Param);
  }
}

//===----------------------------------------------------------------------===//
// Common: stack locals and frame size computation
//===----------------------------------------------------------------------===//

constexpr int64_t kMaxFrameSize = limits::kMaxFrameSize;

void collectStackLocals(
    MedFunc &Func, const std::vector<LowToMedConverter::StackSlot> &Slots) {
  for (const auto &Slot : Slots) {
    MedVar Local;
    Local.Kind = MedVar::Stack;
    Local.Id = Slot.VarId;
    Local.Size = Slot.Size;
    Local.StackOff = Slot.Offset;
    Func.Locals.push_back(Local);
  }
}

void computeFrameBounds(
    MedFunc &Func,
    const std::vector<LowToMedConverter::StackSlot> &Slots) {
  Func.FrameSize = 0;
  Func.FrameHeadroom = 0;
  constexpr uint64_t MaxFrame = static_cast<uint64_t>(kMaxFrameSize);
  for (const auto &Slot : Slots) {
    if (Slot.Offset >= 0) {
      uint64_t Start = static_cast<uint64_t>(Slot.Offset);
      if (Start > MaxFrame || Slot.Size > MaxFrame - Start) {
        syncWarning()
            << "low_to_med: ignoring bogus incoming stack slot offset="
            << Slot.Offset << " size=" << Slot.Size
            << " (would exceed frame headroom limit)\n";
        continue;
      }
      uint64_t Extent = Start + Slot.Size;
      Func.FrameHeadroom =
          std::max(Func.FrameHeadroom, static_cast<int64_t>(Extent));
      continue;
    }

    // A slot describes the half-open interval [offset, offset + size).  Its
    // negative and positive portions reserve storage independently; adding
    // size to abs(offset) over-allocates ordinary spills and misses a slot
    // that straddles the synthetic entry SP.
    uint64_t Distance = static_cast<uint64_t>(-(Slot.Offset + 1)) + 1;
    uint64_t UpperExtent = Slot.Size > Distance ? Slot.Size - Distance : 0;
    if (Distance > MaxFrame || UpperExtent > MaxFrame) {
      syncWarning() << "low_to_med: ignoring bogus stack slot offset="
                    << Slot.Offset << " size=" << Slot.Size
                    << " (would exceed frame bounds limit)\n";
      continue;
    }
    Func.FrameSize = std::max(Func.FrameSize, static_cast<int64_t>(Distance));
    Func.FrameHeadroom =
        std::max(Func.FrameHeadroom, static_cast<int64_t>(UpperExtent));
  }

  if (Func.FrameSize > kMaxFrameSize) {
    syncWarning() << "low_to_med: clamping oversized FrameSize "
                  << Func.FrameSize << " to " << kMaxFrameSize << "\n";
    Func.FrameSize = kMaxFrameSize;
  }
}

//===----------------------------------------------------------------------===//
// Register-ABI stack-passed parameter detection (x86-64 SysV, AArch64/ARM
// AAPCS)
//===----------------------------------------------------------------------===//

// Recover arguments passed on the stack (those beyond the parameter registers)
// for the register-based ABIs.  i386 CDECL — where *every* argument is on the
// stack — is handled separately by detectCdeclStackParams.  An incoming stack
// argument is read from [entry_sp + k]; recover those loads as parameters
// appended after the register parameters and rewrite each load to read its
// parameter, so a callee with more than 8/6/4 arguments gains the full arity
// instead of dereferencing uninitialised stack.
//
// \p MaxStackOff (0 = unbounded) restricts recovery to incoming stack offsets
// strictly below it.  A Darwin AArch64 variadic function with more than 8 named
// integer arguments uses this to recover only the NAMED stack prefix
// (offsets [0, overflow_base)); the trailing variadic arguments live at and
// above the overflow base and are read through the va_arg walk (not direct
// [entry_sp+k] loads), so they must not be mistaken for fixed stack parameters.
void detectStackParams(MedFunc &Func, Arch TargetArch,
                       int64_t MaxStackOff = 0) {
  if (TargetArch == Arch::X86 || Func.Blocks.empty())
    return;
  const auto &TRI = getTargetRegInfo(TargetArch);
  const uint64_t SpOff = TRI.StackPointer;
  const int MaxRegArgs = static_cast<int>(TRI.IntParamRegs.size());
  const int Slot = TRI.PointerSize;
  if (MaxRegArgs <= 0 || Slot <= 0)
    return;

  // x86-64 `call` pushes a return address into [entry_sp]; the first stack
  // argument sits one slot above it.  AArch64/ARM keep the return address in
  // LR, so the first stack argument is at [entry_sp + 0].
  const int64_t Base = (TargetArch == Arch::X64) ? Slot : 0;

  // Count the leading integer parameter registers already recovered.  A stack
  // argument implies every register slot is a real argument and the stack
  // indices must align with the full register set; the gate (after the load
  // scan finds a stack argument) either accepts a complete prefix or, for a
  // pure forwarder that never reads its register arguments, synthesizes the
  // missing leading register parameters.
  int Leading = 0;
  for (int I = 0; I < MaxRegArgs && I < static_cast<int>(Func.Params.size());
       ++I) {
    if (Func.Params[I].RegOff != TRI.IntParamRegs[I])
      break;
    ++Leading;
  }

  auto findDef = [&](const MedVar &V) -> const MedOp * {
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
            Op.Output.SSAVer == V.SSAVer)
          return &Op;
    return nullptr;
  };

  // Offset of \p V relative to the entry stack pointer (folding the COPY /
  // zext / subpiece / push-sub chain detectCc sees before copy propagation),
  // or nullopt when V is not entry-SP-derived.
  std::function<std::optional<int64_t>(const MedVar &, int)> traceOff =
      [&](const MedVar &V, int Depth) -> std::optional<int64_t> {
    if (Depth > 64)
      return std::nullopt;
    const MedOp *Def = findDef(V);
    if (!Def)
      return (V.Kind == MedVar::Reg && V.RegOff == SpOff)
                 ? std::optional<int64_t>(0)
                 : std::nullopt;
    auto constOf = [](const MedVar &X) -> std::optional<int64_t> {
      return X.Kind == MedVar::Const
                 ? std::optional<int64_t>(static_cast<int64_t>(X.ConstVal))
                 : std::nullopt;
    };
    switch (Def->Opcode) {
    case NdOp::COPY:
      if (Def->NumInputs >= 1) {
        const MedVar &In = Def->Inputs[0];
        if (In.Kind == MedVar::Reg && In.RegOff == SpOff && In.Id == V.Id &&
            In.SSAVer == V.SSAVer)
          return 0; // entry stack-pointer self-copy
        return traceOff(In, Depth + 1);
      }
      return std::nullopt;
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Def->NumInputs == 1 ? traceOff(Def->Inputs[0], Depth + 1)
                                 : std::nullopt;
    case NdOp::SUBBYTES:
      return Def->NumInputs >= 2 && Def->Inputs[1].Kind == MedVar::Const &&
                     Def->Inputs[1].ConstVal == 0
                 ? traceOff(Def->Inputs[0], Depth + 1)
                 : std::nullopt;
    case NdOp::INT_ADD:
      if (Def->NumInputs >= 2) {
        if (auto C = constOf(Def->Inputs[1]))
          if (auto B = traceOff(Def->Inputs[0], Depth + 1))
            return *B + *C;
        if (auto C = constOf(Def->Inputs[0]))
          if (auto B = traceOff(Def->Inputs[1], Depth + 1))
            return *B + *C;
      }
      return std::nullopt;
    case NdOp::INT_SUB:
      if (Def->NumInputs >= 2)
        if (auto C = constOf(Def->Inputs[1]))
          if (auto B = traceOff(Def->Inputs[0], Depth + 1))
            return *B - *C;
      return std::nullopt;
    default:
      return std::nullopt;
    }
  };

  // Offset of a stack read relative to the entry stack pointer, bounded to the
  // incoming-argument window.  Unlike a slot-aligned check it keeps sub-slot
  // byte offsets: a read at a non-slot offset is a field of a by-value
  // aggregate that spans several argument slots, or the high half of a wide
  // scalar argument, recovered below as a SUBBYTES of the slot it falls in.
  auto rawStackOff = [&](const MedVar &AddrVar) -> std::optional<int64_t> {
    if (AddrVar.Kind != MedVar::Temp && AddrVar.Kind != MedVar::Reg)
      return std::nullopt;
    auto Off = traceOff(AddrVar, 0);
    if (!Off || *Off < Base || *Off > 0x400)
      return std::nullopt;
    if (MaxStackOff > 0 && *Off >= MaxStackOff)
      return std::nullopt; // beyond the named prefix (variadic overflow area)
    return *Off;
  };
  // The slot-aligned base at or below \p Off and the byte offset of \p Off
  // within that slot.
  auto slotOf = [&](int64_t Off) -> std::pair<int64_t, int64_t> {
    int64_t SlotBase = Base + ((Off - Base) / Slot) * Slot;
    return {SlotBase, Off - SlotBase};
  };
  auto offToIdx = [&](int64_t Off) {
    return MaxRegArgs + static_cast<int>((Off - Base) / Slot);
  };

  std::set<int64_t> Offsets;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
        if (auto Off = rawStackOff(Op.Inputs[0])) {
          auto [SlotBase, ByteOff] = slotOf(*Off);
          Offsets.insert(SlotBase);
          // A slot-aligned load wider than one pointer slot (an 8-byte double
          // stack argument on a 4-byte-slot target) spans several slots; record
          // the highest so every covered slot is created as a parameter below.
          uint16_t LoadSz =
              Op.Output.Size > 0 ? Op.Output.Size : static_cast<uint16_t>(Slot);
          if (ByteOff == 0 && LoadSz > Slot)
            Offsets.insert(slotOf(*Off + LoadSz - Slot).first);
        }
  if (Offsets.empty())
    return;

  // A stack argument was found, so the function takes more than the register
  // arguments.  The stack-argument indices must align with a full parameter-
  // register prefix on the caller, so ensure all MaxRegArgs leading register
  // slots are present.
  if (Leading != MaxRegArgs) {
    bool HasRegParam = false;
    for (const auto &P : Func.Params)
      if (P.RegOff != kNoParamReg && TRI.regToArgIdx(P.RegOff) >= 0) {
        HasRegParam = true;
        break;
      }
    if (!HasRegParam) {
      // Pure tail-call forwarder that never reads its register arguments
      // (detectCc's live-in scan saw none): synthesize the full register set as
      // real forwarded parameters so the stack indices align with the caller.
      std::vector<MedVar> RegParams;
      for (int I = 0; I < MaxRegArgs; ++I) {
        MedVar P;
        P.Kind = MedVar::Param;
        P.Id = I;
        P.RegOff = TRI.IntParamRegs[I];
        P.Size = TRI.FullRegWidth;
        P.TheArch = TargetArch;
        RegParams.push_back(P);
      }
      Func.Params.insert(Func.Params.begin(), RegParams.begin(),
                         RegParams.end());
    } else {
      // Partial register prefix + stack arguments: an x86-64 MEMORY-class
      // by-value aggregate is stack-passed even with parameter registers free,
      // so the prefix can be incomplete (`f(BigStruct s, int n)` puts n in RDI
      // and s on the stack).  Pad the missing register slots [Leading,
      // MaxRegArgs) with unused placeholders (after the existing leading
      // integer prefix, before any FP parameters) so the stack parameters keep
      // their MaxRegArgs+ indices — the backend then places them on the stack.
      std::vector<MedVar> Pad;
      for (int I = Leading; I < MaxRegArgs; ++I) {
        MedVar P;
        P.Kind = MedVar::Param;
        P.Id = -1;
        P.RegOff = TRI.IntParamRegs[I];
        P.Size = TRI.FullRegWidth;
        P.TheArch = TargetArch;
        Pad.push_back(P);
      }
      Func.Params.insert(Func.Params.begin() + Leading, Pad.begin(), Pad.end());
    }
  }

  int MaxIdx = MaxRegArgs - 1;
  for (int64_t Off : Offsets)
    MaxIdx = std::max(MaxIdx, offToIdx(Off));
  for (int I = MaxRegArgs; I <= MaxIdx; ++I) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.Id = I;
    Param.Size = static_cast<uint16_t>(Slot);
    Param.RegOff = kNoParamReg;
    Param.TheArch = TargetArch;
    Func.Params.push_back(Param);
  }

  // A stack-argument slot whose home is a mutable local — the function WRITES
  // it directly, or its ADDRESS ESCAPES (passed to a call / stored, so a callee
  // may write it through the escaped pointer) — keeps its loads as memory reads
  // rather than folding them to the incoming value (which would drop the
  // write). The home slot already holds the argument (the caller places it at
  // exactly [entry_sp + Base + k*Slot], which is what a memory load reads) and
  // the emitter re-seeds it at entry, so memory access is correct.
  std::set<int64_t> MutableSlots;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 1)
        if (auto Off = rawStackOff(Op.Inputs[0]))
          MutableSlots.insert(slotOf(*Off).first); // direct write
      // SP and any stack-slot address thread through copies, extends and
      // address arithmetic to reach their real use; those propagation ops are
      // not escapes. On AArch64/ARM the first stack argument sits at [sp+0], so
      // the bare SP base flowing through the block-entry `COPY SP SP` (or an
      // address copy) would otherwise be mis-read as slot 0 escaping and
      // corrupt every read-only stack argument there.  A genuine escape (the
      // address used as a call argument, a stored value, or returned) is a
      // non-propagation op and is still caught here, where rawStackOff threads
      // back through the chain to the slot.  Kept in sync with the ops traceOff
      // walks through.
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::SUBBYTES:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        continue;
      default:
        break;
      }
      for (uint8_t K = 0; K < Op.NumInputs; ++K) {
        if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && K == 0)
          continue;
        if (auto Off = rawStackOff(Op.Inputs[K]))
          MutableSlots.insert(slotOf(*Off).first); // escaped address
      }
    }

  // The home slot of each mutable stack argument is [frame_end + SlotBase].
  // Record it so the emitter seeds that headroom slot with the parameter at
  // entry; the memory loads/stores left below then read the argument and
  // observe later writes.
  for (int64_t SlotBase : MutableSlots) {
    int Idx = offToIdx(SlotBase);
    if (Idx >= MaxRegArgs && Idx <= MaxIdx)
      Func.MutableStackParamHomes.push_back({Idx, SlotBase});
  }

  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs < 1)
        continue;
      auto Off = rawStackOff(Op.Inputs[0]);
      if (!Off)
        continue;
      auto [SlotBase, ByteOff] = slotOf(*Off);
      uint16_t ReadSz =
          Op.Output.Size > 0 ? Op.Output.Size : static_cast<uint16_t>(Slot);
      // A slot-aligned 8-byte scalar argument on a 4-byte-slot target (an ARM32
      // `double` / `long long` passed on the stack as `vldr d,[sp+k]`) spans
      // two pointer slots; rebuild it from the two slot parameters (low slot
      // first) so it reads its arguments instead of an absolute address.  The
      // caller passes the same value as two consecutive 4-byte slot arguments.
      bool WidePair = ByteOff == 0 && Slot == 4 && ReadSz == 2u * Slot;
      // Mutable stack-argument home (written or address-escaped): keep the load
      // as a memory read so a later write through it is observed.
      if (MutableSlots.count(SlotBase) ||
          (WidePair && MutableSlots.count(SlotBase + Slot)))
        continue;
      // A read straddling the slot end (and not a clean two-slot pair) is not a
      // clean field of this slot; leave it for a later pass.
      if (!WidePair && ByteOff + static_cast<int64_t>(ReadSz) > Slot)
        continue;
      if (WidePair) {
        MedVar Lo, Hi;
        Lo.Kind = Hi.Kind = MedVar::Param;
        Lo.Id = offToIdx(SlotBase);
        Hi.Id = offToIdx(SlotBase + Slot);
        Lo.Size = Hi.Size = static_cast<uint16_t>(Slot);
        Lo.RegOff = Hi.RegOff = kNoParamReg;
        Lo.TheArch = Hi.TheArch = TargetArch;
        Op.Opcode = NdOp::CONCAT;
        Op.Inputs[0] = Hi;
        Op.Inputs[1] = Lo;
        Op.NumInputs = 2;
      } else if (ByteOff == 0) {
        MedVar Param;
        Param.Kind = MedVar::Param;
        Param.Id = offToIdx(SlotBase);
        Param.Size = ReadSz;
        Param.RegOff = kNoParamReg;
        Param.TheArch = TargetArch;
        Op.Opcode = NdOp::COPY;
        Op.Inputs[0] = Param;
        Op.NumInputs = 1;
      } else {
        // Sub-slot field of a by-value aggregate (or wide-scalar high half):
        // extract the lane from the full-width slot parameter.
        MedVar Param;
        Param.Kind = MedVar::Param;
        Param.Id = offToIdx(SlotBase);
        Param.Size = static_cast<uint16_t>(Slot);
        Param.RegOff = kNoParamReg;
        Param.TheArch = TargetArch;
        Op.Opcode = NdOp::SUBBYTES;
        Op.Inputs[0] = Param;
        Op.Inputs[1] = MedVar::makeConst(static_cast<uint64_t>(ByteOff), 4);
        Op.NumInputs = 2;
      }
    }
}

// Recover the hidden indirect-result (sret) pointer parameter: a function that
// returns a by-value aggregate too large for the return registers receives the
// caller-allocated result buffer in the indirect-result register (AArch64 x8)
// and writes the aggregate through it.  x8 is caller-saved scratch, so a
// live-in x8 (an entry self-copy) used as a memory base is the result-buffer
// pointer. Append it as a trailing pointer parameter so the through-x8 stores
// reference the parameter (instead of an uninitialised register the emitter
// folds to 0, storing the fields to absolute addresses).
void detectIndirectResultParam(MedFunc &Func, const MedBlock &Entry,
                               const TargetRegInfo &TRI, Arch TargetArch) {
  const uint64_t IRR = TRI.indirectResultReg();
  if (IRR == 0 || Func.Blocks.empty())
    return;
  for (const auto &P : Func.Params)
    if (P.RegOff == IRR)
      return;

  int LiveInId = -1;
  uint16_t LiveInSz = 0;
  for (const auto &Op : Entry.Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == IRR &&
        Op.NumInputs >= 1 && Op.Inputs[0].RegOff == IRR &&
        Op.Inputs[0].Id == Op.Output.Id) {
      LiveInId = Op.Output.Id;
      LiveInSz = Op.Output.Size;
      break;
    }
  }
  if (LiveInId < 0)
    return;

  auto findDef = [&](const MedVar &V) -> const MedOp * {
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
            Op.Output.SSAVer == V.SSAVer && Op.Output.RegOff == V.RegOff)
          return &Op;
    return nullptr;
  };
  std::function<bool(const MedVar &, int)> derivesFromIRR =
      [&](const MedVar &V, int Depth) -> bool {
    if (Depth > 32)
      return false;
    if (V.Kind == MedVar::Reg && V.RegOff == IRR)
      return true;
    const MedOp *Def = findDef(V);
    if (!Def)
      return false;
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::SUBBYTES:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Def->NumInputs >= 1 && derivesFromIRR(Def->Inputs[0], Depth + 1);
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (!Def->Inputs[I].isConst() &&
            derivesFromIRR(Def->Inputs[I], Depth + 1))
          return true;
      return false;
    default:
      return false;
    }
  };

  bool UsedAsBase = false;
  for (const auto &Blk : Func.Blocks) {
    for (const auto &Op : Blk.Ops) {
      // x8 (or a value derived from it) used as a memory base: the buffer
      // pointer is dereferenced to write the aggregate fields directly -- the
      // explicit- field return form (`struct R r={..}; return r;`).
      if ((Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::LOAD) &&
          Op.NumInputs >= 1 && derivesFromIRR(Op.Inputs[0], 0)) {
        UsedAsBase = true;
        break;
      }
      // x8 spilled to a frame slot as a STORE value: at -O0 clang saves the
      // incoming indirect-result pointer to the stack and reloads it inside the
      // body (the loop / memcpy return form, e.g. `for(i) r.a[i]=..; return
      // r;`), so x8 itself is only ever a store value, never a direct base.
      // Paired with the entry self-copy live-in established above (on AArch64
      // x8 is an incoming value only for sret -- otherwise caller-saved scratch
      // written before read, so no live-in), this is the result buffer being
      // preserved. Without recognizing it the spilled pointer is lost (x8 folds
      // to 0) and the reloaded base dereferences null -> the callee SIGSEGVs.
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          derivesFromIRR(Op.Inputs[1], 0)) {
        UsedAsBase = true;
        break;
      }
    }
    if (UsedAsBase)
      break;
  }
  if (!UsedAsBase)
    return;

  MedVar Param;
  Param.Kind = MedVar::Param;
  Param.Id = LiveInId;
  Param.Size = LiveInSz > 0 ? LiveInSz : static_cast<uint16_t>(TRI.PointerSize);
  Param.RegOff = IRR;
  Param.TheArch = TargetArch;
  Func.Params.push_back(Param);
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// detectCc -- main dispatcher
//===----------------------------------------------------------------------===//

void LowToMedConverter::detectCc(MedFunc &Func, Arch TheArch,
                                 BinaryFormat Fmt) {
  const auto &TRI = getTargetRegInfo(TheArch);
  bool IsWin64 = (TheArch == Arch::X64 && Fmt == BinaryFormat::COFF);

  // --- Determine calling convention ---
  if (TheArch == Arch::X64)
    Func.CC = IsWin64 ? CallingConv::Win64 : CallingConv::SysV_AMD64;
  else if (TheArch == Arch::AArch64 || TheArch == Arch::ARM)
    Func.CC = CallingConv::ARM_AAPCS;

  // --- Build the ordered parameter register list ---
  std::vector<uint64_t> ParamRegs;
  if (IsWin64) {
    ParamRegs.assign(TRI.Win64ParamRegs.begin(), TRI.Win64ParamRegs.end());
    for (uint64_t R : TRI.FPParamRegs)
      if (ParamRegs.size() < TRI.Win64ParamRegs.size() + 4)
        ParamRegs.push_back(R);
  } else {
    // Floating-point arguments use a *separate* register class with its own
    // argument index (XMM0-7, V0-7, ARM D0-7), recovered by detectXMMParams.
    // Folding them into the integer parameter list would make
    // detectRegisterParams synthesize bogus integer placeholders up to the FP
    // index (so a `double f(double,double)` would become N integer placeholders
    // + two FP params).  The FP registers are detected separately below.
    ParamRegs.assign(TRI.IntParamRegs.begin(), TRI.IntParamRegs.end());
  }

  // --- Detect register-passed parameters ---
  if (!Func.Blocks.empty()) {
    const auto &Entry = Func.Blocks[0];
    auto UsedRegs = findLiveInParamRegs(Entry, ParamRegs);
    detectRegisterParams(Func, TRI, ParamRegs, UsedRegs, TargetArch);

    // Detect a variadic prologue (register save area + va_start) so the FP save
    // area is not mistaken for FP parameters and the overflow area is recovered
    // specially once all call sites are known (Pipeline
    // finalizeVariadicCallees).
    detectVariadic(Func, TRI, TargetArch, Fmt);

    // x86-64/x86: also check for XMM floating-point parameters (no-op on ARM).
    // A variadic function's FP register save area is not its parameter list.
    if (!Func.IsVariadic)
      detectXMMParams(Func, Entry, TRI, RegVarMap, TargetArch);
  }

  // x86-64 / AArch64 / ARM: recover stack-passed arguments beyond the parameter
  // registers (no-op on i386, handled by detectCdeclStackParams below).  A
  // variadic function reads its overflow stack arguments through the va_arg
  // pointer walk (PHI-merged, vectorized), invisible to this load scan; those
  // are recovered from the call sites in finalizeVariadicCallees instead.
  if (!Func.IsVariadic)
    detectStackParams(Func, TargetArch);
  else if (TargetArch == Arch::AArch64 && Fmt == BinaryFormat::MachO &&
           Func.VariadicOverflowBase > 0) {
    // Darwin AArch64 variadic with more than 8 named integer args: args 9.. are
    // stack-passed FIXED args sitting below the overflow pointer.  Recover that
    // named prefix only (offsets [0, overflow_base)); the variadic args at and
    // above the overflow base are read through the va_arg walk (invisible to
    // the load scan) and finalized from the call sites.  Record how many named
    // stack params were recovered so finalizeVariadicCallees sizes the fixed
    // prefix as register params + these (the overflow count is everything past
    // them).
    const size_t Before = Func.Params.size();
    detectStackParams(Func, TargetArch, Func.VariadicOverflowBase);
    Func.VariadicFixedStackArgs = static_cast<int>(Func.Params.size() - Before);
  }

  // i386 CDECL: fall back to stack-passed parameters (no-op off 32-bit x86).
  detectCdeclStackParams(Func, TargetArch);

  // AArch64: recover the hidden indirect-result (sret) pointer in x8 for a
  // by-value aggregate return (no-op on targets without a dedicated x8).
  if (!Func.Blocks.empty())
    detectIndirectResultParam(Func, Func.Blocks[0], TRI, TheArch);

  // --- Common: collect stack locals and compute frame size ---
  collectStackLocals(Func, StackSlots);
  computeFrameBounds(Func, StackSlots);
}

} // namespace neverd
