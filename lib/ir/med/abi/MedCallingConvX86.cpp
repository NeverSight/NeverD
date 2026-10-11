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
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallingConvDetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

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
    Arch TargetArch, BinaryFormat TargetFormat) {
  // FP/vector arguments arrive in a dedicated vector register class under
  // clang's -O2 conventions: x86-64 XMM0-7, AArch64 V0-7, ARM VFP D0-7 — all
  // observed as live-in self-copies.
  if (TRI.VecRegCount == 0)
    return;

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
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        if (!Arg.isConst())
          Uses[valueKey(Arg)].push_back({nullptr, &Phi});
      }
    for (const MedOp &Op : Block.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst())
          Uses[valueKey(Op.Inputs[I])].push_back({&Op, nullptr});
  }
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
          (!TRI.isVectorReg(Output.RegOff) || isFPReturnReg(Output.RegOff)))
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
    if (Op.Output.Kind != MedVar::Reg)
      continue;
    if (Op.NumInputs < 1 || Op.Inputs[0].Id != Op.Output.Id)
      continue;

    uint64_t ROff = Op.Output.RegOff;
    if (!TRI.isFPArgReg(ROff, TargetFormat))
      continue;
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

  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs < 1)
        continue;
      if (Op.MemoryOrdering != NdMemoryOrdering::None)
        continue;
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
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
        if (Param.Size == 8) {
          // An eight-byte load spans two four-byte ABI parameters. Encode
          // that composition in MedIR so both source routes retain the high
          // word and agree about which incoming slots are actually used.
          Param.Size = 4;
          MedVar High = Param;
          ++High.Id;
          Op.Opcode = NdOp::CONCAT;
          Op.Inputs[0] = High;
          Op.Inputs[1] = Param;
          Op.NumInputs = 2;
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
  const bool Approx = Id == Intrinsic::X86FPApprox12MemoryState;
  const bool Scalar = Approx ? x86FPApprox12IsScalar(Shape.Control)
                      : Id == Intrinsic::X86FPRoundMemoryState
                          ? x86FPRoundStateIsScalar(Shape.Control)
                          : x86FPArithStateIsScalar(Shape.Control);
  return StackMemoryRead{1, Shape.OutputSize - (Approx ? 0 : 4), Scalar};
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
  int NextTemp = 0;
  const auto Observe = [&](const MedVar &V) {
    if (V.Kind == MedVar::Temp)
      NextTemp = std::max(NextTemp, V.Id + 1);
  };
  for (const auto &Block : Func.Blocks) {
    for (const auto &Phi : Block.Phis) {
      Observe(Phi.Output);
      for (const auto &[Pred, Input] : Phi.Args)
        Observe(Input);
    }
    for (const auto &Op : Block.Ops) {
      Observe(Op.Output);
      for (const auto &V : Op.Inputs)
        Observe(V);
      for (const auto &V : Op.IntrinsicOutputs)
        Observe(V);
    }
  }
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
      if (Id == Intrinsic::X86FPArithMemoryState) {
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
