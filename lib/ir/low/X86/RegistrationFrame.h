//===- RegistrationFrame.h - x86 EH frame values --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONFRAME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONFRAME_H

#include "neverd/Common.h"

#include <array>
#include <compare>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace neverd {
struct LowOp;
struct NdVar;
struct RegistrationRealignedFrame;
} // namespace neverd

namespace neverd::registration_state {

/// A runtime callback owns a separate invocation stack. Its offsets cannot
/// alias either source-frame coordinate, even at the same displacement.
struct CallbackFrameAddress {
  va_t Entry = InvalidVA;
  int32_t Offset = 0;
  auto operator<=>(const CallbackFrameAddress &) const = default;
};

/// Values in the PE32 address domain. Offset names the runtime establisher;
/// EntryOffset names the pre-alignment EBP; CallbackAddress names the active
/// callback's private stack. Unknown values retain possible frame provenance
/// when paths or arithmetic disagree.
struct FrameValue {
  std::optional<int32_t> Offset;
  std::optional<uint32_t> Constant;
  bool PreviousChain = false;
  bool MayBeFrame = false;
  /// An opaque call result is not a proven local/caller frame address. Keep
  /// its possible provenance for memory and escape sinks, while allowing an
  /// otherwise private preserved helper to return it to its checked caller.
  bool FrameOnlyFromCall = false;
  uint8_t SavedRegister = 0;
  bool ReturnPC = false;
  /// Exact PE32 cookie expression: Cookie ^ Frame(Offset) ^ Constant, or
  /// Cookie ^ Constant when the frame offset is absent. Other arithmetic and
  /// partial-width aliases drop this identity while retaining frame taint.
  bool SecurityCookie = false;
  std::optional<int32_t> CookieFrameOffset;
  uint32_t CookieXOR = 0;
  /// Entry EBP is independent of the realigned runtime establisher. It must
  /// never satisfy an Offset query, even when both displacements are zero.
  std::optional<int32_t> EntryOffset;
  std::optional<CallbackFrameAddress> CallbackAddress;
  /// Runtime exception pointers keep the catch invocation that owns them.
  /// Two live reference catches can have the same object-relative offset.
  std::optional<std::pair<uint32_t, uint32_t>> ExceptionObject;

  static FrameValue frame(int32_t Offset) { return {Offset, {}, false, true}; }
  static FrameValue entryFrame(int32_t Offset) {
    FrameValue Result;
    Result.MayBeFrame = true;
    Result.EntryOffset = Offset;
    return Result;
  }
  static FrameValue constant(uint32_t Value) {
    return {{}, Value, false, false};
  }
  static FrameValue callbackFrame(va_t Entry, int32_t Offset) {
    FrameValue Result;
    Result.MayBeFrame = true;
    Result.CallbackAddress = {Entry, Offset};
    return Result;
  }
  static FrameValue previousChain() { return {{}, {}, true, false}; }
  static FrameValue exceptionObject(uint32_t Try, uint32_t Catch) {
    auto Result = frame(0);
    Result.ExceptionObject = {Try, Catch};
    return Result;
  }
  auto operator<=>(const FrameValue &) const = default;
};

FrameValue join(FrameValue Left, const FrameValue &Right);

struct FrameState {
  std::array<FrameValue, 8> Registers;
  /// Flags and vector registers carry conservative byte taint. They cannot
  /// acquire an exact frame offset through a truncation or vector alias.
  std::map<uint64_t, FrameValue> OtherRegisterBytes;
  /// Calls may define any volatile flag/vector byte. Explicit subsequent
  /// writes override this default; a missing map entry is not a zero value.
  bool OtherRegistersMayBeFrame = false;
  std::map<int32_t, FrameValue> Cells;
  std::map<int32_t, FrameValue> EntryCells;
  std::map<int32_t, FrameValue> CallbackCells;
  std::set<int32_t> InitializedCallbackBytes;
  std::optional<va_t> CallbackEntry;
  size_t cellCount() const {
    return Cells.size() + EntryCells.size() + CallbackCells.size() +
           InitializedCallbackBytes.size();
  }
  /// Bound a known interval lookup by its overlapping cells. Unknown addresses
  /// still require a complete provenance scan across all frame coordinates.
  size_t memoryAccessWork(const FrameValue &Address, uint16_t Width) const;

  bool merge(const FrameState &Other);
  void forgetCellValues();

  void store(int32_t Offset, uint16_t Width, const FrameValue &Value);

  FrameValue load(std::optional<int32_t> Offset, uint16_t Width) const;
  void storeEntry(int32_t Offset, uint16_t Width, const FrameValue &Value);
  FrameValue loadEntry(int32_t Offset, uint16_t Width) const;
  void storeCallback(int32_t Offset, uint16_t Width, const FrameValue &Value);
  FrameValue loadCallback(int32_t Offset, uint16_t Width) const;
  /// Re-entering the same handler is a new invocation, not an alias of a
  /// pointer retained from an earlier callback. Old pointers keep only taint.
  void enterCallback(va_t Entry);
  void leaveCallback();
  bool callbackMemoryIsPrivate(const CallbackFrameAddress &Address,
                               uint16_t Width, bool Read = false) const;
  void trimCallbackCells();
};

/// One block transfer. Registers and frame cells survive instructions and CFG
/// edges; only the lifter's instruction-local temporaries are discarded.
class FrameTransfer {
public:
  FrameTransfer(FrameState &State, int32_t RegistrationOffset,
                va_t SecurityCookieVA = 0,
                const RegistrationRealignedFrame *Realigned = nullptr)
      : State(State), RegistrationOffset(RegistrationOffset),
        SecurityCookieVA(SecurityCookieVA), Realigned(Realigned) {}

  void beginInstruction(va_t Address);

  FrameValue read(const NdVar &Value) const;

  FrameValue evaluate(const LowOp &Op, bool Installed) const;

  bool isCookieCheck(const LowOp &Op, va_t CookieCheckVA) const;

  void write(const LowOp &Op, FrameValue Value, va_t CookieCheckVA = 0);

private:
  std::optional<FrameValue> evaluateRealignment(const LowOp &Op) const;

  FrameState &State;
  int32_t RegistrationOffset;
  va_t SecurityCookieVA;
  const RegistrationRealignedFrame *Realigned;
  std::map<uint64_t, FrameValue> Temps;
  va_t Instruction = InvalidVA;
};

} // namespace neverd::registration_state

#endif
