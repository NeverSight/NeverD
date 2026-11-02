//===- X86MpxContext.h - Architectural bounds state ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// MPX enable checks, bounds-bank transitions and bounds-table translation.
/// Memory providers own access validity and partial memory effects; this owner
/// sequences their completion with bounds-bank updates. Address calculation is
/// not a memory probe.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86MPXCONTEXT_H
#define NEVERD_IR_X86_X86MPXCONTEXT_H

#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

inline constexpr unsigned X86MpxRegisterCount = 4;
inline constexpr uint64_t X86MpxXcr0Mask = (1u << 3) | (1u << 4);
inline constexpr uint64_t X86MpxConfigEnable = 1;
inline constexpr uint64_t X86MpxConfigPreserve = 2;
inline constexpr uint64_t X86MpxConfigReserved = 0xffc;

enum class X86MpxMode : uint8_t { Bits32, Bits64 };
enum class X86MpxCheck : uint8_t { Lower, UpperComplement, UpperRaw };
enum class X86MpxBranch : uint8_t { Preserve, Initialize };
enum class X86MpxResult : uint8_t {
  Completed,
  Disabled,
  UnknownContext,
  UnknownOperand,
  UnsupportedTransaction,
  InvalidInput,
  BoundsFault,
  PartialStateUnknown,
  MemoryFault,
  UnknownMemory
};

/// Unknown fields are not implicitly zero. A known disabled prerequisite is
/// sufficient to prove that MPX is disabled, even if other fields are unknown.
struct X86MpxConfiguration {
  std::optional<bool> Supported;
  std::optional<bool> OSXSAVE;
  std::optional<uint64_t> XCR0;
  std::optional<uint8_t> PrivilegeLevel;
  std::optional<uint64_t> UserConfig;
  std::optional<uint64_t> SupervisorConfig;
  std::optional<uint8_t> UserAddressWidthAdjust;
  /// A transaction needs the original checkpoint/abort continuation. This
  /// state evaluator cannot commit MPX changes into an unauthenticated RTM/HLE
  /// execution. enabled() itself is independent of transaction state.
  std::optional<bool> InTransaction;
};

struct X86MpxValue {
  uint64_t Value = 0;
  uint64_t KnownMask = 0;
  bool operator==(const X86MpxValue &) const = default;
};

/// Upper is the stored field, normally the one's complement of the limit.
/// KnownMask also represents the unqualified high halves after 32-bit writes.
struct X86MpxBounds {
  X86MpxValue Lower;
  X86MpxValue Upper;
  bool operator==(const X86MpxBounds &) const = default;
};

struct X86MpxAddress {
  X86MpxResult Result = X86MpxResult::UnknownContext;
  uint64_t Address = 0;
  unsigned WordBytes = 0;
};

struct X86MpxStoreValue {
  X86MpxResult Result = X86MpxResult::UnknownContext;
  uint64_t Lower = 0;
  uint64_t Upper = 0;
  unsigned WordBytes = 0;
};

class X86MpxMemoryProvider;

class X86MpxContext {
public:
  /// Validate a supplied snapshot without changing the existing configuration
  /// on failure. Configuration changes do not implicitly reset BNDREGS/status.
  bool configure(const X86MpxConfiguration &C);
  std::optional<bool> enabled() const;
  /// Check this before obtaining operands or touching memory. Completed proves
  /// enabled, nontransactional state and a valid mode/bank, not instruction
  /// encoding validity or memory access permission.
  X86MpxResult operationStatus(X86MpxMode Mode, unsigned Register) const;

  bool setBounds(unsigned Register, const X86MpxBounds &Value);
  std::optional<X86MpxBounds> bounds(unsigned Register) const;
  void setStatus(std::optional<uint64_t> Value) { Status = Value; }
  std::optional<uint64_t> status() const { return Status; }

  X86MpxResult move(X86MpxMode Mode, unsigned Destination, unsigned Source);
  /// Base is the unscaled base register (zero if absent); EffectiveAddress
  /// includes displacement/index/scale but no segment base, like LEA.
  /// A 67H prefix in long mode does not truncate either input.
  X86MpxResult make(X86MpxMode Mode, unsigned Destination,
                    std::optional<uint64_t> Base,
                    std::optional<uint64_t> EffectiveAddress);
  /// Both register and address checks consume an unsigned effective value.
  /// An address check does not read memory or add a segment base.
  X86MpxResult check(X86MpxMode Mode, X86MpxCheck Kind, unsigned Register,
                     std::optional<uint64_t> Value);

  /// Called only after both BNDMOV memory fields have completed successfully.
  /// This does not perform or authorize the memory access.
  X86MpxResult completeLoad(X86MpxMode Mode, unsigned Destination,
                            uint64_t Lower, uint64_t Upper);

  /// Read the two fields for BNDMOV/BNDSTX stores, requiring known bits at the
  /// mode's width. The provider owns memory permissions, LOCK and partial
  /// writes; this query does not perform or commit a store.
  X86MpxStoreValue storeValue(X86MpxMode Mode, unsigned Register) const;

  /// LinearPointerAddress is base+disp plus the selected segment base; the
  /// caller supplies zero before segmentation if the MIB base is absent.
  /// The MIB index and scale do not participate. No canonical/segment check is
  /// applied to this value: only the resulting BDE/BTE addresses are accessed.
  X86MpxAddress
  directoryAddress(X86MpxMode Mode,
                   std::optional<uint64_t> LinearPointerAddress) const;
  /// Consume a successfully read BDE. Invalid entries set BNDSTATUS to the BDE
  /// address | 2 and fault before the bank or table memory is touched.
  X86MpxAddress tableAddress(X86MpxMode Mode,
                             std::optional<uint64_t> LinearPointerAddress,
                             uint64_t DirectoryEntry);
  /// Called only after all three BTE fields have been read successfully.
  /// PointerValue is the unscaled MIB index (zero if absent).
  X86MpxResult completeTableLoad(X86MpxMode Mode, unsigned Destination,
                                 uint64_t Lower, uint64_t Upper,
                                 uint64_t StoredPointer, uint64_t PointerValue);
  /// A BNDLDX fault while reading the BTE may partially update the bank in an
  /// implementation-dependent order. Forget the destination, not other banks
  /// or BNDSTATUS. Do not call this for a BDE access fault or invalid BDE.
  X86MpxResult tableReadFault(X86MpxMode Mode, unsigned Destination);

  /// Execute complete BNDMOV or bounds-table memory transactions. The provider
  /// owns segmentation, canonicality, permissions, LOCK and partial memory
  /// effects. Disabled/unknown contexts and unknown required operands do not
  /// call it. A provider fault is distinct from unavailable memory semantics.
  /// BNDMOV addresses are offsets in the original operand's segment; table
  /// operations take the already segmented pointer address, never dereference
  /// that pointer, and send only the resulting BDE/BTE addresses to memory.
  /// DefaultStack retains the original SP/BP base independently of segment
  /// overrides: long mode ignores CS/DS/ES/SS prefixes for fault provenance.
  X86MpxResult load(X86MpxMode Mode, unsigned Destination,
                    std::optional<uint64_t> Address,
                    X86MpxMemoryProvider &Memory,
                    X86SegmentRegister Segment = X86SegmentRegister::DS,
                    bool DefaultStack = false);
  X86MpxResult store(X86MpxMode Mode, unsigned Source,
                     std::optional<uint64_t> Address, bool Locked,
                     X86MpxMemoryProvider &Memory,
                     X86SegmentRegister Segment = X86SegmentRegister::DS,
                     bool DefaultStack = false);
  X86MpxResult loadTable(X86MpxMode Mode, unsigned Destination,
                         std::optional<uint64_t> LinearPointerAddress,
                         std::optional<uint64_t> PointerValue,
                         X86MpxMemoryProvider &Memory);
  X86MpxResult storeTable(X86MpxMode Mode, unsigned Source,
                          std::optional<uint64_t> LinearPointerAddress,
                          std::optional<uint64_t> PointerValue,
                          X86MpxMemoryProvider &Memory);

  /// Apply an original transfer's effect at its completed instruction boundary,
  /// never before a faulting target/stack access. Initialize means the native
  /// conditional BNDINIT (enabled and BNDPRESERVE clear), not an unconditional
  /// zero. Preserve also describes EB/far transfers/LOOP/J*CXZ and F2 branches.
  /// The caller must retain this effect independently of optimized CFG edges.
  X86MpxResult completeBranch(X86MpxBranch Effect);
  /// BOUND's #BR clears status only when MPX is enabled.
  X86MpxResult completeLegacyBoundsFault();

private:
  X86MpxConfiguration Configuration;
  std::array<X86MpxBounds, X86MpxRegisterCount> Bank{};
  std::optional<uint64_t> Status;

  std::optional<uint64_t> activeConfiguration() const;
  void writeBounds(X86MpxMode Mode, unsigned Register, uint64_t Lower,
                   uint64_t Upper);
};

} // namespace neverd

#endif // NEVERD_IR_X86_X86MPXCONTEXT_H
