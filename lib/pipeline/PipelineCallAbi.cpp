//===- PipelineCallAbi.cpp - Module call-ABI recovery --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovery of every call's arguments and every forwarder's passed-through
/// parameters from the setup each caller writes, bounded by its callee's
/// recovered signature (recoverCallAbi).  The LLVM route uses it for every
/// convention, and the HighIR route for each convention that has no callee
/// summaries (CallArgumentConvention::ArgumentsFromCallSetup).
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

namespace {

/// A selected function still calls the image's other native functions. Use
/// the normal parameter detector for their bodies instead of assigning the
/// default register order merely because the user omitted their output.
/// These bounded support bodies never become emitted definitions.
std::vector<MedFunc>
omittedDirectCalleeSignatures(const BinaryImage &Img,
                              const PipelineResult &Result) {
  const auto *Convention = callArgumentConvention(Img.Arch, Img.abiFormat());
  if (!Convention || !Convention->ArgumentsFromCallSetup || Img.IsRelocatable)
    return {};
  std::set<va_t> Present, Pending, Entries;
  for (const auto &Symbol : Img.Symbols)
    if (Symbol.IsFunc && !Symbol.IsBoundaryGuess)
      Entries.insert(Symbol.Addr);
  for (const MedFunc &F : Result.MedFuncs)
    Present.insert(F.Entry);
  auto enqueueCallees = [&](const MedFunc &F) {
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst()) {
          const va_t Target = Op.Inputs[0].ConstVal;
          if (!Present.count(Target) && !Img.findImportAt(Target) &&
              !Img.isImportStubAt(Target) &&
              (Entries.count(Target) || Img.hasKnownFunctionEntryAt(Target)) &&
              Img.hasExecutableCodeOwnerAt(Target))
            Pending.insert(Target);
        }
  };
  for (const MedFunc &F : Result.MedFuncs)
    enqueueCallees(F);
  std::vector<MedFunc> Signatures;
  if (Pending.empty())
    return Signatures;
  Decoder Dec;
  if (!Dec.init(Img))
    return Signatures;
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  size_t Inspected = 0;
  size_t Operations = 0;
  while (!Pending.empty() && Inspected < limits::kMaxCallEffectExtraLifts) {
    ++Inspected;
    const va_t Target = *Pending.begin();
    Pending.erase(Pending.begin());
    Present.insert(Target);
    LowFunc Low =
        Builder.build(Img, Dec, Target, Img.getFunctionNameAt(Target));
    if (!Low.hasCompleteLiftCoverage() ||
        !Low.UnsafeIndirectBranchAddresses.empty())
      continue;
    for (const auto &Block : Low.Blocks) {
      if (Block.Ops.size() > limits::kMaxSSAFunctionOps - Operations)
        return Signatures;
      Operations += Block.Ops.size();
    }
    try {
      LowToMedConverter Converter;
      Converter.setBinaryImage(&Img);
      Converter.setSourceCallHintsEnabled(false);
      Converter.setCallMayWriteGPRs(&Result.CallMayWriteGPRs);
      Converter.setCallEntryReadGPRs(&Result.CallEntryReadGPRs);
      Converter.setCallEntryStackArgs(&Result.CallEntryStackArgs);
      MedFunc Med = Converter.convert(Low, Img.Arch, Img.abiFormat());
      size_t MedOperations = 0;
      for (const auto &Block : Med.Blocks)
        MedOperations += Block.Ops.size();
      if (Med.Blocks.size() > limits::kMaxStructurableMedBlocks ||
          MedOperations > static_cast<size_t>(limits::kMaxSSANodes))
        continue;
      inferMedTypes(Med, Img.Arch);
      enqueueCallees(Med);
      Signatures.push_back(std::move(Med));
    } catch (const LowToMedConversionError &) {
      // An unsupported callee's body supplies no signature evidence.
    }
  }
  return Signatures;
}

// Variadic overflow parameters are finalized only after call recovery.  Keep
// their interim arity open so neither recovery pass truncates the caller's
// recovered stack tail to the currently known fixed prefix.
int callRecoveryTotalArity(const MedFunc &Func, int MaxParamIndex) {
  return Func.IsVariadic ? limits::kMaxCallArgs : MaxParamIndex + 1;
}

/// The integer register arguments a call to \p Func passes through: its
/// register parameters through \p MaxRegisterIndex, but only the named ones
/// of a variadic function whose save area spills the rest.
int callRegisterArity(const MedFunc &Func, int MaxRegisterIndex) {
  const int Arity = MaxRegisterIndex + 1;
  return Func.IsVariadic && Func.VariadicFirstRegister >= 0
             ? std::min(Arity, Func.VariadicFirstRegister)
             : Arity;
}

/// Count FP arguments a callee takes from its entry-block live-in self-copies
/// (`COPY D0,D0; COPY D1,D1; ...` before any real body).  Used to prime
/// CalleeFPArity for intra-module callees (e.g. `mkD2`) before recoverCallAbi
/// runs, so a tail-call struct-return forwarder (`fwdD2`) can recover its
/// forwarded d0/d1 live-ins (KnownFPCallee gate in recoverCallAbi).
int countEntryLiveInFPArgs(const MedFunc &MF, const TargetRegInfo &TRI) {
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
void propagateForwardedCallArities(
    const std::vector<MedFunc *> &Funcs, Arch TheArch,
    const std::map<va_t, std::string> &FuncNames, const BinaryImage &Img,
    std::map<va_t, int> &CalleeRegArity, std::map<va_t, int> &CalleeTotalArity,
    std::map<va_t, int> &CalleeFPArity,
    const std::map<va_t, uint16_t> &CalleeFPReturnSize,
    std::map<va_t, std::vector<uint64_t>> &CalleeFPRegs,
    const std::map<va_t, bool> &CalleeHasSret,
    std::map<va_t, bool> &CalleeIsVariadic,
    const std::map<va_t, bool> &CalleeConsumesVaList,
    std::map<va_t, std::vector<uint64_t>> &CalleeIntRegs) {
  if (Funcs.empty())
    return;

  std::map<va_t, std::vector<size_t>> DirectCallers;
  std::set<va_t> Entries;
  for (const MedFunc *MF : Funcs)
    Entries.insert(MF->Entry);
  for (size_t I = 0; I < Funcs.size(); ++I)
    for (const auto &Blk : Funcs[I]->Blocks)
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

    const int PreviousRegArity = CalleeRegArity[Funcs[I]->Entry];
    const int PreviousTotalArity = CalleeTotalArity[Funcs[I]->Entry];
    const bool PreviousVariadic =
        CalleeIsVariadic.count(Funcs[I]->Entry) != 0 &&
        CalleeIsVariadic.at(Funcs[I]->Entry);
    MedFunc Probe = *Funcs[I];
    recoverCallAbi(Probe, TheArch, FuncNames, &Img, &CalleeRegArity,
                   &CalleeTotalArity, &CalleeFPArity, &CalleeFPReturnSize,
                   &CalleeFPRegs, &CalleeHasSret, &CalleeIsVariadic,
                   &CalleeConsumesVaList, /*FrameLocalLeafCallees=*/nullptr,
                   &CalleeIntRegs);
    int MaxRegIdx = -1;
    int MaxIdx = -1;
    std::vector<uint64_t> FPRegs;
    const uint64_t IRR = TRI.indirectResultReg();
    const IntegerArgumentLayout ProbeLayout =
        integerArgumentLayoutOf(Probe, TRI);
    for (const auto &P : Probe.Params) {
      if (IRR != 0 && P.RegOff == IRR)
        continue;
      if (P.RegOff != kNoParamReg && TRI.isFPArgReg(P.RegOff)) {
        FPRegs.push_back(P.RegOff);
      } else if (P.RegOff != kNoParamReg) {
        const int ArgIdx = ProbeLayout.registerIndex(P.RegOff);
        MaxRegIdx = std::max(MaxRegIdx, ArgIdx);
        MaxIdx = std::max(MaxIdx, ArgIdx);
      } else if (P.Kind == MedVar::Param) {
        MaxIdx = std::max(MaxIdx, P.Id);
      }
    }
    std::sort(FPRegs.begin(), FPRegs.end());

    const int RegArity = callRegisterArity(Probe, MaxRegIdx);
    const bool IsVariadicPublic =
        Probe.IsVariadic || (CalleeIsVariadic.count(Probe.Entry) != 0 &&
                             CalleeIsVariadic.at(Probe.Entry));
    const int TotalArity =
        IsVariadicPublic ? MaxIdx + 1 : callRecoveryTotalArity(Probe, MaxIdx);
    const int FPArity = static_cast<int>(FPRegs.size());
    if (IsVariadicPublic)
      CalleeIsVariadic[Probe.Entry] = true;
    bool Grew = CalleeRegArity[Probe.Entry] > PreviousRegArity ||
                CalleeTotalArity[Probe.Entry] > PreviousTotalArity ||
                (CalleeIsVariadic.count(Probe.Entry) != 0 &&
                 CalleeIsVariadic.at(Probe.Entry) != PreviousVariadic);
    // A forwarder takes its parameters in the order of the function it passes
    // them to, and its callers pass them so.
    if (!Probe.IntegerArgumentRegisters.empty() &&
        CalleeIntRegs[Probe.Entry] != Probe.IntegerArgumentRegisters) {
      CalleeIntRegs[Probe.Entry] = Probe.IntegerArgumentRegisters;
      Grew = true;
    }
    if (RegArity > CalleeRegArity[Probe.Entry]) {
      CalleeRegArity[Probe.Entry] = RegArity;
      Grew = true;
    }
    if (!IsVariadicPublic) {
      if (TotalArity > CalleeTotalArity[Probe.Entry]) {
        CalleeTotalArity[Probe.Entry] = TotalArity;
        Grew = true;
      }
      if (FPArity > CalleeFPArity[Probe.Entry]) {
        CalleeFPArity[Probe.Entry] = FPArity;
        CalleeFPRegs[Probe.Entry] = std::move(FPRegs);
        Grew = true;
      }
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

} // namespace

void recoverModuleCallAbi(const BinaryImage &Img, PipelineResult &Result,
                          const std::map<va_t, std::string> &AllFuncNames) {
  const CallArgumentConvention *Convention =
      callArgumentConvention(Img.Arch, Img.abiFormat());
  std::map<va_t, int> CalleeRegArity;
  std::map<va_t, int> CalleeTotalArity;
  std::map<va_t, int> CalleeFPArity;
  std::map<va_t, std::vector<uint64_t>> CalleeFPRegs;
  std::map<va_t, uint16_t> CalleeFPReturnSize;
  std::map<va_t, bool> CalleeHasSret;
  std::map<va_t, bool> CalleeIsVariadic;
  std::map<va_t, bool> CalleeConsumesVaList;
  // The integer argument registers of each function that takes them in its
  // own order (MedFunc::IntegerArgumentRegisters).
  std::map<va_t, std::vector<uint64_t>> CalleeIntRegs;
  auto OmittedCallees = omittedDirectCalleeSignatures(Img, Result);
  std::vector<MedFunc *> SignatureBodies;
  for (MedFunc &F : Result.MedFuncs)
    SignatureBodies.push_back(&F);
  for (MedFunc &F : OmittedCallees)
    SignatureBodies.push_back(&F);
  {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    // Internal x86/x86-64 scalar float/double, AArch64, and ARM hard-float
    // calls return through XMM0/V0/D0.  External i386 cdecl calls are excluded
    // by recoverCallAbi's relocation/import gates; x86 long double uses x87.
    auto modelsScalarFPReturnInVectorReg = [&](uint16_t Size) {
      if (TRI.WideFloatsReturnInX87 && Size > 8)
        return false;
      return TRI.isVectorReg(TRI.fpReturnModelReg());
    };
    for (const MedFunc *Body : SignatureBodies) {
      const MedFunc &MF = *Body;
      int MaxRegIdx = -1, MaxIdx = -1;
      const IntegerArgumentLayout IntegerLayout =
          integerArgumentLayoutOf(MF, TRI);
      if (!MF.IntegerArgumentRegisters.empty())
        CalleeIntRegs[MF.Entry] = MF.IntegerArgumentRegisters;
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
          if (int Idx = IntegerLayout.registerIndex(P.RegOff); Idx > MaxRegIdx)
            MaxRegIdx = Idx;
          MaxIdx = std::max(MaxIdx, IntegerLayout.registerIndex(P.RegOff));
        } else if (P.Kind == MedVar::Param) {
          // Stack parameter (detectStackParams / detectCdeclStackParams): its
          // Id is the argument index.
          MaxIdx = std::max(MaxIdx, P.Id);
        }
      }
      bool ConsumesVaList = false;
      for (const auto &Blk : MF.Blocks)
        for (const auto &Op : Blk.Ops)
          if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
              Op.Inputs[0].isConst()) {
            if (const Import *Imp = Img.findImportAt(Op.Inputs[0].ConstVal)) {
              if (libc::isVaListConsumer(stripLeadingUnderscores(Imp->Name)))
                ConsumesVaList = true;
            }
          }
      CalleeConsumesVaList[MF.Entry] = ConsumesVaList;
      std::sort(FPRegs.begin(), FPRegs.end());
      CalleeRegArity[MF.Entry] = callRegisterArity(MF, MaxRegIdx);
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
      const uint16_t ReturnSize =
          MF.ReturnType && MF.ReturnType->Size ? MF.ReturnType->Size : 8;
      const bool ReturnsScalarFP =
          MF.ReturnType && MF.ReturnType->Kind == NdTypeKind::Float &&
          MF.MultiReturn.empty() && !MF.FPReturnViaX87 &&
          modelsScalarFPReturnInVectorReg(ReturnSize);
      CalleeFPReturnSize[MF.Entry] = ReturnsScalarFP ? ReturnSize : 0;
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
    for (const MedFunc *MF : SignatureBodies)
      ByEntry[MF->Entry] = MF;
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
              G->MultiReturn.empty() && !G->FPReturnViaX87)
            FpRetSize = G->ReturnType->Size ? G->ReturnType->Size : 8;
        }
        if (IntArgs <= 0 && FpArgs <= 0 && FpRetSize == 0)
          continue; // nothing to inherit
        int NFp =
            std::min<int>(FpArgs, static_cast<int>(TRI.FPParamRegs.size()));
        const auto IntegerRegs =
            TRI.integerArgumentLayout(MF.CC == CallingConv::Win64).Registers;
        int NInt = std::min<int>(IntArgs, static_cast<int>(IntegerRegs.size()));
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
          if (regWrittenInF(IntegerRegs[K]))
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
        if (FpRetSize && !MF.FPReturnViaX87) {
          MF.ReturnType = NdType::makeFloat(FpRetSize);
          CalleeFPReturnSize[MF.Entry] =
              !(TRI.WideFloatsReturnInX87 && FpRetSize > 8) &&
                      TRI.isVectorReg(TRI.fpReturnModelReg())
                  ? FpRetSize
                  : 0;
        }
      }
  }

  propagateForwardedCallArities(
      SignatureBodies, Img.Arch, AllFuncNames, Img, CalleeRegArity,
      CalleeTotalArity, CalleeFPArity, CalleeFPReturnSize, CalleeFPRegs,
      CalleeHasSret, CalleeIsVariadic, CalleeConsumesVaList, CalleeIntRegs);

  const auto FrameLocalLeafCallees =
      Convention && Convention->TargetSpillsSurviveLeafCalls
          ? findFrameLocalLeafCallees(Result.MedFuncs, Img.Arch)
          : std::set<va_t>{};
  for (MedFunc *Body : SignatureBodies) {
    MedFunc &MF = *Body;
    recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &CalleeRegArity,
                   &CalleeTotalArity, &CalleeFPArity, &CalleeFPReturnSize,
                   &CalleeFPRegs, &CalleeHasSret, &CalleeIsVariadic,
                   &CalleeConsumesVaList, &FrameLocalLeafCallees,
                   &CalleeIntRegs);
  }

  // Regparm two-pass call recovery (i386): the first pass promotes forwarder
  // register params (PromoteParams).  Recompute CalleeRegArity from the
  // now-promoted params and re-run: forwarders now have CalleeRegArgs > 0, so
  // the cdecl clearing (CalleeRegArgs == 0) no longer fires for them, while
  // true cdecl callees remain at 0 and get their stack arguments correctly
  // indexed.
  if (Convention && Convention->RegparmOnlyForInternalCalls) {
    const auto &TRI2 = getTargetRegInfo(Img.Arch);
    // Omitted support bodies retain the same contract in both passes.
    auto CRA2 = CalleeRegArity, CTA2 = CalleeTotalArity;
    auto CalleeIntRegs2 = CalleeIntRegs;
    for (const MedFunc *Body : SignatureBodies) {
      const MedFunc &MF = *Body;
      int MaxRI = -1, MaxI = -1;
      const IntegerArgumentLayout Layout = integerArgumentLayoutOf(MF, TRI2);
      for (const auto &P : MF.Params) {
        const int Index =
            P.RegOff != kNoParamReg ? Layout.registerIndex(P.RegOff) : -1;
        if (Index >= 0) {
          MaxRI = std::max(MaxRI, Index);
          MaxI = std::max(MaxI, Index);
        } else if (P.Kind == MedVar::Param)
          MaxI = std::max(MaxI, P.Id);
      }
      CRA2[MF.Entry] = MaxRI + 1;
      CTA2[MF.Entry] = callRecoveryTotalArity(MF, MaxI);
      if (!MF.IntegerArgumentRegisters.empty())
        CalleeIntRegs2[MF.Entry] = MF.IntegerArgumentRegisters;
    }
    for (MedFunc *Body : SignatureBodies) {
      MedFunc &MF = *Body;
      recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &CRA2, &CTA2,
                     &CalleeFPArity, &CalleeFPReturnSize, &CalleeFPRegs,
                     &CalleeHasSret, &CalleeIsVariadic, &CalleeConsumesVaList,
                     /*FrameLocalLeafCallees=*/nullptr, &CalleeIntRegs2);
    }
  }
  // An omitted definition cannot give the emitter its parameter count.
  // Apply the same final signature bound used for an emitted callee after
  // forwarder promotion, so incidental live registers do not become extra
  // arguments merely because output was restricted to its caller.
  std::map<va_t, const MedFunc *> OmittedByEntry;
  for (const MedFunc &F : OmittedCallees)
    if (!F.IsVariadic && !F.SourceParametersBound)
      OmittedByEntry.emplace(F.Entry, &F);
  for (MedFunc &F : Result.MedFuncs)
    for (MedCallInfo &CI : F.CallInfos) {
      auto It = CI.IsIndirect ? OmittedByEntry.end()
                              : OmittedByEntry.find(CI.TargetAddr);
      if (It != OmittedByEntry.end() &&
          CI.Args.size() > It->second->Params.size())
        CI.Args.resize(It->second->Params.size());
    }
}

void matchCallsToCalleeSignatures(const BinaryImage &Img,
                                  PipelineResult &Result) {
  std::map<va_t, const MedFunc *> ByEntry;
  for (const MedFunc &MF : Result.MedFuncs)
    ByEntry.emplace(MF.Entry, &MF);
  for (MedFunc &MF : Result.MedFuncs)
    for (MedCallInfo &CI : MF.CallInfos) {
      const auto It =
          CI.IsIndirect ? ByEntry.end() : ByEntry.find(CI.TargetAddr);
      if (It == ByEntry.end())
        continue;
      const MedFunc &Callee = *It->second;
      if (Callee.IsVariadic || Callee.SourceParametersBound ||
          Img.findImportAt(Callee.Entry))
        continue;
      if (CI.Args.size() > Callee.Params.size())
        CI.Args.resize(Callee.Params.size());
    }
}

} // namespace neverd
