//===- MedCallingConvX86.cpp - x86 calling convention detection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific calling convention parameter detection:
///   - XMM/SSE register-passed floating-point parameters (x86-64 SysV/Win64)
///   - i386 CDECL stack-passed parameter recovery
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {

//===----------------------------------------------------------------------===//
// XMM / floating-point parameter detection (x86-64)
//===----------------------------------------------------------------------===//

void detectXMMParams(
    MedFunc &Func, const MedBlock &Entry, const TargetRegInfo &TRI,
    const std::map<std::pair<uint64_t, uint16_t>, int> &RegVarMap,
    Arch TargetArch) {
  // FP/vector arguments arrive in a dedicated vector register class under
  // clang's -O2 conventions: x86-64 XMM0-7, AArch64 V0-7, ARM VFP D0-7 — all
  // observed as live-in self-copies.
  if (TRI.VecRegCount == 0)
    return;

  // An entry self-copy alone does not prove a parameter: the lifter emits one
  // for every vector register the body touches, including those used purely as
  // scratch for an FP computation (e.g. an i386 `int` function whose body
  // builds a determinant in XMM0-7, each register first zeroed by `xorps x,x`).
  // A real FP parameter's incoming value reaches a genuine consumer; a scratch
  // register's only flows — possibly carried through PHIs — into
  // self-cancelling idioms (`x^x`, `x-x` = 0) that discard it.  On i386 every
  // argument is stack-passed, so a phantom XMM parameter shifts every real
  // stack argument to a wrong offset; recover the register only when its
  // live-in is truly used.
  auto liveInValueUsed = [&](const MedVar &LiveIn) {
    auto key = [](const MedVar &V) { return std::make_pair(V.Id, V.SSAVer); };
    auto tainted = [](const MedVar &V, const std::set<std::pair<int, int>> &T) {
      return (V.Kind == MedVar::Reg || V.Kind == MedVar::Temp) &&
             T.count(std::make_pair(V.Id, V.SSAVer)) != 0;
    };
    // A value-preserving forward (the live-in keeps flowing, not yet consumed).
    auto isPassThrough = [](const MedOp &Op) {
      switch (Op.Opcode) {
      case NdOp::COPY:
        return Op.NumInputs == 1;
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        return Op.NumInputs == 1;
      case NdOp::SUBBYTES:
        return Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
               Op.Inputs[1].ConstVal == 0;
      default:
        return false;
      }
    };
    // `x ^ x` / `x - x`: both operands the same tainted value, result is 0 —
    // the value is discarded, not consumed.
    auto isSelfCancel = [&](const MedOp &Op,
                            const std::set<std::pair<int, int>> &T) {
      if ((Op.Opcode != NdOp::INT_XOR && Op.Opcode != NdOp::INT_SUB) ||
          Op.NumInputs != 2)
        return false;
      const MedVar &A = Op.Inputs[0], &B = Op.Inputs[1];
      return tainted(A, T) && A.Id == B.Id && A.SSAVer == B.SSAVer &&
             A.Kind == B.Kind;
    };

    std::set<std::pair<int, int>> Taint{key(LiveIn)};
    bool Changed = true;
    int Guard = 0;
    while (Changed && Guard++ < 100000) {
      Changed = false;
      for (const auto &Blk : Func.Blocks) {
        for (const auto &Phi : Blk.Phis)
          for (const auto &[Pred, AV] : Phi.Args)
            if (tainted(AV, Taint) && Taint.insert(key(Phi.Output)).second)
              Changed = true;
        for (const auto &Op : Blk.Ops) {
          bool Reads = false;
          for (uint8_t K = 0; K < Op.NumInputs; ++K)
            if (tainted(Op.Inputs[K], Taint)) {
              Reads = true;
              break;
            }
          if (!Reads)
            continue;
          // The self-copy that re-publishes the live-in keeps the same value.
          bool IsSelfCopy = Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
                            Op.Output.Kind == MedVar::Reg &&
                            Op.Inputs[0].Id == Op.Output.Id;
          if (isSelfCancel(Op, Taint))
            continue; // discards the value
          // A reinterpret of the FP value into a general-purpose register
          // (`fmov w0, s0` / `movd eax, xmm0`) materializes the float's bits in
          // the integer domain: a genuine use of the FP parameter even though
          // it is a single-operand COPY/cast.  A function that only bit-casts
          // its float argument to its integer representation (`uint32_t
          // fbits(float f){ return *(uint32_t*)&f; }`) has no other consumer,
          // so without this its incoming value looks "unused" and the FP
          // parameter is dropped (the function then reads 0).  Same-class
          // FP->FP forwards and flows into a Temp stay value-preserving
          // (handled below).
          if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
               Op.Opcode == NdOp::INT_SEXT || Op.Opcode == NdOp::SUBBYTES) &&
              Op.Output.Kind == MedVar::Reg &&
              !TRI.isVectorReg(Op.Output.RegOff))
            return true;
          if (IsSelfCopy || isPassThrough(Op)) {
            if (Taint.insert(key(Op.Output)).second)
              Changed = true;
            continue;
          }
          return true; // a genuine consumer of the live-in value
        }
      }
    }
    return false;
  };

  std::set<uint64_t> AlreadyParam;
  for (const auto &P : Func.Params)
    AlreadyParam.insert(P.RegOff);

  std::vector<MedVar> FPParams;
  for (const auto &Op : Entry.Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (Op.Output.Kind != MedVar::Reg)
      continue;
    if (Op.NumInputs < 1 || Op.Inputs[0].Id != Op.Output.Id)
      continue;

    uint64_t ROff = Op.Output.RegOff;
    if (AlreadyParam.count(ROff))
      continue;
    if (!TRI.isFPArgReg(ROff))
      continue;
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

//===----------------------------------------------------------------------===//
// i386 CDECL stack-passed parameter detection
//===----------------------------------------------------------------------===//

void detectCdeclStackParams(MedFunc &Func, Arch TargetArch) {
  // i386 passes stack arguments at [esp_entry + 4 + 4*i] (the return address
  // occupies [esp_entry + 0]).  Recover those incoming-stack loads as function
  // parameters AND rewrite each load to read its parameter, so the lifted
  // function gains a real signature instead of dereferencing a bogus absolute
  // address (the raw displacement, e.g. `inttoptr 4`).  Self-guarding so the
  // generic detector can call it unconditionally: only 32-bit x86.
  //
  // Register arguments already recovered by detectRegisterParams (clang's
  // fastcall-style ECX/EDX for internal functions) occupy the leading parameter
  // slots, so stack arguments are numbered from there; a pure cdecl callee has
  // none and the first stack argument is arg0.
  if (TargetArch != Arch::X86 || Func.Blocks.empty())
    return;
  const int BaseIdx = static_cast<int>(Func.Params.size());

  const uint64_t SpOff = getTargetRegInfo(TargetArch).StackPointer;

  auto findDef = [&](const MedVar &V) -> const MedOp * {
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
            Op.Output.SSAVer == V.SSAVer)
          return &Op;
    return nullptr;
  };

  auto findPhi = [&](const MedVar &V) -> const PhiNode * {
    for (const auto &Blk : Func.Blocks)
      for (const auto &Phi : Blk.Phis)
        if (Phi.Output.Kind == V.Kind && Phi.Output.Id == V.Id &&
            Phi.Output.SSAVer == V.SSAVer)
          return &Phi;
    return nullptr;
  };

  // Offset of \p V relative to the *entry* stack pointer, or std::nullopt when
  // V is not esp-derived.  detectCc runs before copy propagation, and i386
  // routes esp through COPY identity, INT_ZEXT/SUBBYTES (32<->64-bit) and
  // push/pop INT_SUB/INT_ADD adjustments, so the raw displacement on a load is
  // not the incoming-stack offset.  Folding the whole chain (e.g. a `push`
  // before `mov 8(%esp),..` yields +4, the true first argument) is what
  // distinguishes an argument from a spilled local. i386 models every esp
  // adjustment as a 32<->64 round-trip (INT_SUB -> INT_ZEXT RSP -> SUBBYTES
  // ESP), so each callee-saved push costs ~3 chain links; with up to 4 pushes
  // plus the frame sub and the get-PC call/pop the esp chain to a stack
  // argument can run past 20 links — a shallow cap silently drops the recovery
  // and the arg is read as 0.
  std::set<std::tuple<int, int, uint64_t>> VisitedPhi;
  // Every def visited on the current esp-chain walk, keyed by SSA identity.  A
  // value cycle through COPY/ADD/SUB/extend defs (not only PHIs) would otherwise
  // recurse until the depth cap — but 4096 levels of this std::function
  // recursion overflow even the enlarged main stack (SIGBUS).  Cutting a re-entry
  // of any already-seen def bounds the walk to the (small) number of distinct
  // SSA vars, so the depth cap only guards genuinely long acyclic chains.
  std::set<std::tuple<int, int, int, uint64_t>> VisitedDef;
  std::function<std::optional<int64_t>(const MedVar &, int)> traceOff =
      [&](const MedVar &V, int Depth) -> std::optional<int64_t> {
    if (Depth == 0) {
      VisitedPhi.clear();
      VisitedDef.clear();
    }
    if (!VisitedDef
             .insert({static_cast<int>(V.Kind), V.Id, V.SSAVer, V.RegOff})
             .second)
      return std::nullopt;
    // The esp chain to a stack access can be long: each i386 stack adjustment
    // is a 32<->64 round-trip (~3 links) and a call that pushes many arguments
    // adds one push per 4-byte slot before the last argument's read (a re-read
    // of the function's own incoming parameter, pushed last, threads the whole
    // push chain).  A `f(10 long long)` call pushes 20 slots, so the cap must
    // comfort- ably exceed ~3 * (slots + saves); the walk is acyclic (PHI
    // cycles are cut by VisitedPhi), so a generous bound only guards against
    // runaway recursion.
    if (Depth > 4096)
      return std::nullopt;
    const MedOp *Def = findDef(V);
    if (!Def) {
      // A stack pointer with no straight-line def is a loop-carried PHI:
      // detectCc runs post-SSA, so a `push`/`[esp+k]` inside a loop body reads
      // through the esp PHI.  The stack pointer is loop-invariant, so any PHI
      // argument yields the same entry-relative offset — resolve it instead of
      // mistaking the frame-adjusted esp for the entry esp (which would read an
      // outgoing-arg spill as a bogus incoming parameter).
      if (const PhiNode *Phi = findPhi(V)) {
        if (!VisitedPhi.insert({V.Id, V.SSAVer, V.RegOff}).second)
          return std::nullopt;
        for (const auto &[Pred, AV] : Phi->Args)
          if (auto O = traceOff(AV, Depth + 1))
            return O;
        return std::nullopt;
      }
      return (V.Kind == MedVar::Reg && V.RegOff == SpOff)
                 ? std::optional<int64_t>(0)
                 : std::nullopt;
    }
    auto constOf = [](const MedVar &X) -> std::optional<int64_t> {
      return X.Kind == MedVar::Const
                 ? std::optional<int64_t>(static_cast<int64_t>(X.ConstVal))
                 : std::nullopt;
    };
    switch (Def->Opcode) {
    case NdOp::COPY:
      if (Def->NumInputs == 1) {
        const MedVar &In = Def->Inputs[0];
        if (In.Kind == MedVar::Reg && In.RegOff == SpOff && In.Id == V.Id &&
            In.SSAVer == V.SSAVer)
          return 0; // identity self-copy of the entry stack pointer
        return traceOff(In, Depth + 1);
      }
      return std::nullopt;
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Def->NumInputs == 1 ? traceOff(Def->Inputs[0], Depth + 1)
                                 : std::nullopt;
    case NdOp::SUBBYTES:
      if (Def->NumInputs >= 2 && Def->Inputs[1].Kind == MedVar::Const &&
          Def->Inputs[1].ConstVal == 0)
        return traceOff(Def->Inputs[0], Depth + 1);
      return std::nullopt;
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

  // Byte offset of an incoming stack argument loaded through \p AddrVar (the
  // address relative to the entry esp must land at +4 or above, since +0 is the
  // return address); std::nullopt otherwise.
  auto stackArgOffset = [&](const MedVar &AddrVar) -> std::optional<int64_t> {
    if (AddrVar.Kind != MedVar::Temp)
      return std::nullopt;
    auto Off = traceOff(AddrVar, 0);
    if (!Off || *Off < 4 || *Off > 0x400 || (*Off % 4) != 0)
      return std::nullopt;
    return *Off;
  };

  // As stackArgOffset but without the 4-byte alignment requirement: a sub-slot
  // field access of a by-value aggregate argument (e.g. a `short` at [esp+6],
  // the high half of the first stack slot) lands at a non-aligned entry-esp
  // offset.  Used only to fold such reads into a SUBBYTES of the slot
  // parameter.
  auto rawStackArgOffset =
      [&](const MedVar &AddrVar) -> std::optional<int64_t> {
    if (AddrVar.Kind != MedVar::Temp)
      return std::nullopt;
    auto Off = traceOff(AddrVar, 0);
    if (!Off || *Off < 4 || *Off > 0x400)
      return std::nullopt;
    return *Off;
  };

  // A contiguous parameter list 0..MaxIdx keeps every later argument at its
  // true stack offset even when an intermediate slot is unused (idx = (off - 4)
  // / 4). A wide load (an 8-byte double, or a vectorized multi-argument copy)
  // spans several 4-byte slots, so it extends MaxIdx by its width — otherwise
  // the high slots of a wide argument read at the last offset are never
  // created.
  std::set<int64_t> Offsets;
  int MaxIdx = -1;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
        if (auto Off = stackArgOffset(Op.Inputs[0])) {
          Offsets.insert(*Off);
          int W = Op.Output.Size > 0 ? Op.Output.Size : 4;
          int LastSlot = static_cast<int>((*Off - 4) / 4) + (W + 3) / 4 - 1;
          MaxIdx = std::max(MaxIdx, LastSlot);
        }

  if (Offsets.empty())
    return;
  Func.CC = CallingConv::CDECL;
  for (int I = 0; I <= MaxIdx; ++I) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.Id = BaseIdx + I;
    Param.Size = 4;
    // A stack parameter has no backing register; RegOff aliases StackOff in the
    // MedVar union, so leaving a stack displacement here would masquerade as a
    // register offset and overwrite a real register parameter in the emitter's
    // register->arg map (a stack slot at +8 collides with EDX, etc.).
    Param.RegOff = kNoParamReg;
    Param.TheArch = TargetArch;
    Func.Params.push_back(Param);
  }

  // An incoming-argument slot whose home is a mutable local — the function
  // WRITES it directly (a parameter updated in a loop), or its ADDRESS ESCAPES
  // (passed to a call / stored), so a callee may write it through the escaped
  // pointer — must keep its loads as memory reads.  Folding them to the
  // original incoming value would drop the update; the home slot already holds
  // the argument under cdecl and is writable.  Read-only, non-escaping slots
  // keep the COPY rewrite.
  std::set<int64_t> MutableArgOffsets;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 1)
        if (auto Off = stackArgOffset(Op.Inputs[0]))
          MutableArgOffsets.insert(*Off); // direct write to the home slot
      // The home address used as anything but a load/store address (a call
      // argument, a stored value, ...) escapes and may be written through.
      for (uint8_t K = 0; K < Op.NumInputs; ++K) {
        if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && K == 0)
          continue;
        if (auto Off = stackArgOffset(Op.Inputs[K]))
          MutableArgOffsets.insert(*Off);
      }
    }

  // The home slot of each mutable parameter is [frame_end + Off] (the entry-SP
  // offset equals the frame_end offset).  Record it so the emitter seeds that
  // headroom slot with the parameter at entry; the memory loads/stores left
  // below then read the argument and observe later writes.
  for (int64_t Off : MutableArgOffsets) {
    int Idx = BaseIdx + static_cast<int>((Off - 4) / 4);
    if (Idx <= BaseIdx + MaxIdx)
      Func.MutableStackParamHomes.push_back({Idx, Off});
  }

  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs < 1)
        continue;
      auto Off = stackArgOffset(Op.Inputs[0]);
      if (Off) {
        if (MutableArgOffsets.count(*Off))
          continue; // mutable parameter home: keep as a memory load
        MedVar Param;
        Param.Kind = MedVar::Param;
        Param.Id = BaseIdx + static_cast<int>((*Off - 4) / 4);
        Param.Size = Op.Output.Size > 0 ? Op.Output.Size : 4;
        Param.RegOff = kNoParamReg;
        Param.TheArch = TargetArch;
        Op.Opcode = NdOp::COPY;
        Op.Inputs[0] = Param;
        Op.NumInputs = 1;
        continue;
      }
      // Sub-slot field of a by-value aggregate argument: a read whose entry-esp
      // offset falls strictly inside a recovered 4-byte parameter slot (e.g. a
      // `short` at [esp+6] = the high half of the first stack argument).  Fold
      // it to a SUBBYTES of that slot parameter so the field resolves to the
      // argument instead of a bare absolute address (the lost esp base would
      // otherwise leave `inttoptr 6` -> READ_UNMAPPED).
      auto Raw = rawStackArgOffset(Op.Inputs[0]);
      if (!Raw)
        continue;
      int64_t ByteOff = (*Raw - 4) % 4;
      int Slot = static_cast<int>((*Raw - 4) / 4);
      if (ByteOff == 0 || Slot > MaxIdx)
        continue; // aligned reads handled above; the slot must be a parameter
      int64_t SlotBase = *Raw - ByteOff;
      if (MutableArgOffsets.count(SlotBase))
        continue; // mutable parameter home: keep as a memory load
      uint16_t ReadSz = Op.Output.Size > 0 ? Op.Output.Size : 1;
      if (ByteOff + static_cast<int64_t>(ReadSz) > 4)
        continue; // straddles the slot end: not a clean field of this slot
      MedVar Param;
      Param.Kind = MedVar::Param;
      Param.Id = BaseIdx + Slot;
      Param.Size = 4;
      Param.RegOff = kNoParamReg;
      Param.TheArch = TargetArch;
      Op.Opcode = NdOp::SUBBYTES;
      Op.Inputs[0] = Param;
      Op.Inputs[1] = MedVar::makeConst(static_cast<uint64_t>(ByteOff), 4);
      Op.NumInputs = 2;
    }
}

} // namespace neverd
