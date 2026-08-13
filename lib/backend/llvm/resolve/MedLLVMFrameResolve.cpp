//===- MedLLVMFrameResolve.cpp - Frame/stack-pointer classification ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Frame-derived / stack-pointer classification and dynamic (VLA) stack-
/// allocation recovery for MedLLVMEmitter.  These answer "does this address
/// trace back to the frame / stack pointer?" and rebuild `alloca`s from the
/// `sp - size` idiom, feeding getVar/setVar in MedLLVMVarAccess.cpp.  Shared
/// address tracing remains in MedLLVMAddrResolve.cpp; literal/select,
/// indexed/induction, and code-pointer resolution live in their dedicated
/// translation units.  Every routine here is a MedLLVMEmitter member declared
/// in the shared header, so this is a pure translation-unit split.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <utility>

namespace neverd {

namespace {
// {(Id<<32)|SSAVer, Kind} key for the per-function frame-derived memo, matching
// the DefIndex/PhiIndex scheme (lossless for 32-bit Id/SSAVer; the small Kind
// enum never collides with DenseMap's int sentinels).
std::pair<int64_t, int> frameKey(const MedVar &V) {
  return {static_cast<int64_t>(
              (static_cast<uint64_t>(static_cast<uint32_t>(V.Id)) << 32) |
              static_cast<uint32_t>(V.SSAVer)),
          static_cast<int>(V.Kind)};
}
} // namespace

// Recursive worker backed by a visited set, so each distinct (Kind,Id,SSAVer)
// SSA value in the operand DAG is examined at most once.  Without it, a value
// reachable through many shared sub-expressions — e.g. a 16-way switch whose
// merge PHI feeds a loop-carried i64 accumulator, where the i386 register-pair
// halves fan back into the loop-header PHI — is re-expanded along every path,
// which is exponential (observed as a ~50s lift of a single i386 function).
// The visited set makes the walk linear in the value graph and also guarantees
// termination on cyclic SSA (loop back-edges), so no recursion-depth cap is
// needed: the answer is the exact "does this value's address arithmetic trace
// back to the stack pointer", independent of the path/depth at which a node
// happens to be reached.  Definitions/PHIs are resolved through the O(1)
// lookupDef/lookupPhi index instead of rescanning every op in the function,
// which made this the dominant cost of emit() on large functions.
bool MedLLVMEmitter::frameDerivedRec(
    const MedVar &V, llvm::DenseSet<std::pair<int64_t, int>> &Visited) const {
  if (V.isConst() || !CurMedFunc)
    return false;

  if (V.Kind == MedVar::Reg) {
    uint64_t SpOff = getTargetRegInfo(TargetArch).StackPointer;
    if (SpOff != 0 && V.RegOff == SpOff)
      return true;
  }

  // Examine each SSA value once; revisits of a shared node or a back-edge
  // collapse here instead of fanning out into an exponential re-expansion.
  if (!Visited.insert(frameKey(V)).second)
    return false;

  const MedOp *Def = lookupDef(V);
  if (!Def) {
    // A PHI fed by any frame-derived predecessor — an induction pointer walking
    // a stack array, `p = PHI(sp-k, p+stride)` — is itself frame-derived.  PHIs
    // live in Blk.Phis, not Blk.Ops, so lookupDef never sees them; without this
    // the induction store address is mistaken for an absolute const-base store,
    // poisoning StoredConstBases and disabling rodata table redirection for the
    // whole function (e.g. clang's table-driven CRC alongside msg[]).
    if (const PhiNode *Phi = lookupPhi(V))
      for (const auto &[PredId, Arg] : Phi->Args)
        if (frameDerivedRec(Arg, Visited))
          return true;
    return false;
  }

  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
    for (int I = 0; I < Def->NumInputs; ++I)
      if (frameDerivedRec(Def->Inputs[I], Visited))
        return true;
    return false;
  default:
    return false;
  }
}

bool MedLLVMEmitter::varIsFrameDerived(const MedVar &V, int /*Depth*/) const {
  if (V.isConst() || !CurMedFunc)
    return false;

  // The frame-derived property of a value is a pure function of the operand
  // graph, so memoize per top-level query and reuse across the many per-operand
  // queries the op emitter makes for the same function.
  if (FrameDerivedCacheFor != CurMedFunc) {
    FrameDerivedCacheFor = CurMedFunc;
    FrameDerivedCache.clear();
  }
  auto Key = frameKey(V);
  auto It = FrameDerivedCache.find(Key);
  if (It != FrameDerivedCache.end())
    return It->second;

  llvm::DenseSet<std::pair<int64_t, int>> Visited;
  bool Result = frameDerivedRec(V, Visited);
  FrameDerivedCache[Key] = Result;
  return Result;
}

bool MedLLVMEmitter::varIsStackPtrDerived(const MedVar &V, int Depth) const {
  const uint64_t SpOff = getTargetRegInfo(TargetArch).StackPointer;
  // Cycle-detecting walk: a loop-carried stack pointer is defined by a PHI
  // whose back-edge feeds itself (`sp_next = phi(sp_init, sp_next - size)`), so
  // the recursion must not re-enter a PHI it is already exploring.
  std::set<std::pair<int, int>> VisitedPhis;
  std::function<bool(const MedVar &, int)> rec = [&](const MedVar &Cur,
                                                     int D) -> bool {
    if (Cur.isConst() || !CurMedFunc || D > limits::kMaxStackPtrTraceDepth)
      return false;
    if (Cur.Kind == MedVar::Reg && Cur.SSAVer == 0 && SpOff != 0 &&
        Cur.RegOff == SpOff)
      return true;
    // An in-loop dynamic allocation subtracts from the loop-carried SP, which
    // is a PHI (`sp_next = phi(sp_entry, sp_prev - size)`).  The PHI is stack-
    // pointer-derived when any incoming value is; the visited set breaks the
    // self-referential back-edge so the cycle terminates (the entry edge
    // reaches the entry SP and proves it).  Without this an alloca inside a
    // loop is left as raw integer SP arithmetic into the fixed logical frame,
    // so its memory aliases unreserved stack a later call clobbers.
    if (const PhiNode *Phi = lookupPhi(Cur)) {
      if (!VisitedPhis.insert({Cur.Id, Cur.SSAVer}).second)
        return false;
      for (const auto &A : Phi->Args)
        if (rec(A.second, D + 1))
          return true;
      return false;
    }
    const MedOp *Def = lookupDef(Cur);
    if (!Def || Def->NumInputs < 1)
      return false;
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::SUBBYTES:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::INT_AND:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      return rec(Def->Inputs[0], D + 1);
    default:
      return false;
    }
  };
  return rec(V, Depth);
}

bool MedLLVMEmitter::varReaches(const MedVar &V, const MedVar &Target,
                                int Depth) const {
  if (!CurMedFunc || Depth > limits::kMaxStackPtrTraceDepth)
    return false;
  if (V.Kind == Target.Kind && V.Id == Target.Id && V.SSAVer == Target.SSAVer)
    return true;
  if (V.isConst())
    return false;
  const MedOp *Def = lookupDef(V);
  if (!Def || Def->NumInputs < 1)
    return false;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return varReaches(Def->Inputs[0], Target, Depth + 1);
  default:
    return false;
  }
}

llvm::Value *
MedLLVMEmitter::tryEmitDynamicStackAlloc(const MedOp &Op,
                                         llvm::IRBuilder<> &Builder) {
  if (!FrameBaseInt || Op.Opcode != NdOp::INT_SUB || Op.NumInputs < 2 ||
      !CurMedFunc)
    return nullptr;
  const MedVar &Size = Op.Inputs[1];
  // A constant decrement is the fixed prologue frame, not a dynamic allocation.
  if (Size.isConst())
    return nullptr;
  // `sp - size`: minuend is the stack pointer, subtrahend a runtime byte count
  // (and not itself a stack pointer, which would be a pointer difference).
  if (!varIsStackPtrDerived(Op.Inputs[0]) || varIsStackPtrDerived(Size))
    return nullptr;
  // The result must become the new stack pointer: written straight to SP
  // (`sub sp,sp,xN`) or copied there through a width chain (i386 routes the
  // value `ecx -> esp`).  This rules out any other `sp - value`.
  uint64_t SpOff = getTargetRegInfo(TargetArch).StackPointer;
  bool ReachesSp = (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == SpOff);
  for (const auto &Blk : CurMedFunc->Blocks) {
    if (ReachesSp)
      break;
    for (const auto &O : Blk.Ops)
      if (O.Output.Kind == MedVar::Reg && O.Output.RegOff == SpOff &&
          O.NumInputs >= 1 && varReaches(O.Inputs[0], Op.Output)) {
        ReachesSp = true;
        break;
      }
  }
  if (!ReachesSp)
    return nullptr;

  llvm::Value *SizeVal = getVar(Size, Builder);
  if (!SizeVal || !SizeVal->getType()->isIntegerTy())
    return nullptr;
  auto *Dyn = Builder.CreateAlloca(llvm::Type::getInt8Ty(*Ctx), SizeVal, "vla");
  Dyn->setAlignment(llvm::Align(16));
  unsigned Bits = Op.Output.Size > 0 ? Op.Output.Size * 8u : 64u;
  auto *VlaSp =
      Builder.CreatePtrToInt(Dyn, llvm::IntegerType::get(*Ctx, Bits), "vla_sp");
  DynVlaBases[sizeRootKey(Size)] = VlaSp;
  return VlaSp;
}

std::pair<int, int> MedLLVMEmitter::sizeRootKey(const MedVar &V,
                                                int Depth) const {
  if (!CurMedFunc || V.isConst() || Depth > limits::kMaxStackPtrTraceDepth)
    return {V.Id, V.SSAVer};
  const MedOp *Def = lookupDef(V);
  if (!Def || Def->NumInputs < 1)
    return {V.Id, V.SSAVer};
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return sizeRootKey(Def->Inputs[0], Depth + 1);
  case NdOp::SUBBYTES:
    return Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
                   Def->Inputs[1].ConstVal == 0
               ? sizeRootKey(Def->Inputs[0], Depth + 1)
               : std::make_pair(V.Id, V.SSAVer);
  default:
    return {V.Id, V.SSAVer};
  }
}

std::optional<std::pair<std::pair<int, int>, int64_t>>
MedLLVMEmitter::addrSlotKey(const MedVar &V, int Depth,
                            bool ThroughRegs) const {
  if (V.isConst() || Depth > limits::kMaxStackPtrTraceDepth)
    return std::nullopt;
  // A register anchors the key (frame/stack pointer base); never trace past it,
  // so a load and store of the same slot share an identical base.  ThroughRegs
  // threads only the TOP-LEVEL register's definition (escape detection): a slot
  // address copied into a parameter register before a call keys to the same
  // slot while the inner stack/frame-pointer register still anchors (threading
  // it would diverge from the load/store form via prologue self-copies).
  if (V.Kind == MedVar::Reg && !(ThroughRegs && Depth == 0))
    return std::make_pair(std::make_pair(V.Id, V.SSAVer), int64_t{0});
  if (!CurMedFunc)
    return std::nullopt;
  const MedOp *Def = lookupDef(V);
  if (!Def || Def->NumInputs < 1)
    return std::make_pair(std::make_pair(V.Id, V.SSAVer), int64_t{0});
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return addrSlotKey(Def->Inputs[0], Depth + 1, ThroughRegs);
  case NdOp::SUBBYTES:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
        Def->Inputs[1].ConstVal == 0)
      return addrSlotKey(Def->Inputs[0], Depth + 1, ThroughRegs);
    return std::make_pair(std::make_pair(V.Id, V.SSAVer), int64_t{0});
  case NdOp::INT_ADD:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst()) {
      if (auto B = addrSlotKey(Def->Inputs[0], Depth + 1, ThroughRegs))
        return std::make_pair(
            B->first,
            B->second + static_cast<int64_t>(Def->Inputs[1].ConstVal));
    }
    if (Def->NumInputs >= 2 && Def->Inputs[0].isConst()) {
      if (auto B = addrSlotKey(Def->Inputs[1], Depth + 1, ThroughRegs))
        return std::make_pair(
            B->first,
            B->second + static_cast<int64_t>(Def->Inputs[0].ConstVal));
    }
    return std::nullopt;
  case NdOp::INT_SUB:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst()) {
      if (auto B = addrSlotKey(Def->Inputs[0], Depth + 1, ThroughRegs))
        return std::make_pair(
            B->first,
            B->second - static_cast<int64_t>(Def->Inputs[1].ConstVal));
    }
    return std::nullopt;
  default:
    return std::make_pair(std::make_pair(V.Id, V.SSAVer), int64_t{0});
  }
}

bool MedLLVMEmitter::varIsReloadedStackPtr(const MedVar &V, int Depth) const {
  if (!CurMedFunc || V.isConst() || Depth > limits::kMaxStackPtrTraceDepth)
    return false;
  const MedOp *Def = lookupDef(V);
  if (!Def || Def->NumInputs < 1)
    return false;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return varIsReloadedStackPtr(Def->Inputs[0], Depth + 1);
  case NdOp::SUBBYTES:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
        Def->Inputs[1].ConstVal == 0)
      return varIsReloadedStackPtr(Def->Inputs[0], Depth + 1);
    return false;
  case NdOp::LOAD: {
    auto LKey = addrSlotKey(Def->Inputs[0]);
    if (!LKey)
      return false;
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &O : Blk.Ops)
        if (O.Opcode == NdOp::STORE && O.NumInputs >= 2 &&
            varIsStackPtrDerived(O.Inputs[1])) {
          auto SKey = addrSlotKey(O.Inputs[0]);
          if (SKey && *SKey == *LKey)
            return true;
        }
    return false;
  }
  default:
    return false;
  }
}

llvm::Value *
MedLLVMEmitter::tryResolveDynVlaAddr(const MedOp &Op,
                                     llvm::IRBuilder<> & /*Builder*/) {
  if (DynVlaBases.empty() || Op.NumInputs < 2 ||
      (Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB))
    return nullptr;

  auto findDef = [&](const MedVar &V) { return lookupDef(V); };

  // The negation behind an INT_ADD's offset (`old_sp + (-size)`): look through
  // the width casts the negated size threads through, then follow an INT_NEG2
  // or `0 - size` to the size itself.  Returns the size operand, or nullptr
  // when the offset is not a negated runtime value.
  auto negatedSize = [&](const MedVar &OffIn) -> const MedVar * {
    MedVar Off = OffIn;
    for (int Guard = 0; Guard <= limits::kMaxStackPtrTraceDepth; ++Guard) {
      const MedOp *Def = findDef(Off);
      if (!Def || Def->NumInputs < 1)
        return nullptr;
      switch (Def->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        Off = Def->Inputs[0];
        continue;
      case NdOp::SUBBYTES:
        if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
            Def->Inputs[1].ConstVal == 0) {
          Off = Def->Inputs[0];
          continue;
        }
        return nullptr;
      case NdOp::INT_NEG2:
        return &Def->Inputs[0];
      case NdOp::INT_SUB:
        if (Def->NumInputs >= 2 && Def->Inputs[0].isConst() &&
            Def->Inputs[0].ConstVal == 0)
          return &Def->Inputs[1];
        return nullptr;
      default:
        return nullptr;
      }
    }
    return nullptr;
  };

  // Candidate (stack-pointer base, runtime size) pairs for this op.
  auto tryPair = [&](const MedVar &Base, const MedVar *SizeV) -> llvm::Value * {
    if (!SizeV || SizeV->isConst() ||
        (!varIsStackPtrDerived(Base) && !varIsReloadedStackPtr(Base)))
      return nullptr;
    auto It = DynVlaBases.find(sizeRootKey(*SizeV));
    return It != DynVlaBases.end() ? It->second : nullptr;
  };

  if (Op.Opcode == NdOp::INT_SUB) {
    if (auto *V = tryPair(Op.Inputs[0], &Op.Inputs[1]))
      return V;
    return nullptr;
  }
  // INT_ADD: either operand may be the SP base; the other must be a negated
  // size.
  if (auto *V = tryPair(Op.Inputs[0], negatedSize(Op.Inputs[1])))
    return V;
  if (auto *V = tryPair(Op.Inputs[1], negatedSize(Op.Inputs[0])))
    return V;
  return nullptr;
}

bool MedLLVMEmitter::isStackProbeCall(const MedOp &Op) const {
  if (!Img || !CurMedFunc || Img->ImportPtrSlots.empty())
    return false;
  if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
      Op.NumInputs < 1)
    return false;

  auto findDef = [&](const MedVar &V) { return lookupDef(V); };

  // Resolve a var to a constant address, threading copy/width casts and a
  // constant-offset add (the folded `GOT_base + slot_offset` form a GOT load
  // computes its address from).
  std::function<bool(const MedVar &, uint64_t &, int)> constAddr =
      [&](const MedVar &V, uint64_t &Out, int Depth) -> bool {
    if (Depth > limits::kMaxStackPtrTraceDepth)
      return false;
    if (V.isConst()) {
      Out = V.ConstVal;
      return true;
    }
    const MedOp *D = findDef(V);
    if (!D || D->NumInputs < 1)
      return false;
    switch (D->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return constAddr(D->Inputs[0], Out, Depth + 1);
    case NdOp::SUBBYTES:
      return D->NumInputs >= 2 && D->Inputs[1].isConst() &&
             D->Inputs[1].ConstVal == 0 &&
             constAddr(D->Inputs[0], Out, Depth + 1);
    case NdOp::INT_ADD: {
      uint64_t A = 0, B = 0;
      if (D->NumInputs >= 2 && constAddr(D->Inputs[0], A, Depth + 1) &&
          constAddr(D->Inputs[1], B, Depth + 1)) {
        Out = A + B;
        return true;
      }
      return false;
    }
    default:
      return false;
    }
  };

  // Find the pointer-slot the call target is loaded from: an indirect call's
  // target is `LOAD <slot>` (possibly threaded through copies).
  uint64_t SlotAddr = 0;
  bool HaveSlot = false;
  const MedVar &Tgt = Op.Inputs[0];
  const MedOp *D = findDef(Tgt);
  for (int Guard = 0; D && Guard <= limits::kMaxStackPtrTraceDepth; ++Guard) {
    if (D->Opcode == NdOp::COPY && D->NumInputs >= 1) {
      D = findDef(D->Inputs[0]);
      continue;
    }
    if (D->Opcode == NdOp::LOAD && D->NumInputs >= 1)
      HaveSlot = constAddr(D->Inputs[0], SlotAddr, 0);
    break;
  }
  // A direct call to the routine's own address (no GOT indirection).
  if (!HaveSlot && Tgt.isConst()) {
    SlotAddr = Tgt.ConstVal;
    HaveSlot = true;
  }
  if (!HaveSlot)
    return false;

  std::string Name;
  if (auto It = Img->ImportPtrSlots.find(SlotAddr);
      It != Img->ImportPtrSlots.end())
    Name = It->second;
  else if (const Import *Imp = Img->findImportAt(SlotAddr))
    Name = Imp->Name;
  if (Name.empty())
    return false;
  return isDarwinStackProbeName(Name);
}

} // namespace neverd
