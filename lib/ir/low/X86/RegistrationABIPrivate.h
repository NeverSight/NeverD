//===- RegistrationABIPrivate.h - PE32 call frame checks ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Share bounded PE32 callee frame, storage and invocation proof authorities.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H
#define NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H

#include "RegistrationFrame.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
class Decoder;
struct Import;
struct LowInstructionBoundary;
namespace registration_state {
struct BoundPlainCalleeEffect;
class OriginalImageFrameProof;
} // namespace registration_state
} // namespace neverd

namespace neverd::registration_abi {

struct ImageFrameEffects {
  std::set<std::pair<va_t, va_t>> Reads;
  std::set<std::pair<va_t, va_t>> Writes;
  std::set<std::pair<va_t, va_t>> CallerPCWrites;
  std::set<std::pair<int32_t, int32_t>> ECXReads;
  std::set<std::pair<int32_t, int32_t>> ECXWrites;
  std::set<std::pair<int32_t, int32_t>> CopySourceReads;
};

template <typename Set, typename Vector>
void copyExtents(const Set &From, Vector &To) {
  for (const auto &[Begin, End] : From) {
    if (!To.empty() && Begin <= To.back().End)
      To.back().End = std::max(To.back().End, End);
    else
      To.push_back({Begin, End});
  }
}

std::optional<std::vector<RegistrationRuntimeThrowInfo>>
collectRegistrationRuntimeThrowInfos(const LowFunc &Function,
                                     const BinaryImage &Image, size_t &Work,
                                     ImageFrameEffects &Effects,
                                     std::set<std::pair<va_t, va_t>> &Code);

bool chargeDecodedCallee(size_t &Work, const LowFunc &Function);
/// Nested C++ proofs depend on their active ancestors and depth. A stack
/// memo keyed only by target and SEH level cannot retain these answers.
bool hasActiveCxxCalleeProof();
/// Negative-only admission check for a zero-argument native C++ parent.
/// A checked straight-line shared prefix accesses incoming frame storage or
/// reaches an indirect call before any earlier ordinary call or branch.
/// Absence proves no calling convention or callee effect.
bool hasUnboundCxxEntryBarrier(const BinaryImage &Image,
                               const LowFunc &Function, size_t &Work);

/// Plain object/initializer proofs cannot own an EH frame. Check admission
/// before asking any subsidiary stack proof to build another callee graph.
bool isPlainRegistrationCalleeEntry(const BinaryImage &Image, va_t Target);
bool hasImmutableCalleeInstruction(const BinaryImage &Image,
                                   const LowInstructionBoundary &Boundary,
                                   Decoder &Dec);
bool collectCalleeCodeRanges(const LowFunc &Function, const BinaryImage &Image,
                             size_t &Work,
                             std::vector<ExceptionAddressRange> &Ranges);

bool chargeCalleeWork(size_t &Work, size_t Amount);

/// Exact shared frame operations belong to registration-state analysis, not
/// ordinary callee inference. These pointers identify only current LowIR
/// occurrences; another call to the same helper receives no exemption. This
/// classification grants no frame, return-stack or native lowering contract.
std::optional<std::set<const LowOp *>>
checkedRegistrationFrameCalls(const BinaryImage &Image, const LowFunc &Function,
                              size_t &Work);

struct CleanupFramePrefix {
  unsigned Size = 0;
  uint8_t BaseRegister = 5;
  int32_t BaseOffset = 0;
  bool CallsLeaf = false;
  std::optional<RegistrationCleanupParentFrame> RealignedParent;
};

std::optional<CleanupFramePrefix>
getCleanupFramePrefix(const BinaryImage &Image, va_t Target, size_t &Work);

/// Entry projection and actual normal-return evidence are separate facts.
/// A cleanup-only parent with closed throw exits has a zero-argument C entry,
/// but must never supply a normal-return stack or scalar-result contract.
struct ParentEntryABI {
  uint16_t PopBytes = 0;
  bool HasNormalReturn = false;
};
std::optional<ParentEntryABI> parentEntryABI(const LowFunc &Function,
                                             const BinaryImage &Image,
                                             size_t *CumulativeWork = nullptr);

std::optional<uint32_t>
checkedRegistrationImportStackPop(const BinaryImage &Image, va_t Target);
/// A separate normal-return effect for exact pointer-value transforms.
/// No result, frame, dispatch or unconditional-return authority follows.
bool preservesRegistrationImportBindings(const BinaryImage &Image, va_t Target);
const Import *checkedImportSlot(const BinaryImage &Image, va_t Target);
/// Authenticate the exact eager PE32 RaiseException IAT slot. A returning
/// stack adjustment alone cannot identify exception dispatch or its effects.
bool isCheckedRegistrationRaiseSlot(const BinaryImage &Image, va_t Target);
bool isCheckedRegistrationStartupInfoSlot(const BinaryImage &Image,
                                          va_t Target);
bool isCheckedRegistrationStartupInfoCall(const BinaryImage &Image,
                                          const LowOp &Op, size_t &Work);
bool isCheckedRegistrationHeapTerminationSlot(const BinaryImage &Image,
                                              va_t Target);
bool isCheckedRegistrationHeapTerminationCall(const BinaryImage &Image,
                                              const LowOp &Op, size_t &Work);
bool isCheckedRegistrationInterlockedSlot(const BinaryImage &Image,
                                          va_t Target);
bool isCheckedRegistrationInterlockedCall(const BinaryImage &Image,
                                          const LowOp &Op, size_t &Work);
bool isCheckedRegistrationSleepSlot(const BinaryImage &Image, va_t Target);
bool isCheckedRegistrationSleepCall(const BinaryImage &Image, const LowOp &Op,
                                    size_t &Work);
bool callerPCIsNotReadBack(const ImageFrameEffects &Effects);
/// Initialization and value knowledge are independent. Without an explicit
/// byte inventory, only a retained full-word cell supplies an input.
std::optional<registration_state::FrameValue>
readCallerFrameWord(const registration_state::FrameState &State, int32_t Offset,
                    const std::set<int32_t> *Initialized, size_t &Work);
/// Prove a particular caller's plain callee frame. An optional physical EAX
/// result keeps scalar/image identity, never a callee's frame coordinate.
/// Failure clears it; no ordinary return supplies no result. No native call
/// declaration or caller argument ownership is implied.
/// Effect additionally requires a closed returning graph with no reached
/// calls. Its stack cleanup, image reads and projected result share this same
/// input-bound analysis; failure clears the complete effect.
/// Image reads require Effect even when the returned value is ignored: both
/// the result and private-frame proof may depend on those original bytes.
bool hasConditionalCallFrame(
    const BinaryImage &Image, const LowOp &Op,
    const registration_state::FrameState &State, size_t &Work,
    const std::set<int32_t> *Initialized = nullptr,
    std::optional<registration_state::FrameValue> *ReturnValue = nullptr,
    registration_state::BoundPlainCalleeEffect *Effect = nullptr,
    registration_state::OriginalImageFrameProof *OriginalImage = nullptr);
bool hasClosedRegistrationRaise(const BinaryImage &Image, const LowOp &Op,
                                const registration_state::FrameState &State);
bool hasSEHCallbackImageMemory(const BinaryImage &Image, va_t Address,
                               uint16_t Width, bool Write, size_t &Work);
/// The loader owns immutable PE32 data mappings and fixup exclusion. Share
/// their work accounting across conditional callees; returned bytes grant
/// neither an address identity nor a native call or object-lifetime receipt.
std::optional<std::vector<uint8_t>>
readImmutableRegistrationImageBytes(const BinaryImage &Image, va_t Address,
                                    uint16_t Width, size_t &Work);

/// Checked immutable storage excludes live security-cookie bytes. Pointer-like
/// bytes retain storage evidence but never acquire a fixed integer identity.
struct ImmutableRegistrationScalar {
  std::vector<uint8_t> Bytes;
  std::optional<uint32_t> Value;
};
std::optional<ImmutableRegistrationScalar>
readImmutableRegistrationScalar(const BinaryImage &Image, va_t Address,
                                uint16_t Width, size_t &Work);

struct InitializerFrame {
  uint32_t StackPopBytes = 0;
  std::optional<std::set<int32_t>> Initialized;
};

using InitializerCalls =
    std::map<std::pair<va_t, int>, RegistrationInitializerCalleeABI>;

/// A fresh invocation owns this bounded, cycle-rejecting constructor graph.
/// The cache includes failures and never crosses an image or proof budget.
struct InitializerContext {
  std::map<va_t, std::optional<RegistrationInitializerCalleeABI>> Callees;
  unsigned Depth = 0;
};

std::optional<RegistrationInitializerCalleeABI>
checkedInitializerCalleeABI(const BinaryImage &Image, va_t Target, size_t &Work,
                            InitializerContext &Context);

using ObjectCallbackCalls =
    std::map<std::pair<va_t, int>, RegistrationLeafCalleeABI>;

/// A separate object graph never inherits constructor return-this or
/// initialization authority. Failures and in-progress nodes are memoized.
struct ObjectCallbackContext {
  std::map<va_t, std::optional<RegistrationLeafCalleeABI>> Callees;
  unsigned Depth = 0;
};

std::optional<RegistrationLeafCalleeABI>
checkedObjectCallbackCalleeABI(const BinaryImage &Image, va_t Target,
                               size_t &Work, ObjectCallbackContext &Context);

struct PrivateFrameOptions {
  bool BorrowECX = false;
  RegistrationThrowCalleeABI *ThrowProof = nullptr;
  bool *IndependentScalarReturn = nullptr;
  bool DiscardReturnValue = false;
  uint32_t CopyObjectSize = 0;
  InitializerFrame *Initializer = nullptr;
  const InitializerCalls *Initializers = nullptr;
  const ObjectCallbackCalls *ObjectCallbacks = nullptr;
  uint32_t CallbackStackPopBytes = 0;
  /// Known caller-written 4-byte stack arguments (offsets from the
  /// entry frame). An empty map still selects a bound plain-callee domain.
  /// Only ScalarRegisters and result outputs combine with this option.
  const std::map<int32_t, registration_state::FrameValue> *ScalarArguments =
      nullptr;
  /// Full volatile GPR facts, indexed like FrameState::Registers. Requires
  /// the bound domain above; no saved-register or frame identity is replaced.
  const std::map<unsigned, registration_state::FrameValue> *ScalarRegisters =
      nullptr;
  /// Joined physical EAX at every reached ordinary return of a bound call.
  /// Published only after the whole proof succeeds; absent for no returns or
  /// an incomplete result shape. Callee frame identities need caller
  /// projection.
  std::optional<registration_state::FrameValue> *ReturnValue = nullptr;
  /// The bound source consumer owns no nested runtime/callback effects.
  /// Unreached calls remain outside the invocation's memory footprint.
  bool ExcludeCalls = false;
  /// Only the explicit original-image invocation query supplies this scope.
  /// It requires the closed plain-callee domain and cannot publish native
  /// admission or an unscoped layout-independent scalar return.
  registration_state::OriginalImageFrameProof *OriginalImage = nullptr;
  /// Optional stronger effect proof. Unknown faults, FP and calls other than
  /// ThrowProof's exact terminal runtime call withdraw it monotonically.
  bool *ClosedIntegerEffects = nullptr;
};

bool hasPrivateCallerFrame(const LowFunc &Function, const BinaryImage &Image,
                           size_t &Work, ImageFrameEffects &Effects,
                           const PrivateFrameOptions &Options = {});

std::optional<RegistrationLeafCalleeABI> checkedLeafCalleeABI(
    const BinaryImage &Image, va_t Target, size_t &Work,
    RegistrationLeafResult ResultKind, uint32_t CopyObjectSize = 0,
    std::vector<RegistrationObjectExtent> *CopySource = nullptr,
    InitializerFrame *Initializer = nullptr,
    InitializerContext *Context = nullptr,
    ObjectCallbackContext *Callbacks = nullptr,
    uint32_t CallbackStackPopBytes = 0);

} // namespace neverd::registration_abi

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::registration_abi {
inline constexpr unsigned kMaxInitializerGraphDepth =
    ::neverd::limits::ir::kRegistrationABIPrivateMaxInitializerGraphDepth;
inline constexpr size_t kMaxInitializerGraphNodes =
    ::neverd::limits::ir::kRegistrationABIPrivateMaxInitializerGraphNodes;
inline constexpr unsigned kMaxObjectCallbackDepth =
    ::neverd::limits::ir::kRegistrationABIPrivateMaxObjectCallbackDepth;
inline constexpr size_t kMaxObjectCallbackNodes =
    ::neverd::limits::ir::kRegistrationABIPrivateMaxObjectCallbackNodes;
} // namespace neverd::registration_abi

#endif // NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H
