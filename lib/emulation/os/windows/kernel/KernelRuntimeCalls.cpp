//===- KernelRuntimeCalls.cpp - Unicode and debug calls -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
// Every Windows ANSI code page and UTF-8 encode ASCII identically. Other text,
// including its converted length, depends on the unmodeled system code page.
constexpr uint8_t MaxASCIIUnit = 0x7f;
// A GUID is a little-endian Data1/Data2/Data3 followed by eight Data4 bytes,
// written in registry form as {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}.
constexpr unsigned GUIDSize = 16;
constexpr unsigned GUIDTextUnits = 38;

int hexValue(uint16_t Unit) {
  if (Unit >= '0' && Unit <= '9')
    return Unit - '0';
  if (Unit >= 'a' && Unit <= 'f')
    return Unit - 'a' + 10;
  if (Unit >= 'A' && Unit <= 'F')
    return Unit - 'A' + 10;
  return -1;
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::initializeUnicodeString(llvm::ArrayRef<uint64_t> A) {
  if (auto E = runtime::checkBufferRange(A[0], 16))
    return E;
  uint64_t Length = 0;
  if (A[1]) {
    if (auto E = runtime::checkBufferRange(A[1], MaxUnicodeBytes + 2))
      return E;
    while (Length <= MaxUnicodeBytes) {
      if (auto E = validateGuestAccess(A[1] + Length, 2, false))
        return E;
      auto Unit = Memory.readInteger(A[1] + Length, 2);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        break;
      Length += 2;
    }
    if (Length > MaxUnicodeBytes)
      return modelError("RtlInitUnicodeString source exceeds model capacity");
  }
  if (auto E = validateGuestAccess(A[0], 4, true))
    return E;
  if (auto E = validateGuestAccess(A[0] + 8, 8, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], Length, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + 2, A[1] ? Length + 2 : 0, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + 8, A[1], 8))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::upcaseUnicodeString(llvm::ArrayRef<uint64_t> A) {
  auto Read = [&](uint64_t Base, unsigned Offset,
                  unsigned Size) -> llvm::Expected<uint64_t> {
    if (auto E = runtime::checkBufferRange(Base, uint64_t(Offset) + Size))
      return E;
    if (auto E = validateGuestAccess(Base + Offset, Size, false))
      return E;
    return Memory.readInteger(Base + Offset, Size);
  };
  auto Write = [&](uint64_t Base, unsigned Offset, uint64_t Value,
                   unsigned Size) -> llvm::Error {
    if (auto E = runtime::checkBufferRange(Base, uint64_t(Offset) + Size))
      return E;
    if (auto E = validateGuestAccess(Base + Offset, Size, true))
      return E;
    return Memory.writeInteger(Base + Offset, Value, Size);
  };
  auto Length = Read(A[1], 0, 2);
  if (!Length)
    return Length.takeError();
  if (uint8_t(A[2])) {
    if (auto E = Write(A[0], UnicodeMaximumOffset, *Length, 2))
      return E;
    auto Buffer =
        allocatePool({PoolPaged, *Length, RuntimeStringPoolTag}, false);
    if (!Buffer)
      return Buffer.takeError();
    if (auto E = Write(A[0], UnicodeBufferOffset, *Buffer, 8))
      return E;
    if (!*Buffer)
      return StatusNoMemory;
    Length = Read(A[1], 0, 2);
    if (!Length)
      return Length.takeError();
  } else {
    auto Maximum = Read(A[0], UnicodeMaximumOffset, 2);
    if (!Maximum)
      return Maximum.takeError();
    if (*Length > *Maximum)
      return StatusBufferOverflow;
  }
  const unsigned Units = *Length / 2;
  for (unsigned I = 0; I < Units; ++I) {
    // Payload stores can alias either descriptor. The pinned routine reloads
    // both Buffer fields on every iteration and Length only after the loop.
    auto Source = Read(A[1], UnicodeBufferOffset, 8);
    if (!Source)
      return Source.takeError();
    auto Unit = Read(*Source, I * 2, 2);
    if (!Unit)
      return Unit.takeError();
    auto Upper = runtime::uppercaseUnicodeUnit(*Unit);
    if (!Upper)
      return Upper.takeError();
    auto Destination = Read(A[0], UnicodeBufferOffset, 8);
    if (!Destination)
      return Destination.takeError();
    if (auto E = Write(*Destination, I * 2, *Upper, 2))
      return E;
  }
  Length = Read(A[1], 0, 2);
  if (!Length)
    return Length.takeError();
  if (auto E = Write(A[0], 0, *Length, 2))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::ansiToUnicodeString(llvm::ArrayRef<uint64_t> A) {
  // RtlAnsiStringToUnicodeString (RVA 0x6f6840) stores Length before checking
  // capacity and replaces Buffer when it allocates, even on failure.
  if (auto E = validateGuestAccess(A[1], UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(A[1], 2);
  if (!Length)
    return Length.takeError();
  auto Source = Memory.readInteger(A[1] + UnicodeBufferOffset, 8);
  if (!Source)
    return Source.takeError();
  std::vector<uint8_t> Text(*Length);
  if (!Text.empty()) {
    if (auto E = validateGuestAccess(*Source, Text.size(), false))
      return E;
    if (auto E = Memory.read(*Source, Text))
      return E;
  }
  if (llvm::any_of(Text, [](uint8_t Byte) { return Byte > MaxASCIIUnit; }))
    return modelError("RtlAnsiStringToUnicodeString of non-ASCII text "
                      "requires an unmodeled ANSI code page");
  const uint64_t Size = Text.size() * 2 + 2;
  if (!uint8_t(A[2])) {
    std::vector<uint8_t> Wide(Text.size() * 2);
    for (size_t I = 0; I < Text.size(); ++I)
      Wide[I * 2] = Text[I];
    return runtime::writeCountedUnicode(*this, Memory, A[0], Wide);
  }
  if (Size > runtime::MaxCountedUnicodeBytes)
    return StatusInvalidParameter2;
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], Size - 2, 2))
    return E;
  uint64_t Buffer = 0;
  if (uint8_t(A[2])) {
    auto Pointer = allocatePool({PoolPaged, Size, RuntimeStringPoolTag}, false);
    if (!Pointer)
      return Pointer.takeError();
    if (auto E = Memory.writeInteger(A[0] + UnicodeMaximumOffset, Size, 2))
      return E;
    if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, *Pointer, 8))
      return E;
    if (!*Pointer)
      return StatusNoMemory;
    Buffer = *Pointer;
  } else {
    auto Maximum = Memory.readInteger(A[0] + UnicodeMaximumOffset, 2);
    if (!Maximum)
      return Maximum.takeError();
    if (Size > *Maximum)
      return StatusBufferOverflow;
    auto Pointer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
    if (!Pointer)
      return Pointer.takeError();
    Buffer = *Pointer;
  }
  std::vector<uint8_t> Wide(Size);
  for (size_t I = 0; I < Text.size(); ++I)
    Wide[I * 2] = Text[I];
  if (auto E = validateGuestAccess(Buffer, Wide.size(), true))
    return E;
  if (auto E = Memory.write(Buffer, Wide))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::unicodeToAnsiString(llvm::ArrayRef<uint64_t> A) {
  // RtlUnicodeStringToAnsiString (RVA 0x5edb00) stores Length first. Without
  // allocation it truncates to the capacity, still converts and terminates,
  // and reports STATUS_BUFFER_OVERFLOW.
  if (auto E = validateGuestAccess(A[1], UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(A[1], 2);
  if (!Length)
    return Length.takeError();
  auto Source = Memory.readInteger(A[1] + UnicodeBufferOffset, 8);
  if (!Source)
    return Source.takeError();
  // A trailing odd byte is not a complete UTF-16 unit and is never converted.
  std::vector<uint8_t> Units(*Length & ~uint64_t(1));
  if (!Units.empty()) {
    if (auto E = validateGuestAccess(*Source, Units.size(), false))
      return E;
    if (auto E = Memory.read(*Source, Units))
      return E;
  }
  for (size_t I = 0; I < Units.size(); I += 2)
    if (Units[I + 1] || Units[I] > MaxASCIIUnit)
      return modelError("RtlUnicodeStringToAnsiString of non-ASCII text "
                        "requires an unmodeled ANSI code page");
  const uint64_t Count = Units.size() / 2;
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], Count, 2))
    return E;
  uint64_t Status = StatusSuccess;
  uint64_t Converted = Count;
  uint64_t Buffer = 0;
  if (uint8_t(A[2])) {
    auto Pointer =
        allocatePool({PoolPaged, Count + 1, RuntimeStringPoolTag}, false);
    if (!Pointer)
      return Pointer.takeError();
    if (auto E = Memory.writeInteger(A[0] + UnicodeMaximumOffset, Count + 1, 2))
      return E;
    if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, *Pointer, 8))
      return E;
    if (!*Pointer)
      return StatusNoMemory;
    Buffer = *Pointer;
  } else {
    auto Maximum = Memory.readInteger(A[0] + UnicodeMaximumOffset, 2);
    if (!Maximum)
      return Maximum.takeError();
    if (Count >= *Maximum) {
      if (!*Maximum)
        return StatusBufferOverflow;
      Status = StatusBufferOverflow;
      Converted = *Maximum - 1;
      if (auto E = Memory.writeInteger(A[0], Converted, 2))
        return E;
    }
    auto Pointer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
    if (!Pointer)
      return Pointer.takeError();
    Buffer = *Pointer;
  }
  std::vector<uint8_t> Narrow(Converted + 1);
  for (uint64_t I = 0; I < Converted; ++I)
    Narrow[I] = Units[I * 2];
  if (auto E = validateGuestAccess(Buffer, Narrow.size(), true))
    return E;
  if (auto E = Memory.write(Buffer, Narrow))
    return E;
  return Status;
}

llvm::Expected<uint64_t>
KernelModel::stringFromGUID(llvm::ArrayRef<uint64_t> A) {
  // RtlStringFromGUID (RVA 0x6f34d0) stores MaximumLength and Buffer before
  // checking the allocation and Length only once it succeeded.
  std::vector<uint8_t> Guid(GUIDSize);
  if (auto E = validateGuestAccess(A[0], GUIDSize, false))
    return E;
  if (auto E = Memory.read(A[0], Guid))
    return E;
  if (auto E = validateGuestAccess(A[1], UnicodeRecordSize, true))
    return E;
  const uint64_t Size = (GUIDTextUnits + 1) * 2;
  auto Pointer = allocatePool({PoolPaged, Size, RuntimeStringPoolTag}, false);
  if (!Pointer)
    return Pointer.takeError();
  if (auto E = Memory.writeInteger(A[1] + UnicodeMaximumOffset, Size, 2))
    return E;
  if (auto E = Memory.writeInteger(A[1] + UnicodeBufferOffset, *Pointer, 8))
    return E;
  if (!*Pointer)
    return StatusNoMemory;
  const uint32_t Data1 =
      Guid[0] | Guid[1] << 8 | Guid[2] << 16 | uint32_t(Guid[3]) << 24;
  const unsigned Data2 = Guid[4] | Guid[5] << 8;
  const unsigned Data3 = Guid[6] | Guid[7] << 8;
  std::string Text =
      llvm::formatv("{{{0:x-8}-{1:x-4}-{2:x-4}-", Data1, Data2, Data3).str();
  for (unsigned I = 8; I < GUIDSize; ++I) {
    if (I == 10)
      Text += '-';
    Text += llvm::formatv("{0:x-2}", Guid[I]).str();
  }
  Text += '}';
  std::vector<uint8_t> Wide(Size);
  for (size_t I = 0; I < Text.size(); ++I)
    Wide[I * 2] = static_cast<uint8_t>(Text[I]);
  if (auto E = Memory.writeInteger(A[1], GUIDTextUnits * 2, 2))
    return E;
  if (auto E = validateGuestAccess(*Pointer, Wide.size(), true))
    return E;
  if (auto E = Memory.write(*Pointer, Wide))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::guidFromString(llvm::ArrayRef<uint64_t> A) {
  // RtlGUIDFromString (RVA 0x6bd650) scans the registry form with exact field
  // widths in either case. Data1 through Data3 are stored as soon as they are
  // scanned, so a later mismatch leaves them written; Data4 is copied only
  // after the whole form matched. Text may follow only after a zero unit.
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(A[0], 2);
  if (!Length)
    return Length.takeError();
  auto Source = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
  if (!Source)
    return Source.takeError();
  std::vector<uint8_t> Bytes(*Length & ~uint64_t(1));
  if (!Bytes.empty()) {
    if (auto E = validateGuestAccess(*Source, Bytes.size(), false))
      return E;
    if (auto E = Memory.read(*Source, Bytes))
      return E;
  }
  if (auto E = validateGuestAccess(A[1], GUIDSize, true))
    return E;
  const size_t Units = Bytes.size() / 2;
  size_t Next = 0;
  auto Unit = [&](size_t Index) -> uint16_t {
    return Bytes[Index * 2] | Bytes[Index * 2 + 1] << 8;
  };
  auto Literal = [&](char16_t Expected) {
    if (Next >= Units || Unit(Next) != Expected)
      return false;
    ++Next;
    return true;
  };
  auto Hex = [&](unsigned Digits, uint32_t &Value) {
    Value = 0;
    for (unsigned I = 0; I < Digits; ++I) {
      const int Digit = Next < Units ? hexValue(Unit(Next)) : -1;
      if (Digit < 0)
        return false;
      Value = Value * 16 + Digit;
      ++Next;
    }
    return true;
  };
  uint32_t Field = 0;
  if (!Literal(u'{') || !Hex(8, Field))
    return StatusInvalidParameter;
  if (auto E = Memory.writeInteger(A[1], Field, 4))
    return E;
  for (unsigned Offset : {4u, 6u}) {
    if (!Literal(u'-') || !Hex(4, Field))
      return StatusInvalidParameter;
    if (auto E = Memory.writeInteger(A[1] + Offset, Field, 2))
      return E;
  }
  std::vector<uint8_t> Data4(8);
  for (unsigned I = 0; I < Data4.size(); ++I) {
    if ((I == 0 || I == 2) && !Literal(u'-'))
      return StatusInvalidParameter;
    if (!Hex(2, Field))
      return StatusInvalidParameter;
    Data4[I] = static_cast<uint8_t>(Field);
  }
  if (!Literal(u'}') || (Next < Units && Unit(Next)))
    return StatusInvalidParameter;
  if (auto E = Memory.write(A[1] + 8, Data4))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::freeCountedString(llvm::ArrayRef<uint64_t> A) {
  // RtlFreeUnicodeString and RtlFreeAnsiString (RVA 0x602cb0) free a non-null
  // Buffer without loading a pool tag, then clear the whole record.
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, false))
    return E;
  auto Buffer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer)
    return StatusSuccess;
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  if (const auto Allocation = Allocations.find(*Buffer);
      Allocation != Allocations.end() &&
      A[0] < *Buffer + Allocation->second.Size &&
      *Buffer < A[0] + UnicodeRecordSize)
    return modelError("RTL string record overlaps the buffer being freed");
  auto Freed = freePool({*Buffer}, false);
  if (!Freed)
    return Freed.takeError();
  if (auto E = Memory.writeInteger(A[0], 0, 8))
    return E;
  if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, 0, 8))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::debugMessage(
    llvm::ArrayRef<uint64_t> A, unsigned FormatIndex,
    llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument) {
  auto Text = runtime::formatDebugMessage(*this, Memory, A[FormatIndex],
                                          FormatIndex + 1, ReadArgument);
  if (!Text)
    return Text.takeError();
  // The observation profile enables all debugger component/level filters.
  Result.Messages.push_back(std::move(*Text));
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::prefixedDebugMessage(llvm::ArrayRef<uint64_t> A) {
  auto Text =
      runtime::formatPrefixedDebugMessage(*this, Memory, A[0], A[3], A[4]);
  if (!Text)
    return Text.takeError();
  // Component and level filters share the enabled observation profile.
  Result.Messages.push_back(std::move(*Text));
  return StatusSuccess;
}

} // namespace neverd::emulation
