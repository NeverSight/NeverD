//===- MedCallingConvX86.cpp - x86 calling convention detection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// i386 CDECL and register calling-convention parameter recovery.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallingConvDetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {

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

  // traceOff follows each link of an esp chain to its SSA definition. Index the
  // definitions once instead of scanning every operation for each link: on a
  // 10k-block i386 body the scan made the walk cubic in the function size.
  // Each key keeps its first definition in block order, as the scan did. The
  // index points into Func's operations, so inserting operations resets it.
  using SSAKey = std::tuple<int, int, int>;
  auto keyOf = [](const MedVar &V) {
    return SSAKey{static_cast<int>(V.Kind), V.Id, V.SSAVer};
  };
  std::map<SSAKey, const MedOp *> DefIndex;
  std::map<SSAKey, const PhiNode *> PhiIndex;
  bool Indexed = false;
  auto indexDefinitions = [&] {
    if (Indexed)
      return;
    DefIndex.clear();
    PhiIndex.clear();
    for (const auto &Blk : Func.Blocks) {
      for (const auto &Op : Blk.Ops)
        DefIndex.emplace(keyOf(Op.Output), &Op);
      for (const auto &Phi : Blk.Phis)
        PhiIndex.emplace(keyOf(Phi.Output), &Phi);
    }
    Indexed = true;
  };

  auto findDef = [&](const MedVar &V) -> const MedOp * {
    indexDefinitions();
    const auto It = DefIndex.find(keyOf(V));
    return It == DefIndex.end() ? nullptr : It->second;
  };

  auto findPhi = [&](const MedVar &V) -> const PhiNode * {
    indexDefinitions();
    const auto It = PhiIndex.find(keyOf(V));
    return It == PhiIndex.end() ? nullptr : It->second;
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
  // value cycle through COPY/ADD/SUB/extend defs (not only PHIs) would
  // otherwise recurse until the depth cap — but 4096 levels of this
  // std::function recursion overflow even the enlarged main stack (SIGBUS).
  // Cutting a re-entry of any already-seen def bounds the walk to the (small)
  // number of distinct SSA vars, so the depth cap only guards genuinely long
  // acyclic chains.
  std::set<std::tuple<int, int, int, uint64_t>> VisitedDef;
  std::function<std::optional<int64_t>(const MedVar &, int)> traceOff =
      [&](const MedVar &V, int Depth) -> std::optional<int64_t> {
    if (Depth == 0) {
      VisitedPhi.clear();
      VisitedDef.clear();
    }
    if (!VisitedDef.insert({static_cast<int>(V.Kind), V.Id, V.SSAVer, V.RegOff})
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
  auto isVariadicOverflowOffset = [&](int64_t Off) {
    return Func.IsVariadic && Func.VariadicOverflowBase > 0 &&
           Off >= Func.VariadicOverflowBase;
  };
  auto stackArgOffset = [&](const MedVar &AddrVar) -> std::optional<int64_t> {
    if (AddrVar.Kind != MedVar::Temp)
      return std::nullopt;
    auto Off = traceOff(AddrVar, 0);
    if (!Off || *Off < 4 || *Off > 0x400 || (*Off % 4) != 0)
      return std::nullopt;
    // A detected variadic callee recovers only its named prefix here.  Values
    // at and above the va_list overflow base are walked dynamically and are
    // finalized from call sites; rewriting one loop iteration to a fixed
    // parameter (notably the high word of an i386 long long) corrupts every
    // later va_arg and also duplicates overflow homes in the emitter.
    if (isVariadicOverflowOffset(*Off))
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
    if (isVariadicOverflowOffset(*Off))
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
      if (auto Read = med_calling_conv_detail::stackMemoryRead(Op))
        if (auto Off = Read->AddressInput == 1 ? rawStackArgOffset(Op.Inputs[1])
                                               : stackArgOffset(Op.Inputs[0])) {
          Offsets.insert(4 + ((*Off - 4) / 4) * 4);
          int LastSlot = static_cast<int>((*Off - 4 + Read->Bytes - 1) / 4);
          MaxIdx = std::max(MaxIdx, LastSlot);
        }

  // A tail jump at the entry stack pointer hands its callee this function's
  // incoming arguments: the slots of the positions the callee is known to
  // read (MedOp::CalleeStackArgs) are parameters passed through, though the
  // body never loads them, as in a thunk `jmp [__imp__calloc]`.
  for (const auto &Blk : Func.Blocks)
    for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
      const MedOp &Call = Blk.Ops[I];
      if ((Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
          Call.CalleeStackArgs <= 0 || Blk.Ops[I + 1].Opcode != NdOp::RETURN ||
          Blk.Ops[I + 1].Addr != Call.Addr)
        continue;
      // The stack pointer at the call: the block's last write before it, else
      // the entry block's incoming one.
      std::optional<int64_t> CallOff;
      bool Written = false;
      for (size_t J = I; J-- > 0 && !Written;)
        if (const MedVar &Out = Blk.Ops[J].Output;
            Out.Kind == MedVar::Reg && Out.RegOff == SpOff && Out.Size > 0) {
          CallOff = traceOff(Out, 0);
          Written = true;
        }
      if (!Written && Blk.Preds.empty())
        CallOff = 0;
      if (CallOff != 0)
        continue;
      for (int K = 0; K < Call.CalleeStackArgs; ++K) {
        Offsets.insert(4 + 4 * K);
        MaxIdx = std::max(MaxIdx, K);
      }
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
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto Off = stackArgOffset(Op.Inputs[0]))
          MutableArgOffsets.insert(*Off); // direct write to the home slot
      // The stack address can thread through these value-preserving operations
      // before reaching its real use.  They are not escapes themselves; keep
      // this list in sync with traceOff above.  In particular, i386 address
      // arithmetic now carries explicit 32-bit ZEXT views, and treating their
      // input as an escape turns ordinary read-only arguments into mutable
      // homes.
      const bool HasTraceableForwardedAddress =
          Op.Output.Kind == MedVar::Temp &&
          stackArgOffset(Op.Output).has_value();
      if (HasTraceableForwardedAddress)
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
      // The home address used as anything but a load/store address (a call
      // argument, a stored value, ...) escapes and may be written through.
      for (uint8_t K = 0; K < Op.NumInputs; ++K) {
        if (((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && K == 0) ||
            (med_calling_conv_detail::stackMemoryRead(Op) &&
             K == med_calling_conv_detail::stackMemoryRead(Op)->AddressInput))
          continue;
        if (auto Off = stackArgOffset(Op.Inputs[K]))
          MutableArgOffsets.insert(*Off);
      }
    }

  med_calling_conv_detail::preserveFPStackHomes(Func, 4, 4, rawStackArgOffset,
                                                MutableArgOffsets);

  // The home slot of each mutable parameter is [frame_end + Off] (the entry-SP
  // offset equals the frame_end offset).  Record it so the emitter seeds that
  // headroom slot with the parameter at entry; the memory loads/stores left
  // below then read the argument and observe later writes.
  for (int64_t Off : MutableArgOffsets) {
    int Idx = BaseIdx + static_cast<int>((Off - 4) / 4);
    if (Idx <= BaseIdx + MaxIdx)
      Func.MutableStackParamHomes.push_back({Idx, Off});
  }

  // A load of more than two slots composes in temporaries numbered above
  // every value.
  int NextTemp = med_calling_conv_detail::nextValueId(Func);
  // The compositions to insert before an op, by its block and index.
  std::map<std::pair<size_t, size_t>, std::vector<MedOp>> Compositions;
  for (size_t BI = 0; BI < Func.Blocks.size(); ++BI)
    for (size_t OI = 0; OI < Func.Blocks[BI].Ops.size(); ++OI) {
      MedOp &Op = Func.Blocks[BI].Ops[OI];
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs < 1)
        continue;
      if (Op.MemoryOrdering != NdMemoryOrdering::None)
        continue;
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      auto Off = stackArgOffset(Op.Inputs[0]);
      if (Off) {
        const uint16_t Bytes = Op.Output.Size > 0 ? Op.Output.Size : 4;
        // A load wider than a slot reads every slot it covers, the last one
        // perhaps in part.
        const unsigned Slots = Bytes > 4 ? (Bytes + 3u) / 4u : 1;
        bool Mutable = false;
        for (unsigned K = 0; K < Slots; ++K)
          Mutable |= MutableArgOffsets.count(*Off + 4 * K) != 0;
        if (Mutable)
          continue; // mutable parameter home: keep as a memory load
        MedVar Param;
        Param.Kind = MedVar::Param;
        Param.Id = BaseIdx + static_cast<int>((*Off - 4) / 4);
        Param.Size = Bytes;
        Param.RegOff = kNoParamReg;
        Param.TheArch = TargetArch;
        if (Slots > 1) {
          // A load of several slots, an eight-byte double, a ten-byte long
          // double or a vectorized copy of four arguments, spans as many
          // four-byte ABI parameters.
          // Encode that composition in MedIR, each slot above the ones below
          // it, so both source routes keep every slot and agree about which
          // incoming slots are actually used.
          std::vector<MedOp> Prefix;
          med_calling_conv_detail::composeStackSlots(Op, Param, 4, Bytes,
                                                     NextTemp, Prefix);
          if (!Prefix.empty())
            Compositions[{BI, OI}] = std::move(Prefix);
          continue;
        }
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
  // Insert each composition before its load, latest first so earlier
  // indices stay valid.
  for (auto It = Compositions.rbegin(); It != Compositions.rend(); ++It) {
    auto &Ops = Func.Blocks[It->first.first].Ops;
    Ops.insert(Ops.begin() + static_cast<std::ptrdiff_t>(It->first.second),
               It->second.begin(), It->second.end());
    Indexed = false;
  }
  med_calling_conv_detail::recoverFPStackReads(
      Func, TargetArch, 4, 4, BaseIdx, rawStackArgOffset, MutableArgOffsets);
}

namespace med_calling_conv_detail {

std::optional<StackMemoryRead> stackMemoryRead(const MedOp &Op) {
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return std::nullopt;
  if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 && Op.Output.Size)
    return StackMemoryRead{0, Op.Output.Size};
  if (Op.MemoryOrdering != NdMemoryOrdering::None)
    return std::nullopt;
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs || !Op.Inputs[0].isConst())
    return std::nullopt;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
  if (Op.Inputs[0].ConstVal != unsigned(Id) || !isX86FPStateMemoryIntrinsic(Id))
    return std::nullopt;
  const auto Shape = x86FPStateMedShape(Op);
  if (!x86FPStateShapeIsValid(Id, Shape))
    return std::nullopt;
  if (Id == Intrinsic::X86F16CStoreState)
    return std::nullopt;
  const unsigned Bytes =
      x86FPStateMemoryBytes(Id, Shape.OutputSize, Shape.Control);
  if (Id == Intrinsic::X86F16CReadState ||
      Id == Intrinsic::X86FPPackedConvertMemoryState)
    return StackMemoryRead{1, Bytes, false};
  if (Id == Intrinsic::X86FPMaskCompareMemoryState)
    return StackMemoryRead{1, Bytes, x86FPMaskCompareIsScalar(Shape.Control)};
  if (Id == Intrinsic::X86FPComiMemoryState)
    return StackMemoryRead{1, Bytes, true};
  if (Id == Intrinsic::X86FPScalarConvertMemoryState)
    // This denotes an eligible scalar completion source, including raw integer
    // bits. Its own conversion contract retains the source's numerical type.
    return StackMemoryRead{1, Bytes, true};
  const bool Scalar = Id == Intrinsic::X86FPApprox12MemoryState
                          ? x86FPApprox12IsScalar(Shape.Control)
                      : Id == Intrinsic::X86FPRoundMemoryState
                          ? x86FPRoundStateIsScalar(Shape.Control)
                          : x86FPArithStateIsScalar(Shape.Control);
  return StackMemoryRead{1, Bytes, Scalar};
}

void preserveFPStackHomes(
    const MedFunc &Func, int64_t Base, unsigned Slot,
    llvm::function_ref<std::optional<int64_t>(const MedVar &)> Trace,
    std::set<int64_t> &MutableSlots) {
  // A write or escaped pointer to one part of a memory operand keeps every
  // covered slot in its physical home. Packed reads retain alignment/fault
  // ownership even when all incoming bytes are otherwise immutable.
  bool Changed;
  do {
    Changed = false;
    for (const auto &Block : Func.Blocks)
      for (const auto &Op : Block.Ops) {
        const auto Read = stackMemoryRead(Op);
        if (!Read || Read->AddressInput != 1)
          continue;
        const auto Offset = Trace(Op.Inputs[Read->AddressInput]);
        if (!Offset)
          continue;
        const int64_t First = Base + ((*Offset - Base) / Slot) * Slot;
        const int64_t Last =
            Base + ((*Offset - Base + Read->Bytes - 1) / Slot) * Slot;
        const unsigned ByteOffset = *Offset - First;
        const bool Pair = ByteOffset == 0 && Slot == 4 && Read->Bytes == 8;
        bool Keep =
            !Read->ScalarFP || (!Pair && ByteOffset + Read->Bytes > Slot);
        for (int64_t Home = First; Home <= Last; Home += Slot)
          Keep |= MutableSlots.count(Home) != 0;
        if (Keep)
          for (int64_t Home = First; Home <= Last; Home += Slot)
            Changed |= MutableSlots.insert(Home).second;
      }
  } while (Changed);
}

void recoverFPStackReads(
    MedFunc &Func, Arch Architecture, int64_t Base, unsigned Slot,
    unsigned ParameterBase,
    llvm::function_ref<std::optional<int64_t>(const MedVar &)> Trace,
    const std::set<int64_t> &MutableSlots) {
  if (Architecture != Arch::X86 && Architecture != Arch::X64)
    return;
  int NextTemp = nextValueId(Func);
  struct Rewrite {
    MedOp Value;
    std::vector<MedOp> Prefix;
  };
  std::map<std::pair<size_t, size_t>, Rewrite> Rewrites;
  // Compute every trace before inserting operations: the ABI owner holds
  // pointers to the original unique definitions across all blocks.
  for (size_t B = 0; B < Func.Blocks.size(); ++B)
    for (size_t I = 0; I < Func.Blocks[B].Ops.size(); ++I) {
      const auto &Op = Func.Blocks[B].Ops[I];
      const auto Read = stackMemoryRead(Op);
      if (!Read || !Read->ScalarFP)
        continue;
      const auto Offset = Trace(Op.Inputs[Read->AddressInput]);
      if (!Offset)
        continue;
      const int64_t Home = Base + ((*Offset - Base) / Slot) * Slot;
      const unsigned ByteOffset = *Offset - Home;
      const bool Pair = ByteOffset == 0 && Slot == 4 && Read->Bytes == 8;
      if ((!Pair && ByteOffset + Read->Bytes > Slot) ||
          MutableSlots.count(Home) || (Pair && MutableSlots.count(Home + Slot)))
        continue;
      Rewrite R{Op, {}};
      MedVar Source;
      Source.Kind = MedVar::Param;
      Source.Id = ParameterBase + (Home - Base) / Slot;
      Source.RegOff = kNoParamReg;
      Source.Size = Read->Bytes;
      Source.TheArch = Architecture;
      if (Pair || ByteOffset) {
        MedOp Compose;
        Compose.Opcode = Pair ? NdOp::CONCAT : NdOp::SUBBYTES;
        Compose.Addr = Op.Addr;
        Compose.OriginSeq = Op.OriginSeq;
        Compose.Output = Source;
        Compose.Output.Kind = MedVar::Temp;
        Compose.Output.Id = NextTemp++;
        Source.Size = Slot;
        if (Pair) {
          auto High = Source;
          ++High.Id;
          Compose.addInput(High);
          Compose.addInput(Source);
        } else {
          Compose.addInput(Source);
          Compose.addInput(MedVar::makeConst(ByteOffset, 4));
        }
        R.Prefix.push_back(Compose);
        Source = Compose.Output;
      }
      const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
      if (Id == Intrinsic::X86FPScalarConvertMemoryState) {
        R.Value.Inputs[0] =
            MedVar::makeConst(unsigned(Intrinsic::X86FPScalarConvertState), 2);
        R.Value.Inputs[1] = Source;
      } else if (Id == Intrinsic::X86FPFmaMemoryState) {
        R.Value.Inputs[0] =
            MedVar::makeConst(unsigned(Intrinsic::X86FPFmaState), 2);
        R.Value.Inputs[1] =
            MedVar::makeConst(Op.Inputs[2].ConstVal & ~UINT64_C(32), 2);
        R.Value.Inputs[2] = Op.Inputs[3];
        R.Value.Inputs[3] = Op.Inputs[4];
        R.Value.Inputs[4] = Source;
      } else if (Id == Intrinsic::X86FPMaskCompareMemoryState) {
        R.Value.Inputs[0] =
            MedVar::makeConst(unsigned(Intrinsic::X86FPMaskCompareState), 2);
        R.Value.Inputs[1] = Op.Inputs[2];
        R.Value.Inputs[2] = Op.Inputs[3];
        R.Value.Inputs[3] = Op.Inputs[4];
        R.Value.Inputs[4] = Source;
      } else if (Id == Intrinsic::X86FPComiMemoryState) {
        R.Value.Inputs[0] =
            MedVar::makeConst(unsigned(Intrinsic::X86FPComiState), 2);
        R.Value.Inputs[1] = Op.Inputs[2];
        R.Value.Inputs[2] = Op.Inputs[3];
        R.Value.Inputs[3] = Source;
      } else if (Id == Intrinsic::X86FPArithMemoryState) {
        R.Value.Inputs[0] =
            MedVar::makeConst(unsigned(Intrinsic::X86FPArithState), 2);
        R.Value.Inputs[1] = MedVar::makeConst(Op.Inputs[2].ConstVal & 31, 1);
        R.Value.Inputs[2] = Op.Inputs[3];
        R.Value.Inputs[3] = Source;
      } else {
        const auto ValueId = Id == Intrinsic::X86FPRoundMemoryState
                                 ? Intrinsic::X86FPRoundState
                                 : Intrinsic::X86FPApprox12State;
        R.Value.Inputs[0] = MedVar::makeConst(unsigned(ValueId), 2);
        R.Value.Inputs[1] = MedVar::makeConst(
            Op.Inputs[2].ConstVal &
                (ValueId == Intrinsic::X86FPRoundState ? 6 : 3),
            1);
        R.Value.Inputs[2] = Source;
      }
      Rewrites.emplace(std::pair{B, I}, std::move(R));
    }
  if (Rewrites.empty())
    return;
  for (size_t B = 0; B < Func.Blocks.size(); ++B) {
    auto &Ops = Func.Blocks[B].Ops;
    std::vector<MedOp> Rebuilt;
    for (size_t I = 0; I < Ops.size(); ++I) {
      const auto It = Rewrites.find({B, I});
      if (It != Rewrites.end()) {
        for (auto &Prefix : It->second.Prefix)
          Rebuilt.push_back(std::move(Prefix));
        Rebuilt.push_back(std::move(It->second.Value));
      } else
        Rebuilt.push_back(std::move(Ops[I]));
    }
    Ops = std::move(Rebuilt);
  }
}

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

  llvm::DenseMap<med_calling_conv_detail::ValueKey, const MedOp *> Definitions;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (!Op.Output.isConst())
        Definitions.try_emplace(med_calling_conv_detail::valueKey(Op.Output),
                                &Op);
  auto findDef = [&](const MedVar &V) -> const MedOp * {
    if (V.isConst())
      return nullptr;
    auto It = Definitions.find(med_calling_conv_detail::valueKey(V));
    return It == Definitions.end() ? nullptr : It->second;
  };

  // Seed the taint set with the entry-block live-in self-copies of the
  // register.
  llvm::SmallVector<MedVar, 2> Seeds;
  for (const auto &Op : Func.Blocks[0].Ops) {
    if (Op.Opcode != NdOp::COPY || Op.NumInputs < 1)
      continue;
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == ParamRegOff &&
        Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[0].Id == Op.Output.Id) {
      Seeds.push_back(Op.Output);
      if (isRenamedEntryLiveInCopy(Op, ParamRegOff))
        Seeds.push_back(Op.Inputs[0]);
    }
  }
  if (Seeds.empty())
    return false; // no identifiable live-in value: keep existing behavior

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

  // Track the exact incoming bits that survive each lane view/splice.  Clang
  // commonly lowers consecutive CH/CL writes as
  //
  //   SUBBYTES old_ecx, 0/2; CONCAT ...; COPY new_ecx
  //
  // rather than as an AND/OR mask.  Boolean taint would call the first
  // CONCAT a genuine use, even when every surviving incoming lane is later
  // discarded.  A bit mask distinguishes that scratch reconstruction from a
  // real regparm use of the preserved upper bytes.
  auto widthMask = [](uint16_t Size) -> std::optional<uint64_t> {
    if (Size == 0 || Size > sizeof(uint64_t))
      return std::nullopt;
    const unsigned Bits = static_cast<unsigned>(Size) * 8;
    return Bits == 64 ? ~uint64_t{0} : (uint64_t{1} << Bits) - 1;
  };

  uint16_t ParamWidth = 0;
  for (const MedVar &Seed : Seeds)
    ParamWidth = std::max(ParamWidth, Seed.Size);
  if (ParamWidth == 0 || ParamWidth > sizeof(uint64_t))
    return false;

  struct TaintUse {
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
  };
  llvm::DenseMap<med_calling_conv_detail::ValueKey,
                 llvm::SmallVector<TaintUse, 4>>
      Uses;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        if (!Arg.isConst())
          Uses[med_calling_conv_detail::valueKey(Arg)].push_back(
              {nullptr, &Phi});
      }
    for (const MedOp &Op : Block.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst())
          Uses[med_calling_conv_detail::valueKey(Op.Inputs[I])].push_back(
              {&Op, nullptr});
  }

  llvm::DenseMap<med_calling_conv_detail::ValueKey, uint64_t> TaintMasks;
  llvm::SmallVector<med_calling_conv_detail::ValueKey, 32> Worklist;
  auto taintMask = [&](const MedVar &V) -> uint64_t {
    if (V.isConst())
      return 0;
    auto It = TaintMasks.find(med_calling_conv_detail::valueKey(V));
    return It == TaintMasks.end() ? 0 : It->second;
  };
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X86);
  auto isScratchTransport = [&](const MedVar &V) {
    if (V.Kind == MedVar::Temp)
      return true;
    if (V.Kind != MedVar::Reg || V.Size == 0 || V.Size > sizeof(uint64_t))
      return false;
    const uint64_t End = V.RegOff + V.Size;
    if (End < V.RegOff)
      return false;
    const auto [WideReg, WideSize] = TRI.findWideReg(V.RegOff, V.Size);
    (void)WideSize;
    // Transport through an ordinary scratch register remains internal until a
    // later use proves otherwise.  Crossing into a frame register, the return
    // register, or the other regparm slot is already ABI-observable and must
    // keep the incoming parameter.  Permit the queried register itself even
    // when it aliases i386's secondary integer-return register.
    if (TRI.isFrameReg(WideReg) ||
        (WideReg != ParamRegOff && TRI.isReturnReg(WideReg)))
      return false;
    for (uint64_t OtherParam : TRI.IntParamRegs) {
      if (OtherParam == ParamRegOff)
        continue;
      const uint64_t OtherEnd = OtherParam + ParamWidth;
      if (V.RegOff < OtherEnd && OtherParam < End)
        return false;
    }
    return true;
  };
  bool InvalidTransport = false;
  auto addTaint = [&](const MedVar &Output, uint64_t Mask) {
    std::optional<uint64_t> OutputMask = widthMask(Output.Size);
    if (!OutputMask || !isScratchTransport(Output)) {
      InvalidTransport = Mask != 0;
      return;
    }
    Mask &= *OutputMask;
    if (Mask == 0)
      return;
    uint64_t &Known = TaintMasks[med_calling_conv_detail::valueKey(Output)];
    const uint64_t Added = Mask & ~Known;
    Known |= Mask;
    if (Added != 0)
      Worklist.push_back(med_calling_conv_detail::valueKey(Output));
  };
  for (const MedVar &Seed : Seeds) {
    std::optional<uint64_t> Mask = widthMask(Seed.Size);
    if (!Mask)
      return false;
    addTaint(Seed, *Mask);
    if (InvalidTransport)
      return false;
  }

  auto propagateOp = [&](const MedOp &Op) {
    llvm::SmallVector<uint64_t, 6> InputMasks(Op.NumInputs, 0);
    bool HasTaint = false;
    for (uint8_t I = 0; I < Op.NumInputs; ++I) {
      InputMasks[I] = taintMask(Op.Inputs[I]);
      HasTaint |= InputMasks[I] != 0;
    }
    if (!HasTaint)
      return true;

    // BSR/BSF's old-destination operand is architecturally undefined when
    // selected.  It must not make a scratch destination into an argument.
    if (isBsrBsfPreserve(Op, 1)) {
      bool OnlyPreservedDestination = InputMasks[1] != 0;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (I != 1)
          OnlyPreservedDestination &= InputMasks[I] == 0;
      if (OnlyPreservedDestination)
        return true;
    }

    uint64_t OutputTaint = 0;
    switch (Op.Opcode) {
    case NdOp::COPY:
      if (Op.NumInputs != 1)
        return false;
      OutputTaint = InputMasks[0];
      break;
    case NdOp::INT_ZEXT:
      if (Op.NumInputs != 1)
        return false;
      OutputTaint = InputMasks[0];
      break;
    case NdOp::INT_SEXT: {
      if (Op.NumInputs != 1 || Op.Inputs[0].Size == 0 ||
          Op.Inputs[0].Size > sizeof(uint64_t))
        return false;
      OutputTaint = InputMasks[0];
      const unsigned SignBit = static_cast<unsigned>(Op.Inputs[0].Size) * 8 - 1;
      std::optional<uint64_t> OutputMask = widthMask(Op.Output.Size);
      if (!OutputMask)
        return false;
      if (OutputTaint & (uint64_t{1} << SignBit))
        OutputTaint |= *OutputMask & (~uint64_t{0} << SignBit);
      break;
    }
    case NdOp::SUBBYTES: {
      if (Op.NumInputs < 2 || Op.Inputs[1].Kind != MedVar::Const)
        return false;
      const uint64_t Offset = Op.Inputs[1].ConstVal;
      OutputTaint =
          Offset >= sizeof(uint64_t) ? 0 : InputMasks[0] >> (Offset * 8);
      break;
    }
    case NdOp::CONCAT: {
      if (Op.NumInputs != 2 || Op.Inputs[1].Size == 0 ||
          Op.Inputs[1].Size > sizeof(uint64_t))
        return false;
      const unsigned LowBits = static_cast<unsigned>(Op.Inputs[1].Size) * 8;
      if (LowBits == 64 && InputMasks[0] != 0)
        return false;
      OutputTaint = InputMasks[1];
      if (LowBits < 64)
        OutputTaint |= InputMasks[0] << LowBits;
      break;
    }
    case NdOp::INT_AND: {
      if (Op.NumInputs != 2)
        return false;
      const int ConstIdx = Op.Inputs[0].Kind == MedVar::Const
                               ? 0
                               : (Op.Inputs[1].Kind == MedVar::Const ? 1 : -1);
      if (ConstIdx < 0)
        return false;
      OutputTaint =
          InputMasks[ConstIdx == 0 ? 1 : 0] & Op.Inputs[ConstIdx].ConstVal;
      break;
    }
    case NdOp::INT_OR:
      if (Op.NumInputs != 2)
        return false;
      OutputTaint = InputMasks[0] | InputMasks[1];
      if (Op.Inputs[0].Kind == MedVar::Const)
        OutputTaint = InputMasks[1] & ~Op.Inputs[0].ConstVal;
      else if (Op.Inputs[1].Kind == MedVar::Const)
        OutputTaint = InputMasks[0] & ~Op.Inputs[1].ConstVal;
      break;
    default:
      return false; // a genuine consumer: keep the register parameter
    }

    addTaint(Op.Output, OutputTaint);
    return !InvalidTransport;
  };

  while (!Worklist.empty()) {
    const med_calling_conv_detail::ValueKey Changed = Worklist.pop_back_val();
    auto UsesIt = Uses.find(Changed);
    if (UsesIt == Uses.end())
      continue;
    for (const TaintUse &Use : UsesIt->second) {
      if (Use.Phi) {
        uint64_t Mask = 0;
        for (const auto &[Pred, Arg] : Use.Phi->Args) {
          (void)Pred;
          Mask |= taintMask(Arg);
        }
        addTaint(Use.Phi->Output, Mask);
        if (InvalidTransport)
          return false;
        continue;
      }
      if (!propagateOp(*Use.Op))
        return false;
    }
  }

  return true; // all surviving incoming lanes feed scratch reconstruction only
}

bool liveInReachesNonPushUse(const MedFunc &Func, uint64_t RegOff) {
  if (Func.Blocks.empty())
    return true;
  llvm::SmallVector<MedVar, 2> Seeds;
  for (const MedOp &Op : Func.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (!isEntryLiveInCopy(Op, RegOff))
      continue;
    Seeds.push_back(Op.Output);
    if (isRenamedEntryLiveInCopy(Op, RegOff))
      Seeds.push_back(Op.Inputs[0]);
  }
  if (Seeds.empty())
    return true;
  // The operations that carry the value's bytes without reading them.
  auto Carries = [](const MedOp &Op) {
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::SUBBYTES:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::CONCAT:
      return true;
    default:
      return false;
    }
  };
  const ValueSet Reached = computeForwardValueClosure(
      Func, Seeds, [&](const MedOp &Op, unsigned) { return Carries(Op); });
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Carries(Op))
        continue;
      // A return reads the integer return register whatever the function
      // returns (a float in XMM0 leaves EAX untouched).
      if (Op.Opcode == NdOp::RETURN)
        continue;
      // A push stores at the stack pointer itself; a spill to a frame slot,
      // read back later, is a use.
      const bool Push = Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
                        Op.Inputs[0].Kind == MedVar::Reg &&
                        Op.Inputs[0].RegOff ==
                            getTargetRegInfo(Op.Inputs[0].TheArch).StackPointer;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (containsValue(Reached, Op.Inputs[I]) && !(Push && I == 1))
          return true;
    }
  return false;
}

} // namespace med_calling_conv_detail

} // namespace neverd
