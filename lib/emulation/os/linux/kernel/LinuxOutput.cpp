//===- LinuxOutput.cpp - Captured scalar and vectored Linux output --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxKernel.h"
#include "LinuxWriteBuffers.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <variant>

namespace neverd::emulation::linux_model {
namespace {
bool userRange(const MemoryLayout &Layout, uint64_t Address, uint64_t Size) {
  return Address < Layout.UserLimit && Size <= Layout.UserLimit - Address;
}

llvm::Expected<std::optional<uint64_t>>
publishOutput(ExecutionBackend &CPU, uint64_t FD,
              llvm::ArrayRef<WriteBuffer> Buffers, const MemoryLayout &Layout,
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
  llvm::SmallVector<WriteBuffer> Buffers;
  materializeWriteBuffers(Bytes, Layout, Buffers);
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
    const WriteBuffer Buffer{Address, Count};
    return publishOutput(CPU, FD, Buffer, Layout, Options, Result);
  }
  assert(Kind == ServiceKind::WriteV);
  llvm::SmallVector<WriteBuffer> Buffers;
  auto Total = importWriteBuffers(CPU, Address, Count, Layout,
                                  Options.LinuxKernel, Buffers);
  if (!Total)
    return Total.takeError();
  if (*Total < 0)
    return std::optional<uint64_t>(*Total);
  // A process descriptor has no write operation, but Linux imports the iovec
  // before consulting that operation. Share this import and error ordering
  // with captured output; never inspect payloads or publish bytes for pidfds.
  if (AfterVectorImportError)
    return AfterVectorImportError;
  return publishOutput(CPU, FD, Buffers, Layout, Options, Result);
}
} // namespace neverd::emulation::linux_model
