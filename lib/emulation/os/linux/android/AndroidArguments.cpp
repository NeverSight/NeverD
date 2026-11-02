//===- AndroidArguments.cpp - Bionic AAPCS64 scalar varargs
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidArguments.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <bit>

namespace neverd::emulation::android_model {
llvm::Error Bionic::ArgumentReader::read(uint64_t Address,
                                         llvm::MutableArrayRef<uint8_t> Bytes) {
  if (Bytes.size() > Remaining)
    return failure(diagnostic::ArgumentInputLimit);
  Remaining -= Bytes.size();
  if (auto E = Model.access(Address, Bytes.size(), Read))
    return E;
  Reads.push_back({Address, static_cast<unsigned>(Bytes.size())});
  return Model.CPU.read(Address, Bytes);
}

llvm::Error Bionic::ArgumentReader::initializeVAList(uint64_t Address) {
  if (Address % 8)
    return failure(diagnostic::ArgumentVAListAlignment);
  std::array<uint8_t, 32> Bytes{};
  if (auto E = read(Address, Bytes))
    return E;
  Stack = llvm::support::endian::read64le(Bytes.data());
  GRTop = llvm::support::endian::read64le(Bytes.data() + 8);
  VRTop = llvm::support::endian::read64le(Bytes.data() + 16);
  GROffset =
      static_cast<int32_t>(llvm::support::endian::read32le(Bytes.data() + 24));
  if (GROffset < -64 || GROffset % 8)
    return failure(diagnostic::ArgumentRegisterOffset);
  VROffset = std::bit_cast<int32_t>(
      llvm::support::endian::read32le(Bytes.data() + 28));
  HasVAList = StackLoaded = true;
  // Validate the FP cursor only when it is used. GP-only callers need not
  // supply a valid, otherwise unobserved vector save area.
  return llvm::Error::success();
}

llvm::Expected<uint64_t> Bionic::ArgumentReader::next(unsigned Size) {
  assert((Size == 4 || Size == 8) && "unsupported GP argument width");
  // Narrow scalar arguments are promoted to int. Their other four bytes are
  // unspecified, but both widths consume one eight-byte argument slot.
  if (!HasVAList && NextRegister < Call.Arguments.size()) {
    uint64_t Value = Call.Arguments[NextRegister++];
    return Size == 4 ? static_cast<uint32_t>(Value) : Value;
  }
  uint64_t Address;
  if (HasVAList && GROffset < 0) {
    const uint64_t Offset = -int64_t(GROffset);
    if (GRTop % 8 || GRTop < Offset)
      return failure(diagnostic::ArgumentRegisterSaveArea);
    Address = GRTop - Offset;
    GROffset += 8;
  } else {
    auto Next = stackArgument();
    if (!Next)
      return Next.takeError();
    Address = *Next;
  }
  std::array<uint8_t, 8> Bytes{};
  if (auto E = read(Address, llvm::MutableArrayRef(Bytes).take_front(Size)))
    return std::move(E);
  return llvm::support::endian::read64le(Bytes.data());
}

llvm::Expected<uint64_t> Bionic::ArgumentReader::stackArgument() {
  if (!StackLoaded) {
    auto SP = Model.CPU.readRegister(CPURegister::AArch64SP);
    if (!SP)
      return SP.takeError();
    Stack = (*SP)[0];
    StackLoaded = true;
    if (Stack % 16)
      return failure(diagnostic::ArgumentCallStackAlignment);
  }
  if (Stack % 8 || Stack > UINT64_MAX - 8)
    return failure(diagnostic::ArgumentStack);
  const auto Address = Stack;
  Stack += 8;
  return Address;
}

llvm::Expected<uint64_t> Bionic::ArgumentReader::nextFloating() {
  if (!HasVAList && NextVector < 8) {
    auto Value = Model.CPU.readRegister(
        vectorRegister(GuestArchitecture::AArch64, NextVector++));
    if (!Value)
      return Value.takeError();
    return (*Value)[0];
  }
  if (HasVAList && (VROffset < -128 || VROffset % 16))
    return failure("invalid AAPCS64 vector register offset");
  uint64_t Address;
  if (HasVAList && VROffset < 0) {
    const uint64_t Offset = -int64_t(VROffset);
    if (VRTop % 16 || VRTop < Offset)
      return failure(diagnostic::ArgumentRegisterSaveArea);
    Address = VRTop - Offset;
    VROffset += 16;
  } else {
    auto Next = stackArgument();
    if (!Next)
      return Next.takeError();
    Address = *Next;
  }
  std::array<uint8_t, 8> Bytes{};
  if (auto E = read(Address, Bytes))
    return std::move(E);
  return llvm::support::endian::read64le(Bytes.data());
}

bool Bionic::ArgumentReader::overlaps(uint64_t Address, unsigned Size) const {
  for (const auto &Range : Reads)
    if (Address <= Range.Address ? Range.Address - Address < Size
                                 : Address - Range.Address < Range.Size)
      return true;
  return false;
}
} // namespace neverd::emulation::android_model
