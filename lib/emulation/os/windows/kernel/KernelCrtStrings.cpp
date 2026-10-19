//===- KernelCrtStrings.cpp - Concrete kernel CRT string semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Concrete kernel CRT string semantics.
///

#include "KernelModel.h"
#include "KernelModelRuntime.h"

namespace neverd::emulation::runtime {
namespace {
constexpr uint64_t MaxUnits = MaxCountedUnicodeBytes / 2;
llvm::Error crtError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "kernel CRT: " + Text);
}
} // namespace

llvm::Expected<uint64_t> crtStringOperation(const KernelModel &Model,
                                            GuestMemory &Memory,
                                            CrtStringOperation Operation,
                                            llvm::ArrayRef<uint64_t> A) {
  if (Operation == CrtStringOperation::LowerC) {
    const uint32_t C = uint32_t(A[0]);
    if (C > 255 && C != UINT32_MAX)
      return crtError("tolower requires an unsigned byte or EOF");
    // The pinned kernel's C-locale classification marks only A..Z uppercase.
    return uint64_t(C >= 'A' && C <= 'Z' ? C + ('a' - 'A') : C);
  }
  auto ReadUnit = [&](uint64_t At) -> llvm::Expected<uint64_t> {
    if (auto E = checkBufferRange(At, 2))
      return E;
    if (auto E = Model.validateGuestAccess(At, 2, false))
      return E;
    return Memory.readInteger(At, 2);
  };
  auto Writable = [&](uint64_t At, uint32_t Bytes) -> llvm::Error {
    if (auto E = checkBufferRange(At, Bytes))
      return E;
    if (auto E = Model.validateGuestAccess(At, Bytes, true))
      return E;
    auto Can = Memory.canAccess(At, Bytes, Write);
    if (!Can)
      return Can.takeError();
    if (!*Can)
      return crtError("destination is not writable");
    return llvm::Error::success();
  };
  if (Operation == CrtStringOperation::FindLastWide) {
    uint64_t Last = 0;
    for (uint64_t I = 0; I < MaxUnits; ++I) {
      if (A[0] > UINT64_MAX - I * 2)
        return crtError("overflowing string address");
      auto C = ReadUnit(A[0] + I * 2);
      if (!C)
        return C.takeError();
      if (*C == uint16_t(A[1]))
        Last = A[0] + I * 2;
      if (!*C)
        return Last;
    }
    return crtError("unterminated wide string exceeds model capacity");
  }
  // Windows 19041's _invalid_parameter is an original RET 0, not a callback.
  // This kernel profile returns errno values without user-CRT errno state.
  if (!A[0] || !A[1])
    return 22;
  if (A[1] > MaxUnits)
    return crtError("wide destination capacity exceeds model limit");
  if (auto E = checkBufferRange(A[0], A[1] * 2))
    return E;
  auto Invalid = [&]() -> llvm::Expected<uint64_t> {
    if (auto E = Writable(A[0], 2))
      return E;
    if (auto E = Memory.writeInteger(A[0], 0, 2))
      return E;
    return 22;
  };
  if (!A[2])
    return Invalid();
  uint64_t Length = 0;
  for (; Length < A[1]; ++Length) {
    auto C = ReadUnit(A[0] + Length * 2);
    if (!C)
      return C.takeError();
    if (!*C)
      break;
  }
  if (Length == A[1])
    return Invalid();
  const uint64_t Remaining = A[1] - Length;
  std::vector<uint8_t> Bytes;
  bool Terminated = false;
  for (uint64_t I = 0; I < Remaining; ++I) {
    if (A[2] > UINT64_MAX - I * 2)
      return crtError("overflowing source address");
    auto C = ReadUnit(A[2] + I * 2);
    if (!C)
      return C.takeError();
    Bytes.push_back(uint8_t(*C));
    Bytes.push_back(uint8_t(*C >> 8));
    if (!*C) {
      Terminated = true;
      break;
    }
  }
  if (A[2] < A[0] + A[1] * 2 && A[0] < A[2] + Bytes.size())
    return crtError("overlapping secure strings have undefined behavior");
  const uint64_t Output = A[0] + Length * 2;
  if (auto E = Writable(Output, uint32_t(Bytes.size())))
    return E;
  if (!Terminated)
    if (auto E = Writable(A[0], 2))
      return E;
  if (auto E = Memory.write(Output, Bytes))
    return E;
  if (!Terminated) {
    // Preserve the partial suffix copied by the original loop before clearing
    // the first WCHAR on ERANGE; do not replace the entire buffer with zero.
    if (auto E = Memory.writeInteger(A[0], 0, 2))
      return E;
    return 34;
  }
  return 0;
}
} // namespace neverd::emulation::runtime
