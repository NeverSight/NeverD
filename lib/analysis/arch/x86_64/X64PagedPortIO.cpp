//===- X64PagedPortIO.cpp - Ordered port device transactions --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own TSS permission checks and element commit across the port/RAM boundary.
//===----------------------------------------------------------------------===//

#include "neverd/analysis/arch/x86_64/X64PagedPortIO.h"

#include "X64PortIOEncoding.h"

#include "llvm/Support/Errc.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::analysis {

llvm::Expected<X64PagedPortIOResult> executeX64PagedPortIO(
    X64ContinuationStateV6 &State, translate::X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Bytes, X64PagedAlignmentPriority AlignmentPriority,
    const X64PagedPortIOBinding &Binding) {
  using namespace translate;
  const auto Form = detail::x64PortIOEncoding(Bytes);
  if (!Form || !Binding.Transfer ||
      (Binding.Profile != X64PagedPortIOProfile::InputBeforeWrite &&
       Binding.Profile != X64PagedPortIOProfile::InputAfterWriteChecks &&
       Binding.Profile != X64PagedPortIOProfile::InputBeforeWriteFragments))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid paged port I/O binding");
  const auto &F = *Form;
  auto &Core = State.Core.Core.Core.Core;
  const uint8_t CPL = State.Core.CS.Selector & 3;
  const uint16_t Port =
      F.ImmediatePort ? uint16_t(*F.ImmediatePort) : uint16_t(Core.GPR[2]);
  X64PagedPortIOResult Result;
  const auto Fault = [&](uint8_t Vector) {
    Result.Access.Status = X86PageWalkStatus::Exception;
    Result.Access.Exception = X86MemoryException{Vector, 0, std::nullopt};
  };
  // The bitmap base and the two-byte permission window are implicit supervisor
  // reads. The last bitmap byte is the required all-ones terminator; accesses
  // spanning port 65535 do not wrap permission checks back to port zero.
  if (CPL > ((Core.RFlags >> 12) & 3)) {
    if ((State.Unusable & 128) || (State.TR.Attributes & 0x9f) != 0x8b)
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "port permissions require a usable 64-bit TSS cache");
    if (State.TR.Limit < 0x67) {
      Fault(13);
      return Result;
    }
    std::array<uint8_t, 2> Data{};
    auto Read = Memory.load(
        {State.TR.Base + 0x66, MemoryAccessKind::Read, 0, true}, Data);
    if (!Read)
      return Read.takeError();
    Result.Access = *Read;
    if (Read->Status != X86PageWalkStatus::Completed)
      return Result;
    const uint32_t Offset = uint32_t(Data[0] | (Data[1] << 8)) + (Port >> 3);
    if (uint64_t(Offset) + 1 > State.TR.Limit) {
      Fault(13);
      return Result;
    }
    Read = Memory.load(
        {State.TR.Base + Offset, MemoryAccessKind::Read, 0, true}, Data);
    if (!Read)
      return Read.takeError();
    Result.Access = *Read;
    if (Read->Status != X86PageWalkStatus::Completed)
      return Result;
    const unsigned Permissions = Data[0] | (Data[1] << 8);
    if ((Permissions >> (Port & 7)) & ((1u << F.Bytes) - 1)) {
      Fault(13);
      return Result;
    }
  }
  const auto AddressValue = [&](uint64_t V) {
    return F.AddressBytes == 4 ? uint64_t(uint32_t(V)) : V;
  };
  const uint64_t Count = F.Repeat ? AddressValue(Core.GPR[1]) : 1;
  if (!Count) {
    Core.GPR[1] = 0;
    Core.PC += Bytes.size();
    Result.Completed = true;
    return Result;
  }
  const uint32_t Mask = F.Bytes == 4 ? UINT32_MAX : (1u << (8 * F.Bytes)) - 1;
  uint32_t Value = Core.GPR[0] & Mask;
  std::array<uint8_t, 4> Data{};
  std::optional<X86PagedMemory::PreparedStore> Store, PrefixStore;
  unsigned PrefixBytes = 0;
  if (F.String) {
    uint64_t Linear = AddressValue(Core.GPR[F.Input ? 7 : 6]);
    if (!F.Input)
      Linear += F.Segment == 1 ? Core.FSBase : F.Segment == 2 ? Core.GSBase : 0;
    // INS/OUTS have data references even with an SS override: legacy segment
    // overrides are ignored in long mode, including the noncanonical fault.
    const X86LinearAccess Access{
        Linear, F.Input ? MemoryAccessKind::Write : MemoryAccessKind::Read,
        CPL};
    auto Check = checkX86LinearAccess(Memory.controls(), Access, F.Bytes);
    if (!Check)
      return Check.takeError();
    const bool Unaligned = CPL == 3 && (Core.RFlags & 0x40000) &&
                           (Memory.controls().CR0 & 0x40000) &&
                           (Linear & (F.Bytes - 1));
    if (*Check) {
      Result.Access.Status = X86PageWalkStatus::Exception;
      Result.Access.Exception = **Check;
    } else if (Unaligned &&
               AlignmentPriority == X64PagedAlignmentPriority::BeforePaging) {
      Fault(17);
    } else {
      if (F.Input) {
        auto Tail = Access;
        if (Binding.Profile ==
                X64PagedPortIOProfile::InputBeforeWriteFragments &&
            (Linear & 0xfff) + F.Bytes > 0x1000) {
          PrefixBytes = 0x1000 - (Linear & 0xfff);
          auto Prefix = Memory.prepareStore(Access, PrefixBytes);
          if (!Prefix)
            return Prefix.takeError();
          Result.Access = *Prefix;
          if (Prefix->Status == X86PageWalkStatus::Completed) {
            PrefixStore = std::move(*Prefix);
            Tail.Address += PrefixBytes;
          }
        }
        if (Result.Access.Status == X86PageWalkStatus::Completed) {
          auto Prepared = Memory.prepareStore(Tail, F.Bytes - PrefixBytes);
          if (!Prepared)
            return Prepared.takeError();
          Result.Access = *Prepared;
          Store = std::move(*Prepared);
        }
      } else {
        auto Read = Memory.load(
            Access, llvm::MutableArrayRef(Data).take_front(F.Bytes));
        if (!Read)
          return Read.takeError();
        Result.Access = *Read;
        Value = 0;
        for (unsigned I = 0; I != F.Bytes; ++I)
          Value |= uint32_t(Data[I]) << (8 * I);
      }
      if (Result.Access.Status == X86PageWalkStatus::Completed && Unaligned) {
        PrefixStore.reset();
        Fault(17);
      }
    }
    if (Result.Access.Status == X86PageWalkStatus::BackingFault ||
        (Result.Access.Status == X86PageWalkStatus::Exception &&
         (!F.Input ||
          Binding.Profile == X64PagedPortIOProfile::InputAfterWriteChecks)))
      return Result;
  }
  auto Transfer = Binding.Transfer(
      Binding.Context, {Port, uint8_t(F.Bytes), F.Input, F.Input ? 0 : Value});
  if (!Transfer)
    return Transfer.takeError();
  if (!*Transfer) {
    // Pending devices defer even a preflighted input fault: input has not
    // happened. Page-walk A/D effects survive, but no attempt is charged.
    Result.Access = {};
    Result.DeviceUnavailable = true;
    return Result;
  }
  Result.DeviceTransferCompleted = true;
  if (F.Input) {
    Value = **Transfer & Mask;
    for (unsigned I = 0; I != F.Bytes; ++I)
      Data[I] = Value >> (8 * I);
    if (PrefixStore) {
      Memory.commitStore(*PrefixStore,
                         llvm::ArrayRef(Data).take_front(PrefixBytes));
      Result.InputBytesWritten = PrefixBytes;
    }
  }
  if (Result.Access.Status == X86PageWalkStatus::Exception)
    return Result;
  if (F.Input) {
    if (F.String) {
      Memory.commitStore(*Store, llvm::ArrayRef(Data).slice(
                                     PrefixBytes, F.Bytes - PrefixBytes));
      Result.InputBytesWritten = F.Bytes;
    } else {
      Core.GPR[0] =
          F.Bytes == 4 ? Value : (Core.GPR[0] & ~uint64_t(Mask)) | Value;
    }
  }
  if (F.String) {
    auto &Index = Core.GPR[F.Input ? 7 : 6];
    Index = AddressValue(Index + ((Core.RFlags & 0x400) ? -uint64_t(F.Bytes)
                                                        : uint64_t(F.Bytes)));
  }
  if (F.Repeat) {
    Core.GPR[1] = Count - 1;
    Result.RepeatContinue = Count != 1;
  }
  if (!Result.RepeatContinue)
    Core.PC += Bytes.size();
  Result.Completed = !Result.RepeatContinue;
  return Result;
}

} // namespace neverd::analysis
