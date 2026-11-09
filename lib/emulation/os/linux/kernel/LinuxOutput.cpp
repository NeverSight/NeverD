//===- LinuxOutput.cpp - Captured scalar and vectored Linux output --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxKernel.h"
#include "LinuxKernelAvailability.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <variant>

namespace neverd::emulation::linux_model {
namespace {
struct OutputBuffer {
  std::variant<uint64_t, llvm::StringRef> Source;
  uint64_t Size;
};

bool userRange(const MemoryLayout &Layout, uint64_t Address, uint64_t Size) {
  return Address < Layout.UserLimit && Size <= Layout.UserLimit - Address;
}

llvm::Expected<std::optional<uint64_t>>
publishOutput(ExecutionBackend &CPU, uint64_t FD,
              llvm::ArrayRef<OutputBuffer> Buffers, const MemoryLayout &Layout,
              const ProcessOptions &Options, ProcessResult &Result) {
  uint64_t Used = Result.StandardOutput.size() + Result.StandardError.size();
  // The workload budget applies to the whole call, before any captured byte
  // is published, including when later buffers would fault or be empty.
  for (const auto &Buffer : Buffers) {
    if (Used > Options.OutputLimit ||
        Buffer.Size > Options.OutputLimit - Used) {
      Result.Stop = ProcessStopReason::OutputLimit;
      Result.Diagnostic = Output;
      return std::optional<uint64_t>();
    }
    Used += Buffer.Size;
  }
  uint64_t Written = 0;
  for (const auto &Buffer : Buffers) {
    auto &Output =
        FD == StandardOutput ? Result.StandardOutput : Result.StandardError;
    if (auto *Bytes = std::get_if<llvm::StringRef>(&Buffer.Source)) {
      if (!Bytes->empty())
        Output.append(Bytes->data(), Bytes->size());
      Written += Bytes->size();
      continue;
    }
    const uint64_t Address = std::get<uint64_t>(Buffer.Source);
    uint64_t Readable = 0;
    while (Readable < Buffer.Size) {
      const uint64_t Start = Address + Readable;
      const uint64_t Size = std::min(Buffer.Size - Readable,
                                     Layout.PageSize - Start % Layout.PageSize);
      auto Access = CPU.canAccess(Start, Size, Read | UserAccessible);
      if (!Access)
        return Access.takeError();
      if (!*Access)
        break;
      Readable += Size;
    }
    if (Readable) {
      std::string Bytes(Readable, '\0');
      if (auto E =
              CPU.read(Address, llvm::MutableArrayRef<uint8_t>(
                                    reinterpret_cast<uint8_t *>(Bytes.data()),
                                    Bytes.size())))
        return std::move(E);
      Output.append(Bytes);
      Written += Readable;
    }
    // These descriptors are virtual byte sinks. A later data fault retains
    // the copied prefix without poisoning the stopped CPU or reading ahead.
    if (Readable != Buffer.Size)
      return std::optional<uint64_t>(Written ? Written
                                             : uint64_t(0) - BadAddress);
  }
  return std::optional<uint64_t>(Written);
}
} // namespace

llvm::Expected<std::optional<uint64_t>>
writeOutputBuffers(ExecutionBackend &CPU, uint32_t FD,
                   llvm::ArrayRef<llvm::StringRef> Bytes,
                   const MemoryLayout &Layout, const ProcessOptions &Options,
                   ProcessResult &Result) {
  if (FD != StandardOutput && FD != StandardError)
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  llvm::SmallVector<OutputBuffer> Buffers;
  for (auto Text : Bytes)
    Buffers.push_back({Text, Text.size()});
  return publishOutput(CPU, FD, Buffers, Layout, Options, Result);
}

llvm::Expected<std::optional<uint64_t>>
writeOutput(ExecutionBackend &CPU, ServiceKind Kind,
            const ProcessServiceEvent &Event, const MemoryLayout &Layout,
            const ProcessOptions &Options, ProcessResult &Result,
            std::optional<uint64_t> AfterVectorImportError) {
  const auto [Descriptor, Address, Count, A3, A4, A5] = Event.Arguments;
  // Linux descriptor lookup consumes the low unsigned 32 bits for both calls.
  const uint32_t FD = Descriptor;
  if (!AfterVectorImportError && FD != StandardOutput && FD != StandardError)
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  if (Kind == ServiceKind::Write) {
    if (AfterVectorImportError)
      return AfterVectorImportError;
    if (!userRange(Layout, Address, Count))
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    if (!Count)
      return std::optional<uint64_t>(0);
    const OutputBuffer Buffer{Address, Count};
    return publishOutput(CPU, FD, Buffer, Layout, Options, Result);
  }
  assert(Kind == ServiceKind::WriteV);
  const uint32_t Entries = Count;
  if (Entries > MaxIOVectors)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  if (!Entries)
    return AfterVectorImportError.value_or(0);
  if (!userRange(Layout, Address, Entries * IOVectorSize))
    return std::optional<uint64_t>(uint64_t(0) - BadAddress);

  IOVectorImportKind Import = IOVectorImportKind::SingleBuffer;
  if (Options.LinuxKernel && Options.LinuxKernel->GKI) {
    auto Selected = gkiIOVectorImport(*Options.LinuxKernel->GKI);
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
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
  }
  // The newer importer reads and validates each entry in sequence. Both
  // policies finish metadata validation before any payload or output effect.
  llvm::SmallVector<OutputBuffer> Buffers;
  Buffers.reserve(Entries);
  for (uint32_t I = 0; I < Entries; ++I) {
    const uint64_t Slot = Address + I * IOVectorSize;
    auto Access = CPU.canAccess(Slot, IOVectorSize, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    auto Size = CPU.readInteger(Slot + IOVectorLengthOffset, PointerSize64);
    if (!Size)
      return Size.takeError();
    auto Base = CPU.readInteger(Slot, PointerSize64);
    if (!Base)
      return Base.takeError();
    if (*Size > MaxSignedIOSize)
      return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
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
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    Buffer.Size = std::min(Buffer.Size, Maximum - Total);
    Total += Buffer.Size;
  }
  // A process descriptor has no write operation, but Linux imports the iovec
  // before consulting that operation. Share this import and error ordering
  // with captured output; never inspect payloads or publish bytes for pidfds.
  if (AfterVectorImportError)
    return AfterVectorImportError;
  return publishOutput(CPU, FD, Buffers, Layout, Options, Result);
}
} // namespace neverd::emulation::linux_model
