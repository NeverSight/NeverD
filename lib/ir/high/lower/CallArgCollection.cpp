//===- CallArgCollection.cpp - Call argument collection
//--------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Collects function call arguments by scanning backward from the call site
/// for register writes and stack stores that match the target ABI.  Shared
/// register/live-in recovery lives here; ISA stack-argument quirks live in
/// CallArgCollectionX86.cpp, CallArgCollectionARM.cpp, and
/// CallArgCollectionAArch64.cpp.
///
//===----------------------------------------------------------------------===//

#include "CallArgCollectionDetail.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <functional>
#include <set>

namespace neverd {

namespace call_args_detail {

bool isWin64(const CallArgScan &Scan) {
  return Scan.TheArch == Arch::X64 && Scan.Image &&
         Scan.Image->Format == BinaryFormat::COFF;
}

void collectSpilledStackArgs(const CallArgScan &Scan,
                             std::vector<ExprPtr> &Found) {
  const auto &Ops = *Scan.Ops;
  const TargetRegInfo &TRI = *Scan.TRI;
  const int64_t SlotBytes = static_cast<int64_t>(TRI.PointerSize);
  // Win64 stores every stack argument into the outgoing area after the
  // previous call; a 12-argument call needs far more than the default window
  // once address arithmetic and argument computations are interleaved.
  const int Window = isWin64(Scan) ? limits::kWin64CallArgStoreScanWindow
                                   : limits::kCallArgStoreScanWindow;
  const int StoreScanStart =
      std::max(0, static_cast<int>(Scan.CallIdx) - Window);

  std::vector<std::pair<int64_t, MedVar>> StoredSlots;
  for (int J = static_cast<int>(Scan.CallIdx) - 1; J >= StoreScanStart; --J) {
    const MedOp &Prev = Ops[J];
    if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
        Prev.Opcode == NdOp::INTRINSIC)
      break;
    if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
        Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;
    if (Scan.IsCalleeSave(Prev.Inputs[1]))
      continue;

    const MedVar &AddrVar = Prev.Inputs[0];
    int64_t StackOff = -1;

    if (AddrVar.Kind == MedVar::Reg && AddrVar.RegOff == Scan.SpRegOff)
      StackOff = 0;

    if (StackOff < 0 && !AddrVar.isConst()) {
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &DefOp = Ops[K];
        if (DefOp.Output.Id != AddrVar.Id ||
            DefOp.Output.SSAVer != AddrVar.SSAVer)
          continue;
        if (DefOp.Opcode == NdOp::INT_ADD && DefOp.NumInputs >= 2) {
          bool HasSP = false;
          int64_t ConstOff = -1;
          for (uint8_t KI = 0; KI < DefOp.NumInputs; ++KI) {
            if (DefOp.Inputs[KI].Kind == MedVar::Reg &&
                DefOp.Inputs[KI].RegOff == Scan.SpRegOff)
              HasSP = true;
            if (DefOp.Inputs[KI].isConst())
              ConstOff = static_cast<int64_t>(DefOp.Inputs[KI].ConstVal);
          }
          if (HasSP && ConstOff >= 0)
            StackOff = ConstOff;
        }
        break;
      }
    }

    if (StackOff < 0 && isWin64(Scan) && Scan.EntryOffsetOf)
      if (std::optional<int64_t> Entry = Scan.EntryOffsetOf(AddrVar))
        StackOff = *Entry + Scan.FrameSize;

    if (StackOff < 0 || StackOff >= Scan.MaxArgs * SlotBytes)
      continue;
    if (SlotBytes == 0 || StackOff % SlotBytes != 0)
      continue;

    int ArgPos = Scan.FirstStackSlot + static_cast<int>(StackOff / SlotBytes);
    // A slot the function reads back is a local stored before the call.
    if (isWin64(Scan) && Scan.LoadedEntrySlots &&
        Scan.LoadedEntrySlots->count(StackOff - Scan.FrameSize))
      continue;
    if (isWin64(Scan)) {
      // Home-area stores spill register arguments; they are not arguments.
      constexpr int64_t kHomeBytes = 32;
      constexpr int kRegisterArgs = 4;
      if (StackOff < kHomeBytes)
        continue;
      ArgPos =
          kRegisterArgs + static_cast<int>((StackOff - kHomeBytes) / SlotBytes);
    }
    if (ArgPos >= 0 && ArgPos < Scan.MaxArgs && !Found[ArgPos]) {
      Found[ArgPos] = Scan.ToExpr(Prev.Inputs[1]);
      StoredSlots.push_back({StackOff, AddrVar});
    }
  }

  // A Win64 stack slot the caller did not write between two it did is still
  // an argument: the callee reads whatever the slot holds.  Read it through
  // the address of a written neighbour rather than dropping later arguments.
  if (!isWin64(Scan) || StoredSlots.empty())
    return;
  int Last = -1;
  for (int K = 4; K < Scan.MaxArgs; ++K)
    if (Found[K])
      Last = K;
  const auto &[BaseOff, BaseAddr] = StoredSlots.front();
  for (int K = 4; K < Last; ++K) {
    if (Found[K])
      continue;
    const int64_t Off = 32 + int64_t(K - 4) * SlotBytes;
    ExprPtr Address = HighExpr::makeBinop(
        NdOp::INT_ADD, Scan.ToExpr(BaseAddr),
        HighExpr::makeConst(static_cast<uint64_t>(Off - BaseOff),
                            TRI.PointerSize));
    Found[K] = HighExpr::makeLoad(std::move(Address),
                                  NdType::makeInt(TRI.PointerSize));
  }
}

static bool preferScannedCallArg(const ExprPtr &Scanned, const ExprPtr &Hinted,
                                 size_t Slot) {
  if (!Scanned || Scanned->Kind == ExprKind::Undef)
    return false;
  if (!Hinted || Hinted->Kind == ExprKind::Undef)
    return true;
  if (Hinted->Kind == ExprKind::Record)
    return false;
  const bool ScannedSlot = Scanned->Kind == ExprKind::Var &&
                           Scanned->Var.Kind == MedVar::Param &&
                           Scanned->Var.Id == static_cast<int>(Slot);
  const bool HintedSlot = Hinted->Kind == ExprKind::Var &&
                          Hinted->Var.Kind == MedVar::Param &&
                          Hinted->Var.Id == static_cast<int>(Slot);
  if (ScannedSlot)
    return true;
  if (HintedSlot)
    return false;
  return true;
}

} // namespace call_args_detail

std::vector<ExprPtr>
MedToHighConverter::collectCallArgs(const MedBlock &CurBlock, size_t CallIdx) {
  const auto &Ops = CurBlock.Ops;
  std::vector<ExprPtr> Hinted;
  if (CallIdx < Ops.size() && Ops[CallIdx].SourceCallHint) {
    const auto &Call = Ops[CallIdx];
    const auto &Signature = Call.SourceCallHint->Signature;
    const auto Bindings = sourceABIParameters(Signature);
    const size_t Count = Signature.HasExplicitABI ? Bindings.size()
                                                  : Signature.Parameters.size();
    const bool HasComponents =
        std::any_of(Signature.Parameters.begin(), Signature.Parameters.end(),
                    [](const auto &P) { return !P.Components.empty(); });
    if ((Signature.HasExplicitABI || !HasComponents) &&
        Count <= static_cast<size_t>(limits::kMaxBoundSourceCallArgs) &&
        Call.NumInputs == Count + 1) {
      Hinted.reserve(Count);
      size_t Index = 0;
      for (const auto &P : Signature.Parameters) {
        if (P.Components.empty()) {
          Hinted.push_back(medvarToExpr(Call.Inputs[++Index]));
        } else {
          std::vector<ExprPtr> Leaves;
          for (const auto &Member : sourceAggregateMembers(P.Type))
            Leaves.push_back(
                sourceScalarValue(Call.Inputs[++Index], Member.Type));
          Hinted.push_back(HighExpr::makeRecord(P.Type, std::move(Leaves)));
        }
      }
    }
  }

  const int MaxArgs = limits::kMaxCallArgs;
  std::vector<ExprPtr> Found(MaxArgs);

  const auto &TRI = getTargetRegInfo(TargetArch);
  const BinaryFormat Format = Image ? Image->Format : BinaryFormat::Unknown;
  const auto ParamRegs = TRI.integerParamRegs(Format);
  const uint64_t SpRegOff = TRI.StackPointer;

  auto IsCalleeSave = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isCalleeSaveReg(V.RegOff);
  };

  bool ReachedBlockStart = true;
  for (int J = static_cast<int>(CallIdx) - 1; J >= 0; --J) {
    const MedOp &Prev = Ops[J];
    if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
        Prev.Opcode == NdOp::INTRINSIC) {
      ReachedBlockStart = false;
      break;
    }
    if (call_args_detail::isNoopRegisterCopy(Prev))
      continue;
    if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0) {
      const int ArgIdx = regToArgIdx(Prev.Output.RegOff);
      if (ArgIdx >= 0 && ArgIdx < MaxArgs && !Found[ArgIdx]) {
        if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1)
          Found[ArgIdx] = medvarToExpr(Prev.Inputs[0]);
        else
          Found[ArgIdx] = medOpToExpr(Prev);
      }
    }
  }

  if (ReachedBlockStart) {
    int MaxRegArg = -1;
    for (int K = 0; K < MaxArgs; ++K)
      if (Found[K])
        MaxRegArg = K;
    const bool Win64 =
        TargetArch == Arch::X64 && Image && Image->Format == BinaryFormat::COFF;
    // Fill holes below the highest written slot.  Do not extend arity with
    // live-in r9 when the call only wrote rcx/rdx/r8 (GSHandlerCheckCommon).
    // A tail-call with no param-reg writes still recovers live-in args up to
    // the current function's parameter count (`jmp __report_gsfailure`).
    int FillLast = MaxRegArg;
    if (Win64 && MaxRegArg < 0 && CurMed && !CurMed->Params.empty())
      FillLast = std::min(static_cast<int>(CurMed->Params.size()),
                          static_cast<int>(ParamRegs.size())) -
                 1;
    for (int K = 0; K <= FillLast && K < static_cast<int>(ParamRegs.size());
         ++K) {
      if (Found[K])
        continue;
      MedVar LiveIn;
      if (reachingRegAtBlockEntry(CurBlock, ParamRegs[K], LiveIn))
        Found[K] = medvarToExpr(LiveIn);
    }
  }

  const bool Win64 =
      TargetArch == Arch::X64 && Image && Image->Format == BinaryFormat::COFF;
  // Last-COPY-by-address across blocks is CFG-unsound: cookie `ror rcx`
  // would steal rcx from the mismatch path's incoming argument. Reaching
  // defs / same-block writes already recovered the live value.
  if (Win64 && CurMed) {
    auto UniqueDef = [&](const MedVar &V) -> const MedOp * {
      const MedOp *Def = nullptr;
      for (const auto &Blk : CurMed->Blocks)
        for (const auto &Op : Blk.Ops)
          if (Op.Output.Id == V.Id && Op.Output.SSAVer == V.SSAVer) {
            if (Def)
              return nullptr;
            Def = &Op;
          }
      return Def;
    };
    std::function<bool(const MedVar &, int)> IsConstLike =
        [&](const MedVar &V, int Depth) -> bool {
      if (Depth > 8)
        return false;
      if (V.isConst())
        return true;
      const MedOp *Def = UniqueDef(V);
      if (!Def || Def->NumInputs < 1)
        return false;
      if (Def->Opcode == NdOp::COPY)
        return IsConstLike(Def->Inputs[0], Depth + 1);
      return false;
    };
    std::function<bool(const MedVar &, int, int)> IsSlot =
        [&](const MedVar &V, int Slot, int Depth) -> bool {
      if (Depth > 8 || Slot < 0)
        return false;
      if (V.Kind == MedVar::Param)
        return abiParamIndex(V) == Slot;
      if (V.Kind == MedVar::Reg && V.SSAVer == 0)
        return regToArgIdx(V.RegOff) == Slot;
      for (const auto &Blk : CurMed->Blocks)
        for (const auto &Phi : Blk.Phis)
          if (Phi.Output.Id == V.Id && Phi.Output.SSAVer == V.SSAVer) {
            if (Phi.Args.empty())
              return false;
            for (const auto &Arg : Phi.Args)
              if (!IsSlot(Arg.second, Slot, Depth + 1))
                return false;
            return true;
          }
      const MedOp *Def = UniqueDef(V);
      if (!Def)
        return false;
      if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1)
        return IsSlot(Def->Inputs[0], Slot, Depth + 1);
      if ((Def->Opcode == NdOp::INT_LEFT || Def->Opcode == NdOp::INT_RIGHT ||
           Def->Opcode == NdOp::INT_OR || Def->Opcode == NdOp::INT_AND) &&
          Def->NumInputs >= 1) {
        bool Any = false;
        for (uint8_t I = 0; I < Def->NumInputs; ++I) {
          if (IsConstLike(Def->Inputs[I], 0))
            continue;
          if (!IsSlot(Def->Inputs[I], Slot, Depth + 1))
            return false;
          Any = true;
        }
        return Any;
      }
      return false;
    };
    for (int I = 0; I < 4 && I < static_cast<int>(CurMed->Params.size()); ++I) {
      if (!Found[I] || Found[I]->Kind != ExprKind::Var)
        continue;
      if (Found[I]->Var.Kind == MedVar::Param && Found[I]->Var.Id == I)
        continue;
      if (!IsSlot(Found[I]->Var, I, 0))
        continue;
      MedVar Param = CurMed->Params[static_cast<size_t>(I)];
      Param.Kind = MedVar::Param;
      Param.Id = I;
      Found[I] = HighExpr::makeVar(Param, TypeRef{});
    }
  }

  // A summarized Win64 callee published exactly the register arguments it
  // reads as the CALL's inputs (LowToMed); SSA already renamed them to the
  // values reaching the call, including a caller's pass-through argument.
  if (CallIdx < Ops.size() && Ops[CallIdx].CalleeRegisterArgs >= 0 &&
      !Ops[CallIdx].SourceCallHint) {
    const MedOp &Call = Ops[CallIdx];
    for (int I = 0; I < 4 && I < MaxArgs; ++I)
      Found[I] = I < Call.CalleeRegisterArgs && 1 + I < Call.NumInputs
                     ? medvarToExpr(Call.Inputs[1 + I])
                     : nullptr;
  }

  int FirstStackSlot = 0;
  for (int K = 0; K < MaxArgs; ++K) {
    if (Found[K])
      FirstStackSlot = K + 1;
    else
      break;
  }

  call_args_detail::CallArgScan Scan;
  Scan.Ops = &Ops;
  Scan.CallIdx = CallIdx;
  Scan.SpRegOff = SpRegOff;
  Scan.TRI = &TRI;
  Scan.Image = Image;
  Scan.TheArch = TargetArch;
  Scan.MaxArgs = MaxArgs;
  Scan.FirstStackSlot = FirstStackSlot;
  auto ToExpr = [this](const MedVar &V) { return medvarToExpr(V); };
  Scan.ToExpr = ToExpr;
  Scan.IsCalleeSave = IsCalleeSave;
  auto ReachingRegArg = [&](int Index) -> ExprPtr {
    if (Index < 0 || Index >= static_cast<int>(ParamRegs.size()))
      return nullptr;
    MedVar LiveIn;
    if (reachingRegAtBlockEntry(CurBlock, ParamRegs[Index], LiveIn))
      return medvarToExpr(LiveIn);
    return nullptr;
  };
  Scan.ReachingRegArg = ReachingRegArg;
  std::function<std::optional<int64_t>(const MedVar &, int)> EntryOffset =
      [&](const MedVar &V, int Depth) -> std::optional<int64_t> {
    if (!CurMed || Depth > 8)
      return std::nullopt;
    if (V.Kind == MedVar::Reg && V.RegOff == SpRegOff && V.SSAVer == 0)
      return 0;
    const MedOp *Def = nullptr;
    for (const auto &Blk : CurMed->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
            Op.Output.SSAVer == V.SSAVer) {
          if (Def)
            return std::nullopt;
          Def = &Op;
        }
    if (!Def || Def->NumInputs < 1)
      return std::nullopt;
    if (Def->Opcode == NdOp::COPY)
      return EntryOffset(Def->Inputs[0], Depth + 1);
    if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
        Def->NumInputs == 2 && Def->Inputs[1].isConst()) {
      auto Base = EntryOffset(Def->Inputs[0], Depth + 1);
      if (!Base)
        return std::nullopt;
      const int64_t C = static_cast<int64_t>(Def->Inputs[1].ConstVal);
      return Def->Opcode == NdOp::INT_ADD ? *Base + C : *Base - C;
    }
    return std::nullopt;
  };
  auto EntryOffsetOf = [&](const MedVar &V) { return EntryOffset(V, 0); };
  Scan.EntryOffsetOf = EntryOffsetOf;
  Scan.FrameSize = CurMed ? CurMed->FrameSize : 0;
  if (CurMed && LoadedEntrySlotsFor != CurMed) {
    LoadedEntrySlots.clear();
    auto AddSlot = [&](const MedVar &V) {
      if (auto Off = EntryOffsetOf(V))
        LoadedEntrySlots.insert(*Off);
    };
    for (const auto &Blk : CurMed->Blocks)
      for (const auto &Op : Blk.Ops) {
        if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          continue;
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
          AddSlot(Op.Inputs[0]);
        // A slot whose address escapes (kept in a register, stored, or
        // passed to a callee) is a local the callee or a later load reads
        // through that pointer, not an outgoing argument.
        // The stack and frame pointers themselves only locate the frame.
        if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::COPY) &&
            Op.Output.Kind == MedVar::Reg && Op.Output.RegOff != SpRegOff &&
            Op.Output.RegOff != TRI.FramePointer)
          AddSlot(Op.Output);
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
          AddSlot(Op.Inputs[1]);
      }
    LoadedEntrySlotsFor = CurMed;
  }
  Scan.LoadedEntrySlots = &LoadedEntrySlots;

  std::vector<ExprPtr> Args;
  switch (TargetArch) {
  case Arch::X86:
  case Arch::X64:
    call_args_detail::collectCallArgsX86(Scan, Found, Args);
    break;
  case Arch::ARM:
    call_args_detail::collectCallArgsARM(Scan, Found, Args);
    break;
  case Arch::AArch64:
    call_args_detail::collectCallArgsAArch64(Scan, Found, Args);
    break;
  default:
    call_args_detail::collectSpilledStackArgs(Scan, Found);
    break;
  }

  if (Args.empty()) {
    for (int K = 0; K < MaxArgs; ++K) {
      if (!Found[K])
        break;
      Args.push_back(Found[K]);
    }
  }

  // A callee summary already published exactly the register arguments the
  // callee reads, SSA-renamed to their reaching values; guessing a
  // pass-through parameter for an unread slot would print the function's
  // incoming RCX where the caller loaded 0xC9.
  const bool SummarizedCallee = CallIdx < Ops.size() &&
                                Ops[CallIdx].CalleeRegisterArgs >= 0 &&
                                !Ops[CallIdx].SourceCallHint;
  if (Win64 && CurMed && !SummarizedCallee) {
    size_t FillTo = 0;
    if (!Hinted.empty())
      FillTo = std::min(Hinted.size(), static_cast<size_t>(4));
    else {
      for (int K = 3; K >= 0; --K)
        if (Found[K] && Found[K]->Kind != ExprKind::Undef) {
          FillTo = static_cast<size_t>(K + 1);
          break;
        }
      // Tail-call with no param-reg writes (`jmp __report_gsfailure`): rcx is
      // still the incoming cookie. Do not extend a 3-arg call with live-in r9.
      if (FillTo == 0 && !CurMed->Params.empty())
        FillTo = 1;
    }
    for (size_t I = 0; I < FillTo && I < CurMed->Params.size(); ++I) {
      if ((Found[I] && Found[I]->Kind != ExprKind::Undef) ||
          CurMed->Params[I].RegOff == kNoParamReg)
        continue;
      MedVar Param = CurMed->Params[I];
      Param.Kind = MedVar::Param;
      Param.Id = static_cast<int>(I);
      Found[I] = HighExpr::makeVar(Param, TypeRef{});
    }
  }

  auto BoundKnownCalleeArity = [&](std::vector<ExprPtr> Collected) {
    if (CallIdx >= Ops.size())
      return Collected;
    const MedOp &Call = Ops[CallIdx];
    std::string Name;
    if (Call.SourceCallHint && !Call.SourceCallHint->TargetName.empty())
      Name = Call.SourceCallHint->TargetName;
    else if (Call.NumInputs >= 1 && Call.Inputs[0].isConst())
      Name = calleeDisplayName(Call.Inputs[0].ConstVal);
    if (Name.empty())
      return Collected;
    const auto Arity = libc::libcArityForSymbol(Name);
    if (!Arity)
      return Collected;
    const size_t N = static_cast<size_t>(std::max(0, Arity->IntArgs) +
                                         std::max(0, Arity->FpArgs));
    if (Collected.size() > N)
      Collected.resize(N);
    return Collected;
  };

  if (!Hinted.empty()) {
    // Source ABI operands are authoritative. Win64 may still prefer a scanned
    // parameter copy over a clobbered CALL input; AArch64 selector stubs must
    // not replace `_cmd` with the const-0 overwrite placeholder.
    if (Win64) {
      for (size_t I = 0; I < Hinted.size(); ++I) {
        const ExprPtr Scanned = I < Found.size() ? Found[I] : ExprPtr{};
        if (call_args_detail::preferScannedCallArg(Scanned, Hinted[I], I))
          Hinted[I] = Scanned;
      }
    }
    return Hinted;
  }
  Args.clear();
  for (int K = 0; K < MaxArgs; ++K) {
    if (!Found[K] || Found[K]->Kind == ExprKind::Undef)
      break;
    Args.push_back(Found[K]);
  }
  return BoundKnownCalleeArity(std::move(Args));
}

bool MedToHighConverter::reachingRegAtBlockEntry(const MedBlock &B,
                                                 uint64_t RegOff,
                                                 MedVar &Out) const {
  if (!CurMed)
    return false;

  auto blockById = [&](int Id) -> const MedBlock * {
    for (const auto &Blk : CurMed->Blocks)
      if (Blk.Id == Id)
        return &Blk;
    return nullptr;
  };

  std::set<int> Visited;
  std::function<bool(const MedBlock &, MedVar &)> entryOf;
  std::function<bool(const MedBlock &, MedVar &)> exitOf;
  entryOf = [&](const MedBlock &Blk, MedVar &R) -> bool {
    if (!Visited.insert(Blk.Id).second)
      return false;
    for (const auto &Phi : Blk.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == RegOff &&
          Phi.Output.Size > 0) {
        R = Phi.Output;
        return true;
      }
    for (int P : Blk.Preds)
      if (const MedBlock *PB = blockById(P))
        if (exitOf(*PB, R))
          return true;
    return false;
  };
  exitOf = [&](const MedBlock &Blk, MedVar &R) -> bool {
    for (auto It = Blk.Ops.rbegin(); It != Blk.Ops.rend(); ++It)
      if (It->Output.Kind == MedVar::Reg && It->Output.RegOff == RegOff &&
          It->Output.Size > 0) {
        R = It->Output;
        return true;
      }
    return entryOf(Blk, R);
  };

  return entryOf(B, Out);
}

int MedToHighConverter::regToArgIdx(uint64_t RegOff) const {
  const bool IsWin64 =
      TargetArch == Arch::X64 && Image && Image->Format == BinaryFormat::COFF;
  return getTargetRegInfo(TargetArch).regToArgIdx(RegOff, IsWin64);
}

std::string MedToHighConverter::calleeDisplayName(va_t Target) const {
  auto Synthetic = [Target] {
    return (kAutoFuncPrefix + llvm::utohexstr(Target)).str();
  };
  auto Usable = [](llvm::StringRef Name) {
    return !Name.empty() && !Name.starts_with(kAutoFuncPrefix);
  };
  if (FuncNames) {
    auto It = FuncNames->find(Target);
    if (It != FuncNames->end() && Usable(It->second))
      return It->second;
  }
  if (Image) {
    if (const Import *Imp = Image->findImportAt(Target);
        Imp && Usable(Imp->Name))
      return Imp->Name;
    const std::string FromImage = Image->getFunctionNameAt(Target);
    if (Usable(FromImage))
      return FromImage;
  }
  return Synthetic();
}

int MedToHighConverter::abiParamIndex(const MedVar &V) const {
  if (!CurMed)
    return V.Kind == MedVar::Param ? V.Id : -1;

  auto SlotByReg = [&](uint64_t RegOff) -> int {
    if (RegOff == kNoParamReg)
      return -1;
    for (size_t I = 0; I < CurMed->Params.size(); ++I)
      if (CurMed->Params[I].RegOff == RegOff)
        return static_cast<int>(I);
    const int Idx = regToArgIdx(RegOff);
    if (Idx >= 0 && static_cast<size_t>(Idx) < CurMed->Params.size())
      return Idx;
    return -1;
  };

  if (V.Kind == MedVar::Param) {
    int IdMatch = -1;
    for (size_t I = 0; I < CurMed->Params.size(); ++I) {
      const MedVar &P = CurMed->Params[I];
      if (P.Id < 0 || P.Id != V.Id)
        continue;
      if (V.RegOff != kNoParamReg && P.RegOff == V.RegOff)
        return static_cast<int>(I);
      if (IdMatch < 0)
        IdMatch = static_cast<int>(I);
    }
    if (IdMatch >= 0)
      return IdMatch;
    const int ByReg = SlotByReg(V.RegOff);
    if (ByReg >= 0)
      return ByReg;
    if (V.Id >= 0 && static_cast<size_t>(V.Id) < CurMed->Params.size())
      return V.Id;
    return -1;
  }

  if (V.Kind == MedVar::Reg)
    return SlotByReg(V.RegOff);
  return -1;
}

} // namespace neverd
