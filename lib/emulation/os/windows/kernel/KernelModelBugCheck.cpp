//===- KernelModelBugCheck.cpp - Bounded callback registration ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
// x64 WDK KBUGCHECK_REASON_CALLBACK_RECORD. List links, checksum and padding
// remain opaque; this model does not execute bug checks or produce dump data.
constexpr uint64_t RecordSize = 48;
constexpr uint64_t StateOffset = 44;
constexpr size_t RecordLimit = 64;
constexpr unsigned ComponentLimit = 256;
uint64_t pageBase(uint64_t Address) {
  return Address & ~(profile::PageSize - 1);
}
llvm::Error callbackError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

bool KernelModel::hasLiveBugCheckCallbacks() const {
  return std::any_of(BugCheckRecords.begin(), BugCheckRecords.end(),
                     [](const auto &Entry) { return Entry.second.Live; });
}

llvm::Error KernelModel::validateStrictBugCheckAccess(uint64_t Address,
                                                      uint32_t Size,
                                                      bool IsWrite) const {
  for (const auto &[Base, Record] : BugCheckRecords) {
    if (Address >= Base + RecordSize || Base >= Address + Size)
      continue;
    if (IsWrite)
      return callbackError("guest write to a read-only bug-check record");
    for (uint64_t I = std::max(Base, Address) - Base;
         I < std::min(Base + RecordSize, Address + Size) - Base; ++I)
      if (!((I >= 16 && I < 32) || (I >= 40 && I <= StateOffset)))
        return callbackError("guest read of an opaque bug-check record field");
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateBugCheckPageBorrow(uint64_t Address,
                                                    uint64_t Size) const {
  if (!Size)
    return llvm::Error::success();
  if (Size > UINT64_MAX - Address)
    return callbackError("overflowing bug-check page borrow");
  const uint64_t First = pageBase(Address), Last = pageBase(Address + Size - 1);
  for (const auto &[Base, Record] : BugCheckRecords)
    if (First <= pageBase(Base + RecordSize - 1) && pageBase(Base) <= Last)
      return callbackError("MDL page borrow intersects an opaque bug-check "
                           "record, including after deregistration");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateBugCheckRecord(uint64_t Record) const {
  if (Contract != ExecutionContract::CheckedX64)
    return callbackError("bug-check registration requires checked-x64 "
                         "execution");
  if (Unloaded)
    return callbackError("bug-check registration requires a live driver");
  if (!Record || Record % 8 || RecordSize > UINT64_MAX - Record ||
      !imageOwnerForRange(Record, RecordSize))
    return callbackError("bug-check record requires a complete aligned "
                         "driver-image range");
  auto Accessible = Memory.canAccess(Record, RecordSize, Read | Write);
  if (!Accessible)
    return Accessible.takeError();
  if (!*Accessible)
    return callbackError("bug-check record requires readable writable RAM");
  if (auto E = Memory.validateBacking(Record, RecordSize))
    return E;
  // hasPinnedPages means ALL covered pages. Query each page separately so a
  // partial lock cannot expose the other half of a cross-page record.
  const uint64_t Last = pageBase(Record + RecordSize - 1);
  for (uint64_t Page = pageBase(Record);; Page += profile::PageSize) {
    if (Physical.hasPinnedPages(Page, profile::PageSize))
      return callbackError("bug-check record page already has physical locks");
    if (Page == Last)
      break;
  }
  auto It = BugCheckRecords.find(Record);
  if (It == BugCheckRecords.end()) {
    if (auto E = validateGuestAccess(Record, RecordSize, false))
      return E;
    if (auto E = validateGuestAccess(Record, RecordSize, true))
      return E;
  } else {
    const auto &Known = It->second;
    for (const auto &[Offset, Expected, Width] :
         std::array<std::tuple<uint64_t, uint64_t, unsigned>, 4>{
             {{16, Known.Callback, 8},
              {24, Known.Component, 8},
              {40, Known.Reason, 4},
              {StateOffset, Known.Live ? 1u : 0u, 1}}}) {
      auto Value = Memory.readInteger(Record + Offset, Width);
      if (!Value)
        return Value.takeError();
      if (*Value != Expected)
        return callbackError("bug-check record differs from its owned state");
    }
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::registerBugCheckCallback(uint64_t Record, uint64_t Callback,
                                      uint32_t Reason, uint64_t Component) {
  if (Version && Version->Major == 10 && Version->Minor == 0 &&
      Version->Build == 19041)
    return registerBugCheck({Record, Callback, Reason, Component});

  if (auto E = validateBugCheckRecord(Record))
    return std::move(E);
  auto Existing = BugCheckRecords.find(Record);
  // Native duplicate registration returns before consulting the new tuple.
  // Neither this path nor removal reads the retained Component's contents.
  if (Existing != BugCheckRecords.end() && Existing->second.Live)
    return 0;
  auto State = Memory.readInteger(Record + StateOffset, 1);
  if (!State)
    return State.takeError();
  if (*State != 0)
    return callbackError("bug-check registration has unknown record state");
  if (Existing == BugCheckRecords.end() &&
      BugCheckRecords.size() >= RecordLimit)
    return callbackError("bug-check record identity limit exceeded");
  if (Reason < 2 || Reason > 7)
    return callbackError("unsupported bug-check callback reason");
  auto Executable = Memory.canAccess(Callback, 1, Execute);
  if (!Executable)
    return Executable.takeError();
  if (!imageOwnerForRange(Callback, 1) || !*Executable)
    return callbackError("bug-check callback requires executable driver code");
  // This bounded initial check is a model restriction, not a native read
  // effect or a promise that the caller's component storage stays alive.
  bool Terminated = false;
  for (unsigned I = 0; I < ComponentLimit; ++I) {
    if (!Component || Component > UINT64_MAX - I)
      return callbackError("invalid bug-check component pointer");
    if (auto E = validateGuestAccess(Component + I, 1, false))
      return std::move(E);
    auto Readable = Memory.canAccess(Component + I, 1, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return callbackError("bug-check component requires readable RAM");
    if (auto E = Memory.validateBacking(Component + I, 1))
      return std::move(E);
    auto Byte = Memory.readInteger(Component + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte) {
      Terminated = true;
      break;
    }
  }
  if (!Terminated)
    return callbackError("bug-check component exceeds the 256-byte bound");
  // All predictable refusals precede these writes to preflighted RAM. Do not
  // fabricate linked-list or checksum bytes. They stay inaccessible even
  // after removal, while the known tuple and State have concrete effects.
  for (const auto &[Offset, Value, Width] :
       std::array<std::tuple<uint64_t, uint64_t, unsigned>, 4>{
           {{16, Callback, 8},
            {24, Component, 8},
            {40, Reason, 4},
            {StateOffset, 1, 1}}})
    if (auto E = Memory.writeInteger(Record + Offset, Value, Width))
      return std::move(E);
  BugCheckRecords.insert_or_assign(
      Record, BugCheckRecord{Callback, Component, Reason, true});
  return 1;
}

llvm::Expected<uint64_t>
KernelModel::deregisterBugCheckCallback(uint64_t Record) {
  if (Version && Version->Major == 10 && Version->Minor == 0 &&
      Version->Build == 19041)
    return deregisterBugCheck(Record);

  if (auto E = validateBugCheckRecord(Record))
    return std::move(E);
  auto It = BugCheckRecords.find(Record);
  if (It == BugCheckRecords.end()) {
    auto State = Memory.readInteger(Record + StateOffset, 1);
    if (!State)
      return State.takeError();
    if (*State != 0)
      return callbackError("bug-check removal has unknown record state");
    return 0;
  }
  if (!It->second.Live)
    return 0;
  if (auto E = Memory.writeInteger(Record + StateOffset, 0, 1))
    return std::move(E);
  It->second.Live = false;
  return 1;
}
} // namespace neverd::emulation

//===- KernelModelBugCheck.cpp - Retained bug-check registrations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "KernelModelRuntime.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
namespace bugcheck {
#define NEVERD_KERNEL_BUGCHECK_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#include "KernelBugCheck.def"
#undef NEVERD_KERNEL_BUGCHECK_VALUE
} // namespace bugcheck
llvm::Error bugError(llvm::StringRef Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
bool overlaps(uint64_t Base, uint64_t Size, uint64_t Other, uint64_t Length) {
  return Size && Length && Base < Other + Length && Other < Base + Size;
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::registerBugCheck(llvm::ArrayRef<uint64_t> A) {
  const uint32_t Reason = uint32_t(A[2]);
  if (Reason < bugcheck::FirstReason || Reason > bugcheck::LastReason)
    return 0;
  if (BugCheckCallbacks.contains(A[0]))
    return 0;
  for (const auto &[Record, State] : BugCheckCallbacks)
    if (overlaps(A[0], bugcheck::RecordSize, Record, bugcheck::RecordSize))
      return bugError("overlapping bug-check callback records");
  if (auto E = runtime::checkBufferRange(A[0], bugcheck::RecordSize))
    return E;
  if (A[0] % 8)
    return bugError("bug-check callback record requires pointer alignment");
  if (auto E = validateGuestAccess(A[0], bugcheck::RecordSize, true))
    return E;
  auto Writable = Memory.canAccess(A[0], bugcheck::RecordSize, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return bugError("bug-check callback record requires writable storage");
  auto State = Memory.readInteger(A[0] + bugcheck::StateOffset, 1);
  if (!State)
    return State.takeError();
  if (*State != bugcheck::Empty)
    return 0;
  auto Executable = Memory.canAccess(A[1], 1, Execute);
  if (!Executable)
    return Executable.takeError();
  if (!A[1] || !*Executable || (Exports && Exports->lookup(A[1])))
    return bugError("bug-check callback requires executable guest code");
  if (auto E = runtime::checkBufferRange(A[3], bugcheck::MaxComponentBytes))
    return E;
  uint64_t ComponentSize = 0;
  bool Terminated = false;
  while (ComponentSize < bugcheck::MaxComponentBytes) {
    if (auto E = validateGuestAccess(A[3] + ComponentSize, 1, false))
      return E;
    auto C = Memory.readInteger(A[3] + ComponentSize++, 1);
    if (!C)
      return C.takeError();
    if (!*C) {
      Terminated = true;
      break;
    }
  }
  if (!Terminated)
    return bugError("bug-check component exceeds model capacity");
  if (overlaps(A[0], bugcheck::RecordSize, A[3], ComponentSize))
    return bugError("bug-check component overlaps callback record");
  if (BugCheckCallbacks.size() >= bugcheck::MaxRegistrations)
    return 0;
  std::array<uint8_t, bugcheck::RecordSize> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), A[0]);
  llvm::support::endian::write64le(Bytes.data() + 8, A[0]);
  llvm::support::endian::write64le(Bytes.data() + bugcheck::RoutineOffset,
                                   A[1]);
  llvm::support::endian::write64le(Bytes.data() + bugcheck::ComponentOffset,
                                   A[3]);
  llvm::support::endian::write32le(Bytes.data() + bugcheck::ReasonOffset,
                                   Reason);
  Bytes[bugcheck::StateOffset] = bugcheck::Inserted;
  if (auto E = Memory.write(A[0], Bytes))
    return E;
  BugCheckCallbacks.emplace(A[0],
                            BugCheckRegistration{A[1], A[3], ComponentSize});
  return 1;
}

llvm::Expected<uint64_t> KernelModel::deregisterBugCheck(uint64_t Record) {
  auto I = BugCheckCallbacks.find(Record);
  if (I == BugCheckCallbacks.end())
    return 0;
  // Kernel-owned writes bypass the guest's opaque-record access boundary.
  if (auto E = Memory.writeInteger(Record + bugcheck::StateOffset,
                                   bugcheck::Empty, 1))
    return E;
  BugCheckCallbacks.erase(I);
  return 1;
}

llvm::Error KernelModel::validateNativeBugCheckAccess(uint64_t Address,
                                                      uint32_t Size,
                                                      bool IsWrite) const {
  for (const auto &[Record, State] : BugCheckCallbacks)
    if (overlaps(Address, Size, Record, bugcheck::RecordSize)) {
      if (!IsWrite && Address == Record + bugcheck::StateOffset && Size == 1)
        continue;
      return bugError("guest access to a registered opaque bug-check record");
    }
  return llvm::Error::success();
}

llvm::Error KernelModel::canReleaseBugCheckStorage(uint64_t Base,
                                                   uint64_t Size) const {
  for (const auto &[Record, State] : BugCheckCallbacks)
    if (overlaps(Base, Size, Record, bugcheck::RecordSize) ||
        overlaps(Base, Size, State.Routine, 1))
      return bugError(
          "cannot release storage retained by a bug-check callback");
  return llvm::Error::success();
}

void KernelModel::noteBugCheckComponentRelease(uint64_t Base, uint64_t Size) {
  // Component is a borrowed diagnostic name, not an ownership reference. The
  // matched 19041 registration stores the original pointer and deregistration
  // does not dereference its bytes. Preserve that pointer, mark its original
  // storage revocation, and leave actual access to the ordinary memory guards.
  for (auto &[Record, State] : BugCheckCallbacks)
    if (overlaps(Base, Size, State.Component, State.ComponentSize))
      State.ComponentStorageReleased = true;
}

void KernelModel::snapshotBugCheckCallbacks() {
  Result.BugCheckCallbacks.clear();
  for (const auto &[Record, State] : BugCheckCallbacks)
    Result.BugCheckCallbacks.push_back({Record, State.Routine, State.Component,
                                        State.ComponentStorageReleased});
}
} // namespace neverd::emulation

namespace neverd::emulation {
llvm::Error KernelModel::validateBugCheckAccess(uint64_t Address, uint32_t Size,
                                                bool IsWrite) const {
  if (auto E = validateStrictBugCheckAccess(Address, Size, IsWrite))
    return E;
  return validateNativeBugCheckAccess(Address, Size, IsWrite);
}
} // namespace neverd::emulation
