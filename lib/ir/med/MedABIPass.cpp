//===- MedABIPass.cpp - ABI analysis pass for MedIR ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MedIR ABI recovery pass (calling conventions, arguments).
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedABIPass.h"

#include "MedABIPassDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/libc/LibCNames.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace neverd {

void recoverCallAbi(MedFunc &Func, Arch TheArch,
                    const std::map<va_t, std::string> &FuncNames,
                    const BinaryImage *Img,
                    const std::map<va_t, int> *CalleeRegArity,
                    const std::map<va_t, int> *CalleeTotalArity,
                    const std::map<va_t, int> *CalleeFPArity,
                    const std::map<va_t, bool> *CalleeReturnsVec,
                    const std::map<va_t, std::vector<uint64_t>> *CalleeFPRegs,
                    const std::map<va_t, bool> *CalleeHasSret,
                    const std::map<va_t, bool> *CalleeIsVariadic) {
  Func.CallInfos.clear();
  (void)CalleeReturnsVec; // FP-return routing is modeled earlier (LowToMed)

  const auto &TRI = getTargetRegInfo(TheArch);

  // Argument registers a forwarder passes straight through from its incoming
  // value (recovered only via the live-in fallback, never written in the
  // function): promoted to real parameters after all calls are processed.
  std::map<int, MedVar> PromoteParams;

  // The floating-point analogue of PromoteParams: FP/vector argument registers
  // (v0-7 / d0-7 / xmm0-7) a pure tail-call forwarder passes straight through
  // into its call.  `double cmag(c){return cabs(c);}` lowers at -O2 to a lone
  // `b _cabs`, so the incoming d0/d1 never appear as a write the FP-argument
  // scan can find; without surfacing them the emitter forwards an uninitialised
  // register (the call gets `0.0`).
  std::map<int, MedVar> PromoteFPParams;

  // Set to the scalar FP return width (4 float / 8 double) when this function
  // is a pure tail-call forwarder to a scalar-FP-returning libc callee: it
  // returns that callee's FP value, but inferReturnType ran before call
  // recovery and saw only the integer return register the rewritten CALL
  // nominally writes, so the return type must be corrected to floating point
  // here.  The block id and CALL address pin the forwarding CALL so its result
  // register (and the RETURN that reads it) can be rewired to the FP return
  // register after argument recovery.
  uint16_t ForwarderFPRetSize = 0;
  int ForwarderFPRetBlk = -1;
  va_t ForwarderFPRetCallAddr = 0;

  // A register is a live-in (incoming parameter) when nothing in the function
  // writes it -- the value a pure forwarder passes straight into its tail call.
  auto funcDefinesReg = [&Func](uint64_t Reg) {
    for (const auto &B : Func.Blocks) {
      for (const auto &O : B.Ops)
        if (O.Output.Kind == MedVar::Reg && O.Output.Size > 0 &&
            O.Output.RegOff == Reg)
          return true;
      for (const auto &Ph : B.Phis)
        if (Ph.Output.Kind == MedVar::Reg && Ph.Output.RegOff == Reg)
          return true;
    }
    return false;
  };

  // Stack-argument slots a tail-call forwarder passes straight through from its
  // own incoming stack frame (the `jmp callee` reuses it, so there is no
  // store): promoted to incoming stack parameters after all calls are
  // processed.
  std::set<int> PromoteStackParams;

  // Deferred SUBBYTES ops that split one wide outgoing store into a separate
  // argument per stack slot (i386 -O2 forwards N adjacent arguments with a
  // single movaps/movups).  Inserted into the block — and the recorded call
  // indices fixed up — after every call is processed, so the in-progress scan
  // indices stay valid.  Their fresh output ids start past every existing id.
  struct LaneInsert {
    int BlockId;
    int BeforeOpIdx;
    std::vector<MedOp> Ops;
  };
  std::vector<LaneInsert> Pending;
  int FreshId = 0;
  for (const auto &B : Func.Blocks) {
    for (const auto &O : B.Ops) {
      if (O.Output.Id >= FreshId)
        FreshId = O.Output.Id + 1;
      for (uint8_t I = 0; I < O.NumInputs; ++I)
        if (O.Inputs[I].Id >= FreshId)
          FreshId = O.Inputs[I].Id + 1;
    }
    for (const auto &Phi : B.Phis) {
      if (Phi.Output.Id >= FreshId)
        FreshId = Phi.Output.Id + 1;
      for (const auto &PA : Phi.Args)
        if (PA.second.Id >= FreshId)
          FreshId = PA.second.Id + 1;
    }
  }
  for (const auto &P : Func.Params)
    if (P.Id >= FreshId)
      FreshId = P.Id + 1;

  for (auto &Blk : Func.Blocks) {
    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;

      MedCallInfo CI;
      CI.BlockId = Blk.Id;
      CI.OpIdx = static_cast<int>(OI);
      CI.IsIndirect = (Op.Opcode == NdOp::INDIR_CALL);

      // SUBBYTES lane-extraction ops for any wide outgoing store at this call,
      // recorded for deferred insertion before the call.
      std::vector<MedOp> CallLaneOps;

      if (Op.NumInputs >= 1 && Op.Inputs[0].isConst())
        CI.TargetAddr = Op.Inputs[0].ConstVal;

      auto FnIt = FuncNames.find(CI.TargetAddr);
      CI.TargetName =
          FnIt != FuncNames.end()
              ? FnIt->second
              : (kAutoFuncPrefix + llvm::utohexstr(CI.TargetAddr)).str();
      // In a relocatable object EVERY direct call's target operand is a
      // placeholder (0 / the next-instruction-relative encoding); the real
      // callee is named by the branch relocation, and the per-VA callee
      // metadata (arity, FP arity, ...) is keyed by the function ENTRY VA.  A
      // call whose placeholder resolves to VA 0 therefore borrows the metadata
      // of whatever intra-module function happens to sit at offset 0 — fine
      // when the relocation names that very function, but a *false* match when
      // the relocation names an EXTERNAL symbol (a libc `memmove` placeholder-0
      // call would otherwise be assembled with the VA-0 function's arity,
      // truncating its src/n arguments).  Flag that case so the intra-module
      // metadata lookups below are skipped and the external call falls through
      // to the libc-arity handling (the register/stack scans recover its real
      // arguments).
      bool IsRelocExtern = false;
      if (Img &&
          (CI.TargetAddr == 0 || CI.TargetName.starts_with(kAutoFuncPrefix))) {
        if (std::string RelName = relocCalleeName(*Img, Op.Addr);
            !RelName.empty()) {
          // External when the relocation names a different symbol than the one
          // the placeholder target address resolves to (compare underscore-
          // normalized so a Mach-O `_memmove` reloc still matches an
          // intra-module `memmove`).
          IsRelocExtern = stripLeadingUnderscores(CI.TargetName) !=
                          stripLeadingUnderscores(RelName);
          CI.TargetName = std::move(RelName);
        }
      }

      const bool IsDirectImport =
          !CI.IsIndirect && Img && Img->findImportAt(CI.TargetAddr);
      std::optional<libc::LibCArity> ExternalArity;
      if (!CI.IsIndirect)
        ExternalArity =
            libc::libcArity(stripLeadingUnderscores(CI.TargetName));

      // The callee's integer register-argument count, if it is a known direct
      // intra-module target; -1 when unknown (external / indirect).  Used to
      // bound how many incoming registers a forwarder passes through.
      int CalleeRegArgs = -1;
      if (!CI.IsIndirect && !IsRelocExtern && !IsDirectImport &&
          CalleeRegArity) {
        auto AIt = CalleeRegArity->find(CI.TargetAddr);
        if (AIt != CalleeRegArity->end())
          CalleeRegArgs = AIt->second;
      }

      // The callee's full integer-argument count (register + stack), for
      // bounding a tail-call forwarder's passed-through stack arguments.
      int CalleeArgs = -1;
      if (!CI.IsIndirect && !IsRelocExtern && !IsDirectImport &&
          CalleeTotalArity) {
        auto AIt = CalleeTotalArity->find(CI.TargetAddr);
        if (AIt != CalleeTotalArity->end())
          CalleeArgs = AIt->second;
      }

      // Executable import veneers can also appear in Callee*Arity as tiny
      // discovered functions.  Their apparent live-in set is not the imported
      // function's signature (an AArch64 stub seems to consume x0-x7), so a
      // curated external signature must override that synthetic arity.  It
      // also lets a compiler-generated helper pass an incoming exception
      // object straight through without an explicit register write.
      const bool UseExternalArity =
          ExternalArity.has_value() &&
          (IsDirectImport || IsRelocExtern || CalleeRegArgs < 0);
      if (UseExternalArity) {
        CalleeRegArgs =
            std::min(ExternalArity->IntArgs,
                     static_cast<int>(TRI.IntParamRegs.size()));
        CalleeArgs = ExternalArity->IntArgs;
      }

      // Apple/Darwin AArch64 passes EVERY variadic argument on the stack
      // (unlike AAPCS64/Linux, which fills x0-x7 first).  For a known-variadic
      // libc callee (the printf/scanf family, ...) the outgoing stores at
      // [call_sp + k*8] are therefore the call's arguments right after the
      // fixed prefix -- argument (NumFixed + k), NOT (NumRegArgSlots + k).
      // Mapping them past the 8 register slots leaves args [NumFixed, 8) as
      // gaps, and the first-gap assembly cutoff then drops every variadic
      // argument (printf("%d", x) loses x and prints a stack garbage value).
      // x86-64 passes the leading varargs in registers (no bug), as does Linux
      // AAPCS64, so this is gated to Mach-O AArch64 known-variadic direct
      // calls.
      int DarwinVarArgBase = -1;
      if (Img && Img->isMachO() && TheArch == Arch::AArch64 && !CI.IsIndirect) {
        llvm::StringRef Bare = stripLeadingUnderscores(CI.TargetName);
        if (unsigned NF = libc::varArgFixedCount(Bare); NF > 0)
          DarwinVarArgBase = static_cast<int>(NF);
        // A user-defined intra-module variadic callee (detected by
        // detectVariadicForwarders) follows the same Darwin convention: its
        // fixed prefix is the register-argument count, after which every
        // variadic argument is stack-passed.
        if (DarwinVarArgBase < 0 && CalleeRegArgs >= 0 && CalleeIsVariadic) {
          auto VIt = CalleeIsVariadic->find(CI.TargetAddr);
          if (VIt != CalleeIsVariadic->end() && VIt->second)
            DarwinVarArgBase = CalleeRegArgs;
        }
      }
      // Indirect call through a function pointer that provably holds a known
      // variadic intra-module function (`double (*fp)(int,...) = dsum;
      // fp(...)`). Such a callee has no direct call site to drive the
      // cross-function variadic recovery (finalizeVariadicCallees), so the call
      // site otherwise passes the FP/stack varargs in registers (the
      // FP-register scan even mistakes the last `fmov d0` leftover for an
      // argument) while the callee reads them off the stack -> garbage. Resolve
      // the target and, when it is a known variadic function, drive the SAME
      // Darwin variadic stack handling as a direct call: DarwinVarArgBase = the
      // fixed register-argument prefix suppresses the spurious FP-register scan
      // and routes every vararg through the stack scan, after which the
      // indirect-variadic block below marks the fixed prefix so the emitter
      // declares a variadic signature.
      if (Img && Img->isMachO() && TheArch == Arch::AArch64 && CI.IsIndirect &&
          DarwinVarArgBase < 0 && CalleeRegArity && CalleeIsVariadic &&
          Op.NumInputs >= 1) {
        va_t Resolved = resolveIndirectTargetAddr(Blk, static_cast<int>(OI),
                                                  Op.Inputs[0], 0);
        if (Resolved) {
          auto VIt = CalleeIsVariadic->find(Resolved);
          auto AIt = CalleeRegArity->find(Resolved);
          if (VIt != CalleeIsVariadic->end() && VIt->second &&
              AIt != CalleeRegArity->end() && AIt->second >= 0)
            DarwinVarArgBase = AIt->second;
        }
      }

      constexpr int MaxArgs = limits::kMaxCallArgs;
      std::vector<MedVar> Found(MaxArgs);
      std::vector<bool> FoundMask(MaxArgs, false);
      // Slots filled by the stack-store scan (vs the register scan).  On i386 a
      // stack-argument slot index can fall into the integer-register slot range
      // (0,1) when the callee takes no integer register argument but does take
      // floating-point register arguments; such slots must be assembled as
      // stack arguments (after the FP arguments), not as register arguments.
      std::vector<bool> FromStackScan(MaxArgs, false);

      // i386 passes arguments in a parameter register (ECX/EDX) only for the
      // internal regparm convention clang gives a *directly*-called static
      // function.  An indirect call goes through a function-pointer type, which
      // always uses standard cdecl (every argument on the stack), so a
      // parameter register live at the call — e.g. the ECX index of `call
      // *tab[ecx*4]` — is not an argument; recover such calls purely from their
      // stack stores.
      const bool RegArgsApply = !(TheArch == Arch::X86 && CI.IsIndirect);

      if (RegArgsApply)
        for (int J = static_cast<int>(OI) - 1; J >= 0; --J) {
          auto &Prev = Blk.Ops[J];
          bool IsCall = Prev.Opcode == NdOp::CALL ||
                        Prev.Opcode == NdOp::INDIR_CALL ||
                        Prev.Opcode == NdOp::INTRINSIC;
          // A preceding call's return value lands in the result register, which
          // is also argument register 0 on these ABIs, so it is the reaching
          // definition for a following call's argument when nothing overwrites
          // it in between — the accumulator chain `acc = f(acc, i)`.  Record it
          // (only if the slot is still open) before stopping the backward scan.
          if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0) {
            int ArgIdx = TRI.regToArgIdx(Prev.Output.RegOff);
            if (ArgIdx >= 0 && ArgIdx < MaxArgs && !FoundMask[ArgIdx]) {
              Found[ArgIdx] = argRegSourceValueInBlock(Blk, J, TRI);
              FoundMask[ArgIdx] = true;
            }
          }
          if (IsCall)
            break;
        }

      // The stack-store scan is windowed to the outgoing-arg setup just before
      // the call; it stops at the previous call boundary and lets the closest
      // store to each slot win.
      int StoreScanStart = 0;

      // The SP offset at the call site, so pushed arguments can be placed at
      // their true distance from it regardless of the moving stack pointer left
      // by successive `push`es (x86/x64 pass stack arguments by `push`,
      // AArch64/ ARM by `str [sp,#k]`).  The call-site SP is the most recent
      // stack-pointer definition before the call (entry SP = offset 0).
      int64_t CallSpDelta = 0;
      std::map<SpOffsetKey, int64_t> CallSpOffsets;
      for (int J = static_cast<int>(OI) - 1; J >= 0; --J) {
        auto &Prev = Blk.Ops[J];
        if (Prev.Output.Kind == MedVar::Reg &&
            Prev.Output.RegOff == TRI.StackPointer) {
          if (auto D = stackPtrDelta(Blk, TRI, Prev.Output))
            CallSpDelta = *D;
          // Relative-offset fallback: map the call SP's own definition chain so
          // pushes can still be placed when the absolute entry delta is
          // unavailable (a post-loop push chain threading a loop-carried PHI).
          buildCallSpOffsets(Blk, TRI, Prev.Output, 0, CallSpOffsets, 0);
          break;
        }
      }

      // A value spilled to the call frame before the call (an outgoing `push` /
      // `str [sp,#k]` landing at or above the call SP) means the ABI has run
      // out of parameter registers — so every parameter register is necessarily
      // a real argument.  Detect this up front so a loop-carried value parked
      // in a parameter register, recovered only via its block PHI, still fills
      // its slot even though it sits "below" an already-found stack argument.
      bool HasStackArg = false;
      for (int J = static_cast<int>(OI) - 1;
           J >= StoreScanStart && !HasStackArg; --J) {
        auto &Prev = Blk.Ops[J];
        if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
            Prev.Opcode == NdOp::INTRINSIC)
          break;
        if (Prev.Opcode == NdOp::STORE && Prev.NumInputs >= 2)
          if (auto D = stackPtrDelta(Blk, TRI, Prev.Inputs[0]))
            if (*D - CallSpDelta >= 0)
              HasStackArg = true;
      }
      const int NumIntParamRegs = static_cast<int>(TRI.IntParamRegs.size());

      // An argument already resident in its parameter register — a loop-carried
      // value never re-moved before the call, e.g. `for(...) acc = f(acc, i)`
      // keeps `acc` in arg0 across iterations — leaves no in-block write for
      // the backward scan above to see.  Recover it from the block's PHI for
      // that argument register (its reaching value at the call point).
      //
      // Indirect call: the arity is unknown, so fill consecutive slots from
      // arg0 until a slot has no reaching PHI.  Direct call: arguments are
      // consecutive, so a found argN proves arg0..argN are all real; without
      // that proof a trailing phi-derived slot could invent an argument the
      // callee never takes.  But a stack argument proves every parameter
      // register is real, so extend the search across the whole
      // parameter-register file in that case.
      int MaxRegArg = -1;
      for (int K = 0; K < MaxArgs; ++K)
        if (FoundMask[K])
          MaxRegArg = K;
      int RegPhiLimit = MaxRegArg;
      if (HasStackArg)
        RegPhiLimit = std::max(RegPhiLimit, NumIntParamRegs - 1);
      for (int K = 0; RegArgsApply && K < MaxArgs; ++K) {
        if (FoundMask[K])
          continue;
        if (!CI.IsIndirect && K > RegPhiLimit)
          break; // direct call: never invent a trailing argument
        // Prefer the widest reaching register view, except when that view is
        // created only on a loop back-edge and therefore has no value on the
        // entry edge.  AArch64 commonly carries a 32-bit Wn argument and also
        // synthesises an Xn PHI from a back-edge zext; selecting the wider PHI
        // would pass undef on the first iteration.  A narrower PHI with a real
        // definition on that same entry edge is the authoritative value.
        auto entryEdgeSeeded = [&](const PhiNode &Phi) {
          for (const auto &A : Phi.Args) {
            const MedBlock *Pred = nullptr;
            for (const auto &B : Func.Blocks)
              if (B.Id == A.first) {
                Pred = &B;
                break;
              }
            if (!Pred || !Pred->Preds.empty())
              continue;
            for (const auto &P : Pred->Phis)
              if (P.Output.Id == A.second.Id &&
                  P.Output.SSAVer == A.second.SSAVer)
                return true;
            for (const auto &O : Pred->Ops)
              if (O.Output.Id == A.second.Id &&
                  O.Output.SSAVer == A.second.SSAVer && O.Output.Size > 0)
                return true;
            for (const auto &P : Func.Params)
              if (P.RegOff == A.second.RegOff && P.Size == A.second.Size)
                return true;
            return false;
          }
          return true; // no function-entry predecessor on this PHI
        };

        const PhiNode *Best = nullptr;
        for (auto &Phi : Blk.Phis) {
          if (Phi.Output.Kind != MedVar::Reg ||
              TRI.regToArgIdx(Phi.Output.RegOff) != K)
            continue;
          const bool PhiSeeded = entryEdgeSeeded(Phi);
          if (!Best) {
            Best = &Phi;
            continue;
          }
          const bool BestSeeded = entryEdgeSeeded(*Best);
          if (PhiSeeded != BestSeeded ? PhiSeeded
                                      : Phi.Output.Size > Best->Output.Size)
            Best = &Phi;
        }
        if (!Best) {
          if (CI.IsIndirect)
            break;  // indirect arguments are consecutive from arg0
          continue; // direct: leave the gap for the stack scan to try
        }
        Found[K] = Best->Output;
        FoundMask[K] = true;
      }

      // A register argument can be materialised in a block that dominates the
      // call rather than the call's own block (e.g. a loop-invariant count set
      // before a vectorised loop whose predicated branches push the call into a
      // later block) — invisible to the in-block and in-block-PHI scans above.
      // For a direct call (arguments consecutive from arg0; the emitter
      // truncates any surplus to the callee arity) fill the remaining
      // consecutive register slots from the value reaching the call across the
      // CFG.
      if (!CI.IsIndirect && RegArgsApply)
        for (int K = 0; K < NumIntParamRegs && K < MaxArgs; ++K) {
          if (FoundMask[K])
            continue;
          if (K > 0 && !FoundMask[K - 1])
            break; // keep arguments consecutive from arg0
          // Recover a pure live-in (a forwarded incoming register with no
          // reaching definition) only within the callee's arity, so a register
          // the callee never takes is not invented as an argument.
          bool AllowLiveIn = (CalleeRegArgs >= 0 && K < CalleeRegArgs);
          bool FromLiveIn = false;
          auto V = findReachingArgReg(Func, TRI, TheArch, Blk.Id, K,
                                      AllowLiveIn, &FromLiveIn);
          if (!V)
            break;
          Found[K] = *V;
          FoundMask[K] = true;
          if (FromLiveIn)
            PromoteParams.emplace(K, *V);
        }

      // The same cross-block materialisation for an INDIRECT call: its arity is
      // unknown, but a found argN proves arg0..argN are all real, so fill any
      // gap BELOW the highest evidenced register argument from the value
      // reaching the call across the CFG.  This recovers a loop-carried
      // accumulator parked in arg0 (`for(...) acc = fp(acc, i)`) whose only
      // definition is the loop- header PHI in a dominating block — the
      // in-block-PHI scan above sees only the call block's own PHIs, which a
      // rotated -Oz loop leaves empty.  The below-highest bound keeps it from
      // inventing a trailing argument from an unrelated reaching write, so no
      // live-in fallback is allowed here.
      if (CI.IsIndirect && RegArgsApply) {
        int HiEvidenced = -1;
        for (int K = 0; K < MaxArgs; ++K)
          if (FoundMask[K])
            HiEvidenced = K;
        for (int K = 0; K < HiEvidenced && K < MaxArgs; ++K) {
          if (FoundMask[K])
            continue;
          auto V = findReachingArgReg(Func, TRI, TheArch, Blk.Id, K,
                                      /*AllowUnknownLiveIn=*/false, nullptr);
          if (!V)
            continue;
          Found[K] = *V;
          FoundMask[K] = true;
        }
      }

      // i386 cdecl: a callee with 0 detected register arguments but >0 stack
      // arguments uses the cdecl convention (ALL arguments on the stack).  The
      // register scan may have placed scratch ECX/EDX values at slots 0-1;
      // clear them so FirstStackSlot is not shifted and the stack scan maps
      // outgoing stores to the correct argument indices.  Safe after the
      // two-pass Pipeline promotion: on the second pass, a forwarder whose
      // register params were promoted has CalleeRegArgs > 0 and is unaffected.
      //
      // An EXTERNAL i386 call (a libc `memcpy`/`memmove`/`memset`, named by a
      // branch relocation with a placeholder-0 target) likewise uses standard
      // cdecl — the ECX/EDX regparm convention is reserved for directly-called
      // intra-module static functions, never an imported symbol — so its
      // register-scan slots are scratch and must be cleared too.  Without this
      // the surviving scratch shifts every stack argument up one slot
      // (`memcpy(scratch, dst, src)` drops the size, copying a wild count).
      if (TheArch == Arch::X86 && !CI.IsIndirect &&
          ((CalleeRegArgs == 0 && CalleeArgs > 0) || IsRelocExtern)) {
        for (int K = 0; K < MaxArgs; ++K)
          if (FoundMask[K] && !FromStackScan[K]) {
            FoundMask[K] = false;
            Found[K] = MedVar();
          }
      }

      // Darwin AArch64 variadic call: only the NumFixed fixed arguments travel
      // in registers (x0..x_{NumFixed-1}); every variadic argument is
      // stack-passed. Drop any register-scan slot at or past the fixed prefix
      // (a scratch register written between the two calls) so it cannot shadow
      // a variadic stack slot recovered below.
      if (DarwinVarArgBase >= 0)
        for (int K = DarwinVarArgBase; K < MaxArgs; ++K)
          if (FoundMask[K]) {
            FoundMask[K] = false;
            Found[K] = MedVar();
          }

      int FirstStackSlot = 0;
      for (int K = 0; K < MaxArgs; ++K) {
        if (FoundMask[K])
          FirstStackSlot = K + 1;
        else
          break;
      }

      // FP/vector overflow stack arguments of an indirect call, keyed by their
      // byte offset from the call SP and recorded at the store granularity (a
      // 4-byte `float` / 8-byte `double`).  The 8-byte-slot model below
      // collapses two AAPCS64-packed floats into a single slot; this finer
      // record lets the FP-argument assembly recover every overflow value in
      // order (see the FP overflow run after the FP-register scan).
      std::map<int64_t, MedVar> FPStackByOff;

      // Integer outgoing stack stores of a DIRECT call, keyed by byte offset
      // from the call SP and recorded at store granularity.  Apple's arm64 ABI
      // packs two sub-8-byte integer stack arguments into one 8-byte slot (`int
      // a@[sp+0], b@[sp+4]`), but the 8-byte-slot scan below keeps only one
      // store per slot (dropping the other and mispositioning the survivor in
      // the slot's low half).  The packing pass after the scan reconstructs
      // each slot's full 8-byte value from these finer records so it matches
      // what the callee reads (a single i64 it unpacks via trunc/lshr).
      std::map<int64_t, MedVar> IntStackByOff;

      for (int J = static_cast<int>(OI) - 1; J >= StoreScanStart; --J) {
        auto &Prev = Blk.Ops[J];
        if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
            Prev.Opcode == NdOp::INTRINSIC)
          break;
        if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2)
          continue;

        const auto &AddrVar = Prev.Inputs[0];
        int64_t StackOff = -1;

        // Resolve the store's true offset from the call SP via the
        // stack-pointer delta chain, so consecutive `push`es land in distinct
        // argument slots (the simple "SP register => offset 0" rule below would
        // collapse every push onto slot 0).
        if (auto D = stackPtrDelta(Blk, TRI, AddrVar)) {
          int64_t Rel = *D - CallSpDelta;
          if (Rel >= 0)
            StackOff = Rel;
        }

        // Fallback when the absolute SP delta is unavailable (post-loop push
        // chain): place the store relative to the call SP via its definition
        // chain.  Only runs when the absolute scan above did not resolve, so
        // working cases are unaffected.
        if (StackOff < 0)
          if (auto R = relStackOff(Blk, TRI, AddrVar, CallSpOffsets, 0))
            if (*R >= 0)
              StackOff = *R;

        if (StackOff < 0 && AddrVar.Kind == MedVar::Reg &&
            AddrVar.RegOff == TRI.StackPointer)
          StackOff = 0;

        if (StackOff < 0 && !AddrVar.isConst()) {
          for (int K = J - 1; K >= 0; --K) {
            auto &DefOp = Blk.Ops[K];
            if (DefOp.Output.Id != AddrVar.Id ||
                DefOp.Output.SSAVer != AddrVar.SSAVer)
              continue;
            if (DefOp.Opcode == NdOp::INT_ADD && DefOp.NumInputs >= 2) {
              bool HasSP = false;
              int64_t ConstOff = -1;
              for (uint8_t KI = 0; KI < DefOp.NumInputs; ++KI) {
                if (DefOp.Inputs[KI].Kind == MedVar::Reg &&
                    DefOp.Inputs[KI].RegOff == TRI.StackPointer)
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

        if (StackOff < 0)
          continue;

        int SlotSize = TRI.PointerSize;
        const int NumRegArgSlots = static_cast<int>(TRI.IntParamRegs.size());
        // The argument index of the first stack slot [call_sp + 0]:
        //  - i386 cdecl: arg FirstStackSlot (every argument is stack-passed).
        //  - Darwin AArch64 variadic: arg NumFixed (all varargs are
        //  stack-passed
        //    immediately after the fixed register prefix).
        //  - other register ABIs: arg NumRegArgSlots (args 0..7 use x0-x7).
        int StackArgBase = (TheArch == Arch::X86)  ? FirstStackSlot
                           : DarwinVarArgBase >= 0 ? DarwinVarArgBase
                                                   : NumRegArgSlots;
        int SlotIdx = StackArgBase + static_cast<int>(StackOff / SlotSize);

        // Record an FP/vector-register-valued outgoing store at store
        // granularity so packed floats are not collapsed by the slot model.
        // The reverse scan visits later stores first; keep the first seen
        // (= last in program order, the value live at the call) per offset.
        if (CI.IsIndirect && Prev.Inputs[1].Kind == MedVar::Reg &&
            TRI.isFPArgReg(Prev.Inputs[1].RegOff) &&
            FPStackByOff.find(StackOff) == FPStackByOff.end())
          FPStackByOff[StackOff] = Prev.Inputs[1];

        // For an indirect call the callee signature is unknown, so an argument
        // that happens to live in a callee-saved register (a loop-carried local
        // clang parked there) must NOT be filtered out as a register save:
        // every SP-relative store in the call's own block is an outgoing
        // argument.
        if (!CI.IsIndirect && Prev.Inputs[1].Kind == MedVar::Reg &&
            TRI.isCalleeSaveReg(Prev.Inputs[1].RegOff) &&
            Prev.Inputs[1].SSAVer == 0) {
          // A prologue register save spills the callee-save register's
          // *incoming* (live-in, SSA version 0) value.  A non-zero version is a
          // computed or loop-carried value that merely lives in a callee-save
          // register, so it is a real outgoing argument — never filter those.
          uint64_t StoredReg = Prev.Inputs[1].RegOff;
          bool WasRedefined = false;
          for (int R = J - 1; R >= 0; --R) {
            auto &Def = Blk.Ops[R];
            if (Def.Output.Kind == MedVar::Reg &&
                Def.Output.RegOff == StoredReg && Def.Opcode != NdOp::COPY) {
              WasRedefined = true;
              break;
            }
            if (Def.Output.Kind == MedVar::Reg &&
                Def.Output.RegOff == StoredReg && Def.Opcode == NdOp::COPY &&
                Def.NumInputs >= 1 && !Def.Inputs[0].isConst()) {
              bool InputIsParam = false;
              for (int AI = 0; AI < MaxArgs; ++AI) {
                if (Def.Inputs[0].Kind == MedVar::Reg &&
                    TRI.regToArgIdx(Def.Inputs[0].RegOff) >= 0) {
                  InputIsParam = true;
                  break;
                }
              }
              if (InputIsParam)
                WasRedefined = true;
              break;
            }
          }
          if (!WasRedefined)
            continue;
        }

        // Record sub-8-byte integer stores (direct call) at store granularity
        // so the packing pass can rebuild an Apple-arm64 packed 8-byte slot
        // holding two of them.  Not gated on FoundMask, so both halves of a
        // packed slot are captured even though the slot scan below keeps only
        // the first seen. Keep the first-seen per offset (reverse scan => last
        // in program order, the value live at the call).  FP-register-valued
        // stores are handled by FPStackByOff; 8-byte stores fill a slot whole
        // and need no packing.
        if (!CI.IsIndirect && SlotSize > 0 && Prev.Inputs[1].Size > 0 &&
            Prev.Inputs[1].Size < SlotSize &&
            !(Prev.Inputs[1].Kind == MedVar::Reg &&
              TRI.isFPArgReg(Prev.Inputs[1].RegOff)) &&
            IntStackByOff.find(StackOff) == IntStackByOff.end())
          IntStackByOff[StackOff] = Prev.Inputs[1];

        if (SlotIdx >= 0 && SlotIdx < MaxArgs && !FoundMask[SlotIdx]) {
          const MedVar &SV = Prev.Inputs[1];
          int Lanes =
              (SlotSize > 0 && SV.Size > SlotSize && (SV.Size % SlotSize) == 0)
                  ? SV.Size / SlotSize
                  : 1;
          // A single wide store covering several outgoing 4-byte slots is split
          // into one argument per slot — but only for a direct callee that
          // actually takes a scalar argument at every covered slot, so a
          // genuine wide (vector) argument is still passed whole.
          bool SplitWide = Lanes > 1 && !CI.IsIndirect && CalleeArgs >= 0 &&
                           SlotIdx + Lanes <= CalleeArgs;
          if (SplitWide)
            for (int L = 0; L < Lanes; ++L)
              if (SlotIdx + L >= MaxArgs || FoundMask[SlotIdx + L]) {
                SplitWide = false;
                break;
              }
          if (SplitWide) {
            for (int L = 0; L < Lanes; ++L) {
              MedVar Lane;
              Lane.Kind = MedVar::Temp;
              Lane.Id = FreshId++;
              Lane.SSAVer = 0;
              Lane.Size = static_cast<uint16_t>(SlotSize);
              Lane.TheArch = TheArch;
              MedOp Sub;
              Sub.Opcode = NdOp::SUBBYTES;
              Sub.Addr = Op.Addr;
              Sub.Output = Lane;
              Sub.addInput(SV);
              Sub.addInput(
                  MedVar::makeConst(static_cast<uint64_t>(L * SlotSize), 4));
              CallLaneOps.push_back(std::move(Sub));
              Found[SlotIdx + L] = Lane;
              FoundMask[SlotIdx + L] = true;
              FromStackScan[SlotIdx + L] = true;
            }
          } else {
            Found[SlotIdx] = SV;
            FoundMask[SlotIdx] = true;
            FromStackScan[SlotIdx] = true;
          }
        }
      }

      // Pack two AAPCS64-packed sub-8-byte integer stack arguments into one
      // 8-byte slot value (Apple arm64 places e.g. `int a8@[sp+0], a9@[sp+4]`
      // in a single slot).  The reverse slot scan above kept only one store per
      // slot and put it in the low half, so reconstruct the slot's full 8-byte
      // content from its constituent sub-stores (each zero-extended and shifted
      // to its intra-slot byte position, then OR'd) -- this is exactly the i64
      // the callee reads and unpacks.  Direct calls only: the callee models the
      // slot as one i64 parameter; an indirect callee's packed integer overflow
      // args are a separate, documented gap (ind-variadic-note family).
      if (!CI.IsIndirect && !IntStackByOff.empty() && TRI.PointerSize > 0) {
        const int PackSlotSize = TRI.PointerSize;
        const int PackStackBase =
            (TheArch == Arch::X86)  ? FirstStackSlot
            : DarwinVarArgBase >= 0 ? DarwinVarArgBase
                                    : static_cast<int>(TRI.IntParamRegs.size());
        std::map<int, std::vector<std::pair<int64_t, MedVar>>> BySlot;
        for (auto &E : IntStackByOff) {
          int Slot = PackStackBase + static_cast<int>(E.first / PackSlotSize);
          int64_t Intra = E.first % PackSlotSize;
          BySlot[Slot].push_back({Intra, E.second});
        }
        for (auto &S : BySlot) {
          int Slot = S.first;
          auto &Subs = S.second;
          if (Slot < 0 || Slot >= MaxArgs)
            continue;
          if (CalleeArgs >= 0 && Slot >= CalleeArgs)
            continue; // beyond the callee's arity: would be truncated anyway
          if (Subs.size() < 2)
            continue; // a lone sub-store is already the slot model's value
          std::sort(Subs.begin(), Subs.end(),
                    [](const std::pair<int64_t, MedVar> &A,
                       const std::pair<int64_t, MedVar> &B) {
                      return A.first < B.first;
                    });
          bool DistinctOffsets = true;
          for (size_t I = 1; I < Subs.size(); ++I)
            if (Subs[I].first == Subs[I - 1].first) {
              DistinctOffsets = false;
              break;
            }
          if (!DistinctOffsets)
            continue;
          auto freshSlotTemp = [&]() {
            MedVar T;
            T.Kind = MedVar::Temp;
            T.Id = FreshId++;
            T.SSAVer = 0;
            T.Size = static_cast<uint16_t>(PackSlotSize);
            T.TheArch = TheArch;
            return T;
          };
          MedVar Acc;
          bool HaveAcc = false;
          for (auto &P : Subs) {
            MedVar Z = freshSlotTemp();
            MedOp ZOp;
            ZOp.Opcode = NdOp::INT_ZEXT;
            ZOp.Addr = Op.Addr;
            ZOp.Output = Z;
            ZOp.addInput(P.second);
            CallLaneOps.push_back(std::move(ZOp));
            MedVar Term = Z;
            if (P.first != 0) {
              MedVar Sh = freshSlotTemp();
              MedOp ShOp;
              ShOp.Opcode = NdOp::INT_LEFT;
              ShOp.Addr = Op.Addr;
              ShOp.Output = Sh;
              ShOp.addInput(Z);
              ShOp.addInput(
                  MedVar::makeConst(static_cast<uint64_t>(P.first * 8), 4));
              CallLaneOps.push_back(std::move(ShOp));
              Term = Sh;
            }
            if (!HaveAcc) {
              Acc = Term;
              HaveAcc = true;
            } else {
              MedVar Or = freshSlotTemp();
              MedOp OrOp;
              OrOp.Opcode = NdOp::INT_OR;
              OrOp.Addr = Op.Addr;
              OrOp.Output = Or;
              OrOp.addInput(Acc);
              OrOp.addInput(Term);
              CallLaneOps.push_back(std::move(OrOp));
              Acc = Or;
            }
          }
          if (HaveAcc) {
            Found[Slot] = Acc;
            FoundMask[Slot] = true;
            FromStackScan[Slot] = true;
          }
        }
      }

      // Tail-call forwarder: the callee's stack arguments are passed straight
      // through from this function's own incoming stack frame (the original
      // `jmp callee`, rewritten to CALL+RETURN, reuses it), so no store exists
      // for the scan above to find.  When the call is in tail position — its
      // result register is immediately returned — recover each still-missing
      // stack-argument slot the callee takes as this function's incoming stack
      // parameter and surface it as a parameter (the stack dual of the
      // passed-through register arguments above).
      bool IsTailReturn = static_cast<size_t>(OI) + 1 < Blk.Ops.size() &&
                          Blk.Ops[OI + 1].Opcode == NdOp::RETURN &&
                          Blk.Ops[OI + 1].NumInputs >= 1 &&
                          Blk.Ops[OI + 1].Inputs[0].Kind == MedVar::Reg &&
                          Blk.Ops[OI + 1].Inputs[0].RegOff == Op.Output.RegOff;

      // A pure tail-call forwarder to a scalar-FP-returning callee returns that
      // callee's FP value; the rewritten tail-call CALL nominally writes the
      // integer return register, so record the forwarding CALL here and rewire
      // its result + the RETURN that reads it to the FP return register after
      // argument recovery.  The forwarder's FP return type was set on Func by
      // the Pipeline forwarder pass (libc via libcArity, intra-module via the
      // callee's recovered ReturnType) -- inferReturnType ran before call
      // recovery and could not see it.
      if (IsTailReturn && Func.ReturnType &&
          Func.ReturnType->Kind == NdTypeKind::Float) {
        ForwarderFPRetSize = Func.ReturnType->Size ? Func.ReturnType->Size : 8;
        ForwarderFPRetBlk = Blk.Id;
        ForwarderFPRetCallAddr = Op.Addr;
      }
      if (IsTailReturn && !CI.IsIndirect && CalleeArgs > NumIntParamRegs) {
        for (int K = NumIntParamRegs; K < CalleeArgs && K < MaxArgs; ++K) {
          if (FoundMask[K])
            continue;
          if (K > 0 && !FoundMask[K - 1])
            break; // keep arguments consecutive from arg0
          MedVar P;
          P.Kind = MedVar::Param;
          P.Id = K;
          P.RegOff = kNoParamReg;
          P.Size = static_cast<uint16_t>(TRI.PointerSize);
          P.TheArch = TheArch;
          Found[K] = P;
          FoundMask[K] = true;
          PromoteStackParams.insert(K);
        }
      }

      // --- Recover floating-point / vector arguments (XMM0-7 / V0-7) ---
      // These use an argument-register class with an index independent of the
      // integer arguments, so `double f(double,double)` takes no integer
      // argument at all.  Each is the value reaching its FP-argument register
      // at the call site (closest in-block write, including a preceding call's
      // FP return; else a loop-carried block PHI), bounded by the callee's FP
      // arity.
      //
      // Skip this for a Darwin AArch64 variadic libc callee: the printf/scanf
      // family takes no floating-point *fixed* argument and passes every
      // variadic argument -- doubles included -- on the stack, where the stack
      // scan recovers them (as the i64 bit pattern, exactly what printf reads).
      // Probing V0..V7 here would pick up a caller-saved register left by a
      // preceding call (e.g. a `double poly(...)` return spilled to the
      // outgoing stack slot) and inject it as a spurious FP argument, shifting
      // the real variadic arguments so the callee reads the wrong slots
      // (printf("p[%d]=%.3f", i, v) then prints "p[0]=0.000").
      std::vector<MedVar> FoundFP;
      if (RegArgsApply && !TRI.FPParamRegs.empty() && DarwinVarArgBase < 0) {
        // The FP-argument registers to probe, in ABI order.  Default to the
        // architecture's FP parameter registers (XMM0-7 / V0-7 / ARM D0-7); for
        // a direct call whose callee's exact FP layout is known, use it instead
        // so an ARM `float`-argument callee is recovered at s0,s1,.. (not
        // d0,d1) — the high-half S registers (s1=0x104) alias no D-register
        // slot.
        std::vector<uint64_t> FPRegs(TRI.FPParamRegs.begin(),
                                     TRI.FPParamRegs.end());
        if (!CI.IsIndirect && !IsRelocExtern && CalleeFPRegs) {
          auto RIt = CalleeFPRegs->find(CI.TargetAddr);
          if (RIt != CalleeFPRegs->end() && !RIt->second.empty())
            FPRegs = RIt->second;
        }
        int FPLimit = static_cast<int>(FPRegs.size());
        bool KnownFPCallee = false;
        if (!CI.IsIndirect && !IsRelocExtern && !IsDirectImport &&
            CalleeFPArity) {
          auto AIt = CalleeFPArity->find(CI.TargetAddr);
          if (AIt != CalleeFPArity->end()) {
            FPLimit = std::min(FPLimit, AIt->second);
            KnownFPCallee = true;
          }
        }
        // Width and count of FP arguments a pure tail-call forwarder passes
        // straight through.  External libc callees are NOT in CalleeFPArity
        // (that map is intra-module only), so consult the libc signature
        // directly: it both bounds the probe (FPLimit) and marks the callee
        // known so the live-in recovery below can fire for `cmag -> cabs`.
        uint16_t FwdFPArgSize = 8;
        if (UseExternalArity) {
          const libc::LibCArity &Sig = *ExternalArity;
          if (Sig.FpIsFloat)
            FwdFPArgSize = 4;
          if (!KnownFPCallee) {
            FPLimit = std::min(FPLimit, Sig.FpArgs);
            KnownFPCallee = true;
          }
        }
        // For a direct call to a known FP callee, clang may hoist an FP
        // argument's setup (e.g. `cvtsi2sd`/`ucvtf` into XMM0) above a branch
        // into the dominating predecessor — `acc += cond ? h(x) : x*0.5` puts
        // the call in one arm but computes x in the shared predecessor.  The
        // value reaching the FP-arg register is then the predecessor's last
        // write; walk up the single-predecessor chain to find it.
        auto reachingFPArgCrossBlock =
            [&](uint64_t Reg) -> std::optional<MedVar> {
          if (!KnownFPCallee || Blk.Preds.size() != 1)
            return std::nullopt;
          std::set<int> Seen{Blk.Id};
          int CurId = Blk.Preds[0];
          while (Seen.insert(CurId).second) {
            MedBlock *B = nullptr;
            for (auto &Cand : Func.Blocks)
              if (Cand.Id == CurId) {
                B = &Cand;
                break;
              }
            if (!B)
              return std::nullopt;
            for (int J = static_cast<int>(B->Ops.size()) - 1; J >= 0; --J) {
              auto &Prev = B->Ops[J];
              if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0 &&
                  Prev.Output.RegOff == Reg)
                return Prev.Output;
              if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL)
                return std::nullopt; // a call clobbers the caller-saved FP bank
            }
            for (const auto &Phi : B->Phis)
              if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == Reg)
                return Phi.Output;
            if (B->Preds.size() != 1)
              return std::nullopt;
            CurId = B->Preds[0];
          }
          return std::nullopt;
        };
        for (int K = 0; K < FPLimit; ++K) {
          uint64_t Reg = FPRegs[K];
          std::optional<MedVar> V;
          va_t ReachingWriteAddr = 0;
          for (int J = static_cast<int>(OI) - 1; J >= 0; --J) {
            auto &Prev = Blk.Ops[J];
            if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0 &&
                Prev.Output.RegOff == Reg) {
              if (!V) {
                V = Prev.Output;
                ReachingWriteAddr = Prev.Addr;
              } else if (ReachingWriteAddr != 0 &&
                         Prev.Addr == ReachingWriteAddr) {
                if (Prev.Output.Size > V->Size)
                  V = Prev.Output;
              } else {
                break;
              }
            }
            // SSE/intrinsic ops that pack a floating-point argument into an XMM
            // register (unpcklps / shufps / cvt*) are modeled as INTRINSIC, so
            // an FP argument set up through them sits *before* the INTRINSIC
            // that writes a later argument register — do not stop the scan
            // there. Only a real call clobbers the caller-saved XMM bank and
            // bounds it.
            if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL)
              break;
          }
          if (!V)
            for (const auto &Phi : Blk.Phis)
              if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == Reg) {
                V = Phi.Output;
                break;
              }
          if (!V)
            V = reachingFPArgCrossBlock(Reg);
          if (!V && IsTailReturn && KnownFPCallee && !funcDefinesReg(Reg)) {
            // Pure tail-call forwarder: this FP argument flows straight from
            // the function's incoming FP parameter register into the call, with
            // no write for the scans above to find.  Recover it as the live-in
            // and surface it as an FP parameter so the emitter forwards the
            // real incoming value rather than an uninitialised register (0.0).
            MedVar LiveIn;
            LiveIn.Kind = MedVar::Reg;
            LiveIn.RegOff = Reg;
            LiveIn.SSAVer = 0;
            LiveIn.Size = FwdFPArgSize;
            LiveIn.TheArch = TheArch;
            V = LiveIn;
            PromoteFPParams[K] = LiveIn;
          }
          if (!V)
            break; // FP arguments are consecutive from FP-argument 0
          FoundFP.push_back(*V);
        }
      }

      // Append FP/vector overflow stack arguments (the 9th+ floating-point
      // value, spilled past v0-v7 / xmm0-7) as scalar FP arguments in offset
      // order, so the emitter places them on the stack exactly as the callee
      // reads them (two AAPCS64-packed floats per 8-byte slot, doubles one per
      // slot).  Only when the FP argument registers are full (overflow is
      // possible) and the run is contiguous from the call SP -- a higher store
      // is a local spill, and an integer stack argument at offset 0 (mixed
      // int+FP overflow) leaves the run empty, falling back to the 8-byte-slot
      // model below.  The covered slots are cleared so that model does not also
      // emit them.
      if (CI.IsIndirect && !FPStackByOff.empty() && !TRI.FPParamRegs.empty() &&
          static_cast<int>(FoundFP.size()) ==
              static_cast<int>(TRI.FPParamRegs.size())) {
        const int NumRegArgSlots = static_cast<int>(TRI.IntParamRegs.size());
        const int SlotSize = TRI.PointerSize;
        int64_t Next = 0;
        for (auto &Entry : FPStackByOff) {
          if (Entry.first != Next)
            break; // gap ends the contiguous overflow run
          FoundFP.push_back(Entry.second);
          if (SlotSize > 0) {
            int Slot =
                NumRegArgSlots + static_cast<int>(Entry.first / SlotSize);
            if (Slot >= 0 && Slot < MaxArgs) {
              FoundMask[Slot] = false;
              Found[Slot] = MedVar();
              FromStackScan[Slot] = false;
            }
          }
          Next = Entry.first +
                 (Entry.second.Size > 0 ? Entry.second.Size : SlotSize);
        }
      }

      // Bound a direct call to a known non-variadic libc function to its true
      // arity.  The heuristic register/FP/stack scans over-collect arguments
      // for an external callee whose signature is unknown -- a dead incoming
      // parameter still resident in its register, a SIMD scratch register (a
      // string-init q-register picked up by the FP scan), or a spill near the
      // call -- and the emitter then declares the external callee variadic and
      // places those bogus overflow arguments on the stack (Darwin AArch64),
      // corrupting the caller frame.  A known libc arity drops the surplus
      // exactly.  (Variadic libc callees are handled by DarwinVarArgBase
      // above.) Gated to an import, relocation-named external, or call whose
      // arity is otherwise unknown, so an intra-module function that happens
      // to share a registered name keeps its recovered signature.
      if (UseExternalArity) {
        for (int K = std::max(0, ExternalArity->IntArgs); K < MaxArgs; ++K) {
          FoundMask[K] = false;
          Found[K] = MedVar();
          FromStackScan[K] = false;
        }
        if (static_cast<int>(FoundFP.size()) > ExternalArity->FpArgs)
          FoundFP.resize(std::max(0, ExternalArity->FpArgs));
      }

      // --- Assemble the argument list in callee parameter order ---
      // The callee parameter list is integer-register, then FP/vector (a
      // separate register class), then stack (the detectRegisterParams /
      // detectXMMParams / detectStackParams order), so the recovered arguments
      // follow suit.  When the callee takes floating-point arguments its
      // integer arity is fully determined (detectXMMParams has run), so cap the
      // integer arguments to it — otherwise a value merely live in a parameter
      // register (the caller's own incoming argument) would be handed to an
      // FP-only callee ahead of its FP arguments, misaligning them.  A
      // pure-integer callee is left uncapped: a forwarder's parameters are
      // promoted only during its own ABI recovery, so its arity is not yet
      // known here, and the emitter truncates any surplus to the real
      // signature.
      const int NumIntRegArgs = static_cast<int>(TRI.IntParamRegs.size());
      bool CalleeHasFP = false;
      if (!CI.IsIndirect && !IsRelocExtern && CalleeFPArity) {
        auto FIt = CalleeFPArity->find(CI.TargetAddr);
        CalleeHasFP = FIt != CalleeFPArity->end() && FIt->second > 0;
      }
      int IntCap =
          (CalleeHasFP && CalleeRegArgs >= 0) ? CalleeRegArgs : NumIntRegArgs;

      // ARM AAPCS passes a 64-bit argument in an even-odd register pair
      // (wasting the odd register before it) and 8-byte-aligns it on the stack
      // (padding the slot before it), so argument lanes are not contiguous: the
      // callee models every 4-byte lane as a parameter but never reads the
      // wasted / padding lanes.  Fill those interior gaps with zero up to the
      // callee's known arity instead of truncating the argument list at the
      // first gap, so the per-lane layout matches the callee on both sides.
      //
      // i386 cdecl leaves a *leading* gap only when clang constant-propagates
      // and drops an unused first argument (a variadic `vfn(3, &G..)` whose
      // count is folded into the callee never stores arg0), so the real stack
      // arguments sit at slot 1+ with slot 0 empty and the strict cutoff would
      // discard them all.  Restrict i386 gap-filling to exactly this shape —
      // slot 0 absent but a later slot present — so an ordinary i386 call (and
      // a forwarder, whose first slot is always present) keeps the strict
      // first-gap cutoff and its register/stack lane classification unchanged.
      bool I386LeadingGap = false;
      if (TheArch == Arch::X86 && !CI.IsIndirect && CalleeArgs >= 0 &&
          MaxArgs > 0 && !FoundMask[0])
        for (int K = 1; K < MaxArgs; ++K)
          if (FoundMask[K]) {
            I386LeadingGap = true;
            break;
          }
      const bool FillGaps = (TheArch == Arch::ARM || I386LeadingGap) &&
                            !CI.IsIndirect && CalleeArgs >= 0;
      // Assemble up to the highest recovered lane so interior gaps are filled
      // but no trailing argument is invented.  A reliable callee arity (>0; a
      // not-yet-promoted forwarder still reports 0 at this point) additionally
      // drops a stray lane recovered above the real argument list.
      int AssembleEnd = MaxArgs;
      if (FillGaps) {
        int LastFound = -1;
        for (int K = 0; K < MaxArgs; ++K)
          if (FoundMask[K])
            LastFound = K;
        AssembleEnd = LastFound + 1;
        if (CalleeArgs > 0 && CalleeArgs < AssembleEnd)
          AssembleEnd = CalleeArgs;
      }
      bool IndirectSretSetup = false;
      if (CI.IsIndirect && TheArch == Arch::AArch64) {
        uint64_t IRR = TRI.indirectResultReg();
        for (int J = static_cast<int>(OI) - 1; IRR != 0 && J >= 0; --J) {
          auto &Prev = Blk.Ops[J];
          if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL)
            break;
          if (Prev.Output.Kind == MedVar::Reg && Prev.Output.RegOff == IRR &&
              Prev.Output.Size > 0 &&
              derivesFromFrameReg(Blk, TRI, Prev.Output)) {
            IndirectSretSetup = true;
            break;
          }
        }
      }
      std::vector<MedVar> IntPart, StackPart;
      for (int K = 0; K < AssembleEnd; ++K) {
        const bool Gap = !FoundMask[K];
        if (Gap && !FillGaps) {
          // Indirect integer variadic (Darwin): stack arguments live in high
          // slots while the register prefix has interior gaps.  Pure-FP
          // indirect calls route overflow through FoundFP instead; do not skip
          // gaps there or spurious integer stack slots get assembled and
          // corrupt the arg list (ind-fp-stackarg regression).  Also do not
          // skip gaps once the call has been resolved to a known variadic
          // callee (DarwinVarArgBase
          // >= 0): its varargs are a CONTIGUOUS run from the fixed prefix, so
          // the first gap ends the run -- skipping past it would re-collect the
          // caller's own local function-pointer stores (at higher frame
          // offsets) as spurious trailing varargs and shift the callee's reads.
          if (CI.IsIndirect && FoundFP.empty() && !IndirectSretSetup &&
              DarwinVarArgBase < 0) {
            bool Higher = false;
            for (int H = K + 1; H < AssembleEnd; ++H)
              if (FoundMask[H] && FromStackScan[H]) {
                Higher = true;
                break;
              }
            if (Higher)
              continue;
          }
          break;
        }
        MedVar A =
            Gap ? MedVar::makeConst(0, static_cast<uint16_t>(TRI.PointerSize))
                : Found[K];
        // A slot found by the stack scan is a stack argument even when its
        // index falls in the integer-register range (i386 FP-register callee
        // with no integer register argument); assemble it after the FP
        // arguments.  A gap lane belongs to whichever range its index falls in.
        if (K < NumIntRegArgs && (Gap || !FromStackScan[K])) {
          if (K < IntCap)
            IntPart.push_back(A);
        } else {
          StackPart.push_back(A);
        }
        // A wide stack argument (an 8-byte double passed whole) covers several
        // pointer slots.  For an INDIRECT call the callee arity is unknown, so
        // SplitWide above leaves the wide store unsplit and its high slot a gap
        // that would otherwise truncate the rest of the argument list -- skip
        // the slots it occupies.  Restricted to indirect calls: a direct call
        // splits its wide stores into per-slot lanes (SplitWide), so a "wide"
        // found arg there is followed by genuine separate-argument slots that
        // must NOT be skipped.
        if (!Gap && CI.IsIndirect && Found[K].Size > TRI.PointerSize &&
            TRI.PointerSize > 0) {
          int Span = Found[K].Size / TRI.PointerSize;
          if (Span > 1)
            K += Span - 1;
        }
      }
      for (auto &A : IntPart)
        CI.Args.push_back(A);
      for (auto &A : FoundFP)
        CI.Args.push_back(A);
      for (auto &A : StackPart)
        CI.Args.push_back(A);

      // Darwin AArch64 indirect variadic: a function pointer call with a
      // partial register prefix and separate stack-scalar outgoing stores
      // (`fp(n, a, b, c, d)`).  Mark the fixed prefix so the emitter declares
      // a variadic signature and LLVM places the tail on the stack (Darwin
      // passes every variadic argument on the stack).  Reject a single wide
      // multi-slot stack blob (likely one by-value struct argument, not a
      // variadic tail).
      if (CI.IsIndirect && Img && Img->isMachO() && TheArch == Arch::AArch64 &&
          FoundFP.empty() && !IndirectSretSetup) {
        const int RegPrefix = static_cast<int>(IntPart.size());
        const int StackCnt = static_cast<int>(StackPart.size());
        if (RegPrefix > 0 && StackCnt > 0 &&
            RegPrefix < static_cast<int>(TRI.IntParamRegs.size())) {
          bool WideBlob = false;
          if (StackCnt == 1) {
            for (int K = 0; K < MaxArgs; ++K)
              if (FoundMask[K] && FromStackScan[K] &&
                  Found[K].Size > TRI.PointerSize)
                WideBlob = true;
          }
          if (!WideBlob)
            CI.VarArgFixedCount = RegPrefix;
        }
      }

      // AArch64 indirect-result (sret) pointer: a callee returning a by-value
      // aggregate too large for the return registers receives the result buffer
      // in x8.  Recover the buffer pointer set just before the call (add x8,
      // sp,#k) and append it as the trailing argument matching the callee's
      // hidden sret parameter (detectIndirectResultParam appends it last).
      //
      // Direct call: trust the callee's recorded sret flag.  Indirect call (a
      // function pointer returning a by-value aggregate, `struct R (*fp)();
      // fp()`): the callee is unknown so CalleeHasSret cannot be consulted --
      // detect the pattern structurally.  x8 is the ABI's reserved
      // indirect-result register, so an x8 set to a STACK buffer pointer (add
      // x8, sp/fp, #k) immediately before the call is the sret buffer; the
      // stackPtrDelta guard rejects an unrelated scratch x8.  NeverD rebuilds a
      // self-consistent ABI (the buffer is just a trailing pointer argument
      // both sides agree on), so appending it at the indirect call site matches
      // the callee's recovered sret parameter exactly as the direct path does
      // -- no x8-specific emitter support needed.
      bool DirectSret = false;
      if (!CI.IsIndirect && !IsRelocExtern && CalleeHasSret) {
        auto SIt = CalleeHasSret->find(CI.TargetAddr);
        DirectSret = SIt != CalleeHasSret->end() && SIt->second;
      }
      bool IndirectSret = CI.IsIndirect && TheArch == Arch::AArch64;
      if (DirectSret || IndirectSret) {
        uint64_t IRR = TRI.indirectResultReg();
        for (int J = static_cast<int>(OI) - 1; IRR != 0 && J >= 0; --J) {
          auto &Prev = Blk.Ops[J];
          if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL)
            break;
          if (Prev.Output.Kind == MedVar::Reg && Prev.Output.RegOff == IRR &&
              Prev.Output.Size > 0) {
            // Indirect call: the nearest x8 write is the only candidate --
            // require it to be a stack buffer pointer (an sret callee always
            // receives a caller-allocated stack slot, addressed off SP or the
            // frame pointer); otherwise it is scratch, not sret, so give up.  A
            // direct sret callee is already proven by CalleeHasSret.
            if (CI.IsIndirect && !derivesFromFrameReg(Blk, TRI, Prev.Output))
              break;
            // The sret buffer is the callee's LAST parameter (appended after
            // the ordinary register/stack arguments).  The register-argument
            // scan can over-recover a parameter register that is live at the
            // call but is NOT an argument of this callee -- a caller forwarding
            // its own `argv` (x1) into a one-argument sret callee, say:
            // findReachingArgReg returns x1 because it is a real parameter of
            // the *caller*, even though the callee takes only x0 plus the x8
            // buffer.  Appending the buffer after that spurious slot pushes it
            // past the callee's parameter count, and the emitter (which
            // truncates surplus arguments) then drops the buffer -- so the
            // callee reads the spurious value as its sret pointer and writes
            // the aggregate to a wild address.  Truncate to the callee's true
            // (non-sret) arity so the buffer lands exactly at the sret
            // parameter's index.
            if (CalleeArgs >= 0 &&
                static_cast<int>(CI.Args.size()) > CalleeArgs)
              CI.Args.resize(CalleeArgs);
            CI.Args.push_back(Prev.Output);
            break;
          }
        }
      }

      Func.CallInfos.push_back(std::move(CI));
      if (!CallLaneOps.empty())
        Pending.push_back(
            {Blk.Id, static_cast<int>(OI), std::move(CallLaneOps)});
    }
  }

  // Materialize the deferred wide-store lane splits: insert each call's
  // SUBBYTES ops just before it (highest index first so lower positions stay
  // valid) and shift the recorded index of that call and every later one.
  for (auto &Blk : Func.Blocks) {
    std::vector<LaneInsert *> Here;
    for (auto &P : Pending)
      if (P.BlockId == Blk.Id)
        Here.push_back(&P);
    std::sort(Here.begin(), Here.end(),
              [](const LaneInsert *A, const LaneInsert *B) {
                return A->BeforeOpIdx > B->BeforeOpIdx;
              });
    for (auto *P : Here) {
      Blk.Ops.insert(Blk.Ops.begin() + P->BeforeOpIdx, P->Ops.begin(),
                     P->Ops.end());
      int Delta = static_cast<int>(P->Ops.size());
      for (auto &CI : Func.CallInfos)
        if (CI.BlockId == Blk.Id && CI.OpIdx >= P->BeforeOpIdx)
          CI.OpIdx += Delta;
    }
  }

  // Surface tail-call-forwarded incoming stack arguments as parameters so the
  // function receives them and forwards them on.  Each slot index is the
  // argument position; appended after existing parameters (a register parameter
  // promoted below is inserted at the front, keeping positions aligned).  Done
  // before the register promotion so the final order is reg params then stack
  // params.
  if (!PromoteStackParams.empty()) {
    std::set<int> ExistingStack;
    for (const auto &P : Func.Params)
      if (P.RegOff == kNoParamReg && P.Kind == MedVar::Param)
        ExistingStack.insert(P.Id);
    for (int K : PromoteStackParams) {
      if (ExistingStack.count(K))
        continue;
      MedVar P;
      P.Kind = MedVar::Param;
      P.Id = K;
      P.RegOff = kNoParamReg;
      P.Size = static_cast<uint16_t>(TRI.PointerSize);
      P.TheArch = TheArch;
      Func.Params.push_back(P);
    }
  }

  // Surface forwarded incoming registers as parameters.  A pure forwarder
  // `f(a){return g(a);}` never reads its argument register (it flows straight
  // into the call), so detectCc's live-in scan misses it and Func.Params lacks
  // it.  The live-in fallback above recovered exactly those registers into
  // PromoteParams; surface them so the function signature preserves the
  // incoming value and the emitter resolves the call argument to the real
  // parameter rather than an uninitialised register (0).  Done only when no
  // integer register parameter was already detected, to leave a function whose
  // register parameters are known untouched.
  bool HasIntRegParam = false;
  for (const auto &P : Func.Params)
    if (P.RegOff != kNoParamReg && TRI.regToArgIdx(P.RegOff) >= 0) {
      HasIntRegParam = true;
      break;
    }
  if (!HasIntRegParam && !PromoteParams.empty()) {
    std::vector<MedVar> RegParams;
    for (int I = 0; I <= PromoteParams.rbegin()->first; ++I) {
      MedVar P;
      P.Kind = MedVar::Param;
      P.Id = -1;
      P.TheArch = TheArch;
      if (auto It = PromoteParams.find(I); It != PromoteParams.end()) {
        P.RegOff = It->second.RegOff;
        P.Size = It->second.Size > 0 ? It->second.Size
                                     : static_cast<uint16_t>(TRI.PointerSize);
      } else {
        P.RegOff = TRI.IntParamRegs[I];
        P.Size = TRI.FullRegWidth;
      }
      RegParams.push_back(P);
    }
    Func.Params.insert(Func.Params.begin(), RegParams.begin(), RegParams.end());
  }

  // Surface forwarded incoming FP/vector registers as parameters (the FP dual
  // of the integer promotion above): a pure forwarder `double f(double
  // a){return g(a);}` reads d0 only by passing it into the tail call, so
  // detectXMMParams misses it.  Append after the integer parameters (AAPCS /
  // SysV order: integer register class first, then the FP register class),
  // skipping any FP register already recovered as a parameter.
  if (!PromoteFPParams.empty()) {
    std::set<uint64_t> ExistingFP;
    for (const auto &P : Func.Params)
      if (P.RegOff != kNoParamReg)
        for (uint64_t FR : TRI.FPParamRegs)
          if (P.RegOff == FR) {
            ExistingFP.insert(P.RegOff);
            break;
          }
    std::vector<MedVar> FPParams;
    for (const auto &[K, V] : PromoteFPParams) {
      (void)K;
      if (ExistingFP.count(V.RegOff))
        continue;
      MedVar P;
      P.Kind = MedVar::Param;
      P.Id = -1;
      P.RegOff = V.RegOff;
      P.Size = V.Size > 0 ? V.Size : 8;
      P.TheArch = TheArch;
      FPParams.push_back(P);
    }
    Func.Params.insert(Func.Params.end(), FPParams.begin(), FPParams.end());
  }

  // Rewire a pure FP-returning forwarder's tail call: its result + the RETURN
  // that consumes it are on the integer return register (CFGBuilder::
  // rewriteAsTailCall hard-codes it), but the callee returns its value in the
  // FP return register.  Move both to the FP return register so the emitter
  // returns the real FP result instead of the stale incoming FP-arg register
  // left in V0/XMM0.  Done after argument recovery so the FP live-in fallback
  // above still sees the call's result register as undefined.  (Func.ReturnType
  // was already set to floating point by the Pipeline forwarder pass.)
  if (ForwarderFPRetSize) {
    for (auto &Blk : Func.Blocks) {
      if (Blk.Id != ForwarderFPRetBlk)
        continue;
      for (size_t I = 0; I < Blk.Ops.size(); ++I) {
        auto &O = Blk.Ops[I];
        if (O.Opcode != NdOp::CALL || O.Addr != ForwarderFPRetCallAddr ||
            O.Output.Kind != MedVar::Reg)
          continue;
        O.Output.RegOff = TRI.FPReturnReg;
        O.Output.Size = ForwarderFPRetSize;
        if (I + 1 < Blk.Ops.size() && Blk.Ops[I + 1].Opcode == NdOp::RETURN &&
            Blk.Ops[I + 1].NumInputs >= 1 &&
            Blk.Ops[I + 1].Inputs[0].Kind == MedVar::Reg) {
          Blk.Ops[I + 1].Inputs[0].RegOff = TRI.FPReturnReg;
          Blk.Ops[I + 1].Inputs[0].Size = ForwarderFPRetSize;
        }
        break;
      }
      break;
    }
  }
}

void finalizeVariadicCallees(std::vector<MedFunc> &Funcs, Arch TheArch,
                             BinaryFormat Fmt) {
  const auto &TRI = getTargetRegInfo(TheArch);
  // Darwin AArch64 passes EVERY variadic argument on the stack (no register
  // save area, no register-passed overflow): the callee's va_start points its
  // overflow pointer straight at the incoming-stack arguments and the va_arg
  // walk reads them in place.  This pass promotes a direct call's overflow
  // varargs to trailing fixed parameters; on Darwin LLVM then places them in
  // x1, x2, ... (fixed params before the `...` go in registers) and the callee
  // SPILLS those registers over the very stack slots the caller placed the
  // varargs in.  That is self-consistent for a function called ONLY directly
  // (every call site is co-rewritten to pass the overflow in registers), but is
  // NOT the real Darwin ABI -- so a variadic function whose ADDRESS IS TAKEN
  // (callable through a function pointer) breaks at the indirect call site,
  // which must follow the real ABI and passes every vararg on the stack.  For
  // an address-taken Darwin AArch64 variadic callee, skip the promotion: the
  // natural per-function recovery already yields the correct `(fixed.., ...)`
  // stack-vararg shape (the va_arg walk reads the incoming stack directly),
  // consistent for both direct and indirect calls.  Direct-only variadic
  // callees keep the promotion.
  std::set<va_t> AddressTakenVariadic;
  if (TheArch == Arch::AArch64 && Fmt == BinaryFormat::MachO) {
    std::set<va_t> FuncEntries;
    for (const auto &MF : Funcs)
      FuncEntries.insert(MF.Entry);
    for (const auto &MF : Funcs)
      for (const auto &Blk : MF.Blocks)
        for (const auto &O : Blk.Ops)
          for (uint8_t I = 0; I < O.NumInputs; ++I)
            if (O.Inputs[I].isConst() &&
                FuncEntries.count(O.Inputs[I].ConstVal)) {
              // A direct call names its target as Inputs[0]; any OTHER use of a
              // function's entry address as a constant takes its address.
              const bool IsDirectCallTarget = O.Opcode == NdOp::CALL && I == 0;
              if (!IsDirectCallTarget)
                AddressTakenVariadic.insert(O.Inputs[I].ConstVal);
            }
  }
  for (auto &Callee : Funcs) {
    if (!Callee.IsVariadic)
      continue;
    if (AddressTakenVariadic.count(Callee.Entry))
      continue;

    // The fixed-argument prefix already recovered before this pass.  On the
    // register ABIs a variadic prologue spills the whole parameter-register
    // file, recovered as ordinary register parameters; on i386 cdecl there is
    // no register save area, so the fixed prefix is the named *stack*
    // parameters (e.g. the format count) recovered by detectCdeclStackParams.
    // Every call's argument list is [fixed args .. overflow stack args], so the
    // count past this prefix is the overflow-argument count.
    int RegParamCount = 0;
    int MaxId = 0;
    for (const auto &P : Callee.Params) {
      if (P.RegOff != kNoParamReg && TRI.regToArgIdx(P.RegOff) >= 0)
        ++RegParamCount;
      if (P.Id > MaxId)
        MaxId = P.Id;
    }
    // i386 has no parameter registers: the named stack parameters are the fixed
    // prefix, so count them all (RegParamCount would be 0 and over-count the
    // overflow, declaring one too many trailing parameters).  On Darwin AArch64
    // a function with more than 8 named integer args also has NAMED stack
    // params before the overflow (VariadicFixedStackArgs, recovered by
    // detectCc); they are part of the fixed prefix, not varargs.
    const int FixedPrefix = TheArch == Arch::X86
                                ? static_cast<int>(Callee.Params.size())
                                : RegParamCount + Callee.VariadicFixedStackArgs;

    int K = 0;
    for (const auto &MF : Funcs)
      for (const auto &CI : MF.CallInfos)
        if (!CI.IsIndirect && CI.TargetAddr == Callee.Entry) {
          int Overflow = static_cast<int>(CI.Args.size()) - FixedPrefix;
          if (Overflow > K)
            K = Overflow;
        }
    if (K <= 0)
      continue; // every vararg fit in registers — the save area suffices

    if (K > limits::kMaxCallArgs)
      K = limits::kMaxCallArgs;

    // One trailing stack parameter per overflow argument.  Their value is never
    // read in the body (the va_arg walk reads memory); the emitter spills them
    // into the frame headroom, so only their position (after the register
    // parameters) and pointer width matter.
    for (int I = 0; I < K; ++I) {
      MedVar P;
      P.Kind = MedVar::Param;
      P.Id = ++MaxId;
      P.RegOff = kNoParamReg;
      P.Size = static_cast<uint16_t>(TRI.PointerSize);
      P.TheArch = TheArch;
      Callee.Params.push_back(P);
    }
    Callee.VariadicOverflowCount = K;

    // Pad every call to the callee's now-fixed arity so the non-variadic
    // declaration and each call agree (a shorter caller zero-fills the unread
    // trailing overflow slots).
    const int Arity = FixedPrefix + K;
    for (auto &MF : Funcs)
      for (auto &CI : MF.CallInfos)
        if (!CI.IsIndirect && CI.TargetAddr == Callee.Entry)
          while (static_cast<int>(CI.Args.size()) < Arity)
            CI.Args.push_back(
                MedVar::makeConst(0, static_cast<uint16_t>(TRI.PointerSize)));
  }
}

} // namespace neverd
