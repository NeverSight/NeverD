//===- SEHFrameProof.h - Local-unwind frame arguments -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Proves exact frame arguments over all ordinary LowIR predecessors.

#ifndef NEVERD_IR_LOW_SEHFRAMEPROOF_H
#define NEVERD_IR_LOW_SEHFRAMEPROOF_H

#include "neverd/ir/low/LowIR.h"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace neverd {

struct TargetRegInfo;
class LowSEHRuntimeMemory;

/// Source analysis can retain a runtime SP without proving that it names the
/// original invocation. Such a result cannot authorize memory forwarding or
/// native reconstruction.
enum class LowSEHFrameUse { InvocationIdentity, SourceProjection };

/// Fresh structural proof for one Windows handler/continuation and the exact
/// LowFunc supplied to the query. Index vectors refer to that snapshot only.
/// Decoded unwind actions and all reaching protected prefixes own the frame;
/// neither storage sizing nor ordinary entry roles grant that identity.
struct LowSEHEstablisherFrame {
  uint64_t FrameBytes = 0;
  bool DynamicSP = false;
  bool HasIndependentEntries = false;
  bool HasRelevantLocalUnwind = false;
  /// Source-only frame: ordinary and protected entries can have different SPs,
  /// but the handler never explicitly reads that SP before restoring it.
  bool HasVaryingEntrySP = false;
  std::vector<bool> RelevantBlocks;
  std::vector<bool> ProtectedBlocks;
  std::vector<va_t> CheckedEnd;
  std::map<va_t, uint64_t> DecodedAdjustments;
  std::vector<int> ProtectedSources;
  std::map<int, std::set<va_t>> ProtectedCalls;
};

/// A certified local unwind (SEHLocalUnwindProtocol.h) that converted MedIR
/// turned into a jump, and whose target only such exits name: a decoded-frame
/// proof enters the target from the call with the frame the call passes, as
/// from LowSEHLocalUnwindEvidence, instead of as an independent root.
struct LowSEHLocalUnwindExitSource {
  va_t CallAddr = InvalidVA;
  int CallSeq = -1;
  va_t Target = InvalidVA;
  /// The frame the call passes, relative to the entry stack pointer.
  int64_t FrameOffset = 0;
};

/// Shared decoded-frame authority. MedIR must additionally check converted
/// ABI effects against DecodedAdjustments. A dynamic frame has no fixed-SP
/// seed. Invocation identity rejects independent entries. Source projection
/// instead retains unknown entry values and requires its caller to prohibit
/// native lowering. Missing provenance and exhausted work always refuse.
llvm::Expected<LowSEHEstablisherFrame> proveLowSEHEstablisherFrame(
    const LowFunc &Function, va_t Handler, const TargetRegInfo &TRI,
    bool OrdinaryEntry, size_t &WorkRemaining,
    llvm::StringRef LocalUnwindDigest = {}, bool CxxContinuation = false,
    LowSEHFrameUse Use = LowSEHFrameUse::InvocationIdentity,
    llvm::ArrayRef<LowSEHLocalUnwindExitSource> Exits = {});

/// Windows x64 calls a termination handler (an out-of-line `__finally`
/// callback) with its parent's EstablisherFrame as the second argument. For a
/// parent without a frame register that is the SP every protected instruction
/// runs with: the entry SP less the prologue's allocations. Returns those
/// bytes when the normalized unwind actions and the decoded prologue name the
/// same allocations, and no operation on an ordinary path into any scope that
/// \p Handler terminates moves SP after the prologue. A frame register,
/// chained or incomplete unwind data, a handler inside the function, another
/// entry into the protected code and exhausted work refuse.
llvm::Expected<uint64_t> proveLowSEHTerminationFrame(const LowFunc &Parent,
                                                     va_t Handler,
                                                     const TargetRegInfo &TRI,
                                                     size_t &WorkRemaining);

/// The bytes below its entry SP at which a Windows x64 function without a
/// frame register runs its body: the allocations its unwind codes record,
/// which its decoded prologue makes exactly.  This is the EstablisherFrame
/// the runtime computes for it.  Chained or incomplete unwind data, a frame
/// register and a prologue write that is no recorded allocation refuse.
llvm::Expected<uint64_t> proveLowSEHFixedFrame(const LowFunc &Function,
                                               const TargetRegInfo &TRI,
                                               size_t &WorkRemaining);

/// The certified frame bytes of every termination handler of \p Parent, by
/// handler entry. A handler whose frame is not proved is absent; each handler
/// has its own `kMaxSEHEstablisherFrameWork` budget.
std::map<va_t, uint64_t> collectLowSEHTerminationFrames(const LowFunc &Parent,
                                                        Arch TargetArch);

struct LowSEHFrameProof {
  /// Exact entry-relative SP at direct calls whose first Win64 argument is
  /// that same value on every reaching ordinary path. No runtime identity,
  /// continuation ownership or unwind-prologue permission is implied.
  std::map<std::pair<va_t, int>, int64_t> Calls;
  std::string DependencyDigest;
  /// Complete decoded callees inspected to retain memory across a call.
  std::vector<va_t> CalleeDependencies;
  bool UsesRuntimeMemory = false;
};

/// Intersection analysis over ordinary CFG edges. Independent entries start
/// without the function's SP identity; an authenticated in-function SEH handler
/// receives only its separately proved fixed SP. Unknown calls/stores
/// invalidate saved memory. Decoded callees and authenticated runtime calls
/// prove concrete effects under one bounded invocation stack; every active
/// return PC is protected. Runtime facts include immutable scalar formats and
/// live format bytes from decoded frame/image stores. Writable file contents
/// grant no byte facts. An opaque frame escape cannot restore privacy.
/// No callee memory contract is inferred from an ABI.
/// Exhausted budgets and incomplete inputs return no proof.
LowSEHFrameProof
proveLowSEHFrames(const LowFunc &Function, const TargetRegInfo &TRI,
                  size_t &WorkRemaining,
                  const std::map<va_t, const LowFunc *> *Callees = nullptr,
                  const LowSEHRuntimeMemory *RuntimeMemory = nullptr);

/// Binds a proof to operations, control boundaries, roots, complete CFG edges
/// and the normalized exception-frame metadata consulted by the analysis.
/// This is a freshness check, not a replacement for the producing analysis.
std::string lowSEHFrameDependencyDigest(const LowFunc &Function);

/// Empty if any dependency is missing, duplicated or does not name its body.
std::string
lowSEHFrameDependencyDigest(const LowFunc &Function,
                            llvm::ArrayRef<va_t> Dependencies,
                            const std::map<va_t, const LowFunc *> *Callees,
                            const LowSEHRuntimeMemory *RuntimeMemory = nullptr);

} // namespace neverd

#endif
