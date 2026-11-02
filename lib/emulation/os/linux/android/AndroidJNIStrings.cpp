//===- AndroidJNIStrings.cpp - UTF-16 and modified UTF-8 JNI storage
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JNI UTF-16 and modified UTF-8 values with owned borrowed copies.
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

namespace neverd::emulation::android_model {
namespace {
llvm::Expected<std::vector<uint8_t>> decode(llvm::StringRef Text) {
  std::vector<uint8_t> Out;
  for (size_t I = 0; I < Text.size();) {
    uint32_t Value = uint8_t(Text[I++]);
    unsigned More = 0;
    if (Value >= 0xf0 && Value <= 0xf4) {
      Value &= 7;
      More = 3;
    } else if (Value >= 0xe0 && Value <= 0xef) {
      Value &= 15;
      More = 2;
    } else if (Value >= 0xc0 && Value <= 0xdf) {
      Value &= 31;
      More = 1;
    } else if (Value >= 0x80) {
      return failure("JNI string is not admitted modified UTF-8");
    }
    for (unsigned J = 0; J < More; ++J) {
      if (I == Text.size() || (uint8_t(Text[I]) & 0xc0) != 0x80)
        return failure("JNI string has a truncated or invalid UTF-8 sequence");
      Value = (Value << 6) | (uint8_t(Text[I++]) & 63);
    }
    if ((More == 1 && Value < 0x80 && Value != 0) ||
        (More == 2 && Value < 0x800) ||
        (More == 3 && (Value < 0x10000 || Value > 0x10ffff)))
      return failure("JNI string has a noncanonical modified UTF-8 sequence");
    if (More == 3) {
      const uint32_t Scalar = Value - 0x10000;
      const uint16_t High = 0xd800 | (Scalar >> 10);
      Out.push_back(uint8_t(High));
      Out.push_back(uint8_t(High >> 8));
      Value = 0xdc00 | (Scalar & 0x3ff);
    }
    Out.push_back(uint8_t(Value));
    Out.push_back(uint8_t(Value >> 8));
  }
  return Out;
}
} // namespace

llvm::Expected<uint64_t> Bionic::jniBorrow(std::shared_ptr<JNIObject> Object,
                                           llvm::ArrayRef<uint8_t> Bytes,
                                           uint64_t IsCopy, unsigned Slot) {
  if (IsCopy)
    if (auto E = access(IsCopy, 1, Write))
      return std::move(E);
  if (JNI.Borrows.size() == jni_abi::ReferenceLimit)
    return failure("JNI borrowed storage budget exhausted");
  auto Address = allocate(Bytes.size());
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return failure("JNI borrowed storage allocation failed");
  if (!Bytes.empty())
    if (auto E = CPU.write(*Address, Bytes))
      return std::move(E);
  if (IsCopy)
    if (auto E = jniPut(IsCopy, 1, 1))
      return std::move(E);
  JNI.Borrows.emplace(*Address, JNIState::Borrow{std::move(Object), threadID(),
                                                 Bytes.size(), Slot});
  return *Address;
}

BionicResult Bionic::jniRelease(unsigned Slot, const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Object = jniObject(A[1]);
  if (!Object)
    return Object.takeError();
  auto I = JNI.Borrows.find(A[2]);
  const unsigned Expected = Slot == 166 ? 165 : Slot == 170 ? 169 : Slot - 8;
  if (I == JNI.Borrows.end() || I->second.Thread != threadID() ||
      I->second.Object != *Object || I->second.Slot != Expected)
    return failure("JNI release does not match a live borrow");
  const bool Array = Slot >= 191 && Slot <= 198;
  const uint32_t Mode = Array ? uint32_t(A[3]) : 2;
  if (Mode > 2)
    return failure("JNI array release mode is invalid");
  if (Array && Mode != 2) {
    if (auto E = access(A[2], I->second.Size, Read))
      return std::move(E);
    std::vector<uint8_t> Bytes(I->second.Size);
    if (!Bytes.empty())
      if (auto E = CPU.read(A[2], Bytes))
        return std::move(E);
    (*Object)->Bytes = std::move(Bytes);
  }
  if (Mode != 1) {
    if (auto E = release(A[2]))
      return std::move(E);
    JNI.Borrows.erase(I);
  }
  return value(0);
}

BionicResult Bionic::jniStrings(unsigned Slot, const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if ((Slot == 167 || Slot == 169) && !A[1])
    return value(0); // ART nullable input; do not touch isCopy.
  if (Slot == 166 || Slot == 170)
    return jniRelease(Slot, Call);
  if (Slot == 163 || Slot == 167) {
    std::vector<uint8_t> Bytes;
    if (Slot == 167) {
      auto Text = string(A[1]);
      if (!Text)
        return Text.takeError();
      auto Decoded = decode(*Text);
      if (!Decoded)
        return Decoded.takeError();
      Bytes = std::move(*Decoded);
    } else {
      if (int32_t(A[2]) < 0)
        return failure("JNI NewString length must be nonnegative");
      const uint64_t Size = uint64_t(uint32_t(A[2])) * 2;
      if (auto E = access(A[1], Size, Read))
        return std::move(E);
      Bytes.resize(Size);
      if (Size)
        if (auto E = CPU.read(A[1], Bytes))
          return std::move(E);
    }
    auto Object =
        jniNewObject(JNIObject::String, "java/lang/String", Bytes.size());
    if (!Object)
      return Object.takeError();
    (*Object)->Bytes = std::move(Bytes);
    (*Object)->Length = (*Object)->Bytes.size() / 2;
    auto Handle = jniReference(*Object);
    if (!Handle)
      return Handle.takeError();
    return value(*Handle);
  }
  auto Object = jniObject(A[1]);
  if (!Object)
    return Object.takeError();
  if (!*Object || (*Object)->Type != JNIObject::String)
    return failure("JNI string operation requires a string reference");
  auto Bytes = llvm::ArrayRef((*Object)->Bytes);
  if (Slot == 164)
    return value((*Object)->Length);
  if (Slot == 220 || Slot == 221) {
    const int32_t Start = A[2], Length = A[3];
    if (Start < 0 || Length < 0 || uint64_t(Start) > (*Object)->Length ||
        uint64_t(Length) > (*Object)->Length - Start) {
      if (auto E = jniException("java/lang/StringIndexOutOfBoundsException"))
        return std::move(E);
      return value(0);
    }
    Bytes = Bytes.slice(uint64_t(Start) * 2, uint64_t(Length) * 2);
    std::vector<uint8_t> Encoded;
    if (Slot == 221) {
      Encoded = mobile::dalvik::artStringUTF8({Bytes.data(), Bytes.size()});
      Bytes = Encoded;
    }
    if (auto E = access(A[4], Bytes.size(), Write))
      return std::move(E);
    if (!Bytes.empty())
      if (auto E = CPU.write(A[4], Bytes))
        return std::move(E);
    return value(0);
  }
  std::vector<uint8_t> Storage;
  if (Slot == 168 || Slot == 169)
    Storage = mobile::dalvik::artStringUTF8({Bytes.data(), Bytes.size()});
  else
    Storage.assign(Bytes.begin(), Bytes.end());
  if (Slot == 168)
    return value(Storage.size());
  // Modified UTF-8 is terminated; a UTF-16 copy also has an extra zero unit,
  // which is allowed but not a promise made by the JNI GetStringChars API.
  Storage.resize(Storage.size() + (Slot == 169 ? 1 : 2));
  auto Address = jniBorrow(*Object, Storage, A[2], Slot);
  if (!Address)
    return Address.takeError();
  return value(*Address);
}
} // namespace neverd::emulation::android_model
