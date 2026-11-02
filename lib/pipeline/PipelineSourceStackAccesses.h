//===- PipelineSourceStackAccesses.h - Stack projection --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Post-conversion stack semantics shared by the initial MedIR conversion and
/// caller reconversion after register-pair return recovery.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINESOURCESTACKACCESSES_H
#define NEVERD_LIB_PIPELINE_PIPELINESOURCESTACKACCESSES_H

#include "neverd/ir/med/MedIntegerReturnRange.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <map>
#include <string>

namespace neverd {

struct BinaryImage;
class DebugContext;
struct LowFunc;
struct MedFunc;
struct PipelineOptions;
struct PipelineResult;

enum class MedStackProjection { Machine, Source };

/// Source projections omit proved caller-state saves. Machine analysis and
/// rewriting retain those operations even when source output was also asked
/// for.
MedStackProjection medStackProjection(const PipelineOptions &Options);

/// Recompute entry kind, alignment and source stack annotations on this fresh
/// conversion. Never copy annotation flags from another operation sequence.
/// With \p WaitForCalleeFacts and no \p CallReturns, a CALL return-slot load
/// deferred only because a CALL's stack position had no local range refuses
/// this attempt instead, before any annotation: the caller retries it once
/// callee return ranges are known.
/// CallFreeMachineSource is reserved for the output of
/// wrapInterpreterMachineStateX64, which rejects remaining CALL/INDIR_CALL
/// operations. Its guest call writes are explicit memory effects, so there
/// are no omitted native CALL writes to check. A source type hint alone does
/// not establish this property.
void prepareMedStackAccesses(MedFunc &Func, const LowFunc &Source,
                             const BinaryImage &Image,
                             MedStackProjection Projection,
                             const MedCallReturnRanges *CallReturns = nullptr,
                             bool WaitForCalleeFacts = false,
                             bool CallFreeMachineSource = false);

/// Read complete local leaf results in parallel after conversion has joined.
/// The returned facts are immutable during the following source checks.
MedCallReturnRanges sourceCallReturnRanges(const BinaryImage &Image,
                                           const PipelineResult &Result);

/// Shared source-only frame observation proof after call ABI recovery. Borrow
/// the original LowIR coverage from the pipeline or SDK's immutable result.
void prepareRecoveredSourceStackAccesses(const BinaryImage &Image,
                                         PipelineResult &Result,
                                         llvm::ArrayRef<LowFunc> Originals);

/// Whether a function MedIR refuses fails the run of \p Options rather than
/// leaving it out (PipelineOptions::OmitRefusedFunctions).
bool refusalFailsRun(const PipelineOptions &Options);

/// Leave each refused function, by its index in Result.LowFuncs and
/// Result.MedFuncs (which later stages pair by index), out of the run. Its
/// audit records MedIRFailed with the diagnostic, which whole-unit output
/// names; the other functions continue.
void omitRefusedFunctions(PipelineResult &Result,
                          const std::map<size_t, std::string> &Refused);

/// Settle the deferred CALL return-slot loads of every function
/// (validateObservedCallReturnReads) once call ABI recovery is complete and
/// before any source cleanup. A run omits each function that still observes
/// one; a run that \p FailsRun (refusalFailsRun) fails with Result.Error
/// instead and returns false. \p Dbg is the debug information the C routes
/// declare parameters from.
bool refuseObservedCallReturnReads(const BinaryImage &Image,
                                   PipelineResult &Result, bool FailsRun,
                                   const DebugContext *Dbg);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINESOURCESTACKACCESSES_H
