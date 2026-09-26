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
#include <optional>
#include <set>

namespace {

bool isAddressAddOp(const neverd::MedOp &Op) {
  if (Op.Opcode != neverd::NdOp::INT_ADD || Op.NumInputs < 2)
    return false;
  return Op.Inputs[0].isConst() != Op.Inputs[1].isConst();
}

const neverd::MedOp *uniqueSsaDef(const neverd::MedFunc *Func,
                                  const neverd::MedVar &V) {
  if (!Func || V.Id < 0)
    return nullptr;
  const neverd::MedOp *Def = nullptr;
  for (const auto &Blk : Func->Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Output.Id == V.Id && Op.Output.SSAVer == V.SSAVer) {
        if (Def)
          return nullptr;
        Def = &Op;
      }
  return Def;
}

bool isLeaLikeCallSetup(const neverd::MedFunc *Func, const neverd::MedOp &Op) {
  if (isAddressAddOp(Op))
    return true;
  if (Op.Opcode != neverd::NdOp::COPY || Op.NumInputs < 1)
    return false;
  const neverd::MedOp *Src = uniqueSsaDef(Func, Op.Inputs[0]);
  return Src && isAddressAddOp(*Src);
}

} // namespace

namespace neverd {

namespace call_args_detail {

void collectSpilledStackArgs(const CallArgScan &Scan,
                             std::vector<ExprPtr> &Found) {
  const TargetRegInfo &TRI = *Scan.TRI;
  const int64_t SlotBytes = static_cast<int64_t>(TRI.PointerSize);
  const bool Win64 = Scan.TheArch == Arch::X64 && Scan.Image &&
                     Scan.Image->Format == BinaryFormat::COFF;
  const auto Layout = TRI.integerArgumentLayout(Win64);

  const BinaryFormat Format = Scan.Image ? Scan.Image->Format : BinaryFormat::Unknown;
  auto preserved = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg &&
           isCallPreservedReg(TRI, Format, V.RegOff, V.Size);
  };

  auto considerStore = [&](const std::vector<MedOp> &Ops, int J) {
    const MedOp &Prev = Ops[static_cast<size_t>(J)];
    if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
        Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return;
    MedVar Stored = Prev.Inputs[1];
    // Incoming callee-save spills (SSA 0) are prologue saves. A computed
    // length parked in ESI/RDI is a real outgoing stack argument
    // (for example, a value stored at [rsp+0x20]).
    if (preserved(Stored) && Stored.SSAVer == 0)
      return;
    if (!Scan.ResolveWindow && Stored.Kind == MedVar::Reg) {
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &Def = Ops[static_cast<size_t>(K)];
        if ((Def.Opcode == NdOp::CALL || Def.Opcode == NdOp::INDIR_CALL ||
             Def.Opcode == NdOp::INTRINSIC) &&
            !preserved(Stored))
          break;
        if (Def.Output.Kind != MedVar::Reg || Def.Output.Size == 0 ||
            Def.Output.RegOff != Stored.RegOff)
          continue;
        if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1)
          Stored = Def.Inputs[0];
        else
          Stored = Def.Output;
        break;
      }
    }

    const MedVar &AddrVar = Prev.Inputs[0];
    int64_t StackOff = -1;

    if (AddrVar.Kind == MedVar::Reg && AddrVar.RegOff == Scan.SpRegOff)
      StackOff = 0;

    if (StackOff < 0 && !AddrVar.isConst()) {
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &DefOp = Ops[static_cast<size_t>(K)];
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

    if (StackOff < 0 || SlotBytes == 0)
      return;
    int ArgPos = -1;
    if (Win64) {
      if (StackOff < Layout.CallStackBase)
        return;
      const int64_t SlotOff = StackOff - Layout.CallStackBase;
      if (SlotOff % SlotBytes != 0)
        return;
      ArgPos = static_cast<int>(Layout.Registers.size()) +
               static_cast<int>(SlotOff / SlotBytes);
    } else {
      if (StackOff >= Scan.MaxArgs * SlotBytes || StackOff % SlotBytes != 0)
        return;
      ArgPos = Scan.FirstStackSlot + static_cast<int>(StackOff / SlotBytes);
    }
    if (ArgPos < 0 || ArgPos >= Scan.MaxArgs || Found[ArgPos])
      return;
    if (Scan.ResolveWindow) {
      if (ExprPtr E = Scan.ResolveWindow(Prev.Inputs[1], Ops, J - 1))
        if (E->Kind != ExprKind::Undef) {
          Found[ArgPos] = std::move(E);
          return;
        }
    }
    for (int Peel = 0; Peel < 4; ++Peel) {
      ExprPtr E = Scan.ToExpr(Stored);
      if (E && E->Kind != ExprKind::Undef) {
        Found[ArgPos] = std::move(E);
        return;
      }
      if (Stored.Kind != MedVar::Reg)
        return;
      bool Progress = false;
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &Def = Ops[static_cast<size_t>(K)];
        if ((Def.Opcode == NdOp::CALL || Def.Opcode == NdOp::INDIR_CALL ||
             Def.Opcode == NdOp::INTRINSIC) &&
            !preserved(Stored))
          break;
        const bool SameSsa = Def.Output.Id == Stored.Id &&
                             Def.Output.SSAVer == Stored.SSAVer;
        const bool SameReg = Def.Output.Kind == MedVar::Reg &&
                             Def.Output.Size > 0 &&
                             Def.Output.RegOff == Stored.RegOff;
        if (!SameSsa && !SameReg)
          continue;
        if (Def.NumInputs >= 1 && Def.Inputs[0].Kind == MedVar::Reg)
          Stored = Def.Inputs[0];
        else if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1)
          Stored = Def.Inputs[0];
        else
          Stored = Def.Output;
        Progress = true;
        break;
      }
      if (!Progress)
        return;
    }
  };

  auto scanWindow = [&](const std::vector<MedOp> &Ops, int Before,
                        int WindowFloor) {
    for (int J = Before; J >= WindowFloor; --J) {
      const MedOp &Prev = Ops[static_cast<size_t>(J)];
      if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
          Prev.Opcode == NdOp::INTRINSIC)
        break;
      // HighIR has no MedCallInfo on the ordinary decompile path. Match the
      // MedABI scan's call boundary, but do not attach stores from an older
      // Win64 outgoing frame to this call. An SP-to-SP copy may only cross
      // the scan boundary when it copies the currently reaching SP value;
      // restoring an older SP version changes the outgoing frame again.
      if (Win64 && Prev.Output.Kind == MedVar::Reg &&
          Prev.Output.Size != 0 && Prev.Output.RegOff == Scan.SpRegOff) {
        const bool SpCopy =
            Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1 &&
            Prev.Inputs[0].Kind == MedVar::Reg &&
            Prev.Inputs[0].RegOff == Scan.SpRegOff &&
            Prev.Inputs[0].Size == Prev.Output.Size;
        bool SameSpBase = SpCopy && isNoopRegisterCopy(Prev);
        if (SpCopy && !SameSpBase)
          for (int K = J - 1; K >= WindowFloor; --K) {
            const MedOp &Def = Ops[static_cast<size_t>(K)];
            if (Def.Opcode == NdOp::CALL ||
                Def.Opcode == NdOp::INDIR_CALL ||
                Def.Opcode == NdOp::INTRINSIC)
              break;
            if (Def.Output.Kind != MedVar::Reg || Def.Output.Size == 0 ||
                Def.Output.RegOff != Scan.SpRegOff)
              continue;
            SameSpBase = Def.Output.Id == Prev.Inputs[0].Id &&
                         Def.Output.SSAVer == Prev.Inputs[0].SSAVer &&
                         Def.Output.Size == Prev.Inputs[0].Size;
            break;
          }
        if (!SameSpBase)
          break;
      }
      considerStore(Ops, J);
    }
  };

  const int StoreScanStart =
      Win64 ? 0 : std::max(0, static_cast<int>(Scan.CallIdx) -
                                 Scan.StoreScanWindow);
  scanWindow(*Scan.Ops, static_cast<int>(Scan.CallIdx) - 1, StoreScanStart);
  for (const auto &W : Scan.ExtraWindows)
    if (W.Ops)
      scanWindow(*W.Ops, W.Before, 0);
}

static bool preferScannedCallArg(const ExprPtr &Scanned, const ExprPtr &Hinted,
                                 size_t Slot) {
  if (!Scanned || Scanned->Kind == ExprKind::Undef)
    return false;
  if (!Hinted || Hinted->Kind == ExprKind::Undef)
    return true;
  if (Hinted->Kind == ExprKind::Record)
    return false;
  const bool ScannedParam = Scanned->Kind == ExprKind::Var &&
                            Scanned->Var.Kind == MedVar::Param;
  const bool HintedParam = Hinted->Kind == ExprKind::Var &&
                           Hinted->Var.Kind == MedVar::Param;
  const bool ScannedSlot =
      ScannedParam && Scanned->Var.Id == static_cast<int>(Slot);
  const bool HintedSlot =
      HintedParam && Hinted->Var.Id == static_cast<int>(Slot);
  if (ScannedSlot)
    return true;
  // `mov rcx, item` leaves a different param in slot 0.  The CALL input is
  // still the incoming sret; the rewrite is the argument.
  if (HintedSlot && ScannedParam &&
      Scanned->Var.Id != static_cast<int>(Slot))
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
    if ((!Signature.HasExplicitABI && HasComponents) ||
        Count > static_cast<size_t>(limits::kMaxBoundSourceCallArgs) ||
        Call.NumInputs != Count + 1)
      return {};
    {
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
    // A validated zero-argument binding is a complete result. An empty list
    // must not select the unbound-call heuristic and acquire live registers.
    if (Hinted.empty())
      return Hinted;
  }

  const int MaxArgs = limits::kMaxCallArgs;
  std::vector<ExprPtr> Found(MaxArgs);

  const auto &TRI = getTargetRegInfo(TargetArch);
  const BinaryFormat Format = Image ? Image->Format : BinaryFormat::Unknown;
  const auto ParamRegs = TRI.integerParamRegs(Format);
  const bool Win64 =
      TargetArch == Arch::X64 && Format == BinaryFormat::COFF;
  const auto IntLayout = TRI.integerArgumentLayout(Win64);
  auto integerSlot = [&](uint64_t RegOff) -> int {
    // Win64 `regToArgIdx` maps XMM0 onto slot 0, same as RCX. A `movups`
    // leftover is not integer `this` for cstr/GetLength.
    if (Win64)
      return IntLayout.registerIndex(RegOff);
    return regToArgIdx(RegOff);
  };
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
      const int ArgIdx = integerSlot(Prev.Output.RegOff);
      if (ArgIdx >= 0 && ArgIdx < MaxArgs && !Found[ArgIdx]) {
        if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1)
          Found[ArgIdx] = medvarToExpr(Prev.Inputs[0]);
        else
          Found[ArgIdx] = medOpToExpr(Prev);
      }
    }
  }
  // Else-edge `mov r9d, 0` sunk into the join must not beat the r9 PHI
  // (`p ? p->field : 0` for a lookup). A same-block copy of
  // the then-arm incoming is the same leftover.
  for (int K = 0; K < static_cast<int>(ParamRegs.size()) && K < MaxArgs; ++K) {
    if (!Found[K])
      continue;
    for (const auto &Phi : CurBlock.Phis) {
      if (Phi.Output.Kind != MedVar::Reg || Phi.Output.Size == 0 ||
          Phi.Output.RegOff != ParamRegs[K])
        continue;
      bool Replace = Found[K]->Kind == ExprKind::Const;
      if (!Replace) {
        for (const auto &A : Phi.Args) {
          if (A.second.isConst())
            continue;
          ExprPtr Inc = medvarToExpr(A.second);
          if (Inc && Found[K]->structuralEq(*Inc)) {
            Replace = true;
            break;
          }
        }
      }
      if (Replace)
        Found[K] = medvarToExpr(Phi.Output);
      break;
    }
  }

  std::vector<call_args_detail::CallArgScan::OpWindow> ExtraWindows;
  auto blockById = [&](int Id) -> const MedBlock * {
    if (!CurMed)
      return nullptr;
    for (const auto &Blk : CurMed->Blocks)
      if (Blk.Id == Id)
        return &Blk;
    return nullptr;
  };
  // `COPY R9 = RDI.61` after `COPY RDI.59 = rax` leaves a dangling SSA.
  // Walk the trailing window for the last write of that register.
  auto exprFromWindowValue = [&](MedVar V, const std::vector<MedOp> &Ops,
                                 int Before) -> ExprPtr {
    const BinaryFormat Format =
        Image ? Image->Format : BinaryFormat::Unknown;
    auto preserved = [&](const MedVar &Reg) {
      return Reg.Kind == MedVar::Reg &&
             call_args_detail::isCallPreservedReg(TRI, Format, Reg.RegOff,
                                                 Reg.Size);
    };
    for (int Peel = 0; Peel < 8; ++Peel) {
      ExprPtr E = medvarToExpr(V);
      if (E && E->Kind != ExprKind::Undef)
        return E;
      if (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp)
        return E;
      bool Progress = false;
      for (int K = Before; K >= 0; --K) {
        const MedOp &Def = Ops[static_cast<size_t>(K)];
        const bool SameSsa = Def.Output.Id == V.Id &&
                             Def.Output.SSAVer == V.SSAVer &&
                             Def.Output.Size > 0;
        const bool SameReg = V.Kind == MedVar::Reg &&
                             Def.Output.Kind == MedVar::Reg &&
                             Def.Output.Size > 0 &&
                             Def.Output.RegOff == V.RegOff;
        if (SameSsa || SameReg) {
          if (Def.Opcode == NdOp::CALL || Def.Opcode == NdOp::INDIR_CALL ||
              Def.Opcode == NdOp::INTRINSIC)
            return medOpToExpr(Def);
          if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
            ExprPtr Src = medvarToExpr(Def.Inputs[0]);
            if (Src && Src->Kind != ExprKind::Undef)
              return Src;
            V = Def.Inputs[0];
            Before = K - 1;
            Progress = true;
            break;
          }
          if ((Def.Opcode == NdOp::SUBBYTES || Def.Opcode == NdOp::INT_ZEXT ||
               Def.Opcode == NdOp::INT_SEXT || Def.Opcode == NdOp::CAST) &&
              Def.NumInputs >= 1) {
            V = Def.Inputs[0];
            Before = K - 1;
            Progress = true;
            break;
          }
          ExprPtr D = medOpToExpr(Def);
          if (D && D->Kind != ExprKind::Undef)
            return D;
        }
        if ((Def.Opcode == NdOp::CALL || Def.Opcode == NdOp::INDIR_CALL ||
             Def.Opcode == NdOp::INTRINSIC) &&
            !preserved(V))
          break;
      }
      if (!Progress)
        return E;
    }
    return medvarToExpr(V);
  };
  // IP-map / EH splits isolate the CALL. Trailing `mov r9, rdi` /
  // `mov [rsp+20h], esi` then sit in the predecessor after the last
  // helper CALL (GetLength/cstr) and must be collected here — leftover
  // Find nKey in r9 is before those helpers, so it stays out.
  if (ReachedBlockStart && CallIdx == 0 && CurBlock.Preds.size() == 1 &&
      (Win64 || TargetArch == Arch::X86)) {
    for (int PredId : CurBlock.Preds) {
      const MedBlock *Pred = blockById(PredId);
      if (!Pred || Pred->Ops.empty())
        continue;
      ExtraWindows.push_back({&Pred->Ops,
                              static_cast<int>(Pred->Ops.size()) - 1});
      // i386 stdcall pushes live in ExtraWindows; ECX/EDX are not arguments.
      if (!Win64)
        continue;
      for (int J = static_cast<int>(Pred->Ops.size()) - 1; J >= 0; --J) {
        const MedOp &Prev = Pred->Ops[static_cast<size_t>(J)];
        if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
            Prev.Opcode == NdOp::INTRINSIC)
          break;
        if (call_args_detail::isNoopRegisterCopy(Prev))
          continue;
        if (Prev.Output.Kind != MedVar::Reg || Prev.Output.Size == 0)
          continue;
        const int ArgIdx = integerSlot(Prev.Output.RegOff);
        if (ArgIdx < 0 || ArgIdx >= MaxArgs || Found[ArgIdx])
          continue;
        if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1)
          Found[ArgIdx] =
              exprFromWindowValue(Prev.Inputs[0], Pred->Ops, J - 1);
        else
          Found[ArgIdx] = medOpToExpr(Prev);
        if (!Found[ArgIdx] || Found[ArgIdx]->Kind == ExprKind::Undef)
          Found[ArgIdx] = exprFromWindowValue(Prev.Output, Pred->Ops, J - 1);
      }
    }
  }

  if (ReachedBlockStart) {
    int MaxRegArg = -1;
    for (int K = 0; K < MaxArgs; ++K)
      if (Found[K] && Found[K]->Kind != ExprKind::Undef)
        MaxRegArg = K;
    // Fill holes below the highest written slot.  Do not extend arity with
    // live-in r9 when the call only wrote rcx/rdx/r8 (GSHandlerCheckCommon).
    // A tail-call with no param-reg writes still recovers live-in args up to
    // the current function's parameter count (`jmp __report_gsfailure`).
    int FillLast = MaxRegArg;
    auto predSetupExpr = [&](const MedVar &LiveIn) -> ExprPtr {
      const MedBlock *Pred = nullptr;
      const MedOp *Def = nullptr;
      int DefIdx = -1;
      int Hits = 0;
      if (CurMed) {
        for (const auto &Blk : CurMed->Blocks) {
          for (int I = 0; I < static_cast<int>(Blk.Ops.size()); ++I) {
            const MedOp &Op = Blk.Ops[static_cast<size_t>(I)];
            if (Op.Output.Id != LiveIn.Id ||
                Op.Output.SSAVer != LiveIn.SSAVer)
              continue;
            ++Hits;
            Pred = &Blk;
            Def = &Op;
            DefIdx = I;
          }
        }
      }
      if (Hits != 1) {
        Pred = nullptr;
        Def = nullptr;
        DefIdx = -1;
        for (int PredId : CurBlock.Preds) {
          Pred = blockById(PredId);
          if (!Pred)
            continue;
          for (int I = 0; I < static_cast<int>(Pred->Ops.size()); ++I) {
            const MedOp &Op = Pred->Ops[static_cast<size_t>(I)];
            if (Op.Output.Id == LiveIn.Id &&
                Op.Output.SSAVer == LiveIn.SSAVer) {
              Def = &Op;
              DefIdx = I;
              break;
            }
          }
          if (Def)
            break;
        }
      }
      if (!Def)
        return medvarToExpr(LiveIn);
      ExprPtr E = medOpToExpr(*Def);
      if (E && E->Kind != ExprKind::Undef)
        return E;
      if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1 && Pred)
        E = exprFromWindowValue(Def->Inputs[0], Pred->Ops, DefIdx - 1);
      if (E && E->Kind != ExprKind::Undef)
        return E;
      if (Pred)
        return exprFromWindowValue(LiveIn, Pred->Ops, DefIdx - 1);
      return E;
    };
    auto tryPredSetupArg = [&](int K) -> bool {
      MedVar LiveIn;
      if (!reachingRegAtBlockEntry(CurBlock, ParamRegs[K], LiveIn))
        return false;
      if (!isCallArgSetupDef(CurBlock, LiveIn, ParamRegs[K]))
        return false;
      ExprPtr E = predSetupExpr(LiveIn);
      if (!E || E->Kind == ExprKind::Undef)
        return false;
      Found[K] = std::move(E);
      return true;
    };
    // Join `mov edx; jmp call` keeps a dominating `lea r8` in the shared
    // fork. Leftover r9 from an earlier Find is not that fork write.
    auto tryDominatingForkSetup = [&](int K) -> bool {
      if (Found[K] || CurBlock.Preds.size() < 2)
        return false;
      std::optional<int> ForkId;
      for (int PredId : CurBlock.Preds) {
        const MedBlock *Pred = blockById(PredId);
        if (!Pred || Pred->Preds.size() != 1)
          return false;
        if (!ForkId)
          ForkId = Pred->Preds[0];
        else if (*ForkId != Pred->Preds[0])
          return false;
      }
      const MedBlock *Fork = ForkId ? blockById(*ForkId) : nullptr;
      if (!Fork)
        return false;
      MedVar LiveIn;
      if (!reachingRegAtBlockEntry(CurBlock, ParamRegs[K], LiveIn))
        return false;
      for (const auto &Op : Fork->Ops) {
        if (Op.Output.Id != LiveIn.Id || Op.Output.SSAVer != LiveIn.SSAVer)
          continue;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
            Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Inputs[0].RegOff == ParamRegs[K] &&
            Op.Inputs[0].Id == Op.Output.Id &&
            Op.Inputs[0].SSAVer == Op.Output.SSAVer)
          return false;
        if (Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != ParamRegs[K] ||
            Op.Output.Size == 0)
          return false;
        Found[K] = medOpToExpr(Op);
        return true;
      }
      return false;
    };
    // Call-only block: recover consecutive live-ins that a predecessor wrote
    // as call setup (`lea r8, name` before a callee). Do not use the
    // caller's parameter count — that drops a 3rd callee arg when the
    // caller is a 2-param sret method.
    if (Win64 && MaxRegArg < 0) {
      int SetupLast = -1;
      for (int K = 0; K < static_cast<int>(ParamRegs.size()) && K < 4; ++K) {
        if (tryPredSetupArg(K) || tryDominatingForkSetup(K))
          SetupLast = K;
      }
      if (SetupLast >= 0)
        FillLast = SetupLast;
      else if (CurMed && !CurMed->Params.empty())
        FillLast = std::min(static_cast<int>(CurMed->Params.size()),
                            static_cast<int>(ParamRegs.size())) -
                   1;
    }
    // Same-block writes of rcx/rdx/r8 must not hide a join PHI in r9
    // (`CStringTable::Find` nKey).  Unused function-entry r9 is not a PHI.
    if (Win64 && MaxRegArg >= 0) {
      int Next = MaxRegArg + 1;
      while (Next < static_cast<int>(ParamRegs.size()) && Next < MaxArgs) {
        MedVar LiveIn;
        if (!reachingRegAtBlockEntry(CurBlock, ParamRegs[Next], LiveIn))
          break;
        bool IsJoinPhi = false;
        for (const auto &Phi : CurBlock.Phis) {
          if (Phi.Output.Id == LiveIn.Id &&
              Phi.Output.SSAVer == LiveIn.SSAVer) {
            IsJoinPhi = true;
            break;
          }
        }
        if (!IsJoinPhi)
          break;
        Found[Next] = medvarToExpr(LiveIn);
        FillLast = Next;
        ++Next;
      }
      // `lea r8, name` can sit in the predecessor when `cmp/jnz` splits
      // it from `mov edx; call`. Recover that setup; leftover r9 whose
      // pred did not write it stays out.
      int SetupNext = FillLast + 1;
      while (SetupNext < static_cast<int>(ParamRegs.size()) &&
             SetupNext < MaxArgs &&
             (tryPredSetupArg(SetupNext) ||
              tryDominatingForkSetup(SetupNext))) {
        FillLast = SetupNext;
        ++SetupNext;
      }
    }
    for (int K = 0; K <= FillLast && K < static_cast<int>(ParamRegs.size());
         ++K) {
      if (Found[K])
        continue;
      MedVar LiveIn;
      if (reachingRegAtBlockEntry(CurBlock, ParamRegs[K], LiveIn))
        Found[K] = medvarToExpr(LiveIn);
    }
  }

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
      int SrcSlot = -1;
      const int Limit = std::min(4, static_cast<int>(CurMed->Params.size()));
      for (int J = 0; J < Limit; ++J) {
        if (!IsSlot(Found[I]->Var, J, 0))
          continue;
        SrcSlot = J;
        break;
      }
      if (SrcSlot < 0)
        continue;
      if (Found[I]->Var.Kind == MedVar::Param &&
          abiParamIndex(Found[I]->Var) == SrcSlot)
        continue;
      MedVar Param = CurMed->Params[static_cast<size_t>(SrcSlot)];
      Param.Kind = MedVar::Param;
      Param.Id = SrcSlot;
      Found[I] = HighExpr::makeVar(Param, TypeRef{});
    }
  }

  // `mov r8d, [p+field]; call` is often `LOAD t; ZEXT r8, t`. The zext is the
  // last param-reg write, so the call would keep a dangling SSA of t if the
  // load assign is later DCE'd. The argument is the loaded value.
  auto sameSsaDef = [](const MedVar &Out, const MedVar &Wanted) {
    if (Out.Id != Wanted.Id || Out.SSAVer != Wanted.SSAVer || Out.Size == 0 ||
        Out.Kind != Wanted.Kind)
      return false;
    if (Out.Kind == MedVar::Reg)
      return Out.RegOff == Wanted.RegOff;
    return true;
  };
  auto attachProducingLoad = [&](ExprPtr E, const std::vector<MedOp> &Win,
                                 int Before) -> ExprPtr {
    if (!E)
      return E;
    const HighExpr *Cur = E.get();
    bool PeeledView = false;
    for (int Peel = 0; Peel < 8 && Cur; ++Peel) {
      if (Cur->Kind == ExprKind::Load)
        return E;
      const bool View =
          (Cur->Kind == ExprKind::UnaryOp &&
           (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT ||
            Cur->Op == NdOp::CAST)) ||
          Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast;
      if (!View || Cur->Operands.empty() || !Cur->Operands[0])
        break;
      PeeledView = true;
      Cur = Cur->Operands[0].get();
    }
    // A bare live-in register is not a widened load. Matching Id/SSA across
    // Kind would steal a later integer LOAD onto a dominating `lea rcx`.
    if (!PeeledView || !Cur || Cur->Kind != ExprKind::Var || Cur->Var.Id < 0)
      return E;
    MedVar Wanted = Cur->Var;
    for (int K = Before; K >= 0; --K) {
      const MedOp &Def = Win[static_cast<size_t>(K)];
      if (!sameSsaDef(Def.Output, Wanted))
        continue;
      if (Def.Opcode == NdOp::LOAD)
        return medOpToExpr(Def);
      if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT ||
           Def.Opcode == NdOp::INT_SEXT || Def.Opcode == NdOp::CAST) &&
          Def.NumInputs >= 1) {
        Wanted = Def.Inputs[0];
        continue;
      }
      break;
    }
    return E;
  };
  auto uniqueLoadDef = [&](const MedVar &V) -> const MedOp * {
    if (!CurMed || V.Id < 0)
      return nullptr;
    const MedOp *UniqueLoad = nullptr;
    for (const auto &Blk : CurMed->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::LOAD && sameSsaDef(Op.Output, V)) {
          if (UniqueLoad)
            return nullptr;
          UniqueLoad = &Op;
        }
    return UniqueLoad;
  };
  std::function<ExprPtr(ExprPtr, int)> inlineUniqueLoadVars =
      [&](ExprPtr E, int Depth) -> ExprPtr {
    if (!E || Depth > 8)
      return E;
    if (E->Kind == ExprKind::Var) {
      if (const MedOp *Load = uniqueLoadDef(E->Var))
        return inlineUniqueLoadVars(medOpToExpr(*Load), Depth + 1);
      return E;
    }
    if (E->Kind != ExprKind::BinOp && E->Kind != ExprKind::UnaryOp &&
        E->Kind != ExprKind::Load && E->Kind != ExprKind::Cast &&
        E->Kind != ExprKind::BitCast)
      return E;
    auto Copy = std::make_shared<HighExpr>(*E);
    for (auto &Op : Copy->Operands)
      Op = inlineUniqueLoadVars(Op, Depth + 1);
    return Copy;
  };
  auto attachProducingLoadFromWindows = [&](ExprPtr E) -> ExprPtr {
    ExprPtr Attached =
        attachProducingLoad(E, Ops, static_cast<int>(CallIdx) - 1);
    if (Attached && Attached->Kind == ExprKind::Load)
      return Attached;
    for (const auto &Window : ExtraWindows) {
      if (!Window.Ops)
        continue;
      Attached = attachProducingLoad(E, *Window.Ops, Window.Before);
      if (Attached && Attached->Kind == ExprKind::Load)
        return Attached;
    }
    if (!CurMed || !E)
      return E;
    const HighExpr *Cur = E.get();
    bool PeeledView = false;
    for (int Peel = 0; Peel < 8 && Cur; ++Peel) {
      if (Cur->Kind == ExprKind::Load)
        return E;
      const bool View =
          (Cur->Kind == ExprKind::UnaryOp &&
           (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT ||
            Cur->Op == NdOp::CAST)) ||
          Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast;
      if (!View || Cur->Operands.empty() || !Cur->Operands[0])
        break;
      PeeledView = true;
      Cur = Cur->Operands[0].get();
    }
    if (!PeeledView || !Cur || Cur->Kind != ExprKind::Var)
      return E;
    if (const MedOp *UniqueLoad = uniqueLoadDef(Cur->Var))
      return medOpToExpr(*UniqueLoad);
    return E;
  };
  for (int K = 0; K < MaxArgs; ++K) {
    if (!Found[K])
      continue;
    Found[K] = attachProducingLoadFromWindows(Found[K]);
    if (Found[K] && Found[K]->Kind == ExprKind::Load)
      Found[K] = inlineUniqueLoadVars(Found[K], 0);
  }

  int FirstStackSlot = 0;
  for (int K = 0; K < MaxArgs; ++K) {
    if (Found[K] && Found[K]->Kind != ExprKind::Undef)
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
  Scan.StoreScanWindow = limits::kCallArgStoreScanWindow;
  Scan.ExtraWindows = ExtraWindows;
  auto ToExpr = [this](const MedVar &V) { return medvarToExpr(V); };
  Scan.ToExpr = ToExpr;
  Scan.IsCalleeSave = IsCalleeSave;
  Scan.ResolveWindow = exprFromWindowValue;

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

  if (Win64 && CurMed) {
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

  if (Hinted.empty() && CurMed) {
    if (const MedCallInfo *CI =
            CurMed->findCall(CurBlock.Id, static_cast<int>(CallIdx))) {
      if (!CI->Args.empty()) {
        std::vector<ExprPtr> FromABI;
        FromABI.reserve(CI->Args.size());
        for (const MedVar &A : CI->Args)
          FromABI.push_back(medvarToExpr(A));
        size_t End = FromABI.size();
        while (End < static_cast<size_t>(MaxArgs) && Found[End] &&
               Found[End]->Kind != ExprKind::Undef)
          ++End;
        for (size_t I = FromABI.size(); I < End; ++I)
          FromABI.push_back(Found[I]);
        for (size_t I = 0; I < FromABI.size(); ++I)
          if ((!FromABI[I] || FromABI[I]->Kind == ExprKind::Undef) &&
              Found[I] && Found[I]->Kind != ExprKind::Undef)
            FromABI[I] = Found[I];
        return BoundKnownCalleeArity(std::move(FromABI));
      }
    }
  }

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
      // An IAT hint can be short (`Concatenate` with only dest+a+na). Keep
      // recovered r9 / [rsp+20h] that sit past that prefix.
      size_t End = Hinted.size();
      while (End < static_cast<size_t>(MaxArgs) && Found[End] &&
             Found[End]->Kind != ExprKind::Undef)
        ++End;
      for (size_t I = Hinted.size(); I < End; ++I)
        Hinted.push_back(Found[I]);
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
    if (Blk.Preds.empty()) {
      // Function entry: the identity COPY is the live-in. Do not invent every
      // unused incoming parameter; a later 1-arg call still has rdx live.
      for (const auto &Op : Blk.Ops) {
        if (Op.Opcode != NdOp::COPY)
          break;
        if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
            Op.Output.Size > 0 && Op.NumInputs >= 1 &&
            Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Inputs[0].RegOff == RegOff &&
            Op.Inputs[0].Id == Op.Output.Id &&
            Op.Inputs[0].SSAVer == Op.Output.SSAVer) {
          R = Op.Output;
          return true;
        }
      }
      return false;
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

bool MedToHighConverter::isCallArgSetupDef(const MedBlock &CallBlk,
                                           const MedVar &LiveIn,
                                           uint64_t RegOff) const {
  if (!CurMed)
    return false;
  for (int PredId : CallBlk.Preds) {
    const MedBlock *Pred = nullptr;
    for (const auto &Blk : CurMed->Blocks)
      if (Blk.Id == PredId) {
        Pred = &Blk;
        break;
      }
    if (!Pred)
      continue;
    for (const auto &Phi : Pred->Phis)
      if (Phi.Output.Id == LiveIn.Id && Phi.Output.SSAVer == LiveIn.SSAVer)
        return Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == RegOff &&
               Phi.Output.Size > 0;
    for (const auto &Op : Pred->Ops) {
      if (Op.Output.Id != LiveIn.Id || Op.Output.SSAVer != LiveIn.SSAVer)
        continue;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
          Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[0].RegOff == RegOff &&
          Op.Inputs[0].Id == Op.Output.Id &&
          Op.Inputs[0].SSAVer == Op.Output.SSAVer)
        return false;
      return Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
             Op.Output.Size > 0;
    }
  }
  // `lea rcx` before `ja` / `test; jz` sits in a dominating ancestor, not
  // the immediate pred. Leftover r9 from an earlier Find is not a lea.
  const MedOp *Unique = uniqueSsaDef(CurMed, LiveIn);
  if (!Unique || !isLeaLikeCallSetup(CurMed, *Unique))
    return false;
  if (Unique->Opcode == NdOp::COPY && Unique->NumInputs >= 1 &&
      Unique->Inputs[0].Kind == MedVar::Reg &&
      Unique->Inputs[0].RegOff == RegOff &&
      Unique->Inputs[0].Id == Unique->Output.Id &&
      Unique->Inputs[0].SSAVer == Unique->Output.SSAVer)
    return false;
  return Unique->Output.Kind == MedVar::Reg &&
         Unique->Output.RegOff == RegOff && Unique->Output.Size > 0;
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
