//===- LinuxWriteBuffers.cpp - Shared Linux vector import ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Captured streams and memory files share complete LP64 iovec import,
/// with borrowed model bytes bypassing guest-memory reads only.
#include "LinuxWriteBuffers.h"

#include "LinuxKernelAvailability.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
namespace {
bool userRange(const MemoryLayout &Layout, uint64_t Address, uint64_t Size) {
  return Address < Layout.UserLimit && Size <= Layout.UserLimit - Address;
}
} // namespace

llvm::Expected<int64_t>
importWriteBuffers(ExecutionBackend &CPU, uint64_t Address, uint32_t Entries,
                   const MemoryLayout &Layout,
                   const std::optional<LinuxKernelOptions> &Kernel,
                   llvm::SmallVectorImpl<WriteBuffer> &Buffers) {
  if (Entries > MaxIOVectors)
    return -int64_t(InvalidArgument);
  if (!Entries)
    return 0;
  if (!userRange(Layout, Address, Entries * IOVectorSize))
    return -int64_t(BadAddress);

  IOVectorImportKind Import = IOVectorImportKind::SingleBuffer;
  if (Kernel && Kernel->GKI) {
    auto Selected = gkiIOVectorImport(*Kernel->GKI);
    if (!Selected)
      return failure(KernelOptions);
    Import = *Selected;
  }
  // The older copy_from_user path consumes the complete descriptor array
  // before testing any length. Later inaccessible metadata therefore precedes
  // an earlier negative length. No payload is inspected by this preflight.
  if (Import == IOVectorImportKind::CopyAll) {
    auto Access =
        CPU.canAccess(Address, Entries * IOVectorSize, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return -int64_t(BadAddress);
  }
  // The newer importer reads and validates each entry in sequence. Both
  // policies finish metadata validation before any payload or output effect.
  Buffers.reserve(Entries);
  for (uint32_t I = 0; I < Entries; ++I) {
    const uint64_t Slot = Address + I * IOVectorSize;
    auto Access = CPU.canAccess(Slot, IOVectorSize, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return -int64_t(BadAddress);
    auto Size = CPU.readInteger(Slot + IOVectorLengthOffset, PointerSize64);
    if (!Size)
      return Size.takeError();
    auto Base = CPU.readInteger(Slot, PointerSize64);
    if (!Base)
      return Base.takeError();
    if (*Size > MaxSignedIOSize)
      return -int64_t(InvalidArgument);
    Buffers.push_back({*Base, *Size});
  }

  const uint64_t Maximum = MaxReadWriteSize & ~(Layout.PageSize - 1);
  uint64_t Total = 0;
  for (auto &Buffer : Buffers) {
    // Only the newer single-buffer path caps before access_ok. Older imports
    // and multi-vector imports check every original extent, even after the
    // cumulative cap is exhausted. Payload mappings are not probed here.
    if (Import == IOVectorImportKind::SingleBuffer && Entries == 1)
      Buffer.Size = std::min(Buffer.Size, Maximum);
    if (!userRange(Layout, std::get<uint64_t>(Buffer.Source), Buffer.Size))
      return -int64_t(BadAddress);
    Buffer.Size = std::min(Buffer.Size, Maximum - Total);
    Total += Buffer.Size;
  }
  return Total;
}

void materializeWriteBuffers(llvm::ArrayRef<llvm::StringRef> Bytes,
                             const MemoryLayout &Layout,
                             llvm::SmallVectorImpl<WriteBuffer> &Buffers) {
  uint64_t Remaining = MaxReadWriteSize & ~(Layout.PageSize - 1);
  for (auto Text : Bytes) {
    Text = Text.take_front(Remaining);
    Buffers.push_back({Text, Text.size()});
    Remaining -= Text.size();
  }
}
} // namespace neverd::emulation::linux_model
