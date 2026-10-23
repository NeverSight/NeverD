//===- RegistrationABIPrivate.h - PE32 call frame checks ------------------===//
//
// NeverD Decompiler
//
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

namespace neverd {
class Decoder;
struct Import;
struct LowInstructionBoundary;
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
bool hasImmutableCalleeInstruction(const BinaryImage &Image,
                                   const LowInstructionBoundary &Boundary,
                                   Decoder &Dec);
bool collectCalleeCodeRanges(const LowFunc &Function, const BinaryImage &Image,
                             size_t &Work,
                             std::vector<ExceptionAddressRange> &Ranges);

bool chargeCalleeWork(size_t &Work, size_t Amount);

struct CleanupFramePrefix {
  unsigned Size = 0;
  uint8_t BaseRegister = 5;
  int32_t BaseOffset = 0;
  bool CallsLeaf = false;
  std::optional<RegistrationCleanupParentFrame> RealignedParent;
};

std::optional<CleanupFramePrefix>
getCleanupFramePrefix(const BinaryImage &Image, va_t Target, size_t &Work);

std::optional<uint16_t> parentPopBytes(const LowFunc &Function,
                                       const BinaryImage &Image);

std::optional<uint32_t>
checkedRegistrationImportStackPop(const BinaryImage &Image, va_t Target);
const Import *checkedImportSlot(const BinaryImage &Image, va_t Target);
bool callerPCIsNotReadBack(const ImageFrameEffects &Effects);
/// Initialization and value knowledge are independent. Without an explicit
/// byte inventory, only a retained full-word cell supplies an input.
std::optional<registration_state::FrameValue>
readCallerFrameWord(const registration_state::FrameState &State, int32_t Offset,
                    const std::set<int32_t> *Initialized, size_t &Work);
bool hasConditionalCallFrame(const BinaryImage &Image, const LowOp &Op,
                             const registration_state::FrameState &State,
                             size_t &Work,
                             const std::set<int32_t> *Initialized = nullptr);
bool hasClosedRegistrationRaise(const BinaryImage &Image, const LowOp &Op,
                                const registration_state::FrameState &State);
bool hasSEHCallbackImageMemory(const BinaryImage &Image, va_t Address,
                               uint16_t Width, bool Write, size_t &Work);

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
  /// entry frame). A plain callee frame only; no other option combines.
  const std::map<int32_t, registration_state::FrameValue> *ScalarArguments =
      nullptr;
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
