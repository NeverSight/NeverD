//===- RegistrationOriginalImage.h - Scoped PE32 values ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Record conditional scalar facts that depend on one original PE32 layout.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONORIGINALIMAGE_H
#define NEVERD_IR_LOW_X86_REGISTRATIONORIGINALIMAGE_H

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionCommon.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowOp;
struct LowFunc;
} // namespace neverd

namespace neverd::registration_state {

/// A source-only hypothesis for one immutable image and one bound invocation.
/// The image must outlive this object and remain unchanged. No global state or
/// target-only cache may carry these facts into another invocation. Native
/// consumers still need original argument identities, preserved code and a
/// replay of the final PE; this object grants none of those contracts.
class OriginalImageFrameProof {
public:
  explicit OriginalImageFrameProof(const BinaryImage &Image) : Image(Image) {}

  /// Only two exact mapped original-image identities cancel their common
  /// load base. Generic FrameTransfer deliberately retains an unknown result.
  /// Loads additionally require the shared immutable scalar-storage authority.
  std::optional<FrameValue>
  evaluate(const LowOp &Op, const FrameTransfer &Transfer, size_t &Work);

  bool permitsRead(va_t Address, uint16_t Width, size_t &Work);
  std::optional<FrameValue> readScalar(va_t Address, uint16_t Width,
                                       size_t &Work);
  bool recordAddress(uint32_t Address, size_t &Work);
  bool recordReadAddress(const FrameValue &Address, uint16_t Width,
                         size_t &Work);
  bool recordCode(const LowFunc &Function, size_t &Work);
  bool recordCookieRead(const LowOp &Op, const FrameTransfer &Transfer,
                        size_t &Work);
  /// Record an already checked shared frame protocol without granting native
  /// relocation or installation permission.
  bool recordProtocol(const ExceptionAddressRange &Range,
                      const std::map<va_t, va_t> &Operands, size_t &Work);
  const std::map<va_t, bool> &cookieReadAddressIdentities() const {
    return CookieReadAddressIdentities;
  }
  const std::map<va_t, std::vector<uint8_t>> &protocolCode() const {
    return ProtocolCode;
  }
  const std::map<va_t, va_t> &protocolOperands() const {
    return ProtocolOperands;
  }

  /// All successfully consumed operands and storage, including reads whose
  /// bytes cannot be interpreted as a non-address scalar. Failed lookups are
  /// cached only inside this invocation and do not become dependencies.
  const std::set<uint32_t> &addresses() const { return Addresses; }
  const std::map<va_t, std::vector<uint8_t>> &code() const { return Code; }
  const std::map<va_t, va_t> &codeOperands() const { return CodeOperands; }
  const std::map<std::pair<va_t, uint16_t>, bool> &
  readAddressIdentities() const {
    return ReadAddressIdentities;
  }
  const std::map<std::pair<va_t, uint16_t>,
                 std::optional<registration_abi::ImmutableRegistrationScalar>> &
  reads() const {
    return Reads;
  }

private:
  bool hasMappedIdentity(uint32_t Address, size_t &Work);
  const registration_abi::ImmutableRegistrationScalar *
  read(va_t Address, uint16_t Width, size_t &Work);

  std::map<va_t, bool> CookieReadAddressIdentities;
  std::map<va_t, std::vector<uint8_t>> ProtocolCode;
  std::map<va_t, va_t> ProtocolOperands;
  const BinaryImage &Image;
  std::set<uint32_t> Addresses;
  std::map<uint32_t, bool> Mapped;
  std::map<va_t, std::vector<uint8_t>> Code;
  std::map<va_t, va_t> CodeOperands;
  std::map<std::pair<va_t, uint16_t>, bool> ReadAddressIdentities;
  std::map<std::pair<va_t, uint16_t>,
           std::optional<registration_abi::ImmutableRegistrationScalar>>
      Reads;
};

/// Conditional source evidence only. In particular, Call.ConstantReturn is
/// absent: an image-dependent scalar is not a layout-independent native fact.
/// Original argument binding and final-image replay remain required before
/// any native consumer can admit this invocation.
struct OriginalImageSEHInvocation {
  RegistrationSEHBoundCall Call;
  uint32_t ReturnValue = 0;
  std::map<int32_t, FrameValue> Arguments;
  std::set<uint32_t> Addresses;
  std::map<va_t, std::vector<uint8_t>> Code;
  /// Absolute root/plain helper fields, including directly addressed memory
  /// whose LowIR address identity alone does not prove a base relocation.
  std::map<va_t, va_t> CodeOperands;
  std::map<std::pair<va_t, uint16_t>,
           registration_abi::ImmutableRegistrationScalar>
      Reads;
  /// True only when every observed address of this ordinary read has exact
  /// original-image provenance. Preferred integer bits are recorded as false.
  /// Missing entries prove nothing. Actual caller and final-image binding are
  /// still separate requirements, including the cookie/runtime protocol.
  std::map<std::pair<va_t, uint16_t>, bool> ReadAddressIdentities;
  /// Cookie loads retain protocol identity; their runtime bits are never read
  /// as original-image constants. Private cookie spills are not image reads.
  std::map<va_t, bool> CookieReadAddressIdentities;
  /// Exact shared entry/exit caller and helper bytes, with decoded absolute
  /// operands. Native retention still needs complete relocation validation.
  std::map<va_t, std::vector<uint8_t>> ProtocolCode;
  std::map<va_t, va_t> ProtocolOperands;
};

std::optional<OriginalImageSEHInvocation>
proveBoundOriginalImageSEHCallee(const BinaryImage &Image, const LowOp &Op,
                                 const FrameState &Parent, size_t &Work,
                                 const std::set<int32_t> *Initialized);

/// Two fresh source hypotheses for one invocation at the same preferred base.
/// The physical CALL is authenticated only in Original; Candidate may retire
/// it. Root/plain helper bytes, arguments and scalar result must agree, while
/// header reads may change. Runtime/table dependencies, relocated identities
/// and native caller/IR admission remain separate obligations.
struct OriginalImageSEHReplay {
  OriginalImageSEHInvocation Original;
  OriginalImageSEHInvocation Candidate;
};

std::optional<OriginalImageSEHReplay>
replayBoundOriginalImageSEHCallee(const BinaryImage &Original,
                                  const BinaryImage &Candidate, const LowOp &Op,
                                  const FrameState &Parent, size_t &Work,
                                  const std::set<int32_t> *Initialized);

/// One budgeted source/candidate replay with relocated ordinary/cookie read
/// addresses, preserved shared protocols, typed scope storage and retained
/// runtime code. Actual source/LLVM caller arguments, cookie failure
/// reachability and final-writer installation still require their own owners.
/// This record grants no native ConstantReturn or dependent-fact propagation.
struct RetainedOriginalImageSEHReplay {
  OriginalImageSEHReplay Invocation;
  coff_loader::X86SEHRuntimeCode Runtime;
  ExceptionAddressRange ScopeRange;
  std::vector<uint8_t> ScopeBytes;
};

std::optional<RetainedOriginalImageSEHReplay>
replayRetainedOriginalImageSEHCallee(const BinaryImage &Original,
                                     const BinaryImage &Candidate,
                                     const LowOp &Op, const FrameState &Parent,
                                     size_t &Work,
                                     const std::set<int32_t> *Initialized);

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONORIGINALIMAGE_H
