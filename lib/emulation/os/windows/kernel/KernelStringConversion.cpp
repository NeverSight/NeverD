//===- KernelStringConversion.cpp - Owned ASCII/UTF-16 RTL conversions ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owned ASCII/UTF-16 RTL conversions.
///

#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation {
namespace {
llvm::Error stringError(llvm::StringRef Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
// Shared closed ASCII policy for character and counted-string conversion.
// Higher values need an explicit NLS/code-page provider, never host locale.
llvm::Expected<uint8_t> asciiUnit(uint16_t Value) {
  if (Value > 0x7f)
    return stringError(
        "non-ASCII RTL conversion requires an explicit code page");
  return uint8_t(Value);
}
} // namespace
namespace runtime {
namespace {
class ConversionRAM {
  const KernelModel &Model;
  GuestMemory &Memory;

public:
  ConversionRAM(const KernelModel &Model, GuestMemory &Memory)
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
      return stringError(
          "Unicode output requires ordinary RAM with the requested access");
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

llvm::Expected<uint64_t> ansiUnicodeSize(const KernelModel &Model,
                                         GuestMemory &Memory, uint64_t Source) {
  const ConversionRAM RAM(Model, Memory);
  if (auto E = checkBufferRange(Source, 16))
    return E;
  auto Length = RAM.read(Source, 2);
  if (!Length)
    return Length.takeError();
  if (!*Length)
    return uint64_t(2);
  auto Buffer = RAM.read(Source + 8, 8);
  if (!Buffer)
    return Buffer.takeError();
  if (auto E = RAM.check(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (auto E = Memory.read(*Buffer, Bytes))
    return E;
  for (uint8_t Byte : Bytes) {
    auto Unit = asciiUnit(Byte);
    if (!Unit)
      return Unit.takeError();
  }
  return (*Length + 1) * 2;
}

llvm::Expected<uint64_t> ansiCharToUnicode(const KernelModel &Model,
                                           GuestMemory &Memory,
                                           uint64_t PointerCell) {
  const ConversionRAM RAM(Model, Memory);
  auto Source = RAM.read(PointerCell, 8);
  if (!Source)
    return Source.takeError();
  // Capture the source before updating the cell: the byte can alias the cell,
  // including through another virtual mapping of the same backing storage.
  auto Byte = RAM.read(*Source, 1);
  if (!Byte)
    return Byte.takeError();
  auto Unit = asciiUnit(uint16_t(*Byte));
  if (!Unit)
    return Unit.takeError();
  if (auto E = RAM.check(PointerCell, 8, true))
    return std::move(E);
  if (auto E = Memory.writeInteger(PointerCell, *Source + 1, 8))
    return std::move(E);
  // NUL is consumed too. The new pointer is not dereferenced or probed.
  return uint64_t(*Unit);
}

llvm::Expected<uint64_t> writeCountedUnicode(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             uint64_t Destination,
                                             llvm::ArrayRef<uint8_t> Text) {
  const ConversionRAM RAM(Model, Memory);
  if ((Text.size() & 1) || Text.size() > UINT16_MAX - 2)
    return stringError("Unicode output exceeds counted-string capacity");
  if (auto E = checkBufferRange(Destination, 16))
    return std::move(E);
  if (auto E = RAM.check(Destination, 2, true))
    return std::move(E);
  auto Capacity = RAM.read(Destination + 2, 2);
  if (!Capacity)
    return Capacity.takeError();
  const uint64_t Length = Text.size();
  // A short buffer observes required Length but consumes neither Buffer nor
  // the old Length/padding. Successful output always includes a WCHAR NUL.
  if (*Capacity < Length + 2) {
    if (auto E = Memory.writeInteger(Destination, Length, 2))
      return std::move(E);
    return uint64_t(0x80000005);
  }
  auto Target = RAM.read(Destination + 8, 8);
  if (!Target)
    return Target.takeError();
  if (auto E = RAM.check(*Target, Length, true))
    return std::move(E);
  // Preflight the digit bytes before publishing Length. Reload Buffer after
  // the copy; a failing terminator retains those preceding native stores.
  if (auto E = Memory.writeInteger(Destination, Length, 2))
    return std::move(E);
  if (Length)
    if (auto E = Memory.write(*Target, Text))
      return std::move(E);
  auto FinalTarget = RAM.read(Destination + 8, 8);
  if (!FinalTarget)
    return FinalTarget.takeError();
  if (*FinalTarget > UINT64_MAX - Length)
    return stringError(
        "Unicode terminator address overflows after Buffer reload");
  const uint64_t Terminator = *FinalTarget + Length;
  if (auto E = RAM.check(Terminator, 2, true))
    return std::move(E);
  if (auto E = Memory.writeInteger(Terminator, 0, 2))
    return std::move(E);
  return uint64_t(0);
}

llvm::Expected<uint64_t> integerUnicodeString(const KernelModel &Model,
                                              GuestMemory &Memory,
                                              llvm::ArrayRef<uint64_t> A,
                                              bool Wide) {
  uint32_t Base = uint32_t(A[1]);
  if (!Base)
    Base = 10;
  if (Base != 2 && Base != 8 && Base != 10 && Base != 16)
    return uint64_t(0xc000000d);
  uint64_t Value = Wide ? A[0] : uint32_t(A[0]);
  std::array<uint8_t, 64> Reverse{};
  unsigned Count = 0;
  do {
    Reverse[Count++] = uint8_t("0123456789ABCDEF"[Value % Base]);
    Value /= Base;
  } while (Value);
  std::vector<uint8_t> Text;
  Text.reserve(Count * 2);
  while (Count) {
    Text.push_back(Reverse[--Count]);
    Text.push_back(0);
  }
  return writeCountedUnicode(Model, Memory, A[2], Text);
}
} // namespace runtime

llvm::Expected<uint64_t>
KernelModel::convertCountedString(llvm::ArrayRef<uint64_t> A, bool ToUnicode) {
  for (uint64_t Record : {A[0], A[1]}) {
    if (auto E = runtime::checkBufferRange(Record, 16))
      return E;
    if (auto E = validateGuestAccess(Record, 16, false))
      return E;
  }
  std::array<uint8_t, 16> Source{}, Destination{};
  if (auto E = Memory.read(A[1], Source))
    return E;
  if (auto E = Memory.read(A[0], Destination))
    return E;
  const uint64_t Length = llvm::support::endian::read16le(Source.data());
  const auto MaxSource = llvm::support::endian::read16le(Source.data() + 2);
  const uint64_t Buffer = llvm::support::endian::read64le(Source.data() + 8);
  if (Length > MaxSource || (!ToUnicode && (Length & 1)))
    return stringError("malformed counted source string");
  if (auto E = runtime::checkBufferRange(Buffer, Length))
    return E;
  if (auto E = validateGuestAccess(Buffer, Length, false))
    return E;
  std::vector<uint8_t> Input(Length);
  if (Length)
    if (auto E = Memory.read(Buffer, Input))
      return E;
  std::vector<uint8_t> Output;
  for (size_t I = 0; I < Input.size(); I += ToUnicode ? 1 : 2) {
    auto Unit = asciiUnit(
        ToUnicode ? uint16_t(Input[I])
                  : llvm::support::endian::read16le(Input.data() + I));
    if (!Unit)
      return Unit.takeError();
    Output.push_back(*Unit);
    if (ToUnicode)
      Output.push_back(0);
  }
  const unsigned Unit = ToUnicode ? 2 : 1;
  const uint64_t ConvertedLength = Output.size();
  if (ConvertedLength + Unit > UINT16_MAX)
    return stringError("RTL conversion exceeds counted-string capacity");
  const bool Allocate = uint8_t(A[2]) != 0;
  if (ToUnicode && !Allocate)
    return runtime::writeCountedUnicode(*this, Memory, A[0], Output);
  uint64_t Capacity = llvm::support::endian::read16le(Destination.data() + 2);
  uint64_t Target = llvm::support::endian::read64le(Destination.data() + 8);
  uint64_t ResultLength = ConvertedLength;
  uint32_t Status = windows::StatusSuccess;
  bool WriteBuffer = true;
  if (!Allocate && Capacity < ConvertedLength + Unit) {
    Status =
        0x80000005; // STATUS_BUFFER_OVERFLOW, independently checked on RTL.
    if (ToUnicode || !Capacity)
      WriteBuffer = false;
    else {
      Output.resize(Capacity - 1);
      ResultLength = Output.size();
    }
  }
  if (WriteBuffer)
    Output.resize(Output.size() + Unit, 0);
  // Preflight all caller-owned outputs before allocating or publishing fields.
  auto Writable = [&](uint64_t Address, uint64_t Size) -> llvm::Error {
    if (auto E = runtime::checkBufferRange(Address, Size))
      return E;
    if (auto E = validateGuestAccess(Address, Size, true))
      return E;
    auto OK = Memory.canAccess(Address, Size, Write);
    if (!OK)
      return OK.takeError();
    if (!*OK)
      return stringError("RTL string output requires writable storage");
    return llvm::Error::success();
  };
  if (auto E = Writable(A[0], 4))
    return E;
  if (Allocate) {
    if (auto E = Writable(A[0] + 8, 8))
      return E;
    const uint64_t Arguments[] = {windows::PoolPaged, Output.size(),
                                  0x7274534e};
    auto New = allocatePool(Arguments, false);
    if (!New)
      return New.takeError();
    if (!*New)
      return uint64_t(windows::StatusInsufficientResources);
    Target = *New;
    Capacity = Output.size();
  } else if (WriteBuffer) {
    if (Target < A[0] + 16 && A[0] < Target + Output.size())
      return stringError("RTL output buffer overlaps its string record");
    if (auto E = Writable(Target, Output.size()))
      return E;
  }
  if (WriteBuffer)
    if (auto E = Memory.write(Target, Output))
      return E;
  if (auto E = Memory.writeInteger(A[0], ResultLength, 2))
    return E;
  if (Allocate) {
    if (auto E = Memory.writeInteger(A[0] + 2, Capacity, 2))
      return E;
    if (auto E = Memory.writeInteger(A[0] + 8, Target, 8))
      return E;
    RtlStringBuffers.insert(Target);
  }
  return uint64_t(Status);
}

llvm::Expected<uint64_t> KernelModel::freeCountedString(uint64_t Address) {
  if (auto E = runtime::checkBufferRange(Address, 16))
    return E;
  if (auto E = validateGuestAccess(Address + 8, 8, false))
    return E;
  auto Buffer = Memory.readInteger(Address + 8, 8);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer)
    return 0;
  if (!RtlStringBuffers.contains(*Buffer))
    return stringError("RTL string free requires an owned conversion buffer");
  const auto Allocation = Allocations.find(*Buffer);
  if (Allocation == Allocations.end())
    return stringError("RTL string conversion allocation is no longer live");
  if (Address < *Buffer + Allocation->second.Size && *Buffer < Address + 16)
    return stringError("RTL string record overlaps the buffer being freed");
  if (auto E = validateGuestAccess(Address, 16, true))
    return E;
  auto Writable = Memory.canAccess(Address, 16, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return stringError("RTL string record is not writable");
  const uint64_t Arguments[] = {*Buffer};
  auto Freed = freePool(Arguments, false);
  if (!Freed)
    return Freed.takeError();
  if (auto E = Memory.write(Address, std::array<uint8_t, 16>{}))
    return E;
  return 0;
}
} // namespace neverd::emulation
