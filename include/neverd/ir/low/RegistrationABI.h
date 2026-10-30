//===- RegistrationABI.h - PE32 registration call ABI ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONABI_H
#define NEVERD_IR_LOW_REGISTRATIONABI_H

#include "neverd/ir/RegistrationCall.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionCommon.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct LowBlock;
struct LowOp;
struct RegistrationStateAnalysis;
class RegistrationCallCalleeIndex;
namespace detail {
class AbsoluteRelocationRootIndex;
}

RegistrationStateAnalysis analyzeCheckedX86SEHStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *Controls,
    RegistrationCallCalleeIndex *Index = nullptr);

/// The nesting of the analyzeCheckedX86SEHStates calls active on this thread,
/// as far as it changes them: an analysis nested this deep or deeper no longer
/// checks bound SEH calls, so every deeper nesting answers alike. A proof that
/// builds a registration callee answers per level.
unsigned x86SEHStateAnalysisLevel();

/// Authenticate the current immutable PE32 E8 rel32 bytes and target. This
/// supplies no source occurrence, argument, frame or exception-effect proof.
bool hasCheckedX86RegistrationDirectCallBytes(const BinaryImage &Image,
                                              va_t Address, va_t Target);
bool hasCheckedX86RegistrationDirectCallBytes(const BinaryImage &Image,
                                              va_t Address, va_t Target,
                                              size_t &Work, size_t Limit);

/// Prove the entry ESP is restored at every ordinary near return and that
/// all return-pop immediates agree. Unchecked calls invalidate ESP; an explicit
/// restoration from an ABI-preserved register can recover it. Ordinary bodies
/// retain no memory values. SEH bodies also require complete current
/// registration state/lifetime evidence to separate dispatcher returns and
/// seed established EBP at except entries. Neither route infers saved-stack
/// memory values or grants memory-effect authority. A termination-only SEH
/// frame has no exceptional parent entry and uses the ordinary register-only
/// proof, including a separate proof for every called finally.
/// Import-dependent callees assume their eager IAT bindings at entry. Source
/// callers consume stackContracts(), which retains those transitive
/// dependencies and checks that the caller has not invalidated them.
std::optional<uint32_t>
getCheckedX86CalleeStackPop(const BinaryImage &Image, va_t Target,
                            size_t *CumulativeWork = nullptr);

/// A fresh standalone returning-stack proof with no entry IAT dependencies.
/// Charge the existing callee proof to Work, up to Limit and the ordinary
/// registration work ceiling. This optional query never starts a fresh proof
/// from inside another callee or registration-state analysis. Neither success
/// nor failure survives an image change. The consumer must independently bind
/// its actual direct CALL; memory effects and native ABI remain separate.
std::optional<uint32_t>
getCheckedX86IndependentCalleeStackPop(const BinaryImage &Image, va_t Target,
                                       size_t &Work, size_t Limit);

/// A single physical caller block ending in a direct CALL may fix the index
/// of an immutable `jmp [reg*4+table]` callee entry. Re-lift the current prefix
/// and prove its scalar index without incoming values or memory forwarding,
/// then prove the selected target's independent returning stack. Roots must
/// include every additional entry into the supplied prefix. This result is
/// invocation-specific: it neither bounds the dispatcher globally nor grants
/// frame memory, return-value, import-preservation or native EH authority.
std::optional<uint32_t> getCheckedX86IndependentIndexedCallStackPop(
    const BinaryImage &Image, const LowBlock &Prefix,
    const std::set<va_t> &Roots, size_t &Work, size_t Limit);

/// The answers of getCheckedX86CalleeStackPop for one unchanged image, each
/// with the work its computation charged, per callee and
/// x86SEHStateAnalysisLevel. A repeated query replays the answer and charges
/// the same work, so both the answer and the caller's cumulative budget,
/// including its exhaustion, are those of a fresh computation. Only
/// computations that stayed within the budget are kept. The image must not
/// change while a scope uses the memo.
class RegistrationStackPopMemo {
public:
  struct Answer {
    std::optional<uint32_t> Pop;
    size_t Work = 0;
  };

  explicit RegistrationStackPopMemo(const BinaryImage &Image) : Image(Image) {}
  RegistrationStackPopMemo(const RegistrationStackPopMemo &) = delete;
  RegistrationStackPopMemo &
  operator=(const RegistrationStackPopMemo &) = delete;

  std::optional<Answer> find(const BinaryImage &Other, va_t Target,
                             unsigned Level);
  void record(const BinaryImage &Other, va_t Target, unsigned Level,
              const Answer &Value);

private:
  const BinaryImage &Image;
  std::mutex Mutex;
  std::map<std::pair<va_t, unsigned>, Answer> Answers;
};

/// Answer this thread's getCheckedX86CalleeStackPop queries from \p Memo
/// while the scope lives. The innermost scope wins.
class RegistrationStackPopMemoScope {
public:
  explicit RegistrationStackPopMemoScope(RegistrationStackPopMemo &Memo);
  ~RegistrationStackPopMemoScope();
  RegistrationStackPopMemoScope(const RegistrationStackPopMemoScope &) = delete;
  RegistrationStackPopMemoScope &
  operator=(const RegistrationStackPopMemoScope &) = delete;

private:
  RegistrationStackPopMemo *Previous;
};

/// Authenticate a PE32 _local_unwind2/4 import or immutable IAT veneer.
/// This identifies its runtime protocol, not its caller's frame or callbacks.
std::optional<RegistrationLocalUnwindContract>
getCheckedX86LocalUnwindContract(const BinaryImage &Image, va_t Target,
                                 bool Indirect);

/// Recheck one local-unwind receipt against its current physical CALL and
/// import binding. Argument and callback effects remain the state owner's.
bool hasCheckedX86LocalUnwindCall(const BinaryImage &Image,
                                  const LowFunc &Function, const LowOp &Op,
                                  const RegistrationSEHLocalUnwind &Call,
                                  size_t &Work);

/// A leaf's two separate address domains: its private invocation frame and
/// the bounded object borrowed through entry ECX. Object offsets are relative
/// to that object, never invented image addresses or private stack offsets.
struct RegistrationLeafCalleeABI {
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  /// Every returning path computes its 32-bit scalar result independently of
  /// incoming registers, borrowed pointers and the physical caller PC. A frame
  /// privacy proof alone may still permit an unobserved entry-EAX return.
  bool HasIndependentScalarReturn = false;
  /// Separate closed integer effects, currently limited to leaves without
  /// object borrows or nested calls. Retains existing IAT bindings across
  /// possible dispatch origins, not an independently inferred entry ABI.
  bool PreservesDispatchBindings = false;
  std::vector<RegistrationObjectExtent> ECXReads;
  std::vector<RegistrationObjectExtent> ECXWrites;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
  std::vector<ExceptionAddressRange> CodeRanges;
};

enum class RegistrationLeafResult { Observed, Discarded };

/// Prove a complete returning PE32 leaf with a private stack, preserved
/// nonvolatile registers and nonescaping ECX object access. This describes
/// the callee only: each caller must still prove object bounds, initialization
/// and separation from its registration fields before using the projection.
/// Discarded requires an independently known void invocation, such as the
/// CRT destructor or an authenticated unwind relay; it does not recover a
/// source call's return type or relax memory/stack/nonvolatile preservation.
std::optional<RegistrationLeafCalleeABI> getCheckedX86RegistrationLeafCalleeABI(
    const BinaryImage &Image, va_t Target, size_t *CumulativeWork = nullptr,
    RegistrationLeafResult Result = RegistrationLeafResult::Observed);

/// Extend the returning leaf contract to a bounded acyclic object call graph.
/// Direct children borrow the same ECX object or a nonnegative subobject and
/// consume only initialized private scalar arguments. A child result remains
/// unknown unless every return proves an independent 32-bit scalar. Observed
/// graph results require this same independent result proof at the root.
/// Transitive code, object and image effects belong to this invocation's proof.
/// Each caller still owns object extent, contents, lifetime and return usage.
/// The public root has no stack arguments or callee cleanup; scalar child
/// arguments are proved only within their parent's private invocation.
std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationObjectCalleeABI(
    const BinaryImage &Image, va_t Target, size_t *CumulativeWork = nullptr,
    RegistrationLeafResult Result = RegistrationLeafResult::Observed);

/// An ordinary ECX member may additionally consume bounded, callee-popped
/// scalar stack words. Each source occurrence must prove those outgoing bytes
/// initialized and pointer-free, separate from its live object borrow. CRT
/// destructor and cleanup callers retain the zero-argument API above.
std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationMemberCalleeABI(
    const BinaryImage &Image, va_t Target, size_t *CumulativeWork = nullptr,
    RegistrationLeafResult Result = RegistrationLeafResult::Observed);

/// A CRT copy constructor borrows two separate objects: destination ECX and
/// the source pointer in its one callee-popped stack word. Every destination
/// byte is initialized on every return, and source storage is read-only.
struct RegistrationCopyCalleeABI {
  RegistrationLeafCalleeABI Destination;
  std::vector<RegistrationObjectExtent> SourceReads;
};

std::optional<RegistrationCopyCalleeABI>
getCheckedX86RegistrationCopyCalleeABI(const BinaryImage &Image, va_t Target,
                                       uint32_t ObjectSize,
                                       size_t *CumulativeWork = nullptr);

/// A returning object initializer takes scalar stack words and entry ECX,
/// returns that exact object pointer and pops its complete argument area.
/// Initialized records bytes written on every returning path. Each caller
/// must independently bind the arguments and object to its live private frame.
/// Bounded acyclic direct initializer calls compose subobject must-writes and
/// preserve the root destination. The proof retains all transitive code and
/// image effects; it does not authorize other calls or pointer arguments.
struct RegistrationInitializerCalleeABI {
  RegistrationLeafCalleeABI Object;
  std::vector<RegistrationObjectExtent> Initialized;
};

std::optional<RegistrationInitializerCalleeABI>
getCheckedX86RegistrationInitializerCalleeABI(const BinaryImage &Image,
                                              va_t Target,
                                              size_t *CumulativeWork = nullptr);

/// A single unadjusted object and its optional preserved runtime callbacks. The
/// destructor must be a private, returning object graph with bounded ECX
/// accesses and no observation of the CRT caller PC. Metadata decoding alone
/// cannot prove these effects or authorize a runtime callback.
struct RegistrationThrowObjectABI {
  coff_loader::X86IdentityCxxThrowInfo Info;
  std::optional<RegistrationLeafCalleeABI> Destructor;
  std::optional<RegistrationCopyCalleeABI> CopyConstructor;

  std::array<const RegistrationLeafCalleeABI *, 2> callbacks() const {
    return {Destructor ? &*Destructor : nullptr,
            CopyConstructor ? &CopyConstructor->Destination : nullptr};
  }
};

std::optional<RegistrationThrowObjectABI>
getCheckedX86RegistrationThrowObjectABI(const BinaryImage &Image, va_t Address,
                                        size_t *CumulativeWork = nullptr);

struct RegistrationCleanupCallABI {
  int32_t ObjectFrameOffset = 0;
  RegistrationLeafCalleeABI Leaf;
};

/// Coordinates read by a realigned cleanup before calling its leaves. The
/// parent state proof must establish the saved entry EBP and keep it intact.
struct RegistrationCleanupParentFrame {
  int32_t BaseOffset = 0;
  int32_t SavedParentFrameOffset = 0;
};

/// A checked cleanup relay derives each ECX object from the establisher EBP.
/// It tail-jumps to one returning object callee or saves/restores runtime EBP
/// around an ordered sequence of calls. The parent proves every borrow.
struct RegistrationCleanupRelayABI {
  va_t Target = InvalidVA;
  va_t EndAddress = InvalidVA;
  std::vector<RegistrationCleanupCallABI> Calls;
  std::optional<RegistrationCleanupParentFrame> RealignedParent;

  /// Bind a cached byte proof to this invocation's frame. This checks only
  /// coordinates; the parent's LowIR proof owns contents and lifetime.
  bool matchesParentFrame(const RegistrationChainInfo &Chain) const;
};

std::optional<RegistrationCleanupRelayABI>
getCheckedX86RegistrationCleanupRelayABI(const BinaryImage &Image, va_t Target,
                                         size_t *CumulativeWork = nullptr);

/// The exact two-argument stdcall runtime import and its transparent jump stub.
/// This proves an entry ABI only; a rethrow occurrence must also prove two null
/// arguments and an active catch in the source state solver.
struct RegistrationThrowImportABI {
  va_t Target = InvalidVA;
  va_t IATVA = InvalidVA;
};

std::optional<RegistrationThrowImportABI>
getCheckedX86RegistrationThrowImportABI(const BinaryImage &Image, va_t Target,
                                        size_t *CumulativeWork = nullptr);

struct RegistrationThrowCalleeABI {
  va_t Target = InvalidVA;
  va_t ImportVA = InvalidVA;
  va_t ImportIATVA = InvalidVA;
  va_t ThrowCallVA = InvalidVA;
  va_t ThrowCallEndVA = InvalidVA;
  int ThrowOpSeq = -1;
  int32_t ObjectOffset = 0;
  /// Exactly two null arguments reuse the active CRT exception. No new object
  /// or ThrowInfo is supplied, and the caller must prove an active catch.
  bool IsRethrow = false;
  /// Separate exceptional-path evidence: only closed integer accesses and the
  /// checked throw call, with no initializer, copy or destructor callback.
  /// A rethrow still requires a live, independently proved scalar exception.
  bool PreservesDispatchBindings = false;
  coff_loader::X86IdentityCxxThrowInfo ThrowInfo;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
  std::vector<ExceptionAddressRange> CodeRanges;
};

/// Prove a closed PE32 helper that initializes a private scalar object or
/// rethrows, then terminates through the exact VCRUNTIME140 import. The
/// original helper is preserved; no source frame pointer may escape except
/// its checked object argument and an observable real caller PC.
std::optional<RegistrationThrowCalleeABI>
getCheckedX86RegistrationThrowCalleeABI(const BinaryImage &Image, va_t Target,
                                        size_t *CumulativeWork = nullptr);

/// A cleanup-only C++ parent with no incoming arguments or object borrows.
/// Proves its private physical frame, ordinary returns, all transitive image
/// effects and required runtime bindings. The original callee is preserved;
/// callers and native emitters must bind/replay this context-free evidence.
std::optional<RegistrationCalleeFrameContract>
getCheckedX86RegistrationCxxCalleeABI(const BinaryImage &Image, va_t Target,
                                      size_t *CumulativeWork = nullptr);

/// Memoized checked callees for one CFG construction and its continuation
/// closure. A shared work budget includes failed proofs; a new CFG build gets
/// a fresh index so edits to an image cannot reuse stale callee evidence.
class RegistrationCallCalleeIndex {
public:
  explicit RegistrationCallCalleeIndex(const BinaryImage &Image,
                                       size_t *CumulativeWork = nullptr)
      : Image(Image), Work(CumulativeWork ? *CumulativeWork : LocalWork) {}
  RegistrationCallCalleeIndex(const RegistrationCallCalleeIndex &) = delete;
  RegistrationCallCalleeIndex &
  operator=(const RegistrationCallCalleeIndex &) = delete;
  bool ownsImage(const BinaryImage &Other) const { return &Image == &Other; }
  /// Share immutable image bytes and runtime pointer roles, never entry- or
  /// budget-dependent callee conclusions, across this invocation closure.
  const detail::AbsoluteRelocationRootIndex &absoluteRelocationRoots();
  /// Bill decoded state propagation, including unsuccessful helper analyses,
  /// to this invocation closure's cumulative work budget.
  bool chargeStateWork(size_t Amount);
  std::optional<RegistrationCalleeFrameContract> cxxCallee(va_t Target);
  /// Prove a standalone entry. Local witnesses from stackContracts() cannot
  /// satisfy this entry's admission or its transitive callee dependencies.
  std::optional<uint32_t> stackPop(va_t Target);
  /// The same standalone proof, retaining every entry import dependency.
  std::optional<RegistrationCalleeStackContract> stackContract(va_t Target);
  std::optional<std::vector<RegistrationCalleeFrameContract>>
  contracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationCleanupFrameContract>>
  cleanupContracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationCalleeStackContract>>
  stackContracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationLocalUnwindContract>>
  localUnwindContracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationSEHControlContract>>
  sehControlContracts(const LowFunc &Function);

private:
  const BinaryImage &Image;
  size_t LocalWork = 0;
  size_t &Work;
  unsigned StackDepth = 0;
  unsigned CxxDepth = 0;
  std::map<va_t, std::optional<RegistrationCalleeFrameContract>> Cache;
  std::map<va_t, std::optional<RegistrationCleanupRelayABI>> CleanupCache;
  // Independent entries only; parent-graph witnesses stay in their query.
  std::map<va_t, std::optional<RegistrationCalleeStackContract>> StackCache;
  std::shared_ptr<const detail::AbsoluteRelocationRootIndex> RelocationRoots;
};

/// Native registration lowering currently emits caller-cleanup calls. Require
/// the source and every preserved direct callee to have that same stack
/// contract. Indirect targets, incomplete bodies and tail-only bodies provide
/// no such proof. The final writer replays this check from immutable input.
bool hasCallerCleanupRegistrationABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites = nullptr);

/// Prove the C++ parent's entry separately from its runtime callbacks, and
/// replay the preserved call/frame closure. Normal returns require one exact
/// RET cleanup. A cleanup-only parent with no incoming frame access and closed
/// private-throw exits can use a zero-argument C entry without proving any
/// normal-return stack effect. Decoder maxima supply neither contract.
std::optional<uint16_t> getCheckedX86RegistrationCxxParentABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites = nullptr);

/// The corresponding SEH3/SEH4 parent contract. Runtime filters and finally
/// callbacks retain their independent zero-pop invocation ABI.
std::optional<uint16_t> getCheckedX86RegistrationSEHParentABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites = nullptr);
} // namespace neverd

#endif
