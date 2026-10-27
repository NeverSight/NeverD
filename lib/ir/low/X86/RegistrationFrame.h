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
#include <cstdint>
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

/// Read-only runtime storage belonging to one SEH search-filter invocation.
/// These coordinates are independent of the establisher and callback stacks.
struct SEHExceptionAddress {
  enum class Kind : uint8_t { Pointers, Record } Storage = Kind::Pointers;
  va_t Filter = InvalidVA;
  int32_t Offset = 0;
  auto operator<=>(const SEHExceptionAddress &) const = default;
};

/// The current thread block, named by the exact FS Self read. It is neither
/// the registration chain nor a source-frame or callback-frame coordinate.
struct SEHThreadAddress {
  va_t RootAddress = InvalidVA;
  int RootOpSeq = -1;
  int32_t Offset = 0;
  auto operator<=>(const SEHThreadAddress &) const = default;
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
  std::optional<SEHExceptionAddress> SEHException;
  /// A borrowed live jump buffer is opaque to an inspected callee. Only an
  /// authenticated resume consumes it; source memory/arithmetic gets no
  /// establisher coordinate from this identity.
  std::optional<va_t> JumpCapture;
  /// ExceptionCode/ExceptionFlags and checked callback image reads can contain
  /// arbitrary address bits, so they retain alias taint. Their scalar value is
  /// independent of regenerated frame placement. Mixing with any other frame
  /// provenance drops this fact; it never grants dereference authority.
  bool FrameOnlyFromSEHScalar = false;
  /// A complete address retains its preferred-image identity for checked
  /// memory/runtime consumers. Its bits are not a fixed integer after loading
  /// or rewriting the image, and cannot serve as an affine frame displacement.
  std::optional<uint32_t> ImageAddress;
  /// An EH4 scope-table address XORed with the security cookie. CookieXOR
  /// contains only the additional stable integer mask.
  std::optional<uint32_t> CookieImageXOR;
  /// A runtime callable loaded from an authenticated eager import slot.
  /// This names the provider binding, never the callable's address bits.
  std::optional<uint32_t> ImportTarget;
  std::optional<SEHThreadAddress> ThreadAddress;

  std::optional<uint32_t> preferredAddress() const {
    return ImageAddress ? ImageAddress : Constant;
  }

  std::optional<uint32_t> cookieAddress() const {
    if (!SecurityCookie || CookieFrameOffset)
      return std::nullopt;
    return CookieImageXOR ? (CookieXOR ? std::nullopt : CookieImageXOR)
                          : std::optional<uint32_t>{CookieXOR};
  }

  bool isFrameIndependentScalar() const {
    return !MayBeFrame || FrameOnlyFromSEHScalar;
  }

  static FrameValue taint(bool MayBeFrame, bool SEHScalar = false) {
    FrameValue Result;
    Result.MayBeFrame = MayBeFrame;
    Result.FrameOnlyFromSEHScalar = MayBeFrame && SEHScalar;
    return Result;
  }

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
  static FrameValue imageAddress(uint32_t Value) {
    FrameValue Result;
    Result.ImageAddress = Value;
    return Result;
  }
  static FrameValue importTarget(uint32_t Slot) {
    FrameValue Result;
    Result.ImportTarget = Slot;
    return Result;
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
  static FrameValue sehException(SEHExceptionAddress Address) {
    FrameValue Result;
    Result.MayBeFrame = true;
    Result.SEHException = Address;
    return Result;
  }
  static FrameValue threadBlock(SEHThreadAddress Address) {
    FrameValue Result;
    Result.ThreadAddress = Address;
    return Result;
  }
  auto operator<=>(const FrameValue &) const = default;
};

FrameValue join(FrameValue Left, const FrameValue &Right);

/// Drop fully deallocated words. A word crossing the new stack boundary loses
/// its exact identity but retains taint in the bytes that remain live.
void discardFrameCellsBelow(std::map<int32_t, FrameValue> &Cells,
                            int32_t Boundary);

/// A checked PE32 scalar call defines either its instruction-local result or
/// the complete physical EAX. Other register outputs carry no return authority.
bool hasX86ScalarCallOutput(const LowOp &Op);

/// Complete PE32 GPR indices that a bound invocation may refine. Saved
/// nonvolatile identities and physical frame registers retain entry ownership.
bool isX86VolatileFrameRegister(unsigned Index);

/// Change invocation domains after a complete call proof. Exact scalars and
/// image identities survive; callee-owned identities become opaque returned
/// aliases. The borrow owner separately remaps authenticated caller objects.
FrameValue projectX86CallReturnValue(const FrameValue &Value);

struct FrameState {
  std::array<FrameValue, 8> Registers;
  /// Unknown calls or possibly overlapping writes prevent future cached-IAT
  /// load facts. Cached values have a separate call-preservation obligation.
  bool ImportBindingsIntact = true;
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
  std::optional<va_t> SEHFilterEntry;
  size_t cellCount() const {
    return Cells.size() + EntryCells.size() + CallbackCells.size() +
           InitializedCallbackBytes.size();
  }
  /// Bound a known interval lookup by its overlapping cells. Unknown addresses
  /// still require a complete provenance scan across all frame coordinates.
  size_t memoryAccessWork(const FrameValue &Address, uint16_t Width,
                          bool Write = false) const;

  /// A lookup key only; complete frame equality still proves identity.
  size_t snapshotHash() const;

  bool operator==(const FrameState &) const = default;
  bool merge(const FrameState &Other);
  void forgetCellValues();
  /// A possible write drops exact values but retains pointer provenance from
  /// paths on which the store does not execute. Definite stores use store().
  void forgetCellValues(int32_t Offset, uint32_t Width);

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
  void enterSEHFilter(va_t Entry, int32_t ExceptionPointersOffset);
  /// Only the record link and the scalar code/flags are modeled. An absent
  /// value means this invocation, field, or access width has no authority.
  std::optional<FrameValue> sehExceptionLoad(const SEHExceptionAddress &Address,
                                             uint16_t Width) const;
  bool callbackMemoryIsPrivate(const CallbackFrameAddress &Address,
                               uint16_t Width, bool Read = false) const;
  /// Returning to the runtime requires this invocation's untouched return
  /// slot and the owning function's established frame register.
  bool callbackStackIsRestored(int32_t ParentFrameOffset) const;
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

  FrameValue evaluate(const LowOp &Op, bool Installed,
                      std::optional<va_t> ImportLoad = std::nullopt) const;

  void invalidateImportBindings(const LowOp &Op,
                                const std::set<va_t> &ImportSlots);

  bool isCookieCheck(const LowOp &Op, va_t CookieCheckVA) const;

  /// Cached callables survive only a separately authenticated import ABI.
  /// Returning-stack facts for arbitrary local callees supply no such fact.
  /// Keeping a live slot binding additionally needs its own call effect.
  void write(const LowOp &Op, FrameValue Value, va_t CookieCheckVA = 0,
             bool PreserveImportedTargets = false,
             bool PreserveImportBindings = false);

private:
  std::optional<FrameValue> evaluateRelativeScalar(const LowOp &Op) const;
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
