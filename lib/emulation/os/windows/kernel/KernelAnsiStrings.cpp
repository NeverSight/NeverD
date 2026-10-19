//===- KernelAnsiStrings.cpp - Counted byte uppercase operations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Counted byte uppercase operations.
///

#include "KernelModel.h"
#include "KernelModelRuntime.h"

#include <algorithm>

namespace neverd::emulation::runtime {
namespace {
class AnsiAccess {
  const KernelModel &Model;
  GuestMemory &Memory;

public:
  AnsiAccess(const KernelModel &Model, GuestMemory &Memory)
      : Model(Model), Memory(Memory) {}
  llvm::Error check(uint64_t Address, uint64_t Size, bool Writing) const {
    if (auto E = checkBufferRange(Address, Size))
      return E;
    if (!Size)
      return llvm::Error::success();
    if (auto E = Model.validateGuestAccess(Address, Size, Writing))
      return E;
    auto Allowed = Memory.canAccess(Address, Size, Writing ? Write : Read);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "ANSI operation requires %s ordinary RAM",
                                     Writing ? "writable" : "readable");
    auto Storage = Memory.pinBacking(Address, Size);
    if (!Storage)
      return Storage.takeError();
    return llvm::Error::success();
  }
  llvm::Expected<uint64_t> read(uint64_t Address, unsigned Size) const {
    if (auto E = check(Address, Size, false))
      return std::move(E);
    return Memory.readInteger(Address, Size);
  }
};
} // namespace
llvm::Expected<uint64_t> upperAnsiString(const KernelModel &Model,
                                         GuestMemory &Memory,
                                         llvm::ArrayRef<uint64_t> A) {
  const AnsiAccess RAM(Model, Memory);
  for (uint64_t Record : A)
    if (auto E = checkBufferRange(Record, 16))
      return std::move(E);
  // Capture only the fields used by the native routine. Source MaximumLength,
  // previous destination Length and record padding are not input to this API.
  auto Length = RAM.read(A[1], 2);
  if (!Length)
    return Length.takeError();
  auto Capacity = RAM.read(A[0] + 2, 2);
  if (!Capacity)
    return Capacity.takeError();
  auto Destination = RAM.read(A[0] + 8, 8);
  if (!Destination)
    return Destination.takeError();
  auto Source = RAM.read(A[1] + 8, 8);
  if (!Source)
    return Source.takeError();
  const uint64_t Count = std::min(*Length, *Capacity);
  // Preflight every consumed byte before publishing the caller-owned length.
  // A zero count does not access either buffer, even when its pointer is NULL.
  if (auto E = RAM.check(*Source, Count, false))
    return std::move(E);
  if (auto E = RAM.check(*Destination, Count, true))
    return std::move(E);
  if (auto E = RAM.check(A[0], 2, true))
    return std::move(E);
  if (auto E = Memory.writeInteger(A[0], Count, 2))
    return std::move(E);
  // Forward byte order is observable for overlapping buffers and aliases.
  // Read after earlier writes, including a buffer that overlaps Length itself.
  for (uint64_t I = 0; I < Count; ++I) {
    auto Byte = Memory.readInteger(*Source + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (auto E = Memory.writeInteger(*Destination + I,
                                     upperAnsiByte(uint8_t(*Byte)), 1))
      return std::move(E);
  }
  return 0;
}
llvm::Expected<uint64_t> compareAnsiStrings(const KernelModel &Model,
                                            GuestMemory &Memory,
                                            llvm::ArrayRef<uint64_t> A) {
  const AnsiAccess RAM(Model, Memory);
  for (uint64_t Record : A.take_front(2))
    if (auto E = checkBufferRange(Record, 16))
      return std::move(E);
  auto LeftLength = RAM.read(A[0], 2);
  if (!LeftLength)
    return LeftLength.takeError();
  auto RightLength = RAM.read(A[1], 2);
  if (!RightLength)
    return RightLength.takeError();
  auto Left = RAM.read(A[0] + 8, 8);
  if (!Left)
    return Left.takeError();
  auto Right = RAM.read(A[1] + 8, 8);
  if (!Right)
    return Right.takeError();
  const uint64_t Count = std::min(*LeftLength, *RightLength);
  for (uint64_t Buffer : {*Left, *Right})
    if (auto E = checkBufferRange(Buffer, Count))
      return std::move(E);
  for (uint64_t I = 0; I < Count; ++I) {
    auto L = RAM.read(*Left + I, 1);
    if (!L)
      return L.takeError();
    auto R = RAM.read(*Right + I, 1);
    if (!R)
      return R.takeError();
    if (*L == *R)
      continue;
    const int32_t Difference =
        uint8_t(A[2])
            ? int32_t(upperAnsiByte(uint8_t(*L))) - upperAnsiByte(uint8_t(*R))
            : int32_t(*L) - int32_t(*R);
    if (Difference)
      return uint64_t(uint32_t(Difference));
  }
  return uint64_t(uint32_t(int32_t(*LeftLength) - int32_t(*RightLength)));
}

llvm::Expected<uint64_t> appendAnsiString(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> A) {
  const AnsiAccess RAM(Model, Memory);
  auto SourceLength = RAM.read(A[1], 2);
  if (!SourceLength)
    return SourceLength.takeError();
  // The native empty-source path does not access the destination at all.
  if (!*SourceLength)
    return 0;
  for (uint64_t Record : A)
    if (auto E = checkBufferRange(Record, 16))
      return std::move(E);
  auto Length = RAM.read(A[0], 2);
  if (!Length)
    return Length.takeError();
  auto Capacity = RAM.read(A[0] + 2, 2);
  if (!Capacity)
    return Capacity.takeError();
  if (*Length + *SourceLength > *Capacity)
    return uint64_t(0xc0000023); // STATUS_BUFFER_TOO_SMALL, no buffer reads.
  auto Destination = RAM.read(A[0] + 8, 8);
  if (!Destination)
    return Destination.takeError();
  auto Source = RAM.read(A[1] + 8, 8);
  if (!Source)
    return Source.takeError();
  if (*Destination > UINT64_MAX - *Length)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "overflowing ANSI append destination");
  const uint64_t Target = *Destination + *Length;
  if (auto E = RAM.check(*Source, *SourceLength, false))
    return std::move(E);
  if (auto E = RAM.check(Target, *SourceLength, true))
    return std::move(E);
  if (auto E = RAM.check(A[0], 2, true))
    return std::move(E);
  // The observed shared memcpy/memmove body preserves overlapping bytes.
  // Its following ADD reads the post-copy Length, which may itself be aliased.
  std::vector<uint8_t> Bytes(*SourceLength);
  if (auto E = Memory.read(*Source, Bytes))
    return std::move(E);
  if (auto E = Memory.write(Target, Bytes))
    return std::move(E);
  auto AfterCopy = Memory.readInteger(A[0], 2);
  if (!AfterCopy)
    return AfterCopy.takeError();
  if (auto E =
          Memory.writeInteger(A[0], uint16_t(*AfterCopy + *SourceLength), 2))
    return std::move(E);
  return 0;
}
} // namespace neverd::emulation::runtime
