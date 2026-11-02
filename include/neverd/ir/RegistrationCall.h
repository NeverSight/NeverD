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
#include <utility>
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

/// A returning PE32 call's stack adjustment, independent of its memory
/// effects. An indirect identity names an authenticated import slot, never a
/// runtime pointer value. This cannot authorize a frame borrow or native EH.
struct RegistrationCalleeStackContract {
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  bool Indirect = false;
  /// Current immutable MOV occurrences that read this eager IAT slot. The
  /// load's instruction address and LowOp sequence bind an opaque callable,
  /// not the slot address or its initial file bytes as a runtime value.
  std::vector<std::pair<va_t, int>> Loads;
  /// A direct callee's transitive eager-IAT dependencies. Its returning stack
  /// proof assumes these bindings still hold at entry. Sorted and unique;
  /// an indirect contract instead depends on Target itself.
  std::vector<va_t> RequiredImports;
  /// An exact immutable FF25 tail veneer to RequiredImports' single slot.
  /// Under that binding it inherits the import's nonvolatile register ABI.
  bool ImportVeneer = false;
  /// A separate normal-return effect: existing eager-IAT bindings survive.
  /// Imports, veneers and closed ordinary helpers never restore lost bindings
  /// or grant frame access, result provenance, native EH or no-throw facts.
  bool PreservesImportBindings = false;
};

/// Runtime identity alone grants no frame effect. The state solver must bind
/// the actual arguments and prove every invoked finally before using it.
struct RegistrationLocalUnwindContract {
  enum class Kind : uint8_t { EH3, EH4 };
  va_t Target = InvalidVA;
  bool Indirect = false;
  Kind Protocol = Kind::EH3;
  /// Authenticated import storage; zero supplies no native binding authority.
  va_t ImportSlot = 0;
  bool operator==(const RegistrationLocalUnwindContract &) const = default;
};

/// One local unwind's actual arguments and complete returning state relation.
/// StackOffset uses the established source EBP, including a callback which has
/// restored SavedESP. EntryVA distinguishes that callback from the parent.
/// Native reconstruction must replace the source registration argument with
/// the compiler-owned frame and preserve the CRT's unwind guard protocol.
struct RegistrationSEHLocalUnwind {
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  RegistrationLocalUnwindContract Runtime;
  int32_t StackOffset = 0;
  va_t EntryVA = 0;
  uint32_t Bound = 0;
  std::vector<std::pair<int32_t, int32_t>> Transitions;
  bool operator==(const RegistrationSEHLocalUnwind &) const = default;
};

/// Source-byte evidence for the physical near CALL/RET protocol used by an
/// ordinary invocation of an owned SEH finally. A stack-pop summary does not
/// establish these instruction identities or their return-PC behavior.
struct RegistrationSEHControlContract {
  enum class Kind : uint8_t {
    NearCall,
    NearReturn,
    NearJump,
    /// Complete immutable parent epilogue, including its final near return.
    /// Target names that return; frame restoration needs a separate proof.
    ParentEpilogue
  } Control = Kind::NearCall;
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  va_t Target = InvalidVA;
  bool operator==(const RegistrationSEHControlContract &) const = default;
};

/// A checked original callee's contract. A returning leaf may borrow ECX;
/// a private throw/rethrow helper terminates without borrowing the parent.
/// This is source evidence, not a compiler or native installation receipt.
struct RegistrationCalleeFrameContract {
  enum class Kind : uint8_t {
    Leaf,
    PrivateThrow,
    PrivateRethrow,
    RuntimeThrow,
    Initializer,
    CxxParent
  } CalleeKind = Kind::Leaf;
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  bool DoesNotReturn = false;
  /// Every normal return proves a 32-bit scalar independent of incoming
  /// registers, borrowed pointers and the physical caller PC. This grants no
  /// object identity or initialization; native consumers reprove the callee.
  bool HasIndependentScalarReturn = false;
  va_t ThrownTypeVA = 0;
  uint32_t ThrownObjectSize = 0;
  std::vector<RegistrationRuntimeThrowInfo> RuntimeThrowInfos;
  /// Eager import bindings required by this complete invocation, including
  /// nested language dispatch. Sorted and unique; the caller must keep them
  /// live before using any of this contract's effects.
  std::vector<va_t> RequiredImports;
  std::vector<RegistrationObjectExtent> ECXReads;
  std::vector<RegistrationObjectExtent> ECXWrites;
  /// Bytes initialized on every normal return from an ordinary constructor.
  std::vector<RegistrationObjectExtent> ECXInitialized;
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
  bool isInitializer() const { return CalleeKind == Kind::Initializer; }
  bool isCxxParent() const { return CalleeKind == Kind::CxxParent; }
};

/// An ordinary member call borrows a still-live reference catch's exception
/// allocation. Offset is object-relative and never a parent-frame coordinate.
struct RegistrationRuntimeObjectBorrow {
  uint32_t TryIndex = 0;
  uint32_t CatchIndex = 0;
  int32_t Offset = 0;
  bool operator==(const RegistrationRuntimeObjectBorrow &) const = default;
};

/// One exact source call with a proved stack and bounded, initialized object
/// borrow. Frame extents use established parent EBP coordinates; runtime
/// borrows keep a distinct allocation identity. CalleeIndex selects the
/// immutable callee contract retained by the state analysis.
struct RegistrationCallFrameEffect {
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  va_t Target = InvalidVA;
  uint32_t CalleeIndex = 0;
  uint32_t StackPopBytes = 0;
  bool DoesNotReturn = false;
  std::optional<int32_t> ECXFrameOffset;
  std::optional<RegistrationRuntimeObjectBorrow> ECXRuntimeObject;
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
