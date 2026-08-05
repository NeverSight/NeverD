//===- PipelineReturnModeling.h - Return-value ABI recovery ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Return-value ABI recovery passes shared with the pipeline driver.  These run
/// on the whole MedIR module between type inference and call-ABI recovery
/// (Pipeline::runPatchLiftMode) to reconstruct wide-integer and by-value struct
/// returns the per-function low->med translation cannot see on its own:
///
///   * register-pair i64 returns on 32-bit targets (i386 EDX:EAX, ARM32 R1:R0);
///   * multi-register by-value struct / HFA returns (x86-64 SysV eightbytes,
///     AArch64 homogeneous float aggregates), recovered from a callee's direct
///     callers, its own body, and forwarded tail calls.
///
/// They were split out of Pipeline.cpp to keep that translation unit focused on
/// phase orchestration; this header is an implementation detail of the pipeline
/// library and should NOT be included by code outside lib/pipeline/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_PIPELINERETURNMODELING_H
#define NEVERD_PIPELINE_PIPELINERETURNMODELING_H

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

#endif // NEVERD_PIPELINE_PIPELINERETURNMODELING_H
