//===- KernelModelPersistentDump.cpp - Guarded guest capture metadata ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guarded guest capture metadata.
///

#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "KernelReferenceImage.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace dump {
#define NEVERD_PERSISTENT_DUMP_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#include "KernelPersistentDump.def"
#undef NEVERD_PERSISTENT_DUMP_VALUE
} // namespace dump
namespace {
llvm::Error dumpError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::capturePersistentState(llvm::ArrayRef<uint64_t> A) {
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return dumpError("persistent thread capture requires the explicit Windows "
                     "10.0.19041 metadata profile");
  if (A[1]) {
    auto Thread = currentThreadObject();
    if (!Thread)
      return Thread.takeError();
    if (A[1] != *Thread)
      return dumpError("persistent thread capture requires the current thread "
                       "identity; other thread state is unmodeled");
  }
  if (!A[7])
    return 0; // Native null-output failure does not touch Context.
  const auto Allocation = Allocations.find(A[7]);
  if (Allocation == Allocations.end() || !Allocation->second.NonPaged ||
      Allocation->second.Size < dump::Size)
    return dumpError("persistent thread dump requires a live nonpaged pool "
                     "allocation of at least 0x40000 bytes");
  if (auto E = runtime::checkBufferRange(A[0], dump::ContextSize))
    return E;
  if (A[0] % 16)
    return dumpError("persistent thread context requires 16-byte alignment");
  if (A[0] < A[7] + dump::Size && A[7] < A[0] + dump::ContextSize)
    return dumpError("persistent thread context overlaps its output dump");
  if (auto E = validateGuestAccess(A[0], dump::ContextSize, false))
    return E;
  std::array<uint8_t, dump::ContextSize> Context;
  if (auto E = Memory.read(A[0], Context))
    return E;
  // Existing MDL aliases must not expose bytes outside the bounded dump view.
  // Later MDL admission goes through validateGuestAccess and the same mask.
  if (auto E = Physical.canRetire(A[7]))
    return E;
  for (const auto &[Address, MDL] : MDLs)
    if (MDL.Pool == A[7])
      return dumpError("persistent dump output is retained by an MDL");
  CapturingPersistentDump = A[7];
  auto Reset = llvm::make_scope_exit([&] { CapturingPersistentDump = 0; });
  if (auto E = validateGuestAccess(A[7], dump::Size, true))
    return E;
  auto Writable = Memory.canAccess(A[7], dump::Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return dumpError("persistent thread dump buffer is not writable");
  std::vector<uint8_t> Bytes(dump::Size, 0);
  for (uint64_t I = 0; I < dump::HeaderPatternEnd; I += 4)
    llvm::support::endian::write32le(Bytes.data() + I, 0x45474150); // PAGE
  auto Word = [&](uint64_t Offset, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + Offset, Value);
  };
  auto Pointer = [&](uint64_t Offset, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
  Word(4, 0x34365544); // DU64
  Word(8, 15);         // Capture dump format version, not OS major version.
  Word(12, Version->Build);
  Word(0x30, 0x8664);
  Word(0x34, 1); // The existing driver environment executes on CPU0.
  Word(0x38, uint32_t(A[2]));
  for (unsigned I = 0; I != 4; ++I)
    Pointer(0x40 + I * 8, A[3 + I]);
  std::copy(Context.begin(), Context.end(), Bytes.begin() + dump::Context);
  Word(dump::Exception, 0x80000003);
  Word(dump::Exception + 4, 1);
  Pointer(dump::Exception + 8, 0);
  Pointer(dump::Exception + 16,
          llvm::support::endian::read64le(Context.data() + dump::ContextPC));
  Word(dump::Exception + 24, 0);
  Word(0xf98, 4);
  Pointer(0xfa0, dump::Size);
  Word(dump::Control, 0); // The explicit version profile has no service pack.
  Word(dump::Control + 4, dump::Size);
  Word(dump::Control + 8, dump::Trailer);
  Word(dump::Control + 12, dump::Context);
  Word(dump::Control + 16, dump::Exception);
  if (!DebuggerData.empty()) {
    const auto &Facts = *DebuggerReference->debugger();
    Pointer(0x80, Facts.Address);
    Word(0x2070, 0x2080);
    Word(0x2074, DebuggerData.size());
    std::copy(DebuggerData.begin(), DebuggerData.end(), Bytes.begin() + 0x2080);
  }
  Word(dump::Trailer, 0x44475254); // TRGD
  if (auto E = Memory.write(A[7], Bytes))
    return E;
  PersistentDumps.insert(A[7]);
  return dump::Size;
}

llvm::Error KernelModel::validatePersistentDumpAccess(uint64_t Address,
                                                      uint32_t Size,
                                                      bool IsWrite) const {
  for (uint64_t Base : PersistentDumps) {
    if (Address >= Base + dump::Size || Base >= Address + Size)
      continue;
    if (IsWrite && CapturingPersistentDump == Base)
      continue;
    struct Range {
      uint64_t Offset, Size;
    };
    static constexpr Range Ranges[] = {
#define NEVERD_PERSISTENT_DUMP_RANGE(Offset, Size) {Offset, Size},
#include "KernelPersistentDump.def"
#undef NEVERD_PERSISTENT_DUMP_RANGE
    };
    bool Allowed = false;
    const uint64_t First = std::max(Address, Base) - Base;
    const uint64_t Last = std::min(Address + Size, Base + dump::Size) - Base;
    if (!DebuggerData.empty() &&
        ((First >= 0x2080 && Last <= 0x2080 + DebuggerData.size()) ||
         (First >= 0x80 && Last <= 0x88) ||
         (First >= 0x2070 && Last <= 0x2078)))
      continue;
    for (const auto &R : Ranges)
      if (First >= R.Offset && Last <= R.Offset + R.Size) {
        Allowed = true;
        break;
      }
    if (!Allowed) {
      uint64_t Unknown = First;
      for (const auto &R : Ranges)
        if (Unknown >= R.Offset && Unknown < R.Offset + R.Size)
          Unknown = std::min(Last, R.Offset + R.Size);
      const char *Kind = Unknown >= 0x2080 && Unknown < 0x2400
                             ? "KD debugger data"
                             : "kernel state";
      return dumpError("access to unmodeled persistent thread dump data at "
                       "offset 0x" +
                       llvm::utohexstr(Unknown) + " (" + Kind + ")");
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
