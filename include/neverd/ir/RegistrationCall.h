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

/// An immutable scalar ThrowInfo candidate. Only an exact source call's current
/// table argument can select it; its presence does not imply a throw occurred.
struct RegistrationRuntimeThrowInfo {
  va_t Address = 0;
  va_t TypeDescriptorVA = 0;
  uint32_t ObjectSize = 0;
  bool operator==(const RegistrationRuntimeThrowInfo &) const = default;
};

/// One direct CRT call's arguments. A null table rethrows the active exception.
/// Otherwise ObjectOffset names initialized scalar storage in the parent frame,
/// or in the current callback's private stack when CallbackVA is nonzero.
struct RegistrationRuntimeThrow {
  va_t ThrowInfoVA = 0;
  uint32_t ObjectSize = 0;
  int32_t ObjectOffset = 0;
  va_t CallbackVA = 0;
  bool isRethrow() const { return ThrowInfoVA == 0; }
  bool operator==(const RegistrationRuntimeThrow &) const = default;
};

/// A checked original callee's contract. A returning leaf may borrow ECX;
/// a private throw/rethrow helper terminates without borrowing the parent.
/// This is source evidence, not a compiler or native installation receipt.
struct RegistrationCalleeFrameContract {
  enum class Kind : uint8_t {
    Leaf,
    PrivateThrow,
    PrivateRethrow,
    RuntimeThrow
  } CalleeKind = Kind::Leaf;
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  bool DoesNotReturn = false;
  va_t ThrownTypeVA = 0;
  uint32_t ThrownObjectSize = 0;
  std::vector<RegistrationRuntimeThrowInfo> RuntimeThrowInfos;
  std::vector<RegistrationObjectExtent> ECXReads;
  std::vector<RegistrationObjectExtent> ECXWrites;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
  /// Complete executed instruction extents of original code this contract
  /// preserves, including a private throw's authenticated import thunk.
  std::vector<ExceptionAddressRange> CodeRanges;

  bool isThrow() const {
    return CalleeKind == Kind::PrivateThrow || isRethrow() || isRuntimeThrow();
  }
  bool isRethrow() const { return CalleeKind == Kind::PrivateRethrow; }
  bool isRuntimeThrow() const { return CalleeKind == Kind::RuntimeThrow; }
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
  std::optional<RegistrationRuntimeThrow> RuntimeThrow;
  std::vector<RegistrationObjectExtent> FrameReads;
  std::vector<RegistrationObjectExtent> FrameWrites;
  bool operator==(const RegistrationCallFrameEffect &) const = default;
};

struct RegistrationCleanupCallContract {
  int32_t ObjectFrameOffset = 0;
  RegistrationCalleeFrameContract Leaf;
};

/// One unwind-map action's ordered leaf calls. Every dispatch predecessor
/// must prove each borrow against the allocated, initialized parent frame.
struct RegistrationCleanupFrameContract {
  uint32_t ActionState = 0;
  va_t RelayTarget = InvalidVA;
  std::vector<RegistrationCleanupCallContract> Calls;
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
