//===- RuntimeCodeMemory.cpp - One-way W^X code publication --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/RuntimeCodeMemory.h"

#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <limits>
#include <system_error>

namespace neverd::translate {

char RuntimeCodeMemoryError::ID;

RuntimeCodeMemoryError::RuntimeCodeMemoryError(
    RuntimeCodeMemoryErrorCode Reason, std::error_code SystemError)
    : Reason(Reason), SystemError(SystemError) {}

void RuntimeCodeMemoryError::log(llvm::raw_ostream &OS) const {
  switch (Reason) {
  case RuntimeCodeMemoryErrorCode::InvalidSize:
    OS << "runtime code allocation size is invalid";
    break;
  case RuntimeCodeMemoryErrorCode::AllocationFailed:
    OS << "runtime code allocation failed";
    break;
  case RuntimeCodeMemoryErrorCode::IncompleteAllocation:
    OS << "runtime code allocator returned an incomplete mapping";
    break;
  case RuntimeCodeMemoryErrorCode::NotWritable:
    OS << "runtime code memory is not writable";
    break;
  case RuntimeCodeMemoryErrorCode::WriteOutOfBounds:
    OS << "runtime code write falls outside the allocation";
    break;
  case RuntimeCodeMemoryErrorCode::AlreadyExecutable:
    OS << "runtime code memory is already executable";
    break;
  case RuntimeCodeMemoryErrorCode::ProtectionFailed:
    OS << "runtime code publication failed";
    break;
  case RuntimeCodeMemoryErrorCode::NotExecutable:
    OS << "runtime code memory is not executable";
    break;
  case RuntimeCodeMemoryErrorCode::EntryOutOfBounds:
    OS << "runtime code entry falls outside the allocation";
    break;
  }
  if (SystemError)
    OS << ": " << SystemError.message();
}

std::error_code RuntimeCodeMemoryError::convertToErrorCode() const {
  return SystemError ? SystemError
                     : std::make_error_code(std::errc::invalid_argument);
}

namespace {

llvm::Error failure(RuntimeCodeMemoryErrorCode Reason,
                    std::error_code SystemError = {}) {
  return llvm::make_error<RuntimeCodeMemoryError>(Reason, SystemError);
}

} // namespace

llvm::Expected<RuntimeCodeMemory> RuntimeCodeMemory::allocate(size_t Size) {
  if (Size == 0)
    return failure(RuntimeCodeMemoryErrorCode::InvalidSize);

  const size_t PageSize = llvm::sys::Process::getPageSizeEstimate();
  if (PageSize == 0 || Size > std::numeric_limits<size_t>::max() - PageSize + 1)
    return failure(RuntimeCodeMemoryErrorCode::InvalidSize);

  std::error_code EC;
  llvm::sys::MemoryBlock Block = llvm::sys::Memory::allocateMappedMemory(
      Size, nullptr, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE,
      EC);
  if (EC)
    return failure(RuntimeCodeMemoryErrorCode::AllocationFailed, EC);
  if (!Block.base() || Block.allocatedSize() < Size) {
    if (Block.base()) {
      const std::error_code ReleaseError =
          llvm::sys::Memory::releaseMappedMemory(Block);
      if (ReleaseError)
        return failure(RuntimeCodeMemoryErrorCode::IncompleteAllocation,
                       ReleaseError);
    }
    return failure(RuntimeCodeMemoryErrorCode::IncompleteAllocation);
  }

  return RuntimeCodeMemory(llvm::sys::OwningMemoryBlock(Block), Size);
}

llvm::ArrayRef<uint8_t> RuntimeCodeMemory::bytes() const {
  return {static_cast<const uint8_t *>(Memory.base()), Size};
}

llvm::Expected<llvm::MutableArrayRef<uint8_t>>
RuntimeCodeMemory::writableBytes() {
  if (State != RuntimeCodeMemoryState::Writable)
    return failure(RuntimeCodeMemoryErrorCode::NotWritable);
  return llvm::MutableArrayRef<uint8_t>(static_cast<uint8_t *>(Memory.base()),
                                        Size);
}

llvm::Error RuntimeCodeMemory::write(size_t Offset,
                                     llvm::ArrayRef<uint8_t> Bytes) {
  llvm::Expected<llvm::MutableArrayRef<uint8_t>> Writable = writableBytes();
  if (!Writable)
    return Writable.takeError();
  if (Offset > Size || Bytes.size() > Size - Offset)
    return failure(RuntimeCodeMemoryErrorCode::WriteOutOfBounds);
  if (!Bytes.empty())
    std::memcpy(Writable->data() + Offset, Bytes.data(), Bytes.size());
  return llvm::Error::success();
}

llvm::Error RuntimeCodeMemory::publish() {
  if (State != RuntimeCodeMemoryState::Writable)
    return failure(RuntimeCodeMemoryErrorCode::AlreadyExecutable);

  const std::error_code EC = llvm::sys::Memory::protectMappedMemory(
      Memory.getMemoryBlock(),
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
  if (EC)
    return failure(RuntimeCodeMemoryErrorCode::ProtectionFailed, EC);
  State = RuntimeCodeMemoryState::Executable;
  return llvm::Error::success();
}

llvm::Expected<uintptr_t> RuntimeCodeMemory::entryAddress(size_t Offset) const {
  if (State != RuntimeCodeMemoryState::Executable)
    return failure(RuntimeCodeMemoryErrorCode::NotExecutable);
  if (Offset >= Size)
    return failure(RuntimeCodeMemoryErrorCode::EntryOutOfBounds);
  return reinterpret_cast<uintptr_t>(Memory.base()) + Offset;
}

} // namespace neverd::translate
