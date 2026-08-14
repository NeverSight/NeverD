//===- RuntimeCodeMemory.h - One-way W^X code publication ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_RUNTIMECODEMEMORY_H
#define NEVERD_TRANSLATE_RUNTIMECODEMEMORY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Memory.h"

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

namespace neverd::translate {

/// Stable failure categories for generated-code memory publication.
enum class RuntimeCodeMemoryErrorCode : uint8_t {
  InvalidSize = 0,
  AllocationFailed = 1,
  IncompleteAllocation = 2,
  NotWritable = 3,
  WriteOutOfBounds = 4,
  AlreadyExecutable = 5,
  ProtectionFailed = 6,
  NotExecutable = 7,
  EntryOutOfBounds = 8,
};

class RuntimeCodeMemoryError final
    : public llvm::ErrorInfo<RuntimeCodeMemoryError> {
public:
  static char ID;

  RuntimeCodeMemoryError(RuntimeCodeMemoryErrorCode Reason,
                         std::error_code SystemError = {});

  RuntimeCodeMemoryErrorCode reason() const { return Reason; }
  std::error_code systemError() const { return SystemError; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  RuntimeCodeMemoryErrorCode Reason;
  std::error_code SystemError;
};

enum class RuntimeCodeMemoryState : uint8_t {
  Writable = 1,
  Executable = 2,
};

/// Owns one page-isolated generated-code allocation.
///
/// Allocation starts readable and writable, never executable. publish()
/// performs the only permitted transition, to readable and executable, and
/// invalidates the host instruction cache through LLVM's platform support.
/// There is no transition back and no state in which the allocation is both
/// writable and executable. Construction and publication are single-threaded;
/// immutable executable bytes may be read concurrently after publication.
class RuntimeCodeMemory {
public:
  static llvm::Expected<RuntimeCodeMemory> allocate(size_t Size);

  RuntimeCodeMemory(RuntimeCodeMemory &&Other) noexcept
      : Memory(std::move(Other.Memory)), Size(std::exchange(Other.Size, 0)),
        State(Other.State) {}
  RuntimeCodeMemory &operator=(RuntimeCodeMemory &&) = delete;
  RuntimeCodeMemory(const RuntimeCodeMemory &) = delete;
  RuntimeCodeMemory &operator=(const RuntimeCodeMemory &) = delete;

  size_t size() const { return Size; }
  size_t allocatedSize() const { return Memory.allocatedSize(); }
  RuntimeCodeMemoryState state() const { return State; }

  llvm::ArrayRef<uint8_t> bytes() const;
  llvm::Expected<llvm::MutableArrayRef<uint8_t>> writableBytes();
  llvm::Error write(size_t Offset, llvm::ArrayRef<uint8_t> Bytes);

  /// Atomically changes the page protections from RW to RX.
  llvm::Error publish();

  /// Return an executable address within the requested allocation. The page
  /// rounding tail is never addressable through this API.
  llvm::Expected<uintptr_t> entryAddress(size_t Offset = 0) const;

private:
  RuntimeCodeMemory(llvm::sys::OwningMemoryBlock Memory, size_t Size)
      : Memory(std::move(Memory)), Size(Size) {}

  llvm::sys::OwningMemoryBlock Memory;
  size_t Size = 0;
  RuntimeCodeMemoryState State = RuntimeCodeMemoryState::Writable;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_RUNTIMECODEMEMORY_H
