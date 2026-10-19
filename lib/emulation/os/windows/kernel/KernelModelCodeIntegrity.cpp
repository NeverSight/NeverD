//===- KernelModelCodeIntegrity.cpp - Explicit class-103 query observations
//--===//
///
/// \file
/// Explicit class-103 query observations.
///

#include "KernelModel.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
llvm::Expected<uint64_t>
KernelModel::queryCodeIntegrity(llvm::ArrayRef<uint64_t> A, bool ZwCall) {
  auto Unsupported = [](const char *Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "code integrity query: %s", Text);
  };
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return Unsupported("class 103 requires Windows 10.0.19041");
  if (auto Context = executionProcessContext();
      !ZwCall && Context && Context->PreviousMode != 0)
    return Unsupported("Nt class 103 UserMode probing is not modeled");
  // SeCodeIntegrityQueryInformation dispatches to CI before checking lengths.
  // This observation declares a successful provider. Missing input must not
  // invent callback availability, failure, boot policy or host CI flags.
  if (!CodeIntegrityOptions.has_value())
    return Unsupported("requires code_integrity_options");
  constexpr uint32_t Size = 8;
  const uint32_t Length = uint32_t(A[2]);
  bool Exact = false;
  if (Length >= Size) {
    if (auto E = objectBuffer(A[1], 4, false))
      return E;
    auto Storage = Memory.pinBacking(A[1], 4);
    if (!Storage)
      return Storage.takeError();
    auto Declared = Memory.readInteger(A[1], 4);
    if (!Declared)
      return Declared.takeError();
    Exact = *Declared == Size && Length == Size;
  }
  auto Writable = [&](uint64_t Address) -> llvm::Error {
    if (auto E = objectBuffer(Address, 4, true))
      return E;
    auto Storage = Memory.pinBacking(Address, 4);
    if (!Storage)
      return Storage.takeError();
    return llvm::Error::success();
  };
  if (Exact)
    if (auto E = Writable(A[1] + 4))
      return E;
  if (A[3])
    if (auto E = Writable(A[3]))
      return E;
  if (Exact) {
    // The caller-owned Length is read-only input. Return only the final DWORD,
    // including bits unknown to an older SDK, without claiming CI enforcement.
    if (auto E = Memory.writeInteger(A[1] + 4, *CodeIntegrityOptions, 4))
      return E;
    APICallDetail =
        "code_integrity_options=0x" + llvm::utohexstr(*CodeIntegrityOptions);
  }
  // Native CI sets internal required length to 8 on all these provider paths;
  // the kernel epilogue commits caller ReturnLength last, including overlap.
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], Size, 4))
      return E;
  return uint64_t(Exact ? 0 : 0xc0000004);
}
} // namespace neverd::emulation
