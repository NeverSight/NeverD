//===- PipelinePatchLift.cpp - Patch/lift pipeline path ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MedIR-to-LLVM shortcut used by patch and lift modes.
///
//===----------------------------------------------------------------------===//

#include "PipelineMedAudit.h"
#include "PipelineReturnModelingDetail.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

namespace {

// Variadic overflow parameters are finalized only after call recovery.  Keep
// their interim arity open so neither recovery pass truncates the caller's
// recovered stack tail to the currently known fixed prefix.
int callRecoveryTotalArity(const MedFunc &Func, int MaxParamIndex) {
  return Func.IsVariadic ? limits::kMaxCallArgs : MaxParamIndex + 1;
}

/// Count FP arguments a callee takes from its entry-block live-in self-copies
/// (`COPY D0,D0; COPY D1,D1; ...` before any real body).  Used to prime
/// CalleeFPArity for intra-module callees (e.g. `mkD2`) before recoverCallAbi
/// runs, so a tail-call struct-return forwarder (`fwdD2`) can recover its
/// forwarded d0/d1 live-ins (KnownFPCallee gate in recoverCallAbi).
static int countEntryLiveInFPArgs(const MedFunc &MF, const TargetRegInfo &TRI) {
  if (MF.Blocks.empty())
    return 0;
  int Count = 0;
  for (const auto &O : MF.Blocks.front().Ops) {
    if (O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
        O.Inputs[0].Kind == MedVar::Reg &&
        O.Inputs[0].RegOff == O.Output.RegOff &&
        TRI.isFPArgReg(O.Output.RegOff))
      ++Count;
    else if (O.Opcode == NdOp::COPY && TRI.isLinkRegister(O.Output.RegOff))
      continue;
    else
      break;
  }
  return Count;
}

/// Reach a fixed point for parameters forwarded only through direct calls.
///
/// Optimized wrappers can consume an incoming argument solely by passing the
/// register on to another function.  Their initial parameter scan therefore
/// reports arity zero; recoverCallAbi surfaces the live-in only after the
/// callee's arity is known.  A single global pass is order-dependent for a
/// chain such as `outer -> middle -> leaf`: leaf seeds x0, then middle and
/// outer each need a later visit.  Probe copies let us propagate those arities
/// without repeatedly mutating real CallInfos or inserting call-lane helper
/// ops.  The worklist revisits only direct callers of a function whose
/// signature grew.
static void propagateForwardedCallArities(
    const std::vector<MedFunc> &Funcs, Arch TheArch,
    const std::map<va_t, std::string> &FuncNames, const BinaryImage &Img,
    std::map<va_t, int> &CalleeRegArity, std::map<va_t, int> &CalleeTotalArity,
    std::map<va_t, int> &CalleeFPArity,
    const std::map<va_t, bool> &CalleeReturnsVec,
    std::map<va_t, std::vector<uint64_t>> &CalleeFPRegs,
    const std::map<va_t, bool> &CalleeHasSret,
    const std::map<va_t, bool> &CalleeIsVariadic) {
  if (Funcs.empty())
    return;

  std::map<va_t, std::vector<size_t>> DirectCallers;
  std::set<va_t> Entries;
  for (const auto &MF : Funcs)
    Entries.insert(MF.Entry);
  for (size_t I = 0; I < Funcs.size(); ++I)
    for (const auto &Blk : Funcs[I].Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() && Entries.count(Op.Inputs[0].ConstVal) != 0)
          DirectCallers[Op.Inputs[0].ConstVal].push_back(I);

  std::queue<size_t> Work;
  std::vector<bool> Queued(Funcs.size(), true);
  for (size_t I = 0; I < Funcs.size(); ++I)
    Work.push(I);

  const auto &TRI = getTargetRegInfo(TheArch);
  while (!Work.empty()) {
    const size_t I = Work.front();
    Work.pop();
    Queued[I] = false;

    MedFunc Probe = Funcs[I];
    recoverCallAbi(Probe, TheArch, FuncNames, &Img, &CalleeRegArity,
                   &CalleeTotalArity, &CalleeFPArity, &CalleeReturnsVec,
                   &CalleeFPRegs, &CalleeHasSret, &CalleeIsVariadic);

    int MaxRegIdx = -1;
    int MaxIdx = -1;
    std::vector<uint64_t> FPRegs;
    const uint64_t IRR = TRI.indirectResultReg();
    for (const auto &P : Probe.Params) {
      if (IRR != 0 && P.RegOff == IRR)
        continue;
      if (P.RegOff != kNoParamReg && TRI.isFPArgReg(P.RegOff)) {
        FPRegs.push_back(P.RegOff);
      } else if (P.RegOff != kNoParamReg) {
        const int ArgIdx = TRI.regToArgIdx(P.RegOff);
        MaxRegIdx = std::max(MaxRegIdx, ArgIdx);
        MaxIdx = std::max(MaxIdx, ArgIdx);
      } else if (P.Kind == MedVar::Param) {
        MaxIdx = std::max(MaxIdx, P.Id);
      }
    }
    std::sort(FPRegs.begin(), FPRegs.end());

    const int RegArity = MaxRegIdx + 1;
    const int TotalArity = callRecoveryTotalArity(Probe, MaxIdx);
    const int FPArity = static_cast<int>(FPRegs.size());
    bool Grew = false;
    if (RegArity > CalleeRegArity[Probe.Entry]) {
      CalleeRegArity[Probe.Entry] = RegArity;
      Grew = true;
    }
    if (TotalArity > CalleeTotalArity[Probe.Entry]) {
      CalleeTotalArity[Probe.Entry] = TotalArity;
      Grew = true;
    }
    if (FPArity > CalleeFPArity[Probe.Entry]) {
      CalleeFPArity[Probe.Entry] = FPArity;
      CalleeFPRegs[Probe.Entry] = std::move(FPRegs);
      Grew = true;
    }
    if (!Grew)
      continue;

    auto CallerIt = DirectCallers.find(Probe.Entry);
    if (CallerIt == DirectCallers.end())
      continue;
    for (size_t Caller : CallerIt->second)
      if (!Queued[Caller]) {
        Queued[Caller] = true;
        Work.push(Caller);
      }
  }
}

bool isFatalOptimizationStop(OptimizationStopReason Stop) {
  return Stop == OptimizationStopReason::InputInvalid ||
         Stop == OptimizationStopReason::VerificationFailed;
}

} // namespace

bool Pipeline::requiresSerialLLVMEmission(const std::vector<MedFunc> &Funcs,
                                          const BinaryImage &Img) {
  if (Img.CodePtrRelocSlots.empty() || Img.getPointerSize() == 0)
    return false;
  std::set<va_t> FunctionEntries;
  for (const MedFunc &Func : Funcs)
    FunctionEntries.insert(Func.Entry);
  for (va_t Slot : Img.CodePtrRelocSlots) {
    const uint8_t *P = Img.readVA(Slot, Img.getPointerSize());
    if (!P)
      continue;
    const va_t Target = normalizeCodeAddress(
        static_cast<va_t>(readPtr(P, Img.is64Bit())), Img.Arch, Img.Mode);
    if (!FunctionEntries.count(Target))
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// runPatchLiftMode — MedIR -> LLVM IR shortcut
//===----------------------------------------------------------------------===//

bool Pipeline::runPatchLiftMode(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                                const PipelineOptions &Opts,
                                PipelineResult &Result) {
  [[maybe_unused]] const char *ModeName = Opts.PatchMode ? "patch" : "lift";
  LLVM_DEBUG(llvm::dbgs() << "pipeline: " << ModeName
                          << " mode -- MedIR -> LLVM IR, skipping HighIR\n");

  auto AllFuncNames = buildFuncNameMap(Img, Result);

  // Each callee's integer register-argument count and total integer-argument
  // count (register + stack), so a forwarder's call ABI is bounded by the arity
  // its target actually takes — the register count gates passed-through
  // register arguments, the total count gates passed-through stack arguments of
  // a tail call (see recoverCallAbi). Infer types for every function first so
  // each caller's call-ABI recovery can consult its callees' return class
  // (FP-in-vector-register vs integer) and FP-argument count — both derived
  // from the callee's inferred signature.
  for (auto &MF : Result.MedFuncs)
    inferMedTypes(MF, Img.Arch);

  // A callee that merely forwards an indirect scalar-FP call has no explicit
  // post-call D0/V0 read: the native RET returns whatever BLR left in V0, so
  // its own body alone looks like an integer-returning function.  A direct
  // caller provides the missing ABI evidence when LowToMed routes that call's
  // result through V0 and a scalar S0/D0 view is consumed downstream.  Feed
  // that observed width back into the callee before call-ABI recovery; the
  // forwarder's final indirect call can then be rewired from X0 to V0.
  if (Img.Arch == Arch::AArch64) {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    std::map<va_t, MedFunc *> ByEntry;
    for (auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    for (const auto &Caller : Result.MedFuncs) {
      for (const auto &Blk : Caller.Blocks) {
        for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
          const MedOp &Call = Blk.Ops[OI];
          if (Call.Opcode != NdOp::CALL || Call.NumInputs < 1 ||
              !Call.Inputs[0].isConst() || Call.Output.Kind != MedVar::Reg ||
              Call.Output.RegOff != TRI.FPReturnReg)
            continue;
          auto CalleeIt = ByEntry.find(Call.Inputs[0].ConstVal);
          if (CalleeIt == ByEntry.end())
            continue;
          uint16_t ScalarSize = 0;
          for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
            const MedOp &Use = Blk.Ops[J];
            for (uint8_t I = 0; I < Use.NumInputs; ++I) {
              const MedVar &V = Use.Inputs[I];
              if (V.Kind != MedVar::Reg || V.RegOff != Call.Output.RegOff ||
                  V.Id != Call.Output.Id || V.SSAVer != Call.Output.SSAVer ||
                  (V.Size != 4 && V.Size != 8))
                continue;
              if (ScalarSize == 0 || V.Size < ScalarSize)
                ScalarSize = V.Size;
            }
            if (Use.Output.Kind == MedVar::Reg &&
                Use.Output.RegOff == Call.Output.RegOff &&
                Use.Output.SSAVer != Call.Output.SSAVer)
              break;
          }
          if (ScalarSize != 0)
            CalleeIt->second->ReturnType = NdType::makeFloat(ScalarSize);
        }
      }
    }
  }

  modelWideIntReturns(Img, Result);

  recoverStructReturnFromCallers(Img, Result);

  propagateStructReturnForwarderShapes(Img, Result);

  recoverStructReturnFromBody(Img, Result);

  materializeKnownStructReturnCallSites(Img, Result);

  std::map<va_t, int> CalleeRegArity;
  std::map<va_t, int> CalleeTotalArity;
  std::map<va_t, int> CalleeFPArity;
  std::map<va_t, std::vector<uint64_t>> CalleeFPRegs;
  std::map<va_t, bool> CalleeReturnsVec;
  std::map<va_t, bool> CalleeHasSret;
  std::map<va_t, bool> CalleeIsVariadic;
  {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    // On x86-64 a floating-point return lands in XMM0 (a vector register); on
    // ARM/AArch64 the lifter models it in the integer return register, so only
    // the former needs the call result routed to the FP return register.
    const bool FPRetInVecReg = TRI.isVectorReg(TRI.fpReturnModelReg());
    for (const auto &MF : Result.MedFuncs) {
      int MaxRegIdx = -1, MaxIdx = -1;
      // The exact FP-argument register offsets, in ABI order.  ARM `float` args
      // land in the single-width S registers (s0,s1,..) and `double` args in
      // the D registers (d0,d1,..); recording the layout lets the caller
      // recover FP arguments at the registers the callee actually reads (s1 !=
      // d1).
      std::vector<uint64_t> FPRegs;
      const uint64_t IRR = TRI.indirectResultReg();
      bool HasSret = false;
      for (const auto &P : MF.Params) {
        if (IRR != 0 && P.RegOff == IRR) {
          // Hidden indirect-result (sret) pointer (AArch64 x8): not an ordinary
          // integer/FP/stack argument; recorded separately.
          HasSret = true;
        } else if (P.RegOff != kNoParamReg && TRI.isFPArgReg(P.RegOff)) {
          // Floating-point/vector argument register: counted separately.
          FPRegs.push_back(P.RegOff);
        } else if (P.RegOff != kNoParamReg) {
          if (int Idx = TRI.regToArgIdx(P.RegOff); Idx > MaxRegIdx)
            MaxRegIdx = Idx;
          MaxIdx = std::max(MaxIdx, TRI.regToArgIdx(P.RegOff));
        } else if (P.Kind == MedVar::Param) {
          // Stack parameter (detectStackParams / detectCdeclStackParams): its
          // Id is the argument index.
          MaxIdx = std::max(MaxIdx, P.Id);
        }
      }
      std::sort(FPRegs.begin(), FPRegs.end());
      if (MF.IsVariadic && Img.Arch == Arch::AArch64 &&
          Img.Format == BinaryFormat::MachO &&
          MF.VariadicFixedRegArgs > 0)
        MaxRegIdx = MF.VariadicFixedRegArgs - 1;
      CalleeRegArity[MF.Entry] = MaxRegIdx + 1;
      CalleeTotalArity[MF.Entry] = callRecoveryTotalArity(MF, MaxIdx);
      CalleeHasSret[MF.Entry] = HasSret;
      CalleeIsVariadic[MF.Entry] = MF.IsVariadic;
      int FpArity = static_cast<int>(FPRegs.size());
      // Params are not recovered yet (recoverCallAbi runs later), so fall back
      // to entry live-in self-copies for intra-module FP callees like `mkD2`.
      // Without this, tail-call struct-return forwarders (`fwdD2`) miss the
      // KnownFPCallee gate and pass 0.0 for forwarded d0/d1.
      if (FpArity == 0) {
        FpArity = countEntryLiveInFPArgs(MF, TRI);
        if (FpArity > 0)
          FPRegs.assign(TRI.FPParamRegs.begin(),
                        TRI.FPParamRegs.begin() + FpArity);
      }
      CalleeFPArity[MF.Entry] = FpArity;
      CalleeFPRegs[MF.Entry] = std::move(FPRegs);
      CalleeReturnsVec[MF.Entry] = FPRetInVecReg && MF.ReturnType &&
                                   MF.ReturnType->Kind == NdTypeKind::Float;
    }
  }

  // A pure tail-call forwarder `T f(args){return g(args);}` lowers at -O2 to a
  // lone `b g`.  It carries its incoming arguments straight into the call, so
  // it has no parameters of its own here and the per-function FP arity above is
  // 0. Inherit the callee g's scalar-FP return type and FP argument arity for
  // such a forwarder so (a) the function's return type is floating-point
  // (recoverCallAbi rewires the tail call's result register to the FP return
  // register) and (b) a CALLER of it recovers the forwarded FP arguments
  // instead of padding 0.0.  The callee g is a libc import (signature from
  // libcArity) or an intra-module function (signature from its recovered
  // MedFunc).  Gated to a genuine forwarder whose FP argument registers are
  // live-in (never written), so a function that locally computes the callee's
  // FP arguments is left untouched.
  {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    std::map<va_t, const MedFunc *> ByEntry;
    for (const auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    if (!TRI.FPParamRegs.empty())
      for (auto &MF : Result.MedFuncs) {
        // Only a pure forwarder, which has no parameters of its own recovered
        // yet (its incoming arguments flow straight into the tail call).
        if (!MF.Params.empty())
          continue;
        const MedOp *CallOp = nullptr;
        for (const auto &Blk : MF.Blocks) {
          for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
            const auto &Op = Blk.Ops[I];
            if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
                !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg)
              continue;
            const auto &Ret = Blk.Ops[I + 1];
            if (Ret.Opcode == NdOp::RETURN && Ret.NumInputs >= 1 &&
                Ret.Inputs[0].Kind == MedVar::Reg &&
                Ret.Inputs[0].RegOff == Op.Output.RegOff) {
              CallOp = &Op;
              break;
            }
          }
          if (CallOp)
            break;
        }
        if (!CallOp)
          continue;
        va_t Target = CallOp->Inputs[0].ConstVal;
        // Resolve the callee's integer + FP argument counts and scalar-FP
        // return width (libc import via libcArity, intra-module via its
        // recovered MedFunc).  All three must reach the forwarder's signature,
        // or a caller misassembles the forwarded arguments (a mixed `double
        // f(int n,double x) {return g(n,x);}` with only the FP arity propagated
        // would disagree with its own declared int+FP signature).
        int IntArgs = 0, FpArgs = 0;
        uint16_t FpRetSize = 0; // 0 = callee does not return a scalar FP value
        if (const Import *Imp = Img.findImportAt(Target)) {
          if (auto Sig = libc::libcArityForSymbol(Imp->Name)) {
            if (!Sig->FpRetComplex) {
              IntArgs = Sig->IntArgs;
              FpArgs = Sig->FpArgs;
              bool ScalarFPRet = (Sig->FpArgs > 0 && Sig->IntArgs == 0) ||
                                 Sig->FpRet || Sig->FpRetLongDouble;
              if (ScalarFPRet)
                FpRetSize = Sig->FpIsFloat ? 4 : 8;
            }
          }
        } else if (auto It = ByEntry.find(Target); It != ByEntry.end()) {
          const MedFunc *G = It->second;
          IntArgs = CalleeRegArity[Target]; // g's integer-argument count
          FpArgs = CalleeFPArity[Target];   // g's FP-argument count
          if (FpArgs == 0) {
            FpArgs = countEntryLiveInFPArgs(*G, TRI);
            if (FpArgs > 0) {
              CalleeFPArity[Target] = FpArgs;
              CalleeFPRegs[Target] = std::vector<uint64_t>(
                  TRI.FPParamRegs.begin(), TRI.FPParamRegs.begin() + FpArgs);
            }
          }
          if (G->ReturnType && G->ReturnType->Kind == NdTypeKind::Float &&
              G->MultiReturn.empty())
            FpRetSize = G->ReturnType->Size ? G->ReturnType->Size : 8;
        }
        if (IntArgs <= 0 && FpArgs <= 0 && FpRetSize == 0)
          continue; // nothing to inherit
        int NFp =
            std::min<int>(FpArgs, static_cast<int>(TRI.FPParamRegs.size()));
        int NInt =
            std::min<int>(IntArgs, static_cast<int>(TRI.IntParamRegs.size()));
        // Every forwarded argument register must be live-in (genuine forwarder,
        // not a function that computes the callee's arguments locally).  The
        // forwarding CALL itself is excluded: its output is the integer return
        // register, which on AArch64/x86-64 aliases the first integer argument
        // register (x0 / rax-vs-rdi differ, but x0==arg0==ret on AArch64), so
        // the call's result write must not be mistaken for an argument write.
        auto regWrittenInF = [&](uint64_t Reg) {
          for (const auto &B : MF.Blocks) {
            for (const auto &O : B.Ops)
              if (&O != CallOp && O.Output.Kind == MedVar::Reg &&
                  O.Output.Size > 0 && O.Output.RegOff == Reg)
                return true;
            for (const auto &Ph : B.Phis)
              if (Ph.Output.Kind == MedVar::Reg && Ph.Output.RegOff == Reg)
                return true;
          }
          return false;
        };
        bool AllLiveIn = true;
        for (int K = 0; K < NFp && AllLiveIn; ++K)
          if (regWrittenInF(TRI.FPParamRegs[K]))
            AllLiveIn = false;
        for (int K = 0; K < NInt && AllLiveIn; ++K)
          if (regWrittenInF(TRI.IntParamRegs[K]))
            AllLiveIn = false;
        if (!AllLiveIn)
          continue;
        if (NFp > 0) {
          CalleeFPArity[MF.Entry] = NFp;
          CalleeFPRegs[MF.Entry] = std::vector<uint64_t>(
              TRI.FPParamRegs.begin(), TRI.FPParamRegs.begin() + NFp);
        }
        if (NInt > 0) {
          CalleeRegArity[MF.Entry] = NInt;
          if (CalleeTotalArity[MF.Entry] < NInt)
            CalleeTotalArity[MF.Entry] = NInt;
        }
        // Inherit a scalar FP return so the function and its callers treat the
        // result as floating-point; recoverCallAbi rewires the tail call's
        // result register to the FP return register.
        if (FpRetSize) {
          MF.ReturnType = NdType::makeFloat(FpRetSize);
          CalleeReturnsVec[MF.Entry] = TRI.isVectorReg(TRI.fpReturnModelReg());
        }
      }
  }

  propagateForwardedCallArities(Result.MedFuncs, Img.Arch, AllFuncNames, Img,
                                CalleeRegArity, CalleeTotalArity, CalleeFPArity,
                                CalleeReturnsVec, CalleeFPRegs, CalleeHasSret,
                                CalleeIsVariadic);

  for (auto &MF : Result.MedFuncs)
    recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &CalleeRegArity,
                   &CalleeTotalArity, &CalleeFPArity, &CalleeReturnsVec,
                   &CalleeFPRegs, &CalleeHasSret, &CalleeIsVariadic);

  remodelStructReturnForwarderCalls(Img, Result);

  // i386 two-pass call recovery: the first pass promotes forwarder register
  // params (PromoteParams).  Recompute CalleeRegArity from the now-promoted
  // params and re-run: forwarders now have CalleeRegArgs > 0, so the cdecl
  // clearing (CalleeRegArgs == 0) no longer fires for them, while true cdecl
  // callees remain at 0 and get their stack arguments correctly indexed.
  if (Img.Arch == Arch::X86) {
    const auto &TRI2 = getTargetRegInfo(Img.Arch);
    std::map<va_t, int> CRA2, CTA2;
    for (const auto &MF : Result.MedFuncs) {
      int MaxRI = -1, MaxI = -1;
      for (const auto &P : MF.Params) {
        if (P.RegOff != kNoParamReg && TRI2.regToArgIdx(P.RegOff) >= 0) {
          MaxRI = std::max(MaxRI, TRI2.regToArgIdx(P.RegOff));
          MaxI = std::max(MaxI, TRI2.regToArgIdx(P.RegOff));
        } else if (P.Kind == MedVar::Param)
          MaxI = std::max(MaxI, P.Id);
      }
      CRA2[MF.Entry] = MaxRI + 1;
      CTA2[MF.Entry] = callRecoveryTotalArity(MF, MaxI);
    }
    for (auto &MF : Result.MedFuncs)
      recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &CRA2, &CTA2,
                     &CalleeFPArity, &CalleeReturnsVec, &CalleeFPRegs,
                     &CalleeHasSret, &CalleeIsVariadic);
  }

  // Variadic callees: size each one's overflow stack-parameter list from its
  // now recovered call sites, append the trailing stack parameters, and pad
  // every call to that arity.  The emitter spills these into the frame headroom
  // so the unchanged va_arg walk reads the caller's overflow arguments.
  finalizeVariadicCallees(Result.MedFuncs, Img.Arch, Img.Format);
  // Late ABI remodelling may reconvert a function from LowIR.  Refresh the
  // interprocedural facts before either serial or sharded LLVM emission.
  propagateInternalNoReturn(Result.MedFuncs, Img.Arch);

  if (!recordMedIRVerification(Result, "pipeline-backend-input")) {
    Result.Error = "MedIR verification failed before backend emission";
    return false;
  }

  std::vector<std::pair<va_t, std::string>> ImportMap;
  for (const auto &[Addr, Name] : Img.getImportAddressNames())
    ImportMap.emplace_back(Addr, Name);

  // Parallel emit + optimize (the two single-threaded phases that dominate
  // lift).  Only for lift mode — the patch backend depends on the serial path's
  // single-module, internal-linkage globals.  Worker count comes from the
  // shared pool setting, so NEVERD_THREADS / setWorkerThreadCount() throttle
  // this phase too: it is by far the most memory-hungry one, and capping it was
  // previously impossible (it read hardware_concurrency() directly).  Small
  // inputs stay serial: below 8 functions the shard
  // emit/verify/optimize/link setup outweighs the memory and parallelism gains.
  // A large input still takes this path with one worker: the work-budgeted
  // shards then run serially, which is what makes NEVERD_THREADS=1 actually
  // bound the transient LLVM emission working set.
  unsigned Workers = std::max(
      1u, std::min<unsigned>(workerThreadCount(),
                             static_cast<unsigned>(Result.MedFuncs.size())));
  bool UseShards = !Opts.PatchMode && Result.MedFuncs.size() >= 8 &&
                   !requiresSerialLLVMEmission(Result.MedFuncs, Img);

  if (UseShards) {
    bool LLVMVerifierFailed = false;
    Result.LlvmModule = emitLLVMSharded(
        Result.MedFuncs, Ctx, Img.Arch, ImportMap, Img, Img.Format, Opts.NoOpt,
        Workers, Result.BackendUnhandledValueIntrinsics, LLVMVerifierFailed);
    if (!Result.LlvmModule) {
      Result.LLVMVerifierFailed = LLVMVerifierFailed;
      Result.Error = LLVMVerifierFailed
                         ? "LLVM shard verification or optimization failed"
                         : "LLVM shard emission or linking failed";
      return false;
    }
  } else {
    MedLLVMEmitter MedEmitter;
    Result.LlvmModule = MedEmitter.emit(Result.MedFuncs, Ctx, "neverd_output",
                                        Img.Arch, ImportMap, &Img, Img.Format);
    Result.BackendUnhandledValueIntrinsics =
        MedEmitter.unhandledValueIntrinsicCount();

    if (!Result.LlvmModule)
      return false;

    std::string VerifyErr;
    llvm::raw_string_ostream VES(VerifyErr);
    if (llvm::verifyModule(*Result.LlvmModule, &VES)) {
      Result.LLVMVerifierFailed = true;
      Result.Error =
          "LLVM verification failed before optimization: " + VerifyErr;
      llvm::WithColor::warning()
          << "pipeline: LLVM verification failed before optimization: "
          << VerifyErr << "\n";
      return false;
    }
    if (!Opts.NoOpt) {
      OptimizationOptions Options;
      Options.Conservative = Opts.PatchMode;
      OptimizationResult Optimization =
          optimizeModule(*Result.LlvmModule, Options);
      if (isFatalOptimizationStop(Optimization.Stop)) {
        Result.LLVMVerifierFailed = true;
        Result.Error = std::string("LLVM optimization failed: ") +
                       optimizationStopReasonName(Optimization.Stop);
        return false;
      }
    } else {
      // Even with the NeverD optimizer disabled, promote the emitter's
      // memory-SSA scaffolding to registers.  This is semantics-preserving
      // canonicalization (not optimization): it strips the per-temp load/store
      // bloat so a heavily-unrolled -O2 SSE kernel does not lift to an
      // ~80K-instruction single block that is pathological for LLVM codegen and
      // times out under parallel test load.
      promoteScaffoldingAllocas(*Result.LlvmModule);
    }
  }

  Result.LLVMDefinitionNames.clear();
  for (const auto &Function : *Result.LlvmModule)
    if (!Function.isDeclaration())
      Result.LLVMDefinitionNames.push_back(Function.getName().str());
  std::sort(Result.LLVMDefinitionNames.begin(),
            Result.LLVMDefinitionNames.end());

  for (auto &Audit : Result.FunctionAudits) {
    if (!Audit.HasMedIR)
      continue;
    Audit.HasLLVMDefinition =
        std::binary_search(Result.LLVMDefinitionNames.begin(),
                           Result.LLVMDefinitionNames.end(), Audit.Name);
  }

  std::string FinalVerifyError;
  llvm::raw_string_ostream FinalVerifyStream(FinalVerifyError);
  Result.LLVMVerifierFailed =
      llvm::verifyModule(*Result.LlvmModule, &FinalVerifyStream);
  if (Result.LLVMVerifierFailed) {
    llvm::WithColor::warning()
        << "pipeline: final LLVM verification failed: " << FinalVerifyError
        << "\n";
    return false;
  }

  return true;
}

} // namespace neverd
