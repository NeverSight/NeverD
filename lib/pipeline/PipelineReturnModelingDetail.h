//===- PipelineReturnModelingDetail.h - Return ABI helpers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private declarations shared by the wide-integer and aggregate return
/// recovery implementations and the patch/lift pipeline driver.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINERETURNMODELINGDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINERETURNMODELINGDETAIL_H

namespace neverd {

struct BinaryImage;
struct PipelineResult;

/// Widen every callee that returns a 64-bit integer in a register pair to an
/// i64 return, and remodel the call sites (including loop-carried i64
/// accumulators) so both halves flow through.  32-bit targets only.
void modelWideIntReturns(const BinaryImage &Img, PipelineResult &Result);

/// Learn each callee's multi-register return shape from its direct callers'
/// field-extract remodeling and re-type the callee to match.  64-bit targets.
void recoverStructReturnFromCallers(const BinaryImage &Img,
                                    PipelineResult &Result);

/// Propagate a proven multi-register return shape forward through tail-call
/// forwarders (`struct S f(a){return g(a);}`) to callees reached only that way.
void propagateStructReturnForwarderShapes(const BinaryImage &Img,
                                          PipelineResult &Result);

/// Recover a multi-register (or single wide-vector) return shape from a
/// callee's own body when no direct caller reveals it.  AArch64 only.
void recoverStructReturnFromBody(const BinaryImage &Img,
                                 PipelineResult &Result);

/// Materialize the multi-register return at every direct call site to a known
/// register-returning callee, including chained sites whose result no explicit
/// op reads.  AArch64 only.
void materializeKnownStructReturnCallSites(const BinaryImage &Img,
                                           PipelineResult &Result);

/// Remodel struct-return tail-call forwarders after call-ABI recovery so the
/// forwarding call produces every field register the RETURN reassembles.
/// 64-bit targets.
void remodelStructReturnForwarderCalls(const BinaryImage &Img,
                                       PipelineResult &Result);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINERETURNMODELINGDETAIL_H
