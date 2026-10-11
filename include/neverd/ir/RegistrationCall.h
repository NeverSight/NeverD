//===- RegistrationCall.h - Checked PE32 call projections --------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_REGISTRATIONCALL_H
#define NEVERD_IR_REGISTRATIONCALL_H

#include "neverd/loader/ExceptionCommon.h"

#include <optional>
#include <vector>

namespace neverd {

struct RegistrationObjectExtent {
  int32_t Begin = 0;
  int32_t End = 0;
  bool operator==(const RegistrationObjectExtent &) const = default;
};

/// A returning PE32 call's stack adjustment, independent of its memory
/// effects. An indirect identity names an authenticated import slot, never a
/// runtime pointer value. This cannot authorize a frame borrow or native EH.
struct RegistrationCalleeStackContract {
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  bool Indirect = false;
};

/// Runtime identity alone grants no frame effect. The state solver must bind
/// the actual arguments and prove every invoked finally before using it.
struct RegistrationLocalUnwindContract {
  va_t Target = InvalidVA;
  bool Indirect = false;
};

/// A checked original callee's contract. A returning leaf may borrow ECX;
/// a private scalar-throw helper terminates without borrowing the parent.
/// This is source evidence, not a compiler or native installation receipt.
struct RegistrationCalleeFrameContract {
  enum class Kind : uint8_t { Leaf, PrivateThrow } CalleeKind = Kind::Leaf;
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  bool DoesNotReturn = false;
  va_t ThrownTypeVA = 0;
  uint32_t ThrownObjectSize = 0;
  std::vector<RegistrationObjectExtent> ECXReads;
  std::vector<RegistrationObjectExtent> ECXWrites;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
  /// Complete executed instruction extents of original code this contract
  /// preserves, including a private throw's authenticated import thunk.
  std::vector<ExceptionAddressRange> CodeRanges;
};

/// One exact source call with a proved stack and bounded, initialized object
/// borrow. Frame extents use established parent EBP coordinates. CalleeIndex
/// selects the immutable callee contract retained by the state analysis.
struct RegistrationCallFrameEffect {
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  va_t Target = InvalidVA;
  uint32_t CalleeIndex = 0;
  uint32_t StackPopBytes = 0;
  bool DoesNotReturn = false;
  std::optional<int32_t> ECXFrameOffset;
  std::vector<RegistrationObjectExtent> FrameReads;
  std::vector<RegistrationObjectExtent> FrameWrites;
  bool operator==(const RegistrationCallFrameEffect &) const = default;
};

/// A source unwind-map action with an authenticated EBP-to-ECX relay and
/// returning leaf. It grants no object borrow until the state solver checks
/// all dispatch predecessors against the allocated, initialized parent frame.
struct RegistrationCleanupFrameContract {
  uint32_t ActionState = 0;
  va_t RelayTarget = InvalidVA;
  int32_t ObjectFrameOffset = 0;
  RegistrationCalleeFrameContract Leaf;
};

struct RegistrationCleanupFrameEffect {
  int BlockId = -1;
  ExceptionAddressRange Range;
  int32_t DispatchLevel = -1;
  uint32_t ActionState = 0;
  uint32_t CleanupIndex = 0;
  int32_t StackOffset = 0;
  std::vector<RegistrationObjectExtent> FrameReads;
  std::vector<RegistrationObjectExtent> FrameWrites;
  bool operator==(const RegistrationCleanupFrameEffect &Other) const {
    return BlockId == Other.BlockId && Range.Begin == Other.Range.Begin &&
           Range.End == Other.Range.End &&
           DispatchLevel == Other.DispatchLevel &&
           ActionState == Other.ActionState &&
           CleanupIndex == Other.CleanupIndex &&
           StackOffset == Other.StackOffset && FrameReads == Other.FrameReads &&
           FrameWrites == Other.FrameWrites;
  }
};

} // namespace neverd

#endif
