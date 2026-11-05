//===- X86PortIO.cpp - Ordered device and RAM effects ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own permission-window reads and input publication across execution modes.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86PortIO.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate {

llvm::Error validateX86PortIOBinding(const X86PortIOBinding &B) {
  if (!B.Transfer || (B.Profile != X86PortIOProfile::InputBeforeWrite &&
                      B.Profile != X86PortIOProfile::InputAfterWriteChecks &&
                      B.Profile != X86PortIOProfile::InputBeforeWriteFragments))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid paged port I/O binding");
  return llvm::Error::success();
}

llvm::Expected<X86MemoryAccessResult>
checkX86PortBitmap(X86PagedMemory &M, uint64_t Base, uint32_t Limit,
                   uint16_t Port, uint8_t Bytes, bool Wrap32) {
  X86MemoryAccessResult R;
  const auto Deny = [&] {
    R.Status = X86PageWalkStatus::Exception;
    R.Exception = X86MemoryException{13, 0, {}};
  };
  if (Limit < 0x67) {
    Deny();
    return R;
  }
  std::array<uint8_t, 2> Data{};
  const auto Read = [&](uint32_t Offset) {
    const auto Address =
        Wrap32 ? uint64_t(uint32_t(Base + Offset)) : Base + Offset;
    return M.load({Address, MemoryAccessKind::Read, 0, true, false, Wrap32},
                  Data);
  };
  auto Got = Read(0x66);
  if (!Got)
    return Got.takeError();
  R = std::move(*Got);
  if (R.Status != X86PageWalkStatus::Completed)
    return R;
  const uint32_t Offset = uint32_t(Data[0] | (Data[1] << 8)) + (Port >> 3);
  if (uint64_t(Offset) + 1 > Limit) {
    Deny();
    return R;
  }
  Got = Read(Offset);
  if (!Got)
    return Got.takeError();
  Got->SelfModifications.insert(Got->SelfModifications.begin(),
                                R.SelfModifications.begin(),
                                R.SelfModifications.end());
  R = std::move(*Got);
  if (R.Status == X86PageWalkStatus::Completed &&
      ((unsigned(Data[0] | (Data[1] << 8)) >> (Port & 7)) &
       ((1u << Bytes) - 1)))
    Deny();
  return R;
}

llvm::Expected<X86PortIOResult>
transferX86PortIO(X86PagedMemory &M, const X86PortIOBinding &B,
                  X86PortIORequest Request,
                  const std::optional<X86PortMemoryOperand> &Operand) {
  X86PortIOResult R;
  const unsigned N = Request.Bytes;
  const uint32_t Mask = N == 4 ? UINT32_MAX : (1u << (8 * N)) - 1;
  std::array<uint8_t, 4> Data{};
  std::optional<X86PagedMemory::PreparedStore> Store, PrefixStore;
  unsigned PrefixBytes = 0;
  const auto Accept = [&](const X86MemoryAccessResult &Access) {
    R.Access.SelfModifications.insert(R.Access.SelfModifications.end(),
                                      Access.SelfModifications.begin(),
                                      Access.SelfModifications.end());
    R.Access.Status = Access.Status;
    R.Access.Exception = Access.Exception;
    R.Access.BackingFault = Access.BackingFault;
  };
  if (Operand) {
    const auto &A = Operand->Access;
    if (Operand->Fault) {
      R.Access.Status = X86PageWalkStatus::Exception;
      R.Access.Exception = Operand->Fault;
    } else {
      if (Request.Input) {
        auto Tail = A;
        if (B.Profile == X86PortIOProfile::InputBeforeWriteFragments &&
            (A.Address & 0xfff) + N > 0x1000) {
          PrefixBytes = 0x1000 - (A.Address & 0xfff);
          auto Prefix = M.prepareStore(A, PrefixBytes);
          if (!Prefix)
            return Prefix.takeError();
          Accept(*Prefix);
          if (Prefix->Status == X86PageWalkStatus::Completed) {
            PrefixStore = std::move(*Prefix);
            Tail.Address = A.Wrap32
                               ? uint64_t(uint32_t(A.Address + PrefixBytes))
                               : A.Address + PrefixBytes;
          }
        }
        if (R.Access.Status == X86PageWalkStatus::Completed) {
          auto Prepared = M.prepareStore(Tail, N - PrefixBytes);
          if (!Prepared)
            return Prepared.takeError();
          Accept(*Prepared);
          Store = std::move(*Prepared);
        }
      } else {
        auto Read = M.load(A, llvm::MutableArrayRef(Data).take_front(N));
        if (!Read)
          return Read.takeError();
        Accept(*Read);
        Request.Value = 0;
        for (unsigned I = 0; I != N; ++I)
          Request.Value |= uint32_t(Data[I]) << (8 * I);
      }
      if (R.Access.Status == X86PageWalkStatus::Completed &&
          Operand->AlignmentFaultAfterPaging) {
        PrefixStore.reset();
        R.Access.Status = X86PageWalkStatus::Exception;
        R.Access.Exception = X86MemoryException{17, 0, {}};
      }
    }
    if (R.Access.Status == X86PageWalkStatus::BackingFault ||
        (R.Access.Status == X86PageWalkStatus::Exception &&
         (!Request.Input ||
          B.Profile == X86PortIOProfile::InputAfterWriteChecks)))
      return R;
  }
  Request.Value = Request.Input ? 0 : Request.Value & Mask;
  auto Transfer = B.Transfer(B.Context, Request);
  if (!Transfer)
    return Transfer.takeError();
  if (!*Transfer) {
    // Preparation's RAM effects remain visible, but no device input occurred.
    R.Access.Status = X86PageWalkStatus::Completed;
    R.Access.Exception.reset();
    R.DeviceUnavailable = true;
    return R;
  }
  R.DeviceTransferCompleted = true;
  if (Request.Input) {
    R.InputValue = **Transfer & Mask;
    for (unsigned I = 0; I != N; ++I)
      Data[I] = R.InputValue >> (8 * I);
    if (PrefixStore) {
      M.commitStore(*PrefixStore, llvm::ArrayRef(Data).take_front(PrefixBytes));
      R.InputBytesWritten = PrefixBytes;
    }
  }
  if (R.Access.Status == X86PageWalkStatus::Exception)
    return R;
  if (Request.Input && Operand) {
    M.commitStore(*Store,
                  llvm::ArrayRef(Data).slice(PrefixBytes, N - PrefixBytes));
    R.InputBytesWritten = N;
  }
  R.Completed = true;
  return R;
}

} // namespace neverd::translate
