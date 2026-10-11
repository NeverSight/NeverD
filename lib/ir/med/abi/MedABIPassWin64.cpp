//===- MedABIPassWin64.cpp - Microsoft x64 call-ABI recovery steps --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Microsoft x64 steps of recoverCallAbi.  MSVC splits a call from its
/// setup at EH state changes and `cmp/jnz` boundaries, so a call first in its
/// block can have its home-area stores and register writes in a predecessor;
/// it passes a member function's `this` in RCX and a returned aggregate's
/// buffer in RDX after it.  Each step recovers one of those shapes; generic
/// recovery runs them through Win64AbiCallPolicy.
///
//===----------------------------------------------------------------------===//

#include "MedABIPassDetail.h"

#include "neverd/ir/TargetRegInfo.h"

namespace neverd {

namespace {

/// Intervening Win64 thiscall clobbers rcx.  The unique predecessor's
/// `if (p)` pointer is the callee this, not a reaching pred rcx / live-in
/// parent this.  An in-block `mov rcx` after the helper still wins.
void resolveFirstArgAfterCall(AbiCallContext &C, bool &Arg0FromInBlock) {
  if (C.FoundMask[0]) {
    for (const MedCallClobber &Clobber : C.Func.CallClobbers) {
      if (Clobber.Value.Kind == C.Found[0].Kind &&
          Clobber.Value.Id == C.Found[0].Id &&
          Clobber.Value.SSAVer == C.Found[0].SSAVer &&
          Clobber.Value.RegOff == C.Found[0].RegOff) {
        Arg0FromInBlock = false;
        break;
      }
    }
  }
  if (Arg0FromInBlock)
    return;
  bool HasInterveningCall = false;
  for (int J = 0; J < C.CallIdx; ++J) {
    const NdOp PrevOp = C.Blk.Ops[static_cast<size_t>(J)].Opcode;
    if (PrevOp == NdOp::CALL || PrevOp == NdOp::INDIR_CALL) {
      HasInterveningCall = true;
      break;
    }
  }
  if (HasInterveningCall)
    if (auto Guard = uniquePredNonNullGuard(C.Func, C.Blk)) {
      C.Found[0] = *Guard;
      C.FoundMask[0] = true;
    }
}

/// Win64 IAT call with no stack arg: rcx/rdx written in the predecessor are
/// the real arguments (`cstr(&name)`, `assign(&dst, &src)`).  Do not use the
/// function's incoming this when nothing in the function wrote that
/// register.
void takeIndirectCallRegisters(AbiCallContext &C) {
  const int NumIntParamRegs = static_cast<int>(C.Layout.Registers.size());
  for (int K = 0; K < NumIntParamRegs && K < C.MaxArgs; ++K) {
    if (C.FoundMask[K])
      continue;
    if (K > 0 && !C.FoundMask[K - 1])
      break;
    bool FoundDef = false;
    auto V =
        findReachingArgReg(C.Func, C.TRI, C.TheArch, C.Blk.Id, K, C.Layout,
                           /*AllowUnknownLiveIn=*/false, nullptr, &FoundDef);
    if (!V || !FoundDef)
      break;
    C.Found[K] = *V;
    C.FoundMask[K] = true;
  }
}

/// A virtual call's object is arg0; when this function already has an rdx
/// param, the result buffer it passes on is arg1.
void takeVirtualCallResultBuffer(AbiCallContext &C) {
  if (!C.FoundMask[0] || C.FoundMask[1] || C.Layout.Registers.size() <= 1)
    return;
  const uint64_t SretOff = C.Layout.Registers[1];
  bool HaveSretParam = false;
  for (const auto &P : C.Func.Params)
    if ((P.Kind == MedVar::Reg || P.Kind == MedVar::Param) &&
        P.RegOff == SretOff)
      HaveSretParam = true;
  if (HaveSretParam)
    if (auto V =
            findReachingArgReg(C.Func, C.TRI, C.TheArch, C.Blk.Id, 1, C.Layout,
                               /*AllowUnknownLiveIn=*/false, nullptr)) {
      C.Found[1] = *V;
      C.FoundMask[1] = true;
    }
}

} // namespace

extern const AbiCallPolicy Win64AbiCallPolicy;
const AbiCallPolicy Win64AbiCallPolicy = {
    .TheArch = Arch::X64,
    .Format = BinaryFormat::COFF,
    .ResolveFirstArgAfterCall = resolveFirstArgAfterCall,
    .TakeIndirectCallRegisters = takeIndirectCallRegisters,
    .TakeVirtualCallResultBuffer = takeVirtualCallResultBuffer,
};

} // namespace neverd
