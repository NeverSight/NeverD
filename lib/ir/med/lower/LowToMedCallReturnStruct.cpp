//===- LowToMedCallReturnStruct.cpp - Struct call returns ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Models small struct call returns carried across multiple registers.
///
//===----------------------------------------------------------------------===//

#include "LowToMedCallReturnDetail.h"

#include "neverd/ir/CallRegisterContract.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

namespace {

bool sameValue(const MedVar &A, const MedVar &B) {
  return A.Kind == B.Kind && A.Id == B.Id && A.SSAVer == B.SSAVer &&
         A.RegOff == B.RegOff && A.Size == B.Size;
}

} // namespace

bool lowtomed_detail::keepsUpperBytesOfWrite(const std::vector<MedOp> &Ops,
                                             size_t At) {
  const MedOp &Keep = Ops[At];
  // The merge keeps the register's bytes above the ones written: masked
  // (`t = R & ~0xff; u = t | v`), or sliced off and joined above them
  // (`t = R[1:]; u = concat(t, v)`), as an 8-bit write lowers to.
  const bool Masked =
      Keep.Opcode == NdOp::INT_AND && Keep.NumInputs == 2 &&
      Keep.Inputs[0].Kind == MedVar::Reg && Keep.Inputs[1].isConst() &&
      (Keep.Inputs[1].ConstVal & 0xFF) == 0 && Keep.Inputs[1].ConstVal != 0;
  const bool Sliced = Keep.Opcode == NdOp::SUBBYTES && Keep.NumInputs == 2 &&
                      Keep.Inputs[0].Kind == MedVar::Reg &&
                      Keep.Inputs[1].isConst() && Keep.Inputs[1].ConstVal > 0;
  if (!Masked && !Sliced)
    return false;
  const uint64_t Register = Keep.Inputs[0].RegOff;
  for (size_t K = At + 1;
       K < Ops.size() &&
       K <= At + ::neverd::limits::ir::kLowToMedCallReturnStructWindow;
       ++K) {
    const MedOp &Merge = Ops[K];
    if (Merge.NumInputs != 2 ||
        (Masked ? Merge.Opcode != NdOp::INT_OR ||
                      (!sameValue(Merge.Inputs[0], Keep.Output) &&
                       !sameValue(Merge.Inputs[1], Keep.Output))
                : Merge.Opcode != NdOp::CONCAT ||
                      !sameValue(Merge.Inputs[0], Keep.Output)))
      continue;
    if (Merge.Output.Kind == MedVar::Reg && Merge.Output.RegOff == Register)
      return true;
    // Joined back to the register's whole width, the slice is the register
    // with its low bytes replaced, wherever copy propagation took the value.
    if (Sliced && Merge.Output.Size == Keep.Inputs[0].Size)
      return true;
    // So is the mask's merge with a value no wider than the bytes the mask
    // clears: `setg r9b; movzx r9d, r9b` reads only the merge's low byte, and
    // the copy back into R9 is gone.
    if (Masked && Merge.Output.Size == Keep.Inputs[0].Size) {
      const uint64_t Mask = Keep.Inputs[1].ConstVal;
      unsigned Cleared = 0;
      while (Cleared < 8 && ((Mask >> (8 * Cleared)) & 0xFF) == 0)
        ++Cleared;
      const bool Contiguous =
          Cleared < 8 &&
          (Mask | ((uint64_t(1) << (8 * Cleared)) - 1)) ==
              (Keep.Inputs[0].Size >= 8
                   ? ~uint64_t(0)
                   : (uint64_t(1) << (8 * Keep.Inputs[0].Size)) - 1);
      const MedVar &Value = sameValue(Merge.Inputs[0], Keep.Output)
                                ? Merge.Inputs[1]
                                : Merge.Inputs[0];
      bool Narrow = Value.isConst() && Cleared < 8 &&
                    (Value.ConstVal >> (8 * Cleared)) == 0;
      for (size_t D = K;
           !Narrow && D-- > 0 &&
           K - D <= ::neverd::limits::ir::kLowToMedCallReturnStructWindow;)
        if (sameValue(Ops[D].Output, Value)) {
          Narrow = Ops[D].Opcode == NdOp::INT_ZEXT && Ops[D].NumInputs >= 1 &&
                   Ops[D].Inputs[0].Size <= Cleared;
          break;
        }
      if (Contiguous && Narrow)
        return true;
    }
    for (size_t M = K + 1;
         M < Ops.size() &&
         M <= K + ::neverd::limits::ir::kLowToMedCallReturnStructWindow;
         ++M)
      if (Ops[M].Opcode == NdOp::COPY && Ops[M].NumInputs >= 1 &&
          Ops[M].Output.Kind == MedVar::Reg &&
          Ops[M].Output.RegOff == Register &&
          sameValue(Ops[M].Inputs[0], Merge.Output))
        return true;
    return false;
  }
  return false;
}

namespace {
using lowtomed_detail::keepsUpperBytesOfWrite;
} // namespace

void LowToMedConverter::modelCallStructReturn(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);
  // 64-bit targets only: x86-64 SysV and AArch64 return a small struct by value
  // in registers.  32-bit targets return such aggregates through the sret
  // pointer or the wide-int register pair (handled by modelCallWideIntReturn).
  if (TRI.PointerSize != 8)
    return;
  // Where a call returns one register value at most, reading another
  // volatile result register after it does not establish a second source
  // result. Explicit source contracts are already bound separately, before
  // this heuristic inference.
  if (const CallArgumentConvention *Convention =
          callArgumentConvention(TargetArch, TargetFormat);
      Convention && Convention->SingleRegisterReturns)
    return;
  if (TRI.IntReturnRegs.empty() && TRI.FPReturnRegs.empty())
    return;
  llvm::DenseSet<std::tuple<uint32_t, unsigned, int, int>> ClobberIdentities;
  for (const auto &Clobber : Func.CallClobbers)
    ClobberIdentities.insert({Clobber.CallSiteId, Clobber.Value.Kind,
                              Clobber.Value.Id, Clobber.Value.SSAVer});
  const auto isCallClobber = [&](const MedVar &V, uint32_t CallSiteId) {
    return ClobberIdentities.contains({CallSiteId, V.Kind, V.Id, V.SSAVer});
  };

  // Candidate return registers in canonical aggregate-field order: the integer
  // pair first (RAX/RDX, X0/X1), then the FP/vector registers (XMM0/XMM1,
  // V0..V3).  A field's position here is the order the LLVM struct return type
  // is built in, which the backend's ABI lowering maps back to the registers.
  struct Cand {
    uint64_t RegOff;
    bool IsFP;
  };
  std::vector<Cand> Cands;
  for (uint64_t R : TRI.IntReturnRegs)
    Cands.push_back({R, false});
  for (uint64_t R : TRI.FPReturnRegs)
    Cands.push_back({R, true});
  auto candIdx = [&](uint64_t RegOff) -> int {
    for (size_t K = 0; K < Cands.size(); ++K)
      if (Cands[K].RegOff == RegOff)
        return static_cast<int>(K);
    return 1000;
  };
  // A SUBBYTES narrowing a register's own value (input and output share the
  // register offset) is a sub-register VIEW, such as the EAX the lowering
  // derives from every RAX a call defines.  It forwards the value unchanged:
  // it neither consumes nor redefines the register.
  auto isSelfView = [](const MedOp &Op) {
    return Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 1 &&
           Op.Inputs[0].Kind == MedVar::Reg &&
           Op.Inputs[0].RegOff == Op.Output.RegOff;
  };

  // Exception ranges can end the call's block before its result is read.
  // SSA identifies that result across such splits without guessing from a
  // same-numbered register in a later block. Index genuine input reads once;
  // these only propose a direct register return, still subject to the
  // complete callee-body proof in the pipeline.
  const auto Identity = [](const MedVar &V) {
    return std::tuple{V.Kind, V.Id, V.SSAVer};
  };
  using ValueKey = decltype(Identity(MedVar{}));
  llvm::DenseMap<ValueKey, uint16_t> ReadWidths;
  std::map<uint32_t, std::vector<MedVar>> ResultClobbers;
  if (TargetArch == Arch::AArch64) {
    const auto ResultRegister = [&](uint64_t Offset) {
      return (TRI.IntReturnRegs.size() == 2 &&
              Offset == TRI.IntReturnRegs[1]) ||
             std::find(TRI.FPReturnRegs.begin(), TRI.FPReturnRegs.end(),
                       Offset) != TRI.FPReturnRegs.end();
    };
    for (const auto &Clobber : Func.CallClobbers)
      if (Clobber.Value.Kind == MedVar::Reg &&
          ResultRegister(Clobber.Value.RegOff))
        ResultClobbers[Clobber.CallSiteId].push_back(Clobber.Value);
    if (!ResultClobbers.empty()) {
      llvm::DenseMap<ValueKey, const PhiNode *> PhiDefinitions;
      llvm::SmallVector<ValueKey, 16> Work;
      const auto Observe = [&](const MedVar &Value, uint16_t Bytes) {
        if (Value.Kind != MedVar::Reg || !ResultRegister(Value.RegOff))
          return;
        const ValueKey Key = Identity(Value);
        Bytes = std::min(Bytes, Value.Size);
        auto &Width = ReadWidths[Key];
        if (Bytes > Width) {
          Width = Bytes;
          Work.push_back(Key);
        }
      };
      for (const MedBlock &Block : Func.Blocks) {
        for (const PhiNode &Phi : Block.Phis)
          if (Phi.Output.Kind == MedVar::Reg &&
              ResultRegister(Phi.Output.RegOff))
            PhiDefinitions.try_emplace(Identity(Phi.Output), &Phi);
        for (size_t I = 0; I < Block.Ops.size(); ++I) {
          const MedOp &Read = Block.Ops[I];
          if (isSelfView(Read) || keepsUpperBytesOfWrite(Block.Ops, I) ||
              ((Read.Opcode == NdOp::INT_XOR || Read.Opcode == NdOp::INT_SUB) &&
               Read.NumInputs == 2 &&
               sameValue(Read.Inputs[0], Read.Inputs[1])))
            continue;
          for (uint8_t J = 0; J < Read.NumInputs; ++J)
            Observe(Read.Inputs[J], Read.Inputs[J].Size);
        }
      }
      // A PHI transports an observation to its incoming SSA values; an unread
      // PHI is not itself a read. Propagate growing widths through the indexed
      // graph, including chains and loops, without rescanning per call site.
      while (!Work.empty()) {
        const ValueKey Key = Work.pop_back_val();
        const auto Found = PhiDefinitions.find(Key);
        if (Found == PhiDefinitions.end())
          continue;
        const PhiNode &Phi = *Found->second;
        const uint16_t Width = std::min(ReadWidths[Key], Phi.Output.Size);
        for (const auto &[Pred, Input] : Phi.Args)
          Observe(Input, Width);
      }
    }
  }

  int MaxVer = 0, MaxId = 0;
  auto bump = [&](const MedVar &V) {
    MaxVer = std::max(MaxVer, V.SSAVer);
    MaxId = std::max(MaxId, V.Id);
  };
  for (const auto &B : Func.Blocks) {
    for (const auto &Op : B.Ops) {
      bump(Op.Output);
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        bump(Op.Inputs[I]);
    }
    for (const auto &Phi : B.Phis) {
      bump(Phi.Output);
      for (const auto &A : Phi.Args)
        bump(A.second);
    }
  }
  int NextVer = MaxVer + 1, NextId = MaxId + 1;

  for (auto &Blk : Func.Blocks) {
    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      // A direct or indirect call whose result is consumed across multiple
      // return registers.  The CALL-SITE remodel is identical for both: the
      // caller's straight-line reads of x0:x1 (or v0:v1) prove the
      // multi-register return regardless of how the target is reached.  A
      // direct call's callee is re-typed via the Pipeline (resolvable in-module
      // target); an indirect call's unknown callee is re-typed from its own
      // body (recoverMultiReturnFromBody), so this remodel only rewrites the
      // call site.
      if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
          Op.NumInputs < 1 || Op.SourceCallHint)
        continue;
      // A call already remodeled (FP/x87/wide produce a non-register output) is
      // left alone; the default-modeled call writes the integer return
      // register.
      // The direct callee may preserve the default integer return register
      // while its callers consume an HFA in the floating return bank.
      const bool NoPrimaryOutput =
          TargetArch == Arch::AArch64 && Op.Opcode == NdOp::CALL &&
          Op.Inputs[0].isConst() && Op.Output.Size == 0;
      if (!NoPrimaryOutput &&
          (Op.Output.Kind != MedVar::Reg || TRI.isX87StackReg(Op.Output.RegOff)))
        continue;

      // A loader-bound scalar libm result cannot become a mixed aggregate
      // merely because RET still carries the lifter's default RAX dependency.
      // Local functions with the same spelling remain subject to the ordinary
      // field proof; only an exact executable import stub supplies this ABI.
      if (Image && Op.Opcode == NdOp::CALL && Op.Inputs[0].isConst())
        if (const Import *Imp = Image->findImportStubAt(Op.Inputs[0].ConstVal))
          if (const auto Signature = libc::libcArityForSymbol(
                  Imp->Name, libc::libcVariantOf(*Image));
              Signature && libc::floatReturnBytes(*Signature))
            continue;
      // Nor does an import whose C declaration returns nothing or a pointer.
      // After `call setlocale`, GCC pads the next call's stack arguments with
      // `push %rdx`: a read of what setlocale left in RDX, no second field.
      if (Image && Op.Opcode == NdOp::CALL && Op.Inputs[0].isConst() &&
          importDeclaresNoRecord(*Image, Op.Inputs[0].ConstVal))
        continue;

      // Which candidate return registers does the caller read straight-line
      // after the call (a genuine input) before that register is redefined?
      // A register the callee provably never writes still holds the caller's
      // own value (GCC keeps one in RDX across a call it knows leaves RDX
      // alone), so it returns nothing.
      auto PreservedByCall = [&](const MedVar &V) {
        return callPreservesRegisterBytes(TargetArch, Op.CallPreservedGPRs,
                                          V.RegOff, V.Size);
      };
      struct FieldRead {
        uint64_t RegOff;
        bool IsFP;
        uint16_t Size;
        MedVar Value;
      };
      const bool ConstantCallee =
          Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst();
      const GPRReadWidths *CalleeReads = nullptr;
      if (ConstantCallee && CallEntryReadGPRs)
        if (const auto R = CallEntryReadGPRs->find(Op.Inputs[0].ConstVal);
            R != CallEntryReadGPRs->end())
          CalleeReads = &R->second;
      const bool GoCall =
          Image &&
          (Op.RegisterContract == CallRegisterContract::GoCode ||
           Op.RegisterContract == CallRegisterContract::GoInternal) &&
          goCallEntersInternalABI(
              *Image, Func.Entry,
              ConstantCallee ? Op.Inputs[0].ConstVal : InvalidVA, CalleeReads);
      // Go's internal ABI decides what a call into Go code returns, and
      // materializeGoCallResults gives every call of a callee the same
      // record from all of its callers' reads.  A record this block's
      // reads alone suggest would disagree with that one: the return's
      // default RAX would make one of floats alone hold RAX too.
      if (GoCall)
        continue;
      std::vector<FieldRead> Fields;
      std::set<uint64_t> Redefined;
      for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL)
          break;
        // Only a later read of a view consumes the value it forwards, and that
        // read names the same register.
        if (isSelfView(Nx))
          continue;
        // A self-zeroing idiom (`xorps x,x` / `sub x,x`) reads its register
        // only to discard it — the result is 0, independent of the value — so
        // it redefines the register rather than consuming the call's result. An
        // int call followed by the caller zeroing XMM0 for its own FP scratch
        // must not make XMM0 look like a second struct-return field (#469).
        bool SelfZero =
            (Nx.Opcode == NdOp::INT_XOR || Nx.Opcode == NdOp::INT_SUB) &&
            Nx.NumInputs == 2 && Nx.Inputs[0].Kind == MedVar::Reg &&
            Nx.Inputs[1].Kind == MedVar::Reg &&
            Nx.Inputs[0].RegOff == Nx.Inputs[1].RegOff &&
            Nx.Inputs[0].Id == Nx.Inputs[1].Id &&
            Nx.Inputs[0].SSAVer == Nx.Inputs[1].SSAVer;
        // So does a write of the register's low bytes (`sete dl` after an int
        // call): `t = rdx & ~0xff; rdx = t | dl` reads the register only to
        // keep the bytes it does not write, and a caller that overwrites its
        // low bytes reads no field there.
        if (!SelfZero && keepsUpperBytesOfWrite(Blk.Ops, J))
          continue;
        if (!SelfZero)
          for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
            const auto &In = Nx.Inputs[I];
            if (In.Kind != MedVar::Reg)
              continue;
            int CI = candIdx(In.RegOff);
            if (CI >= 1000 || Redefined.count(In.RegOff) || PreservedByCall(In))
              continue;
            // The field holds every byte the caller reads from it: `mov ecx,
            // eax` followed by `shr rax, 32` needs all of RAX.
            auto Known = std::find_if(
                Fields.begin(), Fields.end(),
                [&](const FieldRead &F) { return F.RegOff == In.RegOff; });
            if (Known != Fields.end())
              Known->Size = std::max(Known->Size, In.Size);
            else
              Fields.push_back({In.RegOff, Cands[CI].IsFP, In.Size, In});
          }
        if (Nx.Output.Kind == MedVar::Reg && candIdx(Nx.Output.RegOff) < 1000)
          Redefined.insert(Nx.Output.RegOff);
      }
      // AArch64 __int128 returned in the X0:X1 GP pair: clang -O0 moves it into
      // V0 for storage (`mov v0.d[0],x0; mov v0.d[1],x1; str q0`).  The lifter
      // models each lane insert as reading the old V0 (to preserve the
      // untouched lane) and writing V0 back, so the post-call scan above
      // records a stale V0 read as an FP field — making the pure-GP pair look
      // like a mixed int+FP shape that the AArch64 reject below would drop,
      // losing the real X0:X1 return.  Distinguish this re-housing from a
      // genuine returned HFA field by data flow: an FP register that is WRITTEN
      // by a value assembled from an integer return register (`mov v0.d[k],
      // xN`) is not an independent field — the X0:X1 pair is.  A real HFA
      // field, even when clang spills and reloads it (`str d1; ldr
      // d1,[frame]`), is rewritten only from memory (an untainted LOAD), never
      // from a return register.  Compute the values tainted by an integer
      // return register across the straight-line region, then drop any FP field
      // whose register is written by a tainted value. Only where aggregates
      // return in a single register class: x86-64 SysV genuinely returns mixed
      // int+SSE aggregates and does not use this GP→vector idiom.
      if (TRI.ReturnAggregatesSingleClass) {
        auto isIntCandReg = [&](const MedVar &V) {
          if (V.Kind != MedVar::Reg)
            return false;
          int CI = candIdx(V.RegOff);
          return CI < 1000 && !Cands[CI].IsFP;
        };
        // (Id, SSAVer) of values derived from an integer return register.  A
        // single forward pass over the straight-line region suffices: the
        // lifter emits each value's definition before its uses.
        std::set<std::pair<int, int>> Tainted;
        std::set<uint64_t> Assembled; // FP regs written from a tainted value
        for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
          auto &Nx = Blk.Ops[J];
          if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL)
            break;
          bool InTaint = false;
          for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
            const auto &In = Nx.Inputs[I];
            if (isIntCandReg(In) ||
                (In.Kind != MedVar::Const && Tainted.count({In.Id, In.SSAVer})))
              InTaint = true;
          }
          if (!InTaint)
            continue;
          if (Nx.Output.Kind == MedVar::Reg || Nx.Output.Kind == MedVar::Temp ||
              Nx.Output.Kind == MedVar::Stack) {
            Tainted.insert({Nx.Output.Id, Nx.Output.SSAVer});
            if (Nx.Output.Kind == MedVar::Reg &&
                candIdx(Nx.Output.RegOff) < 1000)
              Assembled.insert(Nx.Output.RegOff);
          }
        }
        Fields.erase(std::remove_if(Fields.begin(), Fields.end(),
                                    [&](const FieldRead &F) {
                                      return F.IsFP &&
                                             Assembled.count(F.RegOff);
                                    }),
                     Fields.end());
      }
      // A compiler runtime routine declared to return a double word returns
      // both halves of the integer pair, whichever of them its caller reads:
      // `(n / d) >> 64` reads only RDX after `call __udivti3`.
      if (Image && Op.Inputs[0].isConst() && TRI.IntReturnReg2 &&
          importReturnsDoubleWord(*Image, Op.Inputs[0].ConstVal,
                                  TRI.PointerSize))
        for (const uint64_t Register : {TRI.IntReturnReg, TRI.IntReturnReg2}) {
          auto Known = std::find_if(
              Fields.begin(), Fields.end(),
              [&](const FieldRead &F) { return F.RegOff == Register; });
          if (Known == Fields.end())
            Fields.push_back({Register, false, TRI.PointerSize, MedVar{}});
          else
            Known->Size = std::max<uint16_t>(Known->Size, TRI.PointerSize);
        }
      if (Op.Opcode == NdOp::CALL)
        if (const auto Clobbers = ResultClobbers.find(Op.CallSiteId);
            Clobbers != ResultClobbers.end())
          for (const MedVar &Value : Clobbers->second)
            if (const auto Read = ReadWidths.find(Identity(Value));
                Read != ReadWidths.end() &&
                std::none_of(Fields.begin(), Fields.end(), [&](const auto &F) {
                  return F.RegOff == Value.RegOff;
                }))
              Fields.push_back({Value.RegOff, TRI.isVectorReg(Value.RegOff),
                                Read->second, Value});
      // Reading just the second word still proposes an integer pair. Keep
      // this as a candidate: only the direct callee's complete return proof
      // can establish the otherwise unobserved first word.
      bool HasUnobservedFields = false;
      if (TargetArch == Arch::AArch64 && Op.Opcode == NdOp::CALL &&
          TRI.IntReturnRegs.size() == 2 && Fields.size() == 1 &&
          !Fields.front().IsFP &&
          Fields.front().RegOff == TRI.IntReturnRegs[1] &&
          isCallClobber(Fields.front().Value, Op.CallSiteId)) {
        Fields.push_back({TRI.IntReturnReg, false, TRI.PointerSize, Op.Output});
        HasUnobservedFields = true;
      }
      // A read of S1/D1 (or a later HFA field) can cross an exception block
      // boundary and need not read the first result. Propose the contiguous
      // homogeneous prefix only for a direct call. The pipeline must prove
      // every proposed field against the complete callee before binding it.
      if (TargetArch == Arch::AArch64 && Op.Opcode == NdOp::CALL &&
          !Fields.empty() &&
          std::all_of(Fields.begin(), Fields.end(), [&](const auto &F) {
            return F.IsFP && (F.Size == 4 || F.Size == 8) &&
                   F.Size == Fields.front().Size &&
                   isCallClobber(F.Value, Op.CallSiteId);
          })) {
        size_t Last = 0;
        for (const auto &Field : Fields)
          Last = std::max(Last,
                          static_cast<size_t>(
                              std::find(TRI.FPReturnRegs.begin(),
                                        TRI.FPReturnRegs.end(), Field.RegOff) -
                              TRI.FPReturnRegs.begin()));
        if (Last > 0 && Last < TRI.FPReturnRegs.size())
          for (size_t I = 0; I <= Last; ++I) {
            const uint64_t Register = TRI.FPReturnRegs[I];
            if (std::any_of(Fields.begin(), Fields.end(), [&](const auto &F) {
                  return F.RegOff == Register;
                }))
              continue;
            MedVar Proposed;
            Proposed.Kind = MedVar::Reg;
            Proposed.TheArch = TargetArch;
            Proposed.RegOff = Register;
            Proposed.Size = Fields.front().Size;
            Proposed.Id = NextId++;
            Proposed.SSAVer = NextVer++;
            Fields.push_back({Register, true, Proposed.Size, Proposed});
            HasUnobservedFields = true;
          }
      }
      if (Fields.size() < 2)
        continue;
      bool AnyFP = false, AnyInt = false;
      for (auto &F : Fields) {
        if (F.IsFP)
          AnyFP = true;
        else
          AnyInt = true;
      }
      // x86-64 all-integer pair {RAX,RDX} is a 2-eightbyte struct return
      // (`struct{int*;int}`).  The div/mul byproduct concern does not apply to
      // a genuine field read: a divide's `cqo` WRITES RDX (a redefine that
      // drops it from Fields) before any read, so an un-redefined RDX read
      // straight after the call is the second eightbyte.  AArch64's {X0,X1} is
      // likewise safe. AArch64 returns a small struct either entirely in GP
      // registers (a non-HFA aggregate -> X0,X1) or entirely in V registers (an
      // HFA -> V0..V3) — never mixed.  A mixed int+FP read after the call is
      // therefore NOT a struct return: it is a plain FP-returning callee whose
      // lifted call also carries the lifter's dead integer-return-register
      // placeholder. Rejecting the mixed shape keeps such single-FP returns
      // (incl. recursive FP callees) on the scalar path.
      if (TRI.ReturnAggregatesSingleClass && AnyFP && AnyInt)
        continue;

      // Canonical field order (int regs then fp regs).
      std::sort(Fields.begin(), Fields.end(),
                [&](const FieldRead &A, const FieldRead &B) {
                  return candIdx(A.RegOff) < candIdx(B.RegOff);
                });

      // Field sizes: an x86-64 SSE eightbyte is always 8 bytes (XMM0 may pack
      // two floats), so force FP fields to 8 there; AArch64 HFA elements keep
      // their natural width (S=4, D=8).  Integer fields take the read width
      // (a full GPR slot is 8).
      uint16_t Total = 0;
      for (auto &F : Fields) {
        uint16_t Sz;
        if (F.IsFP)
          Sz = TRI.VectorReturnFieldsAreEightbytes ? 8 : (F.Size <= 4 ? 4 : 8);
        else
          Sz = F.Size >= 8 ? 8 : (F.Size >= 4 ? 4 : (F.Size ? F.Size : 8));
        F.Size = Sz;
        Total += Sz;
      }

      bool HasImplicitField =
          std::any_of(Fields.begin(), Fields.end(), [&](const FieldRead &F) {
            return isCallClobber(F.Value, Op.CallSiteId);
          });
      // With no primary output, only a complete observation of this call's
      // own floating clobbers can propose a shape. The callee must still
      // prove all returned fields before the pipeline can bind any of them.
      if (NoPrimaryOutput &&
          (AnyInt || HasUnobservedFields ||
           !llvm::all_of(Fields, [&](const auto &Field) {
             return isCallClobber(Field.Value, Op.CallSiteId);
           })))
        continue;
      if (TRI.ReturnAggregatesSingleClass && Op.Opcode == NdOp::CALL &&
          HasImplicitField) {
        MedStructReturnCandidate Candidate;
        Candidate.CallSiteId = Op.CallSiteId;
        Candidate.HasUnobservedFields = HasUnobservedFields;
        for (const FieldRead &F : Fields) {
          MedVar Field = F.Value;
          Field.Size = F.Size;
          Candidate.Fields.push_back(Field);
        }
        Func.StructReturnCandidates.push_back(std::move(Candidate));
        continue;
      }

      // Remodel: the call produces a flat wide-integer temp; SUBBYTES each
      // field out into its return register at the packed byte offset.
      MedVar OrigOut = Op.Output;
      MedVar Tv;
      Tv.Kind = MedVar::Temp;
      Tv.TheArch = TargetArch;
      Tv.Id = NextId++;
      Tv.SSAVer = 0;
      Tv.Size = Total;
      Op.Output = Tv;
      auto ReturnFields = std::make_shared<std::vector<MedReturnReg>>();
      for (const auto &Field : Fields)
        ReturnFields->push_back({Field.RegOff, Field.Size, Field.IsFP});
      Op.RegisterReturnFields = std::move(ReturnFields);

      std::vector<MedOp> Extracts;
      std::vector<std::pair<uint64_t, std::pair<int, int>>> NewVers;
      uint16_t Cum = 0;
      for (auto &F : Fields) {
        int Id = (F.RegOff == OrigOut.RegOff) ? OrigOut.Id : NextId++;
        int Ver = NextVer++;
        MedVar Out;
        Out.Kind = MedVar::Reg;
        Out.TheArch = TargetArch;
        Out.RegOff = F.RegOff;
        Out.Id = Id;
        Out.SSAVer = Ver;
        Out.Size = F.Size;
        MedOp Ex;
        Ex.Opcode = NdOp::SUBBYTES;
        Ex.Addr = Op.Addr;
        Ex.Output = Out;
        Ex.addInput(Tv);
        Ex.addInput(MedVar::makeConst(Cum, TRI.PointerSize));
        Extracts.push_back(Ex);
        NewVers.push_back({F.RegOff, {Id, Ver}});
        Cum += F.Size;
      }
      Blk.Ops.insert(Blk.Ops.begin() + OI + 1, Extracts.begin(),
                     Extracts.end());

      // Rewire straight-line reads of each field register to its extract
      // version until that register is redefined (before copy propagation, so a
      // plain by-RegOff match is exact).
      std::set<uint64_t> Done;
      for (size_t J = OI + 1 + Extracts.size();
           J < Blk.Ops.size() && Done.size() < Fields.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL)
          break;
        for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
          auto &In = Nx.Inputs[I];
          if (In.Kind != MedVar::Reg)
            continue;
          for (auto &NV : NewVers)
            if (In.RegOff == NV.first && !Done.count(NV.first)) {
              In.Id = NV.second.first;
              In.SSAVer = NV.second.second;
            }
        }
        // A view of the extract is not a redefinition; marking it Done would
        // strand a later full-width read of the same field (e.g. a pointer
        // field reused as a load base after its low half is viewed) — the base
        // would keep its stale pre-call version (#470-style sub-register view
        // handling).
        if (Nx.Output.Kind == MedVar::Reg && !isSelfView(Nx))
          for (auto &NV : NewVers)
            if (Nx.Output.RegOff == NV.first)
              Done.insert(NV.first);
      }

      // Cross-block straight-line reads: a field still live at the call block's
      // end reaches reads in successor blocks the call dominates (e.g. a
      // returned field consumed in only one arm of `if (cond)`), which the
      // same-block scan above never visits.  Without rewiring them the read
      // resolves to the stale pre-call register value (the call's own argument)
      // — mirrors the cross- block handling in modelCallFPReturn.  Bounded to
      // single-predecessor successors so the call block provably dominates
      // them; a field carried through a PHI is handled by the PHI-arg update
      // below.
      for (auto &NV : NewVers) {
        if (Done.count(NV.first))
          continue;
        std::set<int> Seen{Blk.Id};
        std::vector<int> Work(Blk.Succs.begin(), Blk.Succs.end());
        while (!Work.empty()) {
          int BId = Work.back();
          Work.pop_back();
          if (!Seen.insert(BId).second)
            continue;
          MedBlock *SB = nullptr;
          for (auto &B : Func.Blocks)
            if (B.Id == BId) {
              SB = &B;
              break;
            }
          if (!SB || SB->Preds.size() != 1)
            continue; // not dominated solely by the call chain
          bool HasFieldPhi = false;
          for (const auto &Phi : SB->Phis)
            if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == NV.first)
              HasFieldPhi = true;
          if (HasFieldPhi)
            continue; // the PHI (updated below) carries the value
          bool Redef = false;
          for (auto &Nx : SB->Ops) {
            for (uint8_t I = 0; I < Nx.NumInputs; ++I)
              if (Nx.Inputs[I].Kind == MedVar::Reg &&
                  Nx.Inputs[I].RegOff == NV.first) {
                Nx.Inputs[I].Id = NV.second.first;
                Nx.Inputs[I].SSAVer = NV.second.second;
              }
            if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == NV.first &&
                !isSelfView(Nx)) {
              Redef = true;
              break;
            }
          }
          if (!Redef)
            Work.insert(Work.end(), SB->Succs.begin(), SB->Succs.end());
        }
      }

      // Loop-carried / cross-edge result: update successor PHI arguments to the
      // call's field version — but only for a field register that is NOT
      // redefined after the extract in this block.  If it is redefined (e.g. a
      // loop accumulator `acc += r.x` whose new value becomes the carried
      // value, or is spilled and reloaded), that later definition is the
      // loop-carried value; overriding the PHI arg to the call's field here
      // would drop the accumulation (mirrors modelCallFPReturn / #469).  `Done`
      // holds exactly the field registers redefined after the extracts above.
      for (int Succ : Blk.Succs)
        for (auto &B : Func.Blocks) {
          if (B.Id != Succ)
            continue;
          for (auto &Phi : B.Phis)
            for (auto &A : Phi.Args)
              if (A.first == Blk.Id && A.second.Kind == MedVar::Reg)
                for (auto &NV : NewVers)
                  if (A.second.RegOff == NV.first && !Done.count(NV.first)) {
                    A.second.Id = NV.second.first;
                    A.second.SSAVer = NV.second.second;
                  }
        }
      OI += Extracts.size();
    }
  }
}

} // namespace neverd
