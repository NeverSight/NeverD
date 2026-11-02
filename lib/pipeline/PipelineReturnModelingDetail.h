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

#include "PipelineSourceStackAccesses.h"

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <map>

namespace neverd {

struct BinaryImage;
struct MedFunc;
struct MedReturnReg;
struct MedOp;
struct PipelineResult;
class DebugContext;

/// The function of this module that a forwarding call \p Call enters: its
/// direct target, or this image's own routine that an import's stub or slot
/// reaches (importOwnRoutine), which sets \p ThroughImport.  Null for another
/// module's routine and for the stub that reaches one.
MedFunc *forwardingCallee(const BinaryImage &Img, const PipelineResult &Result,
                          const std::map<va_t, MedFunc *> &ByEntry,
                          const MedOp &Call, bool *ThroughImport = nullptr);

/// Widen every callee that returns a 64-bit integer in a register pair to an
/// i64 return, and remodel the call sites (including loop-carried i64
/// accumulators) so both halves flow through.  32-bit targets only.
void modelWideIntReturns(const BinaryImage &Img, PipelineResult &Result,
                         MedStackProjection StackProjection);

/// Learn each callee's multi-register return shape from its direct callers'
/// field-extract remodeling and re-type the callee to match.  64-bit targets.
void recoverStructReturnFromCallers(const BinaryImage &Img,
                                    PipelineResult &Result);

/// Type a callee that returns a whole vector in its FP return register, as
/// its direct callers read it, Float(16) (MedFunc::ReturnsVectorRegister).
/// Targets whose FP return register is a vector register.
void recoverVectorReturnFromCallers(const BinaryImage &Img,
                                    PipelineResult &Result);

/// Propagate a proven multi-register return shape forward through tail-call
/// forwarders (`struct S f(a){return g(a);}`) to callees reached only that way.
void propagateStructReturnForwarderShapes(const BinaryImage &Img,
                                          PipelineResult &Result);

/// What may reveal a callee's multi-register return shape.
enum class StructReturnEvidence : uint8_t {
  /// Only a direct caller's reads of the fields after the call.  A body that
  /// merely writes a second return register, a clobbered scratch, reveals
  /// nothing: C prints the callee's signature, which an extra field changes.
  Callers,
  /// Callers, or else the callee's own writes before each RETURN.  A rewrite
  /// may return an extra register that its callers ignore.
  CallersAndBody,
};

/// Whether every normal return supplies the caller-observed fields. Used for
/// selected definitions and bounded omitted-callee bodies alike. Without
/// \p PreservedArguments (no caller observed the first word), an argument
/// register the callee never writes supplies no field.
bool returnsStructFields(const MedFunc &Function,
                         llvm::ArrayRef<MedReturnReg> Shape, Arch Architecture,
                         BinaryFormat Format, bool PreservedArguments = true);

/// Recover a multi-register (or single wide-vector) return shape from a
/// callee's own body when no direct caller reveals it.  AArch64 only.
void recoverStructReturnFromBody(const BinaryImage &Img, PipelineResult &Result,
                                 StructReturnEvidence Evidence);

/// Materialize the multi-register return at every direct call site to a known
/// register-returning callee, including chained sites whose result no explicit
/// op reads.  AArch64 only.
void materializeKnownStructReturnCallSites(const BinaryImage &Img,
                                           PipelineResult &Result);

/// Bind declared scalar floating results to their physical return register,
/// and propagate the resulting types through forwarding calls in both routes.
void bindFloatCallResults(const BinaryImage &Img, PipelineResult &Result);

/// Settle which functions return no value a caller could rely on
/// (MedFunc::ReturnsNoValue), once for both C backends.
void settleReturnContracts(const BinaryImage &Img, PipelineResult &Result,
                           const DebugContext *Dbg = nullptr);

/// Remodel struct-return tail-call forwarders after call-ABI recovery so the
/// forwarding call produces every field register the RETURN reassembles.
/// 64-bit targets.
void remodelStructReturnForwarderCalls(const BinaryImage &Img,
                                       PipelineResult &Result);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINERETURNMODELINGDETAIL_H
