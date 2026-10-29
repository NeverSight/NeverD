//===- SEHLocalUnwindX64.cpp - x64 local unwind convention ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows x64 `_local_unwind(TargetFrame, TargetIp)` takes the frame as its
/// EstablisherFrame: for a function without a frame register, the stack
/// pointer its prologue leaves for the body, which the runtime also passes
/// the function's filters and termination handlers.  A callee may write its
/// caller's home area, one slot per register argument above the stack
/// pointer at the call, and `__C_specific_handler` matches a scope by the
/// return address of the call into the runtime.  `__GSHandlerCheckCommon`
/// finds the GS cookie at the EstablisherFrame plus the GS data's offset and
/// unmasks it with the EstablisherFrame, a frame register being refused here.
/// The routine unwinds through
/// `RtlUnwind(TargetFrame, TargetIp, NULL, 0)` (vcruntime140's
/// `xor r9d, r9d` before the call), which resumes with that last argument,
/// zero, in RAX.
///
//===----------------------------------------------------------------------===//

#include "../SEHLocalUnwindProtocolDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/SEHFrameProof.h"

namespace neverd::seh_local_unwind {

llvm::Expected<SEHLocalUnwindConvention> x64Convention(const LowFunc &Function,
                                                       const TargetRegInfo &TRI,
                                                       size_t &WorkRemaining) {
  auto Bytes = proveLowSEHFixedFrame(Function, TRI, WorkRemaining);
  if (!Bytes)
    return Bytes.takeError();
  SEHLocalUnwindConvention Convention;
  Convention.Routine = "local_unwind";
  Convention.BodySP = -static_cast<int64_t>(*Bytes);
  Convention.TargetFrame = Convention.BodySP;
  Convention.HandlerFrame = Convention.BodySP;
  // No frame register: the cookie sits at, and is masked with, the
  // EstablisherFrame.
  Convention.GSCookieFrame = Convention.HandlerFrame;
  Convention.CalleeHomeBytes =
      static_cast<uint64_t>(TRI.Win64ParamRegs.size()) * TRI.PointerSize;
  Convention.ControlPcIsReturnAddress = true;
  Convention.ResumeReturnValue = 0;
  return Convention;
}

} // namespace neverd::seh_local_unwind
