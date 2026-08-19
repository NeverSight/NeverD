//===- MedABIPassSupport.cpp - ABI-recovery slicing helpers ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Data-flow and stack-offset tracing helpers backing the per-call-site
/// argument scans in recoverCallAbi (MedABIPass.cpp): indirect-target
/// resolution, stack-pointer offset tracing, and cross-block argument-register
/// recovery.  See MedABIPassDetail.h for the shared declarations.
///
//===----------------------------------------------------------------------===//

#include "MedABIPassDetail.h"

#include "neverd/Common.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

// Canonical (base value, constant byte offset) of an address \p V, following
// the offset-preserving casts (COPY / ZEXT / SEXT / SUBBYTES@0) and the
// constant add/sub chain.  Bottoms out at the deepest value with no in-block
// definition (a live-in register such as a frame pointer).  Lets a spill and
// its reload be recognized as the *same* stack slot by structural address
// equality, without resolving a (possibly cross-block) frame-pointer base to an
// entry-SP offset.
static std::pair<MedVar, int64_t>
reduceAddr(const MedBlock &Blk, const MedVar &V, int64_t Off, int Depth) {
  if (Depth > 128 || V.isConst())
    return {V, Off};
  const MedOp *Def = nullptr;
  for (auto &Op : Blk.Ops)
    if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
        Op.Output.SSAVer == V.SSAVer && Op.Output.RegOff == V.RegOff) {
      Def = &Op;
      break;
    }
  if (!Def)
    return {V, Off};
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def->NumInputs >= 1 ? reduceAddr(Blk, Def->Inputs[0], Off, Depth + 1)
                               : std::pair<MedVar, int64_t>{V, Off};
  case NdOp::SUBBYTES:
    return (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
            Def->Inputs[1].ConstVal == 0)
               ? reduceAddr(Blk, Def->Inputs[0], Off, Depth + 1)
               : std::pair<MedVar, int64_t>{V, Off};
  case NdOp::INT_ADD:
    for (uint8_t I = 0; I < Def->NumInputs; ++I)
      if (Def->Inputs[I].isConst())
        for (uint8_t J = 0; J < Def->NumInputs; ++J)
          if (!Def->Inputs[J].isConst())
            return reduceAddr(
                Blk, Def->Inputs[J],
                Off + static_cast<int64_t>(Def->Inputs[I].ConstVal), Depth + 1);
    return {V, Off};
  case NdOp::INT_SUB:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst())
      return reduceAddr(Blk, Def->Inputs[0],
                        Off - static_cast<int64_t>(Def->Inputs[1].ConstVal),
                        Depth + 1);
    return {V, Off};
  default:
    return {V, Off};
  }
}

// Whether two reduced addresses name the same memory: identical base value
// (kind/id/version/register) and identical byte offset.
static bool sameReducedAddr(const std::pair<MedVar, int64_t> &A,
                            const std::pair<MedVar, int64_t> &B) {
  return A.second == B.second && A.first.Kind == B.first.Kind &&
         A.first.Id == B.first.Id && A.first.SSAVer == B.first.SSAVer &&
         A.first.RegOff == B.first.RegOff;
}

// Resolve an indirect call target to a constant code address when the call goes
// through a function pointer that provably holds a known function (`fp = &F;
// ...; fp(...)`).  Follows COPY / width-cast chains and a single stack-slot
// store/load round trip (the pointer parked in a local), matching the load's
// address to a prior store's address by structural equality (reduceAddr /
// sameReducedAddr).  Returns the resolved address, or 0 when not provable -- a
// conservative best effort within the call's own block that never changes
// behavior unless the target is proven, so non-resolvable indirect calls keep
// their existing (heuristic) handling.
va_t resolveIndirectTargetAddr(const MedBlock &Blk, int FromIdx,
                               const MedVar &V, int Depth) {
  if (Depth > 16)
    return 0;
  if (V.isConst())
    return V.ConstVal;
  int DefIdx = -1;
  for (int J = FromIdx - 1; J >= 0; --J) {
    const auto &O = Blk.Ops[J];
    if (O.Output.Kind == V.Kind && O.Output.Id == V.Id &&
        O.Output.SSAVer == V.SSAVer && O.Output.RegOff == V.RegOff) {
      DefIdx = J;
      break;
    }
  }
  if (DefIdx < 0)
    return 0;
  const MedOp &Def = Blk.Ops[DefIdx];
  switch (Def.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def.NumInputs >= 1 ? resolveIndirectTargetAddr(
                                    Blk, DefIdx, Def.Inputs[0], Depth + 1)
                              : 0;
  case NdOp::SUBBYTES:
    return (Def.NumInputs >= 2 && Def.Inputs[1].isConst() &&
            Def.Inputs[1].ConstVal == 0)
               ? resolveIndirectTargetAddr(Blk, DefIdx, Def.Inputs[0],
                                           Depth + 1)
               : 0;
  case NdOp::LOAD: {
    if (Def.NumInputs < 1)
      return 0;
    auto LoadAddr = reduceAddr(Blk, Def.Inputs[0], 0, 0);
    // Nearest prior store to the same slot (memory is not SSA; scan backward).
    for (int J = DefIdx - 1; J >= 0; --J) {
      const auto &O = Blk.Ops[J];
      if (O.Opcode != NdOp::STORE || O.NumInputs < 2)
        continue;
      if (sameReducedAddr(LoadAddr, reduceAddr(Blk, O.Inputs[0], 0, 0)))
        return resolveIndirectTargetAddr(Blk, J, O.Inputs[1], Depth + 1);
    }
    return 0;
  }
  default:
    return 0;
  }
}

std::optional<int> resolveIndirectTargetArgIdx(const MedBlock &Blk, int FromIdx,
                                               const TargetRegInfo &TRI,
                                               const MedVar &V, int Depth) {
  if (Depth > 16 || V.isConst())
    return std::nullopt;

  int DefIdx = -1;
  for (int J = FromIdx - 1; J >= 0; --J) {
    const auto &O = Blk.Ops[J];
    if (O.Output.Kind == V.Kind && O.Output.Id == V.Id &&
        O.Output.SSAVer == V.SSAVer && O.Output.RegOff == V.RegOff) {
      DefIdx = J;
      break;
    }
  }
  if (DefIdx < 0) {
    if (V.Kind != MedVar::Reg && V.Kind != MedVar::Param)
      return std::nullopt;
    int ArgIdx = TRI.regToArgIdx(V.RegOff);
    return ArgIdx >= 0 ? std::optional<int>(ArgIdx) : std::nullopt;
  }

  const MedOp &Def = Blk.Ops[DefIdx];
  switch (Def.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    if (Def.NumInputs < 1)
      return std::nullopt;
    // Entry live-ins are represented by a self-copy.  Treat that as the
    // provenance root instead of recursing through the identical SSA value.
    if (Def.Inputs[0].Kind == V.Kind && Def.Inputs[0].Id == V.Id &&
        Def.Inputs[0].SSAVer == V.SSAVer && Def.Inputs[0].RegOff == V.RegOff) {
      int ArgIdx = TRI.regToArgIdx(V.RegOff);
      return ArgIdx >= 0 ? std::optional<int>(ArgIdx) : std::nullopt;
    }
    return resolveIndirectTargetArgIdx(Blk, DefIdx, TRI, Def.Inputs[0],
                                       Depth + 1);
  case NdOp::SUBBYTES:
    return (Def.NumInputs >= 2 && Def.Inputs[1].isConst() &&
            Def.Inputs[1].ConstVal == 0)
               ? resolveIndirectTargetArgIdx(Blk, DefIdx, TRI, Def.Inputs[0],
                                             Depth + 1)
               : std::nullopt;
  case NdOp::LOAD: {
    if (Def.NumInputs < 1)
      return std::nullopt;
    auto LoadAddr = reduceAddr(Blk, Def.Inputs[0], 0, 0);
    for (int J = DefIdx - 1; J >= 0; --J) {
      const auto &O = Blk.Ops[J];
      if (O.Opcode != NdOp::STORE || O.NumInputs < 2)
        continue;
      if (sameReducedAddr(LoadAddr, reduceAddr(Blk, O.Inputs[0], 0, 0)))
        return resolveIndirectTargetArgIdx(Blk, J, TRI, O.Inputs[1], Depth + 1);
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

// Offset of \p V relative to the function-entry stack pointer, following the SP
// definition chain through constant add/sub decrements (cdecl `push`/`sub esp`)
// and width casts.  Lets the stack-argument scan place each pushed argument at
// its true distance from the call-site SP even when successive `push`es
// retarget a moving stack pointer.  nullopt when \p V does not derive from the
// SP.
std::optional<int64_t> stackPtrDelta(const MedBlock &Blk,
                                     const TargetRegInfo &TRI, const MedVar &V,
                                     int Depth) {
  if (Depth > 128 || V.isConst())
    return std::nullopt;

  const MedOp *Def = nullptr;
  for (auto &Op : Blk.Ops)
    if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
        Op.Output.SSAVer == V.SSAVer && Op.Output.RegOff == V.RegOff) {
      Def = &Op;
      break;
    }
  if (!Def) {
    // No in-block definition: the live-in stack pointer is the zero reference;
    // anything else is not SP-derived.
    if (V.Kind == MedVar::Reg && V.RegOff == TRI.StackPointer)
      return 0;
    return std::nullopt;
  }

  switch (Def->Opcode) {
  case NdOp::COPY:
    if (Def->NumInputs >= 1) {
      const MedVar &In = Def->Inputs[0];
      // The entry stack-pointer self-copy (`COPY ESP, ESP`, the live-in marker)
      // is the zero reference; following it would recurse forever to the depth
      // cap and report the whole push chain as un-traceable, collapsing every
      // pushed argument onto slot 0.
      if (In.Kind == MedVar::Reg && In.RegOff == TRI.StackPointer &&
          In.Id == V.Id && In.SSAVer == V.SSAVer)
        return 0;
      return stackPtrDelta(Blk, TRI, In, Depth + 1);
    }
    return std::nullopt;
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def->NumInputs >= 1
               ? stackPtrDelta(Blk, TRI, Def->Inputs[0], Depth + 1)
               : std::nullopt;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB: {
    if (Def->NumInputs < 2)
      return std::nullopt;
    std::optional<int64_t> Base;
    int64_t K = 0;
    bool HaveK = false;
    for (uint8_t I = 0; I < Def->NumInputs; ++I) {
      if (Def->Inputs[I].isConst()) {
        K = static_cast<int64_t>(Def->Inputs[I].ConstVal);
        HaveK = true;
      } else if (!Base) {
        Base = stackPtrDelta(Blk, TRI, Def->Inputs[I], Depth + 1);
      }
    }
    if (!Base || !HaveK)
      return std::nullopt;
    return Def->Opcode == NdOp::INT_ADD ? *Base + K : *Base - K;
  }
  case NdOp::LOAD: {
    // Store-to-load forwarding for a spilled-and-reloaded stack pointer.  Clang
    // at -O0 with a long outgoing-argument list parks the argument-area base (a
    // copy of ESP) in a frame slot and reloads it before the call; the reloaded
    // base is otherwise opaque, so every argument addressed through it would be
    // dropped and the recovered call truncated at the first gap.  Match the
    // load against the nearest prior store to the *same* address (compared
    // structurally, so a frame-pointer-relative spill slot resolves without
    // tracing the cross-block frame pointer to an entry-SP offset) and forward
    // that store's value, yielding the spilled SP delta.
    if (Def->NumInputs < 1)
      return std::nullopt;
    auto LoadAddr = reduceAddr(Blk, Def->Inputs[0], 0, 0);
    size_t DefIdx = static_cast<size_t>(Def - Blk.Ops.data());
    for (size_t I = DefIdx; I-- > 0;) {
      const MedOp &S = Blk.Ops[I];
      if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
        continue;
      if (!sameReducedAddr(reduceAddr(Blk, S.Inputs[0], 0, 0), LoadAddr))
        continue; // a store to a different slot does not define this load
      // The nearest prior store to this slot defines the loaded value: forward
      // its SP delta (nullopt when the slot holds a non-pointer).
      return stackPtrDelta(Blk, TRI, S.Inputs[1], Depth + 1);
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

// Whether \p V is, or derives from (through copies, width casts and a constant
// add/sub), a frame register -- the stack OR frame pointer.  Unlike
// stackPtrDelta this also accepts frame-pointer (x29) relative addresses, which
// clang -O0 uses for stack buffers once the frame is large enough.  Used to
// confirm an AArch64 indirect-call x8 value is a genuine stack buffer pointer
// (the sret result slot, `add x8, sp/fp, #k`) rather than an unrelated scratch
// value, so the hidden sret-buffer argument is recovered only for real
// indirect-sret call sites.
bool derivesFromFrameReg(const MedBlock &Blk, const TargetRegInfo &TRI,
                         const MedVar &V, int Depth) {
  if (Depth > 64 || V.isConst())
    return false;
  if (V.Kind == MedVar::Reg && TRI.isFrameReg(V.RegOff))
    return true;
  const MedOp *Def = nullptr;
  for (const auto &Op : Blk.Ops)
    if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
        Op.Output.SSAVer == V.SSAVer && Op.Output.RegOff == V.RegOff) {
      Def = &Op;
      break;
    }
  if (!Def)
    return false;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def->NumInputs >= 1 &&
           derivesFromFrameReg(Blk, TRI, Def->Inputs[0], Depth + 1);
  case NdOp::INT_ADD:
  case NdOp::INT_SUB: {
    // A frame-relative buffer is `frame_reg +/- constant`: one operand derives
    // from a frame register, the other is a constant displacement.
    bool HaveBase = false, HaveConst = false;
    for (uint8_t I = 0; I < Def->NumInputs; ++I) {
      if (Def->Inputs[I].isConst())
        HaveConst = true;
      else if (derivesFromFrameReg(Blk, TRI, Def->Inputs[I], Depth + 1))
        HaveBase = true;
    }
    return HaveBase && HaveConst;
  }
  default:
    return false;
  }
}

// Identifies an SSA stack-pointer value (Id, version, register offset).
static SpOffsetKey spOffsetKey(const MedVar &V) {
  return {V.Id, V.SSAVer, V.RegOff};
}

// Finds the in-block definition of \p V (matching kind/id/version/regoff).
static const MedOp *findBlockDef(const MedBlock &Blk, const MedVar &V) {
  for (auto &Op : Blk.Ops)
    if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
        Op.Output.SSAVer == V.SSAVer && Op.Output.RegOff == V.RegOff)
      return &Op;
  return nullptr;
}

// Records, for each stack-pointer value on the call-site SP's definition chain,
// its byte offset *above* the call SP (call SP = 0, each earlier `push`'s SP
// one slot higher).  Offset-preserving casts (SUBBYTES/ZEXT/SEXT/COPY) keep the
// offset; a `sub`/`add` of a constant shifts it.  Used to place pushed
// arguments relative to the call SP when the absolute entry-relative delta is
// unavailable (a post-loop push chain whose SP threads a loop-carried PHI,
// where the chain round-trips ESP<->RSP and never reaches the entry SP as a
// constant).
void buildCallSpOffsets(const MedBlock &Blk, const TargetRegInfo &TRI,
                        const MedVar &V, int64_t Off,
                        std::map<SpOffsetKey, int64_t> &Map, int Depth) {
  if (Depth > 128 || V.isConst())
    return;
  if (!Map.emplace(spOffsetKey(V), Off).second)
    return; // already visited (e.g. the entry SP self-copy)
  const MedOp *Def = findBlockDef(Blk, V);
  if (!Def)
    return;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    if (Def->NumInputs >= 1)
      buildCallSpOffsets(Blk, TRI, Def->Inputs[0], Off, Map, Depth + 1);
    break;
  case NdOp::INT_SUB:
    // V = base - C  =>  base sits C higher  =>  base offset = Off + C.
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst())
      buildCallSpOffsets(Blk, TRI, Def->Inputs[0],
                         Off + static_cast<int64_t>(Def->Inputs[1].ConstVal),
                         Map, Depth + 1);
    break;
  case NdOp::INT_ADD:
    // V = base + C  =>  base sits C lower  =>  base offset = Off - C.
    if (Def->NumInputs >= 2) {
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (Def->Inputs[I].isConst())
          for (uint8_t J = 0; J < Def->NumInputs; ++J)
            if (!Def->Inputs[J].isConst())
              buildCallSpOffsets(
                  Blk, TRI, Def->Inputs[J],
                  Off - static_cast<int64_t>(Def->Inputs[I].ConstVal), Map,
                  Depth + 1);
    }
    break;
  default:
    break;
  }
}

// Offset of store-address \p V above the call SP, resolved against the call
// SP's offset map.  Follows \p V's definition chain (offset-preserving casts,
// and constant add/sub) until it reaches a value on the call SP chain.  nullopt
// when the address is not stack-pointer derived.
std::optional<int64_t> relStackOff(const MedBlock &Blk,
                                   const TargetRegInfo &TRI, const MedVar &V,
                                   const std::map<SpOffsetKey, int64_t> &Map,
                                   int Depth) {
  if (Depth > 128 || V.isConst())
    return std::nullopt;
  if (auto It = Map.find(spOffsetKey(V)); It != Map.end())
    return It->second;
  const MedOp *Def = findBlockDef(Blk, V);
  if (!Def)
    return std::nullopt;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def->NumInputs >= 1
               ? relStackOff(Blk, TRI, Def->Inputs[0], Map, Depth + 1)
               : std::nullopt;
  case NdOp::INT_SUB:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst())
      if (auto B = relStackOff(Blk, TRI, Def->Inputs[0], Map, Depth + 1))
        return *B - static_cast<int64_t>(Def->Inputs[1].ConstVal);
    return std::nullopt;
  case NdOp::INT_ADD:
    if (Def->NumInputs >= 2) {
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (Def->Inputs[I].isConst())
          for (uint8_t J = 0; J < Def->NumInputs; ++J)
            if (!Def->Inputs[J].isConst())
              if (auto B =
                      relStackOff(Blk, TRI, Def->Inputs[J], Map, Depth + 1))
                return *B + static_cast<int64_t>(Def->Inputs[I].ConstVal);
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

std::string relocCalleeName(const BinaryImage &Img, va_t InsnAddr) {
  for (const auto &Rel : Img.Relocations) {
    // The branch relocation sits at the instruction's displacement field: at
    // the instruction address itself for AArch64/ARM `bl` (the imm is in the
    // opcode word), but one byte in for x86 `call rel32` (the 0xE8 opcode
    // precedes the 4-byte displacement).  Accept both so a relocatable-object
    // external call is named on x86 too (otherwise its placeholder-0 target
    // resolves to whatever symbol sits at VA 0 — e.g. the function itself,
    // lifting a libc call into a bogus self-recursion).
    if ((Rel.Address != InsnAddr && Rel.Address != InsnAddr + 1) ||
        Rel.SymbolName.empty())
      continue;
    llvm::StringRef Name = stripLeadingUnderscores(Rel.SymbolName);
    if (libc::isKnownFunction(Name))
      return Name.str();
    return Rel.SymbolName;
  }
  return {};
}

// The value an argument-register-defining op contributes to its slot: the
// source of a copy, or — for a sub-register sync that narrows the register's
// own wider value (`EDI := low32(RDI)` emitted after a 64-bit write) — that
// wider value, so a struct or other wide argument keeps its high bits instead
// of the truncated sub-register view.  Otherwise the op's output itself.
static MedVar argRegSourceValue(const MedOp &Op) {
  if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1)
    return Op.Inputs[0];
  if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
      Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal == 0 &&
      Op.Inputs[0].Kind == MedVar::Reg &&
      Op.Inputs[0].RegOff == Op.Output.RegOff &&
      Op.Inputs[0].Size > Op.Output.Size)
    return Op.Inputs[0];
  return Op.Output;
}

// argRegSourceValue with block context: resolves a low-half sub-register sync
// whose source is a *temporary* (`SUBBYTES Wn = subpiece(t, 0)`, not the
// `subpiece(Xn, 0)` of the register itself that argRegSourceValue already
// widens) to the full-width value of a paired full-register write of the same
// source (`COPY Xn = t`) appearing just before it in the block.  The backward
// register scan meets the 32-bit sync before the 64-bit write, so without this
// a pointer written whole to an argument register and then synced to its 32-bit
// view -- a FILE* loaded into x1 right before an external `fputs` -- would be
// recovered as the truncated low 32 bits, yielding a wild pointer at run time.
MedVar argRegSourceValueInBlock(const MedBlock &Blk, int J,
                                const TargetRegInfo &TRI) {
  const MedOp &Op = Blk.Ops[J];
  MedVar Base = argRegSourceValue(Op);
  // Only a sub-register sync whose source is not the register itself needs the
  // sibling lookup; everything else (a plain copy, or `subpiece(Xn, 0)`) is
  // already resolved by argRegSourceValue.
  if (Op.Opcode != NdOp::SUBBYTES || Op.NumInputs < 2 ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].ConstVal != 0 ||
      Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg ||
      (Op.Inputs[0].Kind == MedVar::Reg &&
       Op.Inputs[0].RegOff == Op.Output.RegOff))
    return Base;
  const MedVar &Src = Op.Inputs[0];
  const int ArgIdx = TRI.regToArgIdx(Op.Output.RegOff);
  for (int K = J - 1; K >= 0; --K) {
    const MedOp &W = Blk.Ops[K];
    if (W.Output.Kind != MedVar::Reg ||
        TRI.regToArgIdx(W.Output.RegOff) != ArgIdx)
      continue;
    // The nearest earlier write to this same argument register: when it is a
    // wider write of the same source value, it is the full-width definition the
    // sub-register sync mirrors.
    if (W.Output.Size > Op.Output.Size && W.NumInputs >= 1 &&
        !W.Inputs[0].isConst() && W.Inputs[0].Kind == Src.Kind &&
        W.Inputs[0].Id == Src.Id && W.Inputs[0].SSAVer == Src.SSAVer)
      return argRegSourceValue(W);
    break; // only the nearest earlier write to this register is the pair
  }
  return Base;
}

const PhiNode *selectAuthoritativeArgPhi(const MedFunc &Func,
                                         const MedBlock &Block,
                                         const TargetRegInfo &TRI, int ArgIdx) {
  // AArch64 may carry both Wn and Xn PHIs for one ABI argument.  Ordinarily the
  // full-width view is authoritative, but a wide alias synthesized only by a
  // loop back-edge is undef on the first iteration.  Prefer a PHI whose value
  // is genuinely defined on every function-entry edge, then prefer width.
  auto entryEdgesSeeded = [&](const PhiNode &Phi) {
    for (const auto &A : Phi.Args) {
      const MedBlock *Pred = nullptr;
      for (const auto &B : Func.Blocks)
        if (B.Id == A.first) {
          Pred = &B;
          break;
        }
      if (!Pred)
        return false;
      if (!Pred->Preds.empty())
        continue;
      if (A.second.isConst())
        continue;

      bool Defined = false;
      for (const auto &P : Pred->Phis)
        if (P.Output.Id == A.second.Id && P.Output.SSAVer == A.second.SSAVer) {
          Defined = true;
          break;
        }
      if (!Defined)
        for (const auto &O : Pred->Ops)
          if (O.Output.Id == A.second.Id &&
              O.Output.SSAVer == A.second.SSAVer && O.Output.Size > 0) {
            Defined = true;
            break;
          }
      if (!Defined)
        for (const auto &P : Func.Params)
          if ((A.second.Kind == MedVar::Reg ||
               A.second.Kind == MedVar::Param) &&
              P.RegOff == A.second.RegOff && P.Size == A.second.Size) {
            Defined = true;
            break;
          }
      if (!Defined)
        return false;
    }
    return true; // no undef value on a function-entry predecessor
  };

  const PhiNode *Best = nullptr;
  bool BestSeeded = false;
  for (const auto &Phi : Block.Phis) {
    if (Phi.Output.Kind != MedVar::Reg ||
        TRI.regToArgIdx(Phi.Output.RegOff) != ArgIdx)
      continue;
    const bool PhiSeeded = entryEdgesSeeded(Phi);
    if (!Best ||
        (PhiSeeded != BestSeeded ? PhiSeeded
                                 : Phi.Output.Size > Best->Output.Size)) {
      Best = &Phi;
      BestSeeded = PhiSeeded;
    }
  }
  return Best;
}

// Value reaching argument register \p ArgIdx at the call in (\p BlockId,
// \p OpIdx), found by walking the CFG backwards into predecessor blocks: the
// nearest write to that argument register, then a block PHI for it.  Returns
// the reaching value or nullopt when the register is never set on any path to
// the call.  Recovers a register argument materialised in a block that
// *dominates* the call rather than the call's own block — e.g. a loop-invariant
// count set before a vectorised loop whose predicated branches push the call
// downstream, which the in-block and in-block-PHI scans (call block only)
// cannot see.
//
// When \p AllowUnknownLiveIn is set the caller has bounded this index to the
// callee's arity, so a parameter register with no reaching definition is the
// incoming argument of a forwarder and is recovered as a live-in even when it
// is not yet a recorded parameter of the function.
std::optional<MedVar> findReachingArgReg(const MedFunc &Func,
                                         const TargetRegInfo &TRI, Arch TheArch,
                                         int BlockId, int ArgIdx,
                                         bool AllowUnknownLiveIn,
                                         bool *FromLiveIn) {
  std::map<int, const MedBlock *> ById;
  for (const auto &B : Func.Blocks)
    ById[B.Id] = &B;

  auto scanBlock = [&](const MedBlock &B, int UpTo) -> std::optional<MedVar> {
    for (int J = UpTo - 1; J >= 0; --J) {
      const auto &Op = B.Ops[J];
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0 &&
          TRI.regToArgIdx(Op.Output.RegOff) == ArgIdx)
        return argRegSourceValueInBlock(B, J, TRI);
    }
    if (const PhiNode *Phi = selectAuthoritativeArgPhi(Func, B, TRI, ArgIdx))
      return Phi->Output;
    return std::nullopt;
  };

  auto It = ById.find(BlockId);
  if (It == ById.end())
    return std::nullopt;
  // The call block's straight-line ops were already handled by the caller (with
  // its call-boundary stop); only its PHIs and the predecessor chain remain.
  if (const PhiNode *Phi =
          selectAuthoritativeArgPhi(Func, *It->second, TRI, ArgIdx))
    return Phi->Output;

  std::set<int> Visited{BlockId};
  std::vector<int> Work(It->second->Preds.begin(), It->second->Preds.end());
  while (!Work.empty()) {
    int BId = Work.back();
    Work.pop_back();
    if (!Visited.insert(BId).second)
      continue;
    auto BIt = ById.find(BId);
    if (BIt == ById.end())
      continue;
    if (auto V =
            scanBlock(*BIt->second, static_cast<int>(BIt->second->Ops.size())))
      return V;
    Work.insert(Work.end(), BIt->second->Preds.begin(),
                BIt->second->Preds.end());
  }

  // No in-function definition reaches the call: the value is the live-in
  // (incoming parameter), as when a caller forwards its own argument straight
  // to the callee (`return callee(a)`), leaving no write for the scans to find.
  // Recover it only when this argument register is a real parameter of the
  // current function, so an uninitialised caller-saved register is never
  // invented as an argument.
  if (ArgIdx >= 0 && ArgIdx < static_cast<int>(TRI.IntParamRegs.size())) {
    uint64_t Reg = TRI.IntParamRegs[ArgIdx];
    for (const auto &P : Func.Params)
      if ((P.Kind == MedVar::Reg || P.Kind == MedVar::Param) &&
          P.RegOff == Reg) {
        MedVar V;
        V.Kind = MedVar::Reg;
        V.RegOff = Reg;
        V.SSAVer = 0;
        V.Size = P.Size > 0 ? P.Size : static_cast<uint16_t>(TRI.PointerSize);
        V.TheArch = TheArch;
        return V;
      }
    if (AllowUnknownLiveIn) {
      MedVar V;
      V.Kind = MedVar::Reg;
      V.RegOff = Reg;
      V.SSAVer = 0;
      V.Size = static_cast<uint16_t>(TRI.PointerSize);
      V.TheArch = TheArch;
      if (FromLiveIn)
        *FromLiveIn = true;
      return V;
    }
  }
  return std::nullopt;
}

} // namespace neverd
