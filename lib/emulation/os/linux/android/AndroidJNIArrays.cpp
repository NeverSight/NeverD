//===- AndroidJNIArrays.cpp - Owned JNI arrays and copy-back modes --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JNI object and primitive arrays, regions and copy-back modes.
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

namespace neverd::emulation::android_model {
BionicResult Bionic::jniArrays(unsigned Slot, const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  constexpr unsigned Widths[] = {1, 1, 2, 2, 4, 8, 4, 8};
  constexpr char Kinds[] = "ZBCSIJFD";
  auto Throw = [&](llvm::StringRef Class) -> BionicResult {
    if (auto E = jniException(Class))
      return std::move(E);
    return value(0);
  };
  if (Slot >= 191 && Slot <= 198)
    return jniRelease(Slot, Call);
  if (Slot == 172 || (Slot >= 175 && Slot <= 182)) {
    const int32_t Length = A[1];
    if (Length < 0)
      return failure("JNI abort: negative array creation length");
    std::string ClassName;
    std::shared_ptr<JNIObject> Initial;
    if (Slot == 172) {
      auto Class = jniObject(A[2]);
      if (!Class)
        return Class.takeError();
      if (!*Class || (*Class)->Type != JNIObject::Class)
        return failure("JNI object array requires a class reference");
      auto Value = jniObject(A[3]);
      if (!Value)
        return Value.takeError();
      Initial = *Value;
      if (Initial &&
          !jniAssignable(Initial->Type == JNIObject::Class ? "java/lang/Class"
                                                           : Initial->ClassName,
                         (*Class)->ClassName))
        return failure("JNI abort: incompatible object-array initializer");
      ClassName = (*Class)->ClassName.starts_with("[")
                      ? "[" + (*Class)->ClassName
                      : "[L" + (*Class)->ClassName + ";";
    } else {
      ClassName = std::string("[") + Kinds[Slot - 175];
    }
    auto Object = JNI.Heap->allocateArray(ClassName, Length);
    if (!Object)
      return failure("JNI object storage budget exhausted");
    if (Slot == 172)
      Object->Elements.assign(Length, Initial);
    auto Handle = jniReference(Object);
    if (!Handle)
      return Handle.takeError();
    return value(*Handle);
  }
  auto Object = jniObject(A[1]);
  if (!Object)
    return Object.takeError();
  if (!*Object || ((*Object)->Type != JNIObject::PrimitiveArray &&
                   (*Object)->Type != JNIObject::ObjectArray))
    return failure("JNI array operation requires an array reference");
  if (Slot == 171)
    return value((*Object)->Length);
  if (Slot == 173 || Slot == 174) {
    if ((*Object)->Type != JNIObject::ObjectArray)
      return failure("JNI object array operation has a primitive array");
    const int32_t Index = A[2];
    if (Index < 0 || uint32_t(Index) >= (*Object)->Length)
      return Throw("java/lang/ArrayIndexOutOfBoundsException");
    if (Slot == 173) {
      auto Handle = jniReference((*Object)->Elements[Index].lock());
      if (!Handle)
        return Handle.takeError();
      return value(*Handle);
    }
    auto Item = jniObject(A[3]);
    if (!Item)
      return Item.takeError();
    if (*Item && !mobile::dalvik::referenceAssignable(
                     mobile::dalvik::objectDescriptor(**Item),
                     std::string_view((*Object)->ClassName).substr(1)))
      return Throw("java/lang/ArrayStoreException");
    (*Object)->Elements[Index] = *Item;
    return value(0);
  }
  const unsigned Kind = Slot >= 207   ? Slot - 207
                        : Slot >= 199 ? Slot - 199
                                      : Slot - 183;
  if (Kind >= 8 || (*Object)->Type != JNIObject::PrimitiveArray ||
      (*Object)->ClassName != std::string("[") + Kinds[Kind])
    return failure("JNI primitive array element kind mismatch");
  if (Slot >= 183 && Slot <= 190) {
    auto Address = jniBorrow(*Object, (*Object)->Bytes, A[2], Slot);
    if (!Address)
      return Address.takeError();
    return value(*Address);
  }
  const int32_t Start = A[2], Length = A[3];
  if (Start < 0 || Length < 0 || uint64_t(Start) > (*Object)->Length ||
      uint64_t(Length) > (*Object)->Length - Start)
    return Throw("java/lang/ArrayIndexOutOfBoundsException");
  const uint64_t Offset = uint64_t(Start) * Widths[Kind];
  const uint64_t Size = uint64_t(Length) * Widths[Kind];
  if (auto E = access(A[4], Size, Slot >= 207 ? Read : Write))
    return std::move(E);
  if (Size) {
    if (Slot >= 207) {
      std::vector<uint8_t> Bytes(Size);
      if (auto E = CPU.read(A[4], Bytes))
        return std::move(E);
      std::copy(Bytes.begin(), Bytes.end(), (*Object)->Bytes.begin() + Offset);
    } else if (auto E = CPU.write(
                   A[4],
                   llvm::ArrayRef((*Object)->Bytes).slice(Offset, Size))) {
      return std::move(E);
    }
  }
  return value(0);
}
} // namespace neverd::emulation::android_model
