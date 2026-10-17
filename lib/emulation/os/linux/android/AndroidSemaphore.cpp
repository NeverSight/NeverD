//===- AndroidSemaphore.cpp - Guest-owned API 28 semaphore state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Models API 28 unnamed semaphore state in guest memory.

#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
namespace {
// Independently implemented from the Android 9 LP64 ABI; see the versioned
// semaphore.cpp and semaphore.h references in android-native-emulation.md.
constexpr uint32_t Shared = 1, Contended = 0xfffffffe, Maximum = 0x3fffffff;
constexpr unsigned WordBytes = 4;
} // namespace

llvm::Error Bionic::semaphoreAccess(uint64_t Address, unsigned Permissions) {
  if (Address % WordBytes)
    return failure(diagnostic::SemaphoreAlignment);
  return access(Address, WordBytes, Permissions);
}

llvm::Expected<uint32_t> Bionic::readSemaphore(uint64_t Address) {
  if (auto E = semaphoreAccess(Address, Read))
    return std::move(E);
  uint8_t Bytes[WordBytes];
  if (auto E = CPU.read(Address, Bytes))
    return std::move(E);
  const uint32_t Word = llvm::support::endian::read32le(Bytes);
  if ((Word >> 1) > Maximum && (Word & ~Shared) != Contended)
    return failure(diagnostic::SemaphoreState);
  return Word;
}

llvm::Error Bionic::writeSemaphore(uint64_t Address, uint32_t Word) {
  if (auto E = semaphoreAccess(Address, Write))
    return E;
  uint8_t Bytes[WordBytes];
  llvm::support::endian::write32le(Bytes, Word);
  return CPU.write(Address, Bytes);
}

BionicResult Bionic::semaphoreWait(uint64_t Address, uint32_t Word,
                                   bool TryOnly) {
  const uint32_t Count = Word >> 1;
  if (Count && Count <= Maximum) {
    if (auto E = writeSemaphore(Address, Word - 2))
      return std::move(E);
    return value(0);
  }
  if (TryOnly)
    return kernelResult(uint64_t(0) - linux_model::TryAgain);
  if (!Threads || !Threads->enabled()) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = diagnostic::SemaphoreBlocking;
    return std::optional<BionicValue>();
  }
  if (auto E = writeSemaphore(Address, Contended | (Word & Shared)))
    return std::move(E);
  Threads->waitSemaphore(Address, Word & Shared);
  return std::optional<BionicValue>(GuestThreadWait{});
}

BionicResult Bionic::resumeSemaphore(uint64_t Address, bool SharedFlag) {
  auto Word = readSemaphore(Address);
  if (!Word)
    return Word.takeError();
  if (bool(*Word & Shared) != SharedFlag)
    return failure(diagnostic::SemaphoreWaitState);
  // Waking grants no token: a runnable caller may have consumed it already.
  return semaphoreWait(Address, *Word, false);
}

BionicResult Bionic::semaphore(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (Call.Name == symbol::SemaphoreDestroy)
    return value(0); // API 28 does not dereference or change sem_t here.
  if (Call.Name == symbol::SemaphoreInit) {
    const uint32_t Count = A[2];
    if (Count > Maximum)
      return kernelResult(uint64_t(0) - linux_model::InvalidArgument);
    if (auto E = writeSemaphore(A[0], (Count << 1) | (uint32_t(A[1]) != 0)))
      return std::move(E);
    return value(0);
  }
  auto Word = readSemaphore(A[0]);
  if (!Word)
    return Word.takeError();
  if (Call.Name == symbol::SemaphoreWait ||
      Call.Name == symbol::SemaphoreTryWait)
    return semaphoreWait(A[0], *Word, Call.Name == symbol::SemaphoreTryWait);
  if (Call.Name == symbol::SemaphoreGetValue) {
    const uint32_t Count = *Word >> 1;
    if (auto E = writeSemaphore(A[1], Count > Maximum ? 0 : Count))
      return std::move(E);
    return value(0);
  }
  assert(Call.Name == symbol::SemaphorePost);
  if ((*Word >> 1) == Maximum)
    return kernelResult(uint64_t(0) - linux_model::ValueOverflow);
  const bool HadWaiters = (*Word & ~Shared) == Contended;
  if (auto E =
          writeSemaphore(A[0], HadWaiters ? 2 | (*Word & Shared) : *Word + 2))
    return std::move(E);
  if (HadWaiters && Threads)
    Threads->wakeSemaphore(A[0]);
  return value(0);
}
} // namespace neverd::emulation::android_model
