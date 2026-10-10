//===- DarwinFileIO.cpp - Shared scalar and vectored Darwin I/O ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original implementation of the LP64 ABI and copy ordering documented in
// docs/darwin-emulation.md. Guest buffers never become host descriptors.
#include "DarwinFiles.h"
#include "DarwinUserMemory.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
} // namespace

llvm::Expected<DarwinFiles::VectorInput>
DarwinFiles::readVectors(uint64_t Address, uint32_t Count) {
  // iovcnt is a signed int in the low carrier; all negative values exceed
  // this bound when viewed as uint32_t. Copy the complete array before I/O.
  if (!Count || Count > MaxIOVectors)
    return uint32_t(InvalidArgument);
  const uint64_t Size = uint64_t(Count) * 16;
  auto Prefix = userMemoryPrefix(Memory, Address, Size, Read);
  if (!Prefix)
    return Prefix.takeError();
  if (*Prefix != Size)
    return *Prefix ? VectorInput(diagnostic::FilePartialVectors)
                   : VectorInput(uint32_t(BadAddress));
  std::vector<uint8_t> Bytes(Size);
  if (auto E = Memory.read(Address, Bytes))
    return std::move(E);
  std::vector<Buffer> Buffers;
  Buffers.reserve(Count);
  for (uint32_t I = 0; I != Count; ++I)
    Buffers.push_back(
        {llvm::support::endian::read64le(Bytes.data() + I * 16),
         llvm::support::endian::read64le(Bytes.data() + I * 16 + 8)});
  return Buffers;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::read(Description &File, llvm::ArrayRef<Buffer> Buffers,
                  uint64_t Count, uint64_t Offset, bool Positioned,
                  ProcessResult &Result) {
  if (Offset > INT64_MAX)
    return returned(InvalidArgument, true);
  if (File.Type == Kind::Directory)
    return returned(IsDirectory, true);
  // The vnode prefix clips a request at the maximum signed offset before
  // invoking symbolic-link I/O. Other offsets never expose target bytes,
  // including zero-length requests and offsets beyond the target's length.
  if (File.Type == Kind::SymbolicLink)
    return returned(Offset == INT64_MAX ? 0 : OperationNotPermitted,
                    Offset != INT64_MAX);
  if (Count && File.Type == Kind::Input &&
      (!Options || !Options->StandardInput))
    return unsupported(Result, diagnostic::FileInput);
  const auto Bytes = File.bytes();
  const uint64_t Available =
      Bytes.size() - std::min<uint64_t>(Offset, Bytes.size());
  Count = std::min(Count, Available);
  uint64_t Done = 0;
  for (const auto &B : Buffers) {
    if (Done == Count)
      break;
    const uint64_t Size = std::min(B.Size, Count - Done);
    if (!Size)
      continue;
    auto Stored = copyout(B.Address, Bytes.slice(Offset + Done, Size),
                          diagnostic::FilePartialRead, Result);
    if (!Stored || !*Stored || (**Stored).Error)
      return Stored;
    Done += Size;
    if (!Positioned)
      File.Offset += Size;
  }
  return returned(Done);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::write(Description &File, llvm::ArrayRef<Buffer> Buffers,
                   uint64_t Count, uint64_t Offset, bool Positioned,
                   ProcessResult &Result) {
  if (File.Type != Kind::File && File.Type != Kind::SymbolicLink)
    return returned(IllegalSeek, true);
  if (Offset > INT64_MAX)
    return returned(InvalidArgument, true);
  // Clip the entire request against the original cursor before append selects
  // EOF. Repeating this decision for each vector would admit excess bytes.
  if (Offset == INT64_MAX)
    return returned(FileTooLarge, true);
  if (File.Type == Kind::SymbolicLink)
    return returned(OperationNotPermitted, true);
  Count = std::min(Count, uint64_t(INT64_MAX) - Offset);
  if (!Count)
    return returned(0);
  const bool Append = !Positioned && (File.Flags & OpenAppend);
  if (Append)
    Offset = File.bytes().size();
  const uint64_t Size = std::max<uint64_t>(File.bytes().size(), Offset + Count);
  auto Admitted = admitMutation(File, Size, Result);
  if (!Admitted || !*Admitted)
    return Admitted;

  uint64_t Readable = 0;
  for (const auto &B : Buffers) {
    if (Readable == Count)
      break;
    const uint64_t Span = std::min(B.Size, Count - Readable);
    if (!Span)
      continue;
    auto Prefix = userMemoryPrefix(Memory, B.Address, Span, Read);
    if (!Prefix)
      return Prefix.takeError();
    if (*Prefix != Span) {
      // A partial individual file copy has no established effects contract.
      // A wholly bad vector does: stop here, retaining earlier full vectors.
      if (*Prefix)
        return unsupported(Result, diagnostic::FilePartialWrite);
      break;
    }
    Readable += Span;
  }
  if (Readable) {
    const auto Previous = File.bytes();
    std::vector<uint8_t> Bytes(
        std::max<uint64_t>(Previous.size(), Offset + Readable), 0);
    std::copy(Previous.begin(), Previous.end(), Bytes.begin());
    uint64_t Done = 0;
    for (const auto &B : Buffers) {
      if (Done == Readable)
        break;
      const uint64_t Span = std::min(B.Size, Readable - Done);
      if (!Span)
        continue;
      if (auto E = Memory.read(
              B.Address,
              llvm::MutableArrayRef<uint8_t>(Bytes).slice(Offset + Done, Span)))
        return std::move(E);
      Done += Span;
    }
    publish(File, std::move(Bytes), std::pair{Offset, Readable});
    File.Flags |= FileWasWritten;
  }
  if (!Positioned && (Readable || Append))
    File.Offset = Offset + Readable;
  if (Readable != Count) {
    // Filesystem times after EFAULT are not proved by the virtual success
    // policy. Keep transferred bytes, but invalidate the complete observation.
    File.File->MetadataInvalidated = true;
    invalidateAttributes(File.File->ExtendedAttributes);
    return returned(BadAddress, true);
  }
  return returned(Readable);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::capture(Description &File, llvm::ArrayRef<Buffer> Buffers,
                     uint64_t Count, bool Vectored, ProcessResult &Result) {
  if (!Count)
    return returned(0);
  auto ValidRange = [](const Buffer &B) {
    return B.Address < UserLimit && B.Size <= UserLimit - B.Address;
  };
  // Preserve scalar address-error precedence over the model output budget.
  if (!Vectored && !ValidRange(Buffers.front()))
    return returned(BadAddress, true);
  const uint64_t Used =
      Result.StandardOutput.size() + Result.StandardError.size();
  if (Used > OutputLimit || Count > OutputLimit - Used) {
    Result.Stop = ProcessStopReason::OutputLimit;
    Result.Diagnostic = diagnostic::OutputLimit;
    return std::nullopt;
  }
  uint64_t Readable = 0;
  for (const auto &B : Buffers) {
    if (!B.Size)
      continue;
    // A span crossing the user boundary contributes no bytes. In particular,
    // permission granules must not invent a prefix inside that rejected span.
    if (!ValidRange(B))
      break;
    auto Prefix = userMemoryPrefix(Memory, B.Address, B.Size, Read);
    if (!Prefix)
      return Prefix.takeError();
    Readable += *Prefix;
    if (*Prefix != B.Size)
      break;
  }
  if (Readable) {
    std::string Bytes(Readable, '\0');
    uint64_t Done = 0;
    for (const auto &B : Buffers) {
      if (Done == Readable)
        break;
      const uint64_t Span = std::min(B.Size, Readable - Done);
      if (!Span)
        continue;
      if (auto E = Memory.read(
              B.Address,
              llvm::MutableArrayRef<uint8_t>(
                  reinterpret_cast<uint8_t *>(Bytes.data()), Bytes.size())
                  .slice(Done, Span)))
        return std::move(E);
      Done += Span;
    }
    (File.Type == Kind::Output ? Result.StandardOutput : Result.StandardError)
        .append(Bytes);
    File.Flags |= FileWasWritten;
  }
  return Readable == Count ? returned(Readable) : returned(BadAddress, true);
}
} // namespace neverd::emulation::darwin_model
